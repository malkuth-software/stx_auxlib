#pragma once

// A sequence tree with summed heights: an order-statistic B-tree over a
// list of rows, each with a height in 1/64 logical units (26.6 fixed point)
// and a state. It answers the prefix sum of heights before a row, the row
// at an offset, and row extents, with cursors that walk and seek from where
// they are. RowHeightIndex (row_height_index.hpp) is its public form; the
// design, invariants and proof sketches are in
// btree/docs/ORDER_STATISTIC_RESEARCH.md (§3, §8), and the measurements
// that chose this configuration in its §9.
//
// Runs. Rows are kept as runs: a run of `count` rows with total height
// `total` gives its row i the height q + [i < s], where q = total / count
// and s = total % count. Splitting a run at any row keeps every row's
// height (Lemma S, study §8.2). Estimated runs are uniform (s = 0); a
// measured row is a run of one; an approximate (coarsened) run is any
// (count, total).
//
// Pages. Leaves hold 8-byte packed runs (count 24 bits, total 38 bits,
// state 2 bits) or, when the configuration admits them, dense runs of
// measured single rows (4 bytes a row). Branches keep, for each child, the
// prefix of row counts and the prefix of heights, exact unsigned integers
// below 2^62; slots past the length hold all ones. A descent picks a child
// by counting the prefixes at most (or below) the target over every slot,
// branch-free when the configuration asks.
//
// Rebalancing follows weak B-trees (Huddleston and Mehlhorn 1982): a node
// holds between a minimum and a capacity of at least twice the minimum; an
// underfull leaf merges with a sibling, or borrows from it when a merge
// would need three leaves; an underfull branch merges with a sibling; the
// root branch keeps at least two children and collapses when it has one.
//
// Failure. An edit that would break a limit (a row height outside
// [0, sequence_height_limit], a total past the configuration's total limit,
// a row count past its row limit) is refused with false and leaves the tree
// unchanged. Every page an edit can need is reserved from the allocator
// before the edit changes anything (study §8.1, "Failure"), so
// std::bad_alloc from the allocator also leaves the tree unchanged. The
// bounds behind each reservation are stated where they are computed. Pages
// an edit frees return to the tree's spare lists, which are trimmed after
// each edit.
//
// Execution. One thread. A cursor is valid until the next edit of its
// tree; a stale cursor (or one from another tree) is detected by the
// revision and owner and answered by a fresh seek.

#include "stx_btree/page_allocator.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <span>

// Arithmetic modulo 2^64 by design: the counting search's sign test
// (PrefixCount). The integer-sanitizer build halts on any other wrap.
#if defined(__clang__)
#define STX_BTREE_MODULAR                                                                     \
    __attribute__((no_sanitize("unsigned-integer-overflow", "unsigned-shift-base",          \
                               "implicit-integer-truncation")))
#else
#define STX_BTREE_MODULAR
#endif

namespace stx_btree {

// Row counts and row positions inside the tree. RowHeightIndex separates
// positions (RowIndex) from counts at its surface.
using RowCount = std::uint64_t;

enum class RowState : std::uint8_t { estimated = 0, measured = 1, approximate = 2 };

// A height or an offset in 1/64 logical units.
struct HeightUnits final {
    std::int64_t value{};

    [[nodiscard]] bool operator==(const HeightUnits& other) const noexcept {
        const bool same = value == other.value;
        return same;
    }
    [[nodiscard]] bool operator!=(const HeightUnits& other) const noexcept {
        const bool differs = value != other.value;
        return differs;
    }
    [[nodiscard]] bool operator<(const HeightUnits& other) const noexcept {
        const bool below = value < other.value;
        return below;
    }
    [[nodiscard]] bool operator<=(const HeightUnits& other) const noexcept {
        const bool at_most = value <= other.value;
        return at_most;
    }
    [[nodiscard]] bool operator>(const HeightUnits& other) const noexcept {
        const bool above = value > other.value;
        return above;
    }
    [[nodiscard]] bool operator>=(const HeightUnits& other) const noexcept {
        const bool at_least = value >= other.value;
        return at_least;
    }
};

namespace detail {

using Units = std::int64_t;

inline constexpr Units sequence_total_limit = (static_cast<Units>(1) << 62) - 1;
inline constexpr RowCount sequence_row_limit = (static_cast<RowCount>(1) << 62) - 1;
inline constexpr RowCount packed_count_limit = (static_cast<RowCount>(1) << 24) - 1;
inline constexpr Units packed_total_limit = (static_cast<Units>(1) << 38) - 1;
inline constexpr Units dense_height_limit = static_cast<Units>(0xffffffffU);
// The tallest single row: one packed entry holds it.
inline constexpr Units sequence_height_limit = packed_total_limit;
// Every packed piece of a run but its last holds packed_count_limit rows or
// at least this many units (proof at SequenceTree::segment_bound).
inline constexpr Units packed_piece_least_total = packed_total_limit / 2;

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
    // run.count <= packed_count_limit and 0 <= run.total <= packed_total_limit.
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
        run.state = static_cast<RowState>(static_cast<std::uint8_t>(word >> 62U));
        return run;
    }
};

// The declared choices of a tree: branch slots, leaf words (packed entries;
// a dense leaf holds twice as many rows), the counting or linear child
// search, whether dense leaves are admitted, and the limits.
template <std::size_t BranchSlots, std::size_t LeafWords, bool CountingSearch, bool DenseLeaves,
          Units TotalLimit = sequence_total_limit, RowCount RowLimit = sequence_row_limit>
struct SequenceConfig final {
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
    // Counts every slot, padding included. For values below 2^63,
    // slot <= target exactly when target - slot does not wrap (its top bit
    // is clear), and slot < target exactly when slot - target wraps. Padding
    // (all ones) is counted by both tests for any target below 2^62, so the
    // caller subtracts the padding slots. The subtractions are modular by
    // design.
    [[nodiscard]] STX_BTREE_MODULAR static std::size_t count_all(const std::uint64_t* slots, std::uint64_t target,
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
struct alignas(64) LeafPage final {
    // Entries (packed) or rows (dense).
    std::uint32_t length{};
    std::uint32_t dense{};
    // Packed entry i is words[i]; dense row i is the (i % 2)th half of words[i / 2].
    std::uint64_t words[Config::leaf_words]{};
};

template <typename Config>
struct alignas(64) BranchPage final {
    std::uint32_t length{};
    // The node's height: 1 when its children are leaves.
    std::uint32_t level{};
    std::uint64_t count_prefix[Config::branch_slots]{};
    std::uint64_t height_prefix[Config::branch_slots]{};
    void* children[Config::branch_slots]{};
};

// A spare page on a tree's free list; constructed in the page's storage
// when the page is given back, replaced by a page object when it is taken.
struct SparePage final {
    SparePage* next{};
};

// Where a row is: its top, its height and its state.
struct SequenceExtent final {
    Units top{};
    Units height{};
    RowState state{RowState::estimated};
};

struct InvariantReport final {
    bool ok{true};
    const char* failure{""};
};

// A scratch list of runs with a fixed capacity. The tree sizes every
// buffer from the bounds stated at SequenceTree::run_capacity; append past
// the capacity would mean a broken bound and stops the program rather than
// write past the array.
template <std::size_t Capacity>
class RunBuffer final {
public:
    void clear() noexcept { length_ = 0; }
    [[nodiscard]] std::size_t size() const noexcept { return length_; }
    [[nodiscard]] bool empty() const noexcept { return length_ == 0; }
    void append(const Run& run) noexcept {
        if (length_ == Capacity) {
            std::abort();
        }
        runs_[length_] = run;
        length_ += 1;
    }
    // Keeps the first `length` runs (length <= size()).
    void truncate(std::size_t length) noexcept { length_ = length; }
    [[nodiscard]] Run& operator[](std::size_t index) noexcept { return runs_[index]; }
    [[nodiscard]] const Run& operator[](std::size_t index) const noexcept { return runs_[index]; }

private:
    Run runs_[Capacity]{};
    std::size_t length_{};
};

// A child of a branch with the sums its parent's prefixes hold for it.
struct ChildRef final {
    void* node{};
    RowCount rows{};
    Units total{};
};

template <std::size_t Capacity>
class ChildBuffer final {
public:
    void clear() noexcept { length_ = 0; }
    [[nodiscard]] std::size_t size() const noexcept { return length_; }
    [[nodiscard]] bool empty() const noexcept { return length_ == 0; }
    void append(const ChildRef& child) noexcept {
        if (length_ == Capacity) {
            std::abort();
        }
        children_[length_] = child;
        length_ += 1;
    }
    [[nodiscard]] const ChildRef& operator[](std::size_t index) const noexcept { return children_[index]; }
    [[nodiscard]] const ChildRef* data() const noexcept { return children_; }

private:
    ChildRef children_[Capacity]{};
    std::size_t length_{};
};

template <typename Config, typename Allocator = GlobalPageAllocator>
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
    // least one row, and rows stay below 2^62, so the height stays below 64.
    static constexpr std::size_t maximum_levels = 64;
    static constexpr Units total_limit = Config::total_limit;
    static constexpr RowCount row_limit = Config::row_limit;

    static_assert(branch_slots >= 4 && branch_slots >= 2 * branch_minimum, "weak B-tree branches: b >= 2a");
    static_assert(packed_capacity >= 3 && packed_capacity >= 2 * leaf_minimum, "weak B-tree leaves: b >= 2a");
    static_assert(total_limit <= sequence_total_limit && row_limit <= sequence_row_limit,
                  "prefix slots stay below 2^62 for the counting search");
    static_assert(sizeof(Leaf) >= sizeof(SparePage) && sizeof(Branch) >= sizeof(SparePage),
                  "a spare page holds its link");

    using Count = PrefixCount<Config::branch_slots, Config::counting_search>;

    struct CursorLevel final {
        const Branch* node{};
        std::uint32_t slot{};
        RowCount row_start{};
        Units offset_start{};
    };

    // A position in the sequence, with the path to it. Valid while its
    // tree's revision equals the revision it was placed at.
    class Cursor final {
    public:
        [[nodiscard]] RowCount row() const noexcept { return row_; }
        [[nodiscard]] Units top() const noexcept { return top_; }
        // 0 at the end.
        [[nodiscard]] Units height() const noexcept {
            if (at_end_) {
                return 0;
            }
            const Units height = RunArithmetic::height_at(run_, within_);
            return height;
        }
        [[nodiscard]] RowState state() const noexcept { return run_.state; }
        [[nodiscard]] bool at_end() const noexcept { return at_end_; }

    private:
        friend class SequenceTree;

        CursorLevel levels_[maximum_levels]{};
        std::uint32_t depth_{};
        const SequenceTree* owner_{};
        const Leaf* leaf_{};
        RowCount leaf_row_start_{};
        Units leaf_offset_start_{};
        // The entry (packed) or row (dense) within the leaf, and the row
        // within the entry.
        std::uint32_t entry_{};
        RowCount within_{};
        Run run_{};
        RowCount row_{};
        Units top_{};
        std::uint64_t revision_{};
        bool at_end_{true};
    };

    // The estimate falls back to 64 (one logical unit) outside
    // [1, sequence_height_limit]. Throws std::bad_alloc if the root page
    // cannot be allocated.
    explicit SequenceTree(Units estimate, const Allocator& allocator = Allocator())
        : allocator_(allocator), root_(nullptr), height_(0), rows_(0), total_(0), leaves_(0), branches_(0),
          entries_(0), measured_rows_(0), revision_(1), estimate_(usable_estimate(estimate)), spare_leaves_(nullptr),
          spare_branches_(nullptr), spare_leaf_count_(0), spare_branch_count_(0), run_storage_(), runs_(nullptr),
          edit_(nullptr), pieces_(nullptr), child_storage_(), made_(nullptr), carry_(nullptr), kids_(nullptr),
          path_() {
        runs_ = &run_storage_[0];
        edit_ = &run_storage_[1];
        pieces_ = &run_storage_[2];
        made_ = &child_storage_[0];
        carry_ = &child_storage_[1];
        kids_ = &child_storage_[2];
        reserve_pages(1, 0);
        root_ = take_leaf();
    }
    ~SequenceTree() {
        release_live_pages();
        release_spares(0, 0);
    }
    SequenceTree(const SequenceTree&) = delete;
    SequenceTree& operator=(const SequenceTree&) = delete;
    SequenceTree(SequenceTree&&) = delete;
    SequenceTree& operator=(SequenceTree&&) = delete;

    // ---- queries ------------------------------------------------------------

    [[nodiscard]] RowCount count() const noexcept { return rows_; }
    [[nodiscard]] Units total() const noexcept { return total_; }
    [[nodiscard]] Units estimate() const noexcept { return estimate_; }
    [[nodiscard]] std::uint64_t revision() const noexcept { return revision_; }
    // Rows in the measured state. O(1): each leaf commit adjusts it by its
    // leaves' measured rows before and after (I6).
    [[nodiscard]] RowCount measured_count() const noexcept { return measured_rows_; }

    // The heights of rows [0, row); row >= count() gives total().
    [[nodiscard]] Units offset_of(RowCount row) const noexcept {
        if (row >= rows_) {
            return total_;
        }
        const void* node = root_;
        Units offset = 0;
        RowCount remaining = row;
        std::uint32_t level = height_;
        while (level > 0) {
            const Branch& branch = *static_cast<const Branch*>(node);
            const std::size_t child = Count::at_most(branch.count_prefix, branch.length, remaining);
            if (child > 0) {
                remaining = remaining - branch.count_prefix[child - 1];
                offset = offset + static_cast<Units>(branch.height_prefix[child - 1]);
            }
            node = branch.children[child];
            level -= 1;
        }
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
        const void* node = root_;
        RowCount row = 0;
        Units remaining = offset;
        std::uint32_t level = height_;
        while (level > 0) {
            const Branch& branch = *static_cast<const Branch*>(node);
            const std::size_t child =
                Count::at_most(branch.height_prefix, branch.length, static_cast<std::uint64_t>(remaining));
            if (child > 0) {
                row = row + branch.count_prefix[child - 1];
                remaining = remaining - static_cast<Units>(branch.height_prefix[child - 1]);
            }
            node = branch.children[child];
            level -= 1;
        }
        const RowCount within = leaf_row_at(*static_cast<const Leaf*>(node), remaining);
        const RowCount found = row + within;
        return found;
    }

    // Past the end: the bottom, height 0.
    [[nodiscard]] SequenceExtent extent_of(RowCount row) const noexcept {
        if (row >= rows_) {
            const SequenceExtent bottom{total_, 0, RowState::estimated};
            return bottom;
        }
        const void* node = root_;
        Units top = 0;
        RowCount remaining = row;
        std::uint32_t level = height_;
        while (level > 0) {
            const Branch& branch = *static_cast<const Branch*>(node);
            const std::size_t child = Count::at_most(branch.count_prefix, branch.length, remaining);
            if (child > 0) {
                remaining = remaining - branch.count_prefix[child - 1];
                top = top + static_cast<Units>(branch.height_prefix[child - 1]);
            }
            node = branch.children[child];
            level -= 1;
        }
        const Leaf& leaf = *static_cast<const Leaf*>(node);
        SequenceExtent extent{};
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
        // Not reached for row < count(); the bottom is the fallback.
        extent.top = total_;
        return extent;
    }

    // ---- cursors --------------------------------------------------------------

    // Places the cursor at `row` (past the end: at_end).
    void seek_row(Cursor& cursor, RowCount row) const noexcept {
        cursor.revision_ = revision_;
        cursor.owner_ = this;
        if (row >= rows_) {
            cursor.at_end_ = true;
            cursor.row_ = rows_;
            cursor.top_ = total_;
            return;
        }
        cursor.depth_ = height_;
        cursor.at_end_ = false;
        const void* node = root_;
        RowCount row_start = 0;
        Units offset_start = 0;
        RowCount remaining = row;
        std::uint32_t level = 0;
        while (level < height_) {
            const Branch& branch = *static_cast<const Branch*>(node);
            const std::size_t child = Count::at_most(branch.count_prefix, branch.length, remaining);
            cursor.levels_[level] = CursorLevel{&branch, static_cast<std::uint32_t>(child), row_start, offset_start};
            if (child > 0) {
                remaining = remaining - branch.count_prefix[child - 1];
                row_start = row_start + branch.count_prefix[child - 1];
                offset_start = offset_start + static_cast<Units>(branch.height_prefix[child - 1]);
            }
            node = branch.children[child];
            level += 1;
        }
        cursor.leaf_ = static_cast<const Leaf*>(node);
        cursor.leaf_row_start_ = row_start;
        cursor.leaf_offset_start_ = offset_start;
        place_in_leaf_by_row(cursor, remaining);
    }

    // Places the cursor at row_at(offset); at_end only when empty.
    void seek_offset(Cursor& cursor, Units offset) const noexcept {
        if (rows_ == 0 || offset < 0) {
            seek_row(cursor, 0);
            return;
        }
        if (offset >= total_) {
            seek_row(cursor, rows_ - 1);
            return;
        }
        cursor.revision_ = revision_;
        cursor.owner_ = this;
        cursor.depth_ = height_;
        cursor.at_end_ = false;
        descend_cursor_by_offset(cursor, 0, root_, 0, 0, offset);
    }

    // Moves a valid cursor to row_at(offset), climbing only as far as the
    // target needs: O(log_B d) nodes for a target d rows away. A stale,
    // foreign or finished cursor seeks from the root.
    void seek_offset_from(Cursor& cursor, Units offset) const noexcept {
        const bool valid = cursor.owner_ == this && cursor.revision_ == revision_ && !cursor.at_end_;
        if (!valid || rows_ == 0 || offset < 0 || offset >= total_) {
            seek_offset(cursor, offset);
            return;
        }
        std::uint32_t level = cursor.depth_;
        // Climb while the target lies outside the node at `level`.
        Units node_start = cursor.leaf_offset_start_;
        Units node_end = cursor.leaf_offset_start_ + leaf_total(*cursor.leaf_);
        while (level > 0 && (offset < node_start || offset >= node_end)) {
            level -= 1;
            const CursorLevel& above = cursor.levels_[level];
            node_start = above.offset_start;
            node_end = above.offset_start + static_cast<Units>((*above.node).height_prefix[(*above.node).length - 1]);
        }
        if (level == cursor.depth_) {
            // The target is in the current leaf.
            place_in_leaf_by_offset(cursor, offset - cursor.leaf_offset_start_);
            return;
        }
        const CursorLevel& from = cursor.levels_[level];
        descend_cursor_by_offset(cursor, level, from.node, from.row_start, from.offset_start,
                                 offset - from.offset_start);
    }

    // The next row; at_end after the last. A stale or foreign cursor is
    // re-placed at its row first.
    void advance(Cursor& cursor) const noexcept {
        if (cursor.owner_ != this || cursor.revision_ != revision_) {
            if (cursor.at_end_) {
                return;
            }
            seek_row(cursor, cursor.row_);
        }
        if (cursor.at_end_) {
            return;
        }
        cursor.top_ = cursor.top_ + RunArithmetic::height_at(cursor.run_, cursor.within_);
        cursor.row_ = cursor.row_ + 1;
        if (cursor.row_ >= rows_) {
            cursor.at_end_ = true;
            return;
        }
        cursor.within_ = cursor.within_ + 1;
        if (cursor.within_ < cursor.run_.count) {
            return;
        }
        cursor.within_ = 0;
        cursor.entry_ = cursor.entry_ + 1;
        if (cursor.entry_ < (*cursor.leaf_).length) {
            cursor.run_ = entry_run(*cursor.leaf_, cursor.entry_);
            return;
        }
        // The next leaf: climb to the first level with a right neighbour,
        // then descend leftmost.
        std::uint32_t level = cursor.depth_;
        while (level > 0) {
            CursorLevel& above = cursor.levels_[level - 1];
            if (above.slot + 1 < (*above.node).length) {
                above.slot = above.slot + 1;
                break;
            }
            level -= 1;
        }
        // level >= 1 here: a row remains, so some level has a right
        // neighbour. The end is the fallback if the walk finds none.
        if (level == 0) {
            cursor.at_end_ = true;
            return;
        }
        std::uint32_t walk = level - 1;
        while (true) {
            const CursorLevel& at = cursor.levels_[walk];
            const Branch& branch = *at.node;
            const RowCount child_row = at.row_start + (at.slot > 0 ? branch.count_prefix[at.slot - 1] : 0);
            const Units child_offset =
                at.offset_start + (at.slot > 0 ? static_cast<Units>(branch.height_prefix[at.slot - 1]) : 0);
            const void* child = branch.children[at.slot];
            if (walk + 1 == cursor.depth_) {
                cursor.leaf_ = static_cast<const Leaf*>(child);
                cursor.leaf_row_start_ = child_row;
                cursor.leaf_offset_start_ = child_offset;
                break;
            }
            cursor.levels_[walk + 1] = CursorLevel{static_cast<const Branch*>(child), 0, child_row, child_offset};
            walk += 1;
        }
        cursor.entry_ = 0;
        cursor.run_ = entry_run(*cursor.leaf_, 0);
    }

    // ---- edits --------------------------------------------------------------

    // Replaces every row with `rows` estimated rows at `estimate`, which
    // becomes the estimate (falling back as the constructor's does).
    // Refused (unchanged) past the limits.
    bool assign(RowCount rows, Units estimate) {
        const Units usable = usable_estimate(estimate);
        Units sum = 0;
        if (!estimated_total(rows, usable, sum) || rows > row_limit) {
            return false;
        }
        const Run whole{rows, sum, RowState::estimated};
        const std::size_t segments = rows > 0 ? segment_bound(whole) : 0;
        // The new tree starts from one leaf, then takes the insertion's
        // pages; the old pages go back to the allocator first.
        const std::size_t leaves_needed = checked_sum(1, checked_product(2, segments));
        const std::size_t branches_needed =
            checked_product(segments, height_bound(leaves_needed) + 1);
        reserve_pages(leaves_needed, branches_needed);
        release_live_pages();
        root_ = take_leaf();
        estimate_ = usable;
        revision_ += 1;
        if (rows > 0) {
            insert_segments(0, whole);
        }
        trim_spares();
        return true;
    }

    // Records a measured height. Refused (false, unchanged) past the end,
    // outside [0, sequence_height_limit] or past the total limit; false
    // also when the row is already measured at that height.
    bool set_height(RowCount row, HeightUnits height_units) {
        const Units height = height_units.value;
        if (row >= rows_ || height < 0 || height > sequence_height_limit) {
            return false;
        }
        descend_for_edit(row, false);
        decode_leaf(*path_.leaf, *runs_);
        std::size_t entry = 0;
        RowCount within = 0;
        locate_row(*runs_, row - path_.leaf_row_start, entry, within);
        const Run found = (*runs_)[entry];
        const Units old_height = RunArithmetic::height_at(found, within);
        if (found.state == RowState::measured && old_height == height) {
            return false;
        }
        if (total_ - old_height > total_limit - height) {
            return false;
        }
        reserve_commits(1, 0);
        Run rest = found;
        RunBuffer<run_capacity>& edit = *edit_;
        edit.clear();
        for (std::size_t index = 0; index < entry; index += 1) {
            edit.append((*runs_)[index]);
        }
        if (within > 0) {
            edit.append(RunArithmetic::take_front(rest, within));
        }
        const std::size_t edit_index = edit.size();
        edit.append(Run{1, height, RowState::measured});
        if (rest.count > 1) {
            static_cast<void>(RunArithmetic::take_front(rest, 1));
            edit.append(rest);
        }
        for (std::size_t index = entry + 1; index < (*runs_).size(); index += 1) {
            edit.append((*runs_)[index]);
        }
        swap_runs_and_edit();
        normalize_runs();
        commit_leaf_edit(edit_index);
        revision_ += 1;
        trim_spares();
        return true;
    }

    // Records measured heights for rows [row, row + heights.size()), at
    // most packed_capacity rows a leaf commit. Refused as a whole
    // (unchanged) if any height or the range is invalid or the total would
    // pass the limit.
    bool set_heights(RowCount row, std::span<const HeightUnits> heights) {
        const RowCount wanted = static_cast<RowCount>(heights.size());
        if (wanted == 0) {
            return true;
        }
        if (row >= rows_ || wanted > rows_ - row) {
            return false;
        }
        Units replacement = 0;
        for (const HeightUnits& height : heights) {
            if (height.value < 0 || height.value > sequence_height_limit || replacement > total_limit - height.value) {
                return false;
            }
            replacement = replacement + height.value;
        }
        const Units replaced = offset_of(row + wanted) - offset_of(row);
        if (total_ - replaced > total_limit - replacement) {
            return false;
        }
        // Commits: each handles min(left, room in its leaf, packed_capacity)
        // rows. At most wanted / packed_capacity end at the cap; one that
        // ends at its leaf's end and is not the first covers a whole
        // non-root leaf of at least leaf_minimum rows (I4), so at most
        // 1 + wanted / leaf_minimum end there; one more ends at `wanted`.
        const std::size_t wanted_size = static_cast<std::size_t>(wanted);
        const std::size_t commits = wanted_size / packed_capacity + wanted_size / leaf_minimum + 2;
        reserve_commits(commits, 0);
        RowCount done = 0;
        while (done < wanted) {
            descend_for_edit(row + done, false);
            const RowCount local = row + done - path_.leaf_row_start;
            const RowCount room = path_.leaf_rows - local;
            RowCount here = wanted - done < room ? wanted - done : room;
            if (here > packed_capacity) {
                here = packed_capacity;
            }
            decode_leaf(*path_.leaf, *runs_);
            const std::size_t gap = cut_rows(local, here);
            // cut_rows left edit_ with the rows outside the cut; put the new
            // rows at the gap.
            RunBuffer<run_capacity>& runs = *runs_;
            const RunBuffer<run_capacity>& edit = *edit_;
            runs.clear();
            for (std::size_t index = 0; index < gap; index += 1) {
                runs.append(edit[index]);
            }
            for (RowCount index = 0; index < here; index += 1) {
                const Units height = heights[static_cast<std::size_t>(done + index)].value;
                runs.append(Run{1, height, RowState::measured});
            }
            for (std::size_t index = gap; index < edit.size(); index += 1) {
                runs.append(edit[index]);
            }
            normalize_runs();
            commit_leaf_edit(gap);
            done = done + here;
        }
        revision_ += 1;
        trim_spares();
        return true;
    }

    // Inserts `rows` estimated rows before `row` (clamped to count()).
    // Refused (unchanged) past the limits.
    bool insert(RowCount row, RowCount rows) {
        if (rows == 0) {
            return true;
        }
        Units sum = 0;
        if (!estimated_total(rows, estimate_, sum) || rows > row_limit - rows_ || total_ > total_limit - sum) {
            return false;
        }
        const Run run{rows, sum, RowState::estimated};
        reserve_commits(segment_bound(run), 0);
        const RowCount at = row < rows_ ? row : rows_;
        insert_segments(at, run);
        revision_ += 1;
        trim_spares();
        return true;
    }

    // Erases rows [row, row + rows), clamped to the end.
    void erase(RowCount row, RowCount rows) {
        if (rows == 0 || row >= rows_) {
            return;
        }
        reserve_erase();
        erase_rows(row, rows);
        revision_ += 1;
        trim_spares();
    }

    // Returns rows [row, row + rows) (clamped) to the estimate. Refused
    // (unchanged) if the total would pass the limit.
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
        const Run run{taken, sum, RowState::estimated};
        replace_rows(row, run);
        return true;
    }

    // Replaces rows [row, row + rows) (clamped to the end) by one
    // approximate run with their exact total. offset_of(x) is unchanged
    // bit for bit for every x <= row and every x >= the range's end
    // (study §8.2, "Coarsen").
    void coarsen(RowCount row, RowCount rows) {
        if (rows == 0 || row >= rows_) {
            return;
        }
        const RowCount taken = rows < rows_ - row ? rows : rows_ - row;
        const Units sum = offset_of(row + taken) - offset_of(row);
        const Run run{taken, sum, RowState::approximate};
        replace_rows(row, run);
    }

    // ---- storage and checks --------------------------------------------------

    [[nodiscard]] std::size_t leaf_count() const noexcept { return leaves_; }
    [[nodiscard]] std::size_t branch_count() const noexcept { return branches_; }
    [[nodiscard]] std::size_t node_count() const noexcept { return leaves_ + branches_; }
    // Packed entries plus dense rows: the runs held. O(1).
    [[nodiscard]] std::size_t run_count() const noexcept { return entries_; }
    [[nodiscard]] std::size_t depth() const noexcept { return static_cast<std::size_t>(height_) + 1; }
    // Bytes of live pages; spare pages are not counted.
    [[nodiscard]] std::size_t page_bytes() const noexcept {
        const std::size_t bytes = leaves_ * sizeof(Leaf) + branches_ * sizeof(Branch);
        return bytes;
    }

    // Checks every structural and arithmetic invariant, I1-I6 of study
    // §8.2. A walk with one frame a level; no allocation.
    [[nodiscard]] InvariantReport check_invariants() const noexcept {
        InvariantReport report{};
        RowCount root_rows = 0;
        Units root_total = 0;
        std::size_t leaves_seen = 0;
        std::size_t branches_seen = 0;
        std::size_t entries_seen = 0;
        RowCount measured_seen = 0;
        if (height_ == 0) {
            const Leaf& root = *static_cast<const Leaf*>(root_);
            const char* failure = check_leaf(root, true);
            if (failure != nullptr) {
                return fail(report, failure);
            }
            leaf_sums(root, root_rows, root_total);
            leaves_seen = 1;
            entries_seen = root.length;
            measured_seen = leaf_measured_rows(root);
        } else {
            const Branch& root = *static_cast<const Branch*>(root_);
            if (root.length < 2) {
                return fail(report, "root branch has fewer than two children");
            }
            root_rows = root.count_prefix[root.length - 1];
            root_total = static_cast<Units>(root.height_prefix[root.length - 1]);
            WalkFrame frames[maximum_levels]{};
            std::uint32_t top = 0;
            frames[0] = WalkFrame{&root, 0};
            const char* failure = check_branch(root, height_, true);
            if (failure != nullptr) {
                return fail(report, failure);
            }
            branches_seen = 1;
            while (true) {
                WalkFrame& frame = frames[top];
                const Branch& branch = *frame.node;
                if (frame.next == branch.length) {
                    if (top == 0) {
                        break;
                    }
                    top -= 1;
                    continue;
                }
                const void* child = branch.children[frame.next];
                frame.next += 1;
                if (branch.level == 1) {
                    const Leaf& leaf = *static_cast<const Leaf*>(child);
                    const char* leaf_failure = check_leaf(leaf, false);
                    if (leaf_failure != nullptr) {
                        return fail(report, leaf_failure);
                    }
                    leaves_seen += 1;
                    entries_seen += leaf.length;
                    measured_seen += leaf_measured_rows(leaf);
                    continue;
                }
                const Branch& below = *static_cast<const Branch*>(child);
                const char* branch_failure = check_branch(below, branch.level - 1, false);
                if (branch_failure != nullptr) {
                    return fail(report, branch_failure);
                }
                branches_seen += 1;
                top += 1;
                frames[top] = WalkFrame{&below, 0};
            }
        }
        if (root_rows != rows_ || root_total != total_) {
            return fail(report, "root sums differ from the tree's totals");
        }
        if (total_ < 0 || total_ > total_limit || rows_ > row_limit) {
            return fail(report, "totals past the limits");
        }
        if (leaves_seen != leaves_ || branches_seen != branches_ || entries_seen != entries_) {
            return fail(report, "node or entry tallies differ");
        }
        if (measured_seen != measured_rows_) {
            return fail(report, "measured row tally differs");
        }
        return report;
    }

private:
    // Scratch run capacity. A decoded leaf holds L <= max(dense_capacity,
    // packed_capacity) runs. An edit copy adds at most two (a split run and
    // an inserted run, or a set_height's three-way split); set_heights adds
    // at most packed_capacity measured rows after a cut that adds at most
    // one. Expansion into pieces: decoded runs and their Lemma S parts fit
    // one entry each, and so does every measured row; an inserted segment
    // expands into at most packed_capacity pieces, and joining it with
    // equal estimated neighbours adds at most one piece a neighbour (each
    // piece of a uniform run holds the most rows an entry can). A dense
    // leaf has no runs to split, so pieces <= 3 * packed_capacity; a packed
    // one gives at most 2 * packed_capacity + 1. A merge holds fewer than
    // leaf_minimum own pieces plus a sibling's at most dense_capacity.
    static constexpr std::size_t run_capacity = 2 * packed_capacity + dense_capacity + leaf_minimum + 8;
    // Children spliced into one branch: at most branch_slots - 1 kept plus
    // three new, or fewer than branch_minimum plus a sibling's branch_slots.
    static constexpr std::size_t child_capacity = 2 * branch_slots + 4;
    // Spare pages kept between edits: four commits' worth at height 7.
    static constexpr std::size_t retained_spare_leaves = 8;
    static constexpr std::size_t retained_spare_branches = 32;

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
    struct WalkFrame final {
        const Branch* node{};
        std::uint32_t next{};
    };
    struct ReleaseFrame final {
        Branch* node{};
        std::uint32_t next{};
    };

    [[nodiscard]] static Units usable_estimate(Units estimate) noexcept {
        if (estimate < 1 || estimate > sequence_height_limit) {
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

    // I3 and I4 for one leaf; null when it holds.
    [[nodiscard]] const char* check_leaf(const Leaf& leaf, bool is_root) const noexcept {
        const std::size_t capacity = leaf.dense != 0 ? dense_capacity : packed_capacity;
        if (leaf.dense != 0 && !Config::dense_leaves) {
            return "dense leaf in a configuration without them";
        }
        if (leaf.length > capacity) {
            return "leaf past its capacity";
        }
        if (!is_root && leaf.length < leaf_minimum) {
            return "non-root leaf below its minimum";
        }
        if (is_root && leaf.length == 0 && rows_ != 0) {
            return "empty root leaf with rows";
        }
        for (std::uint32_t index = 0; index < leaf.length; index += 1) {
            const Run run = entry_run(leaf, index);
            if (run.count < 1 || run.total < 0 || static_cast<std::uint8_t>(run.state) > 2) {
                return "invalid entry";
            }
            if (leaf.dense == 0 && (run.count > packed_count_limit || run.total > packed_total_limit)) {
                return "packed entry past its fields";
            }
            if (run.state == RowState::estimated && !RunArithmetic::uniform(run)) {
                return "estimated run not uniform";
            }
        }
        return nullptr;
    }

    // I1, I2 and I4 for one branch whose expected height is `level`.
    [[nodiscard]] static const char* check_branch(const Branch& branch, std::uint32_t level, bool is_root) noexcept {
        if (branch.level != level) {
            return "branch level differs from its depth";
        }
        if (branch.length < 1 || branch.length > branch_slots) {
            return "branch length outside [1, capacity]";
        }
        if (!is_root && branch.length < branch_minimum) {
            return "non-root branch below its minimum";
        }
        for (std::size_t slot = branch.length; slot < branch_slots; slot += 1) {
            if (branch.count_prefix[slot] != ~static_cast<std::uint64_t>(0) ||
                branch.height_prefix[slot] != ~static_cast<std::uint64_t>(0) || branch.children[slot] != nullptr) {
                return "branch padding is not all ones";
            }
        }
        std::uint64_t rows_before = 0;
        std::uint64_t height_before = 0;
        for (std::uint32_t slot = 0; slot < branch.length; slot += 1) {
            const void* child = branch.children[slot];
            if (child == nullptr) {
                return "missing child";
            }
            RowCount child_rows = 0;
            Units child_total = 0;
            if (level == 1) {
                leaf_sums(*static_cast<const Leaf*>(child), child_rows, child_total);
            } else {
                const Branch& below = *static_cast<const Branch*>(child);
                if (below.length < 1 || below.length > branch_slots) {
                    return "child branch length outside [1, capacity]";
                }
                child_rows = below.count_prefix[below.length - 1];
                child_total = static_cast<Units>(below.height_prefix[below.length - 1]);
            }
            if (child_rows < 1 || child_total < 0) {
                return "child without rows";
            }
            const std::uint64_t rows_through = rows_before + child_rows;
            const std::uint64_t height_through = height_before + static_cast<std::uint64_t>(child_total);
            if (branch.count_prefix[slot] != rows_through || branch.height_prefix[slot] != height_through) {
                return "branch prefix differs from its children's sums";
            }
            rows_before = rows_through;
            height_before = height_through;
        }
        return nullptr;
    }

    // ---- pages and reservations ----

    // left + right, throwing std::bad_alloc where the sum would wrap: a
    // reservation that large cannot be met.
    [[nodiscard]] static std::size_t checked_sum(std::size_t left, std::size_t right) {
        if (left > std::numeric_limits<std::size_t>::max() - right) {
            throw std::bad_alloc();
        }
        const std::size_t sum = left + right;
        return sum;
    }
    [[nodiscard]] static std::size_t checked_product(std::size_t left, std::size_t right) {
        if (left != 0 && right > std::numeric_limits<std::size_t>::max() / left) {
            throw std::bad_alloc();
        }
        const std::size_t product = left * right;
        return product;
    }

    // The greatest height of a tree with `leaves` leaves: a tree of height
    // h >= 1 has at least 2 * branch_minimum^(h - 1) leaves (the root branch
    // has two children or more, every other branch branch_minimum, I4).
    [[nodiscard]] static std::size_t height_bound(std::size_t leaves) noexcept {
        std::size_t height = 0;
        std::size_t least = 2;
        while (least <= leaves && height < maximum_levels) {
            height += 1;
            if (least > std::numeric_limits<std::size_t>::max() / branch_minimum) {
                break;
            }
            least = least * branch_minimum;
        }
        return height;
    }

    // Pages for `commits` leaf commits made one after another, plus
    // `extra_leaves`. One commit encodes at most 3 * packed_capacity pieces
    // (run_capacity's bound), so at most three leaves, one of them the
    // edited page: two new leaves. Its replacement children, at most three
    // for one slot, split each ancestor at most once into two (branch_slots
    // + 2 <= 2 * branch_slots), and a split root grows one level: one new
    // branch a level plus one. Merges and borrows reuse their two pages.
    // Each commit adds at most two leaves, so the height stays within
    // height_bound(leaves + 2 * commits + extra_leaves).
    void reserve_commits(std::size_t commits, std::size_t extra_leaves) {
        const std::size_t new_leaves = checked_sum(checked_product(2, commits), extra_leaves);
        const std::size_t height = height_bound(checked_sum(leaves_, new_leaves));
        const std::size_t new_branches = checked_product(checked_sum(commits, extra_leaves), height + 1);
        reserve_pages(new_leaves, new_branches);
    }

    // An erase's pages. Every commit of an erase but the last removes a
    // suffix of its leaf (the cut reaches the leaf's end), which splits no
    // run and so adds no entry: the leaf re-encodes into one page, or merges
    // or borrows within its own and a sibling's pages, and no branch gains a
    // child. Only the last commit can add one entry (a cut inside a run):
    // the bound of one commit.
    void reserve_erase() { reserve_commits(1, 0); }

    // Ensures at least `leaves` and `branches` spare pages. May throw
    // std::bad_alloc; the tree is unchanged then (spares are not content).
    void reserve_pages(std::size_t leaves, std::size_t branches) {
        while (spare_leaf_count_ < leaves) {
            void* memory = allocator_.allocate_page(PageKind::leaf, sizeof(Leaf), alignof(Leaf));
            give_leaf_memory(memory);
        }
        while (spare_branch_count_ < branches) {
            void* memory = allocator_.allocate_page(PageKind::internal, sizeof(Branch), alignof(Branch));
            give_branch_memory(memory);
        }
    }

    void give_leaf_memory(void* memory) noexcept {
        SparePage* spare = new (memory) SparePage{spare_leaves_};
        spare_leaves_ = spare;
        spare_leaf_count_ += 1;
    }
    void give_branch_memory(void* memory) noexcept {
        SparePage* spare = new (memory) SparePage{spare_branches_};
        spare_branches_ = spare;
        spare_branch_count_ += 1;
    }

    // Takes a reserved page. An empty spare list would mean a broken
    // reservation bound; it stops the program rather than allocate in the
    // middle of an edit.
    [[nodiscard]] Leaf* take_leaf() noexcept {
        if (spare_leaves_ == nullptr) {
            std::abort();
        }
        SparePage* spare = spare_leaves_;
        spare_leaves_ = (*spare).next;
        spare_leaf_count_ -= 1;
        void* memory = spare;
        Leaf* leaf = new (memory) Leaf();
        leaves_ += 1;
        return leaf;
    }
    [[nodiscard]] Branch* take_branch() noexcept {
        if (spare_branches_ == nullptr) {
            std::abort();
        }
        SparePage* spare = spare_branches_;
        spare_branches_ = (*spare).next;
        spare_branch_count_ -= 1;
        void* memory = spare;
        Branch* branch = new (memory) Branch();
        branches_ += 1;
        return branch;
    }
    void give_leaf(Leaf* leaf) noexcept {
        leaves_ -= 1;
        give_leaf_memory(leaf);
    }
    void give_branch(Branch* branch) noexcept {
        branches_ -= 1;
        give_branch_memory(branch);
    }

    // Returns spares past the given counts to the allocator.
    void release_spares(std::size_t keep_leaves, std::size_t keep_branches) noexcept {
        while (spare_leaf_count_ > keep_leaves) {
            SparePage* spare = spare_leaves_;
            spare_leaves_ = (*spare).next;
            spare_leaf_count_ -= 1;
            allocator_.deallocate_page(PageKind::leaf, spare, sizeof(Leaf), alignof(Leaf));
        }
        while (spare_branch_count_ > keep_branches) {
            SparePage* spare = spare_branches_;
            spare_branches_ = (*spare).next;
            spare_branch_count_ -= 1;
            allocator_.deallocate_page(PageKind::internal, spare, sizeof(Branch), alignof(Branch));
        }
    }
    void trim_spares() noexcept { release_spares(retained_spare_leaves, retained_spare_branches); }

    // Returns every live page to the allocator without recursion and
    // leaves no root. One frame a level.
    void release_live_pages() noexcept {
        if (root_ == nullptr) {
            return;
        }
        if (height_ == 0) {
            allocator_.deallocate_page(PageKind::leaf, root_, sizeof(Leaf), alignof(Leaf));
        } else {
            ReleaseFrame frames[maximum_levels]{};
            std::uint32_t top = 0;
            frames[0] = ReleaseFrame{static_cast<Branch*>(root_), 0};
            while (true) {
                ReleaseFrame& frame = frames[top];
                Branch* branch = frame.node;
                if (frame.next == (*branch).length) {
                    allocator_.deallocate_page(PageKind::internal, branch, sizeof(Branch), alignof(Branch));
                    if (top == 0) {
                        break;
                    }
                    top -= 1;
                    continue;
                }
                void* child = (*branch).children[frame.next];
                frame.next += 1;
                if ((*branch).level == 1) {
                    allocator_.deallocate_page(PageKind::leaf, child, sizeof(Leaf), alignof(Leaf));
                    continue;
                }
                top += 1;
                frames[top] = ReleaseFrame{static_cast<Branch*>(child), 0};
            }
        }
        root_ = nullptr;
        height_ = 0;
        rows_ = 0;
        total_ = 0;
        leaves_ = 0;
        branches_ = 0;
        entries_ = 0;
        measured_rows_ = 0;
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

    // The rows of a leaf in the measured state: every row of a dense leaf.
    [[nodiscard]] static RowCount leaf_measured_rows(const Leaf& leaf) noexcept {
        if (leaf.dense != 0) {
            return static_cast<RowCount>(leaf.length);
        }
        RowCount rows = 0;
        for (std::uint32_t index = 0; index < leaf.length; index += 1) {
            const Run run = PackedEntry::decode(leaf.words[index]);
            if (run.state == RowState::measured) {
                rows = rows + run.count;
            }
        }
        return rows;
    }

    [[nodiscard]] RowCount pieces_measured_rows() const noexcept {
        const RunBuffer<run_capacity>& pieces = *pieces_;
        RowCount rows = 0;
        for (std::size_t index = 0; index < pieces.size(); index += 1) {
            if (pieces[index].state == RowState::measured) {
                rows = rows + pieces[index].count;
            }
        }
        return rows;
    }

    [[nodiscard]] static Units leaf_total(const Leaf& leaf) noexcept {
        RowCount rows = 0;
        Units total = 0;
        leaf_sums(leaf, rows, total);
        return total;
    }

    [[nodiscard]] static Units leaf_prefix(const Leaf& leaf, RowCount rows) noexcept {
        Units sum = 0;
        if (leaf.dense != 0) {
            const std::size_t wanted = static_cast<std::size_t>(rows);
            for (std::size_t index = 0; index < wanted; index += 1) {
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

    void place_in_leaf_by_row(Cursor& cursor, RowCount local) const noexcept {
        const Leaf& leaf = *cursor.leaf_;
        RowCount remaining = local;
        Units top = cursor.leaf_offset_start_;
        for (std::uint32_t index = 0; index < leaf.length; index += 1) {
            const Run run = entry_run(leaf, index);
            if (remaining < run.count) {
                cursor.entry_ = index;
                cursor.within_ = remaining;
                cursor.run_ = run;
                cursor.row_ = cursor.leaf_row_start_ + local;
                cursor.top_ = top + RunArithmetic::prefix(run, remaining);
                return;
            }
            remaining = remaining - run.count;
            top = top + run.total;
        }
        // Not reached for local < the leaf's rows; the end is the fallback.
        cursor.at_end_ = true;
        cursor.row_ = rows_;
        cursor.top_ = total_;
    }

    void place_in_leaf_by_offset(Cursor& cursor, Units local) const noexcept {
        const Leaf& leaf = *cursor.leaf_;
        Units remaining = local;
        RowCount row = 0;
        Units top = cursor.leaf_offset_start_;
        for (std::uint32_t index = 0; index < leaf.length; index += 1) {
            const Run run = entry_run(leaf, index);
            const bool last = index + 1 == leaf.length;
            if (remaining < run.total || last) {
                RowCount within = run.count - 1;
                if (remaining < run.total) {
                    within = RunArithmetic::inverse(run, remaining);
                }
                cursor.entry_ = index;
                cursor.within_ = within;
                cursor.run_ = run;
                cursor.row_ = cursor.leaf_row_start_ + row + within;
                cursor.top_ = top + RunArithmetic::prefix(run, within);
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
        RowCount row_at_start = row_start;
        Units offset_at_start = offset_start;
        std::uint32_t at = level;
        const void* walk = node;
        while (at < cursor.depth_) {
            const Branch& branch = *static_cast<const Branch*>(walk);
            const std::size_t child =
                Count::at_most(branch.height_prefix, branch.length, static_cast<std::uint64_t>(remaining));
            cursor.levels_[at] =
                CursorLevel{&branch, static_cast<std::uint32_t>(child), row_at_start, offset_at_start};
            if (child > 0) {
                row_at_start = row_at_start + branch.count_prefix[child - 1];
                offset_at_start = offset_at_start + static_cast<Units>(branch.height_prefix[child - 1]);
                remaining = remaining - static_cast<Units>(branch.height_prefix[child - 1]);
            }
            walk = branch.children[child];
            at += 1;
        }
        cursor.leaf_ = static_cast<const Leaf*>(walk);
        cursor.leaf_row_start_ = row_at_start;
        cursor.leaf_offset_start_ = offset_at_start;
        place_in_leaf_by_offset(cursor, remaining);
    }

    // ---- edit paths ----

    // Fills path_ down to the leaf holding `row` (row < count()), or for an
    // insertion the leaf before which `row` (<= count()) falls: a row at a
    // boundary between children goes to the end of the child before it.
    void descend_for_edit(RowCount row, bool insertion) noexcept {
        void* node = root_;
        RowCount remaining = row;
        RowCount row_start = 0;
        RowCount node_rows = rows_;
        std::uint32_t level = 0;
        while (level < height_) {
            Branch& branch = *static_cast<Branch*>(node);
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
        path_.leaf = static_cast<Leaf*>(node);
        path_.leaf_row_start = row_start;
        path_.leaf_rows = node_rows;
    }

    static void decode_leaf(const Leaf& leaf, RunBuffer<run_capacity>& out) noexcept {
        out.clear();
        for (std::uint32_t index = 0; index < leaf.length; index += 1) {
            out.append(entry_run(leaf, index));
        }
    }

    // The entry holding row `local` and the row within it (local < rows held).
    static void locate_row(const RunBuffer<run_capacity>& runs, RowCount local, std::size_t& entry,
                           RowCount& within) noexcept {
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
        within = runs[entry].count - 1;
    }

    void swap_runs_and_edit() noexcept {
        RunBuffer<run_capacity>* held = runs_;
        runs_ = edit_;
        edit_ = held;
    }
    void swap_pieces_and_edit() noexcept {
        RunBuffer<run_capacity>* held = pieces_;
        pieces_ = edit_;
        edit_ = held;
    }
    void swap_made_and_carry() noexcept {
        ChildBuffer<child_capacity>* held = made_;
        made_ = carry_;
        carry_ = held;
    }
    void swap_kids_and_carry() noexcept {
        ChildBuffer<child_capacity>* held = kids_;
        kids_ = carry_;
        carry_ = held;
    }

    // Copies runs_ into edit_ without rows [local, local + rows) (splitting
    // the runs at the cut's ends, Lemma S); returns the index in edit_ where
    // the cut was. A cut that reaches the end of the runs splits at most
    // the run at its start, which keeps its front only: no entry is added.
    std::size_t cut_rows(RowCount local, RowCount rows) noexcept {
        const RunBuffer<run_capacity>& runs = *runs_;
        RunBuffer<run_capacity>& edit = *edit_;
        edit.clear();
        const RowCount cut_end = local + rows;
        RowCount start = 0;
        std::size_t gap = 0;
        bool gap_found = false;
        for (std::size_t index = 0; index < runs.size(); index += 1) {
            Run run = runs[index];
            const RowCount end = start + run.count;
            if (end <= local) {
                edit.append(run);
                start = end;
                continue;
            }
            if (start >= cut_end) {
                if (!gap_found) {
                    gap = edit.size();
                    gap_found = true;
                }
                edit.append(run);
                start = end;
                continue;
            }
            // The run meets the cut.
            if (local > start) {
                edit.append(RunArithmetic::take_front(run, local - start));
            }
            if (!gap_found) {
                gap = edit.size();
                gap_found = true;
            }
            const RowCount from = local > start ? local : start;
            const RowCount dropped_end = cut_end < end ? cut_end : end;
            const RowCount dropped = dropped_end - from;
            if (dropped < run.count) {
                static_cast<void>(RunArithmetic::take_front(run, dropped));
                edit.append(run);
            }
            start = end;
        }
        if (!gap_found) {
            gap = edit.size();
        }
        return gap;
    }

    // Erases rows [row, row + rows) (clamped), one leaf commit at a time.
    // Pages are reserved by the caller (reserve_erase).
    void erase_rows(RowCount row, RowCount rows) noexcept {
        RowCount left = rows < rows_ - row ? rows : rows_ - row;
        while (left > 0) {
            descend_for_edit(row, false);
            const RowCount local = row - path_.leaf_row_start;
            const RowCount room = path_.leaf_rows - local;
            const RowCount here = left < room ? left : room;
            decode_leaf(*path_.leaf, *runs_);
            const std::size_t gap = cut_rows(local, here);
            swap_runs_and_edit();
            normalize_runs();
            commit_leaf_edit(gap);
            left = left - here;
        }
    }

    // Replaces rows [row, row + run.count) by `run` (forget and coarsen):
    // an erase, then the run's insertion. The caller checked the limits.
    void replace_rows(RowCount row, const Run& run) {
        const std::size_t segments = segment_bound(run);
        // The erase's one growing commit, then the insertion's segments.
        reserve_commits(checked_sum(segments, 1), 0);
        erase_rows(row, run.count);
        insert_segments(row, run);
        revision_ += 1;
        trim_spares();
    }

    // The rows of the next packed piece of `run`: all of them when the run
    // fits one entry, else the most rows within both packed fields (at
    // least one, since one row is at most packed_total_limit units).
    [[nodiscard]] static RowCount piece_rows(const Run& run) noexcept {
        if (run.count <= packed_count_limit && run.total <= packed_total_limit) {
            return run.count;
        }
        RowCount rows = RunArithmetic::rows_within(run, packed_total_limit);
        if (rows > packed_count_limit) {
            rows = packed_count_limit;
        }
        if (rows == 0) {
            // Not reached: every row's height is at most the limit.
            rows = 1;
        }
        return rows;
    }

    // The rows of the front of `run` that expand into at most `pieces`
    // packed pieces.
    [[nodiscard]] static RowCount segment_rows(const Run& run, std::size_t pieces) noexcept {
        Run rest = run;
        RowCount rows = 0;
        for (std::size_t piece = 0; piece < pieces; piece += 1) {
            const RowCount taken = piece_rows(rest);
            rows = rows + taken;
            if (taken >= rest.count) {
                return rows;
            }
            static_cast<void>(RunArithmetic::take_front(rest, taken));
        }
        return rows;
    }

    // An upper bound on the segments of packed_capacity pieces that
    // insert_segments makes of `run`. Every piece but the last holds
    // packed_count_limit rows or at least packed_piece_least_total units:
    // a piece of w < count rows ends where one more row (of height at most
    // q + 1, q the run's base, which Lemma S keeps for every remainder)
    // would pass packed_total_limit, so it holds more than
    // packed_total_limit - (q + 1) units, and it holds at least one row of
    // at least q units; max(q, L - q) >= L / 2. Hence pieces <=
    // count / packed_count_limit + total / packed_piece_least_total + 1.
    [[nodiscard]] static std::size_t segment_bound(const Run& run) noexcept {
        const RowCount by_rows = run.count / packed_count_limit;
        const Units by_units = run.total / packed_piece_least_total;
        const std::size_t pieces = static_cast<std::size_t>(by_rows) + static_cast<std::size_t>(by_units) + 1;
        const std::size_t segments = (pieces + packed_capacity - 1) / packed_capacity;
        return segments;
    }

    // Inserts `run` before `row` in segments of at most packed_capacity
    // pieces, front first (each segment's rows keep their heights, Lemma
    // S). The caller checked the limits and reserved the pages.
    void insert_segments(RowCount row, const Run& run) noexcept {
        Run rest = run;
        RowCount at = row;
        while (rest.count > 0) {
            const RowCount rows = segment_rows(rest, packed_capacity);
            if (rows >= rest.count) {
                insert_run(at, rest);
                return;
            }
            const Run front = RunArithmetic::take_front(rest, rows);
            insert_run(at, front);
            at = at + rows;
        }
    }

    // Inserts `run` (at most packed_capacity pieces) before `row`
    // (row <= count()): one leaf commit.
    void insert_run(RowCount row, const Run& run) noexcept {
        descend_for_edit(row, true);
        decode_leaf(*path_.leaf, *runs_);
        const RowCount local = row - path_.leaf_row_start;
        const RunBuffer<run_capacity>& runs = *runs_;
        RunBuffer<run_capacity>& edit = *edit_;
        // Split the run holding `local`, if it is inside one.
        edit.clear();
        RowCount start = 0;
        std::size_t edit_index = 0;
        bool placed = false;
        for (std::size_t index = 0; index < runs.size(); index += 1) {
            Run current = runs[index];
            const RowCount end = start + current.count;
            if (!placed && local <= start) {
                edit_index = edit.size();
                edit.append(run);
                placed = true;
            }
            if (!placed && local < end) {
                edit.append(RunArithmetic::take_front(current, local - start));
                edit_index = edit.size();
                edit.append(run);
                placed = true;
            }
            edit.append(current);
            start = end;
        }
        if (!placed) {
            edit_index = edit.size();
            edit.append(run);
        }
        swap_runs_and_edit();
        normalize_runs();
        commit_leaf_edit(edit_index);
    }

    // Drops empty runs and joins adjacent uniform estimated runs of equal
    // row height.
    void normalize_runs() noexcept {
        RunBuffer<run_capacity>& runs = *runs_;
        std::size_t write = 0;
        for (std::size_t read = 0; read < runs.size(); read += 1) {
            const Run run = runs[read];
            if (run.count == 0) {
                continue;
            }
            if (write > 0) {
                Run& before = runs[write - 1];
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
            runs[write] = run;
            write += 1;
        }
        runs.truncate(write);
    }

    // Splits runs_ into pieces_ that fit packed entries (Lemma S).
    void expand_runs() noexcept {
        const RunBuffer<run_capacity>& runs = *runs_;
        RunBuffer<run_capacity>& pieces = *pieces_;
        pieces.clear();
        for (std::size_t index = 0; index < runs.size(); index += 1) {
            Run run = runs[index];
            while (true) {
                const RowCount rows = piece_rows(run);
                if (rows >= run.count) {
                    break;
                }
                pieces.append(RunArithmetic::take_front(run, rows));
            }
            pieces.append(run);
        }
    }

    [[nodiscard]] static bool dense_eligible(const Run& run) noexcept {
        const bool eligible = run.state == RowState::measured && run.count == 1 && run.total <= dense_height_limit;
        return eligible;
    }

    // The leaves pieces_ encodes into when re-packed together: all dense
    // when every piece is eligible (and dense leaves are admitted), else
    // packed.
    [[nodiscard]] std::size_t chunks_needed() const noexcept {
        const RunBuffer<run_capacity>& pieces = *pieces_;
        bool all_dense = Config::dense_leaves;
        for (std::size_t index = 0; index < pieces.size() && all_dense; index += 1) {
            all_dense = dense_eligible(pieces[index]);
        }
        const std::size_t capacity = all_dense ? dense_capacity : packed_capacity;
        const std::size_t chunks = (pieces.size() + capacity - 1) / capacity;
        return chunks;
    }

    // Writes pieces_[first, first + length) into `leaf`, dense when every
    // one of them is eligible.
    ChildRef write_leaf(Leaf& leaf, std::size_t first, std::size_t length) noexcept {
        const RunBuffer<run_capacity>& pieces = *pieces_;
        bool dense = Config::dense_leaves;
        for (std::size_t index = first; index < first + length && dense; index += 1) {
            dense = dense_eligible(pieces[index]);
        }
        std::memset(leaf.words, 0, sizeof(leaf.words));
        leaf.length = static_cast<std::uint32_t>(length);
        leaf.dense = dense ? 1U : 0U;
        RowCount rows = 0;
        Units total = 0;
        for (std::size_t index = 0; index < length; index += 1) {
            const Run& run = pieces[first + index];
            if (dense) {
                set_dense_height(leaf, index, run.total);
            } else {
                leaf.words[index] = PackedEntry::encode(run);
            }
            rows = rows + run.count;
            total = total + run.total;
        }
        const ChildRef ref{&leaf, rows, total};
        return ref;
    }

    // Encodes pieces_ into leaves, reusing `first_page` and `second_page`
    // (either may be null), into made_. Unused reusable pages go back to
    // the spares. `split` > 0 forces two chunks, the first of `split`
    // pieces (a borrow). Otherwise the chunk sizes are even, except that an
    // edit near the end of a leaf that splits in two leaves the arriving
    // side small (bbtree D32), so appends fill leaves to about three
    // quarters.
    void encode_pieces(std::size_t edit_index, Leaf* first_page, Leaf* second_page, std::size_t split) noexcept {
        ChildBuffer<child_capacity>& made = *made_;
        made.clear();
        const std::size_t pieces = (*pieces_).size();
        Leaf* first_free = first_page;
        Leaf* second_free = second_page;
        if (pieces > 0) {
            bool all_dense = Config::dense_leaves;
            for (std::size_t index = 0; index < pieces && all_dense; index += 1) {
                all_dense = dense_eligible((*pieces_)[index]);
            }
            const std::size_t capacity = all_dense ? dense_capacity : packed_capacity;
            std::size_t chunks = (pieces + capacity - 1) / capacity;
            if (split > 0) {
                chunks = 2;
            }
            const bool end_split = split == 0 && chunks == 2 && edit_index + capacity / 4 >= pieces;
            const std::size_t end_right =
                end_split && pieces - capacity > capacity / 4 ? pieces - capacity : capacity / 4;
            const std::size_t base = pieces / chunks;
            const std::size_t extra = pieces % chunks;
            std::size_t first = 0;
            for (std::size_t chunk = 0; chunk < chunks; chunk += 1) {
                std::size_t size = base + (chunk < extra ? 1 : 0);
                if (split > 0) {
                    size = chunk == 0 ? split : pieces - split;
                } else if (end_split) {
                    size = chunk == 0 ? pieces - end_right : end_right;
                }
                Leaf* page = nullptr;
                if (first_free != nullptr) {
                    page = first_free;
                    first_free = nullptr;
                } else if (second_free != nullptr) {
                    page = second_free;
                    second_free = nullptr;
                } else {
                    page = take_leaf();
                }
                made.append(write_leaf(*page, first, size));
                first = first + size;
            }
        }
        if (first_free != nullptr) {
            give_leaf(first_free);
        }
        if (second_free != nullptr) {
            give_leaf(second_free);
        }
    }

    // After runs_ holds the leaf's new runs: re-encodes, merges an underfull
    // leaf with a sibling (or borrows from it when the merge would need
    // three leaves), and carries the change up the path.
    void commit_leaf_edit(std::size_t edit_index) noexcept {
        Leaf* leaf = path_.leaf;
        entries_ = entries_ - (*leaf).length;
        measured_rows_ = measured_rows_ - leaf_measured_rows(*leaf);
        expand_runs();
        std::size_t index = edit_index;
        Leaf* sibling = nullptr;
        std::size_t first_slot = 0;
        std::size_t replaced = 1;
        std::size_t split = 0;
        if (height_ > 0) {
            const PathLevel& parent_level = path_.levels[height_ - 1];
            const Branch& parent = *parent_level.node;
            first_slot = parent_level.slot;
            const std::size_t own = (*pieces_).size();
            if (own > 0 && own < leaf_minimum) {
                const bool sibling_after = parent_level.slot + 1 < parent.length;
                if (sibling_after) {
                    sibling = static_cast<Leaf*>(parent.children[parent_level.slot + 1]);
                    for (std::uint32_t entry = 0; entry < (*sibling).length; entry += 1) {
                        (*pieces_).append(entry_run(*sibling, entry));
                    }
                } else {
                    sibling = static_cast<Leaf*>(parent.children[parent_level.slot - 1]);
                    RunBuffer<run_capacity>& edit = *edit_;
                    edit.clear();
                    for (std::uint32_t entry = 0; entry < (*sibling).length; entry += 1) {
                        edit.append(entry_run(*sibling, entry));
                    }
                    index = index + edit.size();
                    for (std::size_t piece = 0; piece < own; piece += 1) {
                        edit.append((*pieces_)[piece]);
                    }
                    swap_pieces_and_edit();
                    first_slot = parent_level.slot - 1;
                }
                entries_ = entries_ - (*sibling).length;
                measured_rows_ = measured_rows_ - leaf_measured_rows(*sibling);
                replaced = 2;
                // Same-kind or dense-into-packed merges fit two leaves (own
                // < leaf_minimum <= capacity / 4). Only packed own pieces
                // with a dense sibling of up to 2 * packed_capacity rows can
                // need three: then the own side takes exactly leaf_minimum
                // pieces and the sibling keeps the rest, at least
                // 2 * packed_capacity - 2 * leaf_minimum >= leaf_minimum, all
                // dense-eligible.
                if (chunks_needed() > 2) {
                    const std::size_t total = (*pieces_).size();
                    split = sibling_after ? leaf_minimum : total - leaf_minimum;
                }
            }
        }
        entries_ = entries_ + (*pieces_).size();
        measured_rows_ = measured_rows_ + pieces_measured_rows();
        if (height_ == 0) {
            if ((*pieces_).empty()) {
                (*leaf).length = 0;
                (*leaf).dense = 0;
                rows_ = 0;
                total_ = 0;
                return;
            }
            encode_pieces(index, leaf, nullptr, 0);
            raise_root(0);
            refresh_totals();
            return;
        }
        encode_pieces(index, leaf, sibling, split);
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
    // `second_page` (second may be null), into carry_. kids_ holds at most
    // 2 * branch_slots children here, so at most two parts.
    void split_branches(std::uint32_t level, Branch* first_page, Branch* second_page) noexcept {
        ChildBuffer<child_capacity>& carry = *carry_;
        const ChildBuffer<child_capacity>& kids = *kids_;
        carry.clear();
        const std::size_t length = kids.size();
        const std::size_t parts = (length + branch_slots - 1) / branch_slots;
        const std::size_t base = length / parts;
        const std::size_t extra = length % parts;
        Branch* second_free = second_page;
        std::size_t first = 0;
        for (std::size_t part = 0; part < parts; part += 1) {
            Branch* page = nullptr;
            if (part == 0) {
                page = first_page;
            } else if (second_free != nullptr) {
                page = second_free;
                second_free = nullptr;
            } else {
                page = take_branch();
            }
            const std::size_t size = base + (part < extra ? 1 : 0);
            write_branch(*page, kids.data() + first, size, level);
            carry.append(branch_ref(*page));
            first = first + size;
        }
        if (second_free != nullptr) {
            give_branch(second_free);
        }
    }

    // Replaces slots [at, at + removed) of the branch at path level `level`
    // by made_, rebalancing upward.
    void propagate(std::uint32_t level, std::size_t at, std::size_t removed) noexcept {
        std::uint32_t current = level;
        std::size_t first = at;
        std::size_t gone = removed;
        while (true) {
            Branch& node = *path_.levels[current].node;
            ChildBuffer<child_capacity>& kids = *kids_;
            kids.clear();
            for (std::size_t slot = 0; slot < first; slot += 1) {
                kids.append(child_ref(node, slot));
            }
            const ChildBuffer<child_capacity>& made = *made_;
            for (std::size_t index = 0; index < made.size(); index += 1) {
                kids.append(made[index]);
            }
            for (std::size_t slot = first + gone; slot < node.length; slot += 1) {
                kids.append(child_ref(node, slot));
            }
            const bool is_root = current == 0;
            if (!is_root && kids.size() < branch_minimum) {
                const PathLevel& parent_level = path_.levels[current - 1];
                const Branch& parent = *parent_level.node;
                Branch* sibling = nullptr;
                std::size_t parent_first = parent_level.slot;
                if (parent_level.slot + 1 < parent.length) {
                    sibling = static_cast<Branch*>(parent.children[parent_level.slot + 1]);
                    for (std::uint32_t slot = 0; slot < (*sibling).length; slot += 1) {
                        kids.append(child_ref(*sibling, slot));
                    }
                } else {
                    sibling = static_cast<Branch*>(parent.children[parent_level.slot - 1]);
                    ChildBuffer<child_capacity>& carry = *carry_;
                    carry.clear();
                    for (std::uint32_t slot = 0; slot < (*sibling).length; slot += 1) {
                        carry.append(child_ref(*sibling, slot));
                    }
                    for (std::size_t index = 0; index < kids.size(); index += 1) {
                        carry.append(kids[index]);
                    }
                    swap_kids_and_carry();
                    parent_first = parent_level.slot - 1;
                }
                split_branches(node.level, &node, sibling);
                swap_made_and_carry();
                first = parent_first;
                gone = 2;
                current -= 1;
                continue;
            }
            if (kids.size() > branch_slots) {
                const std::uint32_t node_level = node.level;
                split_branches(node_level, &node, nullptr);
                swap_made_and_carry();
                if (is_root) {
                    raise_root(node_level);
                    return;
                }
                first = path_.levels[current - 1].slot;
                gone = 1;
                current -= 1;
                continue;
            }
            if (kids.empty()) {
                // Only the root can lose every child: the tree is empty. The
                // leaf this commit emptied is on the spare list and becomes
                // the empty root.
                give_branch(&node);
                root_ = take_leaf();
                height_ = 0;
                return;
            }
            const RowCount old_rows = node.count_prefix[node.length - 1];
            const Units old_total = static_cast<Units>(node.height_prefix[node.length - 1]);
            write_branch(node, kids.data(), kids.size(), node.level);
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
    // node remains, which becomes the root. made_ holds at most three
    // children here, so this adds one level.
    void raise_root(std::uint32_t child_level) noexcept {
        std::uint32_t level = child_level;
        while ((*made_).size() > 1) {
            ChildBuffer<child_capacity>& kids = *kids_;
            const ChildBuffer<child_capacity>& made = *made_;
            kids.clear();
            for (std::size_t index = 0; index < made.size(); index += 1) {
                kids.append(made[index]);
            }
            Branch* page = take_branch();
            split_branches(level + 1, page, nullptr);
            swap_made_and_carry();
            level += 1;
        }
        root_ = (*made_)[0].node;
        height_ = level;
    }

    void collapse_root() noexcept {
        while (height_ > 0) {
            Branch* root = static_cast<Branch*>(root_);
            if ((*root).length != 1) {
                return;
            }
            root_ = (*root).children[0];
            give_branch(root);
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

    Allocator allocator_;
    void* root_;
    // The root's height: 0 when the root is a leaf.
    std::uint32_t height_;
    RowCount rows_;
    Units total_;
    // Live pages, entries and measured rows (I6).
    std::size_t leaves_;
    std::size_t branches_;
    std::size_t entries_;
    RowCount measured_rows_;
    std::uint64_t revision_;
    Units estimate_;
    // Reserved pages not in the tree, linked through their storage.
    SparePage* spare_leaves_;
    SparePage* spare_branches_;
    std::size_t spare_leaf_count_;
    std::size_t spare_branch_count_;
    // Scratch with fixed capacities: decoded runs, an edit copy and packed
    // pieces (the three roles rotate by pointer), and replacement children,
    // spliced children and a carry list.
    RunBuffer<run_capacity> run_storage_[3];
    RunBuffer<run_capacity>* runs_;
    RunBuffer<run_capacity>* edit_;
    RunBuffer<run_capacity>* pieces_;
    ChildBuffer<child_capacity> child_storage_[3];
    ChildBuffer<child_capacity>* made_;
    ChildBuffer<child_capacity>* carry_;
    ChildBuffer<child_capacity>* kids_;
    Path path_;
};

} // namespace detail
} // namespace stx_btree
