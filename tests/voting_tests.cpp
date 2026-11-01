// Tests of Preference Ranked Voting (include/stx_auxlib/voting.hpp)
// against the reference itself: tests/reference/prv_cases.txt holds 420
// elections and the outcomes the original prv-core.mjs gives them
// (tests/reference/prv_reference.mjs). Every outcome must match.
//   stx_auxlib_voting_tests <path to prv_cases.txt>

#include "stx_auxlib/files.hpp"
#include "stx_auxlib/paths.hpp"
#include "stx_auxlib/text.hpp"
#include "stx_auxlib/voting.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace voting = stx_auxlib::voting;
namespace text = stx_auxlib::text;

void require(const bool condition, const char* const message, const std::size_t line) {
    if (condition) return;
    std::fprintf(stderr, "stx_auxlib voting: %s (case line %zu)\n", message, line);
    std::abort();
}

const char* failure_name(const voting::PrvFailure failure) {
    switch (failure) {
    case voting::PrvFailure::too_few_candidates:
        return "TOO_FEW_CANDIDATES";
    case voting::PrvFailure::too_few_valid_ballots:
        return "TOO_FEW_VALID_BALLOTS";
    case voting::PrvFailure::invalid_bracket_interval:
        return "INVALID_BRACKET_INTERVAL";
    case voting::PrvFailure::no_expansion_evidence:
        return "NO_EXPANSION_EVIDENCE";
    case voting::PrvFailure::perfect_expansion_tie:
        return "PERFECT_EXPANSION_TIE";
    case voting::PrvFailure::perfect_condorcet_cycle:
        return "PERFECT_CONDORCET_CYCLE";
    case voting::PrvFailure::perfect_final_tie:
        return "PERFECT_FINAL_TIE";
    case voting::PrvFailure::perfect_elimination_tie:
        return "PERFECT_ELIMINATION_TIE";
    case voting::PrvFailure::perfect_seed_tie:
        return "PERFECT_SEED_TIE";
    case voting::PrvFailure::too_many_ballots:
        return "TOO_MANY_BALLOTS";
    }
    return "UNKNOWN";
}

std::string outcome_text(const voting::PrvOutcome& outcome) {
    if (outcome.winner() != nullptr) return "winner " + text::to_text(*outcome.winner());
    std::string result = "failure ";
    result += failure_name(*outcome.failure());
    return result;
}

// One case line: "C threshold interval | b;b;... | outcome".
void check_case(const std::string_view line, const std::size_t number) {
    text::TextScanner scanner(line);
    std::uint64_t candidates = 0;
    std::uint64_t threshold = 0;
    std::uint64_t interval = 0;
    require(scanner.read_unsigned(candidates) && scanner.expect(" ") && scanner.read_unsigned(threshold) &&
                scanner.expect(" ") && scanner.read_unsigned(interval) && scanner.expect(" | "),
            "a case header did not parse", number);
    std::string_view ballots_text{};
    require(scanner.read_until("|", ballots_text) && scanner.expect("| "), "a case's ballots did not parse", number);
    const std::string_view expected = scanner.rest();

    voting::Ballots ballots{};
    text::TextScanner ballot_scanner(text::trim(ballots_text));
    std::vector<voting::Candidate> ranking{};
    while (!ballot_scanner.at_end()) {
        ranking.clear();
        if (!ballot_scanner.expect("-")) {
            std::uint64_t choice = 0;
            while (ballot_scanner.read_unsigned(choice)) {
                ranking.push_back(static_cast<voting::Candidate>(choice));
                if (!ballot_scanner.expect(" ")) break;
            }
        }
        ballots.add(ranking);
        if (!ballot_scanner.expect(";")) break;
    }
    require(ballot_scanner.at_end(), "a case's ballots had trailing text", number);

    voting::PrvOptions options{};
    options.bracket_threshold = threshold;
    options.bracket_interval = interval;
    const voting::PrvOutcome outcome =
        voting::count_preference_ranked(static_cast<std::uint32_t>(candidates), ballots, options);
    const std::string actual = outcome_text(outcome);
    if (actual != expected) {
        std::fprintf(stderr, "stx_auxlib voting: case line %zu: got \"%s\", reference \"%.*s\"\n", number,
                     actual.c_str(), static_cast<int>(expected.size()), expected.data());
        std::abort();
    }
}

void reference_cases(const char* const path) {
    const stx_auxlib::PathParse parsed = stx_auxlib::Path::parse(path);
    require(parsed.succeeded(), "the case path did not parse", 0);
    std::vector<std::byte> bytes{};
    require(stx_auxlib::read_file(*parsed.path(), 16 << 20, bytes) == stx_auxlib::FileStatus::ok,
            "the case file could not be read", 0);
    const std::string_view content(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    std::size_t start = 0;
    std::size_t number = 0;
    while (start < content.size()) {
        std::size_t end = content.find('\n', start);
        if (end == std::string_view::npos) end = content.size();
        const std::string_view line = text::trim(content.substr(start, end - start));
        ++number;
        if (!line.empty()) check_case(line, number);
        start = end + 1;
    }
    require(number >= 420, "fewer reference cases than expected", number);
}

// Weighted copies give the same outcome as the same ballots added singly,
// and a large weighted electorate counts quickly.
void weighted_ballots() {
    const voting::Candidate orders[4][5] = {
        {0, 1, 2, 3, 4}, {1, 0, 2, 4, 3}, {2, 3, 1, 0, 4}, {4, 2, 1, 0, 3}};
    const std::uint64_t weights[4] = {7, 6, 5, 4};
    voting::Ballots single{};
    voting::Ballots weighted{};
    for (std::size_t bloc = 0; bloc < 4; ++bloc) {
        const std::size_t length = 2 + bloc;
        const std::span<const voting::Candidate> ranking(orders[bloc], length);
        for (std::uint64_t copy = 0; copy < weights[bloc]; ++copy) single.add(ranking);
        weighted.add_copies(ranking, weights[bloc]);
    }
    const voting::PrvOutcome a = voting::count_preference_ranked(5, single);
    const voting::PrvOutcome b = voting::count_preference_ranked(5, weighted);
    require(outcome_text(a) == outcome_text(b), "weighted ballots changed the outcome", 0);

    voting::Ballots large{};
    std::uint64_t state = 12345;
    std::vector<voting::Candidate> ranking{};
    for (std::size_t distinct = 0; distinct < 400; ++distinct) {
        ranking = {0, 1, 2, 3, 4, 5, 6, 7};
        for (std::size_t index = ranking.size() - 1; index > 0; --index) {
            state = (state ^ (state << 13)) ^ ((state ^ (state << 13)) >> 7);
            state ^= state << 17;
            const std::size_t other = static_cast<std::size_t>(state % (index + 1));
            const voting::Candidate held = ranking[index];
            ranking[index] = ranking[other];
            ranking[other] = held;
        }
        ranking.resize(1 + static_cast<std::size_t>(state % 8));
        large.add_copies(ranking, 1000 + state % 9000);
    }
    const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    const voting::PrvOutcome outcome = voting::count_preference_ranked(8, large);
    const std::chrono::duration<double, std::milli> elapsed = std::chrono::steady_clock::now() - start;
    std::printf("400 distinct rankings, about two million voters: %s in %.1f ms\n", outcome_text(outcome).c_str(),
                elapsed.count());
}

} // namespace

int main(const int argument_count, char** const arguments) {
    if (argument_count != 2) {
        std::fprintf(stderr, "usage: stx_auxlib_voting_tests <prv_cases.txt>\n");
        return 2;
    }
    reference_cases(arguments[1]);
    weighted_ballots();
    std::puts("stx_auxlib voting tests passed");
    return 0;
}
