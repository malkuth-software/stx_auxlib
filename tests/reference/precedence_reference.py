#!/usr/bin/env python3
"""Independent reference for stx_auxlib's precedence chain.

Implements src/precedence.cpp's construction from its specification with
Python's hashlib.blake2s, and prints the known-answer values
tests/precedence_tests.cpp checks: BLAKE2s vectors, then for a chain made
from fixed seeds the anchor's first key root and the first epoch's leaf,
signature ends and key path.
"""

import hashlib
import struct

EPOCHS_PER_BLOCK = 256
HEIGHT = 8
CHAINS = 67
W1 = 15


def H(tag, *fields):
    hasher = hashlib.blake2s(digest_size=32)
    hasher.update(tag.encode("ascii") + b"\0")
    for field in fields:
        hasher.update(field)
    return hasher.digest()


def u32(value):
    return struct.pack("<I", value)


def u64(value):
    return struct.pack("<Q", value & 0xFFFFFFFFFFFFFFFF)


def ratchet(seed):
    return H("stx.precedence.ratchet", seed)


def wots_secret(seed, chain):
    return H("stx.precedence.wots.secret", seed, u32(chain))


def walk(public, epoch, chain, value, start, steps):
    for step in range(start, start + steps):
        value = H("stx.precedence.wots.chain", public, u64(epoch), u32(chain), u32(step), value)
    return value


def digits_of(message):
    digits = []
    for byte in message:
        digits += [byte >> 4, byte & 15]
    checksum = sum(W1 - d for d in digits)
    digits += [(checksum >> 8) & 15, (checksum >> 4) & 15, checksum & 15]
    return digits


def public_key(public, epoch, seed):
    ends = [walk(public, epoch, chain, wots_secret(seed, chain), 0, W1) for chain in range(CHAINS)]
    return H("stx.precedence.wots.public", public, u64(epoch), *ends)


def sign(public, epoch, seed, message):
    digits = digits_of(message)
    return [walk(public, epoch, chain, wots_secret(seed, chain), 0, digits[chain]) for chain in range(CHAINS)]


def key_leaf(public, epoch, key):
    return H("stx.precedence.key", public, u64(epoch), key)


def node(public, block, height, index, left, right):
    return H("stx.precedence.node", public, u64(block), u32(height), u64(index), left, right)


def tree(public, block, keys):
    levels = [[key_leaf(public, block * EPOCHS_PER_BLOCK + slot, keys[slot]) for slot in range(EPOCHS_PER_BLOCK)]]
    for height in range(1, HEIGHT + 1):
        below = levels[-1]
        levels.append([node(public, block, height, index, below[2 * index], below[2 * index + 1])
                       for index in range(len(below) // 2)])
    return levels


def derive_keys(public, first_seed, first_epoch):
    keys = []
    seed = first_seed
    for slot in range(EPOCHS_PER_BLOCK):
        keys.append(public_key(public, first_epoch + slot, seed))
        seed = ratchet(seed)
    return keys


def infos_digest(infos):
    digest = bytes(32)
    for info in infos:
        digest = H("stx.precedence.infos", digest, info)
    return digest


def leaf(public, epoch, time_ns, infos, previous, chain_digest, next_root):
    return H("stx.precedence.epoch", public, u64(epoch), u64(time_ns), u32(len(infos)), infos_digest(infos),
             previous, chain_digest, next_root)


def main():
    print("blake2s(''):", hashlib.blake2s(b"").hexdigest())
    print("blake2s('abc'):", hashlib.blake2s(b"abc").hexdigest())
    key = bytes(range(32))
    print("blake2s_keyed(00..1f, ''):", hashlib.blake2s(b"", key=key).hexdigest())
    print("blake2s_keyed(00..1f, 00..ff):", hashlib.blake2s(bytes(range(256)), key=key).hexdigest())
    print("blake2s(200 x 'a'):", hashlib.blake2s(b"a" * 200).hexdigest())

    secret = bytes(range(32))
    public = bytes(range(32, 64))
    genesis = 1793482205000000000
    keys = derive_keys(public, secret, 0)
    levels = tree(public, 0, keys)
    first_root = levels[HEIGHT][0]
    print("first_key_root:", first_root.hex())
    chain_digest = H("stx.precedence.genesis", public, first_root, u64(genesis))
    info = hashlib.blake2s(b"alpha").digest()
    when = genesis + 1000000000
    first_leaf = leaf(public, 0, when, [info], bytes(32), chain_digest, bytes(32))
    print("leaf_0:", first_leaf.hex())
    signature = sign(public, 0, secret, first_leaf)
    print("signature_0[0]:", signature[0].hex())
    print("signature_0[66]:", signature[66].hex())
    path = []
    position = 0
    for height in range(HEIGHT):
        path.append(levels[height][position ^ 1])
        position >>= 1
    print("key_path_0[0]:", path[0].hex())
    print("key_path_0[7]:", path[7].hex())


if __name__ == "__main__":
    main()
