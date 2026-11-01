// Desktop integration and the capability query on Windows
// (include/stx_auxlib/desktop.hpp, capabilities.hpp): ShellExecuteW to
// open, explorer.exe /select to reveal, GetModuleFileNameW for the
// executable.

#include "stx_auxlib/capabilities.hpp"
#include "stx_auxlib/children.hpp"
#include "stx_auxlib/desktop.hpp"
#include "stx_auxlib/paths.hpp"

#include "../unicode.hpp"

#include <cstddef>
#include <string>
#include <string_view>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>

namespace stx_auxlib {

namespace {

bool starts_with_folded(const std::string_view text, const std::string_view prefix) noexcept {
    if (text.size() < prefix.size()) return false;
    for (std::size_t index = 0; index < prefix.size(); ++index) {
        char character = text[index];
        if (character >= 'A' && character <= 'Z') character = static_cast<char>(character - 'A' + 'a');
        if (character != prefix[index]) return false;
    }
    return true;
}

// The Windows form of an absolute Path: backslashes, drive or root kept.
std::wstring native_wide(const std::string_view text) {
    std::u16string utf16{};
    (void)unicode::append_utf16(text, utf16);
    std::wstring result(utf16.size(), L'\0');
    for (std::size_t index = 0; index < utf16.size(); ++index) {
        const wchar_t character = static_cast<wchar_t>(utf16[index]);
        result[index] = character == L'/' ? L'\\' : character;
    }
    return result;
}

std::wstring link_wide(const std::string_view text) {
    std::u16string utf16{};
    (void)unicode::append_utf16(text, utf16);
    std::wstring result(utf16.size(), L'\0');
    for (std::size_t index = 0; index < utf16.size(); ++index)
        result[index] = static_cast<wchar_t>(utf16[index]);
    return result;
}

} // namespace

bool capability_available(const Capability capability) noexcept {
    switch (capability) {
    case Capability::child_processes:
    case Capability::desktop_open:
    case Capability::file_reveal:
    case Capability::current_executable:
    case Capability::raw_terminal:
    case Capability::interrupt_events:
        return true;
    case Capability::resize_events:
    case Capability::suspend_events:
        return false;
    }
    return false;
}

CapabilityStatus open_with_desktop(const std::string_view target) {
    for (const char character : target) {
        if (static_cast<unsigned char>(character) < 0x20 || character == 0x7f)
            return CapabilityStatus::invalid_request;
    }
    std::wstring wide{};
    if (starts_with_folded(target, "http://") || starts_with_folded(target, "https://") ||
        starts_with_folded(target, "mailto:")) {
        wide = link_wide(target);
    } else {
        const PathParse parsed = Path::parse(target);
        const Path* const path = parsed.path();
        if (path == nullptr || !(*path).is_absolute()) return CapabilityStatus::invalid_request;
        wide = native_wide((*path).text());
    }
    // ShellExecuteW reports success as a value above 32.
    const HINSTANCE result =
        ::ShellExecuteW(nullptr, L"open", wide.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(result) <= 32) return CapabilityStatus::io_error;
    return CapabilityStatus::ok;
}

CapabilityStatus reveal_in_file_manager(const Path& path) {
    if (!path.is_absolute()) return CapabilityStatus::invalid_request;
    ChildRequest request{};
    request.program = "explorer.exe";
    std::string selection = "/select,";
    for (const char character : path.text()) selection.push_back(character == '/' ? '\\' : character);
    request.arguments.push_back(selection);
    request.streams = ChildStreams::discard;
    request.detached = true;
    const CapabilityValue<Child> started = Child::start(request);
    return started.status();
}

CapabilityValue<Path> current_executable() {
    std::wstring buffer(260, L'\0');
    for (;;) {
        const DWORD length = ::GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) return CapabilityValue<Path>::failure(CapabilityStatus::io_error);
        if (length < buffer.size()) {
            buffer.resize(length);
            break;
        }
        if (buffer.size() >= 32768) return CapabilityValue<Path>::failure(CapabilityStatus::io_error);
        buffer.resize(buffer.size() * 2);
    }
    std::u16string utf16(buffer.size(), u'\0');
    for (std::size_t index = 0; index < buffer.size(); ++index) {
        const wchar_t character = buffer[index];
        utf16[index] = static_cast<char16_t>(character == L'\\' ? L'/' : character);
    }
    std::string text{};
    if (!unicode::append_utf8(utf16, text)) return CapabilityValue<Path>::failure(CapabilityStatus::io_error);
    const PathParse parsed = Path::parse(text);
    const Path* const parsed_path = parsed.path();
    if (parsed_path == nullptr) return CapabilityValue<Path>::failure(CapabilityStatus::io_error);
    return CapabilityValue<Path>::success(*parsed_path);
}

} // namespace stx_auxlib
