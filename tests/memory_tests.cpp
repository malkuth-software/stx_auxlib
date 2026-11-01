// Tests of stx_auxlib::memory and, with the mimalloc backend, of the
// replacement operator new and delete (stx_auxlib::global_allocator).
// CMakeLists.txt defines STX_AUXLIB_EXPECT_MIMALLOC or STX_AUXLIB_EXPECT_HOST
// for the backend this build selected.

#include "stx_auxlib/memory.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <string>
#include <thread>
#include <vector>

namespace {

void require(const bool condition, const char* const message) {
    if (condition) return;
    std::fprintf(stderr, "stx_auxlib memory: %s\n", message);
    std::abort();
}

bool aligned_to(const void* const address, const std::size_t alignment) {
    const std::uintptr_t value = reinterpret_cast<std::uintptr_t>(address);
    const bool result = (value & (alignment - 1)) == 0;
    return result;
}

#if defined(STX_AUXLIB_EXPECT_MIMALLOC)
constexpr bool expect_mimalloc = true;
#else
constexpr bool expect_mimalloc = false;
#endif

void backend_identity() {
    const stx_auxlib::memory::Backend expected = expect_mimalloc
                                                     ? stx_auxlib::memory::Backend::mimalloc
                                                     : stx_auxlib::memory::Backend::host;
    require(stx_auxlib::memory::backend() == expected, "linked backend is not the selected one");
}

void unused_handler() {}

// The program's std::get_new_handler reads back what std::set_new_handler
// stored. A C build of mimalloc broke this by binding its own null-returning
// get_new_handler in place of the C++ library's.
void new_handler_storage() {
    const std::new_handler previous = std::set_new_handler(unused_handler);
    require(std::get_new_handler() == unused_handler, "new-handler did not read back");
    std::set_new_handler(previous);
    require(std::get_new_handler() == previous, "new-handler was not restored");
}

void allocate_and_release() {
    const std::size_t sizes[] = {0, 1, 7, 100, 4096, 1u << 20};
    std::vector<void*> blocks{};
    for (std::size_t alignment = 1; alignment <= 65536; alignment *= 2) {
        for (const std::size_t size : sizes) {
            void* const block = stx_auxlib::memory::allocate(size, alignment);
            require(block != nullptr, "allocation failed");
            require(aligned_to(block, alignment), "allocation is misaligned");
            std::memset(block, 0xa5, size);
            require(stx_auxlib::memory::in_backend_heap(block) == expect_mimalloc,
                    "heap ownership disagrees with the backend");
            for (const void* const other : blocks) require(other != block, "blocks overlap");
            blocks.push_back(block);
        }
    }
    for (void* const block : blocks) stx_auxlib::memory::release(block);
    stx_auxlib::memory::release(nullptr);
}

void rejected_requests() {
    const std::size_t bad_alignments[] = {0, 3, 6, 24, 1000};
    for (const std::size_t alignment : bad_alignments)
        require(stx_auxlib::memory::allocate(16, alignment) == nullptr,
                "a non-power-of-two alignment was accepted");
    const std::size_t largest = std::numeric_limits<std::size_t>::max();
    require(stx_auxlib::memory::allocate(largest, 8) == nullptr,
            "an unrepresentable size was accepted");
    require(stx_auxlib::memory::allocate(largest - 64, 4096) == nullptr,
            "a size overflowing its alignment padding was accepted");
    int local = 0;
    require(!stx_auxlib::memory::in_backend_heap(&local), "a stack address is in the heap");
}

struct alignas(256) Wide final {
    unsigned char bytes[256]{};
};

// With the mimalloc backend, every form of operator new reaches mimalloc.
void operator_new_routing() {
    int* const scalar = new int(7);
    int* const array = new int[100]();
    Wide* const wide = new Wide();
    Wide* const wide_array = new Wide[3]();
    int* const quiet = new (std::nothrow) int(5);
    std::vector<double> values(1000, 1.0);
    std::string text(500, 'x');
    require(aligned_to(wide, 256) && aligned_to(wide_array, 256), "aligned new is misaligned");
    const void* const blocks[] = {scalar, array, wide, wide_array, quiet, values.data(),
                                  text.data()};
    for (const void* const block : blocks)
        require(stx_auxlib::memory::in_backend_heap(block) == expect_mimalloc,
                "operator new bypassed the backend");
    delete scalar;
    delete[] array;
    delete wide;
    delete[] wide_array;
    delete quiet;
}

std::size_t handler_calls = 0;

void exhausted_handler() {
    ++handler_calls;
    std::set_new_handler(nullptr);
}

// The standard's exhaustion protocol for the replacement operator new. Run
// only with mimalloc: the host backend leaves operator new to the
// toolchain, and a sanitizer aborts on such a request.
void exhaustion_protocol() {
    const std::size_t impossible = std::numeric_limits<std::size_t>::max() / 2;
    require(::operator new(impossible, std::nothrow) == nullptr, "nothrow new did not fail");
    require(::operator new(impossible, std::align_val_t{64}, std::nothrow) == nullptr,
            "aligned nothrow new did not fail");
    handler_calls = 0;
    std::set_new_handler(exhausted_handler);
    bool thrown = false;
    try {
        void* const block = ::operator new(impossible);
        ::operator delete(block);
    } catch (const std::bad_alloc&) {
        thrown = true;
    }
    require(thrown, "exhausted new did not throw bad_alloc");
    require(handler_calls == 1, "new-handler was not called exactly once");
    require(std::get_new_handler() == nullptr, "new-handler was not cleared");
}

// Blocks allocated on one thread and released on another.
struct Handoff final {
    std::vector<std::vector<int>*> blocks{};
};

void produce(Handoff* const handoff) {
    Handoff& shared = *handoff;
    for (int index = 0; index < 2000; ++index) {
        std::vector<int>* const block = new std::vector<int>(static_cast<std::size_t>(index % 97 + 1), index);
        shared.blocks.push_back(block);
    }
}

void consume(Handoff* const handoff) {
    Handoff& shared = *handoff;
    for (std::size_t index = 0; index < shared.blocks.size(); ++index) {
        const std::vector<int>& block = *shared.blocks[index];
        require(block.size() == index % 97 + 1, "a handed-off block changed size");
        require(block[0] == static_cast<int>(index), "a handed-off block changed contents");
        delete shared.blocks[index];
    }
    shared.blocks.clear();
}

void cross_thread_release() {
    for (int round = 0; round < 8; ++round) {
        Handoff handoff{};
        std::thread producer(produce, &handoff);
        producer.join();
        std::thread consumer(consume, &handoff);
        consumer.join();
    }
}

} // namespace

int main() {
    backend_identity();
    new_handler_storage();
    allocate_and_release();
    rejected_requests();
    operator_new_routing();
    if (expect_mimalloc) exhaustion_protocol();
    cross_thread_release();
    std::puts("stx_auxlib memory tests passed");
    return 0;
}
