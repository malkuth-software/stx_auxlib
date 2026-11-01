// Ending the process and the fault handler (include/stx_auxlib/process.hpp).
// The exit tasks run from one handler that std::atexit receives once, when
// the first task is registered.

#include "stx_auxlib/process.hpp"

#include "stx_auxlib/console.hpp"
#include "stx_auxlib/threads.hpp"

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <source_location>
#include <string_view>
#include <system_error>

namespace stx_auxlib {

namespace {

struct ExitTask final {
    void (*task)(void* context) noexcept{nullptr};
    void* context{nullptr};
};

// The registered tasks, a stack of `count` entries. Created on first use
// and never destroyed, so that it outlives every exit task and static
// destructor that might register or run one.
struct ExitTasks final {
    Mutex lock{};
    std::array<ExitTask, maximum_exit_tasks> tasks{};
    std::size_t count{0};
    bool handler_registered{false};
};

ExitTasks& exit_tasks() noexcept {
    static ExitTasks* const state = new ExitTasks();
    return *state;
}

// Runs the tasks newest first. Each is taken off the stack under the lock
// and run outside it, so a task may register more.
void run_exit_tasks() noexcept {
    ExitTasks& state = exit_tasks();
    for (;;) {
        ExitTask next{};
        {
            const ScopedLock lock(state.lock);
            if (state.count == 0) return;
            --state.count;
            next = state.tasks[state.count];
        }
        next.task(next.context);
    }
}

// Writes a fault report piece by piece, so that nothing allocates.
void report(const std::string_view piece) noexcept {
    (void)write_console(ConsoleStream::error, piece);
}

} // namespace

void exit_process(const std::uint8_t status) noexcept { std::exit(static_cast<int>(status)); }

void fail(const std::string_view message, const std::source_location location) noexcept {
    std::array<char, 16> line{};
    const std::to_chars_result written =
        std::to_chars(line.data(), line.data() + line.size(), location.line());
    const std::string_view line_text(line.data(),
                                     static_cast<std::size_t>(written.ptr - line.data()));
    report("fatal: ");
    report(message);
    report(" (");
    report(location.file_name());
    report(":");
    report(line_text);
    report(", ");
    report(location.function_name());
    report(")\n");
    std::abort();
}

void ensure(const bool condition, const std::string_view message,
            const std::source_location location) noexcept {
    if (!condition) fail(message, location);
}

ExitTaskStatus register_exit_task(void (*const task)(void* context) noexcept,
                                  void* const context) noexcept {
    if (task == nullptr) return ExitTaskStatus::invalid_task;
    ExitTasks& state = exit_tasks();
    const ScopedLock lock(state.lock);
    if (state.count == maximum_exit_tasks) return ExitTaskStatus::full;
    if (!state.handler_registered) {
        if (std::atexit(run_exit_tasks) != 0) return ExitTaskStatus::full;
        state.handler_registered = true;
    }
    state.tasks[state.count] = ExitTask{task, context};
    ++state.count;
    return ExitTaskStatus::ok;
}

} // namespace stx_auxlib
