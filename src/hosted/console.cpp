// The console (include/stx_auxlib/console.hpp): validation, serialization
// and line assembly over the host backend (console_backend.hpp).

#include "stx_auxlib/console.hpp"

#include "console_backend.hpp"

#include "../unicode.hpp"

#include "stx_auxlib/threads.hpp"

#include <array>
#include <cstddef>
#include <cstdio>
#include <span>
#include <string>
#include <string_view>

namespace stx_auxlib {

namespace {

// Process-lifetime state, created on first use and never destroyed, so that
// it is ready during other translation units' static initialization and
// still valid in exit tasks and static destructors.
Mutex& output_lock() noexcept {
    static Mutex* const lock = new Mutex();
    return *lock;
}

// Standard input's reader: bytes read past the last returned line.
struct InputState final {
    Mutex lock{};
    std::string pending{};
    bool ended{false};
};

InputState& input_state() noexcept {
    static InputState* const state = new InputState();
    return *state;
}

std::FILE* c_stream(const ConsoleStream stream) noexcept {
    if (stream == ConsoleStream::error) return stderr;
    return stdout;
}

constexpr std::size_t read_chunk_bytes = 4096;

} // namespace

ConsoleStatus write_console(const ConsoleStream stream, const std::string_view text) noexcept {
    if (!unicode::valid_utf8(text)) return ConsoleStatus::invalid_text;
    const ScopedLock lock(output_lock());
    (void)std::fflush(c_stream(stream));
    const ConsoleStatus result = console_backend::write(stream, text);
    return result;
}

ConsoleStatus write_console_line(const ConsoleStream stream, const std::string_view text) noexcept {
    if (!unicode::valid_utf8(text)) return ConsoleStatus::invalid_text;
    const ScopedLock lock(output_lock());
    (void)std::fflush(c_stream(stream));
    const ConsoleStatus result = console_backend::write_line(stream, text);
    return result;
}

bool console_is_terminal(const ConsoleStream stream) noexcept {
    const bool result = console_backend::is_terminal(stream);
    return result;
}

// Takes the next line from `pending`, reading more as needed. A line over
// `limit` is discarded through its ending, so the next call starts at the
// following line.
ConsoleStatus read_console_line(const std::size_t limit, std::string& line) {
    line.clear();
    InputState& state = input_state();
    const ScopedLock lock(state.lock);
    std::array<char, read_chunk_bytes> chunk{};
    bool over_limit = false;
    for (;;) {
        const std::size_t ending = state.pending.find('\n');
        if (ending != std::string::npos) {
            std::size_t length = ending;
            if (length > 0 && state.pending[length - 1] == '\r') --length;
            if (length > limit) over_limit = true;
            if (!over_limit) line.assign(state.pending, 0, length);
            state.pending.erase(0, ending + 1);
            break;
        }
        if (state.ended) {
            if (state.pending.empty() && !over_limit) return ConsoleStatus::end_of_input;
            std::size_t length = state.pending.size();
            if (length > 0 && state.pending[length - 1] == '\r') --length;
            if (length > limit) over_limit = true;
            if (!over_limit) line.assign(state.pending, 0, length);
            state.pending.clear();
            break;
        }
        // Discard what is already known to be too long (more than `limit`
        // bytes besides a possible '\r'), keeping memory bounded by the
        // limit while the rest of the line arrives.
        if (state.pending.size() > limit && state.pending.size() - limit > 1) {
            over_limit = true;
            state.pending.clear();
        }
        std::size_t transferred = 0;
        const ConsoleStatus status = console_backend::read(chunk, transferred);
        if (status != ConsoleStatus::ok) return status;
        if (transferred == 0) {
            state.ended = true;
            continue;
        }
        state.pending.append(chunk.data(), transferred);
    }
    if (over_limit) {
        line.clear();
        return ConsoleStatus::too_long;
    }
    if (!unicode::valid_utf8(line)) {
        line.clear();
        return ConsoleStatus::invalid_text;
    }
    return ConsoleStatus::ok;
}

} // namespace stx_auxlib
