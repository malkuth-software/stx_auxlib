// The Windows console backend (console_backend.hpp). A console handle is
// written and read as UTF-16 through WriteConsoleW and ReadConsoleW, so the
// console's code page does not matter; a redirected handle (file or pipe)
// carries UTF-8 bytes through WriteFile and ReadFile.

#include "console_backend.hpp"

#include "../unicode.hpp"

#include <cstddef>
#include <cstring>
#include <span>
#include <string>
#include <string_view>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace stx_auxlib::console_backend {

namespace {

// One WriteConsoleW, WriteFile or read call moves at most this much.
constexpr DWORD largest_transfer = 1u << 16;

HANDLE handle_for(const ConsoleStream stream) noexcept {
    const DWORD which = stream == ConsoleStream::error ? STD_ERROR_HANDLE : STD_OUTPUT_HANDLE;
    const HANDLE result = ::GetStdHandle(which);
    return result;
}

bool usable(const HANDLE handle) noexcept {
    const bool result = handle != nullptr && handle != INVALID_HANDLE_VALUE;
    return result;
}

bool is_console(const HANDLE handle) noexcept {
    DWORD mode = 0;
    const bool result = ::GetConsoleMode(handle, &mode) != 0;
    return result;
}

ConsoleStatus status_from_last_error() noexcept {
    const DWORD error = ::GetLastError();
    if (error == ERROR_BROKEN_PIPE || error == ERROR_NO_DATA || error == ERROR_INVALID_HANDLE)
        return ConsoleStatus::closed;
    return ConsoleStatus::io_error;
}

ConsoleStatus write_console_text(const HANDLE handle, const std::string_view text) {
    std::u16string utf16{};
    if (!unicode::append_utf16(text, utf16)) return ConsoleStatus::invalid_text;
    std::size_t written = 0;
    while (written < utf16.size()) {
        const std::size_t remaining = utf16.size() - written;
        const DWORD request = remaining < largest_transfer ? static_cast<DWORD>(remaining)
                                                           : largest_transfer;
        DWORD count = 0;
        if (!::WriteConsoleW(handle, utf16.data() + written, request, &count, nullptr))
            return status_from_last_error();
        if (count == 0) return ConsoleStatus::io_error;
        written += count;
    }
    return ConsoleStatus::ok;
}

ConsoleStatus write_bytes(const HANDLE handle, const std::string_view text) noexcept {
    std::size_t written = 0;
    while (written < text.size()) {
        const std::size_t remaining = text.size() - written;
        const DWORD request = remaining < largest_transfer ? static_cast<DWORD>(remaining)
                                                           : largest_transfer;
        DWORD count = 0;
        if (!::WriteFile(handle, text.data() + written, request, &count, nullptr))
            return status_from_last_error();
        if (count == 0) return ConsoleStatus::io_error;
        written += count;
    }
    return ConsoleStatus::ok;
}

// A high surrogate read at the end of one ReadConsoleW, kept for the next.
char16_t pending_high_surrogate = 0;

} // namespace

ConsoleStatus write(const ConsoleStream stream, const std::string_view text) noexcept {
    const HANDLE handle = handle_for(stream);
    if (!usable(handle)) return ConsoleStatus::closed;
    if (is_console(handle)) return write_console_text(handle, text);
    const ConsoleStatus result = write_bytes(handle, text);
    return result;
}

ConsoleStatus write_line(const ConsoleStream stream, const std::string_view text) noexcept {
    const ConsoleStatus first = write(stream, text);
    if (first != ConsoleStatus::ok) return first;
    const ConsoleStatus result = write(stream, "\n");
    return result;
}

bool is_terminal(const ConsoleStream stream) noexcept {
    const HANDLE handle = handle_for(stream);
    const bool result = usable(handle) && is_console(handle);
    return result;
}

// Console input arrives as UTF-16 and is converted here; each UTF-16 unit
// becomes at most three UTF-8 bytes, so a read of buffer.size() / 3 units
// always fits. Ctrl+Z at the start of a read ends the input, as cmd does.
ConsoleStatus read(const std::span<char> buffer, std::size_t& transferred) noexcept {
    transferred = 0;
    const HANDLE handle = ::GetStdHandle(STD_INPUT_HANDLE);
    if (!usable(handle)) return ConsoleStatus::closed;
    if (!is_console(handle)) {
        const DWORD request = buffer.size() < largest_transfer ? static_cast<DWORD>(buffer.size())
                                                               : largest_transfer;
        DWORD count = 0;
        if (!::ReadFile(handle, buffer.data(), request, &count, nullptr)) {
            if (::GetLastError() == ERROR_BROKEN_PIPE) return ConsoleStatus::ok;
            return status_from_last_error();
        }
        transferred = count;
        return ConsoleStatus::ok;
    }
    // console.cpp passes a 4096-byte chunk; a buffer below 6 bytes could
    // not hold one converted unit plus a carried surrogate.
    if (buffer.size() < 6) return ConsoleStatus::io_error;
    const std::size_t units = buffer.size() / 3 - 1;
    std::u16string utf16(units + 1, u'\0');
    std::size_t total = 0;
    // Repeats only when a read delivered nothing but a high surrogate,
    // which must not look like the end of input.
    while (total == 0) {
        std::size_t start = 0;
        if (pending_high_surrogate != 0) {
            utf16[0] = pending_high_surrogate;
            pending_high_surrogate = 0;
            start = 1;
        }
        DWORD count = 0;
        if (!::ReadConsoleW(handle, utf16.data() + start, static_cast<DWORD>(units), &count,
                            nullptr))
            return status_from_last_error();
        if (count == 0) return ConsoleStatus::ok;
        if (utf16[start] == u'\x1a') return ConsoleStatus::ok;
        total = start + count;
        if (utf16[total - 1] >= 0xd800 && utf16[total - 1] <= 0xdbff) {
            pending_high_surrogate = utf16[total - 1];
            --total;
        }
    }
    utf16.resize(total);
    std::string utf8{};
    if (!unicode::append_utf8(utf16, utf8)) return ConsoleStatus::invalid_text;
    std::memcpy(buffer.data(), utf8.data(), utf8.size());
    transferred = utf8.size();
    return ConsoleStatus::ok;
}

} // namespace stx_auxlib::console_backend
