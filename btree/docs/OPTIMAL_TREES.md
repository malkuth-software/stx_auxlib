# Optimal search trees and the static optimal map

Two pieces, not from bbtree, built beside the maps:

1. **Optimal binary search trees** for given search weights
   (`search_weights.hpp`, `optimal_tree.hpp`): Knuth's O(n²) construction in
   two memory layouts, Garsia–Wachs for gap-only weights, the balanced tree
   for constant weights, and two near-optimal ones, all returning exact
   128-bit costs and the tree.
2. **`OptimalMap`**, a static ordered map laid out for a known access
   distribution (`optimal_map.hpp`, `detail/tree_packing.hpp`): a
   weight-optimal (or weight-balanced) binary tree, packed into multiway nodes
   that bbtree's node search reads.

Sources: Knuth, "Optimum binary search trees" (Acta Informatica, 1971);
Garsia and Wachs, "A new algorithm for minimum cost binary trees" (SIAM J.
Computing, 1977); Mehlhorn's bisection rule (1975); Gil and Itai, "How to
pack trees" (J. Algorithms, 1999). The weight conventions follow the
manuscript "Core-Sparse Monge Construction for Full Optimal Binary Search
Trees" (OpenAI Codex Sydney; problem selection and sponsorship: Joshuah
Rainstar), falseywinchnet/papers_please `obst/`, commit eef656f, whose
construction this repository carried until it was removed (see History
below). See [NOTICE](../NOTICE).

## Cost model

n keys with success weights p₀…pₙ₋₁, n + 1 gaps with failure weights q₀…qₙ
(gap j lies between keys j − 1 and j). A key at depth d costs p(d + 1), a
gap below d keys costs q·d: weighted three-way comparisons. All weights are
`std::uint64_t`; costs are `ExactCost` (signed 128-bit, a GCC/Clang
extension).

## Constructions

The caller knows its weights and picks the construction
(`TreeConstruction`, also `OptimalMapPlan::tree`):

| `TreeConstruction` | Exact | Use it for | Time | Memory |
| --- | --- | --- | --- | --- |
| `knuth` | yes | any weights | O(n²) | (n+1)(n+2)/2 cells of 8 B when W·(⌈log₂(n+1)⌉+2) < 2⁶³, else 16 B (1.07 GB at 16 384) |
| `knuth_scattered` | yes | popularity scattered over the keys (Zipf, log-normal in random key order) | O(n²) | as `knuth` |
| `garsia_wachs` | yes | gap-only weights (every key weight zero: alphabetic codes, miss-only workloads); refuses others | O(n log n) comparisons, O(n²) moves at worst | O(n) |
| `balanced` | when every key weight is equal and every gap weight is equal | flat weights | O(n) | O(n) |
| `bottom_up` | no, within 0.4–1.7% here | near-optimal trees at any size | about n log n × spine² | O(n) |
| `weight_balanced` | no, within 0.2–22% here | the cheapest approximation | O(n log n) | O(n) |

A guide, by what the weights look like:

- **All keys equal, all gaps equal:** `balanced`. A tree's cost with constant
  key weight p and gap weight q is (p + q)·I + np + 2nq for internal path
  length I, so the least-I tree is optimal and needs no search.
- **Only gaps weighted:** `garsia_wachs` (about 9× faster than `knuth` at
  16 384, linear memory).
- **Anything else, exact:** `knuth`. When the popularity is scattered over
  the keys rather than varying along them, `knuth_scattered` is faster (Zipf
  and log-normal 1.4–1.5×, random 1.15×); when it varies smoothly along the
  keys (ramps, a Laplacian around a centre key, uniform) it is 1.5–2.4×
  slower. Both return the same tree.
- **Large n, or exactness not needed:** `bottom_up`. It took about 1 ms at
  16 384 keys against Knuth's 0.9–1.2 s and stayed within 1.7% of optimal on
  every family measured. `weight_balanced` is cheaper still but loses up to
  22% on Zipf weights.

### Knuth's two layouts

Knuth's recurrence reads, for interval (i, j), the roots of (i, j − 1) and
(i + 1, j) and candidate costs C(i, r) + C(r + 1, j). Two changes cut its
memory by 2.5× and its time by 1.55–2.1× (2.3× for Zipf weights with
`knuth_scattered`), with the same candidates and result:

- **No root table.** Processing by length, both bounds come from the
  previous length, so one array of n + 1 roots serves (interval (i, j)
  overwrites slot i after reading slots i and i + 1). The final tree's roots
  are recovered afterwards from the costs alone: the root of each of its
  intervals is the least r with C(i, r) + C(r + 1, j) = C(i, j) − W(i, j),
  exact in integers, which is the leftmost optimal root the restricted
  search also settles on (under 1 ms at 16 384).
- **64-bit costs when they fit.** An interval's optimum is at most its mass
  times ⌈log₂(n + 1)⌉, so W·(⌈log₂(n + 1)⌉ + 2) < 2⁶³ bounds every cell and
  candidate sum; otherwise the 128-bit path runs.

These two came from an OpenAI GPT session the owner shared (2026-10-25,
"retain the cost table, discard root history"); this implementation is
written here. That session also noted that the diagonal schedule loses at
6000 keys to a row-by-row one; `knuth_scattered` is that schedule: rows i
from the last down with j rising, costs stored by column, so each cell's
right terms C(r + 1, j) are one run of column j beside the cell being
written and its left terms C(i, r) are the current row in one array.

Interleaved in one process (`research/obst/knuth_layouts.cpp`, Apple clang 21
-O2, n = 16 384, median of 3, ms; `research/obst/results/2026-10-25-m4-knuth-layouts-16384.csv`):

| family | previous (128-bit, root table, 20 B/cell) | `knuth` (diagonal, 64-bit) | 128-bit diagonal | `knuth_scattered` (64-bit) |
| --- | ---: | ---: | ---: | ---: |
| uniform | 1784 | 860 | 1072 | 2060 |
| zipf | 1372 | 884 | 1027 | 603 |
| log-normal | 1277 | 815 | 943 | 580 |
| random | 1484 | 869 | 1103 | 764 |
| laplacian | 1425 | 683 | 899 | 1176 |

Every variant's cost and tree equal the previous implementation's.

### Measured constructions, all families (Apple M4, Homebrew clang 23.1.2)

`results/2026-10-25-m4-constructions/raw.csv`, `optimal_map_bench construct`,
median of 3, constructions interleaved. The Mini was shared with another
session's work during this run, so absolute times are indicative. n = 16 384,
milliseconds, and cost against Knuth's optimum:

| family | `knuth` | `knuth_scattered` | `garsia_wachs` | `bottom_up` | `weight_balanced` | `balanced` |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| uniform | 1176 | 2519 | – | 1, ×1.0000 | 0, ×1.0000 | 0, ×1.0000 |
| zipf | 883 | 596 | – | 1, ×1.0171 | 0, ×1.2217 | 0, ×1.6535 |
| log-normal | 1065 | 720 | – | 1, ×1.0107 | 0, ×1.1361 | 0, ×1.3181 |
| laplacian | 905 | 1349 | – | 1, ×1.0059 | 0, ×1.0024 | 0, ×1.1323 |
| random | 1093 | 923 | – | 1, ×1.0036 | 0, ×1.0143 | 0, ×1.0168 |
| gaps only | 1117 | 743 | 122 | 1, ×1.0084 | 0, ×1.0200 | 0, ×1.3839 |

Families as in `construct_weights`: uniform all 1; zipf p = 10⁶/rank + 1
over a random permutation, q = 1; log-normal p = e^{10+2Z} + 1,
q = e^{8+2Z} + 1; laplacian p = 10⁹·e^{−|i−n/2|/(n/16)} + 1 with gaps a tenth;
random p in 1..1000, q in 0..999; gaps only p = 0, q Zipf over a random
permutation. Every exact construction matched Knuth's cost at 1024, 4096 and
16 384. Garsia–Wachs's moves were never the bottleneck here; an array-based
working list is quadratic only on adversarial weights.

### Checks

`tests/optimal_tree_test.cpp`: for every n ≤ 8 and eight weight families
(small random with zeros, keys only, gaps only, up to 2⁴⁰, uniform, Zipf-like,
an odd ramp and a ruler), every binary search tree is enumerated and costed;
the cubic recurrence and both Knuth layouts must equal that minimum (and
return the same tree), Garsia–Wachs must equal it on gap-only weights and
refuse the rest, the balanced tree must equal it on constant weights, and the
approximations must be valid and no cheaper. Then 96 instances 9 ≤ n < 149
against the cubic recurrence, 8 instances 600 ≤ n < 1200 and 24 gap-only
instances 200 ≤ n < 1600 against Knuth, and edge cases (empty, all-zero,
near-2⁶⁴ weights on the 128-bit path, constant non-unit weights). Each
returned tree is checked to be a search tree whose own cost is the reported
cost.

### History: the Monge layers

Until 2026-10-25 this repository also carried the manuscript's exact
construction: bounded-height terminal-penalty layers, each a core-sparse
Monge min-plus product (Gawrychowski–Gorbachev–Kociumaka, ESA 2025) with
witnesses for reconstruction, rewritten from the manuscript's MIT-licensed
prototype, and Codex Sydney's additive base and two-corner root screening.
It beat the previous Knuth layout from n = 16 384 on uniform weights, and lost
on skewed and random weights at every size measured (its time grows with the
total mass). The current `knuth` is 1.5–2.1× faster than the layout it was
compared with: at 16 384 uniform keys it takes 0.9–1.2 s against the layers'
1.78 s, and larger sizes were not re-measured (at 24 576 the layers took
2.67 s against the old Knuth's 5.55 s). Their one remaining niche, flat
weights at very large n, is served exactly by `balanced`, so they were
removed. The code is in the history up to commit f8c72e5; its measurements
are in `results/2026-10-25-m4-monge-fast/` and
`results/2026-10-25-m4-monge-revision/`; why no exact shortcut helps skewed
weights is in `research/obst/NOTES.md`.

## Packing and layout

`TreePacker::pack` cuts the binary tree into connected pieces of at most K
keys (K = 48 for two-byte windows) minimizing weighted node visits for that
tree (a dynamic program over (key, keys sharing its node); time Σ min(K,
size(left)+1)·min(K, size(right)+1)). A piece of k keys has k + 1 exits, so
each is a multiway node. Tests compare its cost with a walk of every search
and, for n ≤ 12 and K ≤ 4, with brute force over every edge cut (1 206
packings).

Packing a fixed tree fragments: a balanced tree fills 48-key nodes about
half, and a Zipf-shaped tree yields many small nodes. `ColdSubtreeLayout`
therefore re-lays a subtree as full count-balanced nodes (a B-tree bulk load)
when that costs at most `count_layout_slack_percent` (default 2) more
weighted visits than the packing there. Uniform weights become a full
B-tree (4M keys: 83 335 nodes, height 4, instead of 157 629 and 5); skewed
weights keep the packing, because under Zipf nearly every subtree has a
dominant key and the count layout loses more than 10% there. The price is
memory: 4–5× the nodes of the uniform layout at s ≥ 0.8.

## The map

Nodes are 128 bytes (one M-series line): 96 bytes of windows (48 × 2 B for
integers, 24 × 4 B for strings), an exit mask, entry and child offsets.
Windows are taken after the common prefix of the node's fences, which every
key and query reaching it shares. Search counts windows below and equal over
all slots in 16-byte lane groups, compares a single candidate inline and
sends several to an outlined tie search; the child is `first_child +
popcount(exits below e)`. `find_many` runs 16 lanes, each prefetching its next
node or candidate entry. Keys and values live in one array in node order.
Tests (`tests/optimal_map_test.cpp`) cover u64 (full range and crowded),
i64, and strings with long shared prefixes, embedded zeros and prefix keys,
for all constructions and slacks 0, 2, 50, under ASan/UBSan.

### Measured lookups (Apple M4, LLVM 23, median of 3, 10 M queries)

`results/2026-10-25-m4-optimal/lookup.csv`. Random u64 keys; query rank r has
weight round(10¹²/(r + 1)^s), ranks a random permutation of the keys, and the
skewed map is built (weight_balanced tree, slack 2) for exactly those
weights. ns per lookup:

| keys | s | sorted array | bbtree unsync | static, uniform | static, uniform, batched | static, skewed | static, skewed, batched | skewed visits | skewed nodes |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1M | 0.0 | 42.6 | 32.0 | 29.8 | 39.9 | 29.3 | 40.1 | 3.93 | 22 721 |
| 1M | 0.8 | 40.6 | 29.4 | 29.6 | 40.0 | 36.2 | 35.3 | 3.28 | 84 209 |
| 1M | 1.0 | 36.8 | 27.8 | 29.7 | 38.3 | 31.3 | 30.7 | 2.54 | 87 427 |
| 1M | 1.2 | 32.3 | 24.1 | 30.8 | 35.0 | 24.4 | 27.3 | 1.73 | 123 346 |
| 8M | 0.0 | 188.6 | 84.8 | 76.4 | 54.1 | 76.0 | 54.3 | 4.33 | 266 806 |
| 8M | 0.8 | 165.1 | 80.2 | 70.2 | 50.9 | 58.8 | 45.7 | 3.76 | 1 371 804 |
| 8M | 1.0 | 124.8 | 63.2 | 59.7 | 46.7 | 42.3 | 36.6 | 2.83 | 1 170 320 |
| 8M | 1.2 | 68.9 | 42.3 | 47.9 | 41.5 | 26.4 | 28.6 | 1.79 | 995 731 |

Reading them:

- **Past the caches the skewed layout pays.** At 8M keys and s = 1.0–1.2 it
  is 1.5–1.6× faster than bbtree and 1.4–1.8× faster than the same static
  map built for uniform weights; batched, s = 0.8 reaches 1.75× bbtree.
- **At 1M keys it does not.** The M4's caches hold most of either structure;
  bbtree ties the skewed map at s = 1.2 and beats it below.
- The uniform static map (a full B-tree with no slack) is 11% faster than
  bbtree at 8M, s = 0, and 13% slower at s = 1.2; it is a static structure
  against a dynamic one. bbtree's own batched lookup is not in this table.
- Visits fall with skew as expected (4.33 → 1.79), but time follows only
  where misses dominate; the extra nodes cost memory (1.17 M × 128 B = 150 MB
  against 34 MB at 8M, s = 1.0).
- One host, one key type, one distribution family; the Linux pod has not run
  this benchmark.

## Limits and next steps

- Construction for the map at millions of keys should use `bottom_up` (or
  `weight_balanced`, cheaper and looser); the exact constructions are
  practical to a few times 10⁴ keys (Knuth, by memory: 8 bytes a cell),
  `garsia_wachs` to millions for gap-only weights.
- The packing is optimal for the given binary tree, not among multiway
  trees; an exact multiway construction (a recurrence whose step splits an
  interval into up to K + 1 parts) is open.
