#pragma once

// Child processes (Capability::child_processes), replacing fork, exec,
// posix_spawn, waitpid, kill and CreateProcess.
//
// Settled platform differences:
// - A child starts directly from a program and an argument list; no shell
//   ever interprets them, so arguments cannot inject commands. On Windows
//   the arguments are quoted for the C runtime's standard parsing
//   (CommandLineToArgvW rules), so the child sees exactly these strings.
// - A program that is not an absolute path is searched for in PATH.
// - Failure to start (a missing program, no permission) is reported by
//   start, never by a child that exits at once with status 127.
// - Every child's standard input is the null device; its output and error
//   are inherited, discarded or captured together through one pipe.
// - A child is waited for with a timeout on every host.
// - A Child destroyed without having been waited for is not left a zombie:
//   it is reaped later, without blocking, by the next start or wait.

#include "stx_auxlib/capabilities.hpp"
#include "stx_auxlib/clocks.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <variant>
#include <vector>

namespace stx_auxlib {

enum class ChildStreams : std::uint8_t {
    // Output and error are the parent's.
    inherit = 0,
    // Output and error go to the null device.
    discard = 1,
    // Output and error go to one pipe that read_output reads.
    capture = 2,
};

struct ChildRequest final {
    // An absolute path, or a bare name searched for in PATH. UTF-8.
    std::string program{};
    // The arguments after the program name. UTF-8; may not contain NUL.
    std::vector<std::string> arguments{};
    ChildStreams streams{ChildStreams::inherit};
    // Starts the child in its own session (POSIX) or process group without
    // a console (Windows), so that terminal signals and the parent's
    // console do not reach it. Use for programs that outlive the parent.
    bool detached{false};
};

// How a child ended: an exit status, or (POSIX hosts) the number of the
// signal that ended it. Never both.
class ChildEnd final {
public:
    [[nodiscard]] static ChildEnd exited(std::uint32_t status) noexcept;
    [[nodiscard]] static ChildEnd signalled(std::uint32_t signal) noexcept;
    // The exit status; null when a signal ended the child.
    [[nodiscard]] const std::uint32_t* exit_status() const noexcept;
    // The signal number; null when the child exited.
    [[nodiscard]] const std::uint32_t* signal() const noexcept;

private:
    struct Exited final {
        std::uint32_t status{0};
    };
    struct Signalled final {
        std::uint32_t signal{0};
    };
    using State = std::variant<Exited, Signalled>;
    explicit ChildEnd(State state) noexcept;
    State state_;
};

// One started child process, owned. Move-only; a moved-from Child is empty.
class Child final {
public:
    [[nodiscard]] static CapabilityValue<Child> start(const ChildRequest& request);
    Child() noexcept;
    ~Child();
    Child(const Child&) = delete;
    Child& operator=(const Child&) = delete;
    Child(Child&& other) noexcept;
    // Releases this child first, as the destructor does.
    Child& operator=(Child&& other) noexcept;

    // Waits until the child ends or `timeout` passes (timed_out; the child
    // keeps running and can be waited for again). After a child has been
    // waited for successfully, later calls report invalid_state.
    [[nodiscard]] CapabilityValue<ChildEnd> wait(MonotonicClock::duration timeout) noexcept;
    // Ends the child forcibly (SIGKILL, TerminateProcess). Still wait for it.
    [[nodiscard]] CapabilityStatus kill() noexcept;
    // Appends the child's captured output and error to `output` until the
    // child closes them; too_large, after discarding the rest, when more
    // than `limit` bytes arrive. Read before waiting: a child blocked on a
    // full pipe never ends. invalid_state unless started with capture.
    [[nodiscard]] CapabilityStatus read_output(std::size_t limit, std::string& output);

    // The started child's host state, defined in src/hosted/children_<host>.cpp.
    struct State;

private:
    explicit Child(std::unique_ptr<State> state) noexcept;
    std::unique_ptr<State> state_{};
};

} // namespace stx_auxlib
