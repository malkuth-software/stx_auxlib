// The POSIX environment backend (environment_backend.hpp), over getenv,
// setenv and unsetenv. environment.cpp serializes the calls.

#include "environment_backend.hpp"

#include "../unicode.hpp"

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>

namespace stx_auxlib::environment_backend {

EnvironmentStatus read(const char* const name, const std::size_t limit, std::string& value) {
    const char* const found = std::getenv(name);
    if (found == nullptr) return EnvironmentStatus::not_set;
    const std::size_t length = std::strlen(found);
    if (length > limit) return EnvironmentStatus::too_long;
    const std::string_view raw(found, length);
    if (!unicode::valid_utf8(raw)) return EnvironmentStatus::invalid_value;
    value.assign(raw);
    return EnvironmentStatus::ok;
}

EnvironmentStatus set(const std::string& name, const std::string& value) {
    if (::setenv(name.c_str(), value.c_str(), 1) != 0) return EnvironmentStatus::host_error;
    return EnvironmentStatus::ok;
}

EnvironmentStatus remove(const std::string& name) {
    if (::unsetenv(name.c_str()) != 0) return EnvironmentStatus::host_error;
    return EnvironmentStatus::ok;
}

} // namespace stx_auxlib::environment_backend
