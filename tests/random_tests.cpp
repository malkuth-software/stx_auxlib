// Tests of stx_auxlib::random (include/stx_auxlib/random.hpp). The expected
// values come from tests/reference/random_reference.py, an independent
// implementation of the published algorithms; its ChaCha core reproduces
// the RFC 8439 section 2.3.2 ChaCha20 block, checked here too.

#include "stx_auxlib/random.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <span>
#include <vector>

namespace {

namespace random = stx_auxlib::random;

void require(const bool condition, const char* const message) {
    if (condition) return;
    std::fprintf(stderr, "stx_auxlib random: %s\n", message);
    std::abort();
}

void chacha20_known_answer() {
    std::array<std::uint32_t, 16> input = {0x61707865U, 0x3320646eU, 0x79622d32U, 0x6b206574U};
    for (std::uint32_t word = 0; word < 8; ++word) {
        const std::uint32_t base = word * 4;
        input[4 + word] = base | (base + 1) << 8 | (base + 2) << 16 | (base + 3) << 24;
    }
    input[12] = 1;
    input[13] = 0x09000000U;
    input[14] = 0x4a000000U;
    input[15] = 0;
    const std::array<std::uint32_t, 16> expected = {
        0xe4e7f110U, 0x15593bd1U, 0x1fdd0f50U, 0xc47120a3U, 0xc7f4d1c7U, 0x0368c033U,
        0x9aaa2204U, 0x4e6cd4c3U, 0x466482d2U, 0x09aa9f07U, 0x05d7c214U, 0xa2028bd9U,
        0xd19c12b5U, 0xb94e16deU, 0xe883d0cbU, 0x4e3c50a2U};
    std::array<std::uint32_t, 16> output{};
    random::detail::chacha_block(input, 10, output);
    require(output == expected, "the ChaCha20 block differs from RFC 8439 2.3.2");
}

void seed_expansion() {
    const random::Seed seed = random::seed_from_number(42);
    const std::array<std::uint64_t, 4> expected = {0xbdd732262feb6e95ULL, 0x28efe333b266f103ULL,
                                                   0x47526757130f9f52ULL, 0x581ce1ff0e4ae394ULL};
    require(seed.words == expected, "seed_from_number(42) differs from SplitMix64");
}

void chacha12_stream() {
    const random::Seed seed = random::seed_from_number(42);
    random::ChaCha12Stream generator(seed);
    const std::array<std::uint64_t, 8> first = {
        0x280b7b79f392fa12ULL, 0x4dadef83bc931d07ULL, 0xc195c99ba5375e5fULL, 0x7e657f1b6bdc3bfdULL,
        0xfe40a244bc14b82fULL, 0x3dd75b637ba65c81ULL, 0x91c8dff96cfcd24aULL, 0xcb61b56a793c1223ULL};
    for (const std::uint64_t value : first) require(generator.next() == value, "ChaCha12 output differs");
    for (int index = 8; index < 999; ++index) (void)generator.next();
    require(generator.next() == 0xd04aeb60bd0b72f6ULL, "ChaCha12 output 999 differs");

    random::ChaCha12Stream other_stream(seed, 7);
    const std::array<std::uint64_t, 4> seventh = {0xe48b814dc75d8cf8ULL, 0x55c9dc5976130508ULL,
                                                  0xf25fe228aa0e1a5bULL, 0x1b7d08372c59e3a8ULL};
    for (const std::uint64_t value : seventh)
        require(other_stream.next() == value, "ChaCha12 stream 7 differs");

    // fill() is next() as little-endian bytes; a partial output is dropped.
    random::ChaCha12Stream bytes_generator(seed);
    std::array<std::byte, 12> bytes{};
    bytes_generator.fill(bytes);
    for (std::size_t index = 0; index < 8; ++index)
        require(bytes[index] == static_cast<std::byte>(first[0] >> (8 * index)),
                "fill's first word differs");
    for (std::size_t index = 0; index < 4; ++index)
        require(bytes[8 + index] == static_cast<std::byte>(first[1] >> (8 * index)),
                "fill's partial word differs");
    require(bytes_generator.next() == first[2], "fill did not consume the partial word");
}

void xoshiro() {
    const random::Seed seed = random::seed_from_number(42);
    random::Xoshiro256PlusPlus generator(seed);
    const std::array<std::uint64_t, 8> first = {
        0xd0764d4f4476689fULL, 0x519e4174576f3791ULL, 0xfbe07cfb0c24ed8cULL, 0xb37d9f600cd835b8ULL,
        0xcb231c3874846a73ULL, 0x968d9f004e50de7dULL, 0x201718ff221a3556ULL, 0x9ae94e070ed8cb46ULL};
    for (const std::uint64_t value : first) require(generator.next() == value, "xoshiro256++ differs");
    random::Xoshiro256PlusPlus jumped(seed);
    jumped.jump();
    require(jumped.next() == 0xc0b6f4be293b1ae5ULL && jumped.next() == 0x5db3dd9683e7bb33ULL,
            "jump differs");
    random::Xoshiro256PlusPlus far(seed);
    far.long_jump();
    require(far.next() == 0x02019a87bfc0bb07ULL && far.next() == 0x25bee49209717963ULL,
            "long_jump differs");
    // An all-zero seed, which xoshiro cannot leave, is replaced.
    random::Xoshiro256PlusPlus zero(random::Seed{});
    random::Xoshiro256PlusPlus replacement(random::seed_from_number(0));
    require(zero.next() == replacement.next() && zero.next() != 0, "the zero seed was not replaced");
}

void product() {
    const random::detail::Product largest =
        random::detail::multiply(std::numeric_limits<std::uint64_t>::max(),
                                 std::numeric_limits<std::uint64_t>::max());
    require(largest.high == 0xfffffffffffffffeULL && largest.low == 1, "the largest product differs");
    const random::detail::Product mixed =
        random::detail::multiply(0x123456789abcdef0ULL, 0x0fedcba987654321ULL);
    require(mixed.high == 0x0121fa00ad77d742ULL && mixed.low == 0x2236d88fe5618cf0ULL,
            "a mixed product differs");
}

void distributions() {
    const random::Seed seed = random::seed_from_number(42);
    random::ChaCha12Stream dice_generator(seed);
    const std::array<std::uint64_t, 10> dice = {0, 1, 4, 2, 5, 1, 3, 4, 1, 4};
    for (const std::uint64_t face : dice)
        require(random::uniform_below(dice_generator, 6) == face, "uniform_below differs");

    random::ChaCha12Stream shuffle_generator(seed);
    std::array<int, 10> deck = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
    random::shuffle(shuffle_generator, std::span<int>(deck));
    const std::array<int, 10> shuffled = {4, 0, 7, 8, 9, 5, 3, 6, 2, 1};
    require(deck == shuffled, "shuffle differs");

    // Uniformity: 600,000 rolls of a die, each face within 1% of 100,000.
    random::Xoshiro256PlusPlus counter_generator(seed);
    std::array<int, 6> counts{};
    for (int roll = 0; roll < 600000; ++roll)
        ++counts[static_cast<std::size_t>(random::uniform_below(counter_generator, 6))];
    for (const int count : counts) require(count > 99000 && count < 101000, "a die is biased");

    require(random::uniform_below(counter_generator, 1) == 0, "a bound of 1 gave nonzero");
    require(random::uniform_below(counter_generator, 0) == 0, "a bound of 0 gave nonzero");
    const std::uint64_t huge_bound = (std::uint64_t{1} << 63) + 1;
    for (int draw = 0; draw < 1000; ++draw)
        require(random::uniform_below(counter_generator, huge_bound) < huge_bound,
                "a huge bound was exceeded");

    for (int draw = 0; draw < 10000; ++draw) {
        const std::int64_t value = random::uniform_between(counter_generator, -3, 3);
        require(value >= -3 && value <= 3, "uniform_between left its range");
    }
    (void)random::uniform_between(counter_generator, std::numeric_limits<std::int64_t>::min(),
                                  std::numeric_limits<std::int64_t>::max());
    require(random::uniform_between(counter_generator, 5, 5) == 5, "an empty range was not low");

    for (int draw = 0; draw < 10000; ++draw) {
        const double unit = random::uniform_unit(counter_generator);
        require(unit >= 0.0 && unit < 1.0, "uniform_unit left [0, 1)");
        require(unit * 0x1.0p53 == std::floor(unit * 0x1.0p53), "uniform_unit has more than 53 bits");
        const float unit_float = random::uniform_unit_float(counter_generator);
        require(unit_float >= 0.0f && unit_float < 1.0f, "uniform_unit_float left [0, 1)");
        const double real = random::uniform_real(counter_generator, -2.5, 7.25);
        require(real >= -2.5 && real < 7.25, "uniform_real left [low, high)");
        const double wide = random::uniform_real(counter_generator, -std::numeric_limits<double>::max(),
                                                 std::numeric_limits<double>::max());
        require(std::isfinite(wide), "a full-range uniform_real was not finite");
    }
    require(random::uniform_real(counter_generator, 1.0, 1.0) == 1.0, "an empty real range was not low");
    require(!random::chance(counter_generator, 0.0), "chance(0) happened");
    require(random::chance(counter_generator, 1.0), "chance(1) did not happen");
}

void entropy() {
    std::array<std::byte, 600> bytes{};
    require(random::fill_entropy(bytes), "the host has no entropy");
    bool any_nonzero = false;
    for (const std::byte value : bytes) any_nonzero = any_nonzero || value != std::byte{0};
    require(any_nonzero, "entropy was all zero");
    const random::SeedResult first = random::entropy_seed();
    const random::SeedResult second = random::entropy_seed();
    require(first.succeeded() && second.succeeded(), "entropy_seed failed");
    require((*first.seed()).words != (*second.seed()).words, "two entropy seeds were equal");
}

} // namespace

int main() {
    chacha20_known_answer();
    seed_expansion();
    chacha12_stream();
    xoshiro();
    product();
    distributions();
    entropy();
    std::puts("stx_auxlib random tests passed");
    return 0;
}
