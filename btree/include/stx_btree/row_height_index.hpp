#pragma once

// RowHeightIndex: the heights of a list of rows, for virtualized views. It
// keeps every row's height in 1/64 logical units (26.6 fixed point) with a
// state (estimated, measured, approximate) and answers the offset of a row
// (the exact sum of the heights before it), the row at an offset and a
// row's extent, each in O(log n), with cursors that step in O(1) amortized
// and seek from where they are in O(log_B d) for a target d rows away.
//
// It is the sequence tree of sequence_tree.hpp in the configuration
// measured as P5/P6 in btree/docs/ORDER_STATISTIC_RESEARCH.md §9: branches
// of 21 slots with the counting child search, leaves of 127 words, dense
// leaves admitted. Memory per row: a measured row in a dense leaf takes 4
// bytes; a run of estimated or approximate rows takes one 8-byte entry,
// and coarsen() turns detail the view no longer needs into such runs while
// keeping every offset outside the coarsened range unchanged bit for bit.
//
// The caller (GUI.Forms) keeps the conversion between its logical units
// and HeightUnits, the view policies (anchor, overscan, when to forget and
// coarsen, the run budget) and its counters (study §8.1).
//
// Failure: expected alternatives are bool results, and a refused call
// leaves the index unchanged. Allocation failure throws std::bad_alloc
// from the allocator and also leaves the index unchanged: every page an
// edit needs is reserved first. Limits: one row at most height_limit
// units; the total at most total_limit; at most row_limit rows.
//
// Execution: one thread. A cursor belongs to one index and is valid until
// that index's next edit; a stale cursor re-seeks from the root (seek
// calls) or from its row (advance).

#include "stx_btree/page_allocator.hpp"
#include "stx_btree/sequence_tree.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

namespace stx_btree {

// A row position. Counts of rows are RowCount.
struct RowIndex final {
    std::uint64_t value{};

    [[nodiscard]] bool operator==(const RowIndex& other) const noexcept {
        const bool same = value == other.value;
        return same;
    }
    [[nodiscard]] bool operator!=(const RowIndex& other) const noexcept {
        const bool differs = value != other.value;
        return differs;
    }
    [[nodiscard]] bool operator<(const RowIndex& other) const noexcept {
        const bool below = value < other.value;
        return below;
    }
};

// Where a row is: its top, its height and its state.
struct RowExtent final {
    HeightUnits top{};
    HeightUnits height{};
    RowState state{RowState::estimated};
};

// The configuration measured as P5/P6 (study §9).
using RowHeightConfig = detail::SequenceConfig<21, 127, true, true>;

template <typename Allocator = GlobalPageAllocator>
class RowHeightIndex final {
public:
    using Tree = detail::SequenceTree<RowHeightConfig, Allocator>;

    static constexpr HeightUnits height_limit{detail::sequence_height_limit};
    static constexpr HeightUnits total_limit{Tree::total_limit};
    static constexpr RowCount row_limit = Tree::row_limit;

    // A position with the path to it.
    class Cursor final {
    public:
        [[nodiscard]] RowIndex row() const noexcept {
            const RowIndex row_index{cursor_.row()};
            return row_index;
        }
        [[nodiscard]] HeightUnits top() const noexcept {
            const HeightUnits row_top{cursor_.top()};
            return row_top;
        }
        // 0 at the end.
        [[nodiscard]] HeightUnits height() const noexcept {
            const HeightUnits row_height{cursor_.height()};
            return row_height;
        }
        [[nodiscard]] RowState state() const noexcept { return cursor_.state(); }
        [[nodiscard]] bool at_end() const noexcept { return cursor_.at_end(); }

    private:
        friend class RowHeightIndex;
        typename Tree::Cursor cursor_{};
    };

    // An estimate outside [1, height_limit] falls back to 64 units (one
    // logical unit). Throws std::bad_alloc if the first page cannot be
    // allocated.
    explicit RowHeightIndex(HeightUnits estimate, const Allocator& allocator = Allocator())
        : tree_(estimate.value, allocator) {}
    RowHeightIndex(const RowHeightIndex&) = delete;
    RowHeightIndex& operator=(const RowHeightIndex&) = delete;
    RowHeightIndex(RowHeightIndex&&) = delete;
    RowHeightIndex& operator=(RowHeightIndex&&) = delete;

    // Replaces every row with `rows` estimated rows at `estimate`, which
    // becomes the estimate. Refused past the limits.
    bool assign(RowCount rows, HeightUnits estimate) { return tree_.assign(rows, estimate.value); }

    [[nodiscard]] RowCount count() const noexcept { return tree_.count(); }
    // Rows in the measured state, O(1).
    [[nodiscard]] RowCount measured_count() const noexcept { return tree_.measured_count(); }
    [[nodiscard]] HeightUnits total() const noexcept {
        const HeightUnits sum{tree_.total()};
        return sum;
    }
    [[nodiscard]] HeightUnits estimate() const noexcept {
        const HeightUnits value{tree_.estimate()};
        return value;
    }

    // The exact sum of the heights of rows before `row`; total() at or
    // past the end.
    [[nodiscard]] HeightUnits offset_of(RowIndex row) const noexcept {
        const HeightUnits offset{tree_.offset_of(row.value)};
        return offset;
    }

    // The least row r with offset_of(r + 1) > offset; row 0 below zero or
    // when empty, the last row at or past total().
    [[nodiscard]] RowIndex row_at(HeightUnits offset) const noexcept {
        const RowIndex row{tree_.row_at(offset.value)};
        return row;
    }

    // One descent. Past the end: the bottom with height 0.
    [[nodiscard]] RowExtent extent_of(RowIndex row) const noexcept {
        const detail::SequenceExtent found = tree_.extent_of(row.value);
        const RowExtent extent{HeightUnits{found.top}, HeightUnits{found.height}, found.state};
        return extent;
    }

    // Records a measured height; refused (unchanged) past the end, outside
    // [0, height_limit] or past the total limit, and false when the row is
    // already measured at that height.
    bool set_height(RowIndex row, HeightUnits height) { return tree_.set_height(row.value, height); }

    // Records measured heights for consecutive rows from `row`, one leaf
    // pass per leaf touched; refused as a whole when any is invalid.
    bool set_heights(RowIndex row, std::span<const HeightUnits> heights) {
        return tree_.set_heights(row.value, heights);
    }

    // Inserts `rows` estimated rows before `row` (clamped to count()).
    bool insert(RowIndex row, RowCount rows) { return tree_.insert(row.value, rows); }

    // Erases rows [row, row + rows), clamped to the end.
    void erase(RowIndex row, RowCount rows) { tree_.erase(row.value, rows); }

    // Returns rows to the estimate; refused when the total would pass the
    // limit.
    bool forget(RowIndex row, RowCount rows) { return tree_.forget(row.value, rows); }

    // Keeps the rows' total and drops their detail: one approximate run.
    // Offsets outside the range are unchanged bit for bit.
    void coarsen(RowIndex row, RowCount rows) { tree_.coarsen(row.value, rows); }

    void seek_row(Cursor& cursor, RowIndex row) const noexcept { tree_.seek_row(cursor.cursor_, row.value); }
    void seek_offset(Cursor& cursor, HeightUnits offset) const noexcept {
        tree_.seek_offset(cursor.cursor_, offset.value);
    }
    // O(log_B d) from a valid cursor d rows from the target.
    void seek_offset_from(Cursor& cursor, HeightUnits offset) const noexcept {
        tree_.seek_offset_from(cursor.cursor_, offset.value);
    }
    // O(1) amortized.
    void advance(Cursor& cursor) const noexcept { tree_.advance(cursor.cursor_); }

    // All O(1).
    [[nodiscard]] std::size_t node_count() const noexcept { return tree_.node_count(); }
    [[nodiscard]] std::size_t run_count() const noexcept { return tree_.run_count(); }
    [[nodiscard]] std::size_t depth() const noexcept { return tree_.depth(); }
    [[nodiscard]] std::size_t page_bytes() const noexcept { return tree_.page_bytes(); }

    // Every structural and arithmetic invariant (study §8.2); for tests.
    [[nodiscard]] detail::InvariantReport check_invariants() const noexcept { return tree_.check_invariants(); }

private:
    Tree tree_;
};

} // namespace stx_btree
