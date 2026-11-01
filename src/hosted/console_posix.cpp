// The POSIX console backend (console_backend.hpp), over descriptors 0, 1
// and 2.

#include "console_backend.hpp"

#include <cerrno>
#include <cstddef>
#include <span>
#include <string_view>

#include <sys/uio.h>
#include <unistd.h>

namespace stx_auxlib::console_backend {

namespace {

int descriptor(const ConsoleStream stream) noexcept {
    if (stream == ConsoleStream::error) return STDERR_FILENO;
    return STDOUT_FILENO;
}

ConsoleStatus status_from_errno(const int error) noexcept {
    if (error == EPIPE || error == EBADF) return ConsoleStatus::closed;
    return ConsoleStatus::io_error;
}

} // namespace

ConsoleStatus write(const ConsoleStream stream, const std::string_view text) noexcept {
    const int fd = descriptor(stream);
    std::size_t written = 0;
    while (written < text.size()) {
        const ssize_t count = ::write(fd, text.data() + written, text.size() - written);
        if (count < 0) {
            if (errno == EINTR) continue;
            return status_from_errno(errno);
        }
        if (count == 0) return ConsoleStatus::io_error;
        written += static_cast<std::size_t>(count);
    }
    return ConsoleStatus::ok;
}

// One writev for the text and its newline; a partial write finishes with
// plain writes of what is left.
ConsoleStatus write_line(const ConsoleStream stream, const std::string_view text) noexcept {
    const int fd = descriptor(stream);
    char newline = '\n';
    iovec pieces[2]{};
    pieces[0].iov_base = const_cast<char*>(text.data());
    pieces[0].iov_len = text.size();
    pieces[1].iov_base = &newline;
    pieces[1].iov_len = 1;
    ssize_t count = ::writev(fd, pieces, 2);
    while (count < 0 && errno == EINTR) count = ::writev(fd, pieces, 2);
    if (count < 0) return status_from_errno(errno);
    const std::size_t written = static_cast<std::size_t>(count);
    if (written == text.size() + 1) return ConsoleStatus::ok;
    if (written < text.size()) {
        const ConsoleStatus rest = write(stream, text.substr(written));
        if (rest != ConsoleStatus::ok) return rest;
    }
    const ConsoleStatus result = write(stream, "\n");
    return result;
}

bool is_terminal(const ConsoleStream stream) noexcept {
    const bool result = ::isatty(descriptor(stream)) == 1;
    return result;
}

ConsoleStatus read(const std::span<char> buffer, std::size_t& transferred) noexcept {
    transferred = 0;
    for (;;) {
        const ssize_t count = ::read(STDIN_FILENO, buffer.data(), buffer.size());
        if (count >= 0) {
            transferred = static_cast<std::size_t>(count);
            return ConsoleStatus::ok;
        }
        if (errno != EINTR) return status_from_errno(errno);
    }
}

} // namespace stx_auxlib::console_backend
