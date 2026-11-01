#pragma once

// Private helpers shared by the child-process backends
// (children_posix.cpp, children_windows.cpp) and desktop.cpp files.

#include "stx_auxlib/children.hpp"
#include "stx_auxlib/clocks.hpp"

namespace stx_auxlib::children_detail {

// A non-empty program, and a program and arguments that are UTF-8 without
// NUL.
[[nodiscard]] bool valid_request(const ChildRequest& request) noexcept;

// now + interval, saturating; a non-positive interval is now.
[[nodiscard]] MonotonicClock::time_point deadline_after(MonotonicClock::duration interval) noexcept;

} // namespace stx_auxlib::children_detail
