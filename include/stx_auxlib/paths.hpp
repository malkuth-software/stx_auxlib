#pragma once

// The program's one path type: UTF-8 text with '/' separators, validated
// and normalized when it is made, so that a Path means the same on every
// host. Conversion to a host's native form happens only inside
// src/hosted/. Path is pure computation and touches no filesystem.
//
// Syntax. A path is a root followed by segments separated by single '/'.
// The root is "/" (absolute), "X:/" for a drive letter X (absolute; always
// upper case), or none (relative). parse() normalizes: repeated '/' collapse,
// "." segments vanish, a ".." removes the segment before it, and a trailing
// '/' is dropped. A ".." at the start of a relative path stays; a ".." above
// an absolute root is refused (escapes_root), never silently dropped. The
// empty relative path is ".".
//
// Refused text: empty text, more than maximum_bytes bytes, a NUL, invalid
// UTF-8 (overlong forms, surrogates, values above U+10FFFF), '\' and ':'
// outside a drive root. '\' and ':' are refused because hosts disagree on
// them ('\' separates on Windows, ':' names streams on NTFS and drives
// elsewhere); refusing them keeps a path's meaning identical everywhere.
//
// Comparison is by bytes. Hosts whose filesystems ignore case or normalize
// Unicode may still treat two unequal Paths as one file.
//
// Hosts: POSIX hosts support "/" roots and report drive roots unsupported;
// Windows supports both ("/" is the current drive's root). UNC names
// ("//server/share") are not supported: their leading "//" collapses.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <variant>

namespace stx_auxlib {

enum class PathFailure : std::uint8_t {
    empty = 1,
    too_long = 2,
    embedded_nul = 3,
    invalid_utf8 = 4,
    reserved_character = 5,
    escapes_root = 6,
    absolute_join = 7,
};

class PathParse;

class Path final {
public:
    // Bounds every Path, before and after normalization, in bytes.
    static constexpr std::size_t maximum_bytes = 4096;

    // Validates and normalizes `text`. Allocates the result.
    [[nodiscard]] static PathParse parse(std::string_view text);

    // The relative path ".".
    Path();

    [[nodiscard]] std::string_view text() const noexcept;
    [[nodiscard]] bool is_absolute() const noexcept;
    // True for "/" and "X:/" alone.
    [[nodiscard]] bool is_root() const noexcept;
    // The drive letter of a "X:/" root, or 0.
    [[nodiscard]] char drive() const noexcept;

    // This path followed by `relative`, normalized. Fails with absolute_join
    // when `relative` is absolute, and with escapes_root or too_long as
    // parse does.
    [[nodiscard]] PathParse join(const Path& relative) const;
    // The path without its last segment: "a/b" -> "a", "a" -> ".",
    // "." -> "..", ".." -> "../..", and a root is its own parent.
    [[nodiscard]] Path parent() const;
    // The last segment, or empty for a root, "." and a path ending in "..".
    [[nodiscard]] std::string_view file_name() const noexcept;
    // The last segment's text from its last '.', including the dot
    // ("a.tar.gz" -> ".gz"); empty when there is none or the name starts
    // with its only dot (".profile").
    [[nodiscard]] std::string_view extension() const noexcept;

    [[nodiscard]] bool operator==(const Path& other) const noexcept;
    [[nodiscard]] bool operator!=(const Path& other) const noexcept;

private:
    friend class PathParse;
    explicit Path(std::string normalized);
    // The length of the root prefix: 0, 1 ("/") or 3 ("X:/").
    [[nodiscard]] std::size_t root_length() const noexcept;
    std::string text_;
};

// The result of Path::parse and Path::join: the path, or why it was refused.
class PathParse final {
public:
    [[nodiscard]] static PathParse parsed(Path path) noexcept;
    [[nodiscard]] static PathParse refused(PathFailure failure) noexcept;
    [[nodiscard]] bool succeeded() const noexcept;
    // The failure; null when the path parsed.
    [[nodiscard]] const PathFailure* failure() const noexcept;
    // The path; null when it was refused.
    [[nodiscard]] const Path* path() const noexcept;

private:
    using State = std::variant<Path, PathFailure>;
    explicit PathParse(State state) noexcept;
    State state_;
};

} // namespace stx_auxlib
