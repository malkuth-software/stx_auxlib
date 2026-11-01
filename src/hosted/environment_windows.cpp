// The Windows environment backend (environment_backend.hpp), over the wide
// Win32 environment block, which child processes inherit. The C runtime's
// getenv reads a separate ANSI copy and is not used. environment.cpp
// serializes the calls.

#include "environment_backend.hpp"

#include "../unicode.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace stx_auxlib::environment_backend {

namespace {

// UTF-16 in a wchar_t string, which Win32 takes; wchar_t is 16 bits here.
std::wstring wide(const std::string& utf8) {
    std::u16string utf16{};
    (void)unicode::append_utf16(utf8, utf16);
    std::wstring result(utf16.size(), L'\0');
    for (std::size_t index = 0; index < utf16.size(); ++index)
        result[index] = static_cast<wchar_t>(utf16[index]);
    return result;
}

} // namespace

EnvironmentStatus read(const char* const name, const std::size_t limit, std::string& value) {
    const std::wstring wide_name = wide(std::string(name));
    std::wstring buffer(256, L'\0');
    for (;;) {
        ::SetLastError(ERROR_SUCCESS);
        const DWORD length = ::GetEnvironmentVariableW(wide_name.c_str(), buffer.data(),
                                                       static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            // Zero is also the length of a set but empty value.
            if (::GetLastError() == ERROR_ENVVAR_NOT_FOUND) return EnvironmentStatus::not_set;
            buffer.clear();
            break;
        }
        if (length < buffer.size()) {
            buffer.resize(length);
            break;
        }
        // Too small: `length` is the size needed, terminator included.
        if (length > 32768) return EnvironmentStatus::too_long;
        buffer.assign(length, L'\0');
    }
    std::u16string utf16(buffer.size(), u'\0');
    for (std::size_t index = 0; index < buffer.size(); ++index)
        utf16[index] = static_cast<char16_t>(buffer[index]);
    std::string converted{};
    if (!unicode::append_utf8(utf16, converted)) return EnvironmentStatus::invalid_value;
    if (converted.size() > limit) return EnvironmentStatus::too_long;
    value = std::move(converted);
    return EnvironmentStatus::ok;
}

EnvironmentStatus set(const std::string& name, const std::string& value) {
    const std::wstring wide_name = wide(name);
    const std::wstring wide_value = wide(value);
    if (!::SetEnvironmentVariableW(wide_name.c_str(), wide_value.c_str()))
        return EnvironmentStatus::host_error;
    return EnvironmentStatus::ok;
}

EnvironmentStatus remove(const std::string& name) {
    const std::wstring wide_name = wide(name);
    if (!::SetEnvironmentVariableW(wide_name.c_str(), nullptr) &&
        ::GetLastError() != ERROR_ENVVAR_NOT_FOUND)
        return EnvironmentStatus::host_error;
    return EnvironmentStatus::ok;
}

} // namespace stx_auxlib::environment_backend
