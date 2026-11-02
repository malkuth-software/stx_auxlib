// The search behind step 1 of tools/sincos_certificate.py: for each job,
// the j in [j_low, j_high] whose fixed-point value (j F + G) mod 2^128 lies
// nearest to an integer, that is nearest to 0 on the circle of 2^128 units.
// F and G are frac(Theta) and frac(gamma) in units of 2^-128; the
// certificate accounts for their truncation and checks the answer exactly.
//
// Baby steps and giant steps over an ordered index (stx_btree's map): with
// B = ceil(sqrt(N)) for N = j_high - j_low + 1, every j in range is
// j_low + i B + t with 0 <= t < B. The map holds T_t = t F mod 2^128 for
// every t < B, keyed by its 16 big-endian bytes, so its order is the
// circle's order cut at 0. For a full block i the value of j is
// T_t + G_i mod 2^128 with G_i = (j_low + i B) F + G; it is nearest to 0
// where T_t is nearest to -G_i on the circle, and the nearest point on a
// circle to a target is its successor or its predecessor in cyclic order:
// lower_bound(-G_i) and the entry before it, wrapping past either end.
// The last, partial block is evaluated directly. Every j in range is
// therefore either an index lookup's cyclic neighbour candidate or
// evaluated, and the minimum is exact over the range: cost B inserts and
// N/B lookups, O(sqrt(N) log N).
//
// Input, one job per line: j_low j_high F G (decimal, decimal, 32 hex
// digits, 32 hex digits). Output, one line per job: j distance (decimal,
// 32 hex digits, the distance in units of 2^-128). Exit 1 on malformed
// input or a repeated key (two t with equal T_t would hide one of them).

#include "stx_btree/unsync_map.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>

namespace {

__extension__ typedef unsigned __int128 Fixed;   // a point of the circle, units of 2^-128
using Key = std::array<std::uint8_t, 16>;
using Index = stx_btree::unsync::BBTreeMap<Key, std::uint64_t>;

Key key_of(const Fixed value) {
    Key key{};
    for (int byte = 0; byte < 16; ++byte) {
        key[static_cast<std::size_t>(byte)] = static_cast<std::uint8_t>(value >> (8 * (15 - byte)));
    }
    return key;
}

Fixed value_of(const Key& key) {
    Fixed value = 0;
    for (int byte = 0; byte < 16; ++byte) {
        value = (value << 8) | key[static_cast<std::size_t>(byte)];
    }
    return value;
}

// Distance from 0 on the circle.
Fixed distance(const Fixed value) {
    const Fixed other = static_cast<Fixed>(0) - value;
    const Fixed result = value < other ? value : other;
    return result;
}

bool parse_hex(const char* const text, Fixed& value) {
    if (std::strlen(text) != 32) return false;
    value = 0;
    for (int digit = 0; digit < 32; ++digit) {
        const char character = text[digit];
        unsigned int nibble = 0;
        if (character >= '0' && character <= '9') {
            nibble = static_cast<unsigned int>(character - '0');
        } else if (character >= 'a' && character <= 'f') {
            nibble = static_cast<unsigned int>(character - 'a' + 10);
        } else {
            return false;
        }
        value = (value << 4) | nibble;
    }
    return true;
}

void print_hex(const Fixed value) {
    const std::uint64_t high = static_cast<std::uint64_t>(value >> 64);
    const std::uint64_t low = static_cast<std::uint64_t>(value);
    std::printf("%016llx%016llx", static_cast<unsigned long long>(high), static_cast<unsigned long long>(low));
}

struct Best final {
    Fixed distance{0};
    std::uint64_t j{0};
    bool found{false};
};

void consider(Best& best, const Fixed value, const std::uint64_t j) {
    const Fixed d = distance(value);
    if (!best.found || d < best.distance) {
        best.distance = d;
        best.j = j;
        best.found = true;
    }
}

// The square root rounded up, for n >= 1.
std::uint64_t ceiling_root(const std::uint64_t n) {
    std::uint64_t root = 1;
    while (root * root < n) root *= 2;
    std::uint64_t low = root / 2;
    std::uint64_t high = root;
    while (low + 1 < high) {
        const std::uint64_t middle = low + (high - low) / 2;
        if (middle * middle >= n) {
            high = middle;
        } else {
            low = middle;
        }
    }
    return high;
}

bool search(const std::uint64_t j_low, const std::uint64_t j_high, const Fixed f, const Fixed g, Best& best) {
    const std::uint64_t count = j_high - j_low + 1;
    const std::uint64_t block = ceiling_root(count);
    Index index;
    for (std::uint64_t t = 0; t < block; ++t) {
        const Fixed baby = static_cast<Fixed>(t) * f;
        const std::optional<std::uint64_t> replaced = index.insert(key_of(baby), t);
        if (replaced.has_value()) return false;
    }
    const Index::Iterator first = index.begin();
    const Index::Iterator end = index.end();
    for (std::uint64_t base = j_low; base <= j_high; base += block) {
        const Fixed giant = static_cast<Fixed>(base) * f + g;
        if (j_high - base + 1 < block) {
            for (std::uint64_t t = 0; base + t <= j_high; ++t) {
                consider(best, static_cast<Fixed>(t) * f + giant, base + t);
            }
            break;
        }
        const Fixed target = static_cast<Fixed>(0) - giant;
        Index::Iterator successor = index.lower_bound(key_of(target));
        if (successor == end) successor = first;  // wraps past 2^128
        // The entry before the successor; before the first, the last
        // (the end iterator steps back to the last entry).
        Index::Iterator predecessor = successor == first ? end : successor;
        --predecessor;
        const Index::Iterator candidates[2] = {successor, predecessor};
        for (const Index::Iterator& candidate : candidates) {
            const std::uint64_t t = (*candidate).value;
            consider(best, value_of((*candidate).key) + giant, base + t);
        }
    }
    return true;
}

} // namespace

int main() {
    unsigned long long j_low = 0;
    unsigned long long j_high = 0;
    char f_text[40] = {};
    char g_text[40] = {};
    while (std::scanf("%llu %llu %39s %39s", &j_low, &j_high, f_text, g_text) == 4) {
        Fixed f = 0;
        Fixed g = 0;
        if (j_high < j_low || !parse_hex(f_text, f) || !parse_hex(g_text, g)) {
            std::fprintf(stderr, "sincos_minimum_remainder: malformed job\n");
            return 1;
        }
        Best best;
        if (!search(j_low, j_high, f, g, best)) {
            std::fprintf(stderr, "sincos_minimum_remainder: repeated baby-step key\n");
            return 1;
        }
        std::printf("%llu ", static_cast<unsigned long long>(best.j));
        print_hex(best.distance);
        std::printf("\n");
    }
    return 0;
}
