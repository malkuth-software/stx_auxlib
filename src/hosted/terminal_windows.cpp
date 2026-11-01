// The terminal on Windows (include/stx_auxlib/terminal.hpp), over the
// console modes. Raw mode switches both directions to virtual-terminal
// sequences, so full-screen programs use the same escape sequences as on
// POSIX hosts.

#include "stx_auxlib/terminal.hpp"

#include <atomic>
#include <memory>
#include <new>
#include <utility>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace stx_auxlib {

struct RawTerminal::State final {
    DWORD input_mode{0};
    DWORD output_mode{0};
};

namespace {

std::atomic<bool> raw_active{false};

bool console_mode(const DWORD which, DWORD& mode) noexcept {
    const HANDLE handle = ::GetStdHandle(which);
    const bool result = handle != nullptr && handle != INVALID_HANDLE_VALUE &&
                        ::GetConsoleMode(handle, &mode) != 0;
    return result;
}

void set_mode(const DWORD which, const DWORD mode) noexcept {
    (void)::SetConsoleMode(::GetStdHandle(which), mode);
}

} // namespace

CapabilityValue<TerminalSize> terminal_size() noexcept {
    CONSOLE_SCREEN_BUFFER_INFO information{};
    const HANDLE output = ::GetStdHandle(STD_OUTPUT_HANDLE);
    if (output == nullptr || output == INVALID_HANDLE_VALUE ||
        !::GetConsoleScreenBufferInfo(output, &information))
        return CapabilityValue<TerminalSize>::failure(CapabilityStatus::not_a_terminal);
    TerminalSize size{};
    size.columns = static_cast<std::uint32_t>(information.srWindow.Right - information.srWindow.Left + 1);
    size.rows = static_cast<std::uint32_t>(information.srWindow.Bottom - information.srWindow.Top + 1);
    return CapabilityValue<TerminalSize>::success(size);
}

RawTerminal::RawTerminal() noexcept = default;

RawTerminal::RawTerminal(std::unique_ptr<State> state) noexcept : state_(std::move(state)) {}

RawTerminal::~RawTerminal() {
    if (!state_) return;
    set_mode(STD_INPUT_HANDLE, (*state_).input_mode);
    set_mode(STD_OUTPUT_HANDLE, (*state_).output_mode);
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
    DWORD input_mode = 0;
    DWORD output_mode = 0;
    if (!console_mode(STD_INPUT_HANDLE, input_mode) || !console_mode(STD_OUTPUT_HANDLE, output_mode))
        return CapabilityValue<RawTerminal>::failure(CapabilityStatus::not_a_terminal);
    if (raw_active.exchange(true, std::memory_order_acq_rel))
        return CapabilityValue<RawTerminal>::failure(CapabilityStatus::busy);
    std::unique_ptr<State> state(new (std::nothrow) State());
    if (!state) {
        raw_active.store(false, std::memory_order_release);
        return CapabilityValue<RawTerminal>::failure(CapabilityStatus::resources_exhausted);
    }
    (*state).input_mode = input_mode;
    (*state).output_mode = output_mode;
    const DWORD raw_input = (input_mode & ~static_cast<DWORD>(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT |
                                                              ENABLE_PROCESSED_INPUT)) |
                            ENABLE_VIRTUAL_TERMINAL_INPUT;
    const DWORD raw_output =
        output_mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING | DISABLE_NEWLINE_AUTO_RETURN;
    if (!::SetConsoleMode(::GetStdHandle(STD_INPUT_HANDLE), raw_input) ||
        !::SetConsoleMode(::GetStdHandle(STD_OUTPUT_HANDLE), raw_output)) {
        set_mode(STD_INPUT_HANDLE, input_mode);
        set_mode(STD_OUTPUT_HANDLE, output_mode);
        raw_active.store(false, std::memory_order_release);
        return CapabilityValue<RawTerminal>::failure(CapabilityStatus::unsupported);
    }
    return CapabilityValue<RawTerminal>::success(RawTerminal(std::move(state)));
}

} // namespace stx_auxlib
