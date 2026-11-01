// Tests of BLAKE2s (hash.hpp) and the precedence chain (precedence.hpp).
// Known answers come from tests/reference/precedence_reference.py, which
// implements the construction independently over Python's hashlib.

#include "stx_auxlib/clocks.hpp"
#include "stx_auxlib/hash.hpp"
#include "stx_auxlib/precedence.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace precedence = stx_auxlib::precedence;
namespace hash = stx_auxlib::hash;

void require(const bool condition, const char* const message) {
    if (condition) return;
    std::fprintf(stderr, "stx_auxlib precedence: %s\n", message);
    std::abort();
}

std::string hex_of(const hash::Digest& digest) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result{};
    for (const std::byte value : digest.bytes) {
        const unsigned byte = std::to_integer<unsigned>(value);
        result.push_back(digits[byte >> 4]);
        result.push_back(digits[byte & 15U]);
    }
    return result;
}

hash::Digest digest_of(const std::string_view text) {
    const hash::Digest result = hash::blake2s(std::as_bytes(std::span<const char>(text.data(), text.size())));
    return result;
}

hash::Digest counting_digest(const unsigned first) {
    hash::Digest result{};
    for (std::size_t index = 0; index < result.bytes.size(); ++index)
        result.bytes[index] = static_cast<std::byte>(first + index);
    return result;
}

stx_auxlib::WallClock::time_point at_ns(const std::int64_t nanoseconds) {
    const stx_auxlib::WallClock::time_point result{stx_auxlib::WallClock::duration(nanoseconds)};
    return result;
}

void blake2s_vectors() {
    require(hex_of(digest_of("")) == "69217a3079908094e11121d042354a7c1f55b6482ca1a51e1b250dfd1ed0eef9",
            "BLAKE2s of the empty string differs");
    require(hex_of(digest_of("abc")) == "508c5e8c327c14e2e1a72ba34eeb452f37458b209ed63a294d999b4c86675982",
            "BLAKE2s of abc differs from RFC 7693");
    require(hex_of(digest_of(std::string(200, 'a'))) ==
                "2b033f9f5ba9cf20671da79e492f41545e673b562603945ffed09662fd92321a",
            "BLAKE2s across several blocks differs");
    const hash::Digest key = counting_digest(0);
    require(hex_of(hash::blake2s_keyed(key.bytes, {})) ==
                "48a8997da407876b3d79c0d92325ad3b89cbb754d86ab71aee047ad345fd2c49",
            "keyed BLAKE2s of the empty string differs");
    std::array<std::byte, 256> counting{};
    for (std::size_t index = 0; index < counting.size(); ++index) counting[index] = static_cast<std::byte>(index);
    require(hex_of(hash::blake2s_keyed(key.bytes, counting)) ==
                "5211d1aefc0025be7f85c06b3e14e0fc645ae12bd41746485ea6d8a364a2eaee",
            "keyed BLAKE2s of 0..255 differs");
    // Feeding in pieces gives the same digest.
    hash::Blake2s pieces{};
    for (std::size_t index = 0; index < counting.size(); index += 7)
        pieces.update(std::span<const std::byte>(counting).subspan(index, index + 7 <= counting.size() ? 7 : counting.size() - index));
    require(pieces.finish() == hash::blake2s(counting), "incremental BLAKE2s differs");
}

constexpr std::int64_t genesis_ns = 1793482205000000000LL;

precedence::Chain fixed_chain() {
    precedence::Chain chain = precedence::Chain::from_seeds(counting_digest(0), counting_digest(32), at_ns(genesis_ns));
    return chain;
}

void known_answers() {
    precedence::Chain chain = fixed_chain();
    require(hex_of(chain.anchor().first_key_root) ==
                "1a59a551b642ef1798d67ad2c0747a706b94dc4bd89cbafd4eacc55616696572",
            "the first key root differs from the reference");
    require(chain.record(digest_of("alpha")) == precedence::PrecedenceStatus::ok, "record failed");
    precedence::Certificate certificate{};
    require(chain.advance(at_ns(genesis_ns + 1000000000LL), certificate) == precedence::PrecedenceStatus::ok,
            "advance failed");
    require(hex_of(precedence::leaf_of(chain.anchor(), certificate)) ==
                "1f9cdf2817920e91622b6b0c4e1577c5fcbecd62b264a327dd8f0cdf391a9113",
            "the first leaf differs from the reference");
    require(hex_of(certificate.signature[0]) == "629a7bb8ffb4ba4de405b4ac739ab800242d1fa24e72de87e681250b3b157840" &&
                hex_of(certificate.signature[66]) ==
                    "661ea115aa6d8bf8e69049ae54dbc0839592938e6d8fae315c2ef84b9bc4762e",
            "the first signature differs from the reference");
    require(hex_of(certificate.key_path[0]) == "59e888090592e82a52c03f6138b58680f312680818b1ecf936754c4bdd5c7fb3" &&
                hex_of(certificate.key_path[7]) ==
                    "ebef89f081ad90193b73441ad1a0267bca2bdec8e5c32942086dc4a497c4d006",
            "the first key path differs from the reference");
}

// Runs a chain across two block boundaries, verifying every certificate in
// order, and checks linkage, precedence and the info limit.
void lifecycle() {
    precedence::Chain chain = fixed_chain();
    precedence::Verifier verifier(chain.anchor());
    std::vector<precedence::Certificate> certificates{};
    const std::size_t epochs = 2 * precedence::epochs_per_block + 10;
    for (std::size_t epoch = 0; epoch < epochs; ++epoch) {
        require(chain.epoch() == epoch, "the open epoch is wrong");
        require(chain.record(digest_of("item " + std::to_string(epoch))) == precedence::PrecedenceStatus::ok,
                "record failed");
        precedence::Certificate certificate{};
        // A clock that steps back is recorded as the previous time.
        const std::int64_t when = genesis_ns + static_cast<std::int64_t>(epoch) * 1000 - (epoch == 5 ? 5000 : 0);
        require(chain.advance(at_ns(when), certificate) == precedence::PrecedenceStatus::ok, "advance failed");
        require(verifier.verify(certificate) == precedence::PrecedenceStatus::ok, "a certificate failed to verify");
        if (!certificates.empty())
            require(precedence::follows_directly(chain.anchor(), certificates.back(), certificate),
                    "consecutive certificates are not linked");
        certificates.push_back(certificate);
    }
    require(verifier.known_blocks() == 3, "the verifier did not learn each block's keys");
    require(certificates[5].timestamp == certificates[4].timestamp, "a clock step back was not clamped");

    std::uint64_t offset = 0;
    require(precedence::precedes(certificates[3], certificates[300], offset) && offset == 297,
            "precedence across a block is wrong");
    require(!precedence::precedes(certificates[300], certificates[3], offset), "a later epoch preceded");
    require(precedence::recorded_in(certificates[300], digest_of("item 300")) == precedence::PrecedenceStatus::ok,
            "a recorded info was not found");
    require(precedence::recorded_in(certificates[300], digest_of("item 3")) ==
                precedence::PrecedenceStatus::not_recorded,
            "an info from another epoch was found");

    // A fresh verifier cannot check block 1 before learning block 0's link.
    precedence::Verifier fresh(chain.anchor());
    require(fresh.verify(certificates[300]) == precedence::PrecedenceStatus::unknown_block,
            "a later block verified without its link");
    require(fresh.verify(certificates[255]) == precedence::PrecedenceStatus::ok &&
                fresh.verify(certificates[300]) == precedence::PrecedenceStatus::ok,
            "the block link did not admit the next block");

    for (std::size_t index = 0; index < precedence::infos_per_epoch; ++index)
        require(chain.record(counting_digest(static_cast<unsigned>(index))) == precedence::PrecedenceStatus::ok,
                "record failed below the limit");
    require(chain.record(counting_digest(99)) == precedence::PrecedenceStatus::epoch_full,
            "the info limit was not enforced");
}

// Any change to a certificate, or a certificate from another secret, fails.
void tampering() {
    precedence::Chain chain = fixed_chain();
    precedence::Verifier verifier(chain.anchor());
    (void)chain.record(digest_of("original"));
    precedence::Certificate certificate{};
    (void)chain.advance(at_ns(genesis_ns + 1), certificate);
    require(verifier.verify(certificate) == precedence::PrecedenceStatus::ok, "the original failed");

    precedence::Certificate changed_info = certificate;
    changed_info.infos[0] = digest_of("forged");
    require(verifier.verify(changed_info) == precedence::PrecedenceStatus::invalid_certificate,
            "a changed info verified");
    precedence::Certificate changed_time = certificate;
    changed_time.timestamp = at_ns(genesis_ns - 1000000000LL);
    require(verifier.verify(changed_time) == precedence::PrecedenceStatus::invalid_certificate,
            "a backdated timestamp verified");
    precedence::Certificate changed_epoch = certificate;
    changed_epoch.epoch = 1;
    require(verifier.verify(changed_epoch) == precedence::PrecedenceStatus::invalid_certificate,
            "a moved epoch verified");
    precedence::Certificate changed_signature = certificate;
    changed_signature.signature[13].bytes[0] ^= std::byte{1};
    require(verifier.verify(changed_signature) == precedence::PrecedenceStatus::invalid_certificate,
            "a changed signature verified");
    precedence::Certificate changed_path = certificate;
    changed_path.key_path[4].bytes[31] ^= std::byte{0x80};
    require(verifier.verify(changed_path) == precedence::PrecedenceStatus::invalid_certificate,
            "a changed key path verified");
    precedence::Certificate added_next = certificate;
    added_next.next_key_root = counting_digest(7);
    require(verifier.verify(added_next) == precedence::PrecedenceStatus::invalid_certificate,
            "a next key root outside a block's end verified");

    precedence::Chain other =
        precedence::Chain::from_seeds(counting_digest(1), counting_digest(32), at_ns(genesis_ns));
    (void)other.record(digest_of("original"));
    precedence::Certificate foreign{};
    (void)other.advance(at_ns(genesis_ns + 1), foreign);
    require(verifier.verify(foreign) == precedence::PrecedenceStatus::invalid_certificate,
            "a certificate from another secret verified");
}

// Saving and restoring continues the same chain, bit for bit; a created
// chain uses fresh entropy.
void persistence() {
    precedence::Chain straight = fixed_chain();
    precedence::Chain interrupted = fixed_chain();
    precedence::Certificate first{};
    precedence::Certificate second{};
    for (std::size_t epoch = 0; epoch < 250; ++epoch) {
        (void)straight.advance(at_ns(genesis_ns + static_cast<std::int64_t>(epoch)), first);
        (void)interrupted.advance(at_ns(genesis_ns + static_cast<std::int64_t>(epoch)), second);
    }
    (void)interrupted.record(digest_of("pending"));
    (void)straight.record(digest_of("pending"));
    precedence::ChainState state{};
    interrupted.save(state);
    precedence::ChainResult restored = precedence::Chain::restore(state);
    require(restored.succeeded(), "restore failed");
    precedence::Chain& resumed = *restored.chain();
    for (std::size_t epoch = 250; epoch < 270; ++epoch) {
        (void)straight.advance(at_ns(genesis_ns + static_cast<std::int64_t>(epoch)), first);
        (void)resumed.advance(at_ns(genesis_ns + static_cast<std::int64_t>(epoch)), second);
        require(precedence::leaf_of(straight.anchor(), first) == precedence::leaf_of(resumed.anchor(), second) &&
                    first.signature == second.signature,
                "a restored chain diverged");
    }
    precedence::ChainState broken = state;
    broken.block_keys.pop_back();
    require(precedence::Chain::restore(broken).status() == precedence::PrecedenceStatus::invalid_certificate,
            "an inconsistent state was restored");

    precedence::ChainResult created = precedence::Chain::create(stx_auxlib::WallClock::now());
    precedence::ChainResult again = precedence::Chain::create(stx_auxlib::WallClock::now());
    require(created.succeeded() && again.succeeded(), "create failed");
    require((*created.chain()).anchor().public_seed != (*again.chain()).anchor().public_seed,
            "two created chains share a public seed");
}

void timing() {
    precedence::Chain chain = fixed_chain();
    precedence::Verifier verifier(chain.anchor());
    precedence::Certificate certificate{};
    const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    for (std::size_t epoch = 0; epoch < 255; ++epoch) {
        (void)chain.advance(at_ns(genesis_ns), certificate);
        (void)verifier.verify(certificate);
    }
    const std::chrono::duration<double, std::micro> elapsed = std::chrono::steady_clock::now() - start;
    std::printf("advance + verify: %.1f us per epoch (within a block)\n", elapsed.count() / 255.0);
}

} // namespace

int main() {
    blake2s_vectors();
    known_answers();
    lifecycle();
    tampering();
    persistence();
    timing();
    std::puts("stx_auxlib precedence tests passed");
    return 0;
}
