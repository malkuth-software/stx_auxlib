// stx_auxlib::random (include/stx_auxlib/random.hpp): the generators and
// seed expansion, pure computation shared by every implementation. Host
// entropy is in src/hosted/entropy_<host>.cpp. tests/reference/
// random_reference.py is the independent reference the tests check
// against.

#include "stx_auxlib/random.hpp"

#include "checked.hpp"

#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <utility>
#include <variant>

namespace stx_auxlib::random {

namespace {

constexpr std::array<std::uint32_t, 4> chacha_constants = {0x61707865U, 0x3320646eU, 0x79622d32U,
                                                           0x6b206574U};
constexpr int chacha12_double_rounds = 6;

// ChaCha, SplitMix64 and xoshiro are defined modulo 2^32 or 2^64; their
// wraparound is the algorithm.
STX_AUXLIB_MODULAR void quarter_round(std::array<std::uint32_t, 16>& x, const std::size_t a, const std::size_t b,
                   const std::size_t c, const std::size_t d) noexcept {
    x[a] += x[b];
    x[d] = std::rotl(x[d] ^ x[a], 16);
    x[c] += x[d];
    x[b] = std::rotl(x[b] ^ x[c], 12);
    x[a] += x[b];
    x[d] = std::rotl(x[d] ^ x[a], 8);
    x[c] += x[d];
    x[b] = std::rotl(x[b] ^ x[c], 7);
}

STX_AUXLIB_MODULAR std::uint64_t splitmix_step(std::uint64_t& state) noexcept {
    state += 0x9e3779b97f4a7c15ULL;
    std::uint64_t mixed = state;
    mixed = (mixed ^ (mixed >> 30)) * 0xbf58476d1ce4e5b9ULL;
    mixed = (mixed ^ (mixed >> 27)) * 0x94d049bb133111ebULL;
    const std::uint64_t result = mixed ^ (mixed >> 31);
    return result;
}

// xoshiro256's jump polynomials (Blackman and Vigna's reference code).
constexpr std::array<std::uint64_t, 4> jump_table = {0x180ec6d33cfd0abaULL, 0xd5a61266f0c9392cULL,
                                                     0xa9582618e03fc9aaULL, 0x39abdc4529b1661cULL};
constexpr std::array<std::uint64_t, 4> long_jump_table = {
    0x76e15d3efefdcbbfULL, 0xc5004e441c522fb3ULL, 0x77710069854ee241ULL, 0x39109bb02acbe635ULL};

} // namespace

STX_AUXLIB_MODULAR void detail::chacha_block(const std::array<std::uint32_t, 16>& input, const int double_rounds,
                          std::array<std::uint32_t, 16>& output) noexcept {
    std::array<std::uint32_t, 16> x = input;
    for (int round = 0; round < double_rounds; ++round) {
        quarter_round(x, 0, 4, 8, 12);
        quarter_round(x, 1, 5, 9, 13);
        quarter_round(x, 2, 6, 10, 14);
        quarter_round(x, 3, 7, 11, 15);
        quarter_round(x, 0, 5, 10, 15);
        quarter_round(x, 1, 6, 11, 12);
        quarter_round(x, 2, 7, 8, 13);
        quarter_round(x, 3, 4, 9, 14);
    }
    for (std::size_t index = 0; index < 16; ++index) output[index] = x[index] + input[index];
}

namespace {

// One ChaCha word across four blocks: lane i belongs to block i. GCC and
// Clang get their generic 16-byte vector type, which they lower to one
// 128-bit register (NEON, SSE2) on every target; it is a compiler feature,
// not a platform intrinsic. Other compilers use the array form, which
// computes the same values.
#if defined(__GNUC__) || defined(__clang__)
#define STX_AUXLIB_CHACHA_VECTORS 1
using Lanes = std::uint32_t __attribute__((vector_size(16)));

STX_AUXLIB_MODULAR void quarter_round_lanes(std::array<Lanes, 16>& x, const std::size_t a,
                                            const std::size_t b, const std::size_t c,
                                            const std::size_t d) noexcept {
    x[a] += x[b];
    x[d] ^= x[a];
    x[d] = (x[d] << 16) | (x[d] >> 16);
    x[c] += x[d];
    x[b] ^= x[c];
    x[b] = (x[b] << 12) | (x[b] >> 20);
    x[a] += x[b];
    x[d] ^= x[a];
    x[d] = (x[d] << 8) | (x[d] >> 24);
    x[c] += x[d];
    x[b] ^= x[c];
    x[b] = (x[b] << 7) | (x[b] >> 25);
}

Lanes broadcast(const std::uint32_t value) noexcept {
    const Lanes result = {value, value, value, value};
    return result;
}

void set_lane(Lanes& lanes, const std::size_t lane, const std::uint32_t value) noexcept {
    lanes[lane] = value;
}

STX_AUXLIB_MODULAR void add_lanes(Lanes& target, const Lanes& source) noexcept { target += source; }
#else
using Lanes = std::array<std::uint32_t, 4>;

STX_AUXLIB_MODULAR void add_lanes(Lanes& target, const Lanes& source) noexcept {
    for (std::size_t lane = 0; lane < 4; ++lane) target[lane] += source[lane];
}

void xor_rotate_lanes(Lanes& target, const Lanes& source, const int count) noexcept {
    for (std::size_t lane = 0; lane < 4; ++lane) target[lane] = std::rotl(target[lane] ^ source[lane], count);
}

void quarter_round_lanes(std::array<Lanes, 16>& x, const std::size_t a, const std::size_t b,
                         const std::size_t c, const std::size_t d) noexcept {
    add_lanes(x[a], x[b]);
    xor_rotate_lanes(x[d], x[a], 16);
    add_lanes(x[c], x[d]);
    xor_rotate_lanes(x[b], x[c], 12);
    add_lanes(x[a], x[b]);
    xor_rotate_lanes(x[d], x[a], 8);
    add_lanes(x[c], x[d]);
    xor_rotate_lanes(x[b], x[c], 7);
}

Lanes broadcast(const std::uint32_t value) noexcept {
    const Lanes result = {value, value, value, value};
    return result;
}

void set_lane(Lanes& lanes, const std::size_t lane, const std::uint32_t value) noexcept {
    lanes[lane] = value;
}
#endif

// Four blocks of the same key and stream at counters c to c + 3, block i
// in output[16 i .. 16 i + 15]; each equals detail::chacha_block at its
// counter.
STX_AUXLIB_MODULAR void chacha_four_blocks(const std::array<std::uint32_t, 16>& input,
                                           const int double_rounds,
                                           std::array<std::uint32_t, 64>& output) noexcept {
    // The counters' two words for the four blocks.
    const std::uint64_t counter = input[12] | (static_cast<std::uint64_t>(input[13]) << 32);
    Lanes counter_low = broadcast(0);
    Lanes counter_high = broadcast(0);
    for (std::size_t lane = 0; lane < 4; ++lane) {
        const std::uint64_t lane_counter = counter + lane;
        set_lane(counter_low, lane, static_cast<std::uint32_t>(lane_counter));
        set_lane(counter_high, lane, static_cast<std::uint32_t>(lane_counter >> 32));
    }
    std::array<Lanes, 16> x{};
    for (std::size_t word = 0; word < 16; ++word) x[word] = broadcast(input[word]);
    x[12] = counter_low;
    x[13] = counter_high;
    // The starting state is rebuilt for the final addition rather than kept:
    // sixteen more live vectors would spill the working state.
    for (int round = 0; round < double_rounds; ++round) {
        quarter_round_lanes(x, 0, 4, 8, 12);
        quarter_round_lanes(x, 1, 5, 9, 13);
        quarter_round_lanes(x, 2, 6, 10, 14);
        quarter_round_lanes(x, 3, 7, 11, 15);
        quarter_round_lanes(x, 0, 5, 10, 15);
        quarter_round_lanes(x, 1, 6, 11, 12);
        quarter_round_lanes(x, 2, 7, 8, 13);
        quarter_round_lanes(x, 3, 4, 9, 14);
    }
    // Word-major vectors to block-major output.
    std::array<std::uint32_t, 64> word_major{};
    for (std::size_t word = 0; word < 16; ++word) {
        Lanes start = broadcast(input[word]);
        if (word == 12) start = counter_low;
        if (word == 13) start = counter_high;
        add_lanes(x[word], start);
        std::memcpy(word_major.data() + word * 4, &x[word], sizeof(Lanes));
    }
    for (std::size_t block = 0; block < 4; ++block) {
        for (std::size_t word = 0; word < 16; ++word) output[block * 16 + word] = word_major[word * 4 + block];
    }
}

} // namespace

detail::Product detail::multiply(const std::uint64_t left, const std::uint64_t right) noexcept {
    const std::uint64_t left_low = left & 0xffffffffULL;
    const std::uint64_t left_high = left >> 32;
    const std::uint64_t right_low = right & 0xffffffffULL;
    const std::uint64_t right_high = right >> 32;
    const std::uint64_t low_low = left_low * right_low;
    const std::uint64_t high_low = left_high * right_low;
    const std::uint64_t low_high = left_low * right_high;
    const std::uint64_t high_high = left_high * right_high;
    // The middle partial sums, carried into the high half.
    const std::uint64_t middle = (low_low >> 32) + (high_low & 0xffffffffULL) + low_high;
    Product result{};
    result.high = high_high + (high_low >> 32) + (middle >> 32);
    result.low = ((middle & 0xffffffffULL) << 32) | (low_low & 0xffffffffULL);
    return result;
}

std::uint64_t detail::distance(const std::int64_t low, const std::int64_t high) noexcept {
    // Same signs: the signed difference cannot overflow. Opposite signs:
    // high + |low|, with |low| = -(low + 1) + 1 so INT64_MIN has one.
    if (low >= 0 || high < 0) return static_cast<std::uint64_t>(high - low);
    const std::uint64_t below_zero = static_cast<std::uint64_t>(-(low + 1)) + 1;
    const std::uint64_t result = static_cast<std::uint64_t>(high) + below_zero;
    return result;
}

std::int64_t detail::advance(const std::int64_t low, const std::uint64_t offset) noexcept {
    if (low >= 0) return low + static_cast<std::int64_t>(offset);
    // Steps that stay below zero, then the rest from zero upwards.
    const std::uint64_t to_minus_one = static_cast<std::uint64_t>(-(low + 1));
    if (offset <= to_minus_one) return low + static_cast<std::int64_t>(offset);
    const std::int64_t result = static_cast<std::int64_t>(offset - to_minus_one - 1);
    return result;
}

double detail::scale_into(const double low, const double high, const double unit) noexcept {
    if (!(low < high) || !std::isfinite(low) || !std::isfinite(high)) return low;
    const double width = high - low;
    double result = 0.0;
    if (std::isfinite(width)) {
        result = low + width * unit;
    } else {
        // The width overflows only for bounds of opposite sign near the
        // largest double; interpolate without forming it.
        result = low * (1.0 - unit) + high * unit;
    }
    if (result >= high) result = std::nextafter(high, low);
    if (result < low) result = low;
    return result;
}

SeedResult::SeedResult(State state) noexcept : state_(std::move(state)) {}

SeedResult SeedResult::seeded(const Seed& seed) noexcept {
    SeedResult result(State(std::in_place_index<0>, seed));
    return result;
}

SeedResult SeedResult::failed(const EntropyFailure failure) noexcept {
    SeedResult result(State(std::in_place_index<1>, failure));
    return result;
}

bool SeedResult::succeeded() const noexcept {
    const bool result = std::holds_alternative<Seed>(state_);
    return result;
}

const Seed* SeedResult::seed() const noexcept {
    const Seed* const result = std::get_if<Seed>(&state_);
    return result;
}

SeedResult entropy_seed() noexcept {
    std::array<std::byte, 32> bytes{};
    if (!fill_entropy(bytes)) return SeedResult::failed(EntropyFailure::unavailable);
    Seed seed{};
    for (std::size_t word = 0; word < 4; ++word) {
        std::uint64_t value = 0;
        for (std::size_t byte = 0; byte < 8; ++byte) {
            const std::uint64_t part = std::to_integer<std::uint64_t>(bytes[word * 8 + byte]);
            value |= part << (8 * byte);
        }
        seed.words[word] = value;
    }
    return SeedResult::seeded(seed);
}

Seed seed_from_number(const std::uint64_t number) noexcept {
    std::uint64_t state = number;
    Seed seed{};
    for (std::uint64_t& word : seed.words) word = splitmix_step(state);
    return seed;
}

ChaCha12Stream::ChaCha12Stream(const Seed& seed, const std::uint64_t stream) noexcept {
    for (std::size_t index = 0; index < 4; ++index) input_[index] = chacha_constants[index];
    for (std::size_t word = 0; word < 4; ++word) {
        input_[4 + 2 * word] = static_cast<std::uint32_t>(seed.words[word]);
        input_[5 + 2 * word] = static_cast<std::uint32_t>(seed.words[word] >> 32);
    }
    input_[12] = 0;
    input_[13] = 0;
    input_[14] = static_cast<std::uint32_t>(stream);
    input_[15] = static_cast<std::uint32_t>(stream >> 32);
}

// Computes the four blocks for counters c to c + 3, then advances the
// 64-bit counter in words 12 and 13 by four.
STX_AUXLIB_MODULAR void ChaCha12Stream::refill() noexcept {
    chacha_four_blocks(input_, chacha12_double_rounds, blocks_);
    std::uint64_t counter = input_[12] | (static_cast<std::uint64_t>(input_[13]) << 32);
    counter += 4;
    input_[12] = static_cast<std::uint32_t>(counter);
    input_[13] = static_cast<std::uint32_t>(counter >> 32);
    position_ = 0;
}

void ChaCha12Stream::fill(const std::span<std::byte> output) noexcept {
    std::size_t filled = 0;
    while (filled < output.size()) {
        const std::uint64_t value = next();
        const std::size_t remaining = output.size() - filled;
        const std::size_t count = remaining < 8 ? remaining : 8;
        for (std::size_t byte = 0; byte < count; ++byte)
            output[filled + byte] = static_cast<std::byte>(value >> (8 * byte));
        filled += count;
    }
}

Xoshiro256PlusPlus::Xoshiro256PlusPlus(const Seed& seed) noexcept : state_(seed.words) {
    const bool all_zero = state_[0] == 0 && state_[1] == 0 && state_[2] == 0 && state_[3] == 0;
    if (all_zero) state_ = seed_from_number(0).words;
}

STX_AUXLIB_MODULAR std::uint64_t Xoshiro256PlusPlus::next() noexcept {
    const std::uint64_t result = std::rotl(state_[0] + state_[3], 23) + state_[0];
    const std::uint64_t shifted = state_[1] << 17;
    state_[2] ^= state_[0];
    state_[3] ^= state_[1];
    state_[1] ^= state_[2];
    state_[0] ^= state_[3];
    state_[2] ^= shifted;
    state_[3] = std::rotl(state_[3], 45);
    return result;
}

namespace {

// Applies a jump polynomial: the state becomes the XOR of the states at the
// polynomial's set bits.
void apply_jump(Xoshiro256PlusPlus& generator, std::array<std::uint64_t, 4>& state,
                const std::array<std::uint64_t, 4>& table) noexcept {
    std::array<std::uint64_t, 4> accumulated{};
    for (const std::uint64_t word : table) {
        for (unsigned bit = 0; bit < 64; ++bit) {
            if ((word & (std::uint64_t{1} << bit)) != 0) {
                for (std::size_t index = 0; index < 4; ++index) accumulated[index] ^= state[index];
            }
            (void)generator.next();
        }
    }
    state = accumulated;
}

} // namespace

void Xoshiro256PlusPlus::jump() noexcept { apply_jump(*this, state_, jump_table); }

void Xoshiro256PlusPlus::long_jump() noexcept { apply_jump(*this, state_, long_jump_table); }

} // namespace stx_auxlib::random
