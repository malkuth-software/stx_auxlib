# stx_auxlib guardrails

`stx_auxlib` seals every platform-dependent call that GUI.Forms and its
consumers make behind one owned C++ surface, so that programs can become
mostly freestanding (and, as a long-term goal, run on UEFI). Exact
mathematics (`<cmath>`'s correctly rounded operations, `<cstring>` memory
primitives, `<bit>`, `<numeric>`, `<algorithm>`) stays with the toolchain;
transcendental functions, which differ between C libraries, get owned
kernels here.

- The repository's house style (`../AGENTS.md`, `HOUSE_STYLE.md` on the notes
  `main` branch) applies in full, tests included.
- Public headers under `include/stx_auxlib/` name no platform type: no
  `std::thread`, `std::mutex`, `std::filesystem`, stream, `FILE`, POSIX,
  Win32 or AppKit type. Platform state lives in the implementation.
- `src/hosted/` implements the surface over the C++ standard library and its
  host. A future freestanding implementation replaces it whole; no public
  header may assume which one is linked.
- This package depends on nothing outside `stx_auxlib/` and is buildable on
  its own (`cmake -S stx_auxlib -B <build>`). Its one third-party component
  is mimalloc in `third_party/mimalloc/`, pinned and hashed in
  `PROVENANCE.md`; it is updated only by replacing it with another pinned,
  hashed release, never edited in place.
- Sanitizer builds use the host memory backend (`STX_AUXLIB_ALLOCATOR=host`)
  so the sanitizers see the whole heap; run the tests in a plain build as
  well, since only it exercises mimalloc.
- Every change passes six clean builds: plain, secure, UBSan, ASan+UBSan,
  TSan and `STX_AUXLIB_INTEGER_CHECKS=ON`. A sanitizer report is a defect,
  never a catch. Every wraparound is impossible by construction or inside
  a function marked `STX_AUXLIB_MODULAR`; check size products and sums with
  `src/checked.hpp`.
- Nothing may be more than twice as slow as its platform equivalent:
  `stx_auxlib_performance_bench` must pass (owner, 2026-10-31).
- Each replaced implementation keeps its reference and a conformance test
  that runs against both (house style section 8). The thread pool's is
  `tests/reference/atomic_pool.c` with `tests/thread_pool_conformance.cpp`.
- Platform code that a GUI.Forms host adapter owns (`src/host/<platform>/`)
  is not hoisted here (owner, 2026-10-31); consumer projects' own platform
  files are.
