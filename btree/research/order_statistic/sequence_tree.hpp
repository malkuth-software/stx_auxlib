#pragma once

// Research prototype of stx_btree's sequence tree with summed heights
// (btree/docs/ORDER_STATISTIC_RESEARCH.md §3 and §8). It is experiment code
// for the M4 harness, not the package: the production form will use
// stx_btree's page allocators, wrapper types for rows and heights, and the
// node stacks of detail/node_stack.hpp.
//
// The sequence is a list of rows. Each row has a height in 1/64 logical
// units (26.6 fixed point) and a state. Rows are kept as runs: a run of
// `count` rows with total height `total` gives its row i the height
// q + [i < s], where q = total / count and s = total % count (Lemma S in the
// study: splitting a run at any row keeps every row's height). Estimated
// runs are uniform (s = 0); a measured row is a run of one; a coarsened
// ("approximate") run is any (count, total).
//
// Leaves are 8-byte packed runs (count 24 bits, total 38 bits, state 2 bits)
// or, when enabled, dense leaves of measured single rows (4 bytes a row).
// Branches keep, for each child, the prefix of row counts and the prefix of
// heights, both exact unsigned integers below 2^62, and slots past the length
// hold all ones. A descent picks a child by counting prefixes at most (or
// below) the target over the slots, branch-free when the configuration asks.
//
// Rebalancing follows weak B-trees (Huddleston and Mehlhorn 1982): a node
// holds between a minimum and a capacity of at least twice the minimum;
// an underfull node merges with or borrows from a sibling; the root branch
// keeps at least two children and collapses when it has one.
//
// Failure: an edit that would break a limit (a row height outside
// [0, height_limit], a total past the configuration's total limit, a row
// count past its row limit) is refused with false and leaves the tree
// unchanged. Page allocation failure throws std::bad_alloc; in this
// prototype a throw in the middle of an edit may leave the tree inconsistent
// (the production form reserves every page first, as stx_btree's maps do).
//
// One thread. A cursor is valid until the next edit; using it afterwards is
// detected by the revision and answered by a fresh seek.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

namespace stx_btree_research {

// Rows and row positions, and heights and offsets in 1/64 logical units.
// The production API separates them with wrapper types (study §3.6); the
// prototype keeps distinct signedness and names only.
using RowCount = std::uint64_t;
using Units = std::int64_t;

enum class RowState : std::uint8_t { estimated = 0, measured = 1, approximate = 2 };

inline constexpr Units default_total_limit = (static_cast<Units>(1) << 62) - 1;
inline constexpr RowCount default_row_limit = (static_cast<RowCount>(1) << 62) - 1;
inline constexpr RowCount packed_count_limit = (static_cast<RowCount>(1) << 24) - 1;
inline constexpr Units packed_total_limit = (static_cast<Units>(1) << 38) - 1;
inline constexpr Units dense_height_limit = static_cast<Units>(0xffffffffU);
// The tallest single row: one packed entry holds it.
inline constexpr Units height_limit = packed_total_limit;

struct Run final {
    RowCount count{};
    Units total{};
    RowState state{RowState::estimated};
};

// Lemma S arithmetic on one run. count >= 1 everywhere.
class RunArithmetic final {
public:
    [[nodiscard]] static Units height_at(const Run& run, RowCount row) noexcept {
        const Units count = static_cast<Units>(run.count);
        const Units base = run.total / count;
        const Units taller = run.total % count;
        const Units extra = static_cast<Units>(row) < taller ? 1 : 0;
        const Units height = base + extra;
        return height;
    }

    // The heights of rows [0, rows) of the run, rows <= count.
    [[nodiscard]] static Units prefix(const Run& run, RowCount rows) noexcept {
        const Units count = static_cast<Units>(run.count);
        const Units base = run.total / count;
        const Units taller = run.total % count;
        const Units wanted = static_cast<Units>(rows);
        const Units extra = wanted < taller ? wanted : taller;
        const Units sum = wanted * base + extra;
        return sum;
    }

    // The row i with prefix(i) <= offset < prefix(i + 1), for
    // 0 <= offset < total. Rows past the taller ones have base > 0 here,
    // since offset < total.
    [[nodiscard]] static RowCount inverse(const Run& run, Units offset) noexcept {
        const Units count = static_cast<Units>(run.count);
        const Units base = run.total / count;
        const Units taller = run.total % count;
        const Units wide = taller * (base + 1);
        if (offset < wide) {
            const Units row = offset / (base + 1);
            return static_cast<RowCount>(row);
        }
        const Units row = taller + (offset - wide) / base;
        return static_cast<RowCount>(row);
    }

    // The most rows i <= count with prefix(i) <= limit, for limit >= 0.
    [[nodiscard]] static RowCount rows_within(const Run& run, Units limit) noexcept {
        if (limit >= run.total) {
            return run.count;
        }
        const Units count = static_cast<Units>(run.count);
        const Units base = run.total / count;
        const Units taller = run.total % count;
        const Units wide = taller * (base + 1);
        if (limit < wide) {
            const Units rows = limit / (base + 1);
            return static_cast<RowCount>(rows);
        }
        // limit < total and limit >= wide leave base > 0.
        const Units rows = taller + (limit - wide) / base;
        return static_cast<RowCount>(rows);
    }

    // Splits off rows [0, rows) of the run (0 < rows < count) and returns
    // them; the run keeps the rest. Every row keeps its height (Lemma S).
    [[nodiscard]] static Run take_front(Run& run, RowCount rows) noexcept {
        const Units front_total = prefix(run, rows);
        const Run front{rows, front_total, run.state};
        run.count = run.count - rows;
        run.total = run.total - front_total;
        return front;
    }

    [[nodiscard]] static bool uniform(const Run& run) noexcept {
        const bool even = run.total % static_cast<Units>(run.count) == 0;
        return even;
    }
};

class PackedEntry final {
public:
    [[nodiscard]] static std::uint64_t encode(const Run& run) noexcept {
        const std::uint64_t count = static_cast<std::uint64_t>(run.count);
        const std::uint64_t total = static_cast<std::uint64_t>(run.total) << 24U;
        const std::uint64_t state = static_cast<std::uint64_t>(run.state) << 62U;
        const std::uint64_t word = count | total | state;
        return word;
    }
    [[nodiscard]] static Run decode(std::uint64_t word) noexcept {
        Run run{};
        run.count = static_cast<RowCount>(word & 0xffffffU);
        run.total = static_cast<Units>((word >> 24U) & ((static_cast<std::uint64_t>(1) << 38U) - 1U));
        run.state = static_cast<RowState>(word >> 62U);
        return run;
    }
};

// The declared choices of a prototype variant.
template <std::size_t BranchSlots, std::size_t LeafWords, bool CountingSearch, bool DenseLeaves,
          Units TotalLimit = default_total_limit, RowCount RowLimit = default_row_limit>
struct TreeConfig final {
    static constexpr std::size_t branch_slots = BranchSlots;
    static constexpr std::size_t leaf_words = LeafWords;
    static constexpr bool counting_search = CountingSearch;
    static constexpr bool dense_leaves = DenseLeaves;
    static constexpr Units total_limit = TotalLimit;
    static constexpr RowCount row_limit = RowLimit;
};

// Prefix counts over a node's slots: how many of the first `length` slots
// are at most (or below) a target. Slots past `length` hold all ones and
// every live slot and target is below 2^62.
template <std::size_t Slots, bool Counting>
class PrefixCount final {
public:
    [[nodiscard]] static std::size_t at_most(const std::uint64_t* slots, std::size_t length,
                                             std::uint64_t target) noexcept {
        if constexpr (Counting) {
            const std::size_t counted = count_all(slots, target, true);
            const std::size_t live = counted - (Slots - length);
            return live;
        } else {
            std::size_t index = 0;
            while (index < length && slots[index] <= target) {
                index += 1;
            }
            return index;
        }
    }

    [[nodiscard]] static std::size_t below(const std::uint64_t* slots, std::size_t length,
                                           std::uint64_t target) noexcept {
        if constexpr (Counting) {
            const std::size_t counted = count_all(slots, target, false);
            const std::size_t live = counted - (Slots - length);
            return live;
        } else {
            std::size_t index = 0;
            while (index < length && slots[index] < target) {
                index += 1;
            }
            return index;
        }
    }

private:
    // Counts every slot, padding included. For values below 2^63, slot <= target
    // exactly when target - slot does not wrap (its top bit is clear), and
    // slot < target exactly when slot - target wraps. Padding (all ones) is
    // counted by both tests for any target below 2^62, so the caller
    // subtracts the padding slots. The subtraction is modular by design.
    [[nodiscard]] static std::size_t count_all(const std::uint64_t* slots, std::uint64_t target,
                                               bool at_most_target) noexcept {
#if defined(__GNUC__) || defined(__clang__)
        typedef std::uint64_t Lanes __attribute__((vector_size(16)));
        const Lanes broadcast = Lanes{} + target;
        const Lanes ones = Lanes{} + static_cast<std::uint64_t>(1);
        const Lanes sign = Lanes{} + static_cast<std::uint64_t>(63);
        Lanes counted = {};
        std::size_t pair = 0;
        for (pair = 0; pair + 2 <= Slots; pair += 2) {
            Lanes lanes;
            std::memcpy(&lanes, slots + pair, sizeof(Lanes));
            if (at_most_target) {
                counted += ((broadcast - lanes) >> sign) ^ ones;
            } else {
                counted += (lanes - broadcast) >> sign;
            }
        }
        std::uint64_t total = counted[0] + counted[1];
#else
        std::uint64_t total = 0;
        std::size_t pair = 0;
        for (pair = 0; pair + 2 <= Slots; pair += 2) {
            for (std::size_t lane = 0; lane < 2; lane += 1) {
                const std::uint64_t slot = slots[pair + lane];
                if (at_most_target) {
                    total += ((target - slot) >> 63U) ^ 1U;
                } else {
                    total += (slot - target) >> 63U;
                }
            }
        }
#endif
        if constexpr (Slots % 2 != 0) {
            const std::uint64_t last = slots[Slots - 1];
            if (at_most_target) {
                total += ((target - last) >> 63U) ^ 1U;
            } else {
                total += (last - target) >> 63U;
            }
        }
        return static_cast<std::size_t>(total);
    }
};

template <typename Config>
struct LeafPage final {
    // Entries (packed) or rows (dense).
    std::uint32_t length{};
    std::uint32_t dense{};
    // Packed entry i is words[i]; dense row i is the (i % 2)th half of words[i / 2].
    std::uint64_t words[Config::leaf_words]{};
};

template <typename Config>
struct BranchPage final {
    std::uint32_t length{};
    // The node's height: 1 when its children are leaves.
    std::uint32_t level{};
    std::uint64_t count_prefix[Config::branch_slots]{};
    std::uint64_t height_prefix[Config::branch_slots]{};
    void* children[Config::branch_slots]{};
};

// Where a row is: its top, its height and its state.
struct RowExtent final {
    Units top{};
    Units height{};
    RowState state{RowState::estimated};
};

struct InvariantReport final {
    bool ok{true};
    const char* failure{""};
};

struct TreeCounters final {
    std::uint64_t descents{};
    std::uint64_t node_visits{};
};

template <typename Config>
class SequenceTree final {
public:
    using Leaf = LeafPage<Config>;
    using Branch = BranchPage<Config>;
    static constexpr std::size_t branch_slots = Config::branch_slots;
    static constexpr std::size_t packed_capacity = Config::leaf_words;
    static constexpr std::size_t dense_capacity = Config::dense_leaves ? Config::leaf_words * 2 : 0;
    static constexpr std::size_t leaf_minimum = packed_capacity / 4 > 1 ? packed_capacity / 4 : 1;
    static constexpr std::size_t branch_minimum = branch_slots / 4 > 2 ? branch_slots / 4 : 2;
    // Every non-root branch has at least two children and every leaf at
    // least one row, and rows stay below 2^62.
    static constexpr std::size_t maximum_levels = 64;
    static constexpr Units total_limit = Config::total_limit;
    static constexpr RowCount row_limit = Config::row_limit;

    static_assert(branch_slots >= 4 && branch_slots >= 2 * branch_minimum, "weak B-tree branches: b >= 2a");
    static_assert(packed_capacity >= 3 && packed_capacity >= 2 * leaf_minimum, "weak B-tree leaves: b >= 2a");
    static_assert(total_limit <= default_total_limit && row_limit <= default_row_limit,
                  "prefix slots stay below 2^62 for the counting search");

    using Count = PrefixCount<Config::branch_slots, Config::counting_search>;

    struct CursorLevel final {
        const Branch* node{};
        std::uint32_t slot{};
        RowCount row_start{};
        Units offset_start{};
    };

    // A position in the sequence, with the path to it. Valid while
    // revision equals the tree's revision.
    struct Cursor final {
        CursorLevel levels[maximum_levels]{};
        std::uint32_t depth{};
        const Leaf* leaf{};
        RowCount leaf_row_start{};
        Units leaf_offset_start{};
        // The entry (packed) or row (dense) within the leaf, and the row within the entry.
        std::uint32_t entry{};
        RowCount within{};
        Run run{};
        RowCount row{};
        Units top{};
        std::uint64_t revision{};
        bool at_end{true};
    };

    explicit SequenceTree(Units estimate)
        : root_(nullptr), height_(0), rows_(0), total_(0), leaves_(0), branches_(0), entries_(0), revision_(0),
          estimate_(usable_estimate(estimate)), runs_(), pieces_(), edit_(), made_(), carry_(), kids_(), path_(),
          counters_() {
        runs_.reserve(4 * (dense_capacity + packed_capacity) + 16);
        pieces_.reserve(4 * (dense_capacity + packed_capacity) + 16);
        edit_.reserve(4 * (dense_capacity + packed_capacity) + 16);
        made_.reserve(3 * branch_slots + 8);
        carry_.reserve(3 * branch_slots + 8);
        kids_.reserve(3 * branch_slots + 8);
        root_ = new_leaf();
    }
    ~SequenceTree() { release_all(); }
    SequenceTree(const SequenceTree&) = delete;
    SequenceTree& operator=(const SequenceTree&) = delete;
    SequenceTree(SequenceTree&&) = delete;
    SequenceTree& operator=(SequenceTree&&) = delete;

    // ---- queries ------------------------------------------------------------

    [[nodiscard]] RowCount count() const noexcept { return rows_; }
    [[nodiscard]] Units total() const noexcept { return total_; }
    [[nodiscard]] Units estimate() const noexcept { return estimate_; }
    [[nodiscard]] std::uint64_t revision() const noexcept { return revision_; }

    // The heights of rows [0, row); row >= count() gives total().
    [[nodiscard]] Units offset_of(RowCount row) const noexcept {
        if (row >= rows_) {
            return total_;
        }
        note_descent();
        const void* node = root_;
        Units offset = 0;
        RowCount remaining = row;
        std::uint32_t level = height_;
        while (level > 0) {
            const Branch& branch = *static_cast<const Branch*>(node);
            note_visit();
            const std::size_t child = Count::at_most(branch.count_prefix, branch.length, remaining);
            if (child > 0) {
                remaining = remaining - branch.count_prefix[child - 1];
                offset = offset + static_cast<Units>(branch.height_prefix[child - 1]);
            }
            node = branch.children[child];
            level -= 1;
        }
        note_visit();
        const Units within = leaf_prefix(*static_cast<const Leaf*>(node), remaining);
        const Units found = offset + within;
        return found;
    }

    // For 0 <= offset < total(): the least row r with offset_of(r + 1) >
    // offset. Below 0: row 0. At or past total(): the last row. Empty: 0.
    [[nodiscard]] RowCount row_at(Units offset) const noexcept {
        if (rows_ == 0 || offset < 0) {
            return 0;
        }
        if (offset >= total_) {
            return rows_ - 1;
        }
        note_descent();
        const void* node = root_;
        RowCount row = 0;
        Units remaining = offset;
        std::uint32_t level = height_;
        while (level > 0) {
            const Branch& branch = *static_cast<const Branch*>(node);
            note_visit();
            const std::size_t child =
                Count::at_most(branch.height_prefix, branch.length, static_cast<std::uint64_t>(remaining));
            if (child > 0) {
                row = row + branch.count_prefix[child - 1];
                remaining = remaining - static_cast<Units>(branch.height_prefix[child - 1]);
            }
            node = branch.children[child];
            level -= 1;
        }
        note_visit();
        const RowCount within = leaf_row_at(*static_cast<const Leaf*>(node), remaining);
        const RowCount found = row + within;
        return found;
    }

    // Past the end: the bottom, height 0.
    [[nodiscard]] RowExtent extent_of(RowCount row) const noexcept {
        if (row >= rows_) {
            const RowExtent bottom{total_, 0, RowState::estimated};
            return bottom;
        }
        note_descent();
        const void* node = root_;
        Units top = 0;
        RowCount remaining = row;
        std::uint32_t level = height_;
        while (level > 0) {
            const Branch& branch = *static_cast<const Branch*>(node);
            note_visit();
            const std::size_t child = Count::at_most(branch.count_prefix, branch.length, remaining);
            if (child > 0) {
                remaining = remaining - branch.count_prefix[child - 1];
                top = top + static_cast<Units>(branch.height_prefix[child - 1]);
            }
            node = branch.children[child];
            level -= 1;
        }
        note_visit();
        const Leaf& leaf = *static_cast<const Leaf*>(node);
        RowExtent extent{};
        for (std::uint32_t index = 0; index < leaf.length; index += 1) {
            const Run run = entry_run(leaf, index);
            if (remaining < run.count) {
                extent.top = top + RunArithmetic::prefix(run, remaining);
                extent.height = RunArithmetic::height_at(run, remaining);
                extent.state = run.state;
                return extent;
            }
            remaining = remaining - run.count;
            top = top + run.total;
        }
        // Past the end: the bottom, height 0.
        extent.top = total_;
        return extent;
    }

    // ---- cursors --------------------------------------------------------------

    // Places the cursor at `row` (past the end: at_end).
    void seek_row(Cursor& cursor, RowCount row) const noexcept {
        cursor.revision = revision_;
        if (row >= rows_) {
            cursor.at_end = true;
            cursor.row = rows_;
            cursor.top = total_;
            return;
        }
        note_descent();
        cursor.depth = height_;
        cursor.at_end = false;
        const void* node = root_;
        RowCount row_start = 0;
        Units offset_start = 0;
        RowCount remaining = row;
        std::uint32_t level = 0;
        while (level < height_) {
            const Branch& branch = *static_cast<const Branch*>(node);
            note_visit();
            const std::size_t child = Count::at_most(branch.count_prefix, branch.length, remaining);
            cursor.levels[level] = CursorLevel{&branch, static_cast<std::uint32_t>(child), row_start, offset_start};
            if (child > 0) {
                remaining = remaining - branch.count_prefix[child - 1];
                row_start = row_start + branch.count_prefix[child - 1];
                offset_start = offset_start + static_cast<Units>(branch.height_prefix[child - 1]);
            }
            node = branch.children[child];
            level += 1;
        }
        note_visit();
        cursor.leaf = static_cast<const Leaf*>(node);
        cursor.leaf_row_start = row_start;
        cursor.leaf_offset_start = offset_start;
        place_in_leaf_by_row(cursor, remaining);
    }

    // Places the cursor at row_at(offset).
    void seek_offset(Cursor& cursor, Units offset) const noexcept {
        if (rows_ == 0 || offset < 0) {
            seek_row(cursor, 0);
            return;
        }
        if (offset >= total_) {
            seek_row(cursor, rows_ - 1);
            return;
        }
        note_descent();
        cursor.revision = revision_;
        cursor.depth = height_;
        cursor.at_end = false;
        descend_cursor_by_offset(cursor, 0, root_, 0, 0, offset);
    }

    // Moves a valid cursor to row_at(offset), climbing only as far as the
    // target needs: O(log_B d) nodes for a target d rows away. A stale or
    // finished cursor seeks from the root.
    void seek_offset_from(Cursor& cursor, Units offset) const noexcept {
        if (cursor.revision != revision_ || cursor.at_end || rows_ == 0 || offset < 0 || offset >= total_) {
            seek_offset(cursor, offset);
            return;
        }
        note_descent();
        std::uint32_t level = cursor.depth;
        // Climb while the target lies outside the node at `level`.
        Units node_start = cursor.leaf_offset_start;
        Units node_end = cursor.leaf_offset_start + leaf_total_from_cursor(cursor);
        while (level > 0 && (offset < node_start || offset >= node_end)) {
            level -= 1;
            const CursorLevel& above = cursor.levels[level];
            node_start = above.offset_start;
            node_end = above.offset_start + static_cast<Units>((*above.node).height_prefix[(*above.node).length - 1]);
        }
        if (level == cursor.depth) {
            // The target is in the current leaf.
            place_in_leaf_by_offset(cursor, offset - cursor.leaf_offset_start);
            return;
        }
        const CursorLevel& from = cursor.levels[level];
        descend_cursor_by_offset(cursor, level, from.node, from.row_start, from.offset_start,
                                 offset - from.offset_start);
    }

    // The next row; at_end after the last.
    void advance(Cursor& cursor) const noexcept {
        if (cursor.at_end) {
            return;
        }
        cursor.top = cursor.top + RunArithmetic::height_at(cursor.run, cursor.within);
        cursor.row = cursor.row + 1;
        if (cursor.row >= rows_) {
            cursor.at_end = true;
            return;
        }
        cursor.within = cursor.within + 1;
        if (cursor.within < cursor.run.count) {
            return;
        }
        cursor.within = 0;
        cursor.entry = cursor.entry + 1;
        if (cursor.entry < (*cursor.leaf).length) {
            cursor.run = entry_run(*cursor.leaf, cursor.entry);
            return;
        }
        // The next leaf: climb to the first level with a right neighbour,
        // then descend leftmost.
        std::uint32_t level = cursor.depth;
        while (level > 0) {
            CursorLevel& above = cursor.levels[level - 1];
            if (above.slot + 1 < (*above.node).length) {
                above.slot = above.slot + 1;
                break;
            }
            level -= 1;
        }
        // level >= 1 here: a row remains, so some level has a right neighbour.
        std::uint32_t walk = level - 1;
        while (true) {
            const CursorLevel& at = cursor.levels[walk];
            const Branch& branch = *at.node;
            note_visit();
            const RowCount child_row = at.row_start + (at.slot > 0 ? branch.count_prefix[at.slot - 1] : 0);
            const Units child_offset =
                at.offset_start + (at.slot > 0 ? static_cast<Units>(branch.height_prefix[at.slot - 1]) : 0);
            const void* child = branch.children[at.slot];
            if (walk + 1 == cursor.depth) {
                cursor.leaf = static_cast<const Leaf*>(child);
                cursor.leaf_row_start = child_row;
                cursor.leaf_offset_start = child_offset;
                break;
            }
            cursor.levels[walk + 1] = CursorLevel{static_cast<const Branch*>(child), 0, child_row, child_offset};
            walk += 1;
        }
        cursor.entry = 0;
        cursor.run = entry_run(*cursor.leaf, 0);
    }

    // ---- edits --------------------------------------------------------------

    // Replaces every row with `rows` estimated rows at `estimate`, which
    // becomes the estimate. Refused (unchanged) past the limits.
    bool assign(RowCount rows, Units estimate) {
        const Units usable = usable_estimate(estimate);
        Units sum = 0;
        if (!estimated_total(rows, usable, sum) || rows > row_limit) {
            return false;
        }
        release_all();
        root_ = new_leaf();
        estimate_ = usable;
        revision_ += 1;
        if (rows > 0) {
            insert_run(0, Run{rows, sum, RowState::estimated});
        }
        return true;
    }

    // Records a measured height. Refused (false, unchanged) past the end,
    // outside [0, height_limit] or past the total limit; false also when
    // the row is already measured at that height.
    bool set_height(RowCount row, Units height) {
        if (row >= rows_ || height < 0 || height > height_limit) {
            return false;
        }
        descend_for_edit(row, false);
        decode_leaf(*path_.leaf, runs_);
        std::size_t entry = 0;
        RowCount within = 0;
        locate_row(runs_, row - path_.leaf_row_start, entry, within);
        const Run found = runs_[entry];
        const Units old_height = RunArithmetic::height_at(found, within);
        if (found.state == RowState::measured && old_height == height) {
            return false;
        }
        if (total_ - old_height > total_limit - height) {
            return false;
        }
        Run rest = found;
        edit_.clear();
        for (std::size_t index = 0; index < entry; index += 1) {
            edit_.push_back(runs_[index]);
        }
        if (within > 0) {
            edit_.push_back(RunArithmetic::take_front(rest, within));
        }
        const std::size_t edit_index = edit_.size();
        edit_.push_back(Run{1, height, RowState::measured});
        if (rest.count > 1) {
            static_cast<void>(RunArithmetic::take_front(rest, 1));
            edit_.push_back(rest);
        }
        for (std::size_t index = entry + 1; index < runs_.size(); index += 1) {
            edit_.push_back(runs_[index]);
        }
        runs_.swap(edit_);
        normalize_runs();
        commit_leaf_edit(edit_index);
        revision_ += 1;
        return true;
    }

    // Records measured heights for rows [row, row + heights.size()), one
    // leaf at a time. Refused as a whole (unchanged) if any height or the
    // range is invalid or the total would pass the limit.
    bool set_heights(RowCount row, std::span<const Units> heights) {
        const RowCount wanted = static_cast<RowCount>(heights.size());
        if (wanted == 0) {
            return true;
        }
        if (row >= rows_ || wanted > rows_ - row) {
            return false;
        }
        Units replacement = 0;
        for (const Units height : heights) {
            if (height < 0 || height > height_limit || replacement > total_limit - height) {
                return false;
            }
            replacement = replacement + height;
        }
        const Units replaced = offset_of(row + wanted) - offset_of(row);
        if (total_ - replaced > total_limit - replacement) {
            return false;
        }
        RowCount done = 0;
        while (done < wanted) {
            descend_for_edit(row + done, false);
            const RowCount local = row + done - path_.leaf_row_start;
            const RowCount room = path_.leaf_rows - local;
            const RowCount here = wanted - done < room ? wanted - done : room;
            decode_leaf(*path_.leaf, runs_);
            const std::size_t gap = cut_rows(local, here);
            // cut_rows left edit_ with the rows outside the cut; put the new rows at the gap.
            runs_.clear();
            for (std::size_t index = 0; index < gap; index += 1) {
                runs_.push_back(edit_[index]);
            }
            for (RowCount index = 0; index < here; index += 1) {
                runs_.push_back(Run{1, heights[static_cast<std::size_t>(done + index)], RowState::measured});
            }
            for (std::size_t index = gap; index < edit_.size(); index += 1) {
                runs_.push_back(edit_[index]);
            }
            normalize_runs();
            commit_leaf_edit(gap);
            done = done + here;
        }
        revision_ += 1;
        return true;
    }

    // Inserts `rows` estimated rows before `row` (clamped to count()).
    bool insert(RowCount row, RowCount rows) {
        if (rows == 0) {
            return true;
        }
        Units sum = 0;
        if (!estimated_total(rows, estimate_, sum) || rows > row_limit - rows_ || total_ > total_limit - sum) {
            return false;
        }
        const RowCount at = row < rows_ ? row : rows_;
        insert_run(at, Run{rows, sum, RowState::estimated});
        revision_ += 1;
        return true;
    }

    // Erases rows [row, row + rows), clamped to the end.
    void erase(RowCount row, RowCount rows) {
        if (rows == 0 || row >= rows_) {
            return;
        }
        RowCount left = rows < rows_ - row ? rows : rows_ - row;
        while (left > 0) {
            descend_for_edit(row, false);
            const RowCount local = row - path_.leaf_row_start;
            const RowCount room = path_.leaf_rows - local;
            const RowCount here = left < room ? left : room;
            decode_leaf(*path_.leaf, runs_);
            const std::size_t gap = cut_rows(local, here);
            runs_.swap(edit_);
            normalize_runs();
            commit_leaf_edit(gap);
            left = left - here;
        }
        revision_ += 1;
    }

    // Returns rows [row, row + rows) to the estimate. Refused (unchanged)
    // if the total would pass the limit.
    bool forget(RowCount row, RowCount rows) {
        if (rows == 0 || row >= rows_) {
            return true;
        }
        const RowCount taken = rows < rows_ - row ? rows : rows_ - row;
        Units sum = 0;
        if (!estimated_total(taken, estimate_, sum)) {
            return false;
        }
        const Units replaced = offset_of(row + taken) - offset_of(row);
        if (total_ - replaced > total_limit - sum) {
            return false;
        }
        erase(row, taken);
        insert_run(row, Run{taken, sum, RowState::estimated});
        revision_ += 1;
        return true;
    }

    // Replaces rows [row, row + rows) by one approximate run with their
    // exact total. offset_of(x) is unchanged for every x <= row and every
    // x >= row + rows (study §4, C2).
    void coarsen(RowCount row, RowCount rows) {
        if (rows == 0 || row >= rows_) {
            return;
        }
        const RowCount taken = rows < rows_ - row ? rows : rows_ - row;
        const Units sum = offset_of(row + taken) - offset_of(row);
        erase(row, taken);
        insert_run(row, Run{taken, sum, RowState::approximate});
        revision_ += 1;
    }

    // ---- storage and checks --------------------------------------------------

    [[nodiscard]] std::size_t leaf_count() const noexcept { return leaves_; }
    [[nodiscard]] std::size_t branch_count() const noexcept { return branches_; }
    [[nodiscard]] std::size_t node_count() const noexcept { return leaves_ + branches_; }
    // Packed entries plus dense rows: the runs held. O(1).
    [[nodiscard]] std::size_t run_count() const noexcept { return entries_; }
    [[nodiscard]] std::size_t depth() const noexcept { return static_cast<std::size_t>(height_) + 1; }
    [[nodiscard]] std::size_t page_bytes() const noexcept {
        const std::size_t bytes = leaves_ * sizeof(Leaf) + branches_ * sizeof(Branch);
        return bytes;
    }
    [[nodiscard]] TreeCounters counters() const noexcept { return counters_; }

    // Checks every structural and arithmetic invariant (study §8.2).
    [[nodiscard]] InvariantReport check_invariants() const {
        InvariantReport report{};
        struct Pending final {
            const void* node;
            std::uint32_t level;
        };
        std::vector<Pending> pending;
        pending.push_back(Pending{root_, height_});
        std::size_t leaves_seen = 0;
        std::size_t branches_seen = 0;
        std::size_t entries_seen = 0;
        RowCount root_rows = 0;
        Units root_total = 0;
        if (height_ == 0) {
            leaf_sums(*static_cast<const Leaf*>(root_), root_rows, root_total);
        } else {
            const Branch& root = *static_cast<const Branch*>(root_);
            if (root.length < 2) {
                return fail(report, "root branch has fewer than two children");
            }
            root_rows = root.count_prefix[root.length - 1];
            root_total = static_cast<Units>(root.height_prefix[root.length - 1]);
        }
        if (root_rows != rows_ || root_total != total_) {
            return fail(report, "root sums differ from the tree's totals");
        }
        if (total_ < 0 || total_ > total_limit || rows_ > row_limit) {
            return fail(report, "totals past the limits");
        }
        while (!pending.empty()) {
            const Pending at = pending.back();
            pending.pop_back();
            const bool is_root = at.node == root_;
            if (at.level == 0) {
                const Leaf& leaf = *static_cast<const Leaf*>(at.node);
                leaves_seen += 1;
                entries_seen += leaf.length;
                const std::size_t capacity = leaf.dense != 0 ? dense_capacity : packed_capacity;
                if (leaf.dense != 0 && !Config::dense_leaves) {
                    return fail(report, "dense leaf in a configuration without them");
                }
                if (leaf.length > capacity) {
                    return fail(report, "leaf past its capacity");
                }
                if (!is_root && leaf.length < leaf_minimum) {
                    return fail(report, "non-root leaf below its minimum");
                }
                if (is_root && leaf.length == 0 && rows_ != 0) {
                    return fail(report, "empty root leaf with rows");
                }
                for (std::uint32_t index = 0; index < leaf.length; index += 1) {
                    const Run run = entry_run(leaf, index);
                    if (run.count < 1 || run.total < 0 || static_cast<std::uint8_t>(run.state) > 2) {
                        return fail(report, "invalid entry");
                    }
                    if (leaf.dense == 0 && (run.count > packed_count_limit || run.total > packed_total_limit)) {
                        return fail(report, "packed entry past its fields");
                    }
                    if (run.state == RowState::estimated && !RunArithmetic::uniform(run)) {
                        return fail(report, "estimated run not uniform");
                    }
                }
                continue;
            }
            const Branch& branch = *static_cast<const Branch*>(at.node);
            branches_seen += 1;
            if (branch.level != at.level) {
                return fail(report, "branch level differs from its depth");
            }
            if (branch.length < 1 || branch.length > branch_slots) {
                return fail(report, "branch length outside [1, capacity]");
            }
            if (!is_root && branch.length < branch_minimum) {
                return fail(report, "non-root branch below its minimum");
            }
            for (std::size_t slot = branch.length; slot < branch_slots; slot += 1) {
                if (branch.count_prefix[slot] != ~static_cast<std::uint64_t>(0) ||
                    branch.height_prefix[slot] != ~static_cast<std::uint64_t>(0) ||
                    branch.children[slot] != nullptr) {
                    return fail(report, "branch padding is not all ones");
                }
            }
            std::uint64_t rows_before = 0;
            std::uint64_t height_before = 0;
            for (std::uint32_t slot = 0; slot < branch.length; slot += 1) {
                const void* child = branch.children[slot];
                if (child == nullptr) {
                    return fail(report, "missing child");
                }
                RowCount child_rows = 0;
                Units child_total = 0;
                if (at.level == 1) {
                    leaf_sums(*static_cast<const Leaf*>(child), child_rows, child_total);
                } else {
                    const Branch& below = *static_cast<const Branch*>(child);
                    if (below.length < 1) {
                        return fail(report, "empty child branch");
                    }
                    child_rows = below.count_prefix[below.length - 1];
                    child_total = static_cast<Units>(below.height_prefix[below.length - 1]);
                }
                if (child_rows < 1) {
                    return fail(report, "child without rows");
                }
                const std::uint64_t rows_through = rows_before + child_rows;
                const std::uint64_t height_through = height_before + static_cast<std::uint64_t>(child_total);
                if (branch.count_prefix[slot] != rows_through || branch.height_prefix[slot] != height_through) {
                    return fail(report, "branch prefix differs from its children's sums");
                }
                rows_before = rows_through;
                height_before = height_through;
                pending.push_back(Pending{child, at.level - 1});
            }
        }
        if (leaves_seen != leaves_ || branches_seen != branches_ || entries_seen != entries_) {
            return fail(report, "node or entry tallies differ");
        }
        return report;
    }

private:
    struct ChildRef final {
        void* node;
        RowCount rows;
        Units total;
    };
    struct PathLevel final {
        Branch* node{};
        std::uint32_t slot{};
    };
    struct Path final {
        PathLevel levels[maximum_levels]{};
        Leaf* leaf{};
        RowCount leaf_row_start{};
        RowCount leaf_rows{};
    };

    [[nodiscard]] static Units usable_estimate(Units estimate) noexcept {
        if (estimate < 1 || estimate > height_limit) {
            return 64;
        }
        return estimate;
    }

    // rows * estimate within the total limit, into sum.
    [[nodiscard]] static bool estimated_total(RowCount rows, Units estimate, Units& sum) noexcept {
        if (rows > static_cast<RowCount>(total_limit / estimate)) {
            return false;
        }
        sum = static_cast<Units>(rows) * estimate;
        return true;
    }

    [[nodiscard]] static InvariantReport fail(InvariantReport& report, const char* failure) noexcept {
        report.ok = false;
        report.failure = failure;
        return report;
    }

    void note_descent() const noexcept {
#if defined(ORDER_STATISTIC_COUNTING)
        counters_.descents += 1;
#endif
    }
    void note_visit() const noexcept {
#if defined(ORDER_STATISTIC_COUNTING)
        counters_.node_visits += 1;
#endif
    }

    // ---- pages ----

    [[nodiscard]] Leaf* new_leaf() {
        Leaf* leaf = new Leaf();
        leaves_ += 1;
        return leaf;
    }
    [[nodiscard]] Branch* new_branch() {
        Branch* branch = new Branch();
        branches_ += 1;
        return branch;
    }
    void free_leaf(Leaf* leaf) noexcept {
        delete leaf;
        leaves_ -= 1;
    }
    void free_branch(Branch* branch) noexcept {
        delete branch;
        branches_ -= 1;
    }

    // Frees every page without recursion.
    void release_all() noexcept {
        if (root_ == nullptr) {
            return;
        }
        struct Pending final {
            void* node;
            std::uint32_t level;
        };
        std::vector<Pending> pending;
        pending.push_back(Pending{root_, height_});
        while (!pending.empty()) {
            const Pending at = pending.back();
            pending.pop_back();
            if (at.level == 0) {
                free_leaf(static_cast<Leaf*>(at.node));
                continue;
            }
            Branch* branch = static_cast<Branch*>(at.node);
            for (std::uint32_t slot = 0; slot < (*branch).length; slot += 1) {
                pending.push_back(Pending{(*branch).children[slot], at.level - 1});
            }
            free_branch(branch);
        }
        root_ = nullptr;
        height_ = 0;
        rows_ = 0;
        total_ = 0;
        entries_ = 0;
    }

    // ---- leaves ----

    [[nodiscard]] static Units dense_height(const Leaf& leaf, std::size_t row) noexcept {
        const std::uint64_t word = leaf.words[row / 2];
        const std::uint64_t half = row % 2 == 0 ? (word & 0xffffffffU) : (word >> 32U);
        return static_cast<Units>(half);
    }
    static void set_dense_height(Leaf& leaf, std::size_t row, Units height) noexcept {
        const std::uint64_t value = static_cast<std::uint64_t>(height);
        std::uint64_t& word = leaf.words[row / 2];
        if (row % 2 == 0) {
            word = (word & 0xffffffff00000000U) | value;
        } else {
            word = (word & 0xffffffffU) | (value << 32U);
        }
    }

    [[nodiscard]] static Run entry_run(const Leaf& leaf, std::size_t entry) noexcept {
        if (leaf.dense != 0) {
            const Run run{1, dense_height(leaf, entry), RowState::measured};
            return run;
        }
        const Run run = PackedEntry::decode(leaf.words[entry]);
        return run;
    }

    static void leaf_sums(const Leaf& leaf, RowCount& rows, Units& total) noexcept {
        rows = 0;
        total = 0;
        for (std::uint32_t index = 0; index < leaf.length; index += 1) {
            const Run run = entry_run(leaf, index);
            rows = rows + run.count;
            total = total + run.total;
        }
    }

    [[nodiscard]] static Units leaf_prefix(const Leaf& leaf, RowCount rows) noexcept {
        Units sum = 0;
        if (leaf.dense != 0) {
            for (std::size_t index = 0; index < rows; index += 1) {
                sum = sum + dense_height(leaf, index);
            }
            return sum;
        }
        RowCount remaining = rows;
        for (std::uint32_t index = 0; index < leaf.length; index += 1) {
            const Run run = PackedEntry::decode(leaf.words[index]);
            if (remaining < run.count) {
                sum = sum + RunArithmetic::prefix(run, remaining);
                return sum;
            }
            remaining = remaining - run.count;
            sum = sum + run.total;
        }
        return sum;
    }

    // The row within the leaf holding `offset` (0 <= offset < the leaf's
    // total): the least row whose end is past it. The last row is the
    // defined fallback for an offset the leaf does not hold.
    [[nodiscard]] static RowCount leaf_row_at(const Leaf& leaf, Units offset) noexcept {
        Units remaining = offset;
        RowCount row = 0;
        for (std::uint32_t index = 0; index < leaf.length; index += 1) {
            const Run run = entry_run(leaf, index);
            if (remaining < run.total) {
                const RowCount within = RunArithmetic::inverse(run, remaining);
                return row + within;
            }
            remaining = remaining - run.total;
            row = row + run.count;
        }
        return row > 0 ? row - 1 : 0;
    }

    [[nodiscard]] static Units leaf_total_from_cursor(const Cursor& cursor) noexcept {
        RowCount rows = 0;
        Units total = 0;
        leaf_sums(*cursor.leaf, rows, total);
        return total;
    }

    void place_in_leaf_by_row(Cursor& cursor, RowCount local) const noexcept {
        const Leaf& leaf = *cursor.leaf;
        RowCount remaining = local;
        Units top = cursor.leaf_offset_start;
        for (std::uint32_t index = 0; index < leaf.length; index += 1) {
            const Run run = entry_run(leaf, index);
            if (remaining < run.count) {
                cursor.entry = index;
                cursor.within = remaining;
                cursor.run = run;
                cursor.row = cursor.leaf_row_start + local;
                cursor.top = top + RunArithmetic::prefix(run, remaining);
                return;
            }
            remaining = remaining - run.count;
            top = top + run.total;
        }
        // Not reached for local < the leaf's rows.
        cursor.at_end = true;
        cursor.row = rows_;
        cursor.top = total_;
    }

    void place_in_leaf_by_offset(Cursor& cursor, Units local) const noexcept {
        const Leaf& leaf = *cursor.leaf;
        Units remaining = local;
        RowCount row = 0;
        Units top = cursor.leaf_offset_start;
        for (std::uint32_t index = 0; index < leaf.length; index += 1) {
            const Run run = entry_run(leaf, index);
            const bool last = index + 1 == leaf.length;
            if (remaining < run.total || last) {
                RowCount within = run.count - 1;
                if (remaining < run.total) {
                    within = RunArithmetic::inverse(run, remaining);
                }
                cursor.entry = index;
                cursor.within = within;
                cursor.run = run;
                cursor.row = cursor.leaf_row_start + row + within;
                cursor.top = top + RunArithmetic::prefix(run, within);
                return;
            }
            remaining = remaining - run.total;
            row = row + run.count;
            top = top + run.total;
        }
    }

    // Descends from `node` at cursor level `level` (whose starts are given)
    // to the row holding `local` (relative to the node's start).
    void descend_cursor_by_offset(Cursor& cursor, std::uint32_t level, const void* node, RowCount row_start,
                                  Units offset_start, Units local) const noexcept {
        Units remaining = local;
        std::uint32_t at = level;
        const void* walk = node;
        while (at < cursor.depth) {
            const Branch& branch = *static_cast<const Branch*>(walk);
            note_visit();
            const std::size_t child =
                Count::at_most(branch.height_prefix, branch.length, static_cast<std::uint64_t>(remaining));
            cursor.levels[at] = CursorLevel{&branch, static_cast<std::uint32_t>(child), row_start, offset_start};
            if (child > 0) {
                row_start = row_start + branch.count_prefix[child - 1];
                offset_start = offset_start + static_cast<Units>(branch.height_prefix[child - 1]);
                remaining = remaining - static_cast<Units>(branch.height_prefix[child - 1]);
            }
            walk = branch.children[child];
            at += 1;
        }
        note_visit();
        cursor.leaf = static_cast<const Leaf*>(walk);
        cursor.leaf_row_start = row_start;
        cursor.leaf_offset_start = offset_start;
        place_in_leaf_by_offset(cursor, remaining);
    }

    // ---- edit paths ----

    // Fills path_ down to the leaf holding `row` (row < count()), or for an
    // insertion the leaf before which `row` (<= count()) falls: a row at a
    // boundary between children goes to the end of the child before it.
    void descend_for_edit(RowCount row, bool insertion) noexcept {
        note_descent();
        void* node = root_;
        RowCount remaining = row;
        RowCount row_start = 0;
        RowCount node_rows = rows_;
        std::uint32_t level = 0;
        while (level < height_) {
            Branch& branch = *static_cast<Branch*>(node);
            note_visit();
            std::size_t child = 0;
            if (insertion) {
                child = Count::below(branch.count_prefix, branch.length, remaining);
                if (child >= branch.length) {
                    child = branch.length - 1;
                }
            } else {
                child = Count::at_most(branch.count_prefix, branch.length, remaining);
            }
            path_.levels[level] = PathLevel{&branch, static_cast<std::uint32_t>(child)};
            const RowCount before = child > 0 ? branch.count_prefix[child - 1] : 0;
            node_rows = branch.count_prefix[child] - before;
            remaining = remaining - before;
            row_start = row_start + before;
            node = branch.children[child];
            level += 1;
        }
        note_visit();
        path_.leaf = static_cast<Leaf*>(node);
        path_.leaf_row_start = row_start;
        path_.leaf_rows = node_rows;
    }

    static void decode_leaf(const Leaf& leaf, std::vector<Run>& out) {
        out.clear();
        for (std::uint32_t index = 0; index < leaf.length; index += 1) {
            out.push_back(entry_run(leaf, index));
        }
    }

    // The entry holding row `local` and the row within it (local < rows held).
    static void locate_row(const std::vector<Run>& runs, RowCount local, std::size_t& entry, RowCount& within) noexcept {
        RowCount remaining = local;
        for (std::size_t index = 0; index < runs.size(); index += 1) {
            if (remaining < runs[index].count) {
                entry = index;
                within = remaining;
                return;
            }
            remaining = remaining - runs[index].count;
        }
        entry = runs.size() - 1;
        within = runs.back().count - 1;
    }

    // Copies runs_ into edit_ without rows [local, local + rows) (splitting
    // the runs at the cut's ends, Lemma S); returns the index in edit_ where
    // the cut was.
    std::size_t cut_rows(RowCount local, RowCount rows) {
        edit_.clear();
        const RowCount cut_end = local + rows;
        RowCount start = 0;
        std::size_t gap = 0;
        bool gap_found = false;
        for (std::size_t index = 0; index < runs_.size(); index += 1) {
            Run run = runs_[index];
            const RowCount end = start + run.count;
            if (end <= local) {
                edit_.push_back(run);
                start = end;
                continue;
            }
            if (start >= cut_end) {
                if (!gap_found) {
                    gap = edit_.size();
                    gap_found = true;
                }
                edit_.push_back(run);
                start = end;
                continue;
            }
            // The run meets the cut.
            if (local > start) {
                edit_.push_back(RunArithmetic::take_front(run, local - start));
            }
            if (!gap_found) {
                gap = edit_.size();
                gap_found = true;
            }
            const RowCount from = local > start ? local : start;
            const RowCount dropped_end = cut_end < end ? cut_end : end;
            const RowCount dropped = dropped_end - from;
            if (dropped < run.count) {
                static_cast<void>(RunArithmetic::take_front(run, dropped));
                edit_.push_back(run);
            }
            start = end;
        }
        if (!gap_found) {
            gap = edit_.size();
        }
        return gap;
    }

    // Inserts `run` before `row` (row <= count()). Caller checked limits.
    void insert_run(RowCount row, const Run& run) {
        descend_for_edit(row, true);
        decode_leaf(*path_.leaf, runs_);
        const RowCount local = row - path_.leaf_row_start;
        // Split the run holding `local`, if it is inside one.
        edit_.clear();
        RowCount start = 0;
        std::size_t edit_index = 0;
        bool placed = false;
        for (std::size_t index = 0; index < runs_.size(); index += 1) {
            Run current = runs_[index];
            const RowCount end = start + current.count;
            if (!placed && local <= start) {
                edit_index = edit_.size();
                edit_.push_back(run);
                placed = true;
            }
            if (!placed && local < end) {
                edit_.push_back(RunArithmetic::take_front(current, local - start));
                edit_index = edit_.size();
                edit_.push_back(run);
                placed = true;
            }
            edit_.push_back(current);
            start = end;
        }
        if (!placed) {
            edit_index = edit_.size();
            edit_.push_back(run);
        }
        runs_.swap(edit_);
        normalize_runs();
        commit_leaf_edit(edit_index);
    }

    // Drops empty runs and joins adjacent uniform estimated runs of equal
    // row height.
    void normalize_runs() noexcept {
        std::size_t write = 0;
        for (std::size_t read = 0; read < runs_.size(); read += 1) {
            const Run run = runs_[read];
            if (run.count == 0) {
                continue;
            }
            if (write > 0) {
                Run& before = runs_[write - 1];
                const bool joinable = before.state == RowState::estimated && run.state == RowState::estimated &&
                                      RunArithmetic::uniform(before) && RunArithmetic::uniform(run) &&
                                      before.total / static_cast<Units>(before.count) ==
                                          run.total / static_cast<Units>(run.count);
                if (joinable) {
                    before.count = before.count + run.count;
                    before.total = before.total + run.total;
                    continue;
                }
            }
            runs_[write] = run;
            write += 1;
        }
        runs_.resize(write);
    }

    // Splits runs_ into pieces_ that fit packed entries (Lemma S).
    void expand_runs() {
        pieces_.clear();
        for (const Run& source : runs_) {
            Run run = source;
            while (run.count > packed_count_limit || run.total > packed_total_limit) {
                RowCount rows = RunArithmetic::rows_within(run, packed_total_limit);
                if (rows > packed_count_limit) {
                    rows = packed_count_limit;
                }
                if (rows >= run.count) {
                    break;
                }
                if (rows == 0) {
                    // Not reached: every row's height is at most height_limit.
                    rows = 1;
                }
                pieces_.push_back(RunArithmetic::take_front(run, rows));
            }
            pieces_.push_back(run);
        }
    }

    [[nodiscard]] static bool dense_eligible(const Run& run) noexcept {
        const bool eligible = run.state == RowState::measured && run.count == 1 && run.total <= dense_height_limit;
        return eligible;
    }

    // Writes pieces_[first, first + length) into `leaf`.
    void write_leaf(Leaf& leaf, std::size_t first, std::size_t length, ChildRef& ref) noexcept {
        bool dense = Config::dense_leaves;
        for (std::size_t index = first; index < first + length && dense; index += 1) {
            dense = dense_eligible(pieces_[index]);
        }
        std::memset(leaf.words, 0, sizeof(leaf.words));
        leaf.length = static_cast<std::uint32_t>(length);
        leaf.dense = dense ? 1U : 0U;
        RowCount rows = 0;
        Units total = 0;
        for (std::size_t index = 0; index < length; index += 1) {
            const Run& run = pieces_[first + index];
            if (dense) {
                set_dense_height(leaf, index, run.total);
            } else {
                leaf.words[index] = PackedEntry::encode(run);
            }
            rows = rows + run.count;
            total = total + run.total;
        }
        ref = ChildRef{&leaf, rows, total};
    }

    // Encodes pieces_ into leaves, reusing `first_page` and `second_page`
    // (either may be null), into made_. Unused reusable pages are freed.
    void encode_pieces(std::size_t edit_index, Leaf* first_page, Leaf* second_page) {
        made_.clear();
        const std::size_t pieces = pieces_.size();
        if (pieces == 0) {
            if (first_page != nullptr) {
                free_leaf(first_page);
            }
            if (second_page != nullptr) {
                free_leaf(second_page);
            }
            return;
        }
        bool all_dense = Config::dense_leaves;
        for (std::size_t index = 0; index < pieces && all_dense; index += 1) {
            all_dense = dense_eligible(pieces_[index]);
        }
        const std::size_t capacity = all_dense ? dense_capacity : packed_capacity;
        const std::size_t chunks = (pieces + capacity - 1) / capacity;
        // Chunk sizes: even, except that an edit near the end of a leaf that
        // splits in two leaves the arriving side small (bbtree D32), so
        // appends fill leaves to about three quarters.
        const bool end_split = chunks == 2 && edit_index + capacity / 4 >= pieces;
        const std::size_t end_right = pieces - capacity > capacity / 4 ? pieces - capacity : capacity / 4;
        const std::size_t base = pieces / chunks;
        const std::size_t extra = pieces % chunks;
        std::size_t first = 0;
        for (std::size_t chunk = 0; chunk < chunks; chunk += 1) {
            std::size_t size = base + (chunk < extra ? 1 : 0);
            if (end_split) {
                size = chunk == 0 ? pieces - end_right : end_right;
            }
            Leaf* page = nullptr;
            if (chunk == 0 && first_page != nullptr) {
                page = first_page;
                first_page = nullptr;
            } else if (second_page != nullptr) {
                page = second_page;
                second_page = nullptr;
            } else if (first_page != nullptr) {
                page = first_page;
                first_page = nullptr;
            } else {
                page = new_leaf();
            }
            ChildRef ref{};
            write_leaf(*page, first, size, ref);
            made_.push_back(ref);
            first = first + size;
        }
        if (first_page != nullptr) {
            free_leaf(first_page);
        }
        if (second_page != nullptr) {
            free_leaf(second_page);
        }
    }

    // After runs_ holds the leaf's new runs: re-encodes, joins an underfull
    // leaf with a sibling, and carries the change up the path.
    void commit_leaf_edit(std::size_t edit_index) {
        Leaf* leaf = path_.leaf;
        entries_ = entries_ - (*leaf).length;
        expand_runs();
        std::size_t index = edit_index;
        Leaf* sibling = nullptr;
        std::size_t first_slot = 0;
        std::size_t replaced = 1;
        if (height_ > 0) {
            const PathLevel& parent_level = path_.levels[height_ - 1];
            const Branch& parent = *parent_level.node;
            first_slot = parent_level.slot;
            if (!pieces_.empty() && pieces_.size() < leaf_minimum) {
                if (parent_level.slot + 1 < parent.length) {
                    sibling = static_cast<Leaf*>(parent.children[parent_level.slot + 1]);
                    for (std::uint32_t entry = 0; entry < (*sibling).length; entry += 1) {
                        pieces_.push_back(entry_run(*sibling, entry));
                    }
                } else {
                    sibling = static_cast<Leaf*>(parent.children[parent_level.slot - 1]);
                    edit_.clear();
                    for (std::uint32_t entry = 0; entry < (*sibling).length; entry += 1) {
                        edit_.push_back(entry_run(*sibling, entry));
                    }
                    index = index + edit_.size();
                    for (const Run& run : pieces_) {
                        edit_.push_back(run);
                    }
                    pieces_.swap(edit_);
                    first_slot = parent_level.slot - 1;
                }
                entries_ = entries_ - (*sibling).length;
                replaced = 2;
            }
        }
        entries_ = entries_ + pieces_.size();
        if (height_ == 0) {
            if (pieces_.empty()) {
                (*leaf).length = 0;
                (*leaf).dense = 0;
                rows_ = 0;
                total_ = 0;
                return;
            }
            encode_pieces(index, leaf, nullptr);
            raise_root(0);
            refresh_totals();
            return;
        }
        encode_pieces(index, leaf, sibling);
        propagate(height_ - 1, first_slot, replaced);
        refresh_totals();
    }

    [[nodiscard]] static ChildRef child_ref(const Branch& branch, std::size_t slot) noexcept {
        const RowCount before_rows = slot > 0 ? branch.count_prefix[slot - 1] : 0;
        const std::uint64_t before_height = slot > 0 ? branch.height_prefix[slot - 1] : 0;
        const ChildRef ref{branch.children[slot], branch.count_prefix[slot] - before_rows,
                           static_cast<Units>(branch.height_prefix[slot] - before_height)};
        return ref;
    }

    static void write_branch(Branch& branch, const ChildRef* kids, std::size_t length, std::uint32_t level) noexcept {
        branch.length = static_cast<std::uint32_t>(length);
        branch.level = level;
        std::uint64_t rows = 0;
        std::uint64_t height = 0;
        for (std::size_t slot = 0; slot < length; slot += 1) {
            rows = rows + kids[slot].rows;
            height = height + static_cast<std::uint64_t>(kids[slot].total);
            branch.count_prefix[slot] = rows;
            branch.height_prefix[slot] = height;
            branch.children[slot] = kids[slot].node;
        }
        for (std::size_t slot = length; slot < branch_slots; slot += 1) {
            branch.count_prefix[slot] = ~static_cast<std::uint64_t>(0);
            branch.height_prefix[slot] = ~static_cast<std::uint64_t>(0);
            branch.children[slot] = nullptr;
        }
    }

    [[nodiscard]] static ChildRef branch_ref(Branch& branch) noexcept {
        const ChildRef ref{&branch, branch.count_prefix[branch.length - 1],
                           static_cast<Units>(branch.height_prefix[branch.length - 1])};
        return ref;
    }

    // Writes kids_ into branches of `level`, reusing `first_page` and
    // `second_page` (second may be null), into carry_.
    void split_branches(std::uint32_t level, Branch* first_page, Branch* second_page) {
        carry_.clear();
        const std::size_t length = kids_.size();
        const std::size_t parts = (length + branch_slots - 1) / branch_slots;
        const std::size_t base = length / parts;
        const std::size_t extra = length % parts;
        std::size_t first = 0;
        for (std::size_t part = 0; part < parts; part += 1) {
            Branch* page = nullptr;
            if (part == 0) {
                page = first_page;
            } else if (second_page != nullptr) {
                page = second_page;
                second_page = nullptr;
            } else {
                page = new_branch();
            }
            const std::size_t size = base + (part < extra ? 1 : 0);
            write_branch(*page, kids_.data() + first, size, level);
            carry_.push_back(branch_ref(*page));
            first = first + size;
        }
        if (second_page != nullptr) {
            free_branch(second_page);
        }
    }

    // Replaces slots [at, at + removed) of the branch at path level `level`
    // by made_, rebalancing upward.
    void propagate(std::uint32_t level, std::size_t at, std::size_t removed) {
        std::uint32_t current = level;
        std::size_t first = at;
        std::size_t gone = removed;
        while (true) {
            Branch& node = *path_.levels[current].node;
            kids_.clear();
            for (std::size_t slot = 0; slot < first; slot += 1) {
                kids_.push_back(child_ref(node, slot));
            }
            for (const ChildRef& ref : made_) {
                kids_.push_back(ref);
            }
            for (std::size_t slot = first + gone; slot < node.length; slot += 1) {
                kids_.push_back(child_ref(node, slot));
            }
            const bool is_root = current == 0;
            if (!is_root && kids_.size() < branch_minimum) {
                const PathLevel& parent_level = path_.levels[current - 1];
                const Branch& parent = *parent_level.node;
                Branch* sibling = nullptr;
                std::size_t parent_first = parent_level.slot;
                if (parent_level.slot + 1 < parent.length) {
                    sibling = static_cast<Branch*>(parent.children[parent_level.slot + 1]);
                    for (std::uint32_t slot = 0; slot < (*sibling).length; slot += 1) {
                        kids_.push_back(child_ref(*sibling, slot));
                    }
                } else {
                    sibling = static_cast<Branch*>(parent.children[parent_level.slot - 1]);
                    carry_.clear();
                    for (std::uint32_t slot = 0; slot < (*sibling).length; slot += 1) {
                        carry_.push_back(child_ref(*sibling, slot));
                    }
                    for (const ChildRef& ref : kids_) {
                        carry_.push_back(ref);
                    }
                    kids_.swap(carry_);
                    parent_first = parent_level.slot - 1;
                }
                split_branches(node.level, &node, sibling);
                made_.swap(carry_);
                first = parent_first;
                gone = 2;
                current -= 1;
                continue;
            }
            if (kids_.size() > branch_slots) {
                const std::uint32_t node_level = node.level;
                split_branches(node_level, &node, nullptr);
                made_.swap(carry_);
                if (is_root) {
                    raise_root(node_level);
                    return;
                }
                first = path_.levels[current - 1].slot;
                gone = 1;
                current -= 1;
                continue;
            }
            if (kids_.empty()) {
                // Only the root can lose every child: the tree is empty.
                free_branch(&node);
                root_ = new_leaf();
                height_ = 0;
                return;
            }
            const RowCount old_rows = node.count_prefix[node.length - 1];
            const Units old_total = static_cast<Units>(node.height_prefix[node.length - 1]);
            write_branch(node, kids_.data(), kids_.size(), node.level);
            if (is_root) {
                collapse_root();
                return;
            }
            const RowCount new_rows = node.count_prefix[node.length - 1];
            const Units new_total = static_cast<Units>(node.height_prefix[node.length - 1]);
            add_upward(current, new_rows, old_rows, new_total - old_total);
            return;
        }
    }

    // Adds the change of a node's sums to every ancestor's prefixes from
    // the node's slot on. Exact: integer addition (study §4, C1).
    void add_upward(std::uint32_t level, RowCount new_rows, RowCount old_rows, Units height_delta) noexcept {
        std::uint32_t above = level;
        while (above > 0) {
            above -= 1;
            Branch& branch = *path_.levels[above].node;
            for (std::size_t slot = path_.levels[above].slot; slot < branch.length; slot += 1) {
                branch.count_prefix[slot] = branch.count_prefix[slot] - old_rows + new_rows;
                if (height_delta >= 0) {
                    branch.height_prefix[slot] = branch.height_prefix[slot] + static_cast<std::uint64_t>(height_delta);
                } else {
                    branch.height_prefix[slot] = branch.height_prefix[slot] - static_cast<std::uint64_t>(-height_delta);
                }
            }
        }
    }

    // Builds levels over made_ (children of height `child_level`) until one
    // node remains, which becomes the root.
    void raise_root(std::uint32_t child_level) {
        std::uint32_t level = child_level;
        while (made_.size() > 1) {
            kids_.clear();
            for (const ChildRef& ref : made_) {
                kids_.push_back(ref);
            }
            Branch* page = new_branch();
            split_branches(level + 1, page, nullptr);
            made_.swap(carry_);
            level += 1;
        }
        root_ = made_.front().node;
        height_ = level;
    }

    void collapse_root() noexcept {
        while (height_ > 0) {
            Branch* root = static_cast<Branch*>(root_);
            if ((*root).length != 1) {
                return;
            }
            root_ = (*root).children[0];
            free_branch(root);
            height_ -= 1;
        }
    }

    void refresh_totals() noexcept {
        if (height_ == 0) {
            leaf_sums(*static_cast<const Leaf*>(root_), rows_, total_);
            return;
        }
        const Branch& root = *static_cast<const Branch*>(root_);
        rows_ = root.count_prefix[root.length - 1];
        total_ = static_cast<Units>(root.height_prefix[root.length - 1]);
    }

    void* root_;
    std::uint32_t height_;
    RowCount rows_;
    Units total_;
    std::size_t leaves_;
    std::size_t branches_;
    std::size_t entries_;
    std::uint64_t revision_;
    Units estimate_;
    // Scratch, reserved once: decoded runs, packed pieces, an edit copy,
    // replacement children and a branch's spliced children.
    std::vector<Run> runs_;
    std::vector<Run> pieces_;
    std::vector<Run> edit_;
    std::vector<ChildRef> made_;
    std::vector<ChildRef> carry_;
    std::vector<ChildRef> kids_;
    Path path_;
    mutable TreeCounters counters_;
};

} // namespace stx_btree_research
