// The precedence chain (include/stx_auxlib/precedence.hpp). Every hash is
// BLAKE2s-256 over a NUL-terminated ASCII tag followed by fixed-length
// fields (digests as 32 bytes, integers little-endian), so no two inputs of
// different shape can collide by concatenation.
// tests/reference/precedence_reference.py implements the same bytes
// independently; the tests compare the two.

#include "stx_auxlib/precedence.hpp"

#include "checked.hpp"

#include "stx_auxlib/clocks.hpp"
#include "stx_auxlib/hash.hpp"
#include "stx_auxlib/random.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace stx_auxlib::precedence {

namespace {

constexpr std::uint32_t chain_length = 15; // w - 1, for w = 16
constexpr std::size_t last_slot = epochs_per_block - 1;

// Builds one domain-separated hash input.
class Input final {
public:
    explicit Input(const std::string_view tag) noexcept {
        hasher_.update(std::as_bytes(std::span<const char>(tag.data(), tag.size())));
        const std::byte terminator{0};
        hasher_.update(std::span<const std::byte>(&terminator, 1));
    }
    void digest(const hash::Digest& value) noexcept { hasher_.update(value.bytes); }
    void u32(const std::uint32_t value) noexcept {
        std::array<std::byte, 4> bytes{};
        for (std::size_t index = 0; index < 4; ++index)
            bytes[index] = static_cast<std::byte>((value >> (8 * index)) & 0xffU);
        hasher_.update(bytes);
    }
    void u64(const std::uint64_t value) noexcept {
        std::array<std::byte, 8> bytes{};
        for (std::size_t index = 0; index < 8; ++index)
            bytes[index] = static_cast<std::byte>((value >> (8 * index)) & 0xffU);
        hasher_.update(bytes);
    }
    // A signed time as its two's-complement 64 bits.
    void time(const WallClock::time_point value) noexcept {
        u64(static_cast<std::uint64_t>(value.time_since_epoch().count()));
    }
    [[nodiscard]] hash::Digest finish() noexcept { return hasher_.finish(); }

private:
    hash::Blake2s hasher_{};
};

hash::Digest ratchet(const hash::Digest& seed) noexcept {
    Input input("stx.precedence.ratchet");
    input.digest(seed);
    return input.finish();
}

hash::Digest wots_secret(const hash::Digest& seed, const std::uint32_t chain) noexcept {
    Input input("stx.precedence.wots.secret");
    input.digest(seed);
    input.u32(chain);
    return input.finish();
}

// `steps` applications of the chain function from step `start`.
hash::Digest walk_chain(const hash::Digest& public_seed, const std::uint64_t epoch, const std::uint32_t chain,
                        hash::Digest value, const std::uint32_t start, const std::uint32_t steps) noexcept {
    for (std::uint32_t step = start; step < start + steps; ++step) {
        Input input("stx.precedence.wots.chain");
        input.digest(public_seed);
        input.u64(epoch);
        input.u32(chain);
        input.u32(step);
        input.digest(value);
        value = input.finish();
    }
    return value;
}

// The 67 base-16 digits signed: the message's 64 nibbles, high nibble first,
// then the 12-bit checksum sum(15 - digit) as three nibbles.
std::array<std::uint32_t, signature_chains> digits_of(const hash::Digest& message) noexcept {
    std::array<std::uint32_t, signature_chains> digits{};
    std::uint32_t checksum = 0;
    for (std::size_t byte = 0; byte < message.bytes.size(); ++byte) {
        const std::uint32_t value = std::to_integer<std::uint32_t>(message.bytes[byte]);
        digits[2 * byte] = value >> 4;
        digits[2 * byte + 1] = value & 15U;
    }
    for (std::size_t index = 0; index < 64; ++index) checksum += chain_length - digits[index];
    digits[64] = (checksum >> 8) & 15U;
    digits[65] = (checksum >> 4) & 15U;
    digits[66] = checksum & 15U;
    return digits;
}

hash::Digest compress_public_key(const hash::Digest& public_seed, const std::uint64_t epoch,
                                 const std::array<hash::Digest, signature_chains>& ends) noexcept {
    Input input("stx.precedence.wots.public");
    input.digest(public_seed);
    input.u64(epoch);
    for (const hash::Digest& end : ends) input.digest(end);
    return input.finish();
}

hash::Digest wots_public_key(const hash::Digest& public_seed, const std::uint64_t epoch,
                             const hash::Digest& seed) noexcept {
    std::array<hash::Digest, signature_chains> ends{};
    for (std::uint32_t chain = 0; chain < signature_chains; ++chain) {
        hash::Digest secret = wots_secret(seed, chain);
        ends[chain] = walk_chain(public_seed, epoch, chain, secret, 0, chain_length);
        hash::secure_erase(secret.bytes);
    }
    return compress_public_key(public_seed, epoch, ends);
}

void wots_sign(const hash::Digest& public_seed, const std::uint64_t epoch, const hash::Digest& seed,
               const hash::Digest& message, std::array<hash::Digest, signature_chains>& signature) noexcept {
    const std::array<std::uint32_t, signature_chains> digits = digits_of(message);
    for (std::uint32_t chain = 0; chain < signature_chains; ++chain) {
        hash::Digest secret = wots_secret(seed, chain);
        signature[chain] = walk_chain(public_seed, epoch, chain, secret, 0, digits[chain]);
        hash::secure_erase(secret.bytes);
    }
}

// The public key a signature implies for `message`; a forged or altered
// signature implies a different key.
hash::Digest wots_public_from_signature(const hash::Digest& public_seed, const std::uint64_t epoch,
                                        const hash::Digest& message,
                                        const std::array<hash::Digest, signature_chains>& signature) noexcept {
    const std::array<std::uint32_t, signature_chains> digits = digits_of(message);
    std::array<hash::Digest, signature_chains> ends{};
    for (std::uint32_t chain = 0; chain < signature_chains; ++chain)
        ends[chain] = walk_chain(public_seed, epoch, chain, signature[chain], digits[chain],
                                 chain_length - digits[chain]);
    return compress_public_key(public_seed, epoch, ends);
}

hash::Digest key_leaf(const hash::Digest& public_seed, const std::uint64_t epoch, const hash::Digest& key) noexcept {
    Input input("stx.precedence.key");
    input.digest(public_seed);
    input.u64(epoch);
    input.digest(key);
    return input.finish();
}

hash::Digest key_node(const hash::Digest& public_seed, const std::uint64_t block, const std::uint32_t height,
                      const std::uint64_t index, const hash::Digest& left, const hash::Digest& right) noexcept {
    Input input("stx.precedence.node");
    input.digest(public_seed);
    input.u64(block);
    input.u32(height);
    input.u64(index);
    input.digest(left);
    input.digest(right);
    return input.finish();
}

// The block's key tree, level by level: level 0 is the 256 key leaves.
std::vector<std::vector<hash::Digest>> key_tree(const hash::Digest& public_seed, const std::uint64_t block,
                                                const std::vector<hash::Digest>& keys) {
    std::vector<std::vector<hash::Digest>> levels(key_tree_height + 1);
    levels[0].resize(epochs_per_block);
    const std::uint64_t first_epoch = block * epochs_per_block;
    for (std::size_t slot = 0; slot < epochs_per_block; ++slot)
        levels[0][slot] = key_leaf(public_seed, first_epoch + slot, keys[slot]);
    for (std::size_t height = 1; height <= key_tree_height; ++height) {
        const std::vector<hash::Digest>& below = levels[height - 1];
        levels[height].resize(below.size() / 2);
        for (std::size_t index = 0; index < levels[height].size(); ++index)
            levels[height][index] = key_node(public_seed, block, static_cast<std::uint32_t>(height), index,
                                             below[2 * index], below[2 * index + 1]);
    }
    return levels;
}

hash::Digest infos_digest(const std::vector<hash::Digest>& infos) noexcept {
    hash::Digest digest{};
    for (const hash::Digest& info : infos) {
        Input input("stx.precedence.infos");
        input.digest(digest);
        input.digest(info);
        digest = input.finish();
    }
    return digest;
}

hash::Digest genesis_digest(const Anchor& anchor) noexcept {
    Input input("stx.precedence.genesis");
    input.digest(anchor.public_seed);
    input.digest(anchor.first_key_root);
    input.time(anchor.genesis);
    return input.finish();
}

hash::Digest block_summary(const hash::Digest& public_seed, const std::uint64_t block, const hash::Digest& key_root,
                           const hash::Digest& last_leaf, const WallClock::time_point first,
                           const WallClock::time_point last) noexcept {
    Input input("stx.precedence.block");
    input.digest(public_seed);
    input.u64(block);
    input.digest(key_root);
    input.digest(last_leaf);
    input.time(first);
    input.time(last);
    return input.finish();
}

hash::Digest extend_chain(const hash::Digest& chain_digest, const hash::Digest& summary) noexcept {
    Input input("stx.precedence.chain");
    input.digest(chain_digest);
    input.digest(summary);
    return input.finish();
}

// The 256 public keys of the block whose first epoch has seed
// `first_seed`. The seeds it walks through are erased as it goes.
void derive_keys(const hash::Digest& public_seed, const hash::Digest& first_seed, const std::uint64_t first_epoch,
                 std::vector<hash::Digest>& keys) {
    keys.resize(epochs_per_block);
    hash::Digest seed = first_seed;
    for (std::size_t slot = 0; slot < epochs_per_block; ++slot) {
        keys[slot] = wots_public_key(public_seed, first_epoch + slot, seed);
        hash::Digest next = ratchet(seed);
        hash::secure_erase(seed.bytes);
        seed = next;
        hash::secure_erase(next.bytes);
    }
    hash::secure_erase(seed.bytes);
}

const hash::Digest zero_digest{};

} // namespace

hash::Digest leaf_of(const Anchor& anchor, const Certificate& certificate) noexcept {
    Input input("stx.precedence.epoch");
    input.digest(anchor.public_seed);
    input.u64(certificate.epoch);
    input.time(certificate.timestamp);
    input.u32(static_cast<std::uint32_t>(certificate.infos.size()));
    input.digest(infos_digest(certificate.infos));
    input.digest(certificate.previous_leaf);
    input.digest(certificate.chain_digest);
    input.digest(certificate.next_key_root);
    return input.finish();
}

ChainState::~ChainState() { hash::secure_erase(seed.bytes); }

Chain::Chain() noexcept = default;

Chain::~Chain() { erase_secret(); }

void Chain::erase_secret() noexcept { hash::secure_erase(seed_.bytes); }

Chain::Chain(Chain&& other) noexcept
    : anchor_(other.anchor_), seed_(other.seed_), epoch_(other.epoch_), last_time_(other.last_time_),
      block_first_time_(other.block_first_time_), previous_leaf_(other.previous_leaf_),
      chain_digest_(other.chain_digest_), block_keys_(std::move(other.block_keys_)),
      key_tree_(std::move(other.key_tree_)), pending_(std::move(other.pending_)) {
    other.erase_secret();
}

Chain& Chain::operator=(Chain&& other) noexcept {
    if (this == &other) return *this;
    erase_secret();
    anchor_ = other.anchor_;
    seed_ = other.seed_;
    epoch_ = other.epoch_;
    last_time_ = other.last_time_;
    block_first_time_ = other.block_first_time_;
    previous_leaf_ = other.previous_leaf_;
    chain_digest_ = other.chain_digest_;
    block_keys_ = std::move(other.block_keys_);
    key_tree_ = std::move(other.key_tree_);
    pending_ = std::move(other.pending_);
    other.erase_secret();
    return *this;
}

Chain Chain::from_seeds(const hash::Digest& secret_seed, const hash::Digest& public_seed,
                        const WallClock::time_point now) {
    Chain chain{};
    chain.seed_ = secret_seed;
    chain.anchor_.public_seed = public_seed;
    chain.anchor_.genesis = now;
    derive_keys(public_seed, secret_seed, 0, chain.block_keys_);
    chain.key_tree_ = key_tree(public_seed, 0, chain.block_keys_);
    chain.anchor_.first_key_root = chain.key_tree_[key_tree_height][0];
    chain.last_time_ = now;
    chain.block_first_time_ = now;
    chain.chain_digest_ = genesis_digest(chain.anchor_);
    return chain;
}

ChainResult Chain::create(const WallClock::time_point now) {
    std::array<std::byte, 64> entropy{};
    if (!random::fill_entropy(entropy)) return ChainResult::failed(PrecedenceStatus::entropy_unavailable);
    hash::Digest secret_seed{};
    hash::Digest public_seed{};
    std::memcpy(secret_seed.bytes.data(), entropy.data(), 32);
    std::memcpy(public_seed.bytes.data(), entropy.data() + 32, 32);
    hash::secure_erase(entropy);
    Chain chain = from_seeds(secret_seed, public_seed, now);
    hash::secure_erase(secret_seed.bytes);
    return ChainResult::made(std::move(chain));
}

ChainResult Chain::restore(const ChainState& state) {
    if (state.block_keys.size() != epochs_per_block || state.pending.size() > infos_per_epoch)
        return ChainResult::failed(PrecedenceStatus::invalid_certificate);
    Chain chain{};
    chain.anchor_ = state.anchor;
    chain.seed_ = state.seed;
    chain.epoch_ = state.epoch;
    chain.last_time_ = state.last_time;
    chain.block_first_time_ = state.block_first_time;
    chain.previous_leaf_ = state.previous_leaf;
    chain.chain_digest_ = state.chain_digest;
    chain.block_keys_ = state.block_keys;
    chain.key_tree_ = key_tree(state.anchor.public_seed, state.epoch / epochs_per_block, chain.block_keys_);
    chain.pending_ = state.pending;
    return ChainResult::made(std::move(chain));
}

void Chain::save(ChainState& state) const {
    state.anchor = anchor_;
    state.seed = seed_;
    state.epoch = epoch_;
    state.last_time = last_time_;
    state.block_first_time = block_first_time_;
    state.previous_leaf = previous_leaf_;
    state.chain_digest = chain_digest_;
    state.block_keys = block_keys_;
    state.pending = pending_;
}

const Anchor& Chain::anchor() const noexcept { return anchor_; }

std::uint64_t Chain::epoch() const noexcept { return epoch_; }

PrecedenceStatus Chain::record(const hash::Digest& info) {
    if (pending_.size() == infos_per_epoch) return PrecedenceStatus::epoch_full;
    pending_.push_back(info);
    return PrecedenceStatus::ok;
}

PrecedenceStatus Chain::advance(const WallClock::time_point now, Certificate& certificate) {
    if (epoch_ == std::numeric_limits<std::uint64_t>::max()) return PrecedenceStatus::exhausted;
    const std::uint64_t block = epoch_ / epochs_per_block;
    const std::size_t slot = static_cast<std::size_t>(epoch_ % epochs_per_block);
    const WallClock::time_point time = now < last_time_ ? last_time_ : now;
    if (slot == 0) block_first_time_ = time;

    // The next block's keys, from the next epoch's seed, on a block's last
    // epoch: its signed leaf commits their root.
    std::vector<hash::Digest> next_keys{};
    std::vector<std::vector<hash::Digest>> next_tree{};
    hash::Digest next_root{};
    if (slot == last_slot) {
        hash::Digest next_seed = ratchet(seed_);
        derive_keys(anchor_.public_seed, next_seed, epoch_ + 1, next_keys);
        hash::secure_erase(next_seed.bytes);
        next_tree = key_tree(anchor_.public_seed, block + 1, next_keys);
        next_root = next_tree[key_tree_height][0];
    }

    certificate.epoch = epoch_;
    certificate.timestamp = time;
    certificate.infos = pending_;
    certificate.previous_leaf = previous_leaf_;
    certificate.chain_digest = chain_digest_;
    certificate.next_key_root = next_root;
    const hash::Digest leaf = leaf_of(anchor_, certificate);
    wots_sign(anchor_.public_seed, epoch_, seed_, leaf, certificate.signature);
    std::size_t position = slot;
    for (std::size_t height = 0; height < key_tree_height; ++height) {
        certificate.key_path[height] = key_tree_[height][position ^ 1U];
        position >>= 1;
    }

    previous_leaf_ = leaf;
    last_time_ = time;
    pending_.clear();
    if (slot == last_slot) {
        const hash::Digest summary = block_summary(anchor_.public_seed, block, key_tree_[key_tree_height][0], leaf,
                                                   block_first_time_, time);
        chain_digest_ = extend_chain(chain_digest_, summary);
        block_keys_ = std::move(next_keys);
        key_tree_ = std::move(next_tree);
    }
    const hash::Digest next_seed = ratchet(seed_);
    erase_secret();
    seed_ = next_seed;
    ++epoch_;
    return PrecedenceStatus::ok;
}

ChainResult::ChainResult(State state) noexcept : state_(std::move(state)) {}

ChainResult ChainResult::made(Chain chain) noexcept {
    ChainResult result(State(std::in_place_index<0>, std::move(chain)));
    return result;
}

ChainResult ChainResult::failed(const PrecedenceStatus status) noexcept {
    ChainResult result(State(std::in_place_index<1>, status));
    return result;
}

bool ChainResult::succeeded() const noexcept {
    const bool result = std::holds_alternative<Chain>(state_);
    return result;
}

PrecedenceStatus ChainResult::status() const noexcept {
    const PrecedenceStatus* const failed_status = std::get_if<PrecedenceStatus>(&state_);
    if (failed_status == nullptr) return PrecedenceStatus::ok;
    return *failed_status;
}

Chain* ChainResult::chain() noexcept {
    Chain* const result = std::get_if<Chain>(&state_);
    return result;
}

Verifier::Verifier(const Anchor& anchor) : anchor_(anchor), key_roots_{anchor.first_key_root} {}

std::size_t Verifier::known_blocks() const noexcept { return key_roots_.size(); }

PrecedenceStatus Verifier::verify(const Certificate& certificate) {
    const std::uint64_t block = certificate.epoch / epochs_per_block;
    const std::size_t slot = static_cast<std::size_t>(certificate.epoch % epochs_per_block);
    if (block >= key_roots_.size()) return PrecedenceStatus::unknown_block;
    if (certificate.infos.size() > infos_per_epoch) return PrecedenceStatus::invalid_certificate;
    if (slot != last_slot && certificate.next_key_root != zero_digest) return PrecedenceStatus::invalid_certificate;

    const hash::Digest leaf = leaf_of(anchor_, certificate);
    const hash::Digest key =
        wots_public_from_signature(anchor_.public_seed, certificate.epoch, leaf, certificate.signature);
    hash::Digest node = key_leaf(anchor_.public_seed, certificate.epoch, key);
    std::uint64_t position = slot;
    for (std::size_t height = 0; height < key_tree_height; ++height) {
        const hash::Digest& sibling = certificate.key_path[height];
        const bool right = (position & 1U) != 0;
        node = key_node(anchor_.public_seed, block, static_cast<std::uint32_t>(height + 1), position >> 1,
                        right ? sibling : node, right ? node : sibling);
        position >>= 1;
    }
    if (node != key_roots_[static_cast<std::size_t>(block)]) return PrecedenceStatus::invalid_certificate;

    // A verified block-final certificate vouches for the next block's keys.
    if (slot == last_slot) {
        const std::size_t next = static_cast<std::size_t>(block) + 1;
        if (next == key_roots_.size()) {
            key_roots_.push_back(certificate.next_key_root);
        } else if (key_roots_[next] != certificate.next_key_root) {
            // Two validly signed, conflicting block-final certificates mean
            // the secret state was copied; neither can be trusted.
            return PrecedenceStatus::invalid_certificate;
        }
    }
    return PrecedenceStatus::ok;
}

PrecedenceStatus recorded_in(const Certificate& certificate, const hash::Digest& info) noexcept {
    for (const hash::Digest& recorded : certificate.infos) {
        if (recorded == info) return PrecedenceStatus::ok;
    }
    return PrecedenceStatus::not_recorded;
}

bool precedes(const Certificate& earlier, const Certificate& later, std::uint64_t& offset) noexcept {
    offset = 0;
    if (earlier.epoch >= later.epoch) return false;
    offset = later.epoch - earlier.epoch;
    return true;
}

bool follows_directly(const Anchor& anchor, const Certificate& earlier, const Certificate& later) noexcept {
    if (earlier.epoch == std::numeric_limits<std::uint64_t>::max()) return false;
    const bool result = later.epoch == earlier.epoch + 1 && later.previous_leaf == leaf_of(anchor, earlier) &&
                        later.timestamp >= earlier.timestamp;
    return result;
}

} // namespace stx_auxlib::precedence
