// stx_auxlib::text (include/stx_auxlib/text.hpp), over std::to_chars and
// std::from_chars. Pure computation, shared by every implementation.
//
// Where the standard library's own behaviour differs between hosts, this
// file decides instead: from_chars rejects a leading '+' (accepted here
// explicitly), and on out_of_range some libraries write infinity or zero
// into the result while the standard leaves it unchanged (here the result
// is always the out_of_range failure, and the library's value is unused).

#include "stx_auxlib/text.hpp"

#include "checked.hpp"

#include "stx_auxlib/process.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>

namespace stx_auxlib::text {

namespace {

// Scratch sizes. An integer needs at most 64 binary digits; a shortest real
// at most 24 characters ("-2.2250738585072014e-308"); a fixed or scientific
// real with small precision fits the short buffer, and only a long one
// needs the long buffer: a sign, up to 309 integer digits, the point,
// maximum_precision fraction digits, and an exponent.
constexpr std::size_t short_scratch_bytes = 128;
constexpr std::size_t long_scratch_bytes = 1 + 309 + 1 + maximum_precision + 16;
using ShortScratch = std::array<char, short_scratch_bytes>;
using LongScratch = std::array<char, long_scratch_bytes>;

// The longest text a finite real can produce in a format, for choosing its
// scratch. A fixed form's integer digits are at most floor(e * log10 2) + 2
// for a binary exponent e >= 0 (log10 2 < 0.30103), and one below 1.
std::size_t real_length_bound(const Notation notation, const std::size_t precision,
                              const int binary_exponent) noexcept {
    if (notation == Notation::shortest) return 32;
    if (notation == Notation::scientific) return 1 + 1 + 1 + precision + 6;
    std::size_t integer_digits = 1;
    if (binary_exponent > 0)
        integer_digits = static_cast<std::size_t>(binary_exponent) * 30103 / 100000 + 2;
    const std::size_t result = 1 + integer_digits + 1 + precision;
    return result;
}

bool is_space(const char character) noexcept {
    const bool result = character == ' ' || character == '\t' || character == '\n' ||
                        character == '\r' || character == '\v' || character == '\f';
    return result;
}

std::size_t scalar_values(const std::string_view text) noexcept {
    std::size_t count = 0;
    for (const char character : text) {
        const unsigned char byte = static_cast<unsigned char>(character);
        if ((byte & 0xc0U) != 0x80U) ++count;
    }
    return count;
}

void append_fill(std::string& output, const char fill, const std::size_t count) {
    output.append(count, fill);
}

// The padding a field adds around an item of `length` scalar values.
std::size_t padding_for(const Field& field, const std::size_t length) noexcept {
    const std::size_t width = std::min(field.width, maximum_width);
    if (length >= width) return 0;
    const std::size_t result = width - length;
    return result;
}

// Appends `sign`, `zeros` zero digits and `digits`, padded to the field.
void append_padded(std::string& output, const std::string_view sign, const std::size_t zeros,
                   const std::string_view digits, const Field& field) {
    const std::size_t length = sign.size() + zeros + digits.size();
    const std::size_t padding = padding_for(field, length);
    output.reserve(checked::add_saturating(output.size(), checked::add_saturating(length, padding)));
    if (field.alignment == Alignment::right) append_fill(output, field.fill, padding);
    output.append(sign);
    output.append(zeros, '0');
    output.append(digits);
    if (field.alignment == Alignment::left) append_fill(output, field.fill, padding);
}

void raise_letters(char* const first, char* const last) noexcept {
    for (char* character = first; character != last; ++character) {
        if (*character >= 'a' && *character <= 'z')
            *character = static_cast<char>(*character - 'a' + 'A');
    }
}

void append_magnitude(std::string& output, const std::uint64_t magnitude, const bool negative,
                      const IntegerFormat& format) {
    ShortScratch scratch{};
    char* const first = scratch.data();
    const std::to_chars_result written =
        std::to_chars(first, first + scratch.size(), magnitude, static_cast<int>(format.base));
    // 64 binary digits always fit the 128-byte scratch.
    if (written.ec != std::errc()) fail("an integer did not fit its scratch buffer");
    if (format.letters == Letters::upper) raise_letters(first, written.ptr);
    const std::string_view digits(first, static_cast<std::size_t>(written.ptr - first));
    std::string_view sign{};
    if (negative) {
        sign = "-";
    } else if (format.sign == Sign::always) {
        sign = "+";
    }
    const std::size_t minimum = std::min(format.minimum_digits, maximum_width);
    const std::size_t zeros = minimum > digits.size() ? minimum - digits.size() : 0;
    append_padded(output, sign, zeros, digits, format.field);
}

template <typename Real>
void append_floating(std::string& output, const Real value, const RealFormat& format) {
    std::string_view sign{};
    const bool negative = std::signbit(value);
    if (std::isnan(value)) {
        append_padded(output, sign, 0, "nan", format.field);
        return;
    }
    if (!negative && format.sign == Sign::always) sign = "+";
    if (std::isinf(value)) {
        if (negative) sign = "-";
        append_padded(output, sign, 0, "inf", format.field);
        return;
    }
    const std::size_t bounded_precision = std::min(format.precision, maximum_precision);
    const int precision = static_cast<int>(bounded_precision);
    // The long buffer is zero-filled only when a long result needs it.
    ShortScratch short_scratch{};
    std::unique_ptr<LongScratch> long_scratch{};
    char* first = short_scratch.data();
    char* last = first + short_scratch.size();
    const int binary_exponent = value == 0 ? 0 : std::ilogb(value);
    if (real_length_bound(format.notation, bounded_precision, binary_exponent) >
        short_scratch_bytes) {
        long_scratch = std::make_unique<LongScratch>();
        first = (*long_scratch).data();
        last = first + (*long_scratch).size();
    }
    std::to_chars_result written{};
    switch (format.notation) {
    case Notation::fixed:
        written = std::to_chars(first, last, value, std::chars_format::fixed, precision);
        break;
    case Notation::scientific:
        written = std::to_chars(first, last, value, std::chars_format::scientific, precision);
        break;
    case Notation::shortest:
    default:
        written = std::to_chars(first, last, value);
        break;
    }
    // real_length_bound sized the scratch for the result; a failure here is
    // a defect in that bound, never output to emit.
    if (written.ec != std::errc()) fail("a real did not fit its scratch buffer");
    // to_chars writes '-' itself for negative values, -0 included.
    const std::string_view digits(first, static_cast<std::size_t>(written.ptr - first));
    append_padded(output, sign, 0, digits, format.field);
}

// A number at the start of `text`: its value or failure, and the bytes it
// spans. `consumed` is 0 on failure.
template <typename Value>
struct Prefix final {
    Parsed<Value> parsed;
    std::size_t consumed{0};
};

// Accepts one leading '+' (not followed by another sign), which from_chars
// refuses; '-' is left for from_chars.
std::size_t plus_length(const std::string_view text) noexcept {
    if (text.size() >= 2 && text[0] == '+' && text[1] != '-' && text[1] != '+') return 1;
    return 0;
}

ParseFailure failure_from(const std::errc error) noexcept {
    if (error == std::errc::result_out_of_range) return ParseFailure::out_of_range;
    return ParseFailure::invalid;
}

template <typename Integer>
Prefix<Integer> integer_prefix(const std::string_view text, const Base base) noexcept {
    if (text.empty()) return Prefix<Integer>{Parsed<Integer>::failure(ParseFailure::empty), 0};
    const std::size_t skipped = plus_length(text);
    const char* const first = text.data() + skipped;
    const char* const last = text.data() + text.size();
    Integer value = 0;
    const std::from_chars_result read = std::from_chars(first, last, value, static_cast<int>(base));
    if (read.ec != std::errc())
        return Prefix<Integer>{Parsed<Integer>::failure(failure_from(read.ec)), 0};
    const std::size_t consumed = static_cast<std::size_t>(read.ptr - text.data());
    return Prefix<Integer>{Parsed<Integer>::success(value), consumed};
}

template <typename Real>
Prefix<Real> real_prefix(const std::string_view text) noexcept {
    if (text.empty()) return Prefix<Real>{Parsed<Real>::failure(ParseFailure::empty), 0};
    const std::size_t skipped = plus_length(text);
    const char* const first = text.data() + skipped;
    const char* const last = text.data() + text.size();
    Real value = 0;
    const std::from_chars_result read =
        std::from_chars(first, last, value, std::chars_format::general);
    if (read.ec != std::errc())
        return Prefix<Real>{Parsed<Real>::failure(failure_from(read.ec)), 0};
    const std::size_t consumed = static_cast<std::size_t>(read.ptr - text.data());
    return Prefix<Real>{Parsed<Real>::success(value), consumed};
}

template <typename Value>
Parsed<Value> whole(const Prefix<Value>& prefix, const std::string_view text) noexcept {
    if (!prefix.parsed.succeeded()) return prefix.parsed;
    if (prefix.consumed != text.size()) return Parsed<Value>::failure(ParseFailure::trailing_text);
    return prefix.parsed;
}

} // namespace

void append_signed(std::string& output, const std::int64_t value, const IntegerFormat& format) {
    const bool negative = value < 0;
    // |value| = -(value + 1) + 1, so INT64_MIN has a magnitude without any
    // overflow or wrap.
    const std::uint64_t magnitude = negative ? static_cast<std::uint64_t>(-(value + 1)) + 1
                                             : static_cast<std::uint64_t>(value);
    append_magnitude(output, magnitude, negative, format);
}

void append_unsigned(std::string& output, const std::uint64_t value, const IntegerFormat& format) {
    append_magnitude(output, value, false, format);
}

void append_real(std::string& output, const double value, const RealFormat& format) {
    append_floating(output, value, format);
}

void append_real(std::string& output, const float value, const RealFormat& format) {
    append_floating(output, value, format);
}

void append_text(std::string& output, const std::string_view value, const Field& field) {
    const std::size_t padding = padding_for(field, scalar_values(value));
    output.reserve(checked::add_saturating(output.size(), checked::add_saturating(value.size(), padding)));
    if (field.alignment == Alignment::right) append_fill(output, field.fill, padding);
    output.append(value);
    if (field.alignment == Alignment::left) append_fill(output, field.fill, padding);
}

std::string to_text(const double value) {
    std::string result{};
    append_real(result, value);
    return result;
}

std::string to_text(const float value) {
    std::string result{};
    append_real(result, value);
    return result;
}

Parsed<std::int64_t> parse_signed(const std::string_view text, const Base base) noexcept {
    const Parsed<std::int64_t> result = whole(integer_prefix<std::int64_t>(text, base), text);
    return result;
}

Parsed<std::uint64_t> parse_unsigned(const std::string_view text, const Base base) noexcept {
    const Parsed<std::uint64_t> result = whole(integer_prefix<std::uint64_t>(text, base), text);
    return result;
}

Parsed<double> parse_double(const std::string_view text) noexcept {
    const Parsed<double> result = whole(real_prefix<double>(text), text);
    return result;
}

Parsed<float> parse_float(const std::string_view text) noexcept {
    const Parsed<float> result = whole(real_prefix<float>(text), text);
    return result;
}

std::string_view trim(const std::string_view text) noexcept {
    std::size_t first = 0;
    std::size_t last = text.size();
    while (first < last && is_space(text[first])) ++first;
    while (last > first && is_space(text[last - 1])) --last;
    const std::string_view result = text.substr(first, last - first);
    return result;
}

TextScanner::TextScanner(const std::string_view text) noexcept : text_(text) {}

bool TextScanner::read_signed(std::int64_t& value, const Base base) noexcept {
    const Prefix<std::int64_t> prefix = integer_prefix<std::int64_t>(rest(), base);
    const std::int64_t* const read = prefix.parsed.value();
    if (read == nullptr) return false;
    value = *read;
    position_ += prefix.consumed;
    return true;
}

bool TextScanner::read_unsigned(std::uint64_t& value, const Base base) noexcept {
    const Prefix<std::uint64_t> prefix = integer_prefix<std::uint64_t>(rest(), base);
    const std::uint64_t* const read = prefix.parsed.value();
    if (read == nullptr) return false;
    value = *read;
    position_ += prefix.consumed;
    return true;
}

bool TextScanner::read_double(double& value) noexcept {
    const Prefix<double> prefix = real_prefix<double>(rest());
    const double* const read = prefix.parsed.value();
    if (read == nullptr) return false;
    value = *read;
    position_ += prefix.consumed;
    return true;
}

bool TextScanner::expect(const std::string_view literal) noexcept {
    if (rest().substr(0, literal.size()) != literal) return false;
    position_ += literal.size();
    return true;
}

bool TextScanner::read_until(const std::string_view delimiters, std::string_view& field) noexcept {
    const std::string_view remaining = rest();
    std::size_t length = remaining.find_first_of(delimiters);
    if (length == std::string_view::npos) length = remaining.size();
    if (length == 0) return false;
    field = remaining.substr(0, length);
    position_ += length;
    return true;
}

std::size_t TextScanner::skip_spaces() noexcept {
    const std::size_t start = position_;
    while (position_ < text_.size() && is_space(text_[position_])) ++position_;
    const std::size_t result = position_ - start;
    return result;
}

bool TextScanner::at_end() const noexcept {
    const bool result = position_ == text_.size();
    return result;
}

std::size_t TextScanner::position() const noexcept { return position_; }

std::string_view TextScanner::rest() const noexcept {
    const std::string_view result = text_.substr(position_);
    return result;
}

} // namespace stx_auxlib::text
