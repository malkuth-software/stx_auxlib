# Design

stx_btree is bbtree's design in C++. This page says where each part lives,
what the C++ form changed and why, and what the caller must know. The reasons
for the design itself, with measurements, are bbtree's
[decision record](https://github.com/XiangpengHao/bbtree/blob/8adae97a0ed97e5c98fa2790312eb0aec9719846/design/03-decisions.md);
the source cites its decisions as Dnn.

## Layers

| Header | Contents |
| --- | --- |
| `inline_key.hpp` | `InlineKeyTraits<K>`: a key's encoding, windows, comparisons from a known equal length, hash tags, shared-prefix checks; `ByteEncoding`, `InlineKeyWindows` |
| `page_allocator.hpp` | `GlobalPageAllocator`: zeroed, size-aligned pages, `std::bad_alloc` on failure |
| `pooled_page_allocator.hpp` | `PooledPageAllocator`, `PagePool`: pages carved in address order from shared, growing chunks, reused when freed |
| `detail/node_layout.hpp` | compile-time page layouts (D13, D15, D21, D29, D34, D36, D49) |
| `detail/vector_count.hpp` | the node search: count below and equal over every slot (D56) |
| `detail/node.hpp` | `TreeShape` (the node plan, D47), `LeafNode`, `InternalNode`, `Window`, tagged `NodePointer` |
| `detail/root_page.hpp` | the root page and the shared prefix (D50, D52) |
| `detail/node_stack.hpp` | node stacks (D24) |
| `detail/tree_algorithms.hpp` | splits, prefix refits, rebalancing removal, shared-prefix shrinking, shared by both trees |
| `detail/unsync_tree.hpp`, `detail/tree_walk.hpp` | the single-threaded tree, its cursors and batched inserts |
| `detail/latch.hpp`, `detail/latched_tree.hpp` | latch words, free lists, the shared tree (D54-D57) |
| `detail/batch.hpp` | lookup and insert lanes (D53, D55) |
| `unsync_map.hpp`, `bbtree_map.hpp` | the public maps |

## Nodes

A leaf is 1 KiB (more for large entries), an internal node 512 B, each aligned
to its size; the root lives in a root page for the tree's life, followed by up
to 64 bytes every key shares. Every pointer to a node carries its kind in bit
0. A leaf holds its keys' windows (2, 4 or 8 bytes each), optional hash tags,
then entries: entry 0 is the head (a separator's key and value, D49), entry
`i + 1` key `i` with value `i`. An internal node holds its separators'
windows and its edges; a separator's key is the head of the leftmost leaf
right of it, read on a tie by walking down. Slots past a node's length hold
all ones, so the count needs no mask. Underfull is below
`max(capacity / 8, 4)` keys (D22).

`static_assert`s in `TreeShape` check every layout: capacity within
[12, 1023], entries inside the page, 512-byte alignment for the tag bit.
For `std::uint64_t -> std::uint64_t`, leaves hold 55 entries and internal
nodes 49 children (48 in the shared map, whose header holds a latch word),
as in bbtree.

## Keys

A key type takes part by specializing `stx_btree::InlineKeyTraits<K>`. The
contract is bbtree's: each key has a byte encoding, zero-padded past its end,
and if `a`'s encoding orders before `b`'s, then `a < b`; equal encodings
decide nothing. A wrong encoding gives wrong answers, never out-of-bounds
access. The members are:

| Member | Meaning |
| --- | --- |
| `Key`, `Query` | the stored type and the lookup argument (`std::string_view` for `std::string`) |
| `leaf_width`, `internal_width` | bytes of each window a node keeps: 2, 4 or 8 |
| `has_hash_tags`, `tag_is_length` | a leaf keeps a byte per key; a tag below 255 is the exact length |
| `orders_by_own_bytes` | the key holds its bytes (trivially copyable), so a shared map may copy it racing a writer and compare after the version check |
| `query_of`, `window`, `encoded_length` | the query view, 8-byte windows at an offset, the encoding's length |
| `compare_from`, `equal_from` | order and equality given the first `equal_length` bytes are equal |
| `hash_tag`, `common_prefix_length`, `leading_bytes`, `shared_length`, `compare_shared` | tags, fences' common prefix, and the shared prefix |

`InlineKeyWindows<Traits>` derives the last group from `window`.
[`examples/custom_key.cpp`](../examples/custom_key.cpp) is a complete composite
key. Provided: `std::uint64_t`, `std::int64_t` (sign flipped), 2-byte windows
(D31); `std::array<std::uint8_t, N>`, 2-byte windows, ties read in the node
(D32); `std::string`, 8-byte leaf and 4-byte internal windows with hash tags
(D36, D43).

Keys and values must be nothrow move constructible and destructible; they
move inside nodes by move construction, or `memmove` when both are trivially
copyable. Unlike bbtree, there is no layout for keys without an encoding.

## The node search

`InlineKeyCount<Slot, Capacity>` counts slots below and equal to the query's
window across the whole capacity in 16-byte groups of vector-extension lanes,
accumulating in the slot's own width; a ragged capacity ends with one
overlapping, masked group. On AArch64 this compiles to `cmhi`/`cmeq` and
`sub` per group, the shape of bbtree's hand-written NEON count. A tie, one
candidate or a few, is settled by hash tags, a single equality test or a
binary search over the tied keys. The search adapters are forced inline and
the tie searches kept out of line, which bbtree measured as necessary (D41,
D49); `detail/inlining.hpp` says so to GCC and Clang.

The lanes are summed at the end of each count. Under GCC the eight two-byte
lanes are summed by three shuffle-and-add steps instead of a loop: GCC 13
compiled the loop into one extract and one add per lane on x86-64, and the
shuffles made its unsync u64 lookups at 10M about 25% faster on the compute
pod. Clang keeps the loop, which it compiles well on x86-64 and which
measured 6% faster than the shuffles with LLVM 23 on the M4. bbtree, being
Rust, has only the LLVM case. (results/2026-10-25-pod-x86-diagnosis.)

## The unsync map

`find` descends without a stack; `insert` first tries the remembered last
leaf (D45), shrinks the shared prefix for a key that lacks it (D50), inserts
into a leaf with room, and otherwise searches again recording a node stack,
reserves every page the split needs, and splits (D37). Splits at a node's
end leave the arriving side `min(2 * minimum, capacity / 2)` entries (D32).
Removal rebalances bottom-up by merging with or taking from a sibling, and a
removed separator is replaced by its successor; every node whose fences
changed is then refit (D22, D26). Iterators keep a node stack and never
compare keys. Any change invalidates pointers and iterators.

## The shared map

The same nodes and the same tree algorithms (D54):

- **Latch word** at byte 8 of every page: an exclusive bit, 20 bits of shared
  holders, a version that only grows. Internal nodes are read optimistically;
  leaves are latched shared or exclusive.
- **Descent** reads each internal node's length, prefix and windows racing
  writers, routes, reads the child's version, then checks the parent's. The
  root page alone changes kind, so its edge is followed only after its
  version is checked. Ties compare separator heads: copied where keys order
  by their own bytes, otherwise under the head leaf's shared latch.
- **Lookups** (D57): `get_copied` reads the leaf optimistically and copies a
  trivially copyable value out, checking the version after, where the leaf
  settles the key (integer and array keys; strings that end within their
  window). Otherwise it latches the leaf at the version read.
- **Changes beyond a leaf** descend again keeping the path, take without
  waiting (compare-and-swap at the versions read) every node the unsync
  operation will change or read a separator of, reserve pages, run the
  unsync operation along a node stack from the topmost node held, publish a
  changed root, let go, and only then put freed pages on free lists.
- **Type-stable pages**: freed pages stay on per-kind free lists with their
  latch words until the map is destroyed. No epochs, no guards.
- **Scans** latch the next leaf before letting go of the current one, through
  the kept path, or search again from the last key visited.

Visitors run with the entry's leaf latched and must not call into the same
map. There is no `size()`: a shared counter would serialize writers.

### What the language model says about it

Optimistic lock coupling reads node bytes that a writer may be changing and
discards the result when the version moved. bbtree does the same in Rust, as
do C++ OLC trees (Leis et al., 2016); the C++ memory model counts these
reads as data races. The fields whose values are used before validation (a
node's length, prefix length and edges, latch words, the root pointer, the
shared prefix's length) are accessed as atomics, and every racing read is
bounds-checked against the node's capacity before use. The windows a count
reads and the bytes of a racing copy are plain loads. ThreadSanitizer would
report them; ASan and UBSan runs of the threaded and stress tests are clean.

## Batches

Batched lookups and inserts run lanes, each an explicit state machine
(house style admits no coroutines) with bbtree's stages: route, prefetch the
chosen edge, follow, prefetch the child's header, and so on; a round-robin
executor over up to 64 lanes yields after every prefetch. `Prefetcher` is the
only place the library issues a prefetch (D46). Below about an L2 of leaf
pages (unsync), or three internal levels (shared), keys go one at a time.
`insert_many` gives the same results and the same tree as inserting in order.

## Failure

Expected alternatives are return values (`std::optional`, `bool`). The only
exception the library throws is `std::bad_alloc` from the page allocator;
both maps take every page a change needs before changing anything, so the
map is unchanged when it is thrown. A broken invariant that would make a
count wrap (an empty leaf right of a separator) aborts, as bbtree's
`expect` panics.

## Pooled pages

bbtree's `Allocator` trait pools pages by kind as an option (D8); its maps
default to the global allocator, as this form's do. `PooledPageAllocator` is
that option. Each size class carves pages from chunks that double from
64 KiB to 2 MiB, in address order, and keeps freed pages on an intrusive
list; chunks go back to the system when the pool ends. The pool is a
`std::shared_ptr` shared by allocator copies, so a map owns its pool and maps
built from one allocator share it; the allocator has no move constructor, so
a moved-from map keeps a usable pool. A mutex guards the pool: the shared
map allocates only when its own type-stable free lists are empty (D54), about
once per leaf split, and gives pages back only when it is destroyed.

The reason is measured, not inherited: on Linux x86-64 glibc's aligned path
placed 744 407 1 KiB pages over 1.55 GB for 0.76 GB of pages, for this form
and for bbtree alike, and packed pages (a bench-only arena of the same design) made this form's operations
7–52% faster there (docs/MEASUREMENTS.md, "Writes"). On macOS the system
allocator already packs them; the pool measured the same as the default.
