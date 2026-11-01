// The terminal on POSIX hosts (include/stx_auxlib/terminal.hpp), over
// termios and TIOCGWINSZ.

#include "stx_auxlib/terminal.hpp"

#include <atomic>
#include <cerrno>
#include <memory>
#include <new>
#include <utility>

#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

namespace stx_auxlib {

struct RawTerminal::State final {
    termios original{};
};

namespace {

// True while a RawTerminal holds the terminal.
std::atomic<bool> raw_active{false};

void restore(const termios& original) noexcept {
    while (::tcsetattr(STDIN_FILENO, TCSANOW, &original) != 0 && errno == EINTR) {
    }
}

} // namespace

CapabilityValue<TerminalSize> terminal_size() noexcept {
    if (::isatty(STDOUT_FILENO) != 1)
        return CapabilityValue<TerminalSize>::failure(CapabilityStatus::not_a_terminal);
    winsize dimensions{};
    if (::ioctl(STDOUT_FILENO, TIOCGWINSZ, &dimensions) != 0)
        return CapabilityValue<TerminalSize>::failure(CapabilityStatus::io_error);
    TerminalSize size{};
    size.columns = dimensions.ws_col;
    size.rows = dimensions.ws_row;
    return CapabilityValue<TerminalSize>::success(size);
}

RawTerminal::RawTerminal() noexcept = default;

RawTerminal::RawTerminal(std::unique_ptr<State> state) noexcept : state_(std::move(state)) {}

RawTerminal::~RawTerminal() {
    if (!state_) return;
    restore((*state_).original);
    raw_active.store(false, std::memory_order_release);
}

RawTerminal::RawTerminal(RawTerminal&& other) noexcept : state_(std::move(other.state_)) {}

RawTerminal& RawTerminal::operator=(RawTerminal&& other) noexcept {
    if (this == &other) return *this;
    RawTerminal released(std::move(*this));
    state_ = std::move(other.state_);
    return *this;
}

bool RawTerminal::active() const noexcept {
    const bool result = static_cast<bool>(state_);
    return result;
}

CapabilityValue<RawTerminal> RawTerminal::enter() {
    if (::isatty(STDIN_FILENO) != 1 || ::isatty(STDOUT_FILENO) != 1)
        return CapabilityValue<RawTerminal>::failure(CapabilityStatus::not_a_terminal);
    if (raw_active.exchange(true, std::memory_order_acq_rel))
        return CapabilityValue<RawTerminal>::failure(CapabilityStatus::busy);
    std::unique_ptr<State> state(new (std::nothrow) State());
    if (!state) {
        raw_active.store(false, std::memory_order_release);
        return CapabilityValue<RawTerminal>::failure(CapabilityStatus::resources_exhausted);
    }
    if (::tcgetattr(STDIN_FILENO, &(*state).original) != 0) {
        raw_active.store(false, std::memory_order_release);
        return CapabilityValue<RawTerminal>::failure(CapabilityStatus::io_error);
    }
    termios raw = (*state).original;
    ::cfmakeraw(&raw);
    if (::tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0) {
        raw_active.store(false, std::memory_order_release);
        return CapabilityValue<RawTerminal>::failure(CapabilityStatus::io_error);
    }
    return CapabilityValue<RawTerminal>::success(RawTerminal(std::move(state)));
}

} // namespace stx_auxlib
