// The hosted files and directories (include/stx_auxlib/files.hpp): File and
// whole-file operations over the host backend (file_backend.hpp), and
// metadata and directory operations over std::filesystem's error_code
// overloads. Functions marked noexcept still allocate native paths; memory
// exhaustion there terminates.

#include "stx_auxlib/files.hpp"

#include "file_backend.hpp"

#include "../checked.hpp"

#include "stx_auxlib/clocks.hpp"
#include "stx_auxlib/paths.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace stx_auxlib {

#if defined(_WIN32)
// ERROR_PRIVILEGE_NOT_HELD (winerror.h), which libc++ reports in the system
// category without an errc equivalent: creating a symbolic link without
// developer mode or the symbolic link privilege.
constexpr int windows_privilege_not_held = 1314;
#endif

FileStatus file_backend::status_from(const std::error_code& error) noexcept {
    if (!error) return FileStatus::ok;
#if defined(_WIN32)
    if (error.category() == std::system_category() && error.value() == windows_privilege_not_held)
        return FileStatus::permission_denied;
#endif
    if (error == std::errc::no_such_file_or_directory) return FileStatus::not_found;
    if (error == std::errc::file_exists) return FileStatus::already_exists;
    if (error == std::errc::permission_denied || error == std::errc::operation_not_permitted ||
        error == std::errc::read_only_file_system)
        return FileStatus::permission_denied;
    if (error == std::errc::not_a_directory) return FileStatus::not_a_directory;
    if (error == std::errc::is_a_directory) return FileStatus::is_a_directory;
    if (error == std::errc::directory_not_empty) return FileStatus::directory_not_empty;
    if (error == std::errc::no_space_on_device) return FileStatus::no_space;
    if (error == std::errc::file_too_large || error == std::errc::value_too_large)
        return FileStatus::too_large;
    if (error == std::errc::filename_too_long) return FileStatus::name_too_long;
    if (error == std::errc::illegal_byte_sequence) return FileStatus::invalid_name;
    if (error == std::errc::cross_device_link) return FileStatus::cross_device;
    if (error == std::errc::device_or_resource_busy || error == std::errc::text_file_busy)
        return FileStatus::busy;
    if (error == std::errc::bad_file_descriptor) return FileStatus::bad_handle;
    if (error == std::errc::function_not_supported || error == std::errc::not_supported ||
        error == std::errc::operation_not_supported)
        return FileStatus::unsupported;
    return FileStatus::io_error;
}

namespace {

// The host form of a Path. POSIX hosts have no drive roots.
FileStatus to_native(const Path& path, std::filesystem::path& output) {
    output.clear();
#if !defined(_WIN32)
    if (path.drive() != 0) return FileStatus::unsupported;
#endif
    const std::string_view text = path.text();
    std::u8string utf8{};
    utf8.reserve(text.size());
    for (const char character : text) utf8.push_back(static_cast<char8_t>(character));
    output = std::filesystem::path(utf8);
    return FileStatus::ok;
}

// A host path as a Path; invalid_name when it is not one.
FileValue<Path> from_native(const std::filesystem::path& native) {
    const std::u8string utf8 = native.generic_u8string();
    std::string text{};
    text.reserve(utf8.size());
    for (const char8_t character : utf8) text.push_back(static_cast<char>(character));
    const PathParse parsed = Path::parse(text);
    const Path* const path = parsed.path();
    if (path == nullptr) return FileValue<Path>::failure(FileStatus::invalid_name);
    FileValue<Path> result = FileValue<Path>::success(*path);
    return result;
}

FileKind kind_of(const std::filesystem::file_type type) noexcept {
    switch (type) {
    case std::filesystem::file_type::regular:
        return FileKind::regular;
    case std::filesystem::file_type::directory:
        return FileKind::directory;
    case std::filesystem::file_type::symlink:
        return FileKind::symbolic_link;
    default:
        break;
    }
    return FileKind::other;
}

std::atomic<std::uint64_t> temporary_serial{0};

void append_hexadecimal(std::string& output, const std::uint64_t value) {
    constexpr char digits[] = "0123456789abcdef";
    for (int shift = 60; shift >= 0; shift -= 4)
        output.push_back(digits[(value >> static_cast<unsigned>(shift)) & 0xfU]);
}

// A name for write_file's temporary file, unique within this process and
// unlikely to collide with another's: the monotonic clock and a serial.
// The serial is a full 64-bit counter; at its maximum it would wrap, which
// takes 2^64 writes and only risks a name collision that create_new
// detects.
std::string temporary_name() {
    const std::uint64_t serial = temporary_serial.fetch_add(1, std::memory_order_relaxed);
    const std::uint64_t stamp =
        static_cast<std::uint64_t>(MonotonicClock::now().time_since_epoch().count());
    std::string result = ".stx-write-";
    append_hexadecimal(result, stamp);
    result.push_back('-');
    append_hexadecimal(result, serial);
    return result;
}

// Orders directory entries by name bytes, the same order on every host.
struct ByName final {
    bool operator()(const DirectoryEntry& left, const DirectoryEntry& right) const noexcept {
        const bool result = left.name < right.name;
        return result;
    }
};

// True when `name` is one valid Path segment.
bool is_segment(const std::string& name) {
    if (name == "." || name == "..") return false;
    const PathParse parsed = Path::parse(name);
    const Path* const path = parsed.path();
    const bool result = path != nullptr && (*path).text() == name;
    return result;
}

} // namespace

File::File() noexcept = default;

File::File(const std::intptr_t handle) noexcept : handle_(handle) {}

File::~File() {
    if (handle_ != file_backend::no_handle) (void)file_backend::close(handle_);
}

File::File(File&& other) noexcept : handle_(std::exchange(other.handle_, file_backend::no_handle)) {}

File& File::operator=(File&& other) noexcept {
    if (this == &other) return *this;
    if (handle_ != file_backend::no_handle) (void)file_backend::close(handle_);
    handle_ = std::exchange(other.handle_, file_backend::no_handle);
    return *this;
}

FileValue<File> File::open(const Path& path, const FileMode mode) noexcept {
    std::filesystem::path native{};
    const FileStatus converted = to_native(path, native);
    if (converted != FileStatus::ok) return FileValue<File>::failure(converted);
    std::intptr_t handle = file_backend::no_handle;
    const FileStatus opened = file_backend::open(native, mode, handle);
    if (opened != FileStatus::ok) return FileValue<File>::failure(opened);
    FileValue<File> result = FileValue<File>::success(File(handle));
    return result;
}

bool File::is_open() const noexcept {
    const bool result = handle_ != file_backend::no_handle;
    return result;
}

FileValue<std::size_t> File::read(const std::span<std::byte> output) noexcept {
    if (!is_open()) return FileValue<std::size_t>::failure(FileStatus::bad_handle);
    std::size_t transferred = 0;
    const FileStatus status = file_backend::read(handle_, output, transferred);
    if (status != FileStatus::ok) return FileValue<std::size_t>::failure(status);
    return FileValue<std::size_t>::success(transferred);
}

FileStatus File::write_all(const std::span<const std::byte> input) noexcept {
    if (!is_open()) return FileStatus::bad_handle;
    std::size_t written = 0;
    while (written < input.size()) {
        std::size_t transferred = 0;
        const FileStatus status =
            file_backend::write(handle_, input.subspan(written), transferred);
        if (status != FileStatus::ok) return status;
        written += transferred;
    }
    return FileStatus::ok;
}

FileValue<std::uint64_t> File::size() noexcept {
    if (!is_open()) return FileValue<std::uint64_t>::failure(FileStatus::bad_handle);
    std::uint64_t bytes = 0;
    const FileStatus status = file_backend::size(handle_, bytes);
    if (status != FileStatus::ok) return FileValue<std::uint64_t>::failure(status);
    return FileValue<std::uint64_t>::success(bytes);
}

FileValue<std::uint64_t> File::position() noexcept {
    if (!is_open()) return FileValue<std::uint64_t>::failure(FileStatus::bad_handle);
    std::uint64_t offset = 0;
    const FileStatus status = file_backend::position(handle_, offset);
    if (status != FileStatus::ok) return FileValue<std::uint64_t>::failure(status);
    return FileValue<std::uint64_t>::success(offset);
}

FileStatus File::seek(const std::uint64_t offset) noexcept {
    if (!is_open()) return FileStatus::bad_handle;
    const FileStatus result = file_backend::seek(handle_, offset);
    return result;
}

FileStatus File::sync() noexcept {
    if (!is_open()) return FileStatus::bad_handle;
    const FileStatus result = file_backend::sync(handle_);
    return result;
}

FileStatus File::close() noexcept {
    if (!is_open()) return FileStatus::bad_handle;
    const std::intptr_t handle = std::exchange(handle_, file_backend::no_handle);
    const FileStatus result = file_backend::close(handle);
    return result;
}

// Reads into `output`, growing it toward `limit` as the file proves longer
// than its reported size, and probing one byte at the limit to tell a file
// of exactly `limit` bytes from a longer one.
FileStatus read_file(const Path& path, const std::size_t limit, std::vector<std::byte>& output) {
    output.clear();
    FileValue<File> opened = File::open(path, FileMode::read);
    File* const file = opened.value();
    if (file == nullptr) return opened.status();
    const FileValue<std::uint64_t> reported = (*file).size();
    if (!reported.succeeded()) return reported.status();
    const std::uint64_t bytes = *reported.value();
    if (bytes > limit) return FileStatus::too_large;
    output.resize(static_cast<std::size_t>(bytes));
    std::size_t filled = 0;
    for (;;) {
        if (filled == output.size()) {
            if (output.size() == limit) {
                std::byte probe[1]{};
                const FileValue<std::size_t> extra = (*file).read(probe);
                if (!extra.succeeded()) {
                    output.clear();
                    return extra.status();
                }
                if (*extra.value() != 0) {
                    output.clear();
                    return FileStatus::too_large;
                }
                break;
            }
            const std::size_t doubled =
                output.size() < 2048 ? 4096 : checked::multiply_saturating(output.size(), 2);
            output.resize(std::min(limit, doubled));
        }
        const std::span<std::byte> rest = std::span<std::byte>(output).subspan(filled);
        const FileValue<std::size_t> chunk = (*file).read(rest);
        if (!chunk.succeeded()) {
            output.clear();
            return chunk.status();
        }
        if (*chunk.value() == 0) break;
        filled += *chunk.value();
    }
    output.resize(filled);
    const FileStatus closed = (*file).close();
    if (closed != FileStatus::ok) output.clear();
    return closed;
}

FileStatus write_file(const Path& path, const std::span<const std::byte> content) {
    if (path.file_name().empty()) return FileStatus::invalid_name;
    std::filesystem::path target{};
    const FileStatus converted = to_native(path, target);
    if (converted != FileStatus::ok) return converted;
    const Path directory = path.parent();

    // Creates the temporary file, retrying on the rare name collision.
    constexpr int attempts = 16;
    FileValue<File> opened = FileValue<File>::failure(FileStatus::already_exists);
    std::filesystem::path temporary{};
    for (int attempt = 0; attempt < attempts; ++attempt) {
        const PathParse name = Path::parse(temporary_name());
        const PathParse joined = directory.join(*name.path());
        const Path* const candidate = joined.path();
        if (candidate == nullptr) return FileStatus::name_too_long;
        const FileStatus candidate_converted = to_native(*candidate, temporary);
        if (candidate_converted != FileStatus::ok) return candidate_converted;
        opened = File::open(*candidate, FileMode::create_new);
        if (opened.status() != FileStatus::already_exists) break;
    }
    File* const file = opened.value();
    if (file == nullptr) return opened.status();

    FileStatus status = (*file).write_all(content);
    if (status == FileStatus::ok) status = (*file).sync();
    const FileStatus closed = (*file).close();
    if (status == FileStatus::ok) status = closed;
    if (status == FileStatus::ok) status = file_backend::replace_durably(temporary, target);
    if (status != FileStatus::ok) {
        std::error_code ignored{};
        (void)std::filesystem::remove(temporary, ignored);
    }
    return status;
}

FileValue<FileInformation> inspect(const Path& path, const LinkMode links) noexcept {
    std::filesystem::path native{};
    const FileStatus converted = to_native(path, native);
    if (converted != FileStatus::ok) return FileValue<FileInformation>::failure(converted);
    FileInformation information{};
    const FileStatus status = file_backend::inspect(native, links, information);
    if (status != FileStatus::ok) return FileValue<FileInformation>::failure(status);
    return FileValue<FileInformation>::success(information);
}

FileStatus create_directory(const Path& path) noexcept {
    std::filesystem::path native{};
    const FileStatus converted = to_native(path, native);
    if (converted != FileStatus::ok) return converted;
    std::error_code error{};
    const bool created = std::filesystem::create_directory(native, error);
    if (error) return file_backend::status_from(error);
    if (!created) return FileStatus::already_exists;
    return FileStatus::ok;
}

FileStatus create_directories(const Path& path) noexcept {
    std::filesystem::path native{};
    const FileStatus converted = to_native(path, native);
    if (converted != FileStatus::ok) return converted;
    std::error_code error{};
    (void)std::filesystem::create_directories(native, error);
    if (error) return file_backend::status_from(error);
    if (!std::filesystem::is_directory(native, error)) return FileStatus::not_a_directory;
    return FileStatus::ok;
}

FileStatus remove(const Path& path) noexcept {
    std::filesystem::path native{};
    const FileStatus converted = to_native(path, native);
    if (converted != FileStatus::ok) return converted;
    std::error_code error{};
    const bool removed = std::filesystem::remove(native, error);
    if (error) return file_backend::status_from(error);
    if (!removed) return FileStatus::not_found;
    return FileStatus::ok;
}

FileValue<std::uint64_t> remove_tree(const Path& path) noexcept {
    std::filesystem::path native{};
    const FileStatus converted = to_native(path, native);
    if (converted != FileStatus::ok) return FileValue<std::uint64_t>::failure(converted);
    std::error_code error{};
    const std::uintmax_t removed = std::filesystem::remove_all(native, error);
    if (error) return FileValue<std::uint64_t>::failure(file_backend::status_from(error));
    return FileValue<std::uint64_t>::success(static_cast<std::uint64_t>(removed));
}

FileStatus rename(const Path& from, const Path& to) noexcept {
    std::filesystem::path source{};
    std::filesystem::path target{};
    FileStatus converted = to_native(from, source);
    if (converted == FileStatus::ok) converted = to_native(to, target);
    if (converted != FileStatus::ok) return converted;
    std::error_code error{};
    std::filesystem::rename(source, target, error);
    const FileStatus result = file_backend::status_from(error);
    return result;
}

FileStatus copy_file(const Path& from, const Path& to, const CopyMode mode) noexcept {
    std::filesystem::path source{};
    std::filesystem::path target{};
    FileStatus converted = to_native(from, source);
    if (converted == FileStatus::ok) converted = to_native(to, target);
    if (converted != FileStatus::ok) return converted;
    const std::filesystem::copy_options options = mode == CopyMode::replace
                                                      ? std::filesystem::copy_options::overwrite_existing
                                                      : std::filesystem::copy_options::none;
    std::error_code error{};
    (void)std::filesystem::copy_file(source, target, options, error);
    const FileStatus result = file_backend::status_from(error);
    return result;
}

FileStatus list_directory(const Path& path, const std::size_t limit, DirectoryListing& output) {
    output.entries.clear();
    output.unnamed_entries = 0;
    std::filesystem::path native{};
    const FileStatus converted = to_native(path, native);
    if (converted != FileStatus::ok) return converted;
    std::error_code error{};
    std::filesystem::directory_iterator entry(native, error);
    if (error) return file_backend::status_from(error);
    std::size_t seen = 0;
    const std::filesystem::directory_iterator end{};
    while (entry != end) {
        if (seen == limit) {
            output.entries.clear();
            output.unnamed_entries = 0;
            return FileStatus::too_large;
        }
        ++seen;
        const std::filesystem::directory_entry& current = *entry;
        const std::u8string utf8 = current.path().filename().generic_u8string();
        std::string name{};
        name.reserve(utf8.size());
        for (const char8_t character : utf8) name.push_back(static_cast<char>(character));
        if (is_segment(name)) {
            DirectoryEntry listed{};
            listed.name = std::move(name);
            const std::filesystem::file_status status = current.symlink_status(error);
            listed.kind = error ? FileKind::other : kind_of(status.type());
            output.entries.push_back(std::move(listed));
        } else {
            ++output.unnamed_entries;
        }
        entry.increment(error);
        if (error) {
            output.entries.clear();
            output.unnamed_entries = 0;
            return file_backend::status_from(error);
        }
    }
    std::sort(output.entries.begin(), output.entries.end(), ByName{});
    return FileStatus::ok;
}

FileValue<Path> temporary_directory() {
    std::error_code error{};
    const std::filesystem::path native = std::filesystem::temp_directory_path(error);
    if (error) return FileValue<Path>::failure(file_backend::status_from(error));
    FileValue<Path> result = from_native(native);
    return result;
}

FileValue<Path> working_directory() {
    std::error_code error{};
    const std::filesystem::path native = std::filesystem::current_path(error);
    if (error) return FileValue<Path>::failure(file_backend::status_from(error));
    FileValue<Path> result = from_native(native);
    return result;
}

FileValue<Path> absolute(const Path& path) {
    if (path.is_absolute()) return FileValue<Path>::success(path);
    const FileValue<Path> working = working_directory();
    const Path* const base = working.value();
    if (base == nullptr) return FileValue<Path>::failure(working.status());
    const PathParse joined = (*base).join(path);
    const Path* const result = joined.path();
    if (result == nullptr) return FileValue<Path>::failure(FileStatus::name_too_long);
    return FileValue<Path>::success(*result);
}

FileValue<Path> resolve(const Path& path) {
    std::filesystem::path native{};
    const FileStatus converted = to_native(path, native);
    if (converted != FileStatus::ok) return FileValue<Path>::failure(converted);
    std::error_code error{};
    const std::filesystem::path resolved = std::filesystem::canonical(native, error);
    if (error) return FileValue<Path>::failure(file_backend::status_from(error));
    FileValue<Path> result = from_native(resolved);
    return result;
}

FileValue<Path> read_link(const Path& path) {
    std::filesystem::path native{};
    const FileStatus converted = to_native(path, native);
    if (converted != FileStatus::ok) return FileValue<Path>::failure(converted);
    std::error_code error{};
    const std::filesystem::path target = std::filesystem::read_symlink(native, error);
    if (error) return FileValue<Path>::failure(file_backend::status_from(error));
    FileValue<Path> result = from_native(target);
    return result;
}

FileStatus create_link(const Path& target, const Path& link) noexcept {
    std::filesystem::path native_target{};
    std::filesystem::path native_link{};
    FileStatus converted = to_native(target, native_target);
    if (converted == FileStatus::ok) converted = to_native(link, native_link);
    if (converted != FileStatus::ok) return converted;
    // Windows distinguishes links to directories; a relative target is
    // resolved against the link's directory, as the host will.
    std::error_code error{};
    const std::filesystem::path resolved_target = native_target.is_absolute()
                                                      ? native_target
                                                      : native_link.parent_path() / native_target;
    const bool to_directory = std::filesystem::is_directory(resolved_target, error);
    error.clear();
    if (to_directory) {
        std::filesystem::create_directory_symlink(native_target, native_link, error);
    } else {
        std::filesystem::create_symlink(native_target, native_link, error);
    }
    const FileStatus result = file_backend::status_from(error);
    return result;
}

} // namespace stx_auxlib
