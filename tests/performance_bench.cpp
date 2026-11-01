// Times each stx_auxlib operation against the nearest platform equivalent
// doing the same job (a durable write against a durable write, a sorted
// listing against a sorted listing), on POSIX hosts. Each pair runs three
// times; the best time per operation counts. A ratio above 2 is flagged
// (rows marked "(info)" are reported only):
// the owner's ceiling is twice the platform's time (2026-10-31).
//
// Run with standard output redirected (the console cases write to it):
//   stx_auxlib_performance_bench > /dev/null 2> results.txt
// Results go to standard error. Not part of ctest.

#include "stx_btree/unsync_map.hpp"

#include "stx_auxlib/calendar.hpp"
#include "stx_auxlib/children.hpp"
#include "stx_auxlib/clocks.hpp"
#include "stx_auxlib/console.hpp"
#include "stx_auxlib/environment.hpp"
#include "stx_auxlib/files.hpp"
#include "stx_auxlib/hash.hpp"
#include "stx_auxlib/memory.hpp"
#include "stx_auxlib/paths.hpp"
#include "stx_auxlib/random.hpp"
#include "stx_auxlib/threads.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <filesystem>
#include <mutex>
#include <random>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include <CommonCrypto/CommonDigest.h>
#include <fcntl.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace {

// Shared fixtures, set up once in main.
struct Fixture final {
    stx_auxlib::Path directory{};
    std::string directory_text{};
    stx_auxlib::Path small_file{};
    std::string small_file_text{};
    stx_auxlib::Path write_target{};
    std::string write_target_text{};
    stx_auxlib::Path listing{};
    std::string listing_text{};
    std::vector<std::byte> payload{};
};

Fixture fixture{};

using Operation = std::uint64_t (*)(std::size_t count);

struct Case final {
    const char* name;
    Operation ours;
    Operation platform;
    std::size_t count;
};

// --- memory ---------------------------------------------------------------

std::uint64_t ours_allocate(const std::size_t count) {
    std::uint64_t sink = 0;
    for (std::size_t index = 0; index < count; ++index) {
        void* const block = stx_auxlib::memory::allocate(64, 16);
        sink += reinterpret_cast<std::uintptr_t>(block) & 0xffU;
        stx_auxlib::memory::release(block);
    }
    return sink;
}

std::uint64_t platform_allocate(const std::size_t count) {
    std::uint64_t sink = 0;
    for (std::size_t index = 0; index < count; ++index) {
        void* const block = std::malloc(64);
        sink += reinterpret_cast<std::uintptr_t>(block) & 0xffU;
        std::free(block);
    }
    return sink;
}

// --- clocks ---------------------------------------------------------------

std::uint64_t ours_monotonic(const std::size_t count) {
    std::uint64_t sink = 0;
    for (std::size_t index = 0; index < count; ++index)
        sink += static_cast<std::uint64_t>(stx_auxlib::MonotonicClock::now().time_since_epoch().count());
    return sink;
}

std::uint64_t platform_monotonic(const std::size_t count) {
    std::uint64_t sink = 0;
    for (std::size_t index = 0; index < count; ++index)
        sink += static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
    return sink;
}

std::uint64_t ours_wall(const std::size_t count) {
    std::uint64_t sink = 0;
    for (std::size_t index = 0; index < count; ++index)
        sink += static_cast<std::uint64_t>(stx_auxlib::WallClock::now().time_since_epoch().count());
    return sink;
}

std::uint64_t platform_wall(const std::size_t count) {
    std::uint64_t sink = 0;
    for (std::size_t index = 0; index < count; ++index)
        sink += static_cast<std::uint64_t>(std::chrono::system_clock::now().time_since_epoch().count());
    return sink;
}

// --- threads --------------------------------------------------------------

stx_auxlib::Mutex our_mutex{};
std::mutex platform_mutex{};
std::uint64_t guarded = 0;

std::uint64_t ours_lock(const std::size_t count) {
    for (std::size_t index = 0; index < count; ++index) {
        const stx_auxlib::ScopedLock lock(our_mutex);
        ++guarded;
    }
    return guarded;
}

std::uint64_t platform_lock(const std::size_t count) {
    for (std::size_t index = 0; index < count; ++index) {
        const std::lock_guard<std::mutex> lock(platform_mutex);
        ++guarded;
    }
    return guarded;
}

stx_auxlib::OnceFlag our_once{};
std::once_flag platform_once{};
std::uint64_t once_runs = 0;

void count_once(void*) noexcept { ++once_runs; }

void count_platform_once() { ++once_runs; }

std::uint64_t ours_once(const std::size_t count) {
    for (std::size_t index = 0; index < count; ++index) stx_auxlib::call_once(our_once, count_once, nullptr);
    return once_runs;
}

std::uint64_t platform_once_call(const std::size_t count) {
    for (std::size_t index = 0; index < count; ++index) std::call_once(platform_once, count_platform_once);
    return once_runs;
}

std::uint64_t ours_identity(const std::size_t count) {
    std::uint64_t sink = 0;
    for (std::size_t index = 0; index < count; ++index) sink += stx_auxlib::current_thread().value();
    return sink;
}

std::uint64_t platform_identity(const std::size_t count) {
    std::uint64_t sink = 0;
    for (std::size_t index = 0; index < count; ++index)
        sink += std::hash<std::thread::id>{}(std::this_thread::get_id()) & 1U;
    return sink;
}

void do_nothing(const stx_auxlib::StopSignal&, void*) noexcept {}

void do_nothing_platform() {}

std::uint64_t ours_thread(const std::size_t count) {
    for (std::size_t index = 0; index < count; ++index) {
        stx_auxlib::ThreadStart start = stx_auxlib::Thread::start(do_nothing, nullptr);
        stx_auxlib::Thread thread = start.take_thread();
        (void)thread.join();
    }
    return count;
}

std::uint64_t platform_thread(const std::size_t count) {
    for (std::size_t index = 0; index < count; ++index) {
        std::thread thread(do_nothing_platform);
        thread.join();
    }
    return count;
}

// --- paths ----------------------------------------------------------------

constexpr const char* sample_path = "projects/gui_forms/./src/../include/stx_auxlib//paths.hpp";

std::uint64_t ours_path(const std::size_t count) {
    std::uint64_t sink = 0;
    for (std::size_t index = 0; index < count; ++index)
        sink += (*stx_auxlib::Path::parse(sample_path).path()).text().size();
    return sink;
}

std::uint64_t platform_path(const std::size_t count) {
    std::uint64_t sink = 0;
    for (std::size_t index = 0; index < count; ++index)
        sink += std::filesystem::path(sample_path).lexically_normal().native().size();
    return sink;
}

// --- files ----------------------------------------------------------------

std::uint64_t ours_read_file(const std::size_t count) {
    std::vector<std::byte> content{};
    std::uint64_t sink = 0;
    for (std::size_t index = 0; index < count; ++index) {
        (void)stx_auxlib::read_file(fixture.small_file, 1 << 20, content);
        sink += content.size();
    }
    return sink;
}

std::uint64_t platform_read_file(const std::size_t count) {
    std::vector<std::byte> content{};
    std::uint64_t sink = 0;
    for (std::size_t index = 0; index < count; ++index) {
        std::FILE* const file = std::fopen(fixture.small_file_text.c_str(), "rb");
        std::fseek(file, 0, SEEK_END);
        const long size = std::ftell(file);
        std::fseek(file, 0, SEEK_SET);
        content.resize(static_cast<std::size_t>(size));
        sink += std::fread(content.data(), 1, content.size(), file);
        std::fclose(file);
    }
    return sink;
}

// The platform form of write_file's guarantee: a temporary file, a full
// flush, an atomic rename and a directory flush.
int full_flush(const int descriptor) {
#if defined(F_FULLFSYNC)
    if (::fcntl(descriptor, F_FULLFSYNC) == 0) return 0;
#endif
    return ::fsync(descriptor);
}

std::uint64_t ours_write_file(const std::size_t count) {
    for (std::size_t index = 0; index < count; ++index)
        (void)stx_auxlib::write_file(fixture.write_target, fixture.payload);
    return count;
}

std::uint64_t platform_write_file(const std::size_t count) {
    const std::string temporary = fixture.write_target_text + ".platform-tmp";
    for (std::size_t index = 0; index < count; ++index) {
        const int descriptor = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
        (void)::write(descriptor, fixture.payload.data(), fixture.payload.size());
        (void)full_flush(descriptor);
        (void)::close(descriptor);
        (void)::rename(temporary.c_str(), fixture.write_target_text.c_str());
        const int directory = ::open(fixture.directory_text.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        (void)full_flush(directory);
        (void)::close(directory);
    }
    return count;
}

std::uint64_t ours_list(const std::size_t count) {
    stx_auxlib::DirectoryListing listing{};
    std::uint64_t sink = 0;
    for (std::size_t index = 0; index < count; ++index) {
        (void)stx_auxlib::list_directory(fixture.listing, 1024, listing);
        sink += listing.entries.size();
    }
    return sink;
}

struct PlatformEntry final {
    std::string name{};
    std::filesystem::file_type type{};
};

bool platform_entry_less(const PlatformEntry& left, const PlatformEntry& right) {
    const bool result = left.name < right.name;
    return result;
}

std::uint64_t platform_list(const std::size_t count) {
    std::vector<PlatformEntry> entries{};
    std::uint64_t sink = 0;
    for (std::size_t index = 0; index < count; ++index) {
        entries.clear();
        for (const std::filesystem::directory_entry& entry :
             std::filesystem::directory_iterator(fixture.listing_text)) {
            PlatformEntry listed{};
            listed.name = entry.path().filename().string();
            listed.type = entry.symlink_status().type();
            entries.push_back(listed);
        }
        std::sort(entries.begin(), entries.end(), platform_entry_less);
        sink += entries.size();
    }
    return sink;
}

std::uint64_t ours_inspect(const std::size_t count) {
    std::uint64_t sink = 0;
    for (std::size_t index = 0; index < count; ++index) {
        const stx_auxlib::FileValue<stx_auxlib::FileInformation> information =
            stx_auxlib::inspect(fixture.small_file, stx_auxlib::LinkMode::follow);
        sink += (*information.value()).size;
    }
    return sink;
}

std::uint64_t platform_inspect(const std::size_t count) {
    std::uint64_t sink = 0;
    for (std::size_t index = 0; index < count; ++index) {
        struct stat information{};
        (void)::stat(fixture.small_file_text.c_str(), &information);
        sink += static_cast<std::uint64_t>(information.st_size);
    }
    return sink;
}

// --- environment ----------------------------------------------------------

std::uint64_t ours_getenv(const std::size_t count) {
    std::string value{};
    std::uint64_t sink = 0;
    for (std::size_t index = 0; index < count; ++index) {
        (void)stx_auxlib::read_variable("HOME", 4096, value);
        sink += value.size();
    }
    return sink;
}

std::uint64_t platform_getenv(const std::size_t count) {
    std::string value{};
    std::uint64_t sink = 0;
    for (std::size_t index = 0; index < count; ++index) {
        const char* const found = std::getenv("HOME");
        value.assign(found == nullptr ? "" : found);
        sink += value.size();
    }
    return sink;
}

std::uint64_t ours_setenv(const std::size_t count) {
    for (std::size_t index = 0; index < count; ++index)
        (void)stx_auxlib::set_variable("STX_AUXLIB_BENCH", (index & 1U) != 0 ? "one" : "two");
    return count;
}

std::uint64_t platform_setenv(const std::size_t count) {
    for (std::size_t index = 0; index < count; ++index)
        (void)::setenv("STX_AUXLIB_BENCH", (index & 1U) != 0 ? "one" : "two", 1);
    return count;
}

// --- console --------------------------------------------------------------

std::uint64_t ours_console(const std::size_t count) {
    for (std::size_t index = 0; index < count; ++index)
        (void)stx_auxlib::write_console_line(stx_auxlib::ConsoleStream::output, "a line of console output");
    return count;
}

std::uint64_t platform_console(const std::size_t count) {
    for (std::size_t index = 0; index < count; ++index) {
        std::fputs("a line of console output\n", stdout);
        std::fflush(stdout);
    }
    return count;
}

// --- random ---------------------------------------------------------------

std::uint64_t ours_chacha(const std::size_t count) {
    stx_auxlib::random::ChaCha12Stream generator(stx_auxlib::random::seed_from_number(1));
    std::uint64_t sink = 0;
    for (std::size_t index = 0; index < count; ++index) sink ^= generator.next();
    return sink;
}

std::uint64_t ours_xoshiro(const std::size_t count) {
    stx_auxlib::random::Xoshiro256PlusPlus generator(stx_auxlib::random::seed_from_number(1));
    std::uint64_t sink = 0;
    for (std::size_t index = 0; index < count; ++index) sink ^= generator.next();
    return sink;
}

std::uint64_t platform_mt19937(const std::size_t count) {
    std::mt19937_64 generator(1);
    std::uint64_t sink = 0;
    for (std::size_t index = 0; index < count; ++index) sink ^= generator();
    return sink;
}

// The platform's cryptographic generator, ChaCha12's like-for-like
// equivalent (macOS's arc4random is itself ChaCha-based).
std::uint64_t platform_arc4random(const std::size_t count) {
    std::uint64_t sink = 0;
    for (std::size_t index = 0; index < count; ++index) {
        std::uint64_t value = 0;
        ::arc4random_buf(&value, sizeof value);
        sink ^= value;
    }
    return sink;
}

std::uint64_t ours_dice(const std::size_t count) {
    stx_auxlib::random::ChaCha12Stream generator(stx_auxlib::random::seed_from_number(1));
    std::uint64_t sink = 0;
    for (std::size_t index = 0; index < count; ++index) sink += stx_auxlib::random::uniform_below(generator, 6);
    return sink;
}

std::uint64_t platform_dice(const std::size_t count) {
    std::mt19937_64 generator(1);
    std::uniform_int_distribution<std::uint64_t> die(0, 5);
    std::uint64_t sink = 0;
    for (std::size_t index = 0; index < count; ++index) sink += die(generator);
    return sink;
}

std::uint64_t ours_shuffle(const std::size_t count) {
    stx_auxlib::random::ChaCha12Stream generator(stx_auxlib::random::seed_from_number(1));
    std::array<int, 52> deck{};
    for (std::size_t index = 0; index < deck.size(); ++index) deck[index] = static_cast<int>(index);
    for (std::size_t round = 0; round < count; ++round)
        stx_auxlib::random::shuffle(generator, std::span<int>(deck));
    return static_cast<std::uint64_t>(deck[0]);
}

std::uint64_t platform_shuffle(const std::size_t count) {
    std::mt19937_64 generator(1);
    std::array<int, 52> deck{};
    for (std::size_t index = 0; index < deck.size(); ++index) deck[index] = static_cast<int>(index);
    for (std::size_t round = 0; round < count; ++round) std::shuffle(deck.begin(), deck.end(), generator);
    return static_cast<std::uint64_t>(deck[0]);
}

std::uint64_t ours_entropy(const std::size_t count) {
    std::array<std::byte, 32> bytes{};
    std::uint64_t sink = 0;
    for (std::size_t index = 0; index < count; ++index) {
        (void)stx_auxlib::random::fill_entropy(bytes);
        sink += std::to_integer<std::uint64_t>(bytes[0]);
    }
    return sink;
}

std::uint64_t platform_entropy(const std::size_t count) {
    std::random_device device{};
    std::uint64_t sink = 0;
    for (std::size_t index = 0; index < count; ++index) {
        for (int word = 0; word < 8; ++word) sink += device() & 1U;
    }
    return sink;
}

// --- calendar -------------------------------------------------------------

std::uint64_t ours_civil(const std::size_t count) {
    std::uint64_t sink = 0;
    for (std::size_t index = 0; index < count; ++index) {
        const stx_auxlib::WallClock::time_point instant{
            stx_auxlib::WallClock::duration(static_cast<std::int64_t>(index) * 997000000123LL)};
        sink += stx_auxlib::civil_from_wall(instant).day;
    }
    return sink;
}

std::uint64_t platform_civil(const std::size_t count) {
    std::uint64_t sink = 0;
    for (std::size_t index = 0; index < count; ++index) {
        const std::time_t seconds = static_cast<std::time_t>(static_cast<std::int64_t>(index) * 997LL);
        std::tm broken{};
        (void)::gmtime_r(&seconds, &broken);
        sink += static_cast<std::uint64_t>(broken.tm_mday);
    }
    return sink;
}

std::uint64_t ours_local(const std::size_t count) {
    std::uint64_t sink = 0;
    for (std::size_t index = 0; index < count; ++index) {
        const stx_auxlib::WallClock::time_point instant{
            stx_auxlib::WallClock::duration(static_cast<std::int64_t>(index) * 997000000123LL)};
        sink += (*stx_auxlib::local_time(instant).value()).civil.day;
    }
    return sink;
}

std::uint64_t platform_local(const std::size_t count) {
    std::uint64_t sink = 0;
    for (std::size_t index = 0; index < count; ++index) {
        const std::time_t seconds = static_cast<std::time_t>(static_cast<std::int64_t>(index) * 997LL);
        std::tm broken{};
        (void)::localtime_r(&seconds, &broken);
        sink += static_cast<std::uint64_t>(broken.tm_mday);
    }
    return sink;
}

std::uint64_t ours_iso(const std::size_t count) {
    std::string text{};
    const stx_auxlib::CivilTime time = stx_auxlib::civil_from_wall(stx_auxlib::WallClock::now());
    std::uint64_t sink = 0;
    for (std::size_t index = 0; index < count; ++index) {
        text.clear();
        stx_auxlib::append_iso8601(text, time, 0, {.separator = ' ', .zone = stx_auxlib::ZoneDesignator::none});
        sink += text.size();
    }
    return sink;
}

std::uint64_t platform_iso(const std::size_t count) {
    const std::time_t now = std::time(nullptr);
    std::tm broken{};
    (void)::gmtime_r(&now, &broken);
    std::array<char, 64> text{};
    std::uint64_t sink = 0;
    for (std::size_t index = 0; index < count; ++index)
        sink += std::strftime(text.data(), text.size(), "%Y-%m-%d %H:%M:%S", &broken);
    return sink;
}

// --- children -------------------------------------------------------------

std::uint64_t ours_child(const std::size_t count) {
    stx_auxlib::ChildRequest request{};
    request.program = "/usr/bin/true";
    request.streams = stx_auxlib::ChildStreams::discard;
    std::uint64_t sink = 0;
    for (std::size_t index = 0; index < count; ++index) {
        stx_auxlib::CapabilityValue<stx_auxlib::Child> started = stx_auxlib::Child::start(request);
        sink += (*started.value()).wait(std::chrono::seconds(30)).succeeded() ? 1 : 0;
    }
    return sink;
}

std::uint64_t platform_child(const std::size_t count) {
    std::uint64_t sink = 0;
    std::string program = "/usr/bin/true";
    char* arguments[] = {program.data(), nullptr};
    for (std::size_t index = 0; index < count; ++index) {
        posix_spawn_file_actions_t actions{};
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
        posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
        posix_spawn_file_actions_adddup2(&actions, STDOUT_FILENO, STDERR_FILENO);
        pid_t child = 0;
        if (posix_spawn(&child, program.c_str(), &actions, nullptr, arguments, environ) == 0) {
            int status = 0;
            (void)::waitpid(child, &status, 0);
            sink += 1;
        }
        posix_spawn_file_actions_destroy(&actions);
    }
    return sink;
}

// --- hashing and ordered maps --------------------------------------------

std::array<std::byte, 64> hash_input{};

std::uint64_t ours_blake2s(const std::size_t count) {
    std::uint64_t sink = 0;
    for (std::size_t index = 0; index < count; ++index) {
        hash_input[0] = static_cast<std::byte>(index);
        sink += std::to_integer<std::uint64_t>(stx_auxlib::hash::blake2s(hash_input).bytes[0]);
    }
    return sink;
}

// Apple's SHA-256, hardware-accelerated: a different algorithm, reported
// for scale only.
std::uint64_t platform_sha256(const std::size_t count) {
    std::uint64_t sink = 0;
    std::array<unsigned char, CC_SHA256_DIGEST_LENGTH> digest{};
    for (std::size_t index = 0; index < count; ++index) {
        hash_input[0] = static_cast<std::byte>(index);
        (void)::CC_SHA256(hash_input.data(), static_cast<CC_LONG>(hash_input.size()), digest.data());
        sink += digest[0];
    }
    return sink;
}

std::uint64_t mixed_key(const std::size_t index) {
    // A fixed permutation of 0 .. 2^20 - 1 (odd multiplier modulo 2^20).
    const std::uint64_t result = (static_cast<std::uint64_t>(index) * 0x9e3779b1ULL) & 0xfffffULL;
    return result;
}

std::uint64_t ours_btree(const std::size_t count) {
    stx_btree::unsync::BBTreeMap<std::uint64_t, std::uint64_t> map{};
    for (std::size_t index = 0; index < count; ++index) map.insert(mixed_key(index), index);
    std::uint64_t sink = 0;
    for (std::size_t index = 0; index < count; ++index) {
        const std::uint64_t* const found = map.find(mixed_key(index));
        if (found != nullptr) sink += *found;
    }
    return sink;
}

std::uint64_t platform_map(const std::size_t count) {
    std::map<std::uint64_t, std::uint64_t> map{};
    for (std::size_t index = 0; index < count; ++index) map.emplace(mixed_key(index), index);
    std::uint64_t sink = 0;
    for (std::size_t index = 0; index < count; ++index) {
        const std::map<std::uint64_t, std::uint64_t>::const_iterator found = map.find(mixed_key(index));
        if (found != map.end()) sink += (*found).second;
    }
    return sink;
}

const Case cases[] = {
    {"memory: allocate+release 64 B", ours_allocate, platform_allocate, 2000000},
    {"clocks: MonotonicClock::now", ours_monotonic, platform_monotonic, 2000000},
    {"clocks: WallClock::now", ours_wall, platform_wall, 2000000},
    {"threads: ScopedLock (uncontended)", ours_lock, platform_lock, 2000000},
    {"threads: call_once (done)", ours_once, platform_once_call, 2000000},
    {"threads: current_thread", ours_identity, platform_identity, 2000000},
    {"threads: start+join", ours_thread, platform_thread, 2000},
    {"paths: parse+normalize", ours_path, platform_path, 200000},
    {"files: read_file 4 KiB", ours_read_file, platform_read_file, 5000},
    {"files: write_file durable 4 KiB", ours_write_file, platform_write_file, 100},
    {"files: list_directory 64, sorted", ours_list, platform_list, 2000},
    {"files: inspect", ours_inspect, platform_inspect, 20000},
    {"environment: read HOME", ours_getenv, platform_getenv, 500000},
    {"environment: set", ours_setenv, platform_setenv, 200000},
    {"console: line to /dev/null", ours_console, platform_console, 100000},
    {"random: ChaCha12 vs arc4random (secure)", ours_chacha, platform_arc4random, 10000000},
    {"random: ChaCha12 vs mt19937_64 (info)", ours_chacha, platform_mt19937, 10000000},
    {"random: xoshiro256++ vs mt19937_64", ours_xoshiro, platform_mt19937, 10000000},
    {"random: die roll", ours_dice, platform_dice, 5000000},
    {"random: shuffle 52", ours_shuffle, platform_shuffle, 200000},
    {"random: 32 bytes of entropy", ours_entropy, platform_entropy, 20000},
    {"calendar: civil_from_wall vs gmtime_r", ours_civil, platform_civil, 2000000},
    {"calendar: local_time vs localtime_r", ours_local, platform_local, 200000},
    {"calendar: ISO text vs strftime", ours_iso, platform_iso, 1000000},
    {"children: start+wait /usr/bin/true", ours_child, platform_child, 200},
    {"hash: BLAKE2s vs SHA-256 64 B (info)", ours_blake2s, platform_sha256, 1000000},
    {"btree: insert+find vs std::map", ours_btree, platform_map, 1000000},
};

double best_nanoseconds(const Operation operation, const std::size_t count, std::uint64_t& sink) {
    double best = 1e300;
    for (int trial = 0; trial < 3; ++trial) {
        const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
        sink += operation(count);
        const std::chrono::duration<double, std::nano> elapsed = std::chrono::steady_clock::now() - start;
        const double per = elapsed.count() / static_cast<double>(count);
        if (per < best) best = per;
    }
    return best;
}

void prepare_fixture() {
    const stx_auxlib::FileValue<stx_auxlib::Path> temporary = stx_auxlib::temporary_directory();
    const std::string name =
        "stx-auxlib-bench-" + std::to_string(stx_auxlib::MonotonicClock::now().time_since_epoch().count());
    fixture.directory = *(*temporary.value()).join(*stx_auxlib::Path::parse(name).path()).path();
    fixture.directory_text = std::string(fixture.directory.text());
    (void)stx_auxlib::create_directory(fixture.directory);
    fixture.payload.assign(4096, std::byte{0x5a});
    fixture.small_file = *fixture.directory.join(*stx_auxlib::Path::parse("small.bin").path()).path();
    fixture.small_file_text = std::string(fixture.small_file.text());
    (void)stx_auxlib::write_file(fixture.small_file, fixture.payload);
    fixture.write_target = *fixture.directory.join(*stx_auxlib::Path::parse("target.bin").path()).path();
    fixture.write_target_text = std::string(fixture.write_target.text());
    fixture.listing = *fixture.directory.join(*stx_auxlib::Path::parse("listing").path()).path();
    fixture.listing_text = std::string(fixture.listing.text());
    (void)stx_auxlib::create_directory(fixture.listing);
    for (int index = 0; index < 64; ++index) {
        const std::string entry = "entry-" + std::to_string((index * 37) % 64);
        (void)stx_auxlib::write_file(*fixture.listing.join(*stx_auxlib::Path::parse(entry).path()).path(),
                                     fixture.payload);
    }
}

} // namespace

int main() {
    prepare_fixture();
    std::uint64_t sink = 0;
    int over = 0;
    std::fprintf(stderr, "%-40s %12s %12s %7s\n", "operation", "ours ns", "platform ns", "ratio");
    for (const Case& entry : cases) {
        const double ours = best_nanoseconds(entry.ours, entry.count, sink);
        const double platform = best_nanoseconds(entry.platform, entry.count, sink);
        const double ratio = ours / platform;
        // "(info)" rows compare against something other than the
        // like-for-like equivalent and are reported, not gated.
        const bool gated = std::strstr(entry.name, "(info)") == nullptr;
        const bool flagged = gated && ratio > 2.0;
        if (flagged) ++over;
        std::fprintf(stderr, "%-40s %12.1f %12.1f %7.2f%s\n", entry.name, ours, platform, ratio,
                     flagged ? "  OVER 2x" : "");
    }
    (void)stx_auxlib::remove_tree(fixture.directory);
    std::fprintf(stderr, "%d over the 2x ceiling (sink %llu)\n", over, static_cast<unsigned long long>(sink));
    return over == 0 ? 0 : 1;
}
