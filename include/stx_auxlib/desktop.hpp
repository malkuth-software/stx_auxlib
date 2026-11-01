#pragma once

// Desktop integration (Capability::desktop_open, file_reveal and
// current_executable).
//
// Sealing: open_with_desktop accepts only http, https and mailto links and
// absolute paths, so input can never become a command-line option or an
// arbitrary protocol handler. The desktop's helper (open, xdg-open,
// ShellExecuteW, explorer) runs detached, with no shell and its output
// discarded; neither function waits for the desktop to act.

#include "stx_auxlib/capabilities.hpp"
#include "stx_auxlib/paths.hpp"

#include <string_view>

namespace stx_auxlib {

// Opens a link or an absolute path with the desktop's default handler.
// invalid_request for any other target.
[[nodiscard]] CapabilityStatus open_with_desktop(std::string_view target);

// Shows `path` selected in the desktop's file manager (on hosts that cannot
// select it, its directory opened). `path` must be absolute.
[[nodiscard]] CapabilityStatus reveal_in_file_manager(const Path& path);

// The running program's executable, as an absolute Path.
[[nodiscard]] CapabilityValue<Path> current_executable();

} // namespace stx_auxlib
