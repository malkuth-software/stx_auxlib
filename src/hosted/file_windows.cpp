// The Windows file backend (file_backend.hpp), over the Win32 file API.
// Absolute paths take the "\\?\" prefix so that names longer than MAX_PATH
// work without the process opting into long paths. Files open shared for
// reading, writing and deletion, so that renaming over an open file
// behaves as it does on POSIX hosts where Windows allows it.

#include "file_backend.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <span>
#include <string>
#include <system_error>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace stx_auxlib::file_backend {

namespace {

// One ReadFile or WriteFile moves at most this much (a DWORD count).
constexpr std::size_t largest_transfer = std::size_t{1} << 30;

FileStatus status_from_last_error() noexcept {
    const DWORD error = ::GetLastError();
    switch (error) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
    case ERROR_INVALID_DRIVE:
        return FileStatus::not_found;
    case ERROR_FILE_EXISTS:
    case ERROR_ALREADY_EXISTS:
        return FileStatus::already_exists;
    case ERROR_ACCESS_DENIED:
    case ERROR_WRITE_PROTECT:
        return FileStatus::permission_denied;
    case ERROR_DIRECTORY:
        return FileStatus::not_a_directory;
    case ERROR_DIR_NOT_EMPTY:
        return FileStatus::directory_not_empty;
    case ERROR_DISK_FULL:
    case ERROR_HANDLE_DISK_FULL:
        return FileStatus::no_space;
    case ERROR_FILENAME_EXCED_RANGE:
        return FileStatus::name_too_long;
    case ERROR_INVALID_NAME:
    case ERROR_BAD_PATHNAME:
        return FileStatus::invalid_name;
    case ERROR_NOT_SAME_DEVICE:
        return FileStatus::cross_device;
    case ERROR_SHARING_VIOLATION:
    case ERROR_LOCK_VIOLATION:
        return FileStatus::busy;
    case ERROR_INVALID_HANDLE:
        return FileStatus::bad_handle;
    case ERROR_NOT_SUPPORTED:
    case ERROR_CALL_NOT_IMPLEMENTED:
        return FileStatus::unsupported;
    default:
        break;
    }
    return FileStatus::io_error;
}

HANDLE native_handle(const std::intptr_t handle) noexcept {
    const HANDLE result = reinterpret_cast<HANDLE>(handle);
    return result;
}

// The wide name CreateFileW and MoveFileExW receive.
std::wstring wide_name(const std::filesystem::path& native) {
    std::filesystem::path preferred = native;
    preferred.make_preferred();
    std::wstring result = preferred.wstring();
    if (preferred.is_absolute() && result.rfind(L"\\\\", 0) != 0) result.insert(0, L"\\\\?\\");
    return result;
}

} // namespace

FileStatus open(const std::filesystem::path& native, const FileMode mode,
                std::intptr_t& handle) noexcept {
    handle = no_handle;
    DWORD access = 0;
    DWORD disposition = 0;
    switch (mode) {
    case FileMode::read:
        access = GENERIC_READ;
        disposition = OPEN_EXISTING;
        break;
    case FileMode::create_new:
        access = GENERIC_WRITE;
        disposition = CREATE_NEW;
        break;
    case FileMode::replace:
        access = GENERIC_WRITE;
        disposition = CREATE_ALWAYS;
        break;
    case FileMode::append:
        access = FILE_APPEND_DATA | SYNCHRONIZE;
        disposition = OPEN_ALWAYS;
        break;
    default:
        return FileStatus::io_error;
    }
    const std::wstring name = wide_name(native);
    const DWORD attributes = ::GetFileAttributesW(name.c_str());
    if (mode == FileMode::read && attributes != INVALID_FILE_ATTRIBUTES &&
        (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
        return FileStatus::is_a_directory;
    const DWORD sharing = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
    const HANDLE opened = ::CreateFileW(name.c_str(), access, sharing, nullptr, disposition,
                                        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (opened == INVALID_HANDLE_VALUE) return status_from_last_error();
    handle = reinterpret_cast<std::intptr_t>(opened);
    return FileStatus::ok;
}

FileStatus read(const std::intptr_t handle, const std::span<std::byte> output,
                std::size_t& transferred) noexcept {
    transferred = 0;
    const std::size_t request = output.size() < largest_transfer ? output.size() : largest_transfer;
    DWORD count = 0;
    if (!::ReadFile(native_handle(handle), output.data(), static_cast<DWORD>(request), &count,
                    nullptr)) {
        // A pipe's writer closing is the end of input, not a failure.
        if (::GetLastError() == ERROR_BROKEN_PIPE) return FileStatus::ok;
        return status_from_last_error();
    }
    transferred = static_cast<std::size_t>(count);
    return FileStatus::ok;
}

FileStatus write(const std::intptr_t handle, const std::span<const std::byte> input,
                 std::size_t& transferred) noexcept {
    transferred = 0;
    const std::size_t request = input.size() < largest_transfer ? input.size() : largest_transfer;
    DWORD count = 0;
    if (!::WriteFile(native_handle(handle), input.data(), static_cast<DWORD>(request), &count,
                     nullptr))
        return status_from_last_error();
    if (count == 0 && request != 0) return FileStatus::io_error;
    transferred = static_cast<std::size_t>(count);
    return FileStatus::ok;
}

FileStatus size(const std::intptr_t handle, std::uint64_t& bytes) noexcept {
    bytes = 0;
    LARGE_INTEGER length{};
    if (!::GetFileSizeEx(native_handle(handle), &length)) return status_from_last_error();
    if (length.QuadPart < 0) return FileStatus::io_error;
    bytes = static_cast<std::uint64_t>(length.QuadPart);
    return FileStatus::ok;
}

FileStatus position(const std::intptr_t handle, std::uint64_t& offset) noexcept {
    offset = 0;
    LARGE_INTEGER zero{};
    LARGE_INTEGER current{};
    if (!::SetFilePointerEx(native_handle(handle), zero, &current, FILE_CURRENT))
        return status_from_last_error();
    offset = static_cast<std::uint64_t>(current.QuadPart);
    return FileStatus::ok;
}

FileStatus seek(const std::intptr_t handle, const std::uint64_t offset) noexcept {
    if (offset > static_cast<std::uint64_t>(std::numeric_limits<LONGLONG>::max()))
        return FileStatus::too_large;
    LARGE_INTEGER target{};
    target.QuadPart = static_cast<LONGLONG>(offset);
    if (!::SetFilePointerEx(native_handle(handle), target, nullptr, FILE_BEGIN))
        return status_from_last_error();
    return FileStatus::ok;
}

FileStatus sync(const std::intptr_t handle) noexcept {
    if (!::FlushFileBuffers(native_handle(handle))) return status_from_last_error();
    return FileStatus::ok;
}

FileStatus close(const std::intptr_t handle) noexcept {
    if (!::CloseHandle(native_handle(handle))) return status_from_last_error();
    return FileStatus::ok;
}

// std::filesystem, which follows or describes reparse points as asked; its
// size and time take further host calls on Windows.
FileStatus inspect(const std::filesystem::path& native, const LinkMode links,
                   FileInformation& information) noexcept {
    information = FileInformation{};
    std::error_code error{};
    const std::filesystem::file_status status = links == LinkMode::follow
                                                    ? std::filesystem::status(native, error)
                                                    : std::filesystem::symlink_status(native, error);
    if (status.type() == std::filesystem::file_type::not_found) return FileStatus::not_found;
    if (error) return status_from(error);
    switch (status.type()) {
    case std::filesystem::file_type::regular:
        information.kind = FileKind::regular;
        break;
    case std::filesystem::file_type::directory:
        information.kind = FileKind::directory;
        break;
    case std::filesystem::file_type::symlink:
        information.kind = FileKind::symbolic_link;
        break;
    default:
        information.kind = FileKind::other;
        break;
    }
    if (information.kind == FileKind::regular) {
        const std::uintmax_t bytes = std::filesystem::file_size(native, error);
        if (error) return status_from(error);
        information.size = static_cast<std::uint64_t>(bytes);
    }
    const std::filesystem::file_time_type stamp = std::filesystem::last_write_time(native, error);
    if (!error) {
        const std::chrono::sys_time<std::filesystem::file_time_type::duration> system =
            std::chrono::file_clock::to_sys(stamp);
        information.modified = WallClock::time_point(
            std::chrono::duration_cast<WallClock::duration>(system.time_since_epoch()));
    }
    return FileStatus::ok;
}

FileStatus replace_durably(const std::filesystem::path& from,
                           const std::filesystem::path& to) noexcept {
    const std::wstring source = wide_name(from);
    const std::wstring target = wide_name(to);
    if (!::MoveFileExW(source.c_str(), target.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        return status_from_last_error();
    return FileStatus::ok;
}

} // namespace stx_auxlib::file_backend
