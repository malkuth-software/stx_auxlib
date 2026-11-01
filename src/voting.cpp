// Preference Ranked Voting (include/stx_auxlib/voting.hpp), ported step for
// step from the reference prv-core.mjs, with each distinct ballot held once
// with its multiplicity. Where the reference subtracts the one ballot being
// inferred from a positional count, this subtracts one copy, not the
// group's weight, so every count equals the reference's.
//
// tests/reference/prv_reference.mjs runs the reference itself on random
// elections; tests/voting_tests.cpp compares every outcome.

#include "stx_auxlib/voting.hpp"

#include "checked.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <utility>
#include <variant>
#include <vector>

namespace stx_auxlib::voting {

namespace {

// Counts stay below 2^62, so doubling a count or forming a margin never
// overflows.
constexpr std::uint64_t largest_total = std::uint64_t{1} << 62;

using Counts = std::vector<std::uint64_t>;

struct Group final {
    std::vector<Candidate> ranking{};
    std::uint64_t weight{0};
};

// The reference's compareVectors: lexicographic, missing entries as zero.
int compare_vectors(const Counts& left, const Counts& right) noexcept {
    const std::size_t length = std::max(left.size(), right.size());
    for (std::size_t index = 0; index < length; ++index) {
        const std::uint64_t a = index < left.size() ? left[index] : 0;
        const std::uint64_t b = index < right.size() ? right[index] : 0;
        if (a != b) return a < b ? -1 : 1;
    }
    return 0;
}

std::uint64_t total_weight(const std::vector<Group>& groups) noexcept {
    std::uint64_t total = 0;
    for (const Group& group : groups) total += group.weight;
    return total;
}

// position[c][r]: the weight of ballots ranking candidate c at rank r, for
// ranks below `size`. Candidates are global ids; `count` is the id range.
std::vector<Counts> position_counts(const std::vector<Group>& groups, const std::size_t count,
                                    const std::size_t size) {
    std::vector<Counts> positions(count, Counts(size, 0));
    for (const Group& group : groups) {
        const std::size_t length = std::min(group.ranking.size(), size);
        for (std::size_t rank = 0; rank < length; ++rank) positions[group.ranking[rank]][rank] += group.weight;
    }
    return positions;
}

std::vector<Candidate> filter_ranking(const std::vector<Candidate>& ranking, const std::vector<char>& member) {
    std::vector<Candidate> result{};
    result.reserve(ranking.size());
    for (const Candidate candidate : ranking) {
        if (member[candidate] != 0) result.push_back(candidate);
    }
    return result;
}

std::vector<char> membership(const std::vector<Candidate>& active, const std::size_t count) {
    std::vector<char> member(count, 0);
    for (const Candidate candidate : active) member[candidate] = 1;
    return member;
}

// The reference's subsequenceMatch: where the pattern's candidates appear
// in the donor, in order, until one is missing.
std::vector<std::size_t> subsequence_positions(const std::vector<Candidate>& pattern,
                                               const std::vector<Candidate>& donor) {
    std::vector<std::size_t> positions{};
    std::size_t start = 0;
    for (const Candidate candidate : pattern) {
        std::size_t index = start;
        while (index < donor.size() && donor[index] != candidate) ++index;
        if (index == donor.size()) break;
        positions.push_back(index);
        start = index + 1;
    }
    return positions;
}

int compare_positions(const std::vector<std::size_t>& left, const std::vector<std::size_t>& right) noexcept {
    const std::size_t length = std::max(left.size(), right.size());
    for (std::size_t index = 0; index < length; ++index) {
        const std::size_t a = index < left.size() ? left[index] : 0;
        const std::size_t b = index < right.size() ? right[index] : 0;
        if (a != b) return a < b ? -1 : 1;
    }
    return 0;
}

struct Donor final {
    std::vector<std::size_t> positions{};
    std::vector<Candidate> residual{};
    std::uint64_t weight{0};
};

// The result of inferring one ballot's next candidate.
struct Inference final {
    bool inferred{false};
    Candidate candidate{0};
    PrvFailure failure{PrvFailure::no_expansion_evidence};
};

// The reference's inferOne, for the group at `target`.
Inference infer_one(const std::size_t target, const std::vector<Group>& states, const std::vector<Candidate>& active,
                    const std::size_t count) {
    const std::vector<Candidate>& pattern = states[target].ranking;
    std::vector<char> in_pattern(count, 0);
    for (const Candidate candidate : pattern) in_pattern[candidate] = 1;
    std::vector<Candidate> missing{};
    for (const Candidate candidate : active) {
        if (in_pattern[candidate] == 0) missing.push_back(candidate);
    }
    Inference result{};
    if (missing.size() == 1) {
        result.inferred = true;
        result.candidate = missing[0];
        return result;
    }

    std::vector<Donor> donors{};
    for (std::size_t index = 0; index < states.size(); ++index) {
        if (index == target) continue;
        Donor donor{};
        for (const Candidate candidate : states[index].ranking) {
            if (in_pattern[candidate] == 0) donor.residual.push_back(candidate);
        }
        if (donor.residual.empty()) continue;
        donor.positions = subsequence_positions(pattern, states[index].ranking);
        donor.weight = states[index].weight;
        donors.push_back(std::move(donor));
    }
    if (donors.empty()) {
        result.failure = PrvFailure::no_expansion_evidence;
        return result;
    }

    std::size_t best_length = 0;
    for (const Donor& donor : donors) best_length = std::max(best_length, donor.positions.size());
    const std::vector<std::size_t>* best_positions = nullptr;
    for (const Donor& donor : donors) {
        if (donor.positions.size() != best_length) continue;
        if (best_positions == nullptr || compare_positions(donor.positions, *best_positions) < 0)
            best_positions = &donor.positions;
    }
    std::vector<const Donor*> group{};
    for (const Donor& donor : donors) {
        if (donor.positions.size() == best_length && compare_positions(donor.positions, *best_positions) == 0)
            group.push_back(&donor);
    }

    // Proposals: the group's next choices, weighted.
    Counts proposals(count, 0);
    for (const Donor* const donor : group) proposals[(*donor).residual[0]] += (*donor).weight;
    std::uint64_t best_count = 0;
    for (const Candidate candidate : active) best_count = std::max(best_count, proposals[candidate]);
    std::vector<Candidate> tied{};
    for (const Candidate candidate : active) {
        if (proposals[candidate] == best_count && best_count > 0) tied.push_back(candidate);
    }
    if (tied.size() == 1) {
        result.inferred = true;
        result.candidate = tied[0];
        return result;
    }

    std::size_t residual_length = 0;
    for (const Donor* const donor : group) residual_length = std::max(residual_length, (*donor).residual.size());
    std::vector<Counts> vectors(tied.size(), Counts(residual_length, 0));
    for (std::size_t index = 0; index < tied.size(); ++index) {
        for (const Donor* const donor : group) {
            const std::vector<Candidate>& residual = (*donor).residual;
            for (std::size_t rank = 0; rank < residual.size(); ++rank) {
                if (residual[rank] == tied[index]) vectors[index][rank] += (*donor).weight;
            }
        }
    }
    std::size_t best = 0;
    for (std::size_t index = 1; index < tied.size(); ++index) {
        if (compare_vectors(vectors[index], vectors[best]) > 0) best = index;
    }
    std::size_t matching = 0;
    for (std::size_t index = 0; index < tied.size(); ++index) {
        if (compare_vectors(vectors[index], vectors[best]) == 0) ++matching;
    }
    if (matching != 1) {
        result.failure = PrvFailure::perfect_expansion_tie;
        return result;
    }
    result.inferred = true;
    result.candidate = tied[best];
    return result;
}

// The reference's ballotPriority for one copy of group `index`: for each of
// its candidates in order, that candidate's positional vector without this
// copy.
Counts ballot_priority(const std::size_t index, const std::vector<Group>& states,
                       const std::vector<Counts>& positions, const std::size_t size) {
    const std::vector<Candidate>& ranking = states[index].ranking;
    Counts signature{};
    signature.reserve(ranking.size() * size);
    for (std::size_t rank = 0; rank < ranking.size(); ++rank) {
        const Counts& vector = positions[ranking[rank]];
        for (std::size_t column = 0; column < size; ++column)
            signature.push_back(vector[column] - (column == rank ? 1 : 0));
    }
    return signature;
}

struct Expansion final {
    bool expanded{false};
    PrvFailure failure{PrvFailure::no_expansion_evidence};
    std::vector<Group> groups{};
};

// The reference's expandBallots.
Expansion expand_ballots(const std::vector<Group>& ballots, const std::vector<Candidate>& active,
                         const std::size_t count) {
    const std::vector<char> member = membership(active, count);
    Expansion result{};
    std::vector<Group> states{};
    states.reserve(ballots.size());
    for (const Group& ballot : ballots) states.push_back(Group{filter_ranking(ballot.ranking, member), ballot.weight});
    const std::size_t size = active.size();
    for (;;) {
        std::size_t maximum_length = 0;
        bool any_incomplete = false;
        for (const Group& state : states) {
            if (state.ranking.size() < size) {
                any_incomplete = true;
                maximum_length = std::max(maximum_length, state.ranking.size());
            }
        }
        if (!any_incomplete) {
            result.expanded = true;
            result.groups = std::move(states);
            return result;
        }
        const std::vector<Counts> positions = position_counts(states, count, size);
        std::vector<std::size_t> level{};
        std::vector<Counts> signatures{};
        for (std::size_t index = 0; index < states.size(); ++index) {
            if (states[index].ranking.size() != maximum_length) continue;
            level.push_back(index);
            signatures.push_back(ballot_priority(index, states, positions, size));
        }
        std::size_t highest = 0;
        for (std::size_t index = 1; index < level.size(); ++index) {
            if (compare_vectors(signatures[index], signatures[highest]) > 0) highest = index;
        }
        // Every member of the batch infers from the same snapshot.
        std::vector<std::pair<std::size_t, Candidate>> decisions{};
        for (std::size_t index = 0; index < level.size(); ++index) {
            if (compare_vectors(signatures[index], signatures[highest]) != 0) continue;
            const Inference inferred = infer_one(level[index], states, active, count);
            if (!inferred.inferred) {
                result.failure = inferred.failure;
                return result;
            }
            decisions.emplace_back(level[index], inferred.candidate);
        }
        for (const std::pair<std::size_t, Candidate>& decision : decisions)
            states[decision.first].ranking.push_back(decision.second);
    }
}

std::vector<Group> project(const std::vector<Group>& groups, const std::vector<char>& member) {
    std::vector<Group> projected{};
    projected.reserve(groups.size());
    for (const Group& group : groups) projected.push_back(Group{filter_ranking(group.ranking, member), group.weight});
    return projected;
}

std::size_t index_of(const std::vector<Candidate>& ranking, const Candidate candidate) noexcept {
    std::size_t index = 0;
    while (index < ranking.size() && ranking[index] != candidate) ++index;
    return index;
}

// The reference's perfectCycle: every candidate with the same positional
// vector, a pairwise-majority graph in which each reaches all, and the same
// sorted margins for each.
bool perfect_cycle(const std::vector<Group>& projected, const std::vector<Candidate>& remaining,
                   const std::size_t count, const std::uint64_t total) {
    if (remaining.size() < 3) return false;
    const std::vector<Counts> positions = position_counts(projected, count, remaining.size());
    for (const Candidate candidate : remaining) {
        if (compare_vectors(positions[candidate], positions[remaining[0]]) != 0) return false;
    }
    const std::size_t size = remaining.size();
    std::vector<std::int64_t> margins(size * size, 0);
    std::vector<char> wins(size * size, 0);
    for (std::size_t left = 0; left < size; ++left) {
        for (std::size_t right = left + 1; right < size; ++right) {
            std::uint64_t left_votes = 0;
            for (const Group& group : projected) {
                if (index_of(group.ranking, remaining[left]) < index_of(group.ranking, remaining[right]))
                    left_votes += group.weight;
            }
            const std::int64_t margin = static_cast<std::int64_t>(left_votes) -
                                        static_cast<std::int64_t>(total - left_votes);
            margins[left * size + right] = margin;
            margins[right * size + left] = -margin;
            if (margin > 0) wins[left * size + right] = 1;
            if (margin < 0) wins[right * size + left] = 1;
        }
    }
    for (std::size_t start = 0; start < size; ++start) {
        std::vector<char> seen(size, 0);
        std::vector<std::size_t> stack{start};
        seen[start] = 1;
        std::size_t reached = 1;
        while (!stack.empty()) {
            const std::size_t from = stack.back();
            stack.pop_back();
            for (std::size_t to = 0; to < size; ++to) {
                if (wins[from * size + to] != 0 && seen[to] == 0) {
                    seen[to] = 1;
                    ++reached;
                    stack.push_back(to);
                }
            }
        }
        if (reached != size) return false;
    }
    std::vector<std::int64_t> first_signature{};
    for (std::size_t candidate = 0; candidate < size; ++candidate) {
        std::vector<std::int64_t> signature{};
        for (std::size_t other = 0; other < size; ++other) {
            if (other != candidate) signature.push_back(margins[candidate * size + other]);
        }
        std::sort(signature.begin(), signature.end());
        if (candidate == 0) {
            first_signature = signature;
        } else if (signature != first_signature) {
            return false;
        }
    }
    return true;
}

struct Elimination final {
    bool won{false};
    Candidate winner{0};
    PrvFailure failure{PrvFailure::perfect_final_tie};
};

// The reference's eliminate, over complete expanded ballots.
Elimination eliminate(const std::vector<Group>& ballots, const std::vector<Candidate>& active,
                      const std::size_t count) {
    std::vector<Candidate> remaining = active;
    const std::uint64_t total = total_weight(ballots);
    Elimination result{};
    for (;;) {
        const std::vector<char> member = membership(remaining, count);
        const std::vector<Group> projected = project(ballots, member);
        Counts first(count, 0);
        for (const Group& group : projected) {
            if (!group.ranking.empty()) first[group.ranking[0]] += group.weight;
        }
        if (perfect_cycle(projected, remaining, count, total)) {
            result.failure = PrvFailure::perfect_condorcet_cycle;
            return result;
        }
        for (const Candidate candidate : remaining) {
            if (2 * first[candidate] > total) {
                result.won = true;
                result.winner = candidate;
                return result;
            }
        }
        if (remaining.size() == 2) {
            if (first[remaining[0]] == first[remaining[1]]) {
                result.failure = PrvFailure::perfect_final_tie;
                return result;
            }
            result.won = true;
            result.winner = first[remaining[0]] > first[remaining[1]] ? remaining[0] : remaining[1];
            return result;
        }
        const std::vector<Counts> positions = position_counts(projected, count, remaining.size());
        std::size_t lowest = 0;
        for (std::size_t index = 1; index < remaining.size(); ++index) {
            if (compare_vectors(positions[remaining[index]], positions[remaining[lowest]]) < 0) lowest = index;
        }
        std::size_t matching = 0;
        for (const Candidate candidate : remaining) {
            if (compare_vectors(positions[candidate], positions[remaining[lowest]]) == 0) ++matching;
        }
        if (matching != 1) {
            result.failure = PrvFailure::perfect_elimination_tie;
            return result;
        }
        remaining.erase(remaining.begin() + static_cast<std::ptrdiff_t>(lowest));
    }
}

Elimination run_mini(const std::vector<Group>& ballots, const std::vector<Candidate>& active, const std::size_t count) {
    const Expansion expansion = expand_ballots(ballots, active, count);
    if (!expansion.expanded) {
        Elimination failed{};
        failed.failure = expansion.failure;
        return failed;
    }
    const Elimination result = eliminate(expansion.groups, active, count);
    return result;
}

// The sign of sum(d[r] * 2^(n - 1 - r)): the reference's BigInt seed score
// difference, by carrying from the least significant rank.
int score_sign(const Counts& left, const Counts& right) noexcept {
    std::int64_t carry = 0;
    bool any_residue = false;
    for (std::size_t index = left.size(); index > 0; --index) {
        const std::int64_t difference =
            static_cast<std::int64_t>(left[index - 1]) - static_cast<std::int64_t>(right[index - 1]);
        const std::int64_t total = difference + carry;
        // Floor division by 2, so the residue is 0 or 1.
        std::int64_t quotient = total / 2;
        if (total < 0 && total % 2 != 0) --quotient;
        if (total - 2 * quotient != 0) any_residue = true;
        carry = quotient;
    }
    if (carry > 0) return 1;
    if (carry < 0) return -1;
    return any_residue ? 1 : 0;
}

// Orders candidates by descending seed score, then descending vector.
struct SeedOrder final {
    const std::vector<Counts>* vectors;
    bool operator()(const Candidate left, const Candidate right) const noexcept {
        const int sign = score_sign((*vectors)[left], (*vectors)[right]);
        if (sign != 0) return sign > 0;
        return compare_vectors((*vectors)[left], (*vectors)[right]) > 0;
    }
};

struct ByRanking final {
    bool operator()(const Group& left, const Group& right) const noexcept {
        const bool result = left.ranking < right.ranking;
        return result;
    }
};

std::vector<Group> merge_identical(std::vector<Group> groups) {
    std::sort(groups.begin(), groups.end(), ByRanking{});
    std::vector<Group> merged{};
    for (Group& group : groups) {
        if (!merged.empty() && merged.back().ranking == group.ranking) {
            merged.back().weight += group.weight;
        } else {
            merged.push_back(std::move(group));
        }
    }
    return merged;
}

} // namespace

void Ballots::add(const std::span<const Candidate> ranking) { add_copies(ranking, 1); }

void Ballots::add_copies(const std::span<const Candidate> ranking, const std::uint64_t copies) {
    choices_.insert(choices_.end(), ranking.begin(), ranking.end());
    ends_.push_back(choices_.size());
    copies_.push_back(copies);
}

std::size_t Ballots::distinct() const noexcept { return ends_.size(); }

std::span<const Candidate> Ballots::ranking(const std::size_t index) const noexcept {
    const std::size_t first = index == 0 ? 0 : ends_[index - 1];
    const std::span<const Candidate> result(choices_.data() + first, ends_[index] - first);
    return result;
}

std::uint64_t Ballots::copies(const std::size_t index) const noexcept { return copies_[index]; }

void Ballots::clear() noexcept {
    choices_.clear();
    ends_.clear();
    copies_.clear();
}

PrvOutcome::PrvOutcome(State state, const std::uint64_t rejected) noexcept
    : state_(std::move(state)), rejected_(rejected) {}

PrvOutcome PrvOutcome::won(const Candidate winner, const std::uint64_t rejected) noexcept {
    PrvOutcome result(State(std::in_place_index<0>, winner), rejected);
    return result;
}

PrvOutcome PrvOutcome::failed(const PrvFailure failure, const std::uint64_t rejected) noexcept {
    PrvOutcome result(State(std::in_place_index<1>, failure), rejected);
    return result;
}

const Candidate* PrvOutcome::winner() const noexcept {
    const Candidate* const result = std::get_if<Candidate>(&state_);
    return result;
}

const PrvFailure* PrvOutcome::failure() const noexcept {
    const PrvFailure* const result = std::get_if<PrvFailure>(&state_);
    return result;
}

std::uint64_t PrvOutcome::rejected_ballots() const noexcept { return rejected_; }

PrvOutcome count_preference_ranked(const std::uint32_t candidate_count, const Ballots& ballots,
                                   const PrvOptions& options) {
    if (candidate_count < 2) return PrvOutcome::failed(PrvFailure::too_few_candidates, 0);
    const std::size_t count = candidate_count;

    // Validation, weighted; totals are checked against largest_total.
    std::vector<Group> valid{};
    std::uint64_t rejected = 0;
    std::uint64_t accepted = 0;
    std::vector<char> seen(count, 0);
    for (std::size_t index = 0; index < ballots.distinct(); ++index) {
        const std::span<const Candidate> ranking = ballots.ranking(index);
        const std::uint64_t copies = ballots.copies(index);
        if (copies == 0) continue;
        bool ok = !ranking.empty();
        for (const Candidate candidate : ranking) {
            if (candidate >= count || seen[candidate] != 0) {
                ok = false;
                break;
            }
            seen[candidate] = 1;
        }
        for (const Candidate candidate : ranking) {
            if (candidate < count) seen[candidate] = 0;
        }
        std::uint64_t& tally = ok ? accepted : rejected;
        if (copies > largest_total - tally) return PrvOutcome::failed(PrvFailure::too_many_ballots, rejected);
        tally += copies;
        if (ok) valid.push_back(Group{std::vector<Candidate>(ranking.begin(), ranking.end()), copies});
    }
    if (accepted <= count) return PrvOutcome::failed(PrvFailure::too_few_valid_ballots, rejected);
    valid = merge_identical(std::move(valid));

    std::vector<Candidate> active(count);
    for (std::size_t index = 0; index < count; ++index) active[index] = static_cast<Candidate>(index);
    if (count <= options.bracket_threshold) {
        const Elimination result = run_mini(valid, active, count);
        if (!result.won) return PrvOutcome::failed(result.failure, rejected);
        return PrvOutcome::won(result.winner, rejected);
    }
    if (options.bracket_interval < 2) return PrvOutcome::failed(PrvFailure::invalid_bracket_interval, rejected);

    // Seeding over the valid ballots as cast.
    const std::vector<Counts> vectors = position_counts(valid, count, count);
    for (std::size_t left = 0; left < count; ++left) {
        for (std::size_t right = left + 1; right < count; ++right) {
            if (compare_vectors(vectors[left], vectors[right]) == 0)
                return PrvOutcome::failed(PrvFailure::perfect_seed_tie, rejected);
        }
    }
    std::vector<Candidate> order = active;
    std::sort(order.begin(), order.end(), SeedOrder{&vectors});
    const std::size_t width = options.bracket_interval - 1;
    std::vector<std::vector<Candidate>> segments{};
    for (std::size_t start = 0; start < order.size(); start += width) {
        const std::size_t end = std::min(order.size(), start + width);
        segments.emplace_back(order.begin() + static_cast<std::ptrdiff_t>(start),
                              order.begin() + static_cast<std::ptrdiff_t>(end));
    }
    bool have_promoted = false;
    Candidate promoted = 0;
    for (std::size_t index = segments.size(); index > 0; --index) {
        std::vector<Candidate> bracket = segments[index - 1];
        if (have_promoted) bracket.push_back(promoted);
        if (bracket.size() == 1) {
            promoted = bracket[0];
            have_promoted = true;
            continue;
        }
        const std::vector<char> member = membership(bracket, count);
        const std::vector<Group> projected = project(valid, member);
        const Elimination result = run_mini(projected, bracket, count);
        if (!result.won) return PrvOutcome::failed(result.failure, rejected);
        promoted = result.winner;
        have_promoted = true;
    }
    return PrvOutcome::won(promoted, rejected);
}

} // namespace stx_auxlib::voting
