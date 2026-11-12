# Order-statistic sums in stx_btree: research notes

Research track for moving GUI.Forms' `RowHeightIndex` into stx_btree. It is
written to be resumed from: the state, the decisions taken so far and the
next step are in §7. The owner's decisions of 2026-11-12 are in §6. The
research harness and the API design built on them are in §8. No figure
here is a measurement unless it names its run. Every bound is cited to a
source that was read. The one exception, Iacono's bound, is marked
*(secondary)*: no publisher or author copy was available, and it is stated
from a secondary source.

This branch carries the package (`sequence_tree.hpp`, `row_height_index.hpp`
and their tests) without the research harness. The harness of §5 and §8.3
(`btree/research/order_statistic/`) is on the branch
`claude/btree-order-statistic-research`.

## 0. The question and the short answer

The owner's goal: put `RowHeightIndex` into stx_btree, and "see if we can
push beyond the state of the art towards n log n". The direction of
2026-11-09 (projects `desks/coordinator/POST_0.5_BACKLOG.md`, "Startup bar")
is a height-indexed B-tree whose nodes keep their subtree's row count and
total height, with no Fenwick tree, and later Slotin's layouts.

Short answer:

- **n log n is already the bound, and the current tree meets it.** For n
  operations on arbitrary heights, n log n total (Θ(log n) each, amortized)
  is a proven lower bound for online prefix sums and for the search
  operation (`row_at`). It holds in the cell-probe model, so it binds every
  data structure, randomized ones included (Pătraşcu and Demaine, §1.1). Any
  balanced tree with subtree sums, the current one included, already
  achieves O(log n) per operation. No design can do better than log n per
  operation for arbitrary update sequences, so no asymptotic improvement is
  possible in that measure.
- **The room is elsewhere,** and it is real:
  1. *Adaptive bounds.* Costs in terms of the runs r instead of the rows n,
     in terms of the distance d from the last position used (scrolling has
     strong locality), and O(k + log n) for a window of k adjacent rows
     instead of O(k log n).
  2. *Memory per measured row.* About 40-55 bytes today by layout arithmetic
     (§2.3). A packed entry needs 8 bytes and a dense leaf 2-4 bytes, and a
     total-preserving coarsening bounds memory regardless of scroll history.
  3. *Exactness.* Integer (fixed-point) heights make `offset_of` and
     `row_at` exact inverses, provably, and the same on every host. The
     current `double` sums cannot be proved to bracket (§2.4).
  4. *Cache behaviour and constants.* Contiguous page nodes, prefix arrays
     with a branch-free counting search, and no per-node `std::vector`s.

So the precise target is not "n log n". It is: **Θ(log n) worst-case per
operation (already the lower bound), plus O(log r), O(log d) and
O(k + log n) adaptive bounds, exact integer arithmetic, and memory
O(retained runs) at 2-8 bytes per measured row.** Each of these comes with a
proof sketch in §4.

## 1. Operations, models and known bounds

### 1.0 The operations

| Operation | Meaning | Today |
|---|---|---|
| `set_height(row, h)` | point update of one row's height | O(log n) descent + path refresh |
| `insert(row, k)`, `erase(row, k)` | insert or erase a run of rows (positions shift) | O(log n) per touched path |
| `offset_of(row)` | prefix sum of heights before `row` (sum) | O(log n) |
| `row_at(y)` | the row whose extent holds `y` (search, or select by weight) | O(log n) |
| `assign(n, e)` | bulk replace by n rows at the estimate e | O(1) after release (one run) |
| window walk | k adjacent rows with their tops | O(k log n): one descent per row (§2.2) |

Here n is the number of rows, r the number of runs (maximal stretches that
share one height and state), w the word size in bits and δ the bits of an
update value. "Arbitrary heights" means δ = w.

### 1.1 Cell-probe lower bounds (online, dynamic)

- **Fredman and Saks (STOC 1989)** gave Ω(log n / log log n) for partial sums
  and for related dynamic problems in the cell-probe model.
- **Pătraşcu and Demaine, "Logarithmic Lower Bounds in the Cell-Probe Model"**
  (SIAM J. Computing 2006; arXiv cs/0502041; journal form of "Tight Bounds
  for the Partial-Sums Problem", SODA 2004). These statements were re-read
  in the arXiv text:
  - Theorem 1 (δ = Ω(b) for cell size b; amortized; Las Vegas randomized):
    t_q·lg(t_u/t_q) = Ω(lg n), t_u·lg(t_q/t_u) = Ω(lg n), and the same two
    trade-offs with the select (search) time t_s in place of t_q. "The
    trade-off curves are identical for select and sum." So
    max(t_u, t_q) = Ω(lg n) and max(t_u, t_s) = Ω(lg n).
  - Theorem 2 (δ = o(b)): t_q·(lg(b/δ) + lg(t_u/t_q)) = Ω(lg n), and the
    same with t_s. So max(t_u, t_q) = Ω(lg n / lg(b/δ)).
  - Theorem 3: a deterministic RAM structure with worst-case
    t_u = t_q = t_s = O(lg n / lg(b/δ)). This is a matching upper bound, so
    the δ-dependence is tight.
  - The group model alone also needs Ω(lg n) algebraic operations (SODA
    abstract).
- **Insertions and erasures** (positions shift) are at least as hard: the
  static-length problem is a special case. So the lower bounds bind our
  `insert` and `erase` too.

What this means for row heights:

- Heights in `double` or in a 64-bit fixed point are arbitrary w-bit values,
  so δ = w and the bound is Θ(log n) per operation. A balanced B-tree with
  subtree sums meets it, up to the constant.
- The only asymptotic gain the δ-parametrized bound leaves is a factor
  lg(w/δ) for *small* update deltas. For example, a 16-bit delta on 64-bit
  words gives lg(64/16) = 2. That factor is small; the upper bounds that
  reach it (Dietz 1989; Raman, Raman and Rao 2001) use precomputed tables
  that "do not scale well with b or δ" (Pătraşcu-Demaine). We do not
  pursue it.
- With 128-bit vector registers, one probe reads 128 bits. In the
  cell-probe sense that is a bigger cell (b = 128 with δ = 64), which gains
  at most lg(128/64) = 1 in the denominator. SIMD is a constant-factor tool,
  not an asymptotic one.

### 1.2 External-memory and cache bounds

- **B-trees:** O(log_B n) I/Os per search or update, with B items per block.
- **The lower bound for prefix sums in external memory** follows from
  Theorem 2 by treating a block of B words as one cell (b = B·w bits) with
  word-sized updates (δ = w). This gives
  t_q·(lg B + lg(t_u/t_q)) = Ω(lg n), so max(t_u, t_q) = Ω(log_B n) I/Os.
  This derivation is mine. The paper states this exact form (its Theorem 5)
  for dynamic connectivity, not for partial sums.
- **Buffer trees** (Arge, WADS 1995 / Algorithmica 2003) and **B^ε-trees**
  buffer updates.
  - Brodal and Fagerberg (SODA 2003, §3.4, "B-trees with buffers") give a
    B-tree of degree B^ε with an O(1)-block buffer per node: queries take
    O((1/ε)·log_B(N/M)) I/Os, and B updates take amortized
    O((B^ε/ε)·log_B(N/M)) I/Os, i.e. O(log_B(N/M) / (ε·B^(1-ε))) per update.
  - Their lower bounds are for comparison-based dictionaries: if N
    insertions perform at most δN/B I/Os, some query needs
    Ω(log_{δ log² N}(N/M)) I/Os, for δ = O(B / log³ N) and N ≥ M² (their
    Theorem 1.1(3)).
  - Bender et al. (";login:" 2015) give the name and an introduction; the
    bounds above are taken from Brodal and Fagerberg's text.
  - Putting t_u ≈ log_B n / B^(1-ε) into the partial-sums trade-off gives
    t_q = Ω((1/ε)·log_B n), so the B^ε trade-off is on the curve for prefix
    sums too. Buffering pays only when updates outnumber queries by a large
  factor. A list view queries every frame (row_at, offset_of) and measures
  a few rows per frame, so buffering would buy nothing here.
- **Cache-oblivious B-trees** (Bender, Demaine and Farach-Colton, SICOMP
  35(2), 2005, abstract and §1) search in Θ(1 + log_{B+1} N) memory
  transfers at every level of the hierarchy without knowing B.
  - Their first tree updates in O(1 + log_{B+1} N) amortized transfers.
  - Their second scans S consecutive elements in O(1 + S/B) transfers and
    updates in O(1 + log_{B+1} N + (log² N)/B) amortized transfers.
  - A cache-aware B-tree tuned for one level (the 128-byte M-series line, or
  L2) reaches the same bound at that level. On these machines the
  structure's internal levels fit in L2 (§3.5), so the misses that matter
  are at one level only. Cache-obliviousness would add constant factors
  without removing a miss we pay.

**Where we stand.** The current tree's descent visits O(log_B n) nodes, so
it meets the bound in count of nodes. But each node is a heap `Node` plus
three or four separately allocated `std::vector` buffers, so a node visit
touches 3-4 unrelated cache lines (§2.3). The constant per level is the
target, not the exponent.

### 1.3 Run-adaptive (sparsity) bounds

- A tree over runs instead of rows costs Θ(log r) per operation. That is
  the lower bound again, applied to a sequence of r runs (each run is one
  element of an r-element partial-sums instance). The current tree already
  keeps runs, so it is O(log r) in node visits, apart from the run-split
  cost of `set_height`, which is O(1) extra entries.
- **The catch in the chat workload:** r is not small after scrolling.
  Measuring a row in the middle of an estimated run turns one run into up
  to three, and adjacent measured rows merge only when their heights are
  equal. So r grows by up to 2 per measured row, and roughly r ≈ m + 1 for
  m measured rows in a contiguous measured stretch (CHAT_STRESS_SPEC §4.2,
  gap 9.1). Run-adaptivity bounds time well, since log r ≤ log n. It does
  not bound memory unless measured detail can be dropped (§4, C2).
- **Memory lower bound:** storing m arbitrary measured heights takes at
  least the entropy of those heights. For heights quantized to 1/64 unit in
  [1, 10^6] units that is at most 26 bits per height. Chat heights are
  "header + lines × line height + margins", so in practice much less.
  Today's cost is about 40-55 bytes, roughly 320-440 bits per measured row.

### 1.4 Finger and locality bounds

- **Finger search trees** (Huddleston and Mehlhorn, Acta Informatica 17,
  1982, pp. 157-158).
  - Weak B-trees have every interior node with at least a and at most b
    sons, b ≥ 2a, and all leaves at one depth.
  - In level-linked weak B-trees, a finger is a pointer to a leaf; search
    time is logarithmic in the distance from the finger, and fingers are
    established and moved in constant time.
  - Insertions and deletions take constant time on average over a sequence.
- **Splay trees** have the working-set and dynamic-finger properties (Sleator
  and Tarjan 1985; Cole 2000).
- Iacono's working-set structure (SODA 2001) searches in O(log t) for an
  item whose last access was t distinct accesses ago, with O(log n)
  insertions and deletions *(secondary: Wikipedia, "Iacono's working set
  structure"; no publisher or author copy was available)*.
- Splay-style structures restructure on reads. These
  restructure on reads: every query writes, which costs cache lines, rules
  out shared reading and is harder to prove bounded. A finger gives the
  locality the view needs with O(depth) state and no restructuring.
- **The view's locality:**
  - A frame touches a window of k adjacent rows (visible plus overscan,
    about 10-40 for chat rows in a 1280×800 window).
  - A smooth scroll moves the window by d = 0-3 rows per frame.
  - A jump (thumb drag, "go to message") is a fresh random position.
  - Sequential walking with a cursor costs O(1) amortized per step, so a
    window costs O(log n + k) from scratch and O(log_B d + k) from the
    previous frame's finger.
- **Prefix sums under a finger:** updates far from the finger change the
  finger's offset by a known amount. One finger is kept exact in O(1) per
  update: add the delta when the update lies before it.

### 1.5 Batched and amortized bounds

- **Offline:** Pătraşcu and Demaine note that Θ(log n) per operation is best
  possible even offline in the semigroup model (Fredman, JACM 1982). In the
  cell-probe model, the only known offline lower bound is Chazelle's
  Ω(lg lg n) (Discrete Comput. Geom. 1997). So whether an offline batch of
  n arbitrary operations can beat n log n in a cell-probe or word-RAM model
  is open in the literature. Nothing known suggests a practical method, and
  the view's queries are online anyway.
- **Batches with locality are cheap, and these are what we need:**
  - k updates inside one window of adjacent rows cost O(log n + k): descend
    once, edit along the leaves, refresh each touched ancestor once.
  - A bulk load of k sorted entries costs O(k).
  - An append burst of k estimated rows is one run: O(log n). This is
    already true today, because appended rows are estimated.
  - k appended *measured* rows go into the right spine: O(log n + k).
- **Amortized rebalancing:** weak B-trees with b ≥ 2a do rebalancing work
  linear in the length of an insertion and deletion sequence that starts
  from an empty tree, so O(1) amortized per update (Huddleston and Mehlhorn
  1982, p. 158). The number of updates needing k rebalancing steps
  decreases exponentially in k.
  The current tree does not merge underfull nodes. It compacts the whole
  tree when the depth passes a limit, and its "paid for by the growth"
  claim has no written proof (§2.5).

### 1.6 Summary table

| Measure | Known bound | Current tree | Room |
|---|---|---|---|
| Worst case per op, arbitrary heights (cell probe) | Θ(log n) amortized, randomized; sum and search alike | O(log_B n) nodes × O(B) scan = O(log n) | none asymptotically |
| Small update deltas (δ bits) | Θ(log n / log(w/δ)) | — | factor ≤ 2-3 at large cost; not pursued |
| I/O per op (block B) | Θ(log_B n) | log_B n nodes but 3-4 lines per node | constant: 1-2 lines per level |
| Buffered updates | on the trade-off t_q(lg B + lg(t_u/t_q)) = Ω(lg n) | — | none for query-every-frame use |
| Runs | Θ(log r) | O(log r) | memory per run, not time |
| Locality (finger, distance d) | Θ(log d) comparison model | O(log n) per call, no cursor | O(log_B d) moves, O(1) steps |
| Window of k rows | Θ(log n + k) | O(k log n) | real (the view walks per row) |
| Bulk load / append burst | Θ(k) / Θ(log n + k) | O(k) / O(log n) | already met |
| Memory per measured row | entropy of heights (≤ 26 bits at 1/64 unit) | ~40-55 B (estimate, §2.3) | 5-20× (8 B packed, 2-4 B dense) |
| Exactness | exact with integers | `double`, not provable (§2.4) | provable inverse, host-independent |

## 2. The current code (GUI.Forms, integration `claude/batch-cp29-2b-b`)

### 2.1 Shape

- `src/core/collections/order_statistic_tree.hpp`:
  `detail::OrderStatisticTree<Traits>`.
  - Node capacity is 32, the bulk fill 24 and the split fill 16.
  - Each `Node` holds `parent`, `leaf`, `count`, `summary` and four
    `std::vector`s: `entries`, `children`, `child_counts` and
    `child_summaries`.
  - Totals are recomputed from the children along every changed path, with
    no running deltas, so floating sums do not drift.
  - No merging. Emptied nodes are removed, and a depth limit triggers a
    full compaction.
- `src/controls/collections/row_height_index.hpp/.cpp`: `RowHeightIndex`.
  - Entries are `RowRun {size_t count; double height; bool measured;}`,
    24 bytes.
  - The summary is the subtree's total height as a `double`.
  - `offset_of` and `row_at` descend with linear, branchy scans of the
    child arrays.
  - Inside a run, `row_at` uses `floor(remaining / height)`.
- `src/core/binding/binding_view/binding_order_tree.hpp`: the other client.
  It holds 32-bit elements, one row each, with leaf tracking (`leaf_of_`)
  so a rank starts at the element. It is a different problem (membership
  plus rank by identity) and stays in GUI.Forms (§3.6).
- Users: `VirtualRowsView` (`src/controls/virtual_rows_view/`) and
  `TreeListView`.

### 2.2 How the view uses the index (the access pattern to optimize)

From `virtual_rows_view.cpp`:

- `measure_band`: for each row in the band, `offset_of(row)`,
  `measured(row)`, `height_of(row)` (inside `measure_row`) and
  `set_height(row, h)`. That is four root-to-leaf descents per row.
- `update_realized`: `while (end < count && offset_of(end) < bottom) ++end;`,
  one descent per visible row.
- `capture_anchor`, `anchored_offset` and `finish_scroll`: one to three
  descents each.

So a settle costs about 5k descents for a window of k rows. With a cursor
(§4, C3) this becomes 1-2 descents plus O(k) leaf steps. At depth 6 with
about 11 child slots scanned per level on average, k = 30 gives about
30 × 5 × 6 × 11 ≈ 10^4 slot reads per settle, against about 10^2. This is
arithmetic, not a measurement. Text shaping probably dominates the frame,
so the time share is to be measured (§5), not assumed.

### 2.3 Memory per run (layout arithmetic, to be measured in §5)

- `Node`: 8 (parent) + 8 (`bool`, padded) + 8 + 8 + 4 × 24 (vectors) =
  128 bytes, plus a malloc header and rounding.
- A leaf's runs live in a separate heap block of 24 bytes × capacity:
  - A split assigns 16 entries to the new sibling (capacity 16), while the
    old half keeps its capacity (≥ 33, typically 48-64 after vector
    doubling).
  - A leaf therefore has roughly 1.3-2× the capacity it uses: about 30-48
    bytes per run, plus 128/~22 ≈ 6 bytes of node per run, plus the
    branches (three heap blocks each).
- Estimate: **about 40-55 bytes per run, 400-550 MB at 10M runs.** CHAT
  §4.2 counted the 24-byte runs alone (240 MB). Either figure fails the
  280 MB budget after a full traversal.
- Cache lines per level of descent: the `Node` itself, plus `child_counts`
  or `child_summaries`, plus `children`. That is three unrelated lines, four
  for `offset_of`, which reads both arrays.

### 2.4 Exactness

- `offset_of` sums child summaries along a path. `row_at` subtracts them
  from the target, then divides inside a run. Each node's summary is a
  rounded sum computed in its own order. Floating addition is not
  associative, so for a given `y`, the bracketing property
  `offset_of(row_at(y)) ≤ y < offset_of(row_at(y) + 1)` (CHAT §7.8) is not
  implied by the code. It holds only up to an error bound nobody has stated.
  It may hold in all tests and still fail near a row boundary at large
  offsets.
- **Host-dependence:** `height += count * height` contracts into an FMA on
  AArch64 if contraction is on. GUI.Forms compiles every target with
  `-ffp-contract=off` (its `CMakeLists.txt`, around line 6129), so it is
  safe there. But stx_btree is header-only: stx_auxlib's private
  `-ffp-contract=off` "never reaches consumers" (stx_auxlib
  `CMakeLists.txt` line 69). A `double` index in stx_btree would take each
  consumer's flags, and Clang's default fuses within a statement. Integers
  remove the question.

### 2.5 Smaller findings (for the record, not defects found by running)

- The documentation claims O(log n), and in node visits it is. Each visit
  scans up to 32 slots linearly, and `index_in_parent` scans the parent
  linearly at every level of `refresh_upward` and the splits. RAM cost is
  O(B log_B n), as for any B-tree without in-node search, which is fine
  asymptotically.
- Equal adjacent runs merge only within a leaf, so runs across a leaf
  boundary are not canonical. `run_count` counts entries, not maximal runs.
- **Compaction amortization:** the claim is that the compaction "is paid for
  by the growth or the erasures that made the tree that deep". There is no
  potential-function proof. A quick count is not a proof either way: after a
  compaction at fill 24, raising the depth by repeated insertion at one
  place takes about 16^(depth-1) insertions, while the compaction costs
  O(entries) ≈ 24^depth. That suggests a charge per insertion that can grow
  like (24/16)^depth: about 11 at depth 6, bounded because the depth is at
  most 17, but not the O(1) the comment implies. Standard (a,b) merging
  (bbtree already rebalances removals, `detail/tree_algorithms.hpp`) has
  textbook proofs and makes compaction unnecessary.
- `run_count()` walks every leaf, O(r). It is called for statistics only.

## 3. Integration design for stx_btree

### 3.1 It is a sequence tree, not a map

bbtree is a keyed map: separators are key windows and the node search counts
windows below a query. Row positions and offsets are implicit keys: they
shift on every insertion. So the order-statistic layer is a separate tree
kind in stx_btree. It shares bbtree's machinery but not its key layout:

| bbtree piece | Reused as |
|---|---|
| `GlobalPageAllocator`, `PooledPageAllocator` | node pages, size-aligned, pooled (one allocation per node, not five) |
| `TreeShape`-style compile-time layout with `static_assert`s | the sequence node layout |
| `InlineKeyCount<Slot, Capacity>` (count slots below/equal over every slot, padding all ones) | the in-node search over prefix arrays (§3.3) |
| node stacks (D24), no parent pointers | the cursor's path (§3.4) |
| reserve every page before changing anything (Failure) | the same failure rule |
| split at a node's end leaves the arriving side `min(2·minimum, capacity/2)` (D32) | full leaves under appends |
| rebalancing removal (D22, D26) | merge/borrow on erase, replacing compaction |
| tagged node pointers (kind in bit 0) | the leaf/branch distinction |

### 3.2 Values: fixed-point heights and packed runs

- **Height unit:** a signed 64-bit count of 1/64 logical units. This is 26.6
  fixed point, FreeType's own format, so heights that come from the text
  engine's metrics convert exactly.
  - Range: the view clamps heights to [1, 10^6] units, i.e. at most 6.4·10^7
    fixed units per row. A billion rows at the maximum is 6.4·10^16, below
    2^62 ≈ 4.6·10^18, with room for the comparison trick in §3.3.
  - Totals are kept below 2^62 by construction: an edit that would exceed
    it is refused with the index unchanged (checked sums, as stx_auxlib's
    `src/checked.hpp`).
- **A run is (count, total, state), not (count, height, state).**
  - Estimated and measured runs store the run's total.
  - A run of k rows with total S gives row i inside it the height
    q + [i < s], where q = S div k and s = S mod k.
  - A measured row is a run with k = 1. An estimated run is (k, k·e).
  - A *coarsened* run (§4, C2) is any (k, S): the exact total of rows whose
    individual heights were dropped.
- **Packed entry, 8 bytes:** count 24 bits, total 38 bits, state 2 bits
  (estimated, measured, approximate).
  - A run longer than 2^24 - 1 rows, or with a total of 2^38 or more, is
    stored as several entries. Splitting is exact (Lemma S, §8.2), so this
    is invisible to the API.
  - 10M rows at an estimate of 24 logical units is 1.5·10^10 fixed units,
    below 2^38 ≈ 2.7·10^11: one entry. Splitting is the cost of the packing; it is O(1) entries per
    10^7 rows.
- **Dense leaf (optional, C4):** a leaf that is all measured single rows
  stores only heights: 4 bytes each (26.6, 32 bits), or 2 bytes as offsets
  from a leaf base when the leaf's heights fit. A leaf converts to the
  packed form when a run arrives.

### 3.3 Node layout and in-node search

- **Branch (512 B or 1 KiB page):** a header (length, level), then
  `count_prefix[c]` and `height_prefix[c]` as 64-bit integers (each the sum
  over children 0..j), then edges.
  - At 512 B with 16 B of header, (512 - 16) / 24 = 20 children. At 1 KiB,
    42 children. Measure both (§5).
  - Slots past the length hold all ones, as in bbtree.
- **Search by row or by offset:** child = number of prefix slots ≤ target.
  - This is `InlineKeyCount::equal_range(...)` with `begin + equal`, over
    `std::uint64_t` lanes, two per 16-byte group.
  - Count prefixes strictly increase (every child holds at least one row)
    and height prefixes do not decrease (heights ≥ 0), so the count is the
    child index. Proof in §4, C1.
  - The work is branch-free: about 10 compare-and-subtract groups for 20
    slots, against a data-dependent branchy scan averaging 10 compares.
- **Portability of 64-bit lanes:**
  - NEON has 64-bit unsigned compares (`cmhi`/`cmhs` on `.2d`).
  - Baseline SSE2 has no 64-bit compare (`pcmpgtq` is SSE4.2).
  - Because every live prefix is below 2^62 and padding is all ones, write
    the test as the sign of a subtraction:
    `count += ((target - slot) >> 63) ^ 1` per lane, with unsigned lanes and
    the padding handled by the clip to length. This compiles from
    vector-extension source on both targets, with no intrinsics, following
    the repository's rule against platform intrinsic forks.
- **Updates to prefix arrays:**
  - A height change of Δ at child j adds Δ to `height_prefix[j..len)`; an
    insertion of k rows adds k to `count_prefix[j..len)`.
  - Per level that is a masked vector add over the node, Slotin's
    wide-segment-tree update (algorithmica, "Segment Trees", which stores
    prefix sums in 16-wide nodes and adds a masked broadcast on update).
  - With integers, adding deltas gives exactly the recomputed sums, so the
    "recompute, never accumulate" rule of the current tree becomes
    unnecessary. Proof in C1.
- **The alternative to measure:** per-child sums (not prefixes) make an
  update one slot per level, but each search must prefix-scan in registers.
  The view reads far more than it writes, which favours prefixes. §5, E3
  decides.
- **Leaf (1 KiB):** header plus up to 126 packed entries.
  - Searching 126 entries linearly is the slow part of a descent. Two
    options to measure: group totals in the leaf header (8 groups of 16
    entries, a two-level count inside the leaf), or 512 B leaves.
- **Eytzinger and S-tree:** Slotin's Eytzinger and S-tree layouts
  (algorithmica, "Binary Search" and "Static B-Trees") are static. A node
  of 20-42 slots fits in a few 128-byte lines, so within a node the whole
  slot array is read in one pass. A layout inside a node gains nothing a
  full-width count does not. Across nodes, the dynamic tree cannot keep an
  implicit layout. So these layouts are not used. The one transferable idea
  is the S-tree's B+ shape with prefix-array nodes, which §3.3 is.

### 3.4 Cursors (fingers)

- A cursor is a node stack (bbtree D24, at most `maximum_depth + 1` levels).
  Each level keeps the slot index, the row and offset at the start of that
  node, and the leaf position.
- **Steps and moves:**
  - `next()` and `previous()` move along the leaf, climbing only at leaf
    ends: O(1) amortized over a walk.
  - `seek_row(target)` and `seek_offset(target)` from a cursor climb until
    the target lies in the current node's span, then descend: O(log_B d)
    node visits for a target d rows away.
- **Edits through a cursor** (`set_height`, `insert`, `erase` at the cursor)
  update the stack's nodes in place and keep the cursor valid.
- **Invalidation:**
  - The index keeps a structural revision, advanced by any split, merge or
    page change.
  - A cursor records it. Using a cursor whose revision is stale re-seeks
    from the root by its row, O(log n). This is a defined fallback, not
    undefined behaviour (house style section 4: reject stale identities).
  - Edits that do not change structure (a height change at a leaf with
    room) leave other cursors' rows valid, but their cached offsets may be
    stale. So cursor offsets are re-derived on use whenever the *value*
    revision moved.
  - The view needs one live cursor, so this cost is paid rarely.

### 3.5 Size at 10M rows (arithmetic, for the experiment to confirm)

All rows measured, packed entries, 1 KiB leaves:

- 126 entries per leaf at ln 2 ≈ 69% average fill under random splits gives
  ≈ 87 per leaf: 1024 / 87 ≈ 11.8 B per row, about 118 MB. Append-ordered
  fills with end-splits approach 8 B per row, about 80 MB.
- Branches at fanout ~20 add about 1/87 × 512 / 20 bytes per row, which is
  negligible. The internal levels (~115k leaves → ~6k branches ≈ 3 MB) fit
  in the M4's L2, so a random descent costs about one missed leaf line plus
  at most one branch line.
- Dense 4-byte leaves: about 6 B per row (60 MB). 2-byte leaves: about 3 B
  per row (30 MB).
- With a coarsening budget of R retained runs (C2): O(R) regardless of n
  and of scroll history.

### 3.6 What RowHeightIndex becomes, and what GUI.Forms keeps

(First sketch. The API under the owner's decisions is §8.1, which
supersedes this block where they differ.)

In stx_btree (namespace `stx_btree`, header-only like the rest):

```text
detail::SequenceTree<Traits>       pages, prefix-array branches, splits,
                                   merges, cursors; Traits declares the leaf
                                   entry and its (count, total) projection
HeightUnits {std::int64_t value}   1/64 logical unit (26.6)
RowHeightIndex                     concrete client:
  RowHeightIndex(HeightUnits estimate, Allocator)
  assign(rows, estimate)                         O(1) after release
  count(), total()                               O(1)
  offset_of(row) -> HeightUnits                  exact prefix sum
  row_at(HeightUnits) -> row                     exact inverse (C1)
  extent_of(row) -> RowExtent{top, height, state}  one descent replaces
                                                 offset_of + height_of + measured
  set_height(row, h) -> bool                     refused (unchanged) if invalid
  set_heights(row, span<const HeightUnits>)      window batch, O(log n + k)
  insert(row, k), erase(row, k)                  estimated rows; O(log n) per path
  forget(row, k)                                 back to the estimate
  coarsen(row, k)                                keep the total, drop detail (C2)
  seek_row(row), seek_offset(h) -> Cursor        O(log n); from a cursor O(log_B d)
  Cursor: row(), top(), height(), state(), next(), previous(), set_height(h)
  statistics: node_count(), run_count() (maintained, O(1)), depth(), page_bytes()
```

- **Failure:** expected alternatives are return values. The only exception
  is `std::bad_alloc` from the page allocator, thrown before any change,
  since pages are reserved first (bbtree's Failure rule).
- **Overflow:** a total that would pass 2^62 is refused with the index
  unchanged.
- **Execution:** one thread, as today. No shared variant is planned; the
  latched tree's design does not transfer to a summed tree without a
  separate study.

GUI.Forms keeps:

- The conversion between `double` logical units and `HeightUnits`, with its
  rounding rule (round to nearest 1/64; the view clamps first).
- The view policies: anchoring, overscan, follow-tail, when to `forget`,
  when to `coarsen` and with what budget, and width-change reassignment.
- The counters, and both views as clients.
- `BindingOrderTree` on its own `OrderStatisticTree`: its problem is rank by
  element identity with leaf tracking, not prefix sums. It could later
  become a second `SequenceTree` client with a tracked-leaf entry type, but
  nothing here requires that.

Dependency direction (owner, 2026-11-12): GUI.Forms depends on stx_auxlib's
btree module for the sequence tree and `RowHeightIndex`. stx_auxlib is
worked on from the Mac side, so no cross-desk routing is needed; an earlier
draft of this study said otherwise and was wrong.

## 4. Candidates, ranked by expected gain for the chat workload

"Expected" is a prediction to be checked in §5, not a result. Each candidate
states the invariants it needs and a proof sketch. Correctness is to be
proved ("prove, don't sample"). Exhaustive small-scope checks (§5, E0)
support the proofs and do not replace them.

### C1. Exact fixed-point sums with prefix-array branches (enabler)

- **Gain:** a provable `row_at`/`offset_of` inverse and bracketing (CHAT
  §7.8), host-independent results, incremental delta updates, 64-bit
  integer lanes for the branch-free count, and smaller entries.
  Speed: neutral to positive (§5, E2).
- **Invariants:**
  - I1. All leaves are at one depth.
  - I2. In every branch, `count_prefix[j] = Σ_{i≤j} count(child i)` and
    `height_prefix[j] = Σ_{i≤j} total(child i)`, exactly. Slots ≥ length
    hold 2^64 - 1.
  - I3. Every entry has count ≥ 1 and total ≥ 0. Every subtree's count is
    ≥ 1.
  - I4. The root's total is < 2^62.
- **Specification:** `offset_of(r) = Σ_{i<r} h(i)`, and for 0 ≤ y < total,
  `row_at(y) = min{r : offset_of(r + 1) > y}`. When every height is > 0
  this is the unique r with `offset_of(r) ≤ y < offset_of(r + 1)`. Rows of
  height 0 are never returned for y < total, and the tie rule is explicit.
- **Proof sketch (search):**
  - By induction on height. In a branch, the child containing local target
    y' is the least j with `height_prefix[j] > y'`. Because the prefixes do
    not decrease (I2, I3), that j equals `#{i < len : height_prefix[i] ≤ y'}`,
    which is what the count computes. Padding is excluded because
    y' < 2^62 < 2^64 - 1 and the count is clipped to the length.
  - Descending with y' − `height_prefix[j-1]` preserves the specification
    in the child's frame, because integer subtraction is exact.
  - In a leaf, the same argument over entry totals, then the closed-form
    inverse inside a run (C2's div/mod) settles the row.
  - `offset_of` is the same descent summing prefixes, exact by
    associativity of integer addition. So
    `offset_of(row_at(y)) ≤ y < offset_of(row_at(y) + 1)` holds whenever
    the row has positive height.
- **Proof sketch (updates):**
  - Adding Δ to `height_prefix[j..len)` on each ancestor preserves I2,
    because exactly the sums containing the changed child change by Δ.
  - A split or merge rebuilds the two nodes' prefix arrays from their
    children's totals and replaces one parent slot with two (or two with
    one), keeping the parent's other sums.
  - I4 is checked before mutating.
- **Cost:** O(log_B n) nodes, each O(B / lanes) vector work. This meets
  Θ(log n).

### C2. Total-preserving runs and a coarsening budget (memory; closes gap 9.1)

- **Gain:** memory O(R) for a chosen budget of R runs, whatever the
  scrolling history, and **no anchor movement** when detail is dropped:
  offsets outside the coarsened range are unchanged bit for bit. Today the
  only bounded-memory option is to forget measured rows, which reverts them
  to the estimate and moves offsets (the anchor rule then corrects, but the
  scroll bar jumps).
- **Operation:** `coarsen(row, k)` replaces the entries covering rows
  [row, row + k) by one run (k, S), S being the exact sum. Edge entries are
  split first. The state becomes "approximate": a row shown again is
  re-measured.
  - Policy (GUI.Forms): when `run_count() > R`, coarsen the measured
    stretch farthest from the anchor.
- **Invariant and proof:**
  - `offset_of(x)` for x ≤ row or x ≥ row + k is a sum of whole entries
    before x plus the replaced range's total S. That total is unchanged
    (the entries' sum equals S by construction), so the offset is unchanged.
  - Inside the range, `offset_of(row + i) = offset_of(row) + i·q + min(i, s)`
    with q = S div k and s = S mod k. This is monotone and
    exactly inverted by: if y' < s(q+1) then i = y' div (q+1), else
    i = s + (y' − s(q+1)) div q (for q > 0; if q = 0 the first s rows have
    height 1 unit and the rest 0, handled by the tie rule).
  - Memory: after the policy runs, entries ≤ R + O(1) per coarsening step,
    so memory is O(R).
- **Cost:** O(log n + entries replaced). The replaced entries are freed, so
  this is amortized O(log n) against the `set_height` calls that created
  them.

### C3. Cursors and window batches (locality)

- **Gain:** a settle and paint go from about 5k descents to 1-2 descents
  plus O(k) leaf steps (§2.2). A smooth-scroll frame reuses the previous
  frame's cursor: O(log_B d) with d ≤ a few rows, so typically no climb
  above the leaf.
  - Expected time effect: small in absolute terms (the index is microseconds
    against a 16.7 ms frame), large in proportion to index work. §5, E4
    measures it.
- **Invariants:**
  - The cursor stack is a root-to-leaf path with, at each level, the slot
    index and the exact row and offset of the slot's start. The leaf
    position also holds the row and offset inside the entry.
  - Validity: the structural revision is equal to the one recorded.
- **Proof sketch:**
  - `next()` adds the current row's height (exact) and increments the
    within-entry index. At an entry's end it moves to the next entry. At a
    leaf's end it pops to the first level with a right sibling and
    descends leftmost, adding the passed prefixes.
  - Amortized O(1) per step over a walk of k steps by the standard argument:
    each level's slot advances at most k / B_level times.
  - An edit through the cursor changes only the cursor's path, so the
    stack's starts stay exact. A split or merge advances the revision and
    the cursor re-seeks, so a stale cursor is never trusted.
- **Window batch:** `set_heights(row, span)` walks one cursor across the
  window, accumulates a delta per touched leaf and adds it once per
  ancestor. That is O(log n + k), proved as C1's update proof applied once
  per leaf.

### C4. Page nodes, packed entries and dense leaves (constants and memory)

- **Gain:** about 40-55 B down to 8-12 B per measured row with packed
  entries (2-6 B with dense leaves). That is 4-20× less memory, before C2's
  bound. One allocation per node instead of 2-5, and 1-2 cache lines per
  branch level instead of 3-4.
- **Invariants:**
  - Packed fields are within their widths (count < 2^24, total < 2^39).
  - A dense leaf holds only measured single rows.
  - Conversions preserve the row sequence and every total. Proof: packing
    is a bijection on in-range values, and splitting an over-range run uses
    C2's exact div/mod partition.
- **Cost:** none asymptotically. Entry decode is a shift and a mask.

### C5. Branch-free SIMD counting in nodes (constants)

- **Gain:** bbtree measured its counting search against branchy scans
  (its D56 and the measurements behind stx_btree's map). For prefix arrays
  the expected gain is fewer mispredictions on random jumps. Smooth
  scrolling is predictable either way, so expect most gain on the jump
  workload (E5).
- **Proof:** C1's search proof covers it. The count is the same function
  computed in vector lanes, and the sign-of-difference test is exact
  because all live values are < 2^62.

### C6. (a,b) rebalancing instead of depth-triggered compaction (bounds)

- **Gain:** a provable O(1) amortized structural change per update, node
  count O(r / a), and no O(r) compaction pauses (a pause at 10M runs would
  be a frame hitch, though rare).
- **Proof:** Huddleston and Mehlhorn's weak B-tree analysis (b ≥ 2a: total
  rebalancing linear in the number of updates from an empty tree, p. 158).
  The prototype keeps b ≥ 2a at both node kinds (`static_assert`s in
  `sequence_tree.hpp`). bbtree's rebalancing removal is the implementation
  pattern.

### C7. Per-row estimates from an estimate source (accuracy; owner decision 5)

- **Gain:** a better scroll bar and fewer anchor corrections, at no memory
  per row. Today a per-row estimate would make every row its own run.
- **Design:** an unmeasured run stores (count, source_start). Its total is
  E(source_start + count) − E(source_start), where E is a monotone,
  integer-valued prefix of estimates indexed by the client's stable
  identity (for chat, a block table of estimated prefix sums every 4096
  messages with linear interpolation inside a block). Inserted rows get
  fresh identity ranges.
  - The client supplies E through a named capability (an interface object
    with explicit context, house style section 5), not a callback lambda.
- **Proof obligations:**
  - E monotone gives nonnegative totals (I3).
  - Totals are exact integers.
  - `row_at` inside such a run needs E's inverse, either supplied or found
    by binary search over E: O(log k) extra, still O(log n).
  - Totals must be refreshed if E changes, so E must be immutable for an
    index's life, or changing it is an `assign`.

### Not pursued, with reasons

- Fenwick trees: the owner's direction, and positions shift on insert,
  which a Fenwick tree does not support without rebuilding.
- Splay or working-set trees: writes on reads (§1.4).
- B^ε or buffer trees: queries every frame (§1.2).
- Cache-oblivious layouts: one cache level matters (§1.2).
- Table-driven small-delta structures: at most a factor of 2-3, at large
  complexity (§1.1).
- Ultra-wide word-RAM structures (Bille, Gørtz and Skjoldjensen, TAMC
  2020): a model with w²-bit words that our hardware does not have.

### Precedent

Zed's `sum_tree` crate is a B-tree whose internal nodes keep summaries
with several seekable dimensions, plus cursors. It is the same design family
as `SequenceTree` with cursors (seen in its source documentation at
git.secluded.site/zed `crates/sum_tree`). It is cited as design precedent
only, and none of its code is used.

## 5. Experiment plan (for the integrator, on the M4)

The harness is in `btree/research/order_statistic/` (§8.3), a standalone
CMake project that is not part of the package build. The baseline is
GUI.Forms' current `RowHeightIndex` and `OrderStatisticTree`, compiled from
the integration tree by path, for the experiment only: stx_auxlib's
package must not depend on GUI.Forms. Nothing has been compiled or run
yet.

### 5.1 Variants

| Id | Variant |
|---|---|
| Id | Index | Driver | Variant |
|---|---|---|---|
| B0 | GUI.Forms `RowHeightIndex` | per row | as is (`double`, vectors, linear scans) |
| P1 | `TreeConfig<21, 127, false, false>` | per row | C1 + C4 packed entries: integer prefix-array branches (512 B), 1 KiB leaves, linear in-node scans |
| P2 | `TreeConfig<21, 127, true, false>` | per row | P1 with C5 counting search in branches (leaves stay linear; leaf group totals are a later variant) |
| P3 | `TreeConfig<42, 127, true, false>` | per row | P2 with 1 KiB branches (42 children) |
| P4 | `TreeConfig<21, 127, true, false>` | cursor | P2 with C3 cursors and window batches |
| P5 | `TreeConfig<21, 127, true, true>` | cursor | P4 with dense 4-byte leaves |
| P6 | as P5 | cursor | P5 with the C2 coarsening budget R (`--budget`, default 10^5; R's default is chosen from 10^4, 10^5, 10^6) |

"Per row" is the call pattern `VirtualRowsView` uses today (§2.2); "cursor"
is one seek per walk, steps, and one `set_heights` per stretch of
unmeasured rows.

### 5.2 Workloads

N ∈ {10^5, 10^6, 10^7}. Seeded. Heights come from the chat generator's
shape: 40 units of header for 1 row in 4, plus lines × 18 units, where
lines = ⌈length × 7.5 / 1180⌉ and length is uniform in 100-1000 from a hash
of (seed, row). The driver emulates `VirtualRowsView`: a settle on a
1280×800 viewport with half a viewport of overscan on each side, measuring
unmeasured rows in the band.

| Id | Workload | Emulates |
|---|---|---|
| W1 | `assign(N, e)`, then a settle at row 0 and at N/2 (e = 89.5 units, the generator's mean) | startup (CHAT 7.1) |
| W2 | 600 frames of fling scroll (40 units/frame, decaying 5% a frame, repeated) from N/2 | 7.4b |
| W3 | 600 frames of one wheel notch (3 × e) per frame | 7.4a |
| W4 | PageDown 3600 times | 7.4c (r growth) |
| W5 | a full sequential traversal measuring every row | worst-case memory (gap 9.1) |
| W6 | 1000 random `offset_of` jumps plus 100 thumb positions (`row_at(total × u)`), each followed by a settle | 7.5 |
| W7 | 30 bursts of 10^4 estimated rows with follow-tail, then 600 frames of 10 measured rows appended per frame | 7.6 |
| W8 | 1000 `set_height` on random rows, and 1000 `erase(row, k)` with k ∈ [1, 10^4], a settle every 10 | 7.7 |
| W9 | a width change (`assign`) plus re-measuring the band, 100 times | resize |

### 5.3 Metrics

- ns per settle and per index operation: median and worst of 7
  fresh-process runs (CHAT §7 rules).
- Index operation counts from a counting build: descents, node visits, slots
  read, splits, merges, coarsenings.
- Memory: the index's own `page_bytes()` / `node_count()` / `run_count()`;
  on macOS `malloc_zone_statistics(nullptr, ...)` (size in use) and
  `task_info(TASK_VM_INFO).phys_footprint`; on Linux `mallinfo2()`.
  Reported as bytes per measured row after W4 and W5.
- Cache misses:
  - On the RunPod x86-64 (memory `x86-testing-hosts`), `perf stat -e
    cycles,instructions,cache-misses,L1-dcache-load-misses,branch-misses`.
  - On the M4, hardware counters need the kpc interface and root. That is
    for the integrator to enable per the instrumentation-host settings
    (memory `m4-instrumentation-host`); otherwise the M4 records time and
    bytes only, plus the computed lines-touched count from the counting
    build.
- Correctness riding with every run: after each workload, 10^4 seeded `y`
  must satisfy the bracketing property, and P-variants must agree exactly
  with a flat `std::vector<int64_t>` model at N = 10^5.

### 5.4 E0: exhaustive small-scope check (proof support)

`order_statistic_exhaustive` (§8.3) runs four configurations at the
smallest capacities the tree admits, so splits, merges, root growth and
collapse, and dense and packed leaves all occur within a few operations:

- branches of 4 slots with leaves of 3 words, linear search;
- the same with counting search;
- 5 slots with 4 words, counting, no dense leaves (the odd-slot tail path);
- the counting form with a total limit of 2^33 units and a row limit of
  200 rows, so refusals occur.

From five start states (empty; 4 estimated rows; 9 measured rows; 30 rows
with every other measured; 60 measured rows with a coarsened stretch and an
insertion), every sequence of operations up to depth 3 (the first three
states) or 2 (the larger two) is applied to the tree and to a flat model.
`--scope small` uses depths 2 and 1 for the sanitizer run.

The operations offered on n rows:

- `set_height` at every row, with nine heights: 0, 1, 3, 64, 2^32 - 1,
  2^32, 2^38 - 1, and the refused -1 and 2^38;
- `set_heights` of 1-3 rows at every row, out-of-range runs included;
- `insert` of 1, 2 or 4 rows at every position 0..n;
- `erase` of 1, 2 or 5 rows (clamped), `forget` of 1 or 3, `coarsen` of
  1, 2 or 4, at every row;
- `assign` of 0 or 3 rows.

That is 23n + 5 operations at each step.

After every operation, the tree must match the model:

- every invariant (§8.2), and the returned results (refusals included);
- `count`, `total`, and `extent_of` and `offset_of` at every row and past
  the end;
- `row_at` at every row boundary, one unit either side, below zero and
  past the total;
- a full cursor walk;
- `seek_offset` and `seek_offset_from` (from the middle row) at the same
  offsets.

Named cases cover the packed-field limits, which small rows cannot reach
(L = 2^24 - 1, the packed-count limit). In both long-run cases, row L is
measured at 7 and then `coarsen(10, L)` runs. Each records `offset_of`
outside the range before coarsening and checks it bit for bit afterwards,
along with the total and Lemma S inside the range:

- "long run, interior coarsen": L + 40 rows, so the range [10, L + 10)
  lies inside and crosses the entry boundary.
- "long run, coarsen clamped at the end": L + 5 rows, so the range clamps
  to [10, L + 5).

The first M4 run failed this second case. The fault was in the check, not
the tree: the check expected the range to end at 10 + L, past the end. The
tree had kept offset(10) and the total exactly, as the invariant requires.

A third case is a run of 300 rows of 2^30 units, split by total. The check enumerates every case of that size; it is complete for
that size rather than sampled, and the general case rests on the proofs
(§4, §8.2).

### 5.5 Commands (the integrator runs them; nothing here was run)

The research worktree is not one of `m4ws`'s workspaces, so it is copied
with the same rsync options `m4ws` uses. The B0 baseline compiles
`row_height_index.cpp` from the M4 mirror of the integration tree, which
must be synced at the commit recorded with the results (`m4ws integration
true` syncs it without building). The four steps run one after another, and
nothing else may measure on the M4 during step 4 (AGENTS.md: trials
sequential).

```sh
# 1. Sync the research worktree.
/usr/bin/rsync -rl --checksum --no-times --delete --omit-dir-times --exclude='/build*/' \
  ~/Developer/gui_forms_workers/research/stx_auxlib/ \
  m4mini:Developer/Projects/gui_forms_workers/research/stx_auxlib/

# 2. Sanitizer build: exhaustive (small scope) and every variant/workload, checked, at 10^5 rows.
ssh m4mini 'set -e; export PATH=/opt/homebrew/opt/llvm@22/bin:/opt/homebrew/bin:/usr/bin:/bin; \
  cd ~/Developer/Projects/gui_forms_workers/research/stx_auxlib; \
  cmake -S btree/research/order_statistic -B build/os-sanitize -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_CXX_COMPILER=clang++ -DORDER_STATISTIC_SANITIZE=ON \
    -DGUI_FORMS_ROOT=$HOME/Developer/Projects/gui_forms_workers/integration/gui_forms; \
  cmake --build build/os-sanitize -j 8; \
  export ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1; \
  build/os-sanitize/order_statistic_exhaustive --scope small; \
  for v in B0 P1 P2 P3 P4 P5 P6; do for w in W1 W2 W3 W4 W5 W6 W7 W8 W9; do \
    build/os-sanitize/order_statistic_bench --variant $v --workload $w --rows 100000 --check; \
  done; done'

# 3. Release build and the full-scope exhaustive check.
ssh m4mini 'set -e; export PATH=/opt/homebrew/opt/llvm@22/bin:/opt/homebrew/bin:/usr/bin:/bin; \
  cd ~/Developer/Projects/gui_forms_workers/research/stx_auxlib; \
  cmake -S btree/research/order_statistic -B build/os-release -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_CXX_COMPILER=clang++ \
    -DGUI_FORMS_ROOT=$HOME/Developer/Projects/gui_forms_workers/integration/gui_forms; \
  cmake --build build/os-release -j 8; \
  build/os-release/order_statistic_exhaustive --scope full'

# 4. Timed runs: 7 fresh processes per variant and workload at 10^7 rows (plus
#    checked and counting passes), then the budget sweep that picks R's default.
ssh m4mini 'set -e; cd ~/Developer/Projects/gui_forms_workers/research/stx_auxlib; \
  sh btree/research/order_statistic/run_bench.sh build/os-release build/os-results-1e7.csv 10000000 7; \
  sh btree/research/order_statistic/run_bench.sh build/os-release build/os-results-1e6.csv 1000000 7; \
  for r in 10000 100000 1000000; do for run in 1 2 3 4 5 6 7; do for w in W4 W5; do \
    build/os-release/order_statistic_bench --variant P6 --workload $w --rows 10000000 --budget $r --run $run \
      >> build/os-results-budget.csv; \
  done; done; done'
```

- Outputs: `build/os-results-*.csv` (timed), `*.csv.check` (bracketing
  violations and invariants: P-variants must show 0 and `ok`; B0's
  violations are recorded as evidence for §2.4), `*.csv.counting`
  (descents and node visits for the P-variants).
- Every bench process exits non-zero if a checked P-variant breaks an
  invariant or the bracketing property, so `set -e` stops step 2 at the
  first defect.
- The RunPod (x86-64 Linux) repeats steps 2-4 with that host's Clang and
  GCC, with `perf stat -e cycles,instructions,cache-misses,L1-dcache-load-misses,branch-misses`
  around chosen bench invocations.

## 6. Owner decisions (2026-11-12) and open questions

Decided:

1. **Target:** adaptive bounds (runs, finger, window), memory per row,
   exactness, cache and constants. Worst-case Θ(log n) per operation is
   already at the lower bound.
2. **Heights** are exact 26.6 fixed point inside the index; doubles appear
   only at the API edge (GUI.Forms converts).
3. **Placement:** a generic sequence tree plus `RowHeightIndex` go into
   stx_auxlib's btree module, and GUI.Forms depends on it.
4. **Memory:** both packed and dense leaves (C4), and `coarsen()` under a
   run budget R (C2), with offsets outside coarsened ranges unchanged. R's
   default is chosen by experiment (§5.5 step 4): 10^4 (§9.3).

Open:

5. **Per-row estimates (CHAT decision 5):** C7's estimate source, or one
   estimate? Not yet put to the owner.

## 7. Track state (resume here)

- **2026-11-12, first pass:** read the GUI.Forms code (integration
  `claude/batch-cp29-2b-b`), CHAT_STRESS_SPEC §4.2, §7 and §9, the backlog
  entry, and stx_btree's DESIGN.md, OPTIMAL_TREES.md and node search.
  Checked the cell-probe bounds against the Pătraşcu-Demaine text and
  Slotin's segment-tree article. Wrote §0-§5.
- **2026-11-12, second pass:**
  - Recorded the owner's decisions (§6) and corrected the dependency note
    (§3.6).
  - Verified the cited bounds against the papers: Huddleston-Mehlhorn,
    Brodal-Fagerberg, and Bender-Demaine-Farach-Colton. The copies are kept
    outside the repository in the session scratchpad and are not
    committed. Iacono is cited from a secondary source only.
  - Wrote the harness (§8.3) and the production API design (§8.1).
- **2026-11-12, third pass:**
  - The first M4 run failed the long-run packing case. The fault was in
    the check, not in coarsen (§5.4); fixed at c558eca.
  - Recorded the M4 results (§9) and chose R = 10^4 and the P5/P6
    configuration.
  - Wrote the package: `btree/include/stx_btree/sequence_tree.hpp` and
    `row_height_index.hpp`, with tests `btree/tests/sequence_tree_test.cpp`
    and `sequence_tree_exhaustive_test.cpp`, registered in the root
    `CMakeLists.txt`. Nothing was compiled here; the integrator builds it.
- **Decisions in force:** §6 items 1-4; R = 10^4 (§9.3).
- **Next step:**
  1. The integrator builds the package under the six stx_auxlib builds and
     runs both tests, plus the exhaustive test with `--scope full`.
  2. Re-run W1-W9 against the package's RowHeightIndex. Page reservation
     changes allocation behaviour against the prototype (§9.5).
  3. Then wire GUI.Forms' VirtualRowsView to it (GUI.Forms side,
     coordinated with the comptroller): cursor seeks and walks,
     set_heights per stretch, and the run budget at R = 10^4.
- **Open:** §6 item 5; the child search (§9.4).

## 8. The sequence tree under the decisions

### 8.1 Production API (stx_btree, header-only)

```text
namespace stx_btree

struct HeightUnits { std::int64_t value; }    1/64 logical unit (26.6); comparisons hand-written
struct RowIndex    { std::uint64_t value; }   a row position; RowCount for counts
enum class RowState : std::uint8_t { estimated, measured, approximate }
struct RowExtent   { HeightUnits top; HeightUnits height; RowState state; }

template <typename Config, typename Allocator = GlobalPageAllocator>
class detail::SequenceTree     the tree of §3: prefix-array branches (counting
                               search), packed and dense leaves, weak B-tree
                               rebalancing, node-stack cursors. Config declares
                               slots, words, the search, dense leaves and the
                               limits. (The package has no entry Traits: runs
                               are the one entry kind, house style section 7.)

template <typename Allocator = GlobalPageAllocator>
class RowHeightIndex {
public:
    explicit RowHeightIndex(HeightUnits estimate, Allocator = {});
    bool assign(RowCount rows, HeightUnits estimate);          refused past the limits
    RowCount count() const;  HeightUnits total() const;  HeightUnits estimate() const;
    HeightUnits offset_of(RowIndex) const;                     exact prefix sum
    RowIndex row_at(HeightUnits) const;                        least r with offset_of(r + 1) > y
    RowExtent extent_of(RowIndex) const;                       one descent
    bool set_height(RowIndex, HeightUnits);                    refused (unchanged) if invalid
    bool set_heights(RowIndex, std::span<const HeightUnits>);  one leaf pass per leaf touched
    bool insert(RowIndex, RowCount);  void erase(RowIndex, RowCount);
    bool forget(RowIndex, RowCount);                           back to the estimate
    void coarsen(RowIndex, RowCount);                          keep the total, drop detail
    class Cursor;                                              node stack, revision-checked
    void seek_row(Cursor&, RowIndex) const;  void seek_offset(Cursor&, HeightUnits) const;
    void seek_offset_from(Cursor&, HeightUnits) const;         O(log_B d) from a valid cursor
    void advance(Cursor&) const;                               O(1) amortized
    std::size_t node_count() const, run_count() const, depth() const, page_bytes() const;   all O(1)
};
```

- **Failure:** expected alternatives are `bool` results; a refused call
  leaves the index unchanged. Every page an edit needs is reserved before
  it changes anything, so `std::bad_alloc` from the allocator also leaves
  it unchanged (bbtree's rule; the prototype does not do this yet).
- **Limits:**
  - one row at most 2^38 - 1 units (4.3·10^9 logical units);
  - the total and the row count below 2^62, checked with stx_auxlib's
    checked arithmetic (`src/checked.hpp`); the one modular expression
    (the counting search's sign test) is marked `STX_AUXLIB_MODULAR`.
- **Execution:** one thread. A cursor is valid until the next edit; a stale
  cursor is detected by the revision and re-seeks (house style section 4).
- **GUI.Forms keeps:** the double/26.6 conversion (round to nearest after
  the view's clamp), the view policies (anchor, overscan, follow-tail,
  width changes, when to `forget` and `coarsen`, R), its counters, and
  `BindingOrderTree` on its own tree.

### 8.2 Invariants and proof sketches

Invariants, checked by `check_invariants()` after every operation in E0:

- **I1** All leaves are at one depth; each branch's `level` is its height.
- **I2** Branch prefixes are exact:
  `count_prefix[j] = Σ_{i≤j} rows(child i)` and
  `height_prefix[j] = Σ_{i≤j} total(child i)`. Slots at or past `length`
  hold all ones and no child.
- **I3** Every entry has count ≥ 1 and total ≥ 0. Estimated entries are
  uniform (total divisible by count). A packed entry's fields are within
  24 and 38 bits. A dense leaf holds only measured single rows of at most
  2^32 - 1 units, in a configuration that admits dense leaves.
- **I4** Fill:
  - a non-root branch holds between `branch_minimum = max(2, slots/4)` and
    `slots` children;
  - the root branch holds at least 2;
  - a non-root leaf holds between `leaf_minimum = max(1, words/4)` and its
    kind's capacity (`words` packed, `2·words` dense);
  - capacities are at least twice the minimums (weak B-tree, b ≥ 2a).
- **I5** The root's sums equal `count()` and `total()`; total ≤ total
  limit < 2^62; rows ≤ row limit < 2^62.
- **I6** The leaf, branch and entry tallies equal a walk's counts, so
  `node_count()` and `run_count()` are O(1) and exact.

Proof sketches (beyond C1-C3 in §4):

- **Lemma S (splitting a run keeps every row's height).**
  - A run (k, S) gives row i the height q + [i < s] with q = S div k and
    s = S mod k. Its front i rows total T(i) = i·q + min(i, s).
  - If i ≤ s, the front has total i(q + 1), so all its rows are q + 1, and
    the back (k - i, (k - i)q + (s - i)) has q' = q and s' = s - i < k - i.
    Its first s - i rows are q + 1, as before.
  - If i > s, the front (i, iq + s) has q' = q and s' = s, and the back
    ((k - i), (k - i)q) is uniform at q.
  - In both cases every row keeps its height. Packing splits, edit splits
    and coarsening's inside offsets rest on this lemma.
- **Packing (expand).** A run past 2^24 - 1 rows or 2^38 - 1 units is cut
  at i = min(2^24 - 1, the most rows with T(i) ≤ 2^38 - 1). Then i ≥ 1,
  because one row is at most 2^38 - 1 units. Each cut is Lemma S, so the
  row sequence is unchanged.
- **Leaf edits.** An edit decodes the leaf into runs, edits them (Lemma S
  at the cut ends), joins equal uniform estimated neighbours, re-packs, and
  re-encodes into ⌈m / capacity⌉ leaves of even size. The one exception is
  an edit in the last quarter that splits in two, which leaves the arriving
  side max(m - capacity, capacity/4). Every chunk is at least the minimum:
  - for two chunks, even sizes are ≥ capacity/2, and the end split gives
    ≥ capacity/4 ≥ minimum on the right and min(capacity, m - capacity/4)
    on the left;
  - for L ≥ 3 chunks, m > (L - 1)·capacity, so each chunk exceeds
    2·capacity/3.
- **Underfull leaf.** A leaf left with 1 ≤ m < minimum is re-encoded with
  its sibling's entries (a sibling exists: every non-root branch has ≥ 2
  children by I4, and the root branch ≥ 2). The sibling held at least the
  minimum, so the join holds at least the minimum. If it exceeds a
  capacity, it splits as above. An emptied leaf is removed.
  - Merge or borrow (package): same-kind joins, and dense own rows with a
    packed sibling, hold fewer than capacity + minimum entries, so they fit
    two leaves. Only packed own entries with a dense sibling (up to
    2·words rows) can need three. Then the own side takes exactly
    `minimum` entries from the sibling's adjacent end. The sibling keeps
    more than 2·words − 2·minimum ≥ minimum rows, all dense-eligible, so it
    stays dense. Either way the two pages are reused and the parent gains
    no child.
- **Page reservation (package).** Every page an edit can need is taken
  from the allocator before the edit changes anything.
  - One leaf commit encodes at most 3·words pieces: a dense leaf (2·words)
    plus one inserted segment of at most `words` pieces, or set_heights'
    at most `words` rows a commit. So it needs at most three leaves, two of
    them new.
  - Its at most three replacement children make each ancestor split at
    most once (slots + 2 ≤ 2·slots), and a split root grows one level. So
    a commit needs at most one new branch a level, plus one.
  - Merges and borrows reuse their pages.
  - A tree of height h ≥ 1 has at least 2·minimum^(h−1) leaves, which
    bounds the height from the leaf count.
  - An erase's commits before its last remove a suffix of their leaf. That
    adds no entry, so they need no page.
  - An insertion of P pieces makes ⌈P / words⌉ segments. Every piece but
    the last holds 2^24 − 1 rows or at least (2^38 − 1)/2 units: it holds a
    row of at least q units, and stops where one more row (at most q + 1)
    would pass the field. That bounds P before the insertion.
  - Pages an edit frees go to the tree's spare lists, which are trimmed
    after the edit.
- **Branches.**
  - Replacing slots [a, a + g) by the new children rebuilds the node's
    prefixes from the children's (rows, total), which come exactly from
    prefix differences; I2 holds by construction.
  - An overfull node splits evenly (each part ≥ slots/2 ≥ minimum) and
    replaces its own slot in the parent; a split root grows a level.
  - An underfull non-root node joins its sibling (combined ≥ minimum) and
    splits again if over capacity.
  - A root left with one child is replaced by it; with none, the tree is an
    empty leaf.
  - Where nothing changes shape, each ancestor adds the node's (Δrows,
    Δtotal) to its prefixes from the node's slot on. That is exact, and
    equal to rebuilding, because integer addition is associative (C1).
  - The weak B-tree analysis (Huddleston and Mehlhorn 1982) bounds the
    total split and merge work by O(number of updates) from an empty tree.
- **Coarsen.** `coarsen(r, k)` computes S = offset_of(r + k) - offset_of(r),
  erases the rows and inserts the run (k, S) at r. For x ≤ r, offset_of(x)
  is a sum over rows before r, untouched. For x ≥ r + k it is that sum plus
  S plus rows after, so it is unchanged (C2).
- **Cursor finger.** `seek_offset_from` climbs from the leaf while the
  target is outside the current node's [start, start + total), then
  descends. Starts are exact by I2. A node d rows away is reached through
  the lowest common ancestor, O(log_B d) levels above a leaf in a tree of
  minimum fanout B/4.

### 8.3 The harness (`btree/research/order_statistic/`)

| File | Contents |
|---|---|
| `sequence_tree.hpp` | the prototype `SequenceTree<TreeConfig<slots, words, counting, dense, total limit, row limit>>`: queries, cursors, edits, `check_invariants()` |
| `index_adapters.hpp` | `PrototypeIndex<Config>` and `BaselineIndex` (B0) over one interface in Units; B0 converts at its edge and checks bracketing in its own doubles |
| `chat_workload.hpp` | the chat height generator, `ViewEmulator<Index, UseCursor>` (settle, anchor, band measuring, realize, run budget) |
| `order_statistic_bench.cpp` | W1-W9 over B0 and P1-P6, one variant and workload per process, one CSV line; `--check` adds bracketing and invariants |
| `order_statistic_exhaustive.cpp` | E0 (§5.4) and the packing-limit cases |
| `CMakeLists.txt`, `run_bench.sh` | the standalone build (Release, sanitizer, counting) and the run loop |

Known prototype simplifications, each to be removed in the package form:

- no page reservation before an edit (a `std::bad_alloc` mid-edit can leave
  the tree inconsistent);
- `erase` visits each leaf in the range rather than detaching whole
  subtrees, so it is O(log n) per leaf touched;
- leaf search is linear (no group totals);
- `coarsen` and `forget` are an erase followed by an insert;
- rows and heights are plain integer aliases, not wrapper types;
- pages come from `new`, not stx_btree's page allocators.

## 9. Measured results (M4, 2026-11-12)

These are measurements on one machine and one build, recorded as the
integrator reported them. The report is
`mac_handoff_results/2026-11-12-order-statistic.md`, and the CSVs are
`build/os-results-{1e7,1e6,budget}.csv` on the M4. No claim beyond these
numbers is made.

- **Setup:**
  - stx_auxlib `claude/btree-order-statistic-research` at 347da40, then
    c558eca (the check fix).
  - B0 compiles GUI.Forms' RowHeightIndex from the integration mirror at
    d0cc67e6e.
  - Seven fresh-process runs per cell (run 0 is the checked pass), run
    alone. The variants (§5.1) and workloads W1-W9 (§5.2) are as defined
    above. Times are medians in ms, with [min-max] where reported.

### 9.1 Correctness

- **Exhaustive check (E0, §5.4) at c558eca:** small scope under ASan+UBSan
  and full scope in Release both pass. Each of linear-dense,
  counting-dense, counting-odd-packed and counting-dense-limited ran
  57,140 cases (small) and 13,446,670 (full). The packing-limit cases pass
  for dense and odd-packed.
- **Sanitizer:** 63 of 63 checked bench runs (7 variants × 9 workloads,
  10^5 rows) under ASan+UBSan with leak detection gave no report.
- **Bracketing:** every P-variant has 0 bracket violations, with
  invariants ok, at 10^6 and 10^7. B0 also recorded 0.

### 9.2 Times

At 10^7 rows:

| Variant | W4 PageDown | W5 full traversal | W6 jumps |
|---|---|---|---|
| B0 | 17.50 [17.29-17.74] | 9003.5 [8960.8-9026.8] | 7.03 |
| P1 | 30.77 | 12301 | 16.01 |
| P2 | 31.68 | 15559 | 17.40 |
| P3 | 41.41 | 19032 | 20.01 |
| P4 | 5.37 [5.14-5.65] | 2090 [2082-2094] | 2.11 |
| P5 | 6.05 | 2205 | 2.16 |
| P6 | 6.16 [5.97-6.18] | 1928 [1924-1929] | 2.16 |

At 10^6 rows:

- W5: B0 767.5, P1 1137, P2 1416, P3 1661, P4 191.9, P5 204.8, P6 191.2;
- W4: B0 17.73, P4 5.49, P5 5.90, P6 5.75.

P4-P6 have lower medians than B0 on every workload but W1 at both sizes; W1
is near 0.01 ms for every variant. The full tables for all nine workloads
are in the CSVs.

### 9.3 The run budget R

P6 at 10^7 rows, W4 / W5 medians and the footprint the bench records at
the end of W5:

| R | W4 | W5 | Footprint |
|---|---|---|---|
| 10^4 | 5.78 | 1841 | 1.26 MB |
| 10^5 | 5.75 | 1929 | 2.26 MB |
| 10^6 | 5.71 | 2145 | 11.93 MB |

**Default R = 10^4.** Of the three values tried, it gave the lowest W5 time
and the smallest footprint. W4 differs by 0.07 ms across the three, which
is within the spread of the other cells. The budget keeps detail within
R/4 = 2500 rows of the anchor, well beyond the band of about 20 rows
(1600 units of viewport and overscan at a mean of 89.5 units a row).
Values below 10^4 were not tried, so this is the best measured value, not
a measured optimum. R stays a GUI.Forms view policy (§8.1); the package
provides `coarsen` and `run_count`.

### 9.4 What carries the gain

- **P1-P3 lose to B0** on W4, W5 and W6 at 10^7: 1.8× to 2.4× its time.
  These are the tree alone, driven the way VirtualRowsView drives the
  index today, with several root descents a row.
- **The cursor mode (P4) carries the gain.** It is C3: one seek, then
  O(1) steps, `seek_offset_from` from the previous position, and one
  `set_heights` batch per stretch of unmeasured rows. P4 is P2 plus this
  mode, and it takes W4 from 31.68 to 5.37 ms and W5 from 15559 to
  2090 ms.
- **Dense leaves (P5)** cost a little time against P4 (W4 6.05 vs 5.37 ms;
  W5 2205 vs 2090 ms) for their memory per measured row.
- **The budget (P6)** recovers W5 time (1928 ms against P5's 2205).
- **Counting against linear search:** without cursors, the counting
  search (P2) was slower than linear (P1) on W4 and W5. No linear variant
  with cursors was measured. The package keeps the search a declared
  choice (`SequenceConfig`) and ships the measured counting form. Whether
  linear search is faster with cursors is an open measurement.

### 9.5 What the package changes against the measured prototype

- **Page reservation (§8.2):** each edit allocates its worst-case pages
  first and returns the unused ones to a spare list kept at most 8 leaves
  and 32 branches between edits. The prototype allocated on demand. A
  set_heights over k rows reserves for about k/words + k/minimum + 2
  commits.
- **Merge or borrow** for underfull leaves next to dense siblings (§8.2).
- **Fixed scratch buffers.** These replace the prototype's vectors, so an
  index object holds about 40 KB of scratch at the production
  configuration.
- **Cursors:** they carry their tree's identity and re-seek when stale or
  foreign.

These keep the operations and their counts, but the times in §9.2 are the
prototype's. The package should be re-measured with W1-W9 before GUI.Forms
quotes them.

## Sources

- M. Pătraşcu and E. D. Demaine, "Logarithmic Lower Bounds in the Cell-Probe
  Model", SIAM J. Computing 35(4), 2006; arXiv cs/0502041 (read via
  ar5iv.labs.arxiv.org/html/cs/0502041), Theorems 1-3 and 5.
- M. Pătraşcu and E. D. Demaine, "Tight Bounds for the Partial-Sums Problem",
  SODA 2004 (abstract at erikdemaine.org/papers/PartialSums_SODA2004/).
- M. L. Fredman and M. E. Saks, "The cell probe complexity of dynamic data
  structures", STOC 1989.
- M. L. Fredman, "The complexity of maintaining an array and computing its
  partial sums", J. ACM 29(1), 1982.
- B. Chazelle, "Lower bounds for off-line range searching", Discrete Comput.
  Geom. 17(1), 1997.
- H. Hampapuram and M. L. Fredman, "Optimal biweighted binary trees and the
  complexity of maintaining partial sums", SIAM J. Computing 28(1), 1998
  (FOCS 1993).
- P. F. Dietz, "Optimal algorithms for list indexing and subset rank", WADS
  1989.
- R. Raman, V. Raman and S. S. Rao, "Succinct dynamic data structures", WADS
  2001.
- L. Arge, "The buffer tree", WADS 1995; Algorithmica 37, 2003.
- G. S. Brodal and R. Fagerberg, "Lower bounds for external memory
  dictionaries", SODA 2003 (read from the authors' copy,
  cs.au.dk/~gerth/papers/soda03.pdf: Theorem 1.1, §3.4).
- M. A. Bender, M. Farach-Colton, W. Jannen, R. Johnson, B. C. Kuszmaul,
  D. E. Porter, J. Yuan and Y. Zhan, "An introduction to Bε-trees and
  write-optimization", ;login: 40(5), 2015.
- M. A. Bender, E. D. Demaine and M. Farach-Colton, "Cache-oblivious
  B-trees", FOCS 2000; SIAM J. Computing 35(2), 2005 (read from the
  author's copy, erikdemaine.org/papers/CacheObliviousBTrees_SICOMP/:
  abstract and §1).
- S. Huddleston and K. Mehlhorn, "A new data structure for representing
  sorted lists", Acta Informatica 17, 157-184, 1982 (read from the author's
  copy, people.mpi-inf.mpg.de/~mehlhorn/ftp/: summary and §1, pp. 157-158).
- D. D. Sleator and R. E. Tarjan, "Self-adjusting binary search trees",
  J. ACM 32(3), 1985; R. Cole, "On the dynamic finger conjecture for splay
  trees", SIAM J. Computing 30(1), 2000; J. Iacono, "Alternatives to splay
  trees with O(log n) worst-case access times", SODA 2001, pp. 516-522
  (not read: no publisher or author copy; its bound is cited from
  Wikipedia, "Iacono's working set structure").
- P. Bille, I. L. Gørtz and F. R. Skjoldjensen, "Partial Sums on the
  Ultra-Wide Word RAM", TAMC 2020; arXiv 1908.10159.
- S. Slotin, *Algorithms for Modern Hardware*, en.algorithmica.org/hpc/:
  "Segment Trees" (wide segment tree, prefix sums per node, masked SIMD
  update), "Binary Search" (Eytzinger), "Static B-Trees" (S-tree, B+).
- X. Hao, bbtree, design/03-decisions.md at 8adae97 (via stx_btree
  DESIGN.md).
- Zed `sum_tree` crate source documentation (git.secluded.site/zed,
  `crates/sum_tree/src/sum_tree.rs`), design precedent only.
