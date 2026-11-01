#pragma once

// Locale-free formatting and parsing of numbers and text, replacing
// snprintf, sscanf, std::to_string, std::sto*, atoi/atof, the stream
// manipulators and setlocale. There are no format strings: every format is
// a typed record, so no format can mismatch its arguments or be supplied by
// input. Output is identical on every host and in every locale.
//
// Numbers use std::to_chars and std::from_chars, whose results the standard
// fixes exactly: the shortest form of a floating value is the shortest text
// that parses back to the same value (the shorter of fixed and scientific
// forms, fixed on a tie), and fixed and scientific forms are correctly
// rounded. NaN is always spelled "nan" (sign and payload are not written)
// and infinities "inf" and "-inf".
//
// Formatting appends to a caller's std::string, which keeps its capacity
// for reuse; to_text returns a new string for the common one-off case.
// Parsing reads whole strings (or, with TextScanner, consecutive fields)
// and reports failures as values; it never skips whitespace on its own.

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace stx_auxlib::text {

enum class Base : std::uint8_t {
    binary = 2,
    octal = 8,
    decimal = 10,
    hexadecimal = 16,
};

enum class Sign : std::uint8_t {
    // "-" for negative values only.
    negative_only = 0,
    // "+" or "-" always ("+0" for zero).
    always = 1,
};

enum class Letters : std::uint8_t {
    lower = 0,
    upper = 1,
};

enum class Alignment : std::uint8_t {
    right = 0,
    left = 1,
};

// Bounds the caller-chosen sizes of a format, so that no format can request
// an unbounded allocation: Field::width and IntegerFormat::minimum_digits
// above maximum_width, and RealFormat::precision above maximum_precision,
// are treated as those maxima.
constexpr std::size_t maximum_width = 4096;
// Covers every digit a double's exact decimal expansion can have.
constexpr std::size_t maximum_precision = 1100;

// Pads the formatted item to `width` with `fill` (an ASCII character),
// counting Unicode scalar values (not bytes, not display columns). An item
// already at least `width` long is not cut.
struct Field final {
    std::size_t width{0};
    char fill{' '};
    Alignment alignment{Alignment::right};
};

struct IntegerFormat final {
    Base base{Base::decimal};
    // Zero-pads the digits (not the sign) to at least this many: "%04x" is
    // {.base = Base::hexadecimal, .minimum_digits = 4}.
    std::size_t minimum_digits{1};
    Sign sign{Sign::negative_only};
    Letters letters{Letters::lower};
    Field field{};
};

enum class Notation : std::uint8_t {
    // The shortest text that parses back to the same value.
    shortest = 0,
    // `precision` digits after the point ("%.2f").
    fixed = 1,
    // One digit, the point, `precision` digits, and a signed exponent of at
    // least two digits ("%.3e").
    scientific = 2,
};

struct RealFormat final {
    Notation notation{Notation::shortest};
    // Ignored by Notation::shortest.
    std::size_t precision{6};
    Sign sign{Sign::negative_only};
    Field field{};
};

void append_signed(std::string& output, std::int64_t value, const IntegerFormat& format = {});
void append_unsigned(std::string& output, std::uint64_t value, const IntegerFormat& format = {});
void append_real(std::string& output, double value, const RealFormat& format = {});
// Shortest form here is the shortest that round-trips as a float.
void append_real(std::string& output, float value, const RealFormat& format = {});
// Pads text, assumed to be UTF-8, to the field.
void append_text(std::string& output, std::string_view value, const Field& field = {});

// Any integer type but bool, as signed or unsigned by its signedness.
template <typename Integer>
concept IntegerValue = std::integral<Integer> && !std::same_as<Integer, bool>;

template <IntegerValue Integer>
void append_integer(std::string& output, const Integer value, const IntegerFormat& format = {}) {
    if constexpr (std::signed_integral<Integer>) {
        append_signed(output, static_cast<std::int64_t>(value), format);
    } else {
        append_unsigned(output, static_cast<std::uint64_t>(value), format);
    }
}

// The decimal form of an integer, or the shortest form of a real.
template <IntegerValue Integer>
[[nodiscard]] std::string to_text(const Integer value) {
    std::string result{};
    append_integer(result, value);
    return result;
}
[[nodiscard]] std::string to_text(double value);
[[nodiscard]] std::string to_text(float value);

enum class ParseFailure : std::uint8_t {
    empty = 1,
    // A character that cannot start or continue the number.
    invalid = 2,
    // A well-formed number the type cannot hold: integer overflow, or a
    // real whose magnitude rounds to infinity or to zero (a nonzero literal
    // never parses as zero).
    out_of_range = 3,
    // A number followed by more text.
    trailing_text = 4,
};

// The result of a parse: the value, or why it failed. Never both.
template <typename Value>
class Parsed final {
public:
    [[nodiscard]] static Parsed success(const Value value) noexcept {
        Parsed result(State(std::in_place_index<0>, value));
        return result;
    }
    [[nodiscard]] static Parsed failure(const ParseFailure failure) noexcept {
        Parsed result(State(std::in_place_index<1>, failure));
        return result;
    }
    [[nodiscard]] bool succeeded() const noexcept {
        const bool result = std::holds_alternative<Value>(state_);
        return result;
    }
    // The value; null on failure.
    [[nodiscard]] const Value* value() const noexcept {
        const Value* const result = std::get_if<Value>(&state_);
        return result;
    }
    // The failure; null on success.
    [[nodiscard]] const ParseFailure* failure() const noexcept {
        const ParseFailure* const result = std::get_if<ParseFailure>(&state_);
        return result;
    }

private:
    using State = std::variant<Value, ParseFailure>;
    explicit Parsed(State state) noexcept : state_(std::move(state)) {}
    State state_;
};

// Whole-text parses. Integers: an optional '+' (or '-' for signed), then
// digits of the base, with no prefix ("0x" is not accepted). Reals: an
// optional sign, decimal digits with an optional point and exponent, or
// "inf", "infinity" or "nan" in any case; hexadecimal floats are not
// accepted.
[[nodiscard]] Parsed<std::int64_t> parse_signed(std::string_view text,
                                                Base base = Base::decimal) noexcept;
[[nodiscard]] Parsed<std::uint64_t> parse_unsigned(std::string_view text,
                                                   Base base = Base::decimal) noexcept;
[[nodiscard]] Parsed<double> parse_double(std::string_view text) noexcept;
[[nodiscard]] Parsed<float> parse_float(std::string_view text) noexcept;

// True for a std::string rvalue, which a returned or retained view must not
// borrow. The deleted overloads below are templates constrained by it, so
// string literals and views still choose the string_view overloads.
template <typename Argument>
concept TemporaryString = std::same_as<Argument, std::string>;

// `text` without leading and trailing ASCII whitespace (space, tab, line
// feed, carriage return, vertical tab, form feed). Returns a view into
// `text`; the rvalue overload is deleted so the view cannot outlive a
// temporary.
[[nodiscard]] std::string_view trim(std::string_view text) noexcept;
template <TemporaryString Argument>
std::string_view trim(Argument&& text) = delete;

// Reads consecutive fields from text it borrows, replacing sscanf. Every
// read either consumes its field and succeeds, or fails and consumes
// nothing. Reads never skip whitespace; call skip_spaces where the format
// allows it. Declare the scanner after the text it borrows.
class TextScanner final {
public:
    explicit TextScanner(std::string_view text) noexcept;
    template <TemporaryString Argument>
    explicit TextScanner(Argument&& text) = delete;

    [[nodiscard]] bool read_signed(std::int64_t& value, Base base = Base::decimal) noexcept;
    [[nodiscard]] bool read_unsigned(std::uint64_t& value, Base base = Base::decimal) noexcept;
    [[nodiscard]] bool read_double(double& value) noexcept;
    // Consumes `literal` exactly when the text continues with it.
    [[nodiscard]] bool expect(std::string_view literal) noexcept;
    // Reads up to (not including) the first of `delimiters`, or to the end;
    // fails on an empty field.
    [[nodiscard]] bool read_until(std::string_view delimiters, std::string_view& field) noexcept;
    // Skips ASCII whitespace; returns how many bytes it skipped.
    std::size_t skip_spaces() noexcept;

    [[nodiscard]] bool at_end() const noexcept;
    // Bytes consumed so far.
    [[nodiscard]] std::size_t position() const noexcept;
    [[nodiscard]] std::string_view rest() const noexcept;

private:
    std::string_view text_;
    std::size_t position_{0};
};

} // namespace stx_auxlib::text
