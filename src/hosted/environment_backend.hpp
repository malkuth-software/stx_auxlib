#pragma once

// The primitives each host environment backend supplies
// (environment_posix.cpp, environment_windows.cpp); exactly one is linked.
// environment.cpp validates names and values and serializes every call, so
// each primitive receives a portable name, runs alone, and may assume
// valid UTF-8 values.

#include "stx_auxlib/environment.hpp"

#include <cstddef>
#include <string>

namespace stx_auxlib::environment_backend {

// Replaces `value` with the variable's raw value; too_long, before copying,
// when it has more than `limit` bytes; invalid_value when it is not UTF-8.
[[nodiscard]] EnvironmentStatus read(const char* name, std::size_t limit, std::string& value);
[[nodiscard]] EnvironmentStatus set(const std::string& name, const std::string& value);
[[nodiscard]] EnvironmentStatus remove(const std::string& name);

} // namespace stx_auxlib::environment_backend
