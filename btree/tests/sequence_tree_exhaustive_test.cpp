// Exhaustive small-scope check of stx_btree's sequence tree (study
// btree/docs/ORDER_STATISTIC_RESEARCH.md §5.4, E0), ported from the
// research harness. From each start state, every sequence of operations up
// to a depth is applied to the tree and to a flat model (one height and
// state per row); after every operation the tree must equal the model in
// every query, every cursor walk and seek, and every invariant (I1-I6,
// study §8.2). The node capacities are the smallest the tree admits, so
// splits, merges, borrows, root growth and collapse, and dense and packed
// leaves all occur within the scope. This enumerates every case of that
// size; the general case rests on the study's proofs. Named cases cover the
// packed-field limits, which small rows cannot reach.
//
//   stx_btree_sequence_tree_exhaustive_test [--scope small|full]
//
// small (the default, as ctest runs it): depths 2 and 1; full: 3 and 2.
// Prints one line per configuration and exits non-zero at the first
// difference, naming the sequence.

#include "stx_btree/sequence_tree.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <vector>

namespace {

using stx_btree::HeightUnits;
using stx_btree::RowCount;
using stx_btree::RowState;
using stx_btree::detail::dense_height_limit;
using stx_btree::detail::InvariantReport;
using stx_btree::detail::packed_count_limit;
using stx_btree::detail::SequenceConfig;
using stx_btree::detail::SequenceExtent;
using stx_btree::detail::SequenceTree;
using stx_btree::detail::Units;

constexpr Units height_limit = stx_btree::detail::sequence_height_limit;

constexpr Units test_estimate = 3;
constexpr Units assigned_estimate = 5;

enum class OpKind : std::uint8_t { set_height, set_heights, insert, erase, forget, coarsen, assign };

struct Op final {
    OpKind kind{OpKind::set_height};
    RowCount row{};
    RowCount rows{};
    Units height{};
};

constexpr Units heights_alphabet[] = {0, 1, 3, 64, dense_height_limit, dense_height_limit + 1, height_limit, -1,
                                      height_limit + 1};
constexpr std::size_t heights_kinds = sizeof(heights_alphabet) / sizeof(heights_alphabet[0]);
constexpr HeightUnits batch_pattern[] = {HeightUnits{64}, HeightUnits{1}, HeightUnits{dense_height_limit + 1}};
constexpr RowCount insert_sizes[] = {1, 2, 4};
constexpr RowCount erase_sizes[] = {1, 2, 5};
constexpr RowCount forget_sizes[] = {1, 3};
constexpr RowCount coarsen_sizes[] = {1, 2, 4};
constexpr RowCount assign_sizes[] = {0, 3};

// The operations offered on a sequence of n rows, in a fixed order.
class Alphabet final {
public:
    [[nodiscard]] static std::size_t size(RowCount rows) noexcept {
        const std::size_t n = static_cast<std::size_t>(rows);
        const std::size_t total = heights_kinds * n + 3 * n + 3 * (n + 1) + 3 * n + 2 * n + 3 * n + 2;
        return total;
    }

    [[nodiscard]] static Op at(RowCount rows, std::size_t index) noexcept {
        const std::size_t n = static_cast<std::size_t>(rows);
        std::size_t rest = index;
        Op op{};
        if (rest < heights_kinds * n) {
            op.kind = OpKind::set_height;
            op.row = rest / heights_kinds;
            op.height = heights_alphabet[rest % heights_kinds];
            return op;
        }
        rest -= heights_kinds * n;
        if (rest < 3 * n) {
            op.kind = OpKind::set_heights;
            op.row = rest / 3;
            op.rows = rest % 3 + 1;
            return op;
        }
        rest -= 3 * n;
        if (rest < 3 * (n + 1)) {
            op.kind = OpKind::insert;
            op.row = rest / 3;
            op.rows = insert_sizes[rest % 3];
            return op;
        }
        rest -= 3 * (n + 1);
        if (rest < 3 * n) {
            op.kind = OpKind::erase;
            op.row = rest / 3;
            op.rows = erase_sizes[rest % 3];
            return op;
        }
        rest -= 3 * n;
        if (rest < 2 * n) {
            op.kind = OpKind::forget;
            op.row = rest / 2;
            op.rows = forget_sizes[rest % 2];
            return op;
        }
        rest -= 2 * n;
        if (rest < 3 * n) {
            op.kind = OpKind::coarsen;
            op.row = rest / 3;
            op.rows = coarsen_sizes[rest % 3];
            return op;
        }
        rest -= 3 * n;
        op.kind = OpKind::assign;
        op.rows = assign_sizes[rest % 2];
        return op;
    }
};

// One height and one state per row: the specification.
class Model final {
public:
    Model(Units estimate, Units total_limit, RowCount row_limit)
        : heights_(), states_(), estimate_(estimate), total_limit_(total_limit), row_limit_(row_limit) {}

    [[nodiscard]] RowCount count() const noexcept { return static_cast<RowCount>(heights_.size()); }
    [[nodiscard]] Units height(RowCount row) const noexcept { return heights_[static_cast<std::size_t>(row)]; }
    [[nodiscard]] RowState state(RowCount row) const noexcept { return states_[static_cast<std::size_t>(row)]; }
    [[nodiscard]] Units total() const noexcept { return offset_of(count()); }

    [[nodiscard]] Units offset_of(RowCount row) const noexcept {
        Units sum = 0;
        const RowCount end = row < count() ? row : count();
        for (RowCount index = 0; index < end; index += 1) {
            sum = sum + height(index);
        }
        return sum;
    }

    [[nodiscard]] RowCount row_at(Units offset) const noexcept {
        const RowCount n = count();
        if (n == 0 || offset < 0) {
            return 0;
        }
        if (offset >= total()) {
            return n - 1;
        }
        Units end = 0;
        for (RowCount row = 0; row < n; row += 1) {
            end = end + height(row);
            if (end > offset) {
                return row;
            }
        }
        return n - 1;
    }

    bool set_height(RowCount row, Units value) {
        if (row >= count() || value < 0 || value > height_limit) {
            return false;
        }
        const Units old = height(row);
        if (state(row) == RowState::measured && old == value) {
            return false;
        }
        if (total() - old > total_limit_ - value) {
            return false;
        }
        heights_[static_cast<std::size_t>(row)] = value;
        states_[static_cast<std::size_t>(row)] = RowState::measured;
        return true;
    }

    bool set_heights(RowCount row, std::span<const HeightUnits> values) {
        const RowCount wanted = static_cast<RowCount>(values.size());
        if (wanted == 0) {
            return true;
        }
        if (row >= count() || wanted > count() - row) {
            return false;
        }
        Units replacement = 0;
        for (const HeightUnits& value : values) {
            if (value.value < 0 || value.value > height_limit || replacement > total_limit_ - value.value) {
                return false;
            }
            replacement = replacement + value.value;
        }
        const Units replaced = offset_of(row + wanted) - offset_of(row);
        if (total() - replaced > total_limit_ - replacement) {
            return false;
        }
        for (RowCount index = 0; index < wanted; index += 1) {
            heights_[static_cast<std::size_t>(row + index)] = values[static_cast<std::size_t>(index)].value;
            states_[static_cast<std::size_t>(row + index)] = RowState::measured;
        }
        return true;
    }

    bool insert(RowCount row, RowCount rows) {
        if (rows == 0) {
            return true;
        }
        if (rows > static_cast<RowCount>(total_limit_ / estimate_) || rows > row_limit_ - count()) {
            return false;
        }
        const Units sum = static_cast<Units>(rows) * estimate_;
        if (total() > total_limit_ - sum) {
            return false;
        }
        const RowCount at = row < count() ? row : count();
        heights_.insert(heights_.begin() + static_cast<std::ptrdiff_t>(at), static_cast<std::size_t>(rows), estimate_);
        states_.insert(states_.begin() + static_cast<std::ptrdiff_t>(at), static_cast<std::size_t>(rows),
                       RowState::estimated);
        return true;
    }

    void erase(RowCount row, RowCount rows) {
        if (rows == 0 || row >= count()) {
            return;
        }
        const RowCount taken = rows < count() - row ? rows : count() - row;
        const std::ptrdiff_t first = static_cast<std::ptrdiff_t>(row);
        const std::ptrdiff_t last = static_cast<std::ptrdiff_t>(row + taken);
        heights_.erase(heights_.begin() + first, heights_.begin() + last);
        states_.erase(states_.begin() + first, states_.begin() + last);
    }

    bool forget(RowCount row, RowCount rows) {
        if (rows == 0 || row >= count()) {
            return true;
        }
        const RowCount taken = rows < count() - row ? rows : count() - row;
        if (taken > static_cast<RowCount>(total_limit_ / estimate_)) {
            return false;
        }
        const Units sum = static_cast<Units>(taken) * estimate_;
        const Units replaced = offset_of(row + taken) - offset_of(row);
        if (total() - replaced > total_limit_ - sum) {
            return false;
        }
        for (RowCount index = 0; index < taken; index += 1) {
            heights_[static_cast<std::size_t>(row + index)] = estimate_;
            states_[static_cast<std::size_t>(row + index)] = RowState::estimated;
        }
        return true;
    }

    void coarsen(RowCount row, RowCount rows) {
        if (rows == 0 || row >= count()) {
            return;
        }
        const RowCount taken = rows < count() - row ? rows : count() - row;
        const Units sum = offset_of(row + taken) - offset_of(row);
        const Units base = sum / static_cast<Units>(taken);
        const Units taller = sum % static_cast<Units>(taken);
        for (RowCount index = 0; index < taken; index += 1) {
            const Units extra = static_cast<Units>(index) < taller ? 1 : 0;
            heights_[static_cast<std::size_t>(row + index)] = base + extra;
            states_[static_cast<std::size_t>(row + index)] = RowState::approximate;
        }
    }

    bool assign(RowCount rows, Units estimate) {
        const Units usable = estimate < 1 || estimate > height_limit ? 64 : estimate;
        if (rows > static_cast<RowCount>(total_limit_ / usable) || rows > row_limit_) {
            return false;
        }
        heights_.assign(static_cast<std::size_t>(rows), usable);
        states_.assign(static_cast<std::size_t>(rows), RowState::estimated);
        estimate_ = usable;
        return true;
    }

private:
    std::vector<Units> heights_;
    std::vector<RowState> states_;
    Units estimate_;
    Units total_limit_;
    RowCount row_limit_;
};

template <typename Config>
class Checker final {
public:
    using Tree = SequenceTree<Config>;

    explicit Checker(const char* name) : name_(name), candidates_(), cases_(0) {}

    [[nodiscard]] std::uint64_t cases() const noexcept { return cases_; }

    // Applies op to both; false when their answers differ.
    [[nodiscard]] static bool apply(Tree& tree, Model& model, const Op& op) {
        switch (op.kind) {
        case OpKind::set_height: {
            const bool tree_result = tree.set_height(op.row, HeightUnits{op.height});
            const bool model_result = model.set_height(op.row, op.height);
            return tree_result == model_result;
        }
        case OpKind::set_heights: {
            const std::span<const HeightUnits> values(batch_pattern, static_cast<std::size_t>(op.rows));
            const bool tree_result = tree.set_heights(op.row, values);
            const bool model_result = model.set_heights(op.row, values);
            return tree_result == model_result;
        }
        case OpKind::insert: {
            const bool tree_result = tree.insert(op.row, op.rows);
            const bool model_result = model.insert(op.row, op.rows);
            return tree_result == model_result;
        }
        case OpKind::erase:
            tree.erase(op.row, op.rows);
            model.erase(op.row, op.rows);
            return true;
        case OpKind::forget: {
            const bool tree_result = tree.forget(op.row, op.rows);
            const bool model_result = model.forget(op.row, op.rows);
            return tree_result == model_result;
        }
        case OpKind::coarsen:
            tree.coarsen(op.row, op.rows);
            model.coarsen(op.row, op.rows);
            return true;
        case OpKind::assign: {
            const bool tree_result = tree.assign(op.rows, assigned_estimate);
            const bool model_result = model.assign(op.rows, assigned_estimate);
            return tree_result == model_result;
        }
        default:
            return false;
        }
    }

    // Every query, cursor walk and seek against the model; null when equal.
    [[nodiscard]] const char* compare(const Tree& tree, const Model& model) {
        const InvariantReport report = tree.check_invariants();
        if (!report.ok) {
            return report.failure;
        }
        const RowCount n = model.count();
        if (tree.count() != n || tree.total() != model.total()) {
            return "count or total";
        }
        RowCount measured = 0;
        for (RowCount row = 0; row < n; row += 1) {
            if (model.state(row) == RowState::measured) {
                measured += 1;
            }
        }
        if (tree.measured_count() != measured) {
            return "measured_count";
        }
        Units top = 0;
        for (RowCount row = 0; row < n; row += 1) {
            const SequenceExtent extent = tree.extent_of(row);
            if (extent.top != top || extent.height != model.height(row) || extent.state != model.state(row)) {
                return "extent_of";
            }
            if (tree.offset_of(row) != top) {
                return "offset_of";
            }
            top = top + model.height(row);
        }
        if (tree.offset_of(n) != top || tree.offset_of(n + 3) != top) {
            return "offset_of at the end";
        }
        candidates_.clear();
        candidates_.push_back(-1);
        candidates_.push_back(0);
        candidates_.push_back(top - 1);
        candidates_.push_back(top);
        candidates_.push_back(top + 1);
        Units boundary = 0;
        for (RowCount row = 0; row < n; row += 1) {
            candidates_.push_back(boundary - 1);
            candidates_.push_back(boundary);
            candidates_.push_back(boundary + 1);
            boundary = boundary + model.height(row);
        }
        for (const Units y : candidates_) {
            if (tree.row_at(y) != model.row_at(y)) {
                return "row_at";
            }
        }
        typename Tree::Cursor cursor{};
        tree.seek_row(cursor, 0);
        Units walk_top = 0;
        for (RowCount row = 0; row < n; row += 1) {
            if (cursor.at_end() || cursor.row() != row || cursor.top() != walk_top ||
                cursor.height() != model.height(row) || cursor.state() != model.state(row)) {
                return "cursor walk";
            }
            walk_top = walk_top + model.height(row);
            tree.advance(cursor);
        }
        if (!cursor.at_end()) {
            return "cursor walk end";
        }
        if (n == 0) {
            return nullptr;
        }
        typename Tree::Cursor finger{};
        for (const Units y : candidates_) {
            const RowCount expected = model.row_at(y);
            tree.seek_offset(cursor, y);
            if (cursor.at_end() || cursor.row() != expected || cursor.top() != model.offset_of(expected)) {
                return "seek_offset";
            }
            tree.seek_row(finger, n / 2);
            tree.seek_offset_from(finger, y);
            if (finger.at_end() || finger.row() != expected || finger.top() != model.offset_of(expected)) {
                return "seek_offset_from";
            }
        }
        return nullptr;
    }

    // Builds start state `start` in both through the same operations.
    [[nodiscard]] static bool build_start(Tree& tree, Model& model, int start) {
        bool same = true;
        if (start == 0) {
            return true;
        }
        if (start == 1) {
            same = apply(tree, model, Op{OpKind::assign, 0, 3, 0}) && same;
            same = apply(tree, model, Op{OpKind::insert, 0, 1, 0}) && same;
            return same;
        }
        RowCount rows = 9;
        if (start == 3) {
            rows = 30;
        }
        if (start == 4) {
            rows = 60;
        }
        same = tree.assign(rows, test_estimate) == model.assign(rows, test_estimate) && same;
        for (RowCount row = 0; row < rows; row += 1) {
            if (start == 2) {
                same = tree.set_height(row, HeightUnits{64 * static_cast<Units>(row + 1)}) ==
                           model.set_height(row, 64 * static_cast<Units>(row + 1)) &&
                       same;
            } else if (start == 3 && row % 2 == 0) {
                same = tree.set_height(row, HeightUnits{5 + static_cast<Units>(row)}) ==
                           model.set_height(row, 5 + static_cast<Units>(row)) &&
                       same;
            }
        }
        if (start == 4) {
            std::vector<HeightUnits> values;
            for (RowCount row = 0; row < rows; row += 1) {
                values.push_back(HeightUnits{static_cast<Units>((row * 37) % 11) * 7 + 1});
            }
            const std::span<const HeightUnits> all(values.data(), values.size());
            same = tree.set_heights(0, all) == model.set_heights(0, all) && same;
            tree.coarsen(10, 7);
            model.coarsen(10, 7);
            same = tree.insert(40, 3) == model.insert(40, 3) && same;
        }
        return same;
    }

    // Every sequence up to `depth` from `start`, by replay from the start.
    [[nodiscard]] bool run_start(int start, std::size_t depth) {
        {
            Tree tree(test_estimate);
            Model model(test_estimate, Tree::total_limit, Tree::row_limit);
            if (!build_start(tree, model, start)) {
                return report_start(start, "start state answers differ");
            }
            const char* start_difference = compare(tree, model);
            if (start_difference != nullptr) {
                return report_start(start, start_difference);
            }
        }
        std::vector<std::size_t> digits(depth, 0);
        std::size_t level = 0;
        while (true) {
            Tree tree(test_estimate);
            Model model(test_estimate, Tree::total_limit, Tree::row_limit);
            static_cast<void>(build_start(tree, model, start));
            for (std::size_t step = 0; step < level; step += 1) {
                const Op op = Alphabet::at(model.count(), digits[step]);
                static_cast<void>(apply(tree, model, op));
            }
            const std::size_t offered = Alphabet::size(model.count());
            if (digits[level] < offered) {
                const Op op = Alphabet::at(model.count(), digits[level]);
                const bool same = apply(tree, model, op);
                const char* difference = same ? compare(tree, model) : "returned results differ";
                cases_ += 1;
                if (difference != nullptr) {
                    return report(start, digits, level, difference);
                }
                if (level + 1 < depth) {
                    level += 1;
                    digits[level] = 0;
                } else {
                    digits[level] += 1;
                }
                continue;
            }
            if (level == 0) {
                return true;
            }
            level -= 1;
            digits[level] += 1;
        }
    }

private:
    [[nodiscard]] bool report_start(int start, const char* difference) const {
        std::fprintf(stderr, "FAILED %s start %d: %s\n", name_, start, difference);
        return false;
    }

    [[nodiscard]] bool report(int start, const std::vector<std::size_t>& digits, std::size_t level,
                              const char* difference) const {
        std::fprintf(stderr, "FAILED %s start %d: %s after", name_, start, difference);
        Tree tree(test_estimate);
        Model model(test_estimate, Tree::total_limit, Tree::row_limit);
        static_cast<void>(build_start(tree, model, start));
        for (std::size_t step = 0; step <= level; step += 1) {
            const Op op = Alphabet::at(model.count(), digits[step]);
            std::fprintf(stderr, " [kind %u row %llu rows %llu height %lld]", static_cast<unsigned>(op.kind),
                         static_cast<unsigned long long>(op.row), static_cast<unsigned long long>(op.rows),
                         static_cast<long long>(op.height));
            static_cast<void>(apply(tree, model, op));
        }
        std::fprintf(stderr, "\n");
        return false;
    }

    const char* name_;
    std::vector<Units> candidates_;
    std::uint64_t cases_;
};

template <typename Config>
bool run_config(const char* name, bool full) {
    Checker<Config> checker(name);
    const std::size_t shallow_depth = full ? 3 : 2;
    const std::size_t deep_depth = full ? 2 : 1;
    for (int start = 0; start <= 4; start += 1) {
        const std::size_t depth = start <= 2 ? shallow_depth : deep_depth;
        if (!checker.run_start(start, depth)) {
            return false;
        }
    }
    std::printf("PASS %s: %llu cases\n", name, static_cast<unsigned long long>(checker.cases()));
    return true;
}

// A run of `rows` estimated rows of 2 units (rows > packed_count_limit, so
// it is stored as two or more entries) whose row packed_count_limit is
// measured at 7, then coarsen(10, packed_count_limit). Checks, against values
// recorded before the coarsen: offset_of bit for bit at rows at or before 10
// and at or after the range's end (clamped to count()), the total, and
// Lemma S inside the range.
template <typename Tree>
bool check_long_run_coarsen(const char* name, const char* label, RowCount rows) {
    const RowCount limit = packed_count_limit;
    const RowCount first = 10;
    Tree tree(2);
    if (!tree.assign(rows, 2) || tree.run_count() < 2 || !tree.check_invariants().ok) {
        std::fprintf(stderr, "FAILED %s [%s]: long run not split\n", name, label);
        return false;
    }
    const RowCount probes[] = {0, 1, limit - 1, limit, limit + 1, rows - 1, rows};
    for (const RowCount row : probes) {
        const Units expected = 2 * static_cast<Units>(row);
        const bool at_row = tree.offset_of(row) == expected;
        const bool inverse = row == rows || (tree.row_at(expected) == row && tree.row_at(expected + 1) == row);
        if (!at_row || !inverse) {
            std::fprintf(stderr, "FAILED %s [%s]: long run offsets at %llu\n", name, label,
                         static_cast<unsigned long long>(row));
            return false;
        }
    }
    if (!tree.set_height(limit, HeightUnits{7})) {
        std::fprintf(stderr, "FAILED %s [%s]: set_height on a long run\n", name, label);
        return false;
    }
    const RowCount end = rows - first > limit ? first + limit : rows;
    const RowCount taken = end - first;
    // Rows outside [first, end), recorded before coarsening.
    const RowCount outside_rows[] = {0, 1, first - 1, first, end, end + 1, end + 7, rows - 1, rows};
    Units recorded[9]{};
    bool kept[9]{};
    for (std::size_t index = 0; index < 9; index += 1) {
        const RowCount row = outside_rows[index];
        kept[index] = (row <= first || row >= end) && row <= rows;
        recorded[index] = kept[index] ? tree.offset_of(row) : 0;
    }
    const Units total_before = tree.total();
    const Units start = tree.offset_of(first);
    const Units sum = tree.offset_of(end) - start;
    // The range holds the measured row: 2 units a row plus 5.
    if (sum != 2 * static_cast<Units>(taken) + 5 || start != 20) {
        std::fprintf(stderr, "FAILED %s [%s]: sums before coarsening\n", name, label);
        return false;
    }
    tree.coarsen(first, limit);
    if (tree.count() != rows || tree.total() != total_before) {
        std::fprintf(stderr, "FAILED %s [%s]: count or total changed by coarsening\n", name, label);
        return false;
    }
    for (std::size_t index = 0; index < 9; index += 1) {
        if (kept[index] && tree.offset_of(outside_rows[index]) != recorded[index]) {
            std::fprintf(stderr, "FAILED %s [%s]: offset outside the coarsened range moved at %llu\n", name, label,
                         static_cast<unsigned long long>(outside_rows[index]));
            return false;
        }
    }
    const Units base = sum / static_cast<Units>(taken);
    const Units taller = sum % static_cast<Units>(taken);
    const RowCount inside_rows[] = {0, 1, 2, 4, 5, 6, 8, taken - 1};
    for (const RowCount within : inside_rows) {
        const Units wanted = static_cast<Units>(within);
        const Units extra = wanted < taller ? wanted : taller;
        const Units top = start + wanted * base + extra;
        const Units height = base + (wanted < taller ? 1 : 0);
        const SequenceExtent extent = tree.extent_of(first + within);
        if (tree.offset_of(first + within) != top || extent.top != top || extent.height != height ||
            extent.state != RowState::approximate) {
            std::fprintf(stderr, "FAILED %s [%s]: Lemma S inside the coarsened range at %llu\n", name, label,
                         static_cast<unsigned long long>(within));
            return false;
        }
    }
    if (!tree.check_invariants().ok) {
        std::fprintf(stderr, "FAILED %s [%s]: invariants after coarsening\n", name, label);
        return false;
    }
    return true;
}

// The packed-field limits: a run past 2^24 - 1 rows and a run past
// 2^38 - 1 units, each split into entries by Lemma S.
template <typename Config>
bool run_packing_cases(const char* name) {
    using Tree = SequenceTree<Config>;
    const RowCount limit = packed_count_limit;
    // Named case "long run, interior coarsen": the coarsened range
    // [10, 10 + limit) lies inside the sequence and crosses the boundary
    // between the two packed entries of the long run.
    if (!check_long_run_coarsen<Tree>(name, "long run, interior coarsen", limit + 40)) {
        return false;
    }
    // Named case "long run, coarsen clamped at the end": the case the first
    // M4 run reported. coarsen(10, limit) on limit + 5 rows covers rows
    // [10, limit + 5) only (coarsen clamps to the end, as the model does);
    // the old check expected the range to end at 10 + limit.
    if (!check_long_run_coarsen<Tree>(name, "long run, coarsen clamped at the end", limit + 5)) {
        return false;
    }
    {
        const Units tall = static_cast<Units>(1) << 30;
        Tree tree(tall);
        if (!tree.assign(300, tall) || tree.run_count() < 2 || !tree.check_invariants().ok) {
            std::fprintf(stderr, "FAILED %s: heavy run not split\n", name);
            return false;
        }
        for (RowCount row = 0; row < 300; row += 1) {
            const Units expected = tall * static_cast<Units>(row);
            if (tree.offset_of(row) != expected || tree.row_at(expected) != row ||
                tree.row_at(expected + tall - 1) != row) {
                std::fprintf(stderr, "FAILED %s: heavy run offsets at %llu\n", name,
                             static_cast<unsigned long long>(row));
                return false;
            }
        }
    }
    std::printf("PASS %s: packing limits\n", name);
    return true;
}

} // namespace

int main(int argc, char** argv) {
    bool full = false;
    for (int index = 1; index < argc; index += 1) {
        if (std::strcmp(argv[index], "--scope") == 0 && index + 1 < argc) {
            full = std::strcmp(argv[index + 1], "full") == 0;
            index += 1;
        }
    }
    using Linear = SequenceConfig<4, 3, false, true>;
    using Counting = SequenceConfig<4, 3, true, true>;
    using OddPacked = SequenceConfig<5, 4, true, false>;
    using Limited = SequenceConfig<4, 3, true, true, (static_cast<Units>(1) << 33), 200>;
    bool passed = run_config<Linear>("linear-dense", full);
    passed = passed && run_config<Counting>("counting-dense", full);
    passed = passed && run_config<OddPacked>("counting-odd-packed", full);
    passed = passed && run_config<Limited>("counting-dense-limited", full);
    passed = passed && run_packing_cases<Counting>("counting-dense");
    passed = passed && run_packing_cases<OddPacked>("counting-odd-packed");
    return passed ? 0 : 1;
}
