#pragma once

// The interactive terminal (Capability::raw_terminal): its size, and raw
// mode for full-screen terminal programs.
//
// Raw mode means: input arrives byte by byte without echo or line editing,
// and control keys (Ctrl+C, Ctrl+Z) arrive as bytes instead of signals;
// output is not translated ("\n" does not become "\r\n"). On Windows the
// console additionally switches to virtual-terminal sequences in both
// directions, so the same escape sequences work on every host.

#include "stx_auxlib/capabilities.hpp"

#include <cstdint>
#include <memory>

namespace stx_auxlib {

struct TerminalSize final {
    std::uint32_t columns{0};
    std::uint32_t rows{0};
};

// The size of the terminal on standard output; not_a_terminal when it is
// redirected.
[[nodiscard]] CapabilityValue<TerminalSize> terminal_size() noexcept;

// Holds the terminal in raw mode for its lifetime and restores the earlier
// mode when destroyed. Requires standard input and output to be terminals.
// One at a time: entering while another is active reports busy. Move-only;
// a moved-from RawTerminal restores nothing.
class RawTerminal final {
public:
    [[nodiscard]] static CapabilityValue<RawTerminal> enter();
    RawTerminal() noexcept;
    ~RawTerminal();
    RawTerminal(const RawTerminal&) = delete;
    RawTerminal& operator=(const RawTerminal&) = delete;
    RawTerminal(RawTerminal&& other) noexcept;
    RawTerminal& operator=(RawTerminal&& other) noexcept;

    [[nodiscard]] bool active() const noexcept;

    // The saved modes, defined in src/hosted/terminal_<host>.cpp.
    struct State;

private:
    explicit RawTerminal(std::unique_ptr<State> state) noexcept;
    std::unique_ptr<State> state_{};
};

} // namespace stx_auxlib
