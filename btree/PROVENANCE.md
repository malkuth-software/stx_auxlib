# Provenance

stx_btree is a C++20 form of **bbtree** by **Xiangpeng Hao**
([XiangpengHao/bbtree](https://github.com/XiangpengHao/bbtree), crate
`bbtree` 0.1.5), translated from the upstream commit

    8adae97a0ed97e5c98fa2790312eb0aec9719846  (2026-10, "Thank the post's reviewers, with links")

It is offered under bbtree's own terms, MIT OR Apache-2.0 ([LICENSE](LICENSE)).
The design, the measurements that justify it and the decision record are
bbtree's: [design/03-decisions.md](https://github.com/XiangpengHao/bbtree/blob/8adae97a0ed97e5c98fa2790312eb0aec9719846/design/03-decisions.md)
and the documents beside it, and the post
[bbtree: fastest ordered map](https://bbtree.xiangpeng.systems/post). Decision
numbers (Dnn) in the C++ comments refer to that record.

The C++ source was written for this repository by Claude Code (Claude Opus
5.5) under the owner's direction, following the owner's programming house
style ([docs/PROGRAMMING_HOUSE_STYLE.md](docs/PROGRAMMING_HOUSE_STYLE.md)).
No Rust source is compiled into the library. The upstream checkout is used
only by the reference benchmark (`benchmarks/rust`), which depends on the
pinned crate by path and is not part of the package.

## What maps to what

| bbtree (Rust) | stx_btree (C++) |
| --- | --- |
| `inline_key.rs`, `proof/` encodings (`[u8]`, `u64`, `i64`) | `include/stx_btree/inline_key.hpp` (`InlineKeyTraits`, `ByteEncoding`) |
| `tree/node/layout.rs`, `page.rs`, `node_plan.rs` | `detail/node_layout.hpp`, `detail/node.hpp` (`TreeShape`) |
| `tree/node/vector_count.rs`, `inline_key_search.rs` | `detail/vector_count.hpp` |
| `tree/node/leaf.rs`, `internal.rs`, `inline_keys.rs` (`Window`), `hash_tags.rs` | `detail/node.hpp` (`LeafNode`, `InternalNode`, `Window`) |
| `tree/node/siblings.rs`, `tree/insert.rs`, `tree/remove.rs`, `tree/prefix.rs` | `detail/tree_algorithms.hpp` |
| `tree/node/root_page.rs`, `tree/shared_prefix.rs` | `detail/root_page.hpp` |
| `tree/node_stack.rs` | `detail/node_stack.hpp` |
| `tree/root.rs`, `tree/search.rs`, `unsync/map/` (core operations) | `detail/unsync_tree.hpp`, `unsync_map.hpp` |
| `tree/walk.rs` (cursors) | `detail/tree_walk.hpp` |
| `tree/node/latch.rs`, `tree/latched/pages.rs` | `detail/latch.hpp` |
| `tree/node/optimistic.rs`, `tree/latched/*` (descent, held, insert, remove, lookup, append, shared prefix, scan) | `detail/latched_tree.hpp` |
| `map/` (`BBTreeMap`) | `bbtree_map.hpp` |
| `tree/batch.rs`, `tree/latched/batch.rs` (lookup lanes) | `detail/batch.hpp` |

## Changes made in translation

These are deliberate; each is explained further in [docs/DESIGN.md](docs/DESIGN.md).

- **Every key has an inline-key encoding.** C++ specializes `InlineKeyTraits`
  directly, so the Rust `Auto`/`WithInlineKey` strategy split, which works
  around missing specialization, is gone, and so is the separate layout for
  keys without inline keys (D17's internal-node keys). Provided encodings:
  `std::uint64_t`, `std::int64_t`, `std::array<std::uint8_t, N>` and
  `std::string` (queried by `std::string_view`). Others specialize the trait,
  optionally deriving from `InlineKeyWindows`.
- **Batch lanes are explicit state machines**, not async blocks: the house
  style admits no coroutines. The stages, the prefetch points and the
  executor's live-bit mask follow `tree/batch.rs`.
- **The node count uses GCC/Clang vector extensions** over 16-byte lane
  groups, one portable form for NEON and SSE, instead of hand-written NEON and
  AVX-512. Other compilers take a scalar loop.
- **Allocation failure throws `std::bad_alloc`** before any change, from both
  maps: every page a split needs is reserved first, as bbtree's shared map
  does. The unsync map does not abort (Rust's `handle_alloc_error`).
- **Keys and values move by move construction**, not by byte copies, unless
  both are trivially copyable; the move constructor must not throw.
- **Visitors instead of closures** for the shared map's `read`, `update`,
  `get_or_insert`, `update_or_insert`, `remove_if` and scans; the library's
  own callables are named functors. Scans take explicit bounds and a
  visitor returning whether to continue.

## Not translated

The following parts of bbtree are not in this form: file maps and
`bbtree-mmap` (D58), `bbtree-store` and leaf annexes (D60), the per-kind
allocator hooks beyond one page allocator (D8), node-config page sizes
(D59), the unsync `BBTreeSet` and set algebra, the `entry` API,
`split_off`/`append`/`retain`/`extract_if` and the bulk builder (D25),
`clone`, the cached leaf-run range iterator (D33, D35, D42), batched inserts
(`insert_many`, `update_or_insert_many`), `read_many`, `range_cloned`, the
inspection views and visualizer, the fuzz targets, the Shuttle schedules,
and the Verus proofs. The proofs state properties of the Rust encodings;
the C++ encodings are tested against them by the differential tests, not
proven.

## Optimal search trees and the static map

`search_weights.hpp`, `optimal_tree.hpp`, `detail/tree_packing.hpp` and
`optimal_map.hpp` are not from bbtree. The weight conventions follow the
manuscript "Core-Sparse Monge Construction for Full Optimal Binary Search
Trees" (OpenAI Codex Sydney; problem selection and sponsorship: Joshuah
Rainstar) in
[falseywinchnet/papers_please](https://github.com/falseywinchnet/papers_please)
at commit eef656fae6e6bf65fd19b5f74abc6081ad6e67f7. Its exact layer
construction, rewritten from its MIT-licensed prototype
`obst/code/compact_obst.cpp`, with Codex Sydney's additive base and
two-corner root screening, was part of this repository up to commit f8c72e5
and has been removed (docs/OPTIMAL_TREES.md, History). Knuth's O(n^2)
construction, Garsia–Wachs, Mehlhorn's bisection rule, the Gil–Itai packing
and the count layout are standard; the map's node search reuses this
repository's bbtree-derived inline keys, lane-group count and prefetch.
Written by Claude Code (Claude Opus 5.5) under the owner's direction.
Knuth's single-diagonal root array, root recovery from the cost table and
64-bit cost storage follow suggestions from an OpenAI GPT session the owner
shared (2026-10-25); the row-by-row, column-major layout (`knuth_scattered`)
follows its note that a row schedule wins at larger n. The code is written
here. Garsia–Wachs (1977) follows Knuth, TAOCP 6.2.2.
`bottom_up` comes from this project's research (research/obst/NOTES.md).
[docs/OPTIMAL_TREES.md](docs/OPTIMAL_TREES.md) has the design and measurements.
