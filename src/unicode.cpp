// Private UTF-8 and UTF-16 helpers (unicode.hpp).

#include "unicode.hpp"

#include "checked.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

namespace stx_auxlib::unicode {

namespace {

bool is_continuation(const unsigned char byte) noexcept {
    const bool result = (byte & 0xc0U) == 0x80U;
    return result;
}

// The scalar value starting at `index` of strict UTF-8 `text` and its
// length in bytes; length 0 when the bytes there are not strict UTF-8. The
// second byte's range depends on the lead byte for E0, ED, F0 and F4; every
// other continuation byte is 80..BF.
struct Decoded final {
    char32_t value{0};
    std::size_t length{0};
};

Decoded decode(const std::string_view text, const std::size_t index) noexcept {
    const std::size_t available = text.size() - index;
    const unsigned char lead = static_cast<unsigned char>(text[index]);
    if (lead < 0x80U) return Decoded{static_cast<char32_t>(lead), 1};
    std::size_t trailing = 0;
    unsigned char second_low = 0x80U;
    unsigned char second_high = 0xbfU;
    char32_t value = 0;
    if (lead >= 0xc2U && lead <= 0xdfU) {
        trailing = 1;
        value = lead & 0x1fU;
    } else if (lead == 0xe0U) {
        trailing = 2;
        second_low = 0xa0U;
        value = lead & 0x0fU;
    } else if ((lead >= 0xe1U && lead <= 0xecU) || lead == 0xeeU || lead == 0xefU) {
        trailing = 2;
        value = lead & 0x0fU;
    } else if (lead == 0xedU) {
        trailing = 2;
        second_high = 0x9fU;
        value = lead & 0x0fU;
    } else if (lead == 0xf0U) {
        trailing = 3;
        second_low = 0x90U;
        value = lead & 0x07U;
    } else if (lead >= 0xf1U && lead <= 0xf3U) {
        trailing = 3;
        value = lead & 0x07U;
    } else if (lead == 0xf4U) {
        trailing = 3;
        second_high = 0x8fU;
        value = lead & 0x07U;
    } else {
        return Decoded{};
    }
    if (available <= trailing) return Decoded{};
    const unsigned char second = static_cast<unsigned char>(text[index + 1]);
    if (second < second_low || second > second_high) return Decoded{};
    for (std::size_t offset = 1; offset <= trailing; ++offset) {
        const unsigned char byte = static_cast<unsigned char>(text[index + offset]);
        if (!is_continuation(byte)) return Decoded{};
        value = (value << 6) | (byte & 0x3fU);
    }
    return Decoded{value, trailing + 1};
}

} // namespace

// ASCII runs, the common case, are checked eight bytes at a time: a word
// with no high bit set holds eight one-byte scalar values.
bool valid_utf8(const std::string_view text) noexcept {
    constexpr std::uint64_t high_bits = 0x8080808080808080ULL;
    std::size_t index = 0;
    while (index < text.size()) {
        if (text.size() - index >= 8) {
            std::uint64_t word = 0;
            std::memcpy(&word, text.data() + index, 8);
            if ((word & high_bits) == 0) {
                index += 8;
                continue;
            }
        }
        if (static_cast<unsigned char>(text[index]) < 0x80U) {
            ++index;
            continue;
        }
        const Decoded decoded = decode(text, index);
        if (decoded.length == 0) return false;
        index += decoded.length;
    }
    return true;
}

bool append_utf16(const std::string_view utf8, std::u16string& output) {
    if (!valid_utf8(utf8)) return false;
    output.reserve(checked::add_saturating(output.size(), utf8.size()));
    std::size_t index = 0;
    while (index < utf8.size()) {
        const Decoded decoded = decode(utf8, index);
        index += decoded.length;
        if (decoded.value < 0x10000U) {
            output.push_back(static_cast<char16_t>(decoded.value));
        } else {
            const char32_t offset = decoded.value - 0x10000U;
            output.push_back(static_cast<char16_t>(0xd800U + (offset >> 10)));
            output.push_back(static_cast<char16_t>(0xdc00U + (offset & 0x3ffU)));
        }
    }
    return true;
}

bool append_utf8(const std::u16string_view utf16, std::string& output) {
    std::string encoded{};
    encoded.reserve(checked::multiply_saturating(utf16.size(), 3));
    std::size_t index = 0;
    while (index < utf16.size()) {
        char32_t value = utf16[index];
        ++index;
        if (value >= 0xd800U && value <= 0xdbffU) {
            if (index == utf16.size()) return false;
            const char32_t low = utf16[index];
            if (low < 0xdc00U || low > 0xdfffU) return false;
            ++index;
            value = 0x10000U + ((value - 0xd800U) << 10) + (low - 0xdc00U);
        } else if (value >= 0xdc00U && value <= 0xdfffU) {
            return false;
        }
        if (value < 0x80U) {
            encoded.push_back(static_cast<char>(value));
        } else if (value < 0x800U) {
            encoded.push_back(static_cast<char>(0xc0U | (value >> 6)));
            encoded.push_back(static_cast<char>(0x80U | (value & 0x3fU)));
        } else if (value < 0x10000U) {
            encoded.push_back(static_cast<char>(0xe0U | (value >> 12)));
            encoded.push_back(static_cast<char>(0x80U | ((value >> 6) & 0x3fU)));
            encoded.push_back(static_cast<char>(0x80U | (value & 0x3fU)));
        } else {
            encoded.push_back(static_cast<char>(0xf0U | (value >> 18)));
            encoded.push_back(static_cast<char>(0x80U | ((value >> 12) & 0x3fU)));
            encoded.push_back(static_cast<char>(0x80U | ((value >> 6) & 0x3fU)));
            encoded.push_back(static_cast<char>(0x80U | (value & 0x3fU)));
        }
    }
    output.append(encoded);
    return true;
}

} // namespace stx_auxlib::unicode
