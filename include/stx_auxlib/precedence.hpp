#pragma once

// The precedence chain: proof that something was recorded before
// something else, by one holder, with only hash functions.
//
// The holder keeps a tiny secret (one 32-byte seed and the epoch number)
// in secure storage. Four operations:
// - advance: close the current epoch, signing its leaf, and ratchet the
//   seed forward. The old seed is erased; nothing, including a later thief
//   of the state, can sign for a past epoch again, since recovering it would
//   mean inverting the hash.
// - record: commit an info digest to the current epoch (up to
//   infos_per_epoch of them; the epoch's certificate lists them).
// - verify: anyone holding the chain's Anchor checks a certificate, so
//   that an info digest is shown to have been recorded at that epoch.
// - precedes: given two verified certificates, the earlier epoch's infos
//   were recorded before the later epoch's, and the epochs' distance is the
//   offset between them (the "rewind" proof; actually rewinding the seed
//   is infeasible).
//
// Each epoch also records a timestamp, which never goes backwards. The
// chain does NOT prove that time passed between epochs, and it cannot stop
// someone who copies the secret state from signing epochs from that point
// on; it orders events only while the local state stays secret. It needs
// no consensus and stores no ledger: certificates are outputs the caller
// keeps, the holder's state is a few hashes plus the current block's
// public keys.
//
// Construction (all hashes BLAKE2s-256, domain-separated; the exact bytes
// are in src/precedence.cpp and tests/reference/precedence_reference.py):
// - Seeds ratchet: E(n+1) = H("ratchet" || E(n)).
// - Each epoch n has a WOTS+ one-time key (w = 16, 67 chains) derived from
//   E(n). It signs exactly one 32-byte leaf, when epoch n closes.
// - Epochs come in blocks of epochs_per_block. A block's public keys are
//   Merkle-hashed into its key root; a certificate carries its key's path,
//   so its epoch number is bound to its key's position.
// - Block 0's key root is in the Anchor. The last epoch of each block
//   commits the next block's key root inside its signed leaf, so every
//   block's keys are vouched for by the block before, back to the anchor.
// - Each leaf also binds the previous leaf and a running digest of the
//   completed blocks' summaries, so consecutive certificates are linked
//   and every leaf commits to the chain's block history.

#include "stx_auxlib/clocks.hpp"
#include "stx_auxlib/hash.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <variant>
#include <vector>

namespace stx_auxlib::precedence {

constexpr std::size_t epochs_per_block = 256;
constexpr std::size_t key_tree_height = 8;
constexpr std::size_t infos_per_epoch = 16;
// WOTS+ with w = 16 over a 32-byte message: 64 message digits and 3
// checksum digits.
constexpr std::size_t signature_chains = 67;

enum class PrecedenceStatus : std::uint8_t {
    ok = 0,
    // The current epoch already holds infos_per_epoch infos; advance first.
    epoch_full = 1,
    // The host has no entropy source to create a chain.
    entropy_unavailable = 2,
    // The certificate's signature or key path does not check out.
    invalid_certificate = 3,
    // The certificate's block has no verified key root yet: verify the last
    // certificate of each earlier block first.
    unknown_block = 4,
    // The epoch counter is at its maximum.
    exhausted = 5,
    // The info digest is not in the certificate.
    not_recorded = 6,
};

// What a verifier needs and trusts: published once, when the chain is made.
struct Anchor final {
    hash::Digest public_seed{};
    hash::Digest first_key_root{};
    WallClock::time_point genesis{};
};

// One closed epoch, signed. Kept by whoever needs to prove what it holds.
struct Certificate final {
    std::uint64_t epoch{0};
    WallClock::time_point timestamp{};
    // The info digests recorded in the epoch, in order.
    std::vector<hash::Digest> infos{};
    hash::Digest previous_leaf{};
    // The running digest of all completed blocks' summaries.
    hash::Digest chain_digest{};
    // The next block's key root on a block's last epoch; zero otherwise.
    hash::Digest next_key_root{};
    std::array<hash::Digest, signature_chains> signature{};
    std::array<hash::Digest, key_tree_height> key_path{};
};

// The digest the epoch's key signs; it binds every field above except the
// signature and path.
[[nodiscard]] hash::Digest leaf_of(const Anchor& anchor, const Certificate& certificate) noexcept;

class ChainResult;

// Everything a chain needs to continue, for the caller's secure storage.
// `seed` is the secret; the rest is public. The destructor erases the
// seed. Restoring an old copy would let its holder sign epochs again from
// that point: keep only the latest, and only in secure storage.
struct ChainState final {
    Anchor anchor{};
    hash::Digest seed{};
    std::uint64_t epoch{0};
    WallClock::time_point last_time{};
    WallClock::time_point block_first_time{};
    hash::Digest previous_leaf{};
    hash::Digest chain_digest{};
    std::vector<hash::Digest> block_keys{};
    std::vector<hash::Digest> pending{};

    ChainState() noexcept = default;
    ChainState(const ChainState&) = default;
    ChainState& operator=(const ChainState&) = default;
    ~ChainState();
};

// The holder. Move-only; the secret seed is erased when the chain is
// destroyed or moved from. One thread at a time.
class Chain final {
public:
    // A new chain from the host's entropy; `now` is its genesis.
    [[nodiscard]] static ChainResult create(WallClock::time_point now);
    // A chain from a 32-byte secret seed and a public seed, for tests and
    // for callers with their own entropy.
    [[nodiscard]] static Chain from_seeds(const hash::Digest& secret_seed, const hash::Digest& public_seed,
                                          WallClock::time_point now);
    // A chain continued from saved state; invalid_certificate when the
    // state is inconsistent (a wrong number of block keys or pending infos).
    [[nodiscard]] static ChainResult restore(const ChainState& state);
    Chain(Chain&& other) noexcept;
    Chain& operator=(Chain&& other) noexcept;
    Chain(const Chain&) = delete;
    Chain& operator=(const Chain&) = delete;
    ~Chain();

    [[nodiscard]] const Anchor& anchor() const noexcept;
    // The open epoch, which record writes to.
    [[nodiscard]] std::uint64_t epoch() const noexcept;
    [[nodiscard]] PrecedenceStatus record(const hash::Digest& info);
    // Closes the open epoch into `certificate` and opens the next one.
    // `now` earlier than the previous epoch's time is recorded as that time.
    // A block's last epoch also derives the next block's keys (256 key
    // derivations).
    [[nodiscard]] PrecedenceStatus advance(WallClock::time_point now, Certificate& certificate);
    // Copies the state for secure storage.
    void save(ChainState& state) const;

private:
    Chain() noexcept;
    void erase_secret() noexcept;
    Anchor anchor_{};
    // The open epoch's seed; erased on every ratchet and on destruction.
    hash::Digest seed_{};
    std::uint64_t epoch_{0};
    WallClock::time_point last_time_{};
    WallClock::time_point block_first_time_{};
    hash::Digest previous_leaf_{};
    hash::Digest chain_digest_{};
    // The current block's public keys (public data; regenerated only at a
    // block boundary, from the seed then current).
    std::vector<hash::Digest> block_keys_{};
    // The current block's key tree, level 0 the key leaves; rebuilt from
    // block_keys_ at each block boundary and on restore.
    std::vector<std::vector<hash::Digest>> key_tree_{};
    std::vector<hash::Digest> pending_{};
};

// The result of creating or restoring a chain: the chain, or the status.
class ChainResult final {
public:
    [[nodiscard]] static ChainResult made(Chain chain) noexcept;
    [[nodiscard]] static ChainResult failed(PrecedenceStatus status) noexcept;
    [[nodiscard]] bool succeeded() const noexcept;
    [[nodiscard]] PrecedenceStatus status() const noexcept;
    // The chain; null after a failure.
    [[nodiscard]] Chain* chain() noexcept;

private:
    using State = std::variant<Chain, PrecedenceStatus>;
    explicit ChainResult(State state) noexcept;
    State state_;
};

// A verifier's trusted key roots, starting from the anchor and extended by
// each verified block-final certificate. Verification is a WOTS+ check
// (about 500 hashes) and an 8-hash key path.
class Verifier final {
public:
    explicit Verifier(const Anchor& anchor);
    // ok, invalid_certificate or unknown_block. A verified block-final
    // certificate teaches the verifier the next block's key root.
    [[nodiscard]] PrecedenceStatus verify(const Certificate& certificate);
    [[nodiscard]] std::size_t known_blocks() const noexcept;

private:
    Anchor anchor_{};
    std::vector<hash::Digest> key_roots_{};
};

// ok when `info` is among the certificate's recorded infos.
[[nodiscard]] PrecedenceStatus recorded_in(const Certificate& certificate, const hash::Digest& info) noexcept;

// For two certificates the verifier has accepted: true when `earlier`'s
// epoch precedes `later`'s; `offset` receives later.epoch - earlier.epoch.
[[nodiscard]] bool precedes(const Certificate& earlier, const Certificate& later, std::uint64_t& offset) noexcept;

// True when `later` immediately follows `earlier` in the same chain: its
// epoch is the next and its previous_leaf is earlier's leaf.
[[nodiscard]] bool follows_directly(const Anchor& anchor, const Certificate& earlier,
                                    const Certificate& later) noexcept;

} // namespace stx_auxlib::precedence
