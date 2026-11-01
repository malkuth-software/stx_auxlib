// Tests of stx_auxlib::text (include/stx_auxlib/text.hpp).
//
// Exact expectations come from the C++ standard's definitions. A
// differential check compares fixed and scientific output with the host's
// snprintf, which is correctly rounded on the hosts tested here (macOS,
// glibc; the Windows UCRT is too) and runs in the "C" locale because
// nothing here calls setlocale; shortest output is checked by round trip.
// `--bench` prints timings against snprintf, std::to_string and strtod.

#include "../src/checked.hpp"

#include "stx_auxlib/text.hpp"

#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace text = stx_auxlib::text;

void require(const bool condition, const char* const message, const std::string_view detail) {
    if (condition) return;
    std::fprintf(stderr, "stx_auxlib text: %s: \"%.*s\"\n", message, static_cast<int>(detail.size()),
                 detail.data());
    std::abort();
}

std::string signed_text(const std::int64_t value, const text::IntegerFormat& format) {
    std::string result{};
    text::append_signed(result, value, format);
    return result;
}

std::string unsigned_text(const std::uint64_t value, const text::IntegerFormat& format) {
    std::string result{};
    text::append_unsigned(result, value, format);
    return result;
}

std::string real_text(const double value, const text::RealFormat& format) {
    std::string result{};
    text::append_real(result, value, format);
    return result;
}

void expect(const std::string& actual, const std::string_view expected) {
    require(actual == expected, "formatted wrongly", actual);
}

void integers() {
    expect(signed_text(0, {}), "0");
    expect(signed_text(-42, {}), "-42");
    expect(signed_text(std::numeric_limits<std::int64_t>::min(), {}), "-9223372036854775808");
    expect(unsigned_text(std::numeric_limits<std::uint64_t>::max(), {}), "18446744073709551615");
    expect(signed_text(7, {.sign = text::Sign::always}), "+7");
    expect(signed_text(0, {.sign = text::Sign::always}), "+0");
    // %02X, %08x, %04u
    expect(unsigned_text(10, {.base = text::Base::hexadecimal, .minimum_digits = 2,
                              .letters = text::Letters::upper}),
           "0A");
    expect(unsigned_text(0xbeef, {.base = text::Base::hexadecimal, .minimum_digits = 8}),
           "0000beef");
    expect(unsigned_text(42, {.minimum_digits = 4}), "0042");
    expect(signed_text(-5, {.minimum_digits = 4}), "-0005");
    expect(unsigned_text(5, {.base = text::Base::binary}), "101");
    expect(unsigned_text(8, {.base = text::Base::octal}), "10");
    // %2zu, %-6d, fill
    expect(unsigned_text(3, {.field = {.width = 2}}), " 3");
    expect(signed_text(-12, {.field = {.width = 6, .alignment = text::Alignment::left}}), "-12   ");
    expect(signed_text(12, {.field = {.width = 5, .fill = '*'}}), "***12");
    expect(signed_text(123456, {.field = {.width = 3}}), "123456");
    // Caps: an enormous width or digit count is bounded, never allocated.
    const std::string capped = unsigned_text(1, {.minimum_digits = 1u << 30});
    require(capped.size() == text::maximum_width, "minimum_digits was not capped", "");
    const std::string wide = signed_text(1, {.field = {.width = std::size_t{1} << 40}});
    require(wide.size() == text::maximum_width, "width was not capped", "");
    // The generic forms pick signedness from the type.
    std::string generic{};
    text::append_integer(generic, static_cast<unsigned char>(200));
    text::append_integer(generic, static_cast<short>(-3));
    expect(generic, "200-3");
    expect(text::to_text(-17), "-17");
    expect(text::to_text(17ull), "17");
}

void reals() {
    expect(text::to_text(0.1), "0.1");
    expect(text::to_text(1e21), "1e+21");
    expect(text::to_text(123456.0), "123456");
    expect(text::to_text(-0.0), "-0");
    expect(text::to_text(5e-324), "5e-324");
    expect(text::to_text(0.1f), "0.1");
    expect(text::to_text(1.0f / 3.0f), "0.33333334");
    expect(text::to_text(std::numeric_limits<double>::quiet_NaN()), "nan");
    expect(text::to_text(-std::numeric_limits<double>::quiet_NaN()), "nan");
    expect(text::to_text(std::numeric_limits<double>::infinity()), "inf");
    expect(text::to_text(-std::numeric_limits<double>::infinity()), "-inf");
    // %.1f, %.3f, %3.0f, %.3e
    expect(real_text(2.25, {.notation = text::Notation::fixed, .precision = 1}), "2.2");
    expect(real_text(2.35, {.notation = text::Notation::fixed, .precision = 1}), "2.4");
    expect(real_text(-1.0005, {.notation = text::Notation::fixed, .precision = 3}), "-1.000");
    expect(real_text(7.6, {.notation = text::Notation::fixed, .precision = 0,
                           .field = {.width = 3}}),
           "  8");
    expect(real_text(12345.678, {.notation = text::Notation::scientific, .precision = 3}),
           "1.235e+04");
    expect(real_text(1.5, {.sign = text::Sign::always}), "+1.5");
    expect(real_text(std::numeric_limits<double>::infinity(), {.sign = text::Sign::always}),
           "+inf");
    const std::string huge = real_text(1e308, {.notation = text::Notation::fixed,
                                               .precision = 1u << 20});
    require(huge.size() == 309 + 1 + text::maximum_precision, "precision was not capped", "");
}

// Deterministic pseudo-random doubles over every exponent, subnormals
// included (SplitMix64 over raw bit patterns).
STX_AUXLIB_MODULAR std::uint64_t next_bits(std::uint64_t& state) {
    state += 0x9e3779b97f4a7c15ULL;
    std::uint64_t mixed = state;
    mixed = (mixed ^ (mixed >> 30)) * 0xbf58476d1ce4e5b9ULL;
    mixed = (mixed ^ (mixed >> 27)) * 0x94d049bb133111ebULL;
    const std::uint64_t result = mixed ^ (mixed >> 31);
    return result;
}

void round_trips_and_host_agreement() {
    std::uint64_t state = 1;
    char host[2048];
    for (int sample = 0; sample < 200000; ++sample) {
        const double value = std::bit_cast<double>(next_bits(state));
        if (!std::isfinite(value)) continue;
        const std::string shortest = text::to_text(value);
        const text::Parsed<double> back = text::parse_double(shortest);
        require(back.succeeded() && std::bit_cast<std::uint64_t>(*back.value()) ==
                                        std::bit_cast<std::uint64_t>(value),
                "shortest text did not round-trip", shortest);
        if (sample % 4 != 0) continue;
        const int precision = static_cast<int>(sample % 25);
        std::snprintf(host, sizeof host, "%.*e", precision, value);
        expect(real_text(value, {.notation = text::Notation::scientific,
                                 .precision = static_cast<std::size_t>(precision)}),
               host);
        if (std::fabs(value) < 1e30) {
            std::snprintf(host, sizeof host, "%.*f", precision, value);
            expect(real_text(value, {.notation = text::Notation::fixed,
                                     .precision = static_cast<std::size_t>(precision)}),
                   host);
        }
    }
    for (int sample = 0; sample < 100000; ++sample) {
        const float value = std::bit_cast<float>(static_cast<std::uint32_t>(next_bits(state)));
        if (!std::isfinite(value)) continue;
        const std::string shortest = text::to_text(value);
        const text::Parsed<float> back = text::parse_float(shortest);
        require(back.succeeded() && std::bit_cast<std::uint32_t>(*back.value()) ==
                                        std::bit_cast<std::uint32_t>(value),
                "shortest float text did not round-trip", shortest);
    }
}

template <typename Value>
void expect_failure(const text::Parsed<Value>& parsed, const text::ParseFailure failure,
                    const std::string_view input) {
    require(!parsed.succeeded(), "invalid text parsed", input);
    require(*parsed.failure() == failure, "parse failed for the wrong reason", input);
}

void parsing() {
    require(*text::parse_signed("+5").value() == 5, "a leading '+' was refused", "+5");
    require(*text::parse_signed("-5").value() == -5, "a negative was refused", "-5");
    require(*text::parse_signed("-9223372036854775808").value() ==
                std::numeric_limits<std::int64_t>::min(),
            "INT64_MIN was refused", "-9223372036854775808");
    require(*text::parse_unsigned("ff", text::Base::hexadecimal).value() == 255,
            "hexadecimal was refused", "ff");
    require(*text::parse_unsigned("FF", text::Base::hexadecimal).value() == 255,
            "upper-case hexadecimal was refused", "FF");
    expect_failure(text::parse_signed(""), text::ParseFailure::empty, "");
    expect_failure(text::parse_signed(" 5"), text::ParseFailure::invalid, " 5");
    expect_failure(text::parse_signed("5 "), text::ParseFailure::trailing_text, "5 ");
    expect_failure(text::parse_signed("5x"), text::ParseFailure::trailing_text, "5x");
    expect_failure(text::parse_signed("+"), text::ParseFailure::invalid, "+");
    expect_failure(text::parse_signed("+-5"), text::ParseFailure::invalid, "+-5");
    expect_failure(text::parse_signed("9223372036854775808"), text::ParseFailure::out_of_range,
                   "9223372036854775808");
    expect_failure(text::parse_unsigned("-1"), text::ParseFailure::invalid, "-1");
    expect_failure(text::parse_unsigned("0x10", text::Base::hexadecimal),
                   text::ParseFailure::trailing_text, "0x10");

    require(*text::parse_double("1.5e+3").value() == 1500.0, "exponent form was refused", "1.5e+3");
    require(*text::parse_double("+.5").value() == 0.5, "\"+.5\" was refused", "+.5");
    require(*text::parse_double("1e-310").value() > 0.0, "a subnormal was refused", "1e-310");
    require(std::isinf(*text::parse_double("inf").value()), "\"inf\" was refused", "inf");
    require(std::isinf(*text::parse_double("-Infinity").value()), "\"-Infinity\" was refused",
            "-Infinity");
    require(std::isnan(*text::parse_double("NaN").value()), "\"NaN\" was refused", "NaN");
    require(std::signbit(*text::parse_double("-0").value()), "\"-0\" lost its sign", "-0");
    expect_failure(text::parse_double("1e309"), text::ParseFailure::out_of_range, "1e309");
    expect_failure(text::parse_double("1e-400"), text::ParseFailure::out_of_range, "1e-400");
    expect_failure(text::parse_double("2e-324"), text::ParseFailure::out_of_range, "2e-324");
    expect_failure(text::parse_float("3.5e38"), text::ParseFailure::out_of_range, "3.5e38");
    expect_failure(text::parse_double("1e"), text::ParseFailure::trailing_text, "1e");
    expect_failure(text::parse_double("0x1p3"), text::ParseFailure::trailing_text, "0x1p3");
    expect_failure(text::parse_double("."), text::ParseFailure::invalid, ".");
    expect_failure(text::parse_double("1,5"), text::ParseFailure::trailing_text, "1,5");

    require(text::trim("  \t a b \r\n") == "a b", "trim is wrong", "  \t a b \r\n");
    require(text::trim(" \n ").empty(), "trim of spaces is not empty", " \n ");
}

void scanning() {
    const std::string input = "12,-34; name=5.5 tail";
    text::TextScanner scanner(input);
    std::int64_t first = 0;
    std::int64_t second = 0;
    std::string_view key{};
    double real = 0.0;
    require(scanner.read_signed(first) && first == 12, "first field is wrong", input);
    require(!scanner.read_signed(second) && scanner.position() == 2,
            "a failed read consumed text", input);
    require(scanner.expect(",") && scanner.read_signed(second) && second == -34,
            "second field is wrong", input);
    require(scanner.expect(";") && scanner.skip_spaces() == 1, "separator is wrong", input);
    require(scanner.read_until("=", key) && key == "name", "key is wrong", input);
    require(!scanner.expect(":") && scanner.expect("="), "expect is wrong", input);
    require(scanner.read_double(real) && real == 5.5, "real field is wrong", input);
    require(scanner.rest() == " tail" && !scanner.at_end(), "rest is wrong", input);
    std::string_view word{};
    scanner.skip_spaces();
    require(scanner.read_until(" ", word) && word == "tail" && scanner.at_end(),
            "last field is wrong", input);
    require(!scanner.read_until(" ", word), "an empty field was read", input);
}

// --bench --------------------------------------------------------------------

double nanoseconds_per(const std::chrono::steady_clock::time_point start, const std::size_t count) {
    const std::chrono::duration<double, std::nano> elapsed = std::chrono::steady_clock::now() - start;
    const double result = elapsed.count() / static_cast<double>(count);
    return result;
}

void benchmark() {
    constexpr std::size_t count = 1000000;
    std::vector<double> reals(count);
    std::vector<std::int64_t> integers(count);
    std::uint64_t state = 7;
    for (std::size_t index = 0; index < count; ++index) {
        reals[index] = std::ldexp(static_cast<double>(next_bits(state) >> 11),
                                  static_cast<int>(next_bits(state) % 80) - 100);
        integers[index] = static_cast<std::int64_t>(next_bits(state) >> 20) - (1LL << 43);
    }
    std::string output{};
    output.reserve(64);
    char host[64];
    std::size_t sink = 0;

    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    for (const double value : reals) {
        output.clear();
        text::append_real(output, value);
        sink += output.size();
    }
    const double shortest = nanoseconds_per(start, count);
    start = std::chrono::steady_clock::now();
    for (const double value : reals) sink += static_cast<std::size_t>(std::snprintf(host, sizeof host, "%.17g", value));
    const double host_round_trip = nanoseconds_per(start, count);
    start = std::chrono::steady_clock::now();
    for (const double value : reals) {
        output.clear();
        text::append_real(output, value, {.notation = text::Notation::fixed, .precision = 2});
        sink += output.size();
    }
    const double fixed = nanoseconds_per(start, count);
    start = std::chrono::steady_clock::now();
    for (const double value : reals) sink += static_cast<std::size_t>(std::snprintf(host, sizeof host, "%.2f", value));
    const double host_fixed = nanoseconds_per(start, count);
    start = std::chrono::steady_clock::now();
    for (const double value : reals) sink += std::to_string(value).size();
    const double standard_to_string = nanoseconds_per(start, count);

    start = std::chrono::steady_clock::now();
    for (const std::int64_t value : integers) {
        output.clear();
        text::append_signed(output, value);
        sink += output.size();
    }
    const double integer = nanoseconds_per(start, count);
    start = std::chrono::steady_clock::now();
    for (const std::int64_t value : integers) sink += static_cast<std::size_t>(std::snprintf(host, sizeof host, "%lld", static_cast<long long>(value)));
    const double host_integer = nanoseconds_per(start, count);

    std::vector<std::string> texts(count);
    for (std::size_t index = 0; index < count; ++index) texts[index] = text::to_text(reals[index]);
    start = std::chrono::steady_clock::now();
    for (const std::string& value : texts) sink += text::parse_double(value).succeeded() ? 1 : 0;
    const double parse = nanoseconds_per(start, count);
    start = std::chrono::steady_clock::now();
    for (const std::string& value : texts) sink += std::strtod(value.c_str(), nullptr) > 0.0 ? 1 : 0;
    const double host_parse = nanoseconds_per(start, count);

    std::printf("ns per value over %zu values (sink %zu)\n", count, sink);
    std::printf("  shortest double        %7.1f   snprintf %%.17g %7.1f\n", shortest, host_round_trip);
    std::printf("  fixed, 2 digits        %7.1f   snprintf %%.2f  %7.1f   std::to_string %7.1f\n",
                fixed, host_fixed, standard_to_string);
    std::printf("  signed integer         %7.1f   snprintf %%lld  %7.1f\n", integer, host_integer);
    std::printf("  parse_double           %7.1f   strtod        %7.1f\n", parse, host_parse);
}

} // namespace

int main(const int argument_count, char** const arguments) {
    if (argument_count == 2 && std::strcmp(arguments[1], "--bench") == 0) {
        benchmark();
        return 0;
    }
    integers();
    reals();
    round_trips_and_host_agreement();
    parsing();
    scanning();
    std::puts("stx_auxlib text tests passed");
    return 0;
}
