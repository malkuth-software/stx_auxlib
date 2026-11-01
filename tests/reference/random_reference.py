#!/usr/bin/env python3
"""Independent reference for stx_auxlib::random (include/stx_auxlib/random.hpp).

Prints the known-answer values tests/random_tests.cpp checks: the RFC 8439
section 2.3.2 ChaCha20 block (a check of this script's ChaCha core), then
ChaCha12Stream, Xoshiro256PlusPlus (with jump and long_jump),
seed_from_number, uniform_below (Lemire) and shuffle (Fisher-Yates from the
last element down) outputs. Written from the published algorithms, apart
from the C++ implementation.
"""

MASK64 = (1 << 64) - 1
MASK32 = (1 << 32) - 1


def splitmix64(state):
    state = (state + 0x9E3779B97F4A7C15) & MASK64
    z = state
    z = ((z ^ (z >> 30)) * 0xBF58476D1CE4E5B9) & MASK64
    z = ((z ^ (z >> 27)) * 0x94D049BB133111EB) & MASK64
    return state, z ^ (z >> 31)


def seed_from_number(number):
    words = []
    state = number
    for _ in range(4):
        state, value = splitmix64(state)
        words.append(value)
    return words


def rotl32(value, count):
    return ((value << count) | (value >> (32 - count))) & MASK32


def quarter(x, a, b, c, d):
    x[a] = (x[a] + x[b]) & MASK32; x[d] = rotl32(x[d] ^ x[a], 16)
    x[c] = (x[c] + x[d]) & MASK32; x[b] = rotl32(x[b] ^ x[c], 12)
    x[a] = (x[a] + x[b]) & MASK32; x[d] = rotl32(x[d] ^ x[a], 8)
    x[c] = (x[c] + x[d]) & MASK32; x[b] = rotl32(x[b] ^ x[c], 7)


def chacha_block(state, double_rounds):
    x = list(state)
    for _ in range(double_rounds):
        quarter(x, 0, 4, 8, 12); quarter(x, 1, 5, 9, 13)
        quarter(x, 2, 6, 10, 14); quarter(x, 3, 7, 11, 15)
        quarter(x, 0, 5, 10, 15); quarter(x, 1, 6, 11, 12)
        quarter(x, 2, 7, 8, 13); quarter(x, 3, 4, 9, 14)
    return [(x[i] + state[i]) & MASK32 for i in range(16)]


CONSTANTS = [0x61707865, 0x3320646E, 0x79622D32, 0x6B206574]


def chacha12_outputs(seed, stream, count):
    key = []
    for word in seed:
        key += [word & MASK32, word >> 32]
    outputs = []
    counter = 0
    while len(outputs) < count:
        state = CONSTANTS + key + [counter & MASK32, counter >> 32,
                                   stream & MASK32, stream >> 32]
        block = chacha_block(state, 6)
        for i in range(0, 16, 2):
            outputs.append(block[i] | (block[i + 1] << 32))
        counter += 1
    return outputs[:count]


def rotl64(value, count):
    return ((value << count) | (value >> (64 - count))) & MASK64


class Xoshiro:
    def __init__(self, words):
        self.s = list(words)

    def next(self):
        s = self.s
        result = (rotl64((s[0] + s[3]) & MASK64, 23) + s[0]) & MASK64
        t = (s[1] << 17) & MASK64
        s[2] ^= s[0]; s[3] ^= s[1]; s[1] ^= s[2]; s[0] ^= s[3]
        s[2] ^= t; s[3] = rotl64(s[3], 45)
        return result

    def apply_jump(self, table):
        acc = [0, 0, 0, 0]
        for word in table:
            for bit in range(64):
                if word & (1 << bit):
                    acc = [acc[i] ^ self.s[i] for i in range(4)]
                self.next()
        self.s = acc


JUMP = [0x180EC6D33CFD0ABA, 0xD5A61266F0C9392C, 0xA9582618E03FC9AA, 0x39ABDC4529B1661C]
LONG_JUMP = [0x76E15D3EFEFDCBBF, 0xC5004E441C522FB3, 0x77710069854EE241, 0x39109BB02ACBE635]


def uniform_below(next_value, bound):
    product = next_value() * bound
    low = product & MASK64
    if low < bound:
        threshold = ((1 << 64) - bound) % bound
        while low < threshold:
            product = next_value() * bound
            low = product & MASK64
    return product >> 64


def hexes(values):
    return ", ".join("0x%016xULL" % value for value in values)


def main():
    # RFC 8439 2.3.2: key 00..1f, block counter 1, nonce 00 00 00 09 00 00
    # 00 4a 00 00 00 00 (little-endian words 0x09000000, 0x4a000000, 0).
    key = [int.from_bytes(bytes(range(i, i + 4)), "little") for i in range(0, 32, 4)]
    state = CONSTANTS + key + [1, 0x09000000, 0x4A000000, 0]
    print("rfc8439_block:", ", ".join("0x%08x" % w for w in chacha_block(state, 10)))
    seed = seed_from_number(42)
    print("seed_from_number(42):", hexes(seed))
    print("chacha12(seed 42, stream 0) first 8:", hexes(chacha12_outputs(seed, 0, 8)))
    print("chacha12(seed 42, stream 7) first 4:", hexes(chacha12_outputs(seed, 7, 4)))
    long_run = chacha12_outputs(seed, 0, 1000)
    print("chacha12(seed 42) output 999:", hexes([long_run[999]]))
    generator = Xoshiro(seed)
    print("xoshiro(seed 42) first 8:", hexes([generator.next() for _ in range(8)]))
    jumped = Xoshiro(seed); jumped.apply_jump(JUMP)
    print("xoshiro(seed 42) after jump:", hexes([jumped.next() for _ in range(2)]))
    far = Xoshiro(seed); far.apply_jump(LONG_JUMP)
    print("xoshiro(seed 42) after long_jump:", hexes([far.next() for _ in range(2)]))
    stream = iter(chacha12_outputs(seed, 0, 64))
    dice = [uniform_below(lambda: next(stream), 6) for _ in range(10)]
    print("uniform_below(chacha12 seed 42, 6) x10:", dice)
    stream = iter(chacha12_outputs(seed, 0, 64))
    deck = list(range(10))
    for index in range(len(deck), 1, -1):
        other = uniform_below(lambda: next(stream), index)
        deck[index - 1], deck[other] = deck[other], deck[index - 1]
    print("shuffle(chacha12 seed 42, 0..9):", deck)


if __name__ == "__main__":
    main()
