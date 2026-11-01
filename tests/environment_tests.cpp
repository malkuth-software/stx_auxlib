// Tests of stx_auxlib's environment (include/stx_auxlib/environment.hpp).

#include "stx_auxlib/environment.hpp"
#include "stx_auxlib/threads.hpp"

#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

namespace {

void require(const bool condition, const char* const message) {
    if (condition) return;
    std::fprintf(stderr, "stx_auxlib environment: %s\n", message);
    std::abort();
}

constexpr std::string_view variable = "STX_AUXLIB_TEST_VARIABLE";

void names() {
    require(stx_auxlib::is_portable_variable_name("HOME"), "HOME refused");
    require(stx_auxlib::is_portable_variable_name("_X1"), "_X1 refused");
    require(!stx_auxlib::is_portable_variable_name(""), "empty name accepted");
    require(!stx_auxlib::is_portable_variable_name("1A"), "leading digit accepted");
    require(!stx_auxlib::is_portable_variable_name("A-B"), "'-' accepted");
    require(!stx_auxlib::is_portable_variable_name("A=B"), "'=' accepted");
    require(!stx_auxlib::is_portable_variable_name("\xc3\xa9"), "non-ASCII accepted");
    std::string value = "stale";
    require(stx_auxlib::read_variable("A=B", 64, value) == stx_auxlib::EnvironmentStatus::invalid_name,
            "an invalid name was read");
    require(value.empty(), "a refused read left a value");
    require(stx_auxlib::set_variable("A B", "x") == stx_auxlib::EnvironmentStatus::invalid_name,
            "an invalid name was set");
}

void set_read_remove() {
    std::string value{};
    require(stx_auxlib::remove_variable(variable) == stx_auxlib::EnvironmentStatus::ok,
            "removing an unset variable failed");
    require(stx_auxlib::read_variable(variable, 64, value) == stx_auxlib::EnvironmentStatus::not_set,
            "an unset variable was read");
    const std::string_view text = "caf\xc3\xa9 \xf0\x9f\x98\x80 /usr/local/bin";
    require(stx_auxlib::set_variable(variable, text) == stx_auxlib::EnvironmentStatus::ok,
            "set failed");
    require(stx_auxlib::read_variable(variable, 64, value) == stx_auxlib::EnvironmentStatus::ok &&
                value == text,
            "a UTF-8 value did not round-trip");
    require(stx_auxlib::read_variable(variable, text.size() - 1, value) ==
                    stx_auxlib::EnvironmentStatus::too_long &&
                value.empty(),
            "a value over the limit was read");
    require(stx_auxlib::set_variable(variable, "") == stx_auxlib::EnvironmentStatus::ok,
            "an empty value was refused");
    require(stx_auxlib::read_variable(variable, 64, value) == stx_auxlib::EnvironmentStatus::ok &&
                value.empty(),
            "an empty value did not read back as set and empty");
    require(stx_auxlib::set_variable(variable, std::string_view("a\0b", 3)) ==
                stx_auxlib::EnvironmentStatus::invalid_value,
            "a NUL was accepted");
    require(stx_auxlib::set_variable(variable, "\xff") ==
                stx_auxlib::EnvironmentStatus::invalid_value,
            "invalid UTF-8 was accepted");
    require(stx_auxlib::remove_variable(variable) == stx_auxlib::EnvironmentStatus::ok,
            "remove failed");
    require(stx_auxlib::read_variable(variable, 64, value) == stx_auxlib::EnvironmentStatus::not_set,
            "a removed variable was read");
    const std::string oversized(stx_auxlib::maximum_environment_bytes + 1, 'x');
    require(stx_auxlib::set_variable(variable, oversized) == stx_auxlib::EnvironmentStatus::too_long,
            "an oversized value was accepted");
}

// Concurrent setters and readers: serialized, so no read sees a torn value.
struct Churn final {
    std::atomic<int> torn_reads{0};
};

void churn(const stx_auxlib::StopSignal&, void* const address) noexcept {
    Churn& churn_state = *static_cast<Churn*>(address);
    std::string value{};
    for (int round = 0; round < 2000; ++round) {
        const bool long_value = round % 2 == 0;
        (void)stx_auxlib::set_variable(variable, long_value ? "aaaaaaaaaaaaaaaa" : "bb");
        if (stx_auxlib::read_variable(variable, 64, value) == stx_auxlib::EnvironmentStatus::ok &&
            value != "aaaaaaaaaaaaaaaa" && value != "bb")
            churn_state.torn_reads.fetch_add(1, std::memory_order_relaxed);
    }
}

void concurrent_use() {
    Churn churn_state{};
    {
        std::vector<stx_auxlib::Thread> threads{};
        for (int index = 0; index < 4; ++index) {
            stx_auxlib::ThreadStart start = stx_auxlib::Thread::start(churn, &churn_state);
            require(start.succeeded(), "a thread failed to start");
            threads.push_back(start.take_thread());
        }
    }
    require(churn_state.torn_reads.load() == 0, "a concurrent read saw a torn value");
    (void)stx_auxlib::remove_variable(variable);
}

} // namespace

int main() {
    names();
    set_read_remove();
    concurrent_use();
    std::puts("stx_auxlib environment tests passed");
    return 0;
}
