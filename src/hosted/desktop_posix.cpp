// Desktop integration and the capability query on POSIX hosts
// (include/stx_auxlib/desktop.hpp, capabilities.hpp). Apple hosts use
// /usr/bin/open; other hosts use xdg-open, which can open a directory but
// not select a file in it.

#include "stx_auxlib/capabilities.hpp"
#include "stx_auxlib/children.hpp"
#include "stx_auxlib/desktop.hpp"
#include "stx_auxlib/paths.hpp"

#include <cerrno>
#include <climits>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

#include <unistd.h>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

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

// http, https and mailto links, or absolute paths; never anything a helper
// could read as an option, and no control characters.
bool acceptable_target(const std::string_view target) {
    for (const char character : target) {
        if (static_cast<unsigned char>(character) < 0x20 || character == 0x7f) return false;
    }
    if (starts_with_folded(target, "http://") || starts_with_folded(target, "https://") ||
        starts_with_folded(target, "mailto:"))
        return true;
    const PathParse parsed = Path::parse(target);
    const Path* const path = parsed.path();
    const bool result = path != nullptr && (*path).is_absolute() && (*path).drive() == 0;
    return result;
}

CapabilityStatus run_helper(std::string program, std::vector<std::string> arguments) {
    ChildRequest request{};
    request.program = std::move(program);
    request.arguments = std::move(arguments);
    request.streams = ChildStreams::discard;
    request.detached = true;
    const CapabilityValue<Child> started = Child::start(request);
    // A desktop without its helper program lacks the capability.
    if (started.status() == CapabilityStatus::not_found) return CapabilityStatus::unsupported;
    return started.status();
}

#if defined(__APPLE__)
constexpr const char* desktop_helper = "/usr/bin/open";
#else
constexpr const char* desktop_helper = "xdg-open";
#endif

} // namespace

bool capability_available(const Capability capability) noexcept {
    switch (capability) {
    case Capability::child_processes:
    case Capability::desktop_open:
    case Capability::file_reveal:
    case Capability::raw_terminal:
    case Capability::interrupt_events:
    case Capability::resize_events:
    case Capability::suspend_events:
        return true;
    case Capability::current_executable:
#if defined(__APPLE__) || defined(__linux__)
        return true;
#else
        return false;
#endif
    }
    return false;
}

CapabilityStatus open_with_desktop(const std::string_view target) {
    if (!acceptable_target(target)) return CapabilityStatus::invalid_request;
    const CapabilityStatus result = run_helper(desktop_helper, {std::string(target)});
    return result;
}

CapabilityStatus reveal_in_file_manager(const Path& path) {
    if (!path.is_absolute() || path.drive() != 0) return CapabilityStatus::invalid_request;
#if defined(__APPLE__)
    const CapabilityStatus result = run_helper(desktop_helper, {"-R", std::string(path.text())});
#else
    const CapabilityStatus result = run_helper(desktop_helper, {std::string(path.parent().text())});
#endif
    return result;
}

CapabilityValue<Path> current_executable() {
    std::string text{};
#if defined(__APPLE__)
    std::uint32_t size = 0;
    (void)_NSGetExecutablePath(nullptr, &size);
    std::string raw(size, '\0');
    if (_NSGetExecutablePath(raw.data(), &size) != 0)
        return CapabilityValue<Path>::failure(CapabilityStatus::io_error);
    const std::size_t terminator = raw.find('\0');
    if (terminator != std::string::npos) raw.resize(terminator);
    char resolved[PATH_MAX + 1] = {};
    if (::realpath(raw.c_str(), resolved) == nullptr)
        return CapabilityValue<Path>::failure(CapabilityStatus::io_error);
    text = resolved;
#elif defined(__linux__)
    std::string raw(256, '\0');
    for (;;) {
        const ssize_t length = ::readlink("/proc/self/exe", raw.data(), raw.size());
        if (length < 0) return CapabilityValue<Path>::failure(CapabilityStatus::io_error);
        if (static_cast<std::size_t>(length) < raw.size()) {
            raw.resize(static_cast<std::size_t>(length));
            break;
        }
        if (raw.size() >= 65536) return CapabilityValue<Path>::failure(CapabilityStatus::io_error);
        raw.resize(raw.size() * 2);
    }
    text = raw;
#else
    return CapabilityValue<Path>::failure(CapabilityStatus::unsupported);
#endif
    const PathParse parsed = Path::parse(text);
    const Path* const path = parsed.path();
    if (path == nullptr) return CapabilityValue<Path>::failure(CapabilityStatus::io_error);
    return CapabilityValue<Path>::success(*path);
}

} // namespace stx_auxlib
