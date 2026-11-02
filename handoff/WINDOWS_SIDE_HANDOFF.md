# Windows-side handoff to the stx_auxlib owner (2026-11-02)

This side branch (`claude/windows-side-handoff`) comes from the GUI.Forms coordinator on the Windows x86-64 host. It is the dispatch work from that host's hot-paths worker, plus this note, and is for the stx_auxlib owner (the local sibling) to review and land. **Nothing here is on main.** The owner asked for it to be pushed as a side branch.

## What the branch carries (on origin/main 308c265)

| Commit | What |
|---|---|
| e57a540 and its sse-finetune successors (779abeb, 3f994d2, 7d33e3a, 8d110d2, a7349c9) | SSE2 fine-tune: zeroed cvtss2sd destination (removes the false dependency); atan2 reads the larger operand's exponent from its bits (no libm frexp or ldexp); kernels built with `-fno-math-errno`; Dekker table factors split at compile time; a certified cheap float tan; hidden visibility for the kernel constants |
| 6cc942b | **SIMD dispatch:** `include/stx_auxlib/simd.hpp` and `src/simd.cpp`, one SIMD level per process, with the math kernels dispatched through it |

Also pushed, for the record only: `claude/sse-branchfree-negative` (d089e71), a measured negative result. Don't land it.

### Evidence (Windows 11, LLVM 22.1.8 CLANG64, native x86-64)

- **Bit-identical to 308c265:** all 2^32 inputs of the 17 float functions, the pair and double samples, and the owned-input hashes, in the SSE2, Dekker and v3 builds, at every SIMD level. ASan and UBSan are clean.
- **Float timings under SSE2, before and after:** exp 0.40x; log about 0.39x; pow 0.57x; atanh 0.60x; tan 0.30x (44.2 to 13.4 ns); atan2 double 0.66x. The table splits give 2-6% and hidden visibility mostly 0-5% (both marginal; the splits stay because they help v128).
- **Dispatch:** avx2_fma against v128 for doubles is 0.45-0.82x, except pow at 1.16x (see task 3). Dispatch overhead is in the noise for doubles and 2-9% for floats.
- The full record is in the private projects repo, tracks/gui_forms/hot_paths.md (the last sections).

### The design of simd.hpp

- **Tiers follow BFFT** (owner direction): `scalar`, `vector128` (SSE2 on x86-64, NEON on arm64) and `avx2_fma`. **No AVX-512**, by the owner's decision.
- Detection uses CPUID and XGETBV, once per process, chosen at run time. That is the one deliberate difference from BFFT, which selects at compile time; the owner directed runtime selection.
- Forced selection is available through `STX_SIMD_LEVEL` or `request_level()`, so tests run every tier on one host.
- No dependencies.
- The variants get their target through attribute pragmas, so no AVX code can leak into the SSE2 path; this was checked on the objects.

## Owner decisions behind this (2026-11-01/02)

- CPU detection and SIMD selection are first-class, with SSE, AVX and FMA options, not an afterthought.
- GUI.Forms takes stx_auxlib as a **pinned dependency** and uses `stx_auxlib::simd` (first for stx_conv's AVX2 lanes, later stx_skia). **GUI.Forms needs simd.hpp and simd.cpp on origin** so its pin names a real commit. Until then its wiring sits on a local override and isn't merge-ready.
- `-ffp-contract=off` is now the GUI.Forms default for first-party code (stx_auxlib's math tests already use it).

## Tasks for the stx_auxlib owner

1. **Review and land the dispatch layer** (at least simd.hpp and simd.cpp; the math dispatch too, if accepted), then give the coordinator the landed commit for GUI.Forms' pin.
2. **Review the SSE2 fine-tune commits.** They are bit-identical, so the decision is about design fit only.
3. **pow under avx2_fma is 1.16x, a regression.** The Windows worker found that keeping `log_natural_wide` out of line in the avx2_fma variant takes pow double from 45-51 ns to 25-35 ns (0.5-0.7x of v128). That change is in progress on the Windows side and will follow; coordinate it, or take it over.
4. **DEFECT, a memory bug: on MinGW the global allocator corrupts the heap with any std::thread.**
   - Host: Windows 11, LLVM 22.1.8 CLANG64 (MSYS2 clang64, shared libc++ DLL), -O1 -g.
   - Library: 308c265 plus the study commits. It already reproduces at a7349c9, before any dispatch change. `STX_AUXLIB_ALLOCATOR=mimalloc` (the default).
   - Program: `handoff/mingw_allocator_thread_repro.cpp` calls math::exp once, then starts 4 std::threads and joins them.
   - Build: `clang++ -O1 -g -std=c++20 -I<stx_auxlib>/include handoff/mingw_allocator_thread_repro.cpp -o repro.exe libstx_auxlib_global_allocator.a libstx_auxlib.a -lbcrypt -lshell32 -lpsapi -luser32 -ladvapi32`
   - Result: prints "start" and "one 2.71828", then exits 0xC0000374 (STATUS_HEAP_CORRUPTION) at thread start or join. Without libstx_auxlib_global_allocator.a it completes, rc 0. The math is irrelevant: any std::thread does it.
   - Hypothesis, unverified: libc++ is a DLL, and its std::thread state is allocated by the program's replaced operator new (mimalloc) and freed inside libc++.dll with the DLL's own operator delete (HeapFree), or the reverse. With a shared C++ runtime on Windows, "one allocator per process" may not hold.
   - **GUI.Forms will not enable stx_auxlib_global_allocator until this is fixed.**
5. **The pow hash test:** switch `pow_input`'s three `std::log(x)` calls to `stx_auxlib::math::log(x)`, and set the frozen value at tests/math_tests.cpp:875 to `0xc5ae8756613caabf`. The arm64 sibling confirmed this hash on Apple clang 21 (plain and Dekker, -O0 and -O2), and Windows x86 gives the same.
6. **Windows-only host-comparison test failures:** UCRT's tan(±inf) returns -NaN, and mingw's cbrt is 1 ulp off at -2^-40. These are host-library behaviour, not stx_auxlib's. Either compare against our own reference there, or document the exception.
7. **SSE2 against UCRT:** after the fine-tune, the SSE2 doubles are still 2.1-3.9x UCRT, mostly in the Dekker product path. avx2_fma under dispatch is the answer on capable CPUs. Keep or tighten the 2x ceiling as the design decides.

## Contacts

The owner relays between threads. The GUI.Forms coordinator (Windows side) keeps the private projects repo; its STATE.md records this handoff.
