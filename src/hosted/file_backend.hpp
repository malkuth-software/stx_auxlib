#pragma once

// The primitives each host file backend supplies (file_posix.cpp,
// file_windows.cpp); exactly one is linked. files.cpp builds the public
// operations (include/stx_auxlib/files.hpp) on them and on
// std::filesystem.
//
// A handle is a POSIX descriptor or a Windows HANDLE held in an intptr_t;
// -1 means none on both (INVALID_HANDLE_VALUE is -1). Every primitive is
// noexcept, retries interrupted calls, and reports host errors as
// FileStatus.

#include "stx_auxlib/files.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <system_error>

namespace stx_auxlib::file_backend {

constexpr std::intptr_t no_handle = -1;

// Opens `native` for `mode`. Opening a directory for reading fails with
// is_a_directory.
[[nodiscard]] FileStatus open(const std::filesystem::path& native, FileMode mode,
                              std::intptr_t& handle) noexcept;
// Reads at most output.size() bytes; zero transferred with ok is the end.
[[nodiscard]] FileStatus read(std::intptr_t handle, std::span<std::byte> output,
                              std::size_t& transferred) noexcept;
// Writes a prefix of `input`, at least one byte unless it fails.
[[nodiscard]] FileStatus write(std::intptr_t handle, std::span<const std::byte> input,
                               std::size_t& transferred) noexcept;
[[nodiscard]] FileStatus size(std::intptr_t handle, std::uint64_t& bytes) noexcept;
[[nodiscard]] FileStatus position(std::intptr_t handle, std::uint64_t& offset) noexcept;
[[nodiscard]] FileStatus seek(std::intptr_t handle, std::uint64_t offset) noexcept;
// Waits until the file's content is on stable storage.
[[nodiscard]] FileStatus sync(std::intptr_t handle) noexcept;
// Releases the handle even when it reports a failure.
[[nodiscard]] FileStatus close(std::intptr_t handle) noexcept;
// Describes `native` with one host call (stat or lstat on POSIX hosts).
[[nodiscard]] FileStatus inspect(const std::filesystem::path& native, LinkMode links,
                                 FileInformation& information) noexcept;
// Renames `from` over `to` atomically and makes the rename durable.
[[nodiscard]] FileStatus replace_durably(const std::filesystem::path& from,
                                         const std::filesystem::path& to) noexcept;

// The FileStatus for a host error code in the generic category (errno
// values, and std::filesystem's codes on every host). Defined in files.cpp.
[[nodiscard]] FileStatus status_from(const std::error_code& error) noexcept;

} // namespace stx_auxlib::file_backend
