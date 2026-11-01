#pragma once

// Files and directories, named by stx_auxlib::Path. Every operation reports
// an explicit FileStatus; none throws for a filesystem condition. Host
// errors map onto the same FileStatus values on every host.
//
// Settled platform differences:
// - read_file and list_directory take a caller limit and refuse larger
//   content before allocating for it.
// - write_file replaces a file atomically and durably: readers see the old
//   or the new content, never a mixture, and the new content is on stable
//   storage when it returns ok (F_FULLFSYNC on Apple hosts, fsync on other
//   POSIX hosts, write-through on Windows).
// - list_directory returns entries sorted by name bytes, so every host
//   lists a directory in the same order.
// - Times are WallClock (Unix time) on every host.
//
// Concurrency: operations on distinct File objects may run on any threads.
// The filesystem itself is shared with other programs; nothing here can
// make a sequence of operations atomic against them.

#include "stx_auxlib/clocks.hpp"
#include "stx_auxlib/paths.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace stx_auxlib {

enum class FileStatus : std::uint8_t {
    ok = 0,
    not_found = 1,
    already_exists = 2,
    permission_denied = 3,
    not_a_directory = 4,
    is_a_directory = 5,
    directory_not_empty = 6,
    no_space = 7,
    // The content exceeds the caller's limit; nothing was read past it.
    too_large = 8,
    name_too_long = 9,
    // The host refused the name, or reported a name that is not a valid
    // Path segment.
    invalid_name = 10,
    // The host does not support this path form or operation.
    unsupported = 11,
    // A rename across volumes, which cannot be atomic.
    cross_device = 12,
    // The file is locked or in use by another program.
    busy = 13,
    // The File is closed, or was opened without the access this needs.
    bad_handle = 14,
    // Any other host failure.
    io_error = 15,
};

// The result of an operation that yields a value: the value, or a status
// other than ok. Never both.
template <typename Value>
class FileValue final {
public:
    [[nodiscard]] static FileValue success(Value value) {
        FileValue result(State(std::in_place_index<0>, std::move(value)));
        return result;
    }
    // `status` must not be FileStatus::ok.
    [[nodiscard]] static FileValue failure(const FileStatus status) noexcept {
        FileValue result(State(std::in_place_index<1>, status));
        return result;
    }
    [[nodiscard]] bool succeeded() const noexcept {
        const bool result = std::holds_alternative<Value>(state_);
        return result;
    }
    // FileStatus::ok when the operation succeeded.
    [[nodiscard]] FileStatus status() const noexcept {
        const FileStatus* const failed = std::get_if<FileStatus>(&state_);
        if (failed == nullptr) return FileStatus::ok;
        return *failed;
    }
    // The value; null on failure.
    [[nodiscard]] const Value* value() const noexcept {
        const Value* const result = std::get_if<Value>(&state_);
        return result;
    }
    [[nodiscard]] Value* value() noexcept {
        Value* const result = std::get_if<Value>(&state_);
        return result;
    }

private:
    using State = std::variant<Value, FileStatus>;
    explicit FileValue(State state) noexcept : state_(std::move(state)) {}
    State state_;
};

enum class FileMode : std::uint8_t {
    // Read an existing file.
    read = 0,
    // Write a new file; already_exists if the name is taken.
    create_new = 1,
    // Write a file, creating it or discarding its old content.
    replace = 2,
    // Write at the end of a file, creating it if missing.
    append = 3,
};

// An open file. Move-only; the destructor closes it and discards any close
// failure, so call close() where that failure matters. A moved-from or
// closed File reports bad_handle.
class File final {
public:
    [[nodiscard]] static FileValue<File> open(const Path& path, FileMode mode) noexcept;
    File() noexcept;
    ~File();
    File(const File&) = delete;
    File& operator=(const File&) = delete;
    File(File&& other) noexcept;
    // Closes this file first, discarding any close failure.
    File& operator=(File&& other) noexcept;

    [[nodiscard]] bool is_open() const noexcept;
    // Reads up to output.size() bytes at the current position. Zero bytes
    // with ok means the end of the file.
    [[nodiscard]] FileValue<std::size_t> read(std::span<std::byte> output) noexcept;
    // Writes every byte or fails; on failure an unknown prefix may have been
    // written.
    [[nodiscard]] FileStatus write_all(std::span<const std::byte> input) noexcept;
    [[nodiscard]] FileValue<std::uint64_t> size() noexcept;
    [[nodiscard]] FileValue<std::uint64_t> position() noexcept;
    [[nodiscard]] FileStatus seek(std::uint64_t offset) noexcept;
    // Waits until written content is on stable storage.
    [[nodiscard]] FileStatus sync() noexcept;
    [[nodiscard]] FileStatus close() noexcept;

private:
    explicit File(std::intptr_t handle) noexcept;
    // A POSIX descriptor or a Windows handle; -1 when closed.
    std::intptr_t handle_{-1};
};

// Replaces `output` with the whole file. Refuses (too_large) a file of more
// than `limit` bytes before allocating for it. On failure `output` is empty;
// its capacity is kept either way.
[[nodiscard]] FileStatus read_file(const Path& path, std::size_t limit,
                                   std::vector<std::byte>& output);

// Atomically and durably replaces (or creates) the file with `content`,
// through a uniquely named temporary file in the same directory. On failure
// the old file, if any, is unchanged and the temporary file is removed.
[[nodiscard]] FileStatus write_file(const Path& path, std::span<const std::byte> content);

enum class FileKind : std::uint8_t {
    regular = 0,
    directory = 1,
    symbolic_link = 2,
    other = 3,
};

struct FileInformation final {
    FileKind kind{FileKind::other};
    // Bytes, for regular files; 0 otherwise.
    std::uint64_t size{0};
    WallClock::time_point modified{};
};

enum class LinkMode : std::uint8_t {
    // Describe what a symbolic link points to.
    follow = 0,
    // Describe a symbolic link itself.
    no_follow = 1,
};

// not_found when nothing has that name.
[[nodiscard]] FileValue<FileInformation> inspect(const Path& path, LinkMode links) noexcept;

// Creates one directory; already_exists if anything has that name.
[[nodiscard]] FileStatus create_directory(const Path& path) noexcept;
// Creates a directory and its missing parents; ok if it already exists as a
// directory.
[[nodiscard]] FileStatus create_directories(const Path& path) noexcept;
// Removes a file, a symbolic link or an empty directory.
[[nodiscard]] FileStatus remove(const Path& path) noexcept;
// Removes a directory tree without following symbolic links. Yields the
// number of entries removed; a missing path removes 0.
[[nodiscard]] FileValue<std::uint64_t> remove_tree(const Path& path) noexcept;
// Renames, atomically replacing an existing file at `to`.
[[nodiscard]] FileStatus rename(const Path& from, const Path& to) noexcept;

enum class CopyMode : std::uint8_t {
    fail_if_exists = 0,
    replace = 1,
};

[[nodiscard]] FileStatus copy_file(const Path& from, const Path& to, CopyMode mode) noexcept;

struct DirectoryEntry final {
    // One segment: the entry's name within the directory.
    std::string name{};
    FileKind kind{FileKind::other};
};

struct DirectoryListing final {
    // Sorted by name bytes.
    std::vector<DirectoryEntry> entries{};
    // Entries whose host names are not valid Path segments (invalid UTF-8,
    // or a reserved character); they are counted, not listed.
    std::size_t unnamed_entries{0};
};

// Replaces `output` with the directory's entries, without "." and "..".
// Refuses (too_large) a directory of more than `limit` entries. On failure
// `output` is empty.
[[nodiscard]] FileStatus list_directory(const Path& path, std::size_t limit,
                                        DirectoryListing& output);

// The host's directory for temporary files.
[[nodiscard]] FileValue<Path> temporary_directory();
// The process's working directory.
[[nodiscard]] FileValue<Path> working_directory();
// `path` made absolute against the working directory, lexically: no
// symbolic link is followed and the path need not exist.
[[nodiscard]] FileValue<Path> absolute(const Path& path);
// The existing file's absolute path with every symbolic link resolved.
[[nodiscard]] FileValue<Path> resolve(const Path& path);
// The target a symbolic link names, unresolved.
[[nodiscard]] FileValue<Path> read_link(const Path& path);
// Creates a symbolic link at `link` naming `target`. Hosts that restrict
// links (Windows without developer mode) report permission_denied or
// unsupported.
[[nodiscard]] FileStatus create_link(const Path& target, const Path& link) noexcept;

} // namespace stx_auxlib
