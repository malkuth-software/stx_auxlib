// The POSIX file backend (file_backend.hpp), for macOS, Linux and the BSDs.
// Durability uses F_FULLFSYNC where the host has it (Apple), because plain
// fsync there does not flush the drive's cache, and fsync elsewhere.

#include "file_backend.hpp"

#include "stx_auxlib/clocks.hpp"

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <span>
#include <system_error>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace stx_auxlib::file_backend {

namespace {

// One read or write moves at most this much, below every host's ssize_t
// limit.
constexpr std::size_t largest_transfer = std::size_t{1} << 30;

FileStatus status_from_errno(const int error) noexcept {
    const FileStatus result = status_from(std::error_code(error, std::generic_category()));
    return result;
}

int descriptor(const std::intptr_t handle) noexcept {
    const int result = static_cast<int>(handle);
    return result;
}

int open_flags(const FileMode mode) noexcept {
    switch (mode) {
    case FileMode::read:
        return O_RDONLY | O_CLOEXEC;
    case FileMode::create_new:
        return O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC;
    case FileMode::replace:
        return O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC;
    case FileMode::append:
        return O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC;
    }
    return -1;
}

// Flushes the descriptor's content to stable storage.
int flush_descriptor(const int fd) noexcept {
#if defined(F_FULLFSYNC)
    // Some filesystems (network volumes, RAM disks) refuse F_FULLFSYNC;
    // fsync is then the strongest flush they offer.
    if (::fcntl(fd, F_FULLFSYNC) == 0) return 0;
#endif
    int result = ::fsync(fd);
    while (result != 0 && errno == EINTR) result = ::fsync(fd);
    return result;
}

} // namespace

FileStatus open(const std::filesystem::path& native, const FileMode mode,
                std::intptr_t& handle) noexcept {
    handle = no_handle;
    const int flags = open_flags(mode);
    if (flags < 0) return FileStatus::io_error;
    int fd = ::open(native.c_str(), flags, 0666);
    while (fd < 0 && errno == EINTR) fd = ::open(native.c_str(), flags, 0666);
    if (fd < 0) return status_from_errno(errno);
    if (mode == FileMode::read) {
        struct stat information{};
        if (::fstat(fd, &information) != 0) {
            const int error = errno;
            (void)::close(fd);
            return status_from_errno(error);
        }
        if (S_ISDIR(information.st_mode)) {
            (void)::close(fd);
            return FileStatus::is_a_directory;
        }
    }
    handle = static_cast<std::intptr_t>(fd);
    return FileStatus::ok;
}

FileStatus read(const std::intptr_t handle, const std::span<std::byte> output,
                std::size_t& transferred) noexcept {
    transferred = 0;
    const std::size_t request = output.size() < largest_transfer ? output.size() : largest_transfer;
    ssize_t count = ::read(descriptor(handle), output.data(), request);
    while (count < 0 && errno == EINTR) count = ::read(descriptor(handle), output.data(), request);
    if (count < 0) return status_from_errno(errno);
    transferred = static_cast<std::size_t>(count);
    return FileStatus::ok;
}

FileStatus write(const std::intptr_t handle, const std::span<const std::byte> input,
                 std::size_t& transferred) noexcept {
    transferred = 0;
    const std::size_t request = input.size() < largest_transfer ? input.size() : largest_transfer;
    ssize_t count = ::write(descriptor(handle), input.data(), request);
    while (count < 0 && errno == EINTR) count = ::write(descriptor(handle), input.data(), request);
    if (count < 0) return status_from_errno(errno);
    if (count == 0 && request != 0) return FileStatus::io_error;
    transferred = static_cast<std::size_t>(count);
    return FileStatus::ok;
}

FileStatus size(const std::intptr_t handle, std::uint64_t& bytes) noexcept {
    bytes = 0;
    struct stat information{};
    if (::fstat(descriptor(handle), &information) != 0) return status_from_errno(errno);
    if (information.st_size < 0) return FileStatus::io_error;
    bytes = static_cast<std::uint64_t>(information.st_size);
    return FileStatus::ok;
}

FileStatus position(const std::intptr_t handle, std::uint64_t& offset) noexcept {
    offset = 0;
    const off_t current = ::lseek(descriptor(handle), 0, SEEK_CUR);
    if (current < 0) return status_from_errno(errno);
    offset = static_cast<std::uint64_t>(current);
    return FileStatus::ok;
}

FileStatus seek(const std::intptr_t handle, const std::uint64_t offset) noexcept {
    if (offset > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()))
        return FileStatus::too_large;
    const off_t target = static_cast<off_t>(offset);
    if (::lseek(descriptor(handle), target, SEEK_SET) < 0) return status_from_errno(errno);
    return FileStatus::ok;
}

FileStatus sync(const std::intptr_t handle) noexcept {
    if (flush_descriptor(descriptor(handle)) != 0) return status_from_errno(errno);
    return FileStatus::ok;
}

// POSIX leaves a descriptor's state unspecified after close fails with
// EINTR; Linux and macOS release it, so close is never retried.
FileStatus close(const std::intptr_t handle) noexcept {
    if (::close(descriptor(handle)) != 0 && errno != EINTR) return status_from_errno(errno);
    return FileStatus::ok;
}

FileStatus inspect(const std::filesystem::path& native, const LinkMode links,
                   FileInformation& information) noexcept {
    information = FileInformation{};
    struct stat described{};
    const int result = links == LinkMode::follow ? ::stat(native.c_str(), &described)
                                                 : ::lstat(native.c_str(), &described);
    if (result != 0) return status_from_errno(errno);
    if (S_ISREG(described.st_mode)) {
        information.kind = FileKind::regular;
        information.size = described.st_size < 0 ? 0 : static_cast<std::uint64_t>(described.st_size);
    } else if (S_ISDIR(described.st_mode)) {
        information.kind = FileKind::directory;
    } else if (S_ISLNK(described.st_mode)) {
        information.kind = FileKind::symbolic_link;
    } else {
        information.kind = FileKind::other;
    }
#if defined(__APPLE__)
    const timespec modified = described.st_mtimespec;
#else
    const timespec modified = described.st_mtim;
#endif
    // tv_sec * 10^9 stays in int64 for every time WallClock can hold;
    // seconds beyond its range (past 2262) are clamped to its ends.
    constexpr std::int64_t largest_seconds = 9223372035;
    constexpr std::int64_t smallest_seconds = -9223372035;
    std::int64_t seconds = static_cast<std::int64_t>(modified.tv_sec);
    if (seconds > largest_seconds) seconds = largest_seconds;
    if (seconds < smallest_seconds) seconds = smallest_seconds;
    const std::int64_t count = seconds * 1000000000LL + static_cast<std::int64_t>(modified.tv_nsec);
    information.modified = WallClock::time_point(WallClock::duration(count));
    return FileStatus::ok;
}

FileStatus replace_durably(const std::filesystem::path& from,
                           const std::filesystem::path& to) noexcept {
    if (::rename(from.c_str(), to.c_str()) != 0) return status_from_errno(errno);
    // Make the rename itself durable by flushing the directory that holds
    // `to`. Filesystems that cannot flush a directory report EINVAL or
    // ENOTSUP; the rename has still happened, so those are not failures.
    std::filesystem::path directory = to.parent_path();
    if (directory.empty()) directory = ".";
    int fd = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    while (fd < 0 && errno == EINTR) fd = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return status_from_errno(errno);
    const int flushed = flush_descriptor(fd);
    const int error = errno;
    (void)::close(fd);
    if (flushed != 0 && error != EINVAL && error != ENOTSUP) return status_from_errno(error);
    return FileStatus::ok;
}

} // namespace stx_auxlib::file_backend
