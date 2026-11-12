// The sequence tree's proved properties as tests (study
// btree/docs/ORDER_STATISTIC_RESEARCH.md §8.2), beside the exhaustive
// small-scope check (sequence_tree_exhaustive_test.cpp):
//
// - Lemma S: splitting a run at any row keeps every row's height, and the
//   run's inverse and rows_within agree with its prefix (every run of up to
//   16 rows and 80 units, every split).
// - Coarsen: offsets outside a coarsened range are unchanged bit for bit,
//   and rows inside follow Lemma S, for every range of a 40-row sequence
//   of mixed states, in the smallest configuration and the production one.
// - Finger: seek_offset_from from every row equals seek_offset at every
//   row boundary and one unit either side, in a deep small-capacity tree.
// - Cursors: a walk equals extent_of; a stale cursor re-seeks.
// - Failure: every allocation an edit makes can fail; on each failure the
//   tree is unchanged (every extent, count, total and invariant), and the
//   edit then succeeds when allocation is allowed. Refusals leave the tree
//   unchanged.
// - RowHeightIndex: a deterministic chat-like schedule at production
//   capacities against a flat model, invariants I1-I6 after every step.

#include "stx_btree/row_height_index.hpp"
#include "stx_btree/sequence_tree.hpp"
#include "test_support.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <new>
#include <span>
#include <vector>

namespace {

using stx_btree::GlobalPageAllocator;
using stx_btree::HeightUnits;
using stx_btree::PageKind;
using stx_btree::RowCount;
using stx_btree::RowExtent;
using stx_btree::RowHeightIndex;
using stx_btree::RowIndex;
using stx_btree::RowState;
using stx_btree::detail::InvariantReport;
using stx_btree::detail::Run;
using stx_btree::detail::RunArithmetic;
using stx_btree::detail::SequenceConfig;
using stx_btree::detail::SequenceExtent;
using stx_btree::detail::SequenceTree;
using stx_btree::detail::Units;
using stx_btree_tests::SplitMix64;
using stx_btree_tests::TestFailure;

using SmallConfig = SequenceConfig<4, 3, true, true>;
using SmallLinearConfig = SequenceConfig<4, 3, false, true>;
using ProductionConfig = stx_btree::RowHeightConfig;

// An allocator that allows a set number of page allocations, then throws
// std::bad_alloc, and counts live pages. The budget outlives every tree
// that borrows it.
struct AllocationBudget final {
    bool limited{};
    std::size_t remaining{};
    std::size_t live{};
};

class BudgetPageAllocator final {
public:
    explicit BudgetPageAllocator(AllocationBudget* budget) noexcept : budget_(budget) {}

    [[nodiscard]] void* allocate_page(PageKind kind, std::size_t bytes, std::size_t alignment) const {
        if ((*budget_).limited) {
            if ((*budget_).remaining == 0) {
                throw std::bad_alloc();
            }
            (*budget_).remaining -= 1;
        }
        const GlobalPageAllocator global{};
        void* page = global.allocate_page(kind, bytes, alignment);
        (*budget_).live += 1;
        return page;
    }

    void deallocate_page(PageKind kind, void* page, std::size_t bytes, std::size_t alignment) const noexcept {
        const GlobalPageAllocator global{};
        global.deallocate_page(kind, page, bytes, alignment);
        (*budget_).live -= 1;
    }

    [[nodiscard]] bool operator==(const BudgetPageAllocator& other) const noexcept {
        const bool same = budget_ == other.budget_;
        return same;
    }

private:
    AllocationBudget* budget_;
};

// Every row's extent, with the count and total: what "unchanged" compares.
struct Snapshot final {
    RowCount count{};
    Units total{};
    std::vector<SequenceExtent> extents;
};

template <typename Tree>
void take_snapshot(const Tree& tree, Snapshot& snapshot) {
    snapshot.count = tree.count();
    snapshot.total = tree.total();
    snapshot.extents.clear();
    snapshot.extents.reserve(static_cast<std::size_t>(tree.count()));
    for (RowCount row = 0; row < tree.count(); row += 1) {
        snapshot.extents.push_back(tree.extent_of(row));
    }
}

[[nodiscard]] bool same_snapshot(const Snapshot& left, const Snapshot& right) {
    if (left.count != right.count || left.total != right.total || left.extents.size() != right.extents.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.extents.size(); index += 1) {
        const SequenceExtent& a = left.extents[index];
        const SequenceExtent& b = right.extents[index];
        if (a.top != b.top || a.height != b.height || a.state != b.state) {
            return false;
        }
    }
    return true;
}

template <typename Tree>
void expect_invariants(const Tree& tree, const char* label) {
    const InvariantReport report = tree.check_invariants();
    if (!report.ok) {
        std::fprintf(stderr, "invariant: %s\n", report.failure);
    }
    TestFailure::expect(report.ok, label, "invariants I1-I6");
}

// ---- Lemma S ----------------------------------------------------------------

void check_lemma_s() {
    for (RowCount count = 1; count <= 16; count += 1) {
        for (Units total = 0; total <= 80; total += 1) {
            const Run run{count, total, RowState::approximate};
            Units prefix = 0;
            for (RowCount row = 0; row < count; row += 1) {
                TestFailure::expect(RunArithmetic::prefix(run, row) == prefix, "lemma S", "prefix is the heights' sum");
                const Units height = RunArithmetic::height_at(run, row);
                for (Units offset = prefix; offset < prefix + height; offset += 1) {
                    TestFailure::expect(RunArithmetic::inverse(run, offset) == row, "lemma S", "inverse");
                }
                prefix = prefix + height;
            }
            TestFailure::expect(prefix == total, "lemma S", "heights sum to the total");
            for (Units limit = 0; limit <= total + 2; limit += 1) {
                const RowCount within = RunArithmetic::rows_within(run, limit);
                const bool fits = RunArithmetic::prefix(run, within) <= limit;
                const bool most = within == count || RunArithmetic::prefix(run, within + 1) > limit;
                TestFailure::expect(fits && most, "lemma S", "rows_within is the most rows within the limit");
            }
            for (RowCount split = 1; split < count; split += 1) {
                Run back = run;
                const Run front = RunArithmetic::take_front(back, split);
                for (RowCount row = 0; row < count; row += 1) {
                    const Units whole = RunArithmetic::height_at(run, row);
                    const Units part = row < split ? RunArithmetic::height_at(front, row)
                                                   : RunArithmetic::height_at(back, row - split);
                    TestFailure::expect(whole == part, "lemma S", "a split keeps every row's height");
                }
            }
        }
    }
}

// ---- coarsen ------------------------------------------------------------------

// 40 rows: measured, estimated and approximate stretches, dense-eligible
// and not.
template <typename Tree>
void build_mixed(Tree& tree) {
    TestFailure::expect(tree.assign(40, 3), "coarsen", "assign");
    for (RowCount row = 0; row < 40; row += 1) {
        if (row % 3 == 0) {
            const Units height = 5 + static_cast<Units>((row * 7) % 13);
            TestFailure::expect(tree.set_height(row, HeightUnits{height}), "coarsen", "set_height");
        }
    }
    const HeightUnits tall[] = {HeightUnits{stx_btree::detail::dense_height_limit + 9}, HeightUnits{0},
                                HeightUnits{1}};
    TestFailure::expect(tree.set_heights(20, std::span<const HeightUnits>(tall, 3)), "coarsen", "set_heights");
    tree.coarsen(30, 5);
    TestFailure::expect(tree.forget(33, 1), "coarsen", "forget");
}

template <typename Config>
void check_coarsen_ranges(const char* label) {
    using Tree = SequenceTree<Config>;
    Snapshot before;
    for (RowCount first = 0; first < 40; first += 1) {
        for (RowCount rows = 1; rows <= 42 - first; rows += 1) {
            Tree tree(3);
            build_mixed(tree);
            take_snapshot(tree, before);
            const RowCount end = first + rows < 40 ? first + rows : 40;
            tree.coarsen(first, rows);
            expect_invariants(tree, label);
            TestFailure::expect(tree.count() == 40 && tree.total() == before.total, label, "count and total kept");
            // Outside the range: every offset bit for bit.
            for (RowCount row = 0; row <= 40; row += 1) {
                if (row > first && row < end) {
                    continue;
                }
                const Units expected = row < 40 ? before.extents[static_cast<std::size_t>(row)].top : before.total;
                TestFailure::expect(tree.offset_of(row) == expected, label, "offset outside the range moved");
                if (row < 40 && (row < first || row >= end)) {
                    const SequenceExtent extent = tree.extent_of(row);
                    const SequenceExtent& old = before.extents[static_cast<std::size_t>(row)];
                    TestFailure::expect(extent.height == old.height && extent.state == old.state, label,
                                        "row outside the range changed");
                }
            }
            // Inside: one approximate run, Lemma S.
            const Units start = before.extents[static_cast<std::size_t>(first)].top;
            const Units finish = end < 40 ? before.extents[static_cast<std::size_t>(end)].top : before.total;
            const Run run{end - first, finish - start, RowState::approximate};
            for (RowCount within = 0; within < end - first; within += 1) {
                const SequenceExtent extent = tree.extent_of(first + within);
                const bool top = extent.top == start + RunArithmetic::prefix(run, within);
                const bool height = extent.height == RunArithmetic::height_at(run, within);
                TestFailure::expect(top && height && extent.state == RowState::approximate, label,
                                    "Lemma S inside the range");
            }
        }
    }
}

// ---- finger seeks -------------------------------------------------------------

template <typename Config>
void check_finger(const char* label) {
    using Tree = SequenceTree<Config>;
    Tree tree(4);
    TestFailure::expect(tree.assign(600, 4), label, "assign");
    for (RowCount row = 0; row < 600; row += 1) {
        if (row % 5 != 4) {
            const Units height = static_cast<Units>((row * 11) % 17);
            static_cast<void>(tree.set_height(row, HeightUnits{height}));
        }
    }
    tree.coarsen(100, 37);
    expect_invariants(tree, label);
    TestFailure::expect(tree.depth() >= 4, label, "a deep tree");
    std::vector<Units> targets;
    for (RowCount row = 0; row < tree.count(); row += 1) {
        const Units top = tree.offset_of(row);
        targets.push_back(top - 1);
        targets.push_back(top);
        targets.push_back(top + 1);
    }
    targets.push_back(tree.total());
    typename Tree::Cursor finger{};
    typename Tree::Cursor fresh{};
    for (RowCount start = 0; start < tree.count(); start += 1) {
        for (const Units target : targets) {
            tree.seek_row(finger, start);
            tree.seek_offset_from(finger, target);
            tree.seek_offset(fresh, target);
            const RowCount expected = tree.row_at(target);
            const bool same = finger.row() == expected && fresh.row() == expected &&
                              finger.top() == tree.offset_of(expected) && fresh.top() == finger.top() &&
                              finger.height() == fresh.height() && !finger.at_end();
            TestFailure::expect(same, label, "seek_offset_from equals seek_offset");
        }
    }
}

// ---- cursors --------------------------------------------------------------------

void check_cursors() {
    using Tree = SequenceTree<SmallConfig>;
    Tree tree(2);
    TestFailure::expect(tree.assign(200, 2), "cursor", "assign");
    for (RowCount row = 0; row < 200; row += 3) {
        static_cast<void>(tree.set_height(row, HeightUnits{static_cast<Units>(row % 9)}));
    }
    typename Tree::Cursor cursor{};
    tree.seek_row(cursor, 0);
    for (RowCount row = 0; row < 200; row += 1) {
        const SequenceExtent extent = tree.extent_of(row);
        const bool same = !cursor.at_end() && cursor.row() == row && cursor.top() == extent.top &&
                          cursor.height() == extent.height && cursor.state() == extent.state;
        TestFailure::expect(same, "cursor", "a walk equals extent_of");
        tree.advance(cursor);
    }
    TestFailure::expect(cursor.at_end() && cursor.height() == 0, "cursor", "the walk ends");
    // A stale cursor: an edit before its row moves the row's top.
    tree.seek_row(cursor, 150);
    TestFailure::expect(tree.insert(10, 7), "cursor", "insert");
    tree.seek_offset_from(cursor, tree.offset_of(120));
    TestFailure::expect(cursor.row() == 120 && cursor.top() == tree.offset_of(120), "cursor",
                        "a stale cursor re-seeks");
    tree.seek_row(cursor, 50);
    tree.erase(0, 5);
    tree.advance(cursor);
    TestFailure::expect(cursor.row() == 51 && cursor.top() == tree.offset_of(51), "cursor",
                        "a stale cursor advances from its row");
    // A cursor from another tree.
    Tree other(2);
    TestFailure::expect(other.assign(10, 5), "cursor", "assign");
    tree.seek_offset_from(cursor, 0);
    other.seek_offset_from(cursor, 12);
    TestFailure::expect(cursor.row() == 2 && cursor.top() == 10, "cursor", "a foreign cursor re-seeks");
}

// ---- failure -----------------------------------------------------------------------

enum class EditKind : std::uint8_t { set_height, set_heights, insert, erase, forget, coarsen, assign, assign_tall };

template <typename Tree>
bool apply_edit(Tree& tree, EditKind kind, RowCount row) {
    switch (kind) {
    case EditKind::set_height:
        return tree.set_height(row, HeightUnits{stx_btree::detail::dense_height_limit + 1});
    case EditKind::set_heights: {
        std::vector<HeightUnits> heights;
        for (Units index = 0; index < 40; index += 1) {
            heights.push_back(HeightUnits{index % 3 == 0 ? stx_btree::detail::dense_height_limit + index : index});
        }
        return tree.set_heights(row, std::span<const HeightUnits>(heights.data(), heights.size()));
    }
    case EditKind::insert:
        return tree.insert(row, 5);
    case EditKind::erase:
        tree.erase(row, 23);
        return true;
    case EditKind::forget:
        return tree.forget(row, 17);
    case EditKind::coarsen:
        tree.coarsen(row, 19);
        return true;
    case EditKind::assign:
        return tree.assign(300, 7);
    case EditKind::assign_tall:
        // 40 rows of 2^36 units: three rows a packed piece, so 14 pieces
        // in five segments.
        return tree.assign(40, static_cast<Units>(1) << 36);
    default:
        return false;
    }
}

// A tree of mixed runs (dense and packed leaves) deep enough that edits
// split and merge pages at several levels. Rows 100-119 are 2^36 units
// tall, so forget and coarsen over them insert runs of several pieces in
// segments.
template <typename Tree>
void build_for_failure(Tree& tree) {
    TestFailure::expect(tree.assign(300, 3), "failure", "assign");
    for (RowCount row = 0; row < 300; row += 1) {
        if (row % 7 != 3) {
            static_cast<void>(tree.set_height(row, HeightUnits{1 + static_cast<Units>(row % 5)}));
        }
    }
    std::vector<HeightUnits> tall(20, HeightUnits{static_cast<Units>(1) << 36});
    TestFailure::expect(tree.set_heights(100, std::span<const HeightUnits>(tall.data(), tall.size())), "failure",
                        "set_heights");
    tree.coarsen(200, 9);
}

void check_allocation_failure() {
    using Tree = SequenceTree<SmallConfig, BudgetPageAllocator>;
    const EditKind kinds[] = {EditKind::set_height, EditKind::set_heights, EditKind::insert,
                              EditKind::erase,      EditKind::forget,      EditKind::coarsen,
                              EditKind::assign,     EditKind::assign_tall};
    const RowCount rows[] = {0, 1, 2, 57, 100, 110, 150, 151, 298, 299};
    Snapshot before;
    Snapshot after;
    Snapshot expected;
    for (const EditKind kind : kinds) {
        for (const RowCount row : rows) {
            AllocationBudget budget{};
            {
                // The reference: the same edit with unlimited pages.
                Tree reference(3, BudgetPageAllocator(&budget));
                build_for_failure(reference);
                const bool reference_result = apply_edit(reference, kind, row);
                take_snapshot(reference, expected);

                Tree tree(3, BudgetPageAllocator(&budget));
                build_for_failure(tree);
                take_snapshot(tree, before);
                bool succeeded = false;
                for (std::size_t allowed = 0; allowed < 4096 && !succeeded; allowed += 1) {
                    budget.limited = true;
                    budget.remaining = allowed;
                    bool threw = false;
                    bool result = false;
                    try {
                        result = apply_edit(tree, kind, row);
                    } catch (const std::bad_alloc&) {
                        threw = true;
                    }
                    budget.limited = false;
                    if (threw) {
                        take_snapshot(tree, after);
                        TestFailure::expect(same_snapshot(before, after), "failure",
                                            "a failed allocation left the tree changed");
                        expect_invariants(tree, "failure");
                        continue;
                    }
                    succeeded = true;
                    TestFailure::expect(result == reference_result, "failure", "result differs from the reference");
                    take_snapshot(tree, after);
                    TestFailure::expect(same_snapshot(after, expected), "failure",
                                        "edit after failures differs from the reference");
                    expect_invariants(tree, "failure");
                }
                TestFailure::expect(succeeded, "failure", "the edit never succeeded");
            }
            TestFailure::expect(budget.live == 0, "failure", "pages leaked");
        }
    }
}

void check_refusals() {
    using Limited = SequenceConfig<4, 3, true, true, (static_cast<Units>(1) << 33), 200>;
    using Tree = SequenceTree<Limited>;
    Tree tree(3);
    TestFailure::expect(tree.assign(150, 3), "refusal", "assign");
    tree.coarsen(40, 30);
    static_cast<void>(tree.set_height(5, HeightUnits{9}));
    Snapshot before;
    Snapshot after;
    take_snapshot(tree, before);
    const HeightUnits bad[] = {HeightUnits{4}, HeightUnits{-1}};
    const HeightUnits past[] = {HeightUnits{4}, HeightUnits{4}};
    const bool refused = !tree.set_height(150, HeightUnits{4}) && !tree.set_height(3, HeightUnits{-1}) &&
                         !tree.set_height(3, HeightUnits{stx_btree::detail::sequence_height_limit + 1}) &&
                         !tree.set_height(5, HeightUnits{9}) &&
                         !tree.set_heights(10, std::span<const HeightUnits>(bad, 2)) &&
                         !tree.set_heights(149, std::span<const HeightUnits>(past, 2)) && !tree.insert(0, 51) &&
                         !tree.assign(201, 3) && !tree.assign(3, static_cast<Units>(1) << 32);
    TestFailure::expect(refused, "refusal", "invalid edits refused");
    take_snapshot(tree, after);
    TestFailure::expect(same_snapshot(before, after), "refusal", "a refusal changed the tree");
    // The total limit: 2^33 units.
    TestFailure::expect(!tree.set_height(0, HeightUnits{static_cast<Units>(1) << 33}), "refusal", "total limit");
    take_snapshot(tree, after);
    TestFailure::expect(same_snapshot(before, after), "refusal", "a refusal changed the tree");
    expect_invariants(tree, "refusal");
}

// ---- RowHeightIndex at production capacities ------------------------------------

// The model: one height and state per row.
struct FlatRows final {
    std::vector<Units> heights;
    std::vector<RowState> states;
};

void check_row_height_index(SplitMix64& random) {
    RowHeightIndex<> index(HeightUnits{5728});
    FlatRows model{};
    const RowCount start_rows = 20000;
    TestFailure::expect(index.assign(start_rows, HeightUnits{5728}), "index", "assign");
    model.heights.assign(static_cast<std::size_t>(start_rows), 5728);
    model.states.assign(static_cast<std::size_t>(start_rows), RowState::estimated);
    std::vector<HeightUnits> window;
    for (std::size_t step = 0; step < 400; step += 1) {
        const std::size_t count = model.heights.size();
        const RowCount at = random.below(static_cast<std::uint64_t>(count));
        const std::uint64_t choice = random.below(10);
        if (choice < 5) {
            // A window of measurements.
            const std::size_t length = static_cast<std::size_t>(1 + random.below(300));
            const std::size_t first = static_cast<std::size_t>(at);
            const std::size_t taken = length < count - first ? length : count - first;
            window.clear();
            for (std::size_t offset = 0; offset < taken; offset += 1) {
                const Units height = 64 * (40 + static_cast<Units>(random.below(200)));
                window.push_back(HeightUnits{height});
                model.heights[first + offset] = height;
                model.states[first + offset] = RowState::measured;
            }
            TestFailure::expect(index.set_heights(RowIndex{at}, std::span<const HeightUnits>(window.data(), taken)),
                                "index", "set_heights");
        } else if (choice < 7) {
            // Appends and inserts.
            const RowCount rows = 1 + random.below(50);
            const std::size_t place = choice == 5 ? count : static_cast<std::size_t>(at);
            TestFailure::expect(index.insert(RowIndex{static_cast<std::uint64_t>(place)}, rows), "index", "insert");
            model.heights.insert(model.heights.begin() + static_cast<std::ptrdiff_t>(place),
                                 static_cast<std::size_t>(rows), 5728);
            model.states.insert(model.states.begin() + static_cast<std::ptrdiff_t>(place),
                                static_cast<std::size_t>(rows), RowState::estimated);
        } else if (choice < 8) {
            const RowCount rows = 1 + random.below(40);
            index.erase(RowIndex{at}, rows);
            const std::size_t first = static_cast<std::size_t>(at);
            const std::size_t taken = static_cast<std::size_t>(rows) < count - first ? static_cast<std::size_t>(rows)
                                                                                     : count - first;
            model.heights.erase(model.heights.begin() + static_cast<std::ptrdiff_t>(first),
                                model.heights.begin() + static_cast<std::ptrdiff_t>(first + taken));
            model.states.erase(model.states.begin() + static_cast<std::ptrdiff_t>(first),
                               model.states.begin() + static_cast<std::ptrdiff_t>(first + taken));
        } else {
            // Coarsen a far stretch, as the run budget does.
            const RowCount rows = 1 + random.below(3000);
            const std::size_t first = static_cast<std::size_t>(at);
            const std::size_t taken = static_cast<std::size_t>(rows) < count - first ? static_cast<std::size_t>(rows)
                                                                                     : count - first;
            Units sum = 0;
            for (std::size_t offset = 0; offset < taken; offset += 1) {
                sum = sum + model.heights[first + offset];
            }
            const Units base = sum / static_cast<Units>(taken);
            const Units taller = sum % static_cast<Units>(taken);
            for (std::size_t offset = 0; offset < taken; offset += 1) {
                model.heights[first + offset] = base + (static_cast<Units>(offset) < taller ? 1 : 0);
                model.states[first + offset] = RowState::approximate;
            }
            index.coarsen(RowIndex{at}, rows);
        }
        expect_invariants(index, "index");
        TestFailure::expect(index.count() == static_cast<RowCount>(model.heights.size()), "index", "count");
        if (step % 50 != 49) {
            continue;
        }
        // Every row, by cursor walk and by query.
        RowHeightIndex<>::Cursor cursor{};
        index.seek_row(cursor, RowIndex{0});
        Units top = 0;
        for (std::size_t row = 0; row < model.heights.size(); row += 1) {
            const bool walk = !cursor.at_end() && cursor.top().value == top &&
                              cursor.height().value == model.heights[row] && cursor.state() == model.states[row];
            TestFailure::expect(walk, "index", "cursor walk against the model");
            if (row % 97 == 0) {
                const RowExtent extent = index.extent_of(RowIndex{static_cast<std::uint64_t>(row)});
                const bool query = extent.top.value == top && index.offset_of(RowIndex{static_cast<std::uint64_t>(row)}).value == top &&
                                   index.row_at(HeightUnits{top}).value == row;
                TestFailure::expect(query, "index", "queries against the model");
            }
            top = top + model.heights[row];
            index.advance(cursor);
        }
        TestFailure::expect(cursor.at_end() && index.total().value == top, "index", "total");
        std::size_t measured = 0;
        for (const RowState state : model.states) {
            if (state == RowState::measured) {
                measured += 1;
            }
        }
        TestFailure::expect(index.measured_count() == static_cast<RowCount>(measured), "index", "measured_count");
    }
    TestFailure::expect(index.node_count() > 0 && index.page_bytes() > 0 && index.depth() >= 2, "index",
                        "storage statistics");
}

} // namespace

int main() {
    SplitMix64 random(0x5e97'0ee0'2026ULL);
    check_lemma_s();
    check_coarsen_ranges<SmallConfig>("coarsen small");
    check_coarsen_ranges<ProductionConfig>("coarsen production");
    check_finger<SmallConfig>("finger counting");
    check_finger<SmallLinearConfig>("finger linear");
    check_cursors();
    check_allocation_failure();
    check_refusals();
    check_row_height_index(random);
    std::printf("sequence tree tests passed\n");
    return 0;
}
