#pragma once

// Private UTF-8 and UTF-16 helpers shared by the library's sources (paths,
// environment, console). Pure computation.

#include <cstddef>
#include <string>
#include <string_view>

namespace stx_auxlib::unicode {

// Strict UTF-8 (RFC 3629): no overlong forms, no surrogates, nothing above
// U+10FFFF.
[[nodiscard]] bool valid_utf8(std::string_view text) noexcept;

// Appends `utf8` as UTF-16; false, with `output` unchanged, when `utf8` is
// not strict UTF-8.
[[nodiscard]] bool append_utf16(std::string_view utf8, std::u16string& output);

// Appends `utf16` as UTF-8; false, with `output` unchanged, on an unpaired
// surrogate.
[[nodiscard]] bool append_utf8(std::u16string_view utf16, std::string& output);

} // namespace stx_auxlib::unicode
