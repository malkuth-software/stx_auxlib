#pragma once

// Randomness with named, frozen algorithms, replacing rand, srand,
// std::random_device, the standard engines and distributions, and
// std::shuffle (owner decisions of 2026-10-31).
//
// Reproducibility: a Seed and a generator name fix the whole sequence on
// every host and toolchain, forever. ChaCha12Stream is ChaCha with 12
// rounds over a 256-bit key; Xoshiro256PlusPlus is Blackman and Vigna's
// xoshiro256++ 1.0; seed_from_number expands a 64-bit number with
// SplitMix64. The distributions below are specified here, not by a
// standard library (whose distributions differ between libc++, libstdc++
// and MSVC for the same engine and seed). Changing any of them would break
// stored seeds; a new algorithm gets a new name instead.
//
// Choosing a generator: ChaCha12Stream is the default. Its output cannot be
// predicted from earlier output, so it is safe for anything (shuffles a
// server trusts, temporary names, tokens). Xoshiro256PlusPlus is faster and
// statistically excellent but predictable from its output: use it by name
// for hot loops where speed matters and security never will (particles,
// noise).
//
// Entropy fails closed: when the host has no entropy source, a request for
// an unpredictable seed returns a named failure, never a weak seed.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <variant>

namespace stx_auxlib::random {

// 256 bits that determine a generator's whole sequence.
struct Seed final {
    std::array<std::uint64_t, 4> words{};
};

enum class EntropyFailure : std::uint8_t {
    // The host offers no entropy source, or it failed.
    unavailable = 1,
};

// Fills `output` with unpredictable bytes from the host's entropy source
// (getentropy on POSIX hosts, BCryptGenRandom on Windows). On failure the
// bytes are zeroed and the call reports it; it never substitutes a weaker
// source.
[[nodiscard]] bool fill_entropy(std::span<std::byte> output) noexcept;

// The result of entropy_seed: a seed, or the failure. Never both.
class SeedResult final {
public:
    [[nodiscard]] static SeedResult seeded(const Seed& seed) noexcept;
    [[nodiscard]] static SeedResult failed(EntropyFailure failure) noexcept;
    [[nodiscard]] bool succeeded() const noexcept;
    // The seed; null on failure.
    [[nodiscard]] const Seed* seed() const noexcept;

private:
    using State = std::variant<Seed, EntropyFailure>;
    explicit SeedResult(State state) noexcept;
    State state_;
};

// An unpredictable seed from the host's entropy source.
[[nodiscard]] SeedResult entropy_seed() noexcept;

// A seed fixed by one number (a deal number, a level seed, a test case):
// four successive SplitMix64 outputs from state `number`.
[[nodiscard]] Seed seed_from_number(std::uint64_t number) noexcept;

// ChaCha with 12 rounds. Key: the seed's words, each little-endian into
// two key words. A stream is 2^64 blocks (2^70 bytes) long and repeats
// after that, as ChaCha's 64-bit block counter does. Block layout as in Bernstein's original ChaCha: words 12
// and 13 a 64-bit block counter starting at 0, words 14 and 15 the 64-bit
// `stream`, so one seed gives 2^64 independent streams. Each next() takes
// the next two output words of the block, the first as the low half.
class ChaCha12Stream final {
public:
    static constexpr bool is_stx_auxlib_generator = true;

    explicit ChaCha12Stream(const Seed& seed, std::uint64_t stream = 0) noexcept;
    // Inline: one bounds test and two loads; refill runs once per 32
    // outputs.
    [[nodiscard]] std::uint64_t next() noexcept {
        if (position_ == 64) refill();
        const std::uint64_t low = blocks_[position_];
        const std::uint64_t high = blocks_[position_ + 1];
        position_ += 2;
        const std::uint64_t result = low | (high << 32);
        return result;
    }
    // Fills `output` with the stream's next bytes (little-endian words).
    // Whole 64-bit outputs are consumed; a trailing partial output's unused
    // bytes are discarded.
    void fill(std::span<std::byte> output) noexcept;

private:
    void refill() noexcept;
    std::array<std::uint32_t, 16> input_{};
    // Four consecutive blocks in order, computed together in 128-bit lane
    // groups.
    std::array<std::uint32_t, 64> blocks_{};
    // The next unused word of blocks_; 64 when they are used up.
    std::size_t position_{64};
};

// xoshiro256++ 1.0. The state is the seed's words; an all-zero seed, which
// the algorithm cannot leave, is replaced by seed_from_number(0).
class Xoshiro256PlusPlus final {
public:
    static constexpr bool is_stx_auxlib_generator = true;

    explicit Xoshiro256PlusPlus(const Seed& seed) noexcept;
    [[nodiscard]] std::uint64_t next() noexcept;
    // Advances by 2^128 outputs: up to 2^128 non-overlapping subsequences
    // for parallel work.
    void jump() noexcept;
    // Advances by 2^192 outputs.
    void long_jump() noexcept;

private:
    std::array<std::uint64_t, 4> state_{};
};

// Explicit opt-in: a type is a generator only if it declares
// `is_stx_auxlib_generator`, never because it happens to have next().
template <typename Generator>
concept RandomGenerator = Generator::is_stx_auxlib_generator;

namespace detail {
// One ChaCha block: `double_rounds` column-and-diagonal round pairs over
// `input`, plus `input` (ChaCha12 uses 6, ChaCha20 10). Exposed for the
// known-answer tests.
void chacha_block(const std::array<std::uint32_t, 16>& input, int double_rounds,
                  std::array<std::uint32_t, 16>& output) noexcept;

// The high and low halves of a 64 x 64 bit product, without a 128-bit type.
struct Product final {
    std::uint64_t high{0};
    std::uint64_t low{0};
};
[[nodiscard]] Product multiply(std::uint64_t left, std::uint64_t right) noexcept;
// high - low as an unsigned count, for low <= high, without wrapping.
[[nodiscard]] std::uint64_t distance(std::int64_t low, std::int64_t high) noexcept;
// low + offset, for offset <= distance(low, high), without wrapping.
[[nodiscard]] std::int64_t advance(std::int64_t low, std::uint64_t offset) noexcept;
} // namespace detail

// A uniformly distributed integer in [0, bound), by Lemire's
// nearly-divisionless method (2019), drawing one output, or more when it
// rejects. `bound` must be positive; a zero bound yields 0 without drawing.
template <RandomGenerator Generator>
[[nodiscard]] std::uint64_t uniform_below(Generator& generator, const std::uint64_t bound) noexcept {
    if (bound == 0) return 0;
    detail::Product product = detail::multiply(generator.next(), bound);
    if (product.low < bound) {
        // 2^64 mod bound, as (2^64 - bound) mod bound: UINT64_MAX - bound + 1
        // cannot wrap for bound >= 1.
        const std::uint64_t threshold = (UINT64_MAX - bound + 1) % bound;
        while (product.low < threshold) product = detail::multiply(generator.next(), bound);
    }
    return product.high;
}

// A uniformly distributed integer in [low, high], inclusive; `low` must not
// exceed `high` (when it does, the result is `low`).
template <RandomGenerator Generator>
[[nodiscard]] std::int64_t uniform_between(Generator& generator, const std::int64_t low,
                                           const std::int64_t high) noexcept {
    if (high <= low) return low;
    const std::uint64_t span = detail::distance(low, high);
    std::uint64_t offset = 0;
    if (span == UINT64_MAX) {
        offset = generator.next();
    } else {
        offset = uniform_below(generator, span + 1);
    }
    const std::int64_t result = detail::advance(low, offset);
    return result;
}

// A double in [0, 1): the output's top 53 bits times 2^-53, so every
// result is a multiple of 2^-53.
template <RandomGenerator Generator>
[[nodiscard]] double uniform_unit(Generator& generator) noexcept {
    const std::uint64_t bits = generator.next() >> 11;
    const double result = static_cast<double>(bits) * 0x1.0p-53;
    return result;
}

// A float in [0, 1): the output's top 24 bits times 2^-24.
template <RandomGenerator Generator>
[[nodiscard]] float uniform_unit_float(Generator& generator) noexcept {
    const std::uint64_t bits = generator.next() >> 40;
    const float result = static_cast<float>(bits) * 0x1.0p-24f;
    return result;
}

// A double in [low, high): low + (high - low) * uniform_unit, with a result
// that rounds up to `high` moved to the largest double below it. Requires
// finite low < high (otherwise the result is `low`).
template <RandomGenerator Generator>
[[nodiscard]] double uniform_real(Generator& generator, const double low, const double high) noexcept;

// True with probability `probability`, compared against uniform_unit;
// probabilities at or below 0 never, at or above 1 always.
template <RandomGenerator Generator>
[[nodiscard]] bool chance(Generator& generator, const double probability) noexcept {
    const bool result = uniform_unit(generator) < probability;
    return result;
}

// Fisher–Yates, from the last element down: for i from size - 1 to 1, swap
// element i with element uniform_below(i + 1).
template <RandomGenerator Generator, typename Element>
void shuffle(Generator& generator, const std::span<Element> elements) {
    for (std::size_t index = elements.size(); index > 1; --index) {
        const std::size_t last = index - 1;
        const std::size_t other = static_cast<std::size_t>(uniform_below(generator, index));
        std::swap(elements[last], elements[other]);
    }
}

namespace detail {
[[nodiscard]] double scale_into(double low, double high, double unit) noexcept;
} // namespace detail

template <RandomGenerator Generator>
double uniform_real(Generator& generator, const double low, const double high) noexcept {
    const double unit = uniform_unit(generator);
    const double result = detail::scale_into(low, high, unit);
    return result;
}

} // namespace stx_auxlib::random
