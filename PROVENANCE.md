# Provenance

## Mathematics

`include/stx_auxlib/math.hpp`, `include/stx_auxlib/detail/sincos_table256.hpp`,
`tests/math_tests.cpp`, `tests/reference/wide.hpp` and
`tools/gen_sincos_table.cpp` are first-party. The sine and cosine kernel
derives from bfft's `bruun_table256_poly3_sincos` (falseywinchnet/bfft
`a56bc4c`, `src/detail/MAG_REPRESENT_KERNEL.hpp`, MIT, the same owner). The
table's high parts agree bit for bit with bfft's mpmath table except at its
three exact zeros, where mpmath's 100-digit pi leaves about 1e-102.

## Random

`include/stx_auxlib/random.hpp`, `src/random.cpp` and
`tests/reference/random_reference.py` are first-party implementations of
published algorithms: ChaCha (D. J. Bernstein, 2008; 12 rounds, the
original 64-bit counter and 64-bit nonce layout), xoshiro256++ 1.0 and its
jump polynomials (D. Blackman and S. Vigna, public domain reference code),
SplitMix64 (G. Steele, D. Lea and C. Flood, 2014), and Lemire's
nearly-divisionless bounded integers (D. Lemire, 2019). The ChaCha20
known-answer block is RFC 8439 section 2.3.2.

## Preference Ranked Voting

`src/voting.cpp` is a C++ port of `prv-core.mjs` from the owner's
paymenottowork site (falseywinchnet/paymenottowork, branch `deploy/site`,
commit `d24ac615998e06a8c298412503fd081ec7966749`,
`ideas/preference-ranked-voting/`). The reference itself is kept unchanged
as `tests/reference/prv-core.mjs` (SHA-256
`d57a163e5aefa721e3b09ff3b861ca69d6d75703ec7891c10f8b20b4e68e3166`) and
drives `tests/reference/prv_reference.mjs`. The site's receipt and
credential modules (`receipt-core.mjs`) are an educational demonstration,
not part of the count, and are not ported.

## stx_btree

`btree/` is malkuth-software/stx_btree at commit
`461dda61f1c9c362b12d0d0e928696e1ebcd9ca1`: `include/`, `tests/` (without
the installed-consumer fixture), `LICENSE`, `NOTICE`, `PROVENANCE.md` and
`docs/DESIGN.md`, `docs/OPTIMAL_TREES.md`. It is itself a C++ form of
Xiangpeng Hao's bbtree under bbtree's MIT terms; see `btree/NOTICE` and
`btree/PROVENANCE.md`. Changed for stx_auxlib, each marked "stx_auxlib
adaptation": `detail/latch.hpp` yields through `stx_auxlib::yield_thread`,
`pooled_page_allocator.hpp` locks with `stx_auxlib::Mutex`, and
`detail/node.hpp` reports its one broken-tree fault through
`stx_auxlib::fail`.

## Thread pool

`include/stx_auxlib/thread_pool.hpp` and `src/hosted/thread_pool.cpp` are a
C++ port of the owner's `threadpool_atomic_fast` (MIT, Joshuah Rainstar), as
GUI.Forms carried it in `src/core/threading/atomic_pool/`. The port keeps its
algorithm, memory orders, result codes and lock order; std::thread,
std::mutex and std::condition_variable replace pthreads.

The C original is kept unchanged as the test reference:

| File | SHA-256 |
|---|---|
| `tests/reference/atomic_pool.c` | `b1282195e93177305d7cfd7f1bdd14586ee325fe02c186128cc0a5b5def5d91d` |
| `tests/reference/atomic_pool.h` | `0dc5257334c940cf6080e26321cbb995fb562031f2d81aec92ac8b0892190fc4` |
| `tests/reference/LICENSE-threadpool_atomic_fast.txt` | `7ffa68e2ff116c26c79f75985354b28cb59148efacaff60ceaa5da8795333ad1` |

GUI.Forms took those files from `falseywinchnet/backend`
(`orchestrator/third_party/threadpool_atomic_fast`, revision
`6eb51f84260984b4b293156ed7fefb057267bce5`) and renamed its symbols to the
`gui_forms_atomic_pool_` prefix; the reference keeps those names.

## mimalloc

`third_party/mimalloc/` is mimalloc v3.5.3, <https://github.com/microsoft/mimalloc>,
MIT (`third_party/mimalloc/LICENSE`, Copyright (c) 2018-2025 Microsoft
Corporation, Daan Leijen). Tag `v3.5.3` (tag object
`deac3dcf5f0614884013d411a5c07e16bec96c9f`) names commit
`d4881d338125e1cb7c47ba4cfb398d6f7c0c8d45`, released 2026-09-17: the newest
release at least two weeks old on 2026-10-31 (v3.6.0 and v3.6.1 were eight
days and one day old). Cloned from GitHub on 2026-10-31 and copied
unmodified.

Kept: `LICENSE`, `include/` and `src/`, without the build, test, binding and
documentation trees, the Windows event-tracing manifests (`etw.man`,
`etw-mimalloc.wprp`), the `readme.md` files, and the malloc-override headers
(`mimalloc-override.h`, `mimalloc-new-delete.h`), which stx_auxlib does not
use. Each kept file's Git blob SHA-1 equals the upstream tree at the pinned
commit (checked with `git hash-object` on 2026-10-31):

| File | Blob SHA-1 |
|---|---|
| `LICENSE` | `53315ebee557ac22f3396ea1781df0440c26bb15` |
| `include/mimalloc-profile.h` | `5a1327ff70d725291b837495b92c739427b53972` |
| `include/mimalloc-stats.h` | `ab45a0715f70b149b3efa2aabb39e0925bb36cb5` |
| `include/mimalloc.h` | `2985edd9492a503d2e28005178f9b5023959c2c9` |
| `include/mimalloc/atomic.h` | `9c23398024ab74432a58b34bc2be68cc5fe1c9c8` |
| `include/mimalloc/bits.h` | `5aa0d83755f6c9bfbca00febddf23776f7cf5926` |
| `include/mimalloc/internal.h` | `b79649c72800521a6d4b52ff300de170b4719214` |
| `include/mimalloc/prim-tls.h` | `da78d0193dff9743b2329bfc2b7ece8195f2a044` |
| `include/mimalloc/prim.h` | `82b2d9c3d242e6b1dcaaae85a47b22a0c189fa32` |
| `include/mimalloc/track.h` | `e6a57c8ec4894667f955cafd10287ff27dc12ba2` |
| `include/mimalloc/types.h` | `530c464a368768866191e59e5dedba80582d802a` |
| `src/alloc-aligned.c` | `51f29732cbd85c67641604620dbfc6dc5501705a` |
| `src/alloc-override.c` | `bcb96b52d7dd675717fab11d800e5a503ce86610` |
| `src/alloc-posix.c` | `a2252494255af0f8883fdcbe3885e664c77b516b` |
| `src/alloc.c` | `a176db77f41a667ed57cfe5155e733d6f887e00f` |
| `src/arena.c` | `1b7c8cb336d0cef7bfe614456983130effe6fe42` |
| `src/bitmap.c` | `238c4d4c4c12a3f3d5918ae992779af3be6a19fa` |
| `src/bitmap.h` | `79eda77fad6ab85e26eeb5faefd7b029eabee75a` |
| `src/free.c` | `722ee7bf6933ae6596c6ee05a92a3e05b320f285` |
| `src/heap.c` | `d7f1db13211254dc955b7371b00f76436c226a3b` |
| `src/init.c` | `9b0ded5b547d30a35df2f7efc23e70b1f4f8c9ed` |
| `src/libc.c` | `8f211230d0cd3a8486d6f71a704ecfb1130b8831` |
| `src/options.c` | `eaa2119def5fc2dd575904d2470e3f6d023788e3` |
| `src/os.c` | `15111cf94988cfe61ea0428273d96c722a4890a4` |
| `src/page-map.c` | `949e37ac3afabfabb0923717df94a09510f5ac90` |
| `src/page-queue.c` | `c8e9b5425ce5adff2998fc63ac828b8189e39bce` |
| `src/page.c` | `aa669cdd02928218ab0e9863428d7db36b7dd76f` |
| `src/prim/emscripten/prim.c` | `992965d56f23bf2bc2fc41aa1daa994335486a76` |
| `src/prim/osx/alloc-override-zone.c` | `48f97958218d9e3343249d438da3a4b2c01f0357` |
| `src/prim/osx/prim.c` | `8a2f4e8aa473165a1e61188012a7ca5a8ca1aea5` |
| `src/prim/prim-tls.c` | `1b79321a05ef00cc23b27033b23de0814143fc1f` |
| `src/prim/prim.c` | `3d3d0c2e8c881bf27a181366b39526e2ee353411` |
| `src/prim/unix/prim.c` | `25c2adea039f862665f15541221c58cfe8ae0d57` |
| `src/prim/wasi/prim.c` | `2d69b7245fb1a5fea8ff7b41ca5b46f5a36efe1a` |
| `src/prim/windows/etw.h` | `4e0a092a10f4ba96facb51781296dc32c8ff2f84` |
| `src/prim/windows/prim.c` | `222fa6303e496d70c8c99a271aa20fec7c4a1212` |
| `src/random.c` | `3c012adf45c4abe4221add5e61d0652c7797869c` |
| `src/sample-guarded.c` | `f7c1fe81e99ba7ee610a8e54f2afcea853ae6d93` |
| `src/sample-profile.c` | `f1b5db4913e3e7c09b9b50467e8b64340fba2276` |
| `src/static.c` | `8a4f8c3d54052876e1c6f04e0c1c2b5b0c62445d` |
| `src/stats.c` | `c763bfc3c7b5b7c38fd4b8518705cb8778c6c920` |
| `src/subproc.c` | `f42d4a11c5208b9620e456a412c0cdafa9dec2c9` |
| `src/theap.c` | `1731830a7aa416b07bc82554779a622ef318830d` |
| `src/threadlocal.c` | `6f280d09353c918b28ec414ae5ab10e745797229` |

Build (`CMakeLists.txt`): `src/static.c`, mimalloc's single translation
unit, compiled as C++ (mimalloc's `MI_USE_CXX`) with `MI_STATIC_LIB`,
`MI_BUILD_RELEASE` and `NDEBUG`, hidden visibility, initial-exec TLS and no
warnings; `MI_SECURE=4` with `STX_AUXLIB_MIMALLOC_SECURE`. `MI_MALLOC_OVERRIDE`
is not defined, so the C library's malloc is never replaced. C compilation is
not used: it defines a weak null-returning `std::get_new_handler` that, with
hidden visibility, replaces the C++ library's in the whole program.
