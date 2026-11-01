// Scenarios for stx_auxlib's console (console.hpp) and process control
// (process.hpp). Each mode does one thing whose effect is visible only from
// outside the process (its output, its input, its exit status);
// console_process_check.cmake runs the modes and checks those effects.
//
//   write             writes to both streams, interleaved with printf
//   prepare <path>    writes the input file that `read` is run with
//   read              reads standard input line by line (limit 16)
//   exit              registers exit tasks and exits with status 7
//   fail              faults through ensure and fail

#include "stx_auxlib/console.hpp"
#include "stx_auxlib/files.hpp"
#include "stx_auxlib/paths.hpp"
#include "stx_auxlib/process.hpp"

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

void say(const std::string_view text) {
    (void)stx_auxlib::write_console_line(stx_auxlib::ConsoleStream::output, text);
}

int write_mode() {
    std::printf("from printf\n");
    say("after printf");
    say("h\xc3\xa9llo w\xc3\xb6rld \xf0\x9f\x98\x80");
    if (stx_auxlib::write_console(stx_auxlib::ConsoleStream::output, "\xff") ==
        stx_auxlib::ConsoleStatus::invalid_text)
        say("invalid text refused");
    say(stx_auxlib::console_is_terminal(stx_auxlib::ConsoleStream::output) ? "terminal: yes"
                                                                         : "terminal: no");
    (void)stx_auxlib::write_console(stx_auxlib::ConsoleStream::error, "to stderr\n");
    return 0;
}

int prepare_mode(const char* const path) {
    const stx_auxlib::PathParse parsed = stx_auxlib::Path::parse(path);
    if (!parsed.succeeded()) return 2;
    const std::string_view content =
        "first\r\nsecond\n0123456789abcdefXYZ\n\xff\xfe\nexactly-sixteen!\nlast";
    std::vector<std::byte> bytes(content.size());
    std::memcpy(bytes.data(), content.data(), content.size());
    const stx_auxlib::FileStatus status = stx_auxlib::write_file(*parsed.path(), bytes);
    return status == stx_auxlib::FileStatus::ok ? 0 : 3;
}

int read_mode() {
    std::string line{};
    for (;;) {
        const stx_auxlib::ConsoleStatus status = stx_auxlib::read_console_line(16, line);
        if (status == stx_auxlib::ConsoleStatus::end_of_input) {
            say("end");
            return 0;
        }
        if (status == stx_auxlib::ConsoleStatus::ok) {
            say("ok " + line);
        } else if (status == stx_auxlib::ConsoleStatus::too_long) {
            say("too_long");
        } else if (status == stx_auxlib::ConsoleStatus::invalid_text) {
            say("invalid_text");
        } else {
            say("error");
            return 1;
        }
    }
}

void first_task(void*) noexcept { say("task one"); }

void third_task(void*) noexcept { say("task three, registered by task two"); }

void second_task(void*) noexcept {
    say("task two");
    (void)stx_auxlib::register_exit_task(third_task, nullptr);
}

int exit_mode() {
    if (stx_auxlib::register_exit_task(nullptr, nullptr) == stx_auxlib::ExitTaskStatus::invalid_task)
        say("null task refused");
    (void)stx_auxlib::register_exit_task(first_task, nullptr);
    (void)stx_auxlib::register_exit_task(second_task, nullptr);
    stx_auxlib::exit_process(7);
}

int fail_mode() {
    stx_auxlib::ensure(true, "this must not fire");
    say("before the fault");
    stx_auxlib::ensure(1 + 1 == 3, "broken invariant");
    say("after the fault");
    return 0;
}

} // namespace

int main(const int argument_count, char** const arguments) {
    if (argument_count < 2) return 2;
    const std::string_view mode = arguments[1];
    if (mode == "write") return write_mode();
    if (mode == "prepare" && argument_count == 3) return prepare_mode(arguments[2]);
    if (mode == "read") return read_mode();
    if (mode == "exit") return exit_mode();
    if (mode == "fail") return fail_mode();
    return 2;
}
