#pragma once

// The process environment: named UTF-8 values inherited from the parent
// and passed to child processes.
//
// Settled platform differences:
// - Names are limited to the portable set: an ASCII letter or '_', then
//   letters, digits and '_'. Other names are refused, so a name means the
//   same on every host. Hosts still differ in case: Windows treats "Path"
//   and "PATH" as one variable, POSIX hosts as two; use upper-case names.
// - Values are UTF-8 on every host. Windows reads and writes the wide
//   Win32 environment block, not the C runtime's ANSI copy (which getenv
//   reads and which child processes do not inherit).
// - The host's getenv and setenv race when called concurrently; these
//   functions serialize with one another. Code that still calls the host's
//   functions directly is not covered.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <variant>

namespace stx_auxlib {

enum class EnvironmentStatus : std::uint8_t {
    ok = 0,
    // The variable is not set.
    not_set = 1,
    // The name is not in the portable set.
    invalid_name = 2,
    // The value is not UTF-8, or contains NUL.
    invalid_value = 3,
    // The value is longer than the caller's limit, or than the host allows.
    too_long = 4,
    // The host refused the change (for example, out of memory).
    host_error = 5,
};

// Bounds every value read or written, in bytes; Windows allows at most
// 32,767 UTF-16 code units.
constexpr std::size_t maximum_environment_bytes = 32767;

[[nodiscard]] bool is_portable_variable_name(std::string_view name) noexcept;

// Replaces `value` with the variable's value. Refuses (too_long) a value of
// more than `limit` bytes, before copying it. On failure `value` is empty.
[[nodiscard]] EnvironmentStatus read_variable(std::string_view name, std::size_t limit,
                                              std::string& value);
// Sets the variable, replacing any value it had.
[[nodiscard]] EnvironmentStatus set_variable(std::string_view name, std::string_view value);
// Removes the variable; ok when it was not set.
[[nodiscard]] EnvironmentStatus remove_variable(std::string_view name);

} // namespace stx_auxlib
