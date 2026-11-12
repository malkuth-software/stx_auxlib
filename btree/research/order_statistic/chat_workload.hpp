#pragma once

// The chat demonstrator's index traffic (CHAT_STRESS_SPEC §4 and §7),
// emulated without a window: a viewport of 1280x800 logical units at an
// offset, half a viewport of overscan above and below, lazy measuring of
// rows entering the band, anchoring, and paint's walk over the realized
// rows. Study §5.2 lists the workloads.
//
// Two access modes, a declared choice of the driver:
//   per row  (UseCursor = false): the calls VirtualRowsView makes today,
//            several root descents per row (offset_of, measured, height_of,
//            set_height);
//   cursor   (UseCursor = true): one seek, then steps; measured heights go
//            in as one batch per stretch of unmeasured rows.

#include "index_adapters.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace stx_btree_research {

class SplitMix64 final {
public:
    explicit SplitMix64(std::uint64_t seed) noexcept : state_(seed) {}
    [[nodiscard]] static std::uint64_t mix(std::uint64_t value) noexcept {
        std::uint64_t z = value + 0x9e3779b97f4a7c15ULL;
        z = (z ^ (z >> 30U)) * 0xbf58476d1ce4e5b9ULL;
        z = (z ^ (z >> 27U)) * 0x94d049bb133111ebULL;
        const std::uint64_t mixed = z ^ (z >> 31U);
        return mixed;
    }
    [[nodiscard]] std::uint64_t next() noexcept {
        const std::uint64_t value = mix(state_);
        state_ = state_ + 0x9e3779b97f4a7c15ULL;
        return value;
    }
    // Uniform in [0, bound), bound > 0 (modulo bias is irrelevant here).
    [[nodiscard]] std::uint64_t below(std::uint64_t bound) noexcept {
        const std::uint64_t value = next() % bound;
        return value;
    }

private:
    std::uint64_t state_;
};

// A chat row's measured height in Units, a function of (seed, row): a
// 40-unit header on one row in four, ceil(length * 7.5 / 1180) body lines
// of 18 units for a message length uniform in [100, 1000], 8 units of
// margin, and a sub-unit fraction so neighbouring heights rarely match.
class ChatHeights final {
public:
    explicit ChatHeights(std::uint64_t seed) noexcept : seed_(seed) {}
    [[nodiscard]] Units height(RowCount row) const noexcept {
        const std::uint64_t mixed = SplitMix64::mix(seed_ ^ (row * 0x2545f4914f6cdd1dULL));
        const std::uint64_t length = 100U + mixed % 901U;
        const std::uint64_t lines = (length * 75U + 11799U) / 11800U;
        const std::uint64_t header = ((mixed >> 32U) % 4U) == 0U ? 40U : 0U;
        const std::uint64_t logical = header + lines * 18U + 8U;
        const std::uint64_t fraction = (mixed >> 40U) & 63U;
        const Units units = static_cast<Units>(logical * 64U + fraction);
        return units;
    }
    // The estimate CHAT §4.2 proposes: the distribution's mean, 1/64 unit.
    [[nodiscard]] static Units mean_estimate() noexcept {
        // lines average about 3.97 (ceil of uniform 0.64..6.36), headers 10:
        // 10 + 3.97 * 18 + 8 = 89.5 logical units.
        return static_cast<Units>(89.5 * 64.0);
    }

private:
    std::uint64_t seed_;
};

// The driver's finger type: the tree's cursor in the cursor mode, nothing
// otherwise.
struct NoCursor final {};
template <typename Index, bool UseCursor>
struct CursorOf final {
    using Type = NoCursor;
};
template <typename Index>
struct CursorOf<Index, true> final {
    using Type = typename Index::Tree::Cursor;
};

struct ViewShape final {
    Units viewport = 800 * 64;
    Units overscan = 400 * 64;
    std::size_t budget = 4096;  // rows measured per settle, as VirtualRowsView
    std::size_t passes = 8;     // measure-and-place passes per settle
};

struct ViewCounters final {
    std::uint64_t settles{};
    std::uint64_t rows_measured{};
    std::uint64_t rows_realized{};
    std::uint64_t coarsenings{};
    // Sum of realized rows' heights and tops: keeps the work observable.
    std::uint64_t checksum{};
};

template <typename Index, bool UseCursor>
class ViewEmulator final {
public:
    ViewEmulator(Index& index, const ChatHeights& heights, ViewShape shape, std::size_t run_budget)
        : index_(index), heights_(heights), shape_(shape), run_budget_(run_budget), offset_(0), anchor_row_(0),
          anchor_top_(0), counters_(), stretch_heights_(), stretch_starts_(), stretch_lengths_() {
        stretch_heights_.reserve(shape.budget);
        stretch_starts_.reserve(shape.budget);
        stretch_lengths_.reserve(shape.budget);
        static_assert(!UseCursor || Index::has_cursor, "the cursor mode needs a cursor");
    }

    [[nodiscard]] const ViewCounters& counters() const noexcept { return counters_; }
    [[nodiscard]] Units offset() const noexcept { return offset_; }

    [[nodiscard]] Units maximum_offset() const noexcept {
        const Units excess = index_.total() - shape_.viewport;
        const Units maximum = excess > 0 ? excess : 0;
        return maximum;
    }

    void scroll_to(Units offset) {
        offset_ = clamp_offset(offset);
        capture_anchor();
        settle();
    }
    void scroll_by(Units delta) { scroll_to(offset_ + delta); }
    void jump_to_row(RowCount row) { scroll_to(index_.offset_of(row)); }
    void to_end() { scroll_to(maximum_offset()); }

    // A width change: every measured height is forgotten.
    void reset_width() {
        static_cast<void>(index_.assign(index_.count(), index_.estimate()));
        settle();
    }

    void settle() {
        counters_.settles += 1;
        std::size_t asked = 0;
        std::size_t pass = 0;
        while (pass < shape_.passes && asked < shape_.budget) {
            pass += 1;
            offset_ = anchored_offset();
            const Units top = offset_ - shape_.overscan;
            const Units bottom = offset_ + shape_.viewport + shape_.overscan;
            const std::size_t stored = measure_band(top > 0 ? top : 0, bottom, asked);
            if (stored == 0) {
                break;
            }
        }
        offset_ = anchored_offset();
        capture_anchor();
        realize();
        apply_run_budget();
    }

private:
    [[nodiscard]] Units clamp_offset(Units offset) const noexcept {
        const Units maximum = maximum_offset();
        if (offset < 0) {
            return 0;
        }
        if (offset > maximum) {
            return maximum;
        }
        return offset;
    }

    [[nodiscard]] Units anchored_offset() const noexcept {
        const Units top = index_.offset_of(anchor_row_) - anchor_top_;
        const Units placed = clamp_offset(top);
        return placed;
    }

    // The first row whose top is at or below the viewport's top.
    void capture_anchor() {
        const RowCount count = index_.count();
        if (count == 0) {
            anchor_row_ = 0;
            anchor_top_ = 0;
            return;
        }
        if constexpr (UseCursor) {
            typename Index::Tree& tree = index_.tree();
            tree.seek_offset_from(finger_, offset_);
            if (finger_.top < offset_ && finger_.row + 1 < count) {
                tree.advance(finger_);
            }
            anchor_row_ = finger_.row;
            anchor_top_ = finger_.top - offset_;
        } else {
            RowCount row = index_.row_at(offset_);
            Units top = index_.offset_of(row);
            if (top < offset_ && row + 1 < count) {
                row = row + 1;
                top = index_.offset_of(row);
            }
            anchor_row_ = row;
            anchor_top_ = top - offset_;
        }
    }

    std::size_t measure_band(Units top, Units bottom, std::size_t& asked) {
        const RowCount count = index_.count();
        std::size_t stored = 0;
        if constexpr (UseCursor) {
            typename Index::Tree& tree = index_.tree();
            stretch_heights_.clear();
            stretch_starts_.clear();
            stretch_lengths_.clear();
            tree.seek_offset_from(finger_, top);
            bool open = false;
            while (!finger_.at_end && finger_.top < bottom && asked < shape_.budget) {
                if (finger_.run.state != RowState::measured) {
                    if (!open) {
                        stretch_starts_.push_back(finger_.row);
                        stretch_lengths_.push_back(0);
                        open = true;
                    }
                    stretch_heights_.push_back(heights_.height(finger_.row));
                    stretch_lengths_.back() += 1;
                    asked += 1;
                } else {
                    open = false;
                }
                tree.advance(finger_);
            }
            std::size_t first = 0;
            for (std::size_t stretch = 0; stretch < stretch_starts_.size(); stretch += 1) {
                const std::size_t length = stretch_lengths_[stretch];
                const std::span<const Units> heights(stretch_heights_.data() + first, length);
                if (index_.set_heights(stretch_starts_[stretch], heights)) {
                    stored += length;
                }
                first += length;
            }
        } else {
            RowCount row = index_.row_at(top);
            while (row < count && asked < shape_.budget) {
                const Units row_top = index_.offset_of(row);
                if (row_top >= bottom) {
                    break;
                }
                if (!index_.measured(row)) {
                    asked += 1;
                    const Units estimate = index_.height_of(row);
                    counters_.checksum += static_cast<std::uint64_t>(estimate);
                    if (index_.set_height(row, heights_.height(row))) {
                        stored += 1;
                    }
                }
                row += 1;
            }
        }
        counters_.rows_measured += stored;
        return stored;
    }

    // Paint's walk over the rows meeting the viewport.
    void realize() {
        const RowCount count = index_.count();
        if (count == 0) {
            return;
        }
        const Units bottom = offset_ + shape_.viewport;
        if constexpr (UseCursor) {
            typename Index::Tree& tree = index_.tree();
            tree.seek_offset_from(finger_, offset_);
            while (!finger_.at_end && finger_.top < bottom) {
                counters_.rows_realized += 1;
                counters_.checksum += static_cast<std::uint64_t>(finger_.top);
                tree.advance(finger_);
            }
        } else {
            RowCount row = index_.row_at(offset_);
            while (row < count && index_.offset_of(row) < bottom) {
                counters_.rows_realized += 1;
                counters_.checksum += static_cast<std::uint64_t>(index_.height_of(row));
                row += 1;
            }
        }
    }

    // P6: when the runs pass the budget, coarsen everything farther than a
    // quarter budget of rows from the anchor. Offsets of rows outside the
    // coarsened ranges are unchanged, so the anchor stays put.
    void apply_run_budget() {
        if constexpr (Index::has_coarsen) {
            if (run_budget_ == 0 || index_.run_count() <= run_budget_) {
                return;
            }
            const RowCount count = index_.count();
            const RowCount keep = static_cast<RowCount>(run_budget_ / 4);
            const RowCount low = anchor_row_ > keep ? anchor_row_ - keep : 0;
            const RowCount high = count - anchor_row_ > keep ? anchor_row_ + keep : count;
            if (high < count) {
                index_.coarsen(high, count - high);
            }
            if (low > 0) {
                index_.coarsen(0, low);
            }
            counters_.coarsenings += 1;
        }
    }

    Index& index_;
    const ChatHeights& heights_;
    ViewShape shape_;
    std::size_t run_budget_;
    Units offset_;
    RowCount anchor_row_;
    Units anchor_top_;
    ViewCounters counters_;
    std::vector<Units> stretch_heights_;
    std::vector<RowCount> stretch_starts_;
    std::vector<std::size_t> stretch_lengths_;
    // The finger the cursor mode keeps between frames.
    typename CursorOf<Index, UseCursor>::Type finger_{};
};

} // namespace stx_btree_research
