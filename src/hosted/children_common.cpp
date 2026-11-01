// The host-independent parts of children.hpp: ChildEnd, and the request
// validation both host backends apply.

#include "stx_auxlib/children.hpp"

#include "children_common.hpp"

#include "../unicode.hpp"

#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace stx_auxlib {

ChildEnd::ChildEnd(State state) noexcept : state_(std::move(state)) {}

ChildEnd ChildEnd::exited(const std::uint32_t status) noexcept {
    ChildEnd result(State(std::in_place_index<0>, Exited{status}));
    return result;
}

ChildEnd ChildEnd::signalled(const std::uint32_t signal) noexcept {
    ChildEnd result(State(std::in_place_index<1>, Signalled{signal}));
    return result;
}

const std::uint32_t* ChildEnd::exit_status() const noexcept {
    const Exited* const exited_state = std::get_if<Exited>(&state_);
    if (exited_state == nullptr) return nullptr;
    return &(*exited_state).status;
}

const std::uint32_t* ChildEnd::signal() const noexcept {
    const Signalled* const signalled_state = std::get_if<Signalled>(&state_);
    if (signalled_state == nullptr) return nullptr;
    return &(*signalled_state).signal;
}

namespace children_detail {

namespace {

bool valid_argument(const std::string_view text) noexcept {
    const bool result = text.find('\0') == std::string_view::npos && unicode::valid_utf8(text);
    return result;
}

} // namespace

bool valid_request(const ChildRequest& request) noexcept {
    if (request.program.empty() || !valid_argument(request.program)) return false;
    for (const std::string& argument : request.arguments) {
        if (!valid_argument(argument)) return false;
    }
    return true;
}

MonotonicClock::time_point deadline_after(const MonotonicClock::duration interval) noexcept {
    const MonotonicClock::rep now = MonotonicClock::now().time_since_epoch().count();
    const MonotonicClock::rep step = interval.count();
    const MonotonicClock::rep largest = std::numeric_limits<MonotonicClock::rep>::max();
    MonotonicClock::rep sum = now;
    if (step > 0) sum = now > largest - step ? largest : now + step;
    const MonotonicClock::time_point result{MonotonicClock::duration(sum)};
    return result;
}

} // namespace children_detail

} // namespace stx_auxlib
