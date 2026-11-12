#pragma once

// One interface over the indexes the harness compares: GUI.Forms' current
// RowHeightIndex (B0, doubles; compiled from a GUI.Forms checkout by path,
// for this experiment only) and the prototype sequence tree (P1-P6, exact
// 26.6 units). The driver speaks Units (1/64 logical unit); B0 converts at
// its edge, as GUI.Forms would convert at the API edge after the move.

#include "sequence_tree.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>

#if defined(ORDER_STATISTIC_HAS_BASELINE)
#include "src/controls/collections/row_height_index.hpp"
#endif

namespace stx_btree_research {

template <typename Config>
class PrototypeIndex final {
public:
    using Tree = SequenceTree<Config>;
    static constexpr bool has_cursor = true;
    static constexpr bool has_coarsen = true;
    static constexpr bool exact = true;

    explicit PrototypeIndex(Units estimate) : tree_(estimate) {}

    [[nodiscard]] RowCount count() const noexcept { return tree_.count(); }
    [[nodiscard]] Units total() const noexcept { return tree_.total(); }
    [[nodiscard]] Units estimate() const noexcept { return tree_.estimate(); }
    [[nodiscard]] Units offset_of(RowCount row) const noexcept { return tree_.offset_of(row); }
    [[nodiscard]] RowCount row_at(Units offset) const noexcept { return tree_.row_at(offset); }
    [[nodiscard]] Units height_of(RowCount row) const noexcept { return tree_.extent_of(row).height; }
    [[nodiscard]] bool measured(RowCount row) const noexcept {
        const bool is_measured = tree_.extent_of(row).state == RowState::measured;
        return is_measured;
    }
    bool assign(RowCount rows, Units estimate) { return tree_.assign(rows, estimate); }
    bool set_height(RowCount row, Units height) { return tree_.set_height(row, height); }
    bool set_heights(RowCount row, std::span<const Units> heights) { return tree_.set_heights(row, heights); }
    bool insert(RowCount row, RowCount rows) { return tree_.insert(row, rows); }
    void erase(RowCount row, RowCount rows) { tree_.erase(row, rows); }
    void coarsen(RowCount row, RowCount rows) { tree_.coarsen(row, rows); }
    [[nodiscard]] std::size_t run_count() const noexcept { return tree_.run_count(); }
    [[nodiscard]] std::size_t node_count() const noexcept { return tree_.node_count(); }
    [[nodiscard]] std::size_t depth() const noexcept { return tree_.depth(); }
    [[nodiscard]] std::size_t page_bytes() const noexcept { return tree_.page_bytes(); }
    [[nodiscard]] Tree& tree() noexcept { return tree_; }
    [[nodiscard]] const Tree& tree() const noexcept { return tree_; }

    // offset_of(row_at(y)) <= y < offset_of(row_at(y) + 1), exactly.
    [[nodiscard]] bool brackets(Units offset) const noexcept {
        const RowCount row = tree_.row_at(offset);
        const Units low = tree_.offset_of(row);
        const Units high = tree_.offset_of(row + 1);
        const bool last = row + 1 >= tree_.count();
        const bool holds = low <= offset && (offset < high || last);
        return holds;
    }
    void check(bool& ok, const char*& failure) const {
        const InvariantReport report = tree_.check_invariants();
        ok = report.ok;
        failure = report.failure;
    }
    void read_counters(std::uint64_t& descents, std::uint64_t& node_visits) const noexcept {
        const TreeCounters counters = tree_.counters();
        descents = counters.descents;
        node_visits = counters.node_visits;
    }

private:
    Tree tree_;
};

#if defined(ORDER_STATISTIC_HAS_BASELINE)

// B0: gui_forms::RowHeightIndex as it is at the recorded GUI.Forms commit.
class BaselineIndex final {
public:
    static constexpr bool has_cursor = false;
    static constexpr bool has_coarsen = false;
    static constexpr bool exact = false;

    explicit BaselineIndex(Units estimate) : index_(to_logical(estimate)), estimate_(estimate) {}

    [[nodiscard]] RowCount count() const noexcept { return static_cast<RowCount>(index_.count()); }
    [[nodiscard]] Units total() const noexcept { return to_units(index_.total_height()); }
    [[nodiscard]] Units estimate() const noexcept { return estimate_; }
    [[nodiscard]] Units offset_of(RowCount row) const noexcept {
        return to_units(index_.offset_of(static_cast<std::size_t>(row)));
    }
    [[nodiscard]] RowCount row_at(Units offset) const noexcept {
        return static_cast<RowCount>(index_.row_at(to_logical(offset)));
    }
    [[nodiscard]] Units height_of(RowCount row) const noexcept {
        return to_units(index_.height_of(static_cast<std::size_t>(row)));
    }
    [[nodiscard]] bool measured(RowCount row) const noexcept { return index_.measured(static_cast<std::size_t>(row)); }
    bool assign(RowCount rows, Units estimate) {
        estimate_ = estimate;
        index_.assign(static_cast<std::size_t>(rows), to_logical(estimate));
        return true;
    }
    bool set_height(RowCount row, Units height) {
        return index_.set_height(static_cast<std::size_t>(row), to_logical(height));
    }
    // The view sets one row at a time; so does B0.
    bool set_heights(RowCount row, std::span<const Units> heights) {
        for (std::size_t index = 0; index < heights.size(); index += 1) {
            static_cast<void>(index_.set_height(static_cast<std::size_t>(row) + index, to_logical(heights[index])));
        }
        return true;
    }
    bool insert(RowCount row, RowCount rows) {
        index_.insert(static_cast<std::size_t>(row), static_cast<std::size_t>(rows));
        return true;
    }
    void erase(RowCount row, RowCount rows) {
        index_.erase(static_cast<std::size_t>(row), static_cast<std::size_t>(rows));
    }
    void coarsen(RowCount, RowCount) {}
    [[nodiscard]] std::size_t run_count() const noexcept { return index_.run_count(); }
    [[nodiscard]] std::size_t node_count() const noexcept { return index_.node_count(); }
    [[nodiscard]] std::size_t depth() const noexcept { return index_.depth(); }
    // B0's nodes are heap objects with vectors; its bytes come from the
    // allocator statistics the bench records.
    [[nodiscard]] std::size_t page_bytes() const noexcept { return 0; }

    // The bracketing property in B0's own doubles (no conversion).
    [[nodiscard]] bool brackets(Units offset) const noexcept {
        const double y = to_logical(offset);
        const std::size_t row = index_.row_at(y);
        const double low = index_.offset_of(row);
        const double high = index_.offset_of(row + 1);
        const bool last = row + 1 >= index_.count();
        const bool holds = low <= y && (y < high || last);
        return holds;
    }
    void check(bool& ok, const char*& failure) const noexcept {
        ok = true;
        failure = "";
    }
    void read_counters(std::uint64_t& descents, std::uint64_t& node_visits) const noexcept {
        descents = 0;
        node_visits = 0;
    }

private:
    [[nodiscard]] static double to_logical(Units units) noexcept { return static_cast<double>(units) / 64.0; }
    [[nodiscard]] static Units to_units(double logical) noexcept {
        return static_cast<Units>(std::llround(logical * 64.0));
    }

    gui_forms::RowHeightIndex index_;
    Units estimate_;
};

#endif

} // namespace stx_btree_research
