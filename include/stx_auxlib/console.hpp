#pragma once

// The console: the process's standard output, standard error and standard
// input, as UTF-8 text.
//
// Settled platform differences:
// - Text is UTF-8 everywhere. On a Windows console it is written and read
//   as UTF-16 through the console API, so it displays correctly whatever
//   the console's code page; redirected to a file or pipe it is UTF-8 bytes,
//   as on POSIX hosts.
// - Writes are unbuffered and whole: each call writes all its text before
//   returning, and concurrent calls never interleave within one call. The C
//   library's stdout and stderr buffers are flushed first, so output keeps
//   its order with code that still uses printf during migration.
// - read_console_line strips "\n" and "\r\n" alike.
//
// Not settled here: on POSIX hosts, writing to a pipe whose reader has
// closed raises SIGPIPE, which ends the program unless it ignores that
// signal; with the signal ignored, the write reports closed.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace stx_auxlib {

enum class ConsoleStream : std::uint8_t {
    output = 0,
    error = 1,
};

enum class ConsoleStatus : std::uint8_t {
    ok = 0,
    // No more input will arrive.
    end_of_input = 1,
    // The line exceeded the caller's limit; the rest of it was discarded.
    too_long = 2,
    // The stream is closed or absent (a closed pipe, a process without one).
    closed = 3,
    // The text is not UTF-8; nothing was written, or the line was consumed.
    invalid_text = 4,
    io_error = 5,
};

[[nodiscard]] ConsoleStatus write_console(ConsoleStream stream, std::string_view text) noexcept;
// `text` followed by "\n", under the same lock, so no other call's text
// comes between them.
[[nodiscard]] ConsoleStatus write_console_line(ConsoleStream stream, std::string_view text) noexcept;
// True when the stream is an interactive terminal rather than a file or pipe.
[[nodiscard]] bool console_is_terminal(ConsoleStream stream) noexcept;

// Replaces `line` with the next line of standard input, without its line
// ending. A last line without an ending is returned as a line; the call
// after it reports end_of_input. Refuses (too_long) a line of more than
// `limit` bytes. On failure `line` is empty.
[[nodiscard]] ConsoleStatus read_console_line(std::size_t limit, std::string& line);

} // namespace stx_auxlib
