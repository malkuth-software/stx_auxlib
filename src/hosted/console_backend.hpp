#pragma once

// The primitives each host console backend supplies (console_posix.cpp,
// console_windows.cpp); exactly one is linked. console.cpp validates text,
// serializes calls and assembles lines on them.

#include "stx_auxlib/console.hpp"

#include <cstddef>
#include <span>
#include <string_view>

namespace stx_auxlib::console_backend {

// Writes all of `text`, valid UTF-8, to the stream.
[[nodiscard]] ConsoleStatus write(ConsoleStream stream, std::string_view text) noexcept;
// Writes `text` and then "\n", in one host call where the host can.
[[nodiscard]] ConsoleStatus write_line(ConsoleStream stream, std::string_view text) noexcept;
[[nodiscard]] bool is_terminal(ConsoleStream stream) noexcept;
// Reads UTF-8 bytes from standard input into `buffer` (at least 16 bytes);
// zero transferred with ok is the end of input.
[[nodiscard]] ConsoleStatus read(std::span<char> buffer, std::size_t& transferred) noexcept;

} // namespace stx_auxlib::console_backend
