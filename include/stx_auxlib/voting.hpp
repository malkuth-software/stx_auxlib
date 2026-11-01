#pragma once

// Preference Ranked Voting (PRV 1.0), the counting rule of the owner's
// "Preference Ranked Voting" design (paymenottowork.com, ideas/
// preference-ranked-voting/prv-core.mjs), for any choice among ranked
// options: elections, polls, feature triage, tournament seeding.
//
// The rule, in the reference's order:
// 1. Validation: a ballot is valid when it ranks at least one candidate,
//    names each at most once and names only real candidates; invalid ones
//    are counted and set aside. More valid ballots than candidates are
//    required.
// 2. Expansion: incomplete ballots are completed one rank at a time, the
//    longest first, by inference from other ballots that agree with them
//    (the donors whose rankings best match the ballot's own order propose
//    their next choice; ties go to the proposal ranked earliest most
//    often). Exhausted ballots therefore keep counting.
// 3. Elimination: rounds of first-preference counts; a majority wins; the
//    candidate with the lowest positional vector (first-rank count, then
//    second, ...) is eliminated; two left are decided by first preferences.
// 4. Bracketing: with more than bracket_threshold candidates, they are
//    seeded by positional score and counted in brackets of
//    bracket_interval - 1 seeds plus the previous bracket's winner, from the
//    lowest seeds up.
// Ties the rule cannot break deterministically, and perfect Condorcet
// cycles, are reported as named failures; the rule never resolves by lot.
//
// This implementation counts each distinct ballot once with its
// multiplicity. Identical ballots infer identically at every step (a
// ballot is never its own donor), so the result is exactly the
// reference's, at a cost that grows with the number of distinct rankings
// rather than the number of voters.

#include <cstddef>
#include <cstdint>
#include <span>
#include <variant>
#include <vector>

namespace stx_auxlib::voting {

// Candidates are 0 to candidate_count - 1; names belong to the caller.
using Candidate = std::uint32_t;

// Ballots in one flat buffer: ballot i is choices[ends[i - 1] .. ends[i]).
class Ballots final {
public:
    // Adds a ballot, most preferred first. It is validated when counted.
    void add(std::span<const Candidate> ranking);
    // Adds `copies` identical ballots without storing each.
    void add_copies(std::span<const Candidate> ranking, std::uint64_t copies);
    [[nodiscard]] std::size_t distinct() const noexcept;
    [[nodiscard]] std::span<const Candidate> ranking(std::size_t index) const noexcept;
    [[nodiscard]] std::uint64_t copies(std::size_t index) const noexcept;
    void clear() noexcept;

private:
    std::vector<Candidate> choices_{};
    std::vector<std::size_t> ends_{};
    std::vector<std::uint64_t> copies_{};
};

struct PrvOptions final {
    std::size_t bracket_threshold{30};
    std::size_t bracket_interval{10};
};

enum class PrvFailure : std::uint8_t {
    too_few_candidates = 1,
    too_few_valid_ballots = 2,
    invalid_bracket_interval = 3,
    // An incomplete ballot has no donor that ranks anything it lacks.
    no_expansion_evidence = 4,
    perfect_expansion_tie = 5,
    perfect_condorcet_cycle = 6,
    perfect_final_tie = 7,
    perfect_elimination_tie = 8,
    perfect_seed_tie = 9,
    // A count exceeded 2^63 ballots.
    too_many_ballots = 10,
};

// The count's result: the winner, or why there is none. Never both.
class PrvOutcome final {
public:
    [[nodiscard]] static PrvOutcome won(Candidate winner, std::uint64_t rejected) noexcept;
    [[nodiscard]] static PrvOutcome failed(PrvFailure failure, std::uint64_t rejected) noexcept;
    [[nodiscard]] const Candidate* winner() const noexcept;
    [[nodiscard]] const PrvFailure* failure() const noexcept;
    // Invalid ballots set aside in validation.
    [[nodiscard]] std::uint64_t rejected_ballots() const noexcept;

private:
    using State = std::variant<Candidate, PrvFailure>;
    PrvOutcome(State state, std::uint64_t rejected) noexcept;
    State state_;
    std::uint64_t rejected_{0};
};

// Counts `ballots` over candidates 0 to candidate_count - 1.
[[nodiscard]] PrvOutcome count_preference_ranked(std::uint32_t candidate_count, const Ballots& ballots,
                                                 const PrvOptions& options = {});

} // namespace stx_auxlib::voting
