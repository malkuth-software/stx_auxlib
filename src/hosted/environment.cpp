// The environment (include/stx_auxlib/environment.hpp): validation and
// serialization over the host backend (environment_backend.hpp).

#include "stx_auxlib/environment.hpp"

#include "environment_backend.hpp"
#include "environment_lock.hpp"

#include "../checked.hpp"
#include "../unicode.hpp"

#include "stx_auxlib/threads.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <cstddef>
#include <string>
#include <string_view>

namespace stx_auxlib {

// Every call holds this lock. Created on first use and never destroyed, so
// that it is ready during other translation units' static initialization
// and still valid in exit tasks and static destructors.
environment_detail::SpinLock& environment_detail::environment_lock() noexcept {
    static SpinLock* const lock = new SpinLock();
    return *lock;
}

std::uint64_t& environment_detail::environment_generation() noexcept {
    static std::uint64_t* const generation = new std::uint64_t(0);
    return *generation;
}

namespace {

using environment_detail::environment_lock;

// Records a change for readers that cache what the host derived from the
// environment.
STX_AUXLIB_MODULAR void record_change(const EnvironmentStatus status) noexcept {
    if (status == EnvironmentStatus::ok) ++environment_detail::environment_generation();
}

bool is_letter_or_underscore(const char character) noexcept {
    const bool result = (character >= 'a' && character <= 'z') ||
                        (character >= 'A' && character <= 'Z') || character == '_';
    return result;
}

bool is_digit(const char character) noexcept {
    const bool result = character >= '0' && character <= '9';
    return result;
}

} // namespace

bool is_portable_variable_name(const std::string_view name) noexcept {
    if (name.empty() || !is_letter_or_underscore(name[0])) return false;
    for (const char character : name) {
        if (!is_letter_or_underscore(character) && !is_digit(character)) return false;
    }
    return true;
}

EnvironmentStatus read_variable(const std::string_view name, const std::size_t limit,
                                std::string& value) {
    value.clear();
    if (!is_portable_variable_name(name)) return EnvironmentStatus::invalid_name;
    // The name with its terminator: on the stack when short, which almost
    // every name is.
    std::array<char, 64> short_name{};
    std::string long_name{};
    const char* host_name = nullptr;
    if (name.size() < short_name.size()) {
        std::memcpy(short_name.data(), name.data(), name.size());
        host_name = short_name.data();
    } else {
        long_name.assign(name);
        host_name = long_name.c_str();
    }
    const std::size_t bounded = std::min(limit, maximum_environment_bytes);
    const environment_detail::SpinGuard lock(environment_lock());
    const EnvironmentStatus status = environment_backend::read(host_name, bounded, value);
    if (status != EnvironmentStatus::ok) value.clear();
    return status;
}

EnvironmentStatus set_variable(const std::string_view name, const std::string_view value) {
    if (!is_portable_variable_name(name)) return EnvironmentStatus::invalid_name;
    if (value.size() > maximum_environment_bytes) return EnvironmentStatus::too_long;
    if (value.find('\0') != std::string_view::npos || !unicode::valid_utf8(value))
        return EnvironmentStatus::invalid_value;
    const std::string host_name(name);
    const std::string host_value(value);
    const environment_detail::SpinGuard lock(environment_lock());
    const EnvironmentStatus status = environment_backend::set(host_name, host_value);
    record_change(status);
    return status;
}

EnvironmentStatus remove_variable(const std::string_view name) {
    if (!is_portable_variable_name(name)) return EnvironmentStatus::invalid_name;
    const std::string host_name(name);
    const environment_detail::SpinGuard lock(environment_lock());
    const EnvironmentStatus status = environment_backend::remove(host_name);
    record_change(status);
    return status;
}

} // namespace stx_auxlib
