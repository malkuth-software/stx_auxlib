// Child processes on Windows (include/stx_auxlib/children.hpp), over
// CreateProcessW. The command line is built with the quoting rules the C
// runtime and CommandLineToArgvW parse, so the child receives exactly the
// requested arguments. Only the handles a child should have are inherited
// (PROC_THREAD_ATTRIBUTE_HANDLE_LIST).

#include "stx_auxlib/children.hpp"

#include "children_common.hpp"

#include "../unicode.hpp"

#include "stx_auxlib/clocks.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <string>
#include <utility>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace stx_auxlib {

struct Child::State final {
    HANDLE process{nullptr};
    // The read end of the captured output pipe, or null.
    HANDLE output{nullptr};
    bool waited{false};
};

namespace {

CapabilityStatus status_from_last_error() noexcept {
    switch (::GetLastError()) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
    case ERROR_BAD_EXE_FORMAT:
        return CapabilityStatus::not_found;
    case ERROR_ACCESS_DENIED:
        return CapabilityStatus::permission_denied;
    case ERROR_NOT_ENOUGH_MEMORY:
    case ERROR_OUTOFMEMORY:
    case ERROR_NO_SYSTEM_RESOURCES:
        return CapabilityStatus::resources_exhausted;
    default:
        break;
    }
    return CapabilityStatus::io_error;
}

std::wstring wide(const std::string& utf8) {
    std::u16string utf16{};
    (void)unicode::append_utf16(utf8, utf16);
    std::wstring result(utf16.size(), L'\0');
    for (std::size_t index = 0; index < utf16.size(); ++index)
        result[index] = static_cast<wchar_t>(utf16[index]);
    return result;
}

// Appends one argument quoted for CommandLineToArgvW: wrapped in quotes when
// it is empty or holds a space, tab or quote; a quote is escaped with a
// backslash, and backslashes are doubled only where they precede a quote.
void append_quoted(std::wstring& line, const std::wstring& argument) {
    const bool needs_quotes = argument.empty() || argument.find_first_of(L" \t\"") != std::wstring::npos;
    if (!needs_quotes) {
        line.append(argument);
        return;
    }
    line.push_back(L'"');
    std::size_t backslashes = 0;
    for (const wchar_t character : argument) {
        if (character == L'\\') {
            ++backslashes;
            continue;
        }
        if (character == L'"') {
            line.append(backslashes * 2 + 1, L'\\');
        } else {
            line.append(backslashes, L'\\');
        }
        backslashes = 0;
        line.push_back(character);
    }
    line.append(backslashes * 2, L'\\');
    line.push_back(L'"');
}

// Closes a handle on every exit; null holds none.
struct Handle final {
    HANDLE value{nullptr};
    Handle() noexcept = default;
    explicit Handle(const HANDLE handle) noexcept : value(handle) {}
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    ~Handle() {
        if (value != nullptr && value != INVALID_HANDLE_VALUE) (void)::CloseHandle(value);
    }
    HANDLE release() noexcept { return std::exchange(value, nullptr); }
};

} // namespace

Child::Child() noexcept = default;

Child::Child(std::unique_ptr<State> state) noexcept : state_(std::move(state)) {}

Child::Child(Child&& other) noexcept : state_(std::move(other.state_)) {}

// Windows has no zombies: closing the process handle is all that releases
// a child; the child itself keeps running.
Child::~Child() {
    if (!state_) return;
    State& state = *state_;
    if (state.output != nullptr) (void)::CloseHandle(state.output);
    if (state.process != nullptr) (void)::CloseHandle(state.process);
}

Child& Child::operator=(Child&& other) noexcept {
    if (this == &other) return *this;
    Child released(std::move(*this));
    state_ = std::move(other.state_);
    return *this;
}

CapabilityValue<Child> Child::start(const ChildRequest& request) {
    if (!children_detail::valid_request(request))
        return CapabilityValue<Child>::failure(CapabilityStatus::invalid_request);

    SECURITY_ATTRIBUTES inheritable{};
    inheritable.nLength = sizeof inheritable;
    inheritable.bInheritHandle = TRUE;
    Handle null_input(::CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    &inheritable, OPEN_EXISTING, 0, nullptr));
    if (null_input.value == INVALID_HANDLE_VALUE) return CapabilityValue<Child>::failure(status_from_last_error());
    Handle output_reader{};
    Handle output_writer{};
    if (request.streams == ChildStreams::capture) {
        HANDLE reader = nullptr;
        HANDLE writer = nullptr;
        if (!::CreatePipe(&reader, &writer, &inheritable, 0))
            return CapabilityValue<Child>::failure(status_from_last_error());
        output_reader.value = reader;
        output_writer.value = writer;
        (void)::SetHandleInformation(reader, HANDLE_FLAG_INHERIT, 0);
    } else if (request.streams == ChildStreams::discard) {
        output_writer.value = ::CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                            &inheritable, OPEN_EXISTING, 0, nullptr);
        if (output_writer.value == INVALID_HANDLE_VALUE)
            return CapabilityValue<Child>::failure(status_from_last_error());
    }

    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof startup;
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = null_input.value;
    const HANDLE inherited_output =
        request.streams == ChildStreams::inherit ? ::GetStdHandle(STD_OUTPUT_HANDLE) : output_writer.value;
    const HANDLE inherited_error =
        request.streams == ChildStreams::inherit ? ::GetStdHandle(STD_ERROR_HANDLE) : output_writer.value;
    startup.StartupInfo.hStdOutput = inherited_output;
    startup.StartupInfo.hStdError = inherited_error;

    // Restrict inheritance to the child's own standard handles.
    std::array<HANDLE, 3> handles = {null_input.value, inherited_output, inherited_error};
    // In inherit mode only the null input is listed: the parent's own
    // standard handles are passed as they are.
    const std::size_t handle_count = request.streams == ChildStreams::inherit ? 1 : 2;
    SIZE_T list_bytes = 0;
    (void)::InitializeProcThreadAttributeList(nullptr, 1, 0, &list_bytes);
    std::vector<unsigned char> list_storage(list_bytes);
    const LPPROC_THREAD_ATTRIBUTE_LIST list =
        reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(list_storage.data());
    if (!::InitializeProcThreadAttributeList(list, 1, 0, &list_bytes))
        return CapabilityValue<Child>::failure(status_from_last_error());
    const BOOL listed = ::UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                                    handles.data(), handle_count * sizeof(HANDLE),
                                                    nullptr, nullptr);
    if (!listed) {
        ::DeleteProcThreadAttributeList(list);
        return CapabilityValue<Child>::failure(status_from_last_error());
    }
    startup.lpAttributeList = list;

    std::wstring command_line{};
    append_quoted(command_line, wide(request.program));
    for (const std::string& argument : request.arguments) {
        command_line.push_back(L' ');
        append_quoted(command_line, wide(argument));
    }
    DWORD flags = EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT;
    if (request.detached) flags |= DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP;

    PROCESS_INFORMATION information{};
    // A null application name makes CreateProcessW search PATH for the
    // first token of the command line.
    const BOOL created = ::CreateProcessW(nullptr, command_line.data(), nullptr, nullptr, TRUE, flags,
                                          nullptr, nullptr, &startup.StartupInfo, &information);
    const CapabilityStatus failure = created ? CapabilityStatus::ok : status_from_last_error();
    ::DeleteProcThreadAttributeList(list);
    if (!created) return CapabilityValue<Child>::failure(failure);
    (void)::CloseHandle(information.hThread);

    std::unique_ptr<State> state(new (std::nothrow) State());
    if (!state) {
        (void)::TerminateProcess(information.hProcess, 1);
        (void)::CloseHandle(information.hProcess);
        return CapabilityValue<Child>::failure(CapabilityStatus::resources_exhausted);
    }
    (*state).process = information.hProcess;
    (*state).output = output_reader.release();
    return CapabilityValue<Child>::success(Child(std::move(state)));
}

CapabilityValue<ChildEnd> Child::wait(const MonotonicClock::duration timeout) noexcept {
    if (!state_ || (*state_).waited) return CapabilityValue<ChildEnd>::failure(CapabilityStatus::invalid_state);
    State& state = *state_;
    const std::chrono::milliseconds milliseconds =
        std::chrono::duration_cast<std::chrono::milliseconds>(timeout);
    DWORD wait_milliseconds = 0;
    if (milliseconds.count() > 0)
        wait_milliseconds = milliseconds.count() >= static_cast<long long>(INFINITE)
                                ? INFINITE - 1
                                : static_cast<DWORD>(milliseconds.count());
    const DWORD result = ::WaitForSingleObject(state.process, wait_milliseconds);
    if (result == WAIT_TIMEOUT) return CapabilityValue<ChildEnd>::failure(CapabilityStatus::timed_out);
    if (result != WAIT_OBJECT_0) return CapabilityValue<ChildEnd>::failure(status_from_last_error());
    DWORD code = 0;
    if (!::GetExitCodeProcess(state.process, &code))
        return CapabilityValue<ChildEnd>::failure(status_from_last_error());
    state.waited = true;
    return CapabilityValue<ChildEnd>::success(ChildEnd::exited(code));
}

CapabilityStatus Child::kill() noexcept {
    if (!state_ || (*state_).waited) return CapabilityStatus::invalid_state;
    if (!::TerminateProcess((*state_).process, 1)) return status_from_last_error();
    return CapabilityStatus::ok;
}

CapabilityStatus Child::read_output(const std::size_t limit, std::string& output) {
    if (!state_ || (*state_).output == nullptr) return CapabilityStatus::invalid_state;
    State& state = *state_;
    std::array<char, 4096> chunk{};
    std::size_t received = 0;
    bool over_limit = false;
    for (;;) {
        DWORD count = 0;
        if (!::ReadFile(state.output, chunk.data(), static_cast<DWORD>(chunk.size()), &count, nullptr)) {
            if (::GetLastError() == ERROR_BROKEN_PIPE) break;
            return status_from_last_error();
        }
        if (count == 0) break;
        // `received` counts only kept bytes, so it never exceeds `limit`.
        if (!over_limit && count <= limit - received) {
            output.append(chunk.data(), count);
            received += count;
        } else {
            over_limit = true;
        }
    }
    (void)::CloseHandle(state.output);
    state.output = nullptr;
    if (over_limit) return CapabilityStatus::too_large;
    return CapabilityStatus::ok;
}

} // namespace stx_auxlib
