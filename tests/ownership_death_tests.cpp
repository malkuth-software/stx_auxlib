// Must-trap tests of stx_auxlib::memory's ownership boundary
// (docs/ALLOCATOR_OWNERSHIP.md, 2.6). The test starts itself once per case
// and checks how each child ended:
//
// - round_trip: blocks from allocate, released by release, end normally.
// - foreign_release: release of a malloc block fails with our message.
// - double_release: a second release of one block fails with our message.
// - foreign_free: free of a block from allocate ends the child abnormally.
// - foreign_delete: operator delete of a block from allocate ends the child
//   abnormally.
// - static_runtime (MinGW only): no shared C++ runtime DLL is loaded.
//
// Every case but round_trip needs the check mode (memory::tags_checked());
// without it they are reported as skipped. A case that survives what it
// should not prints "survived" and exits 0, which the parent rejects.

#include "stx_auxlib/capabilities.hpp"
#include "stx_auxlib/children.hpp"
#include "stx_auxlib/desktop.hpp"
#include "stx_auxlib/memory.hpp"
#include "stx_auxlib/paths.hpp"
#include "stx_auxlib/process.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <string_view>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace {

void require(const bool condition, const char* const message) {
    if (condition) return;
    std::fprintf(stderr, "stx_auxlib ownership: %s\n", message);
    std::abort();
}

constexpr std::string_view rejection_message = "stx_auxlib::memory::release: the block is not from memory::allocate";

// ---- the cases, each run in its own child --------------------------------

int round_trip() {
    for (std::size_t alignment = 1; alignment <= 4096; alignment *= 2) {
        for (std::size_t size = 0; size <= 300; size += 37) {
            void* const block = stx_auxlib::memory::allocate(size, alignment);
            require(block != nullptr, "allocation failed");
            std::memset(block, 0x5a, size);
            stx_auxlib::memory::release(block);
        }
    }
    std::puts("round trips done");
    return 0;
}

int foreign_release() {
    void* const block = std::malloc(64);
    require(block != nullptr, "malloc failed");
    stx_auxlib::memory::release(block);
    std::puts("survived");
    return 0;
}

int double_release() {
    void* const block = stx_auxlib::memory::allocate(64, 16);
    require(block != nullptr, "allocation failed");
    stx_auxlib::memory::release(block);
    stx_auxlib::memory::release(block);
    std::puts("survived");
    return 0;
}

int foreign_free() {
    void* const block = stx_auxlib::memory::allocate(64, 16);
    require(block != nullptr, "allocation failed");
    std::free(block);
    std::puts("survived");
    return 0;
}

int foreign_delete() {
    void* const block = stx_auxlib::memory::allocate(64, 16);
    require(block != nullptr, "allocation failed");
    ::operator delete(block);
    std::puts("survived");
    return 0;
}

#if defined(__MINGW32__)
// Static libc++ is required in every module we build (CMakeLists.txt); a
// shared libc++ binds its own operator new and delete and frees across the
// module boundary. The check reads this executable's own import table: a
// toolchain DLL such as the AddressSanitizer runtime may load libc++.dll for
// itself, which is that DLL's business, not this module's.
bool imports_dll(const char* const name) {
    const HMODULE module = GetModuleHandleW(nullptr);
    const unsigned char* const base = reinterpret_cast<const unsigned char*>(module);
    const IMAGE_DOS_HEADER* const dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const IMAGE_NT_HEADERS* const nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + (*dos).e_lfanew);
    const IMAGE_DATA_DIRECTORY& directory = (*nt).OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (directory.VirtualAddress == 0) return false;
    const IMAGE_IMPORT_DESCRIPTOR* descriptor =
        reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + directory.VirtualAddress);
    for (; (*descriptor).Name != 0; ++descriptor) {
        const char* const imported = reinterpret_cast<const char*>(base + (*descriptor).Name);
        if (lstrcmpiA(imported, name) == 0) return true;
    }
    return false;
}

int static_runtime() {
    const bool shared_libcxx = imports_dll("libc++.dll");
    const bool shared_unwind = imports_dll("libunwind.dll");
    if (shared_libcxx || shared_unwind) {
        std::puts("this executable imports a shared C++ runtime");
        return 1;
    }
    std::puts("static runtime");
    return 0;
}
#endif

// A child that crashes on purpose must not stop for Windows Error Reporting
// or the C library's abort dialog.
void quiet_crashes() {
#if defined(_WIN32)
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
}

int run_case(const std::string_view name) {
    quiet_crashes();
    if (name == "round_trip") return round_trip();
    if (name == "foreign_release") return foreign_release();
    if (name == "double_release") return double_release();
    if (name == "foreign_free") return foreign_free();
    if (name == "foreign_delete") return foreign_delete();
#if defined(__MINGW32__)
    if (name == "static_runtime") return static_runtime();
#endif
    std::fprintf(stderr, "unknown case\n");
    return 2;
}

// ---- the parent ----------------------------------------------------------

enum class Expected : std::uint8_t {
    // Exit status 0 and the given text in the output.
    normal_end = 0,
    // A non-zero exit status or a signal, and our rejection message.
    our_rejection = 1,
    // A non-zero exit status or a signal, without "survived".
    abnormal_end = 2,
};

struct DeathCase final {
    const char* name{nullptr};
    Expected expected{Expected::normal_end};
    const char* normal_text{nullptr};  // for normal_end only
    bool needs_tags{false};
};

struct ChildResult final {
    bool ended_normally{false};  // exit status 0
    std::string output{};
};

std::string self_program() {
    const stx_auxlib::CapabilityValue<stx_auxlib::Path> self = stx_auxlib::current_executable();
    require(self.succeeded(), "current_executable failed");
    std::string result((*self.value()).text());
    return result;
}

ChildResult run_child(const std::string& program, const char* const name) {
    stx_auxlib::ChildRequest request{};
    request.program = program;
    request.arguments.push_back(name);
    request.streams = stx_auxlib::ChildStreams::capture;
    stx_auxlib::CapabilityValue<stx_auxlib::Child> started = stx_auxlib::Child::start(request);
    require(started.succeeded(), "a child failed to start");
    stx_auxlib::Child& child = *started.value();
    ChildResult result{};
    const stx_auxlib::CapabilityStatus read = child.read_output(1u << 20, result.output);
    require(read == stx_auxlib::CapabilityStatus::ok, "reading a child's output failed");
    const stx_auxlib::CapabilityValue<stx_auxlib::ChildEnd> end = child.wait(std::chrono::seconds(120));
    require(end.succeeded(), "a child did not end");
    const std::uint32_t* const status = (*end.value()).exit_status();
    result.ended_normally = status != nullptr && *status == 0;
    return result;
}

bool contains(const std::string& text, const std::string_view part) {
    const bool result = text.find(part) != std::string::npos;
    return result;
}

bool case_passed(const DeathCase& death_case, const ChildResult& result) {
    switch (death_case.expected) {
    case Expected::normal_end:
        return result.ended_normally && contains(result.output, death_case.normal_text);
    case Expected::our_rejection:
        return !result.ended_normally && contains(result.output, rejection_message) &&
               !contains(result.output, "survived");
    case Expected::abnormal_end:
        return !result.ended_normally && !contains(result.output, "survived");
    }
    return false;
}

int run_parent() {
    const DeathCase cases[] = {
        {"round_trip", Expected::normal_end, "round trips done", false},
        {"foreign_release", Expected::our_rejection, nullptr, true},
        {"double_release", Expected::our_rejection, nullptr, true},
        {"foreign_free", Expected::abnormal_end, nullptr, true},
        {"foreign_delete", Expected::abnormal_end, nullptr, true},
#if defined(__MINGW32__)
        {"static_runtime", Expected::normal_end, "static runtime", false},
#endif
    };
    const std::string program = self_program();
    const bool tags = stx_auxlib::memory::tags_checked();
    int failures = 0;
    for (const DeathCase& death_case : cases) {
        if (death_case.needs_tags && !tags) {
            std::printf("%-16s skipped (no allocation tags in this build)\n", death_case.name);
            continue;
        }
        const ChildResult result = run_child(program, death_case.name);
        const bool passed = case_passed(death_case, result);
        std::printf("%-16s %s\n", death_case.name, passed ? "ok" : "FAILED");
        if (!passed) {
            std::printf("--- child output ---\n%s\n--- end ---\n", result.output.c_str());
            ++failures;
        }
    }
    if (failures != 0) return 1;
    std::puts("stx_auxlib ownership death tests passed");
    return 0;
}

} // namespace

int main(const int argc, char** const argv) {
    if (argc > 1) return run_case(argv[1]);
    return run_parent();
}
