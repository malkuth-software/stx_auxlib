#pragma once

// Ending the process, and the program's one fault handler, replacing exit,
// abort, atexit and assert.
//
// Settled platform differences:
// - Exit statuses are 0 to 255: POSIX hosts keep only the low eight bits of
//   a wider status, so a wider one would read differently by host.
// - fail() always reports the same way (one line on standard error, naming
//   the source location) and then aborts, so a fault leaves a crash report
//   or core where the host makes one. ensure() is never compiled out,
//   unlike assert, which NDEBUG removes.
// - Exit tasks take a context pointer, which atexit's do not.

#include <cstddef>
#include <cstdint>
#include <source_location>
#include <string_view>

namespace stx_auxlib {

// Ends the process with `status`: runs the exit tasks (newest first), then
// the C and C++ library's own exit handling (static destructors, atexit),
// which flushes standard streams. Call it only when no other thread can be
// using the objects that static destructors destroy.
[[noreturn]] void exit_process(std::uint8_t status) noexcept;

// Reports a fault, a condition the program cannot continue past, on
// standard error as "fatal: <message> (<file>:<line>, <function>)", and
// aborts. On POSIX hosts it allocates nothing, so it works when memory is
// exhausted (on a Windows console the UTF-16 conversion allocates). No exit
// task or static destructor runs.
[[noreturn]] void fail(std::string_view message,
                       std::source_location location = std::source_location::current()) noexcept;

// fail(message) unless `condition` holds.
void ensure(bool condition, std::string_view message,
            std::source_location location = std::source_location::current()) noexcept;

enum class ExitTaskStatus : std::uint8_t {
    ok = 0,
    // All maximum_exit_tasks places are taken.
    full = 1,
    invalid_task = 2,
};

constexpr std::size_t maximum_exit_tasks = 32;

// Registers `task` to run with `context` when the process exits normally
// (exit_process, or returning from main), newest first. A task must not
// throw; it may register further tasks, which run after it. `context` is
// borrowed until the task has run.
[[nodiscard]] ExitTaskStatus register_exit_task(void (*task)(void* context) noexcept,
                                                void* context) noexcept;

} // namespace stx_auxlib
