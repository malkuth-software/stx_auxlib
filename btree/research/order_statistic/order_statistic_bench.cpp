// The chat workloads over B0 and the prototype variants (study §5). One
// process runs one variant on one workload and prints one CSV line, so
// every run starts fresh and the allocator figures belong to that run;
// run_bench.sh loops over variants, workloads and repetitions.
//
//   order_statistic_bench --variant P4 --workload W2 --rows 10000000
//       [--seed 1] [--budget 100000] [--run 1] [--check] [--header]
//
// Experiment code: the allocator and footprint readers below are host
// calls that the stx_auxlib package keeps in src/hosted; this program is
// not part of the package.

#include "chat_workload.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string>

#if defined(__APPLE__)
#include <mach/mach.h>
#include <malloc/malloc.h>
#elif defined(__linux__)
#include <malloc.h>
#include <unistd.h>
#endif

namespace {

using stx_btree_research::ChatHeights;
using stx_btree_research::PrototypeIndex;
using stx_btree_research::RowCount;
using stx_btree_research::SplitMix64;
using stx_btree_research::TreeConfig;
using stx_btree_research::Units;
using stx_btree_research::ViewEmulator;
using stx_btree_research::ViewShape;

using P1Config = TreeConfig<21, 127, false, false>;
using P2Config = TreeConfig<21, 127, true, false>;
using P3Config = TreeConfig<42, 127, true, false>;
using P5Config = TreeConfig<21, 127, true, true>;

static_assert(sizeof(stx_btree_research::LeafPage<P1Config>) == 1024, "1 KiB leaves");
static_assert(sizeof(stx_btree_research::BranchPage<P1Config>) == 512, "512-byte branches");
static_assert(sizeof(stx_btree_research::BranchPage<P3Config>) <= 1024, "1 KiB branches");

struct Options final {
    std::string variant{"P4"};
    std::string workload{"W2"};
    RowCount rows{10000000};
    std::uint64_t seed{1};
    std::size_t budget{0};
    unsigned run{1};
    bool check{false};
    bool header{false};
};

class HostMemory final {
public:
    // Bytes the allocator has handed out and not taken back.
    [[nodiscard]] static std::size_t allocator_in_use() noexcept {
#if defined(__APPLE__)
        malloc_statistics_t statistics{};
        malloc_zone_statistics(nullptr, &statistics);
        return statistics.size_in_use;
#elif defined(__linux__)
        const struct mallinfo2 info = mallinfo2();
        return info.uordblks + info.hblkhd;
#else
        return 0;
#endif
    }
    // The process's footprint (macOS phys_footprint, Linux resident set).
    [[nodiscard]] static std::size_t footprint() noexcept {
#if defined(__APPLE__)
        task_vm_info_data_t info{};
        mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
        const kern_return_t status =
            task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&info), &count);
        if (status != KERN_SUCCESS) {
            return 0;
        }
        return static_cast<std::size_t>(info.phys_footprint);
#elif defined(__linux__)
        std::FILE* file = std::fopen("/proc/self/statm", "r");
        if (file == nullptr) {
            return 0;
        }
        unsigned long size = 0;
        unsigned long resident = 0;
        const int read = std::fscanf(file, "%lu %lu", &size, &resident);
        std::fclose(file);
        if (read != 2) {
            return 0;
        }
        const long page = sysconf(_SC_PAGESIZE);
        return static_cast<std::size_t>(resident) * static_cast<std::size_t>(page);
#else
        return 0;
#endif
    }
};

struct Result final {
    double seconds{};
    std::size_t allocator_bytes{};
    std::size_t footprint_bytes{};
    std::uint64_t bracket_violations{};
    bool invariants_ok{true};
    const char* invariant_failure{""};
    std::uint64_t descents{};
    std::uint64_t node_visits{};
};

template <typename Index, bool UseCursor>
class WorkloadRunner final {
public:
    WorkloadRunner(const Options& options, Index& index, const ChatHeights& heights)
        : options_(options), index_(index), view_(index, heights, ViewShape{}, options.budget), heights_(heights),
          random_(options.seed * 7919U + 17U) {}

    [[nodiscard]] const stx_btree_research::ViewCounters& counters() const noexcept { return view_.counters(); }

    // The untimed set-up for the workload, then the timed part.
    void prepare() {
        static_cast<void>(index_.assign(options_.rows, ChatHeights::mean_estimate()));
        const std::string& workload = options_.workload;
        if (workload == "W1") {
            return;
        }
        if (workload == "W5") {
            view_.scroll_to(0);
            return;
        }
        if (workload == "W7") {
            view_.to_end();
            return;
        }
        view_.jump_to_row(options_.rows / 2);
    }

    void run() {
        const std::string& workload = options_.workload;
        if (workload == "W1") {
            run_startup();
        } else if (workload == "W2") {
            run_fling();
        } else if (workload == "W3") {
            run_wheel();
        } else if (workload == "W4") {
            run_page_down(3600);
        } else if (workload == "W5") {
            run_traversal();
        } else if (workload == "W6") {
            run_jumps();
        } else if (workload == "W7") {
            run_appends();
        } else if (workload == "W8") {
            run_edits();
        } else if (workload == "W9") {
            run_width_changes();
        }
    }

private:
    void run_startup() {
        static_cast<void>(index_.assign(options_.rows, ChatHeights::mean_estimate()));
        view_.scroll_to(0);
        view_.jump_to_row(options_.rows / 2);
    }

    // 600 frames of a fling: 40 units a frame decaying by 5% a frame, repeated.
    void run_fling() {
        Units velocity = 40 * 64;
        for (int frame = 0; frame < 600; frame += 1) {
            view_.scroll_by(velocity);
            velocity = velocity * 95 / 100;
            if (velocity < 64) {
                velocity = 40 * 64;
            }
        }
    }

    // 600 frames of one wheel notch: three lines of the estimated height.
    void run_wheel() {
        const Units notch = 3 * ChatHeights::mean_estimate();
        for (int frame = 0; frame < 600; frame += 1) {
            view_.scroll_by(notch);
        }
    }

    void run_page_down(int pages) {
        const Units page = ViewShape{}.viewport;
        for (int step = 0; step < pages; step += 1) {
            view_.scroll_by(page);
        }
    }

    // Every row measured: page down from the top to the bottom.
    void run_traversal() {
        const Units page = ViewShape{}.viewport;
        RowCount guard = 0;
        while (view_.offset() < view_.maximum_offset() && guard <= options_.rows) {
            view_.scroll_by(page);
            guard += 1;
        }
    }

    // 1000 jumps to seeded rows, then 100 thumb positions.
    void run_jumps() {
        for (int jump = 0; jump < 1000; jump += 1) {
            view_.jump_to_row(random_.below(options_.rows));
        }
        for (int thumb = 0; thumb < 100; thumb += 1) {
            const Units step = index_.total() / 1000;
            const Units at = step * static_cast<Units>(random_.below(1000));
            view_.scroll_to(at);
        }
    }

    // Follow-tail appends: 30 bursts of 10,000 estimated rows, then 600
    // frames of 10 measured rows each.
    void run_appends() {
        for (int burst = 0; burst < 30; burst += 1) {
            static_cast<void>(index_.insert(index_.count(), 10000));
            view_.to_end();
        }
        Units measured[10]{};
        for (int frame = 0; frame < 600; frame += 1) {
            const RowCount first = index_.count();
            static_cast<void>(index_.insert(first, 10));
            for (RowCount row = 0; row < 10; row += 1) {
                measured[row] = heights_.height(first + row);
            }
            static_cast<void>(index_.set_heights(first, std::span<const Units>(measured, 10)));
            view_.to_end();
        }
    }

    // 1000 seeded re-measures, then 1000 erasures of 1..10,000 rows, with a
    // settle after every ten.
    void run_edits() {
        for (int edit = 0; edit < 1000; edit += 1) {
            const RowCount row = random_.below(index_.count());
            const Units height = 64 * static_cast<Units>(20 + random_.below(400));
            static_cast<void>(index_.set_height(row, height));
            if (edit % 10 == 9) {
                view_.settle();
            }
        }
        for (int edit = 0; edit < 1000; edit += 1) {
            if (index_.count() < 20000) {
                break;
            }
            const RowCount row = random_.below(index_.count());
            const RowCount rows = 1 + random_.below(10000);
            index_.erase(row, rows);
            if (edit % 10 == 9) {
                view_.settle();
            }
        }
    }

    void run_width_changes() {
        for (int change = 0; change < 100; change += 1) {
            view_.reset_width();
        }
    }

    const Options& options_;
    Index& index_;
    ViewEmulator<Index, UseCursor> view_;
    const ChatHeights& heights_;
    SplitMix64 random_;
};

// offset_of(row_at(y)) <= y < offset_of(row_at(y) + 1) for seeded y.
template <typename Index>
std::uint64_t count_bracket_violations(const Index& index, std::uint64_t seed) {
    SplitMix64 random(seed ^ 0x5bd1e995U);
    std::uint64_t violations = 0;
    const Units total = index.total();
    if (total <= 0) {
        return 0;
    }
    for (int sample = 0; sample < 10000; sample += 1) {
        const Units y = static_cast<Units>(random.below(static_cast<std::uint64_t>(total)));
        if (!index.brackets(y)) {
            violations += 1;
        }
    }
    return violations;
}

template <typename Index, bool UseCursor>
int run_variant(const Options& options) {
    const std::size_t allocator_before = HostMemory::allocator_in_use();
    const ChatHeights heights(options.seed);
    Index index(ChatHeights::mean_estimate());
    WorkloadRunner<Index, UseCursor> runner(options, index, heights);
    runner.prepare();
    const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    runner.run();
    const std::chrono::steady_clock::time_point stop = std::chrono::steady_clock::now();
    Result result{};
    result.seconds = std::chrono::duration<double>(stop - start).count();
    const std::size_t allocator_after = HostMemory::allocator_in_use();
    result.allocator_bytes = allocator_after > allocator_before ? allocator_after - allocator_before : 0;
    result.footprint_bytes = HostMemory::footprint();
    if (options.check) {
        result.bracket_violations = count_bracket_violations(index, options.seed);
        index.check(result.invariants_ok, result.invariant_failure);
    }
    index.read_counters(result.descents, result.node_visits);
    const stx_btree_research::ViewCounters& counters = runner.counters();
    std::printf("%s,%s,%llu,%llu,%u,%.6f,%llu,%llu,%llu,%llu,%zu,%zu,%zu,%zu,%zu,%zu,%llu,%llu,%llu,%s,%llu,%llu\n",
                options.variant.c_str(), options.workload.c_str(), static_cast<unsigned long long>(options.rows),
                static_cast<unsigned long long>(options.seed), options.run, result.seconds,
                static_cast<unsigned long long>(counters.settles),
                static_cast<unsigned long long>(counters.rows_measured),
                static_cast<unsigned long long>(counters.rows_realized),
                static_cast<unsigned long long>(counters.coarsenings), index.run_count(), index.node_count(),
                index.depth(), index.page_bytes(), result.allocator_bytes, result.footprint_bytes,
                static_cast<unsigned long long>(counters.checksum),
                static_cast<unsigned long long>(result.bracket_violations),
                static_cast<unsigned long long>(options.budget), result.invariants_ok ? "ok" : result.invariant_failure,
                static_cast<unsigned long long>(result.descents), static_cast<unsigned long long>(result.node_visits));
    const bool failed = options.check && (!result.invariants_ok || (Index::exact && result.bracket_violations != 0));
    return failed ? 1 : 0;
}

bool parse(int argc, char** argv, Options& options) {
    for (int index = 1; index < argc; index += 1) {
        const char* argument = argv[index];
        const bool has_value = index + 1 < argc;
        if (std::strcmp(argument, "--check") == 0) {
            options.check = true;
        } else if (std::strcmp(argument, "--header") == 0) {
            options.header = true;
        } else if (std::strcmp(argument, "--variant") == 0 && has_value) {
            options.variant = argv[++index];
        } else if (std::strcmp(argument, "--workload") == 0 && has_value) {
            options.workload = argv[++index];
        } else if (std::strcmp(argument, "--rows") == 0 && has_value) {
            options.rows = std::strtoull(argv[++index], nullptr, 10);
        } else if (std::strcmp(argument, "--seed") == 0 && has_value) {
            options.seed = std::strtoull(argv[++index], nullptr, 10);
        } else if (std::strcmp(argument, "--budget") == 0 && has_value) {
            options.budget = static_cast<std::size_t>(std::strtoull(argv[++index], nullptr, 10));
        } else if (std::strcmp(argument, "--run") == 0 && has_value) {
            options.run = static_cast<unsigned>(std::strtoul(argv[++index], nullptr, 10));
        } else {
            std::fprintf(stderr, "unknown argument: %s\n", argument);
            return false;
        }
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    Options options{};
    if (!parse(argc, argv, options)) {
        return 2;
    }
    if (options.header) {
        std::printf("variant,workload,rows,seed,run,seconds,settles,rows_measured,rows_realized,coarsenings,"
                    "run_count,node_count,depth,page_bytes,allocator_bytes,footprint_bytes,checksum,"
                    "bracket_violations,budget,invariants,descents,node_visits\n");
        return 0;
    }
    if (options.variant == "P6" && options.budget == 0) {
        options.budget = 100000;
    }
#if defined(ORDER_STATISTIC_HAS_BASELINE)
    if (options.variant == "B0") {
        return run_variant<stx_btree_research::BaselineIndex, false>(options);
    }
#endif
    if (options.variant == "P1") {
        return run_variant<PrototypeIndex<P1Config>, false>(options);
    }
    if (options.variant == "P2") {
        return run_variant<PrototypeIndex<P2Config>, false>(options);
    }
    if (options.variant == "P3") {
        return run_variant<PrototypeIndex<P3Config>, false>(options);
    }
    if (options.variant == "P4") {
        return run_variant<PrototypeIndex<P2Config>, true>(options);
    }
    if (options.variant == "P5" || options.variant == "P6") {
        return run_variant<PrototypeIndex<P5Config>, true>(options);
    }
    std::fprintf(stderr, "unknown variant: %s\n", options.variant.c_str());
    return 2;
}
