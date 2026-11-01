#pragma once

// Desktop services that not every platform has: child processes, opening a
// link or revealing a file with the desktop, a raw terminal, and process
// events (interrupts, resizes). Each is a named capability. A program asks
// capability_available() before relying on one, and every operation that a
// host lacks reports CapabilityStatus::unsupported instead of failing in a
// host-specific way. Under UEFI, for example, none of them exist.
//
// The operations are in children.hpp, desktop.hpp, terminal.hpp and
// events.hpp; this header holds what they share.

#include <cstdint>
#include <utility>
#include <variant>

namespace stx_auxlib {

enum class Capability : std::uint8_t {
    child_processes = 0,
    // Opening a link or document with the desktop's default handler.
    desktop_open = 1,
    // Showing a file selected in the desktop's file manager.
    file_reveal = 2,
    // The path of the running executable.
    current_executable = 3,
    raw_terminal = 4,
    // Interrupt (Ctrl+C), terminate and hangup notifications.
    interrupt_events = 5,
    // Terminal resize notifications.
    resize_events = 6,
    // Terminal suspend (Ctrl+Z) notifications.
    suspend_events = 7,
};

// Whether this host provides `capability` at all. A provided capability can
// still fail for a particular request (a missing program, no terminal).
[[nodiscard]] bool capability_available(Capability capability) noexcept;

enum class CapabilityStatus : std::uint8_t {
    ok = 0,
    // The host does not provide this capability.
    unsupported = 1,
    not_found = 2,
    permission_denied = 3,
    // The request itself is refused (an empty program, a target that could
    // be taken as an option).
    invalid_request = 4,
    resources_exhausted = 5,
    timed_out = 6,
    // The capability is already in use (one event watch at a time).
    busy = 7,
    // Standard input or output is not an interactive terminal.
    not_a_terminal = 8,
    // Output exceeded the caller's limit.
    too_large = 9,
    // The operation does not apply in the object's state (a child already
    // waited for).
    invalid_state = 10,
    io_error = 11,
};

// The result of a capability operation that yields a value: the value, or a
// status other than ok. Never both.
template <typename Value>
class CapabilityValue final {
public:
    [[nodiscard]] static CapabilityValue success(Value value) {
        CapabilityValue result(State(std::in_place_index<0>, std::move(value)));
        return result;
    }
    // `status` must not be CapabilityStatus::ok.
    [[nodiscard]] static CapabilityValue failure(const CapabilityStatus status) noexcept {
        CapabilityValue result(State(std::in_place_index<1>, status));
        return result;
    }
    [[nodiscard]] bool succeeded() const noexcept {
        const bool result = std::holds_alternative<Value>(state_);
        return result;
    }
    // CapabilityStatus::ok when the operation succeeded.
    [[nodiscard]] CapabilityStatus status() const noexcept {
        const CapabilityStatus* const failed = std::get_if<CapabilityStatus>(&state_);
        if (failed == nullptr) return CapabilityStatus::ok;
        return *failed;
    }
    // The value; null on failure.
    [[nodiscard]] const Value* value() const noexcept {
        const Value* const result = std::get_if<Value>(&state_);
        return result;
    }
    [[nodiscard]] Value* value() noexcept {
        Value* const result = std::get_if<Value>(&state_);
        return result;
    }

private:
    using State = std::variant<Value, CapabilityStatus>;
    explicit CapabilityValue(State state) noexcept : state_(std::move(state)) {}
    State state_;
};

} // namespace stx_auxlib
