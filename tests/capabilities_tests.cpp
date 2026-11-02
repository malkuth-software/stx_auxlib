// Tests of stx_auxlib's capabilities (capabilities.hpp, children.hpp,
// desktop.hpp, terminal.hpp, events.hpp).
//
// The child processes are this program itself, started with a mode:
//   echo <arguments...>   prints each argument on its own line
//   exit <status>         exits with that status
//   sleep                 sleeps ten seconds
//   sigpipe               prints whether SIGPIPE is ignored (POSIX)
// The desktop functions are checked only for what they refuse, so the
// tests never open a browser or a file manager.
//
// On Windows, main's arguments are in the ANSI code page and stdout turns
// "\n" into "\r\n", so echo reads its arguments as UTF-8 from the wide
// command line (the CommandLineToArgvW rules children.hpp promises) and
// writes standard output in binary mode.

#include "stx_auxlib/capabilities.hpp"
#include "stx_auxlib/children.hpp"
#include "stx_auxlib/clocks.hpp"
#include "stx_auxlib/desktop.hpp"
#include "stx_auxlib/events.hpp"
#include "stx_auxlib/paths.hpp"
#include "stx_auxlib/terminal.hpp"
#include "stx_auxlib/threads.hpp"

#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#include <shellapi.h>
#endif

namespace {

void require(const bool condition, const char* const message) {
    if (condition) return;
    std::fprintf(stderr, "stx_auxlib capabilities: %s\n", message);
    std::abort();
}

std::string self_program() {
    const stx_auxlib::CapabilityValue<stx_auxlib::Path> self = stx_auxlib::current_executable();
    require(self.succeeded(), "current_executable failed");
    std::string result((*self.value()).text());
    return result;
}

stx_auxlib::Child start_self(const std::vector<std::string>& arguments,
                             const stx_auxlib::ChildStreams streams) {
    stx_auxlib::ChildRequest request{};
    request.program = self_program();
    request.arguments = arguments;
    request.streams = streams;
    stx_auxlib::CapabilityValue<stx_auxlib::Child> started = stx_auxlib::Child::start(request);
    require(started.succeeded(), "a child failed to start");
    stx_auxlib::Child result = std::move(*started.value());
    return result;
}

void availability() {
    require(stx_auxlib::capability_available(stx_auxlib::Capability::child_processes),
            "child processes are unavailable");
    require(stx_auxlib::capability_available(stx_auxlib::Capability::current_executable),
            "current_executable is unavailable");
    const std::string self = self_program();
    const stx_auxlib::PathParse parsed = stx_auxlib::Path::parse(self);
    require(parsed.succeeded() && (*parsed.path()).is_absolute(), "the executable is not absolute");
    require((*parsed.path()).file_name().find("stx_auxlib_capabilities_tests") == 0,
            "current_executable named another file");
}

void child_requests() {
    stx_auxlib::ChildRequest empty{};
    require(stx_auxlib::Child::start(empty).status() == stx_auxlib::CapabilityStatus::invalid_request,
            "an empty program started");
    stx_auxlib::ChildRequest with_nul{};
    with_nul.program = self_program();
    with_nul.arguments.push_back(std::string("a\0b", 3));
    require(stx_auxlib::Child::start(with_nul).status() == stx_auxlib::CapabilityStatus::invalid_request,
            "an argument with NUL was accepted");
    stx_auxlib::ChildRequest missing{};
    missing.program = "/nonexistent-stx-auxlib/program";
    require(stx_auxlib::Child::start(missing).status() == stx_auxlib::CapabilityStatus::not_found,
            "a missing absolute program was not reported");
    stx_auxlib::ChildRequest unlisted{};
    unlisted.program = "stx-auxlib-no-such-program";
    require(stx_auxlib::Child::start(unlisted).status() == stx_auxlib::CapabilityStatus::not_found,
            "a missing PATH program was not reported");
}

void child_output_and_status() {
    // Arguments reach the child exactly: spaces, quotes, backslashes, an
    // empty argument and UTF-8 are not reinterpreted by any shell.
    const std::vector<std::string> words = {"plain", "two words", "quote\"inside",
                                            "back\\slash\\", "", "caf\xc3\xa9 \xf0\x9f\x98\x80",
                                            "$HOME;rm -rf /"};
    std::vector<std::string> arguments = {"echo"};
    arguments.insert(arguments.end(), words.begin(), words.end());
    stx_auxlib::Child echo = start_self(arguments, stx_auxlib::ChildStreams::capture);
    std::string output{};
    require(echo.read_output(4096, output) == stx_auxlib::CapabilityStatus::ok, "read_output failed");
    std::string expected{};
    for (const std::string& word : words) expected += word + "\n";
    require(output == expected, "a child's arguments were changed");
    const stx_auxlib::CapabilityValue<stx_auxlib::ChildEnd> echo_end = echo.wait(std::chrono::seconds(30));
    require(echo_end.succeeded() && (*echo_end.value()).exit_status() != nullptr &&
                *(*echo_end.value()).exit_status() == 0,
            "echo did not exit with 0");
    require(echo.wait(std::chrono::seconds(1)).status() == stx_auxlib::CapabilityStatus::invalid_state,
            "a child was waited for twice");
    require(echo.read_output(16, output) == stx_auxlib::CapabilityStatus::invalid_state,
            "output was read twice");

    stx_auxlib::Child exiting = start_self({"exit", "3"}, stx_auxlib::ChildStreams::discard);
    const stx_auxlib::CapabilityValue<stx_auxlib::ChildEnd> exit_end = exiting.wait(std::chrono::seconds(30));
    require(exit_end.succeeded() && *(*exit_end.value()).exit_status() == 3, "exit status 3 was lost");

    // Over the limit: too_large, after the child's output is drained.
    stx_auxlib::Child chatty = start_self({"echo", std::string(5000, 'x')}, stx_auxlib::ChildStreams::capture);
    std::string clipped{};
    require(chatty.read_output(100, clipped) == stx_auxlib::CapabilityStatus::too_large,
            "output over the limit was not refused");
    require(chatty.wait(std::chrono::seconds(30)).succeeded(), "the chatty child did not end");

    // A detached child (its own session) still starts, exits and is waited for.
    stx_auxlib::ChildRequest detached{};
    detached.program = self_program();
    detached.arguments = {"exit", "5"};
    detached.streams = stx_auxlib::ChildStreams::discard;
    detached.detached = true;
    stx_auxlib::CapabilityValue<stx_auxlib::Child> detached_start = stx_auxlib::Child::start(detached);
    require(detached_start.succeeded(), "a detached child failed to start");
    const stx_auxlib::CapabilityValue<stx_auxlib::ChildEnd> detached_end =
        (*detached_start.value()).wait(std::chrono::seconds(30));
    require(detached_end.succeeded() && *(*detached_end.value()).exit_status() == 5,
            "a detached child's status was lost");

    stx_auxlib::Child no_capture = start_self({"exit", "0"}, stx_auxlib::ChildStreams::discard);
    require(no_capture.read_output(16, clipped) == stx_auxlib::CapabilityStatus::invalid_state,
            "output was read without capture");
    require(no_capture.wait(std::chrono::seconds(30)).succeeded(), "the child did not end");
}

void timeouts_and_kill() {
    stx_auxlib::Child sleeper = start_self({"sleep"}, stx_auxlib::ChildStreams::discard);
    const stx_auxlib::MonotonicClock::time_point before = stx_auxlib::MonotonicClock::now();
    require(sleeper.wait(std::chrono::milliseconds(50)).status() == stx_auxlib::CapabilityStatus::timed_out,
            "a sleeping child did not time out");
    require(stx_auxlib::MonotonicClock::now() - before >= std::chrono::milliseconds(50),
            "the timeout ended early");
    require(sleeper.wait(std::chrono::milliseconds(0)).status() == stx_auxlib::CapabilityStatus::timed_out,
            "a zero timeout did not poll once");
    require(sleeper.kill() == stx_auxlib::CapabilityStatus::ok, "kill failed");
    const stx_auxlib::CapabilityValue<stx_auxlib::ChildEnd> killed = sleeper.wait(std::chrono::seconds(30));
    require(killed.succeeded(), "a killed child could not be waited for");
#if !defined(_WIN32)
    require((*killed.value()).signal() != nullptr && *(*killed.value()).signal() == SIGKILL,
            "a killed child did not end by SIGKILL");
#endif
    require(sleeper.kill() == stx_auxlib::CapabilityStatus::invalid_state, "a waited child was killed");

    // Abandoned children are released without blocking the owner.
    for (int index = 0; index < 4; ++index) {
        const stx_auxlib::Child abandoned = start_self({"exit", "0"}, stx_auxlib::ChildStreams::discard);
    }
    stx_auxlib::Child later = start_self({"exit", "0"}, stx_auxlib::ChildStreams::discard);
    require(later.wait(std::chrono::seconds(30)).succeeded(), "a child after abandoned ones failed");
}

void desktop_refusals() {
    const char* const refused[] = {"-x", "--help", "file:///etc/passwd", "javascript:alert(1)",
                                   "relative/path", "https://example.com/\nsecond", ""};
    for (const char* const target : refused)
        require(stx_auxlib::open_with_desktop(target) == stx_auxlib::CapabilityStatus::invalid_request,
                "open_with_desktop accepted a refused target");
    const stx_auxlib::PathParse relative = stx_auxlib::Path::parse("some/file");
    require(stx_auxlib::reveal_in_file_manager(*relative.path()) ==
                stx_auxlib::CapabilityStatus::invalid_request,
            "reveal accepted a relative path");
}

void terminal_without_one() {
    // ctest runs with standard input and output redirected.
    require(stx_auxlib::terminal_size().status() == stx_auxlib::CapabilityStatus::not_a_terminal,
            "a redirected stream reported a terminal size");
    require(stx_auxlib::RawTerminal::enter().status() == stx_auxlib::CapabilityStatus::not_a_terminal,
            "raw mode was entered without a terminal");
}

#if !defined(_WIN32)
void events() {
    const stx_auxlib::EventSet none{};
    require(stx_auxlib::EventWatch::watch(none).status() == stx_auxlib::CapabilityStatus::invalid_request,
            "an empty watch was accepted");
    const stx_auxlib::EventSet watched = stx_auxlib::EventSet()
                                             .with(stx_auxlib::ProcessEvent::interrupt)
                                             .with(stx_auxlib::ProcessEvent::terminal_resized);
    {
        stx_auxlib::CapabilityValue<stx_auxlib::EventWatch> started = stx_auxlib::EventWatch::watch(watched);
        require(started.succeeded(), "watch failed");
        stx_auxlib::EventWatch& watch = *started.value();
        require(stx_auxlib::EventWatch::watch(watched).status() == stx_auxlib::CapabilityStatus::busy,
                "a second watch was accepted");
        require(std::raise(SIGINT) == 0, "raise failed");
        const stx_auxlib::EventSet first = watch.take();
        require(first.contains(stx_auxlib::ProcessEvent::interrupt), "an interrupt was not recorded");
        require(watch.take().empty(), "take did not clear");
        require(std::raise(SIGWINCH) == 0, "raise failed");
        require(watch.take().contains(stx_auxlib::ProcessEvent::terminal_resized),
                "a resize was not recorded");

        // Deferred events wait until the deferral ends.
        {
            const stx_auxlib::EventDeferral deferral(watched);
            require(std::raise(SIGINT) == 0, "raise failed");
            require(watch.take().empty(), "a deferred event arrived early");
        }
        require(watch.take().contains(stx_auxlib::ProcessEvent::interrupt),
                "a deferred event was lost");
    }
    // The watch restored the earlier handling and can be taken again.
    struct sigaction current{};
    require(::sigaction(SIGINT, nullptr, &current) == 0 && current.sa_handler == SIG_DFL,
            "the watch did not restore SIGINT");
    stx_auxlib::CapabilityValue<stx_auxlib::EventWatch> again = stx_auxlib::EventWatch::watch(watched);
    require(again.succeeded(), "a watch after the first ended failed");
}

void children_reset_signals() {
    stx_auxlib::ignore_broken_pipes();
    stx_auxlib::Child child = start_self({"sigpipe"}, stx_auxlib::ChildStreams::capture);
    std::string output{};
    require(child.read_output(64, output) == stx_auxlib::CapabilityStatus::ok, "read_output failed");
    require(output == "default\n", "a child inherited an ignored SIGPIPE");
    require(child.wait(std::chrono::seconds(30)).succeeded(), "the child did not end");
}
#endif

#if defined(_WIN32)
// This process's arguments as UTF-8, parsed from the wide command line.
std::vector<std::string> utf8_arguments() {
    std::vector<std::string> result{};
    int count = 0;
    wchar_t** const wide = CommandLineToArgvW(GetCommandLineW(), &count);
    require(wide != nullptr, "CommandLineToArgvW failed");
    for (int index = 0; index < count; ++index) {
        const int bytes = WideCharToMultiByte(CP_UTF8, 0, wide[index], -1, nullptr, 0, nullptr, nullptr);
        require(bytes > 0, "an argument is not convertible to UTF-8");
        std::string argument(static_cast<std::size_t>(bytes), '\0');
        WideCharToMultiByte(CP_UTF8, 0, wide[index], -1, argument.data(), bytes, nullptr, nullptr);
        argument.resize(static_cast<std::size_t>(bytes - 1));  // drops the converted terminator
        result.push_back(argument);
    }
    LocalFree(wide);
    return result;
}
#endif

int echo_arguments(const int argument_count, char** const arguments) {
#if defined(_WIN32)
    (void)argument_count;
    (void)arguments;
    _setmode(_fileno(stdout), _O_BINARY);
    const std::vector<std::string> words = utf8_arguments();
    for (std::size_t index = 2; index < words.size(); ++index) std::printf("%s\n", words[index].c_str());
#else
    for (int index = 2; index < argument_count; ++index) std::printf("%s\n", arguments[index]);
#endif
    return 0;
}

int child_mode(const int argument_count, char** const arguments) {
    const std::string_view mode = arguments[1];
    if (mode == "echo") return echo_arguments(argument_count, arguments);
    if (mode == "exit" && argument_count == 3) return std::atoi(arguments[2]);
    if (mode == "sleep") {
        stx_auxlib::sleep_for(std::chrono::seconds(10));
        return 0;
    }
#if !defined(_WIN32)
    if (mode == "sigpipe") {
        struct sigaction current{};
        (void)::sigaction(SIGPIPE, nullptr, &current);
        std::printf("%s\n", current.sa_handler == SIG_IGN ? "ignored" : "default");
        return 0;
    }
#endif
    return 2;
}

} // namespace

int main(const int argument_count, char** const arguments) {
    if (argument_count >= 2) return child_mode(argument_count, arguments);
    availability();
    child_requests();
    child_output_and_status();
    timeouts_and_kill();
    desktop_refusals();
    terminal_without_one();
#if !defined(_WIN32)
    events();
    children_reset_signals();
#endif
    std::puts("stx_auxlib capabilities tests passed");
    return 0;
}
