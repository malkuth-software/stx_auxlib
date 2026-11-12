# Order-statistic sums in stx_btree: research notes

Research track for moving GUI.Forms' `RowHeightIndex` into stx_btree. It is
written to be resumed from: the state, the decisions taken so far and the
next step are in the last section. Nothing here is code yet, and no figure
here is a measurement unless it names its run. Statements recalled from a
source but not re-read during this study are marked *(recalled)*. They are
listed for checking in the downloads request at the end.

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
  (Brodal and Fagerberg, SODA 2003; Bender et al., ";login:" 2015) buffer
  updates. A B^ε-tree has updates O(log_B n / (ε·B^(1-ε))) and queries
  O((1/ε)·log_B n) *(recalled)*. Putting t_u ≈ log_B n / B^(1-ε) into the
  trade-off gives t_q = Ω((1/ε)·log_B n), so the B^ε trade-off is on the
  curve. Buffering pays only when updates outnumber queries by a large
  factor. A list view queries every frame (row_at, offset_of) and measures
  a few rows per frame, so buffering would buy nothing here.
- **Cache-oblivious B-trees** (Bender, Demaine and Farach-Colton, FOCS 2000 /
  SICOMP 2005; van Emde Boas layout after Prokop 1999) reach O(log_B n) at
  every level of the hierarchy without knowing B *(recalled)*. A
  cache-aware B-tree tuned for one level (the 128-byte M-series line, or
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

- **Finger search trees:** with a finger at a known position, finding a
  position at distance d costs O(log d). Insertions and deletions at a
  finger cost O(1) amortized in level-linked (2,4)- or (a,b)-trees
  (Huddleston and Mehlhorn, Acta Informatica 17, 1982) *(recalled)*.
  Brodal's and others' worst-case variants exist *(recalled)*.
- **Splay trees** have the working-set and dynamic-finger properties (Sleator
  and Tarjan 1985; Cole 2000). Iacono's working-set structure (2001) gives
  O(log t) for an item last used t operations ago *(recalled)*. These
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
- **Amortized rebalancing:** (a,b)-trees with b ≥ 2a do O(1) amortized node
  splits and merges per update (Huddleston and Mehlhorn 1982) *(recalled)*.
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
- **Packed entry, 8 bytes:** count 24 bits, total 39 bits, state 1 bit.
  - A run longer than 2^24 - 1 rows, or with a total of 2^39 or more, is
    stored as several entries. Splitting is exact (div/mod above), so this
    is invisible to the API.
  - 10M rows at an estimate of 24 units is 1.5·10^10 < 2^39 ≈ 5.5·10^11, one
    entry. Splitting is the cost of the packing; it is O(1) entries per
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

Dependency direction: GUI.Forms would consume stx_auxlib's stx_btree. That
wiring is planned for later with the status-review sibling (memory
`stx-auxlib-ownership`), and the Windows sibling is comptroller of GUI.Forms
and stx_auxlib (memory `mac-side-partition`). So the move needs the
coordinator's routing (open question 3).

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
- **Proof:** Huddleston and Mehlhorn's (a,b)-tree amortization for
  b ≥ 2a *(recalled; to verify, see downloads)*. bbtree's rebalancing
  removal is the implementation pattern.

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

Nothing below exists yet. The next step of this track is to write it under
`btree/research/order_statistic/` (a standalone CMake project, not part of
the package build). The baseline is GUI.Forms' current `RowHeightIndex` and
`OrderStatisticTree`, compiled from the integration tree by path, for the
experiment only: stx_auxlib's package must not depend on GUI.Forms.

### 5.1 Variants

| Id | Variant |
|---|---|
| B0 | GUI.Forms `RowHeightIndex` as is (`double`, vectors, linear scans) |
| P1 | C1 + C4 packed entries: integer prefix-array branches (512 B), 1 KiB leaves, linear in-node scans |
| P2 | P1 with C5 counting search in branches and leaves (leaf group totals) |
| P3 | P2 with 1 KiB branches (42 children) |
| P4 | P2 with C3 cursors used by the workload driver |
| P5 | P4 with dense 4-byte leaves |
| P6 | P4 with the C2 coarsening budget R ∈ {10^4, 10^5, 10^6} |

### 5.2 Workloads

N ∈ {10^5, 10^6, 10^7}. Seeded. Heights come from the chat generator's
shape: 40 units of header for 1 row in 4, plus lines × 18 units, where
lines = ⌈length × 7.5 / 1180⌉ and length is uniform in 100-1000 from a hash
of (seed, row). The driver emulates `VirtualRowsView`: a settle on a
1280×800 viewport with half a viewport of overscan on each side, measuring
unmeasured rows in the band.

| Id | Workload | Emulates |
|---|---|---|
| W1 | `assign(N, 24)`, then a settle at row 0 and at N/2 | startup (CHAT 7.1) |
| W2 | 600 frames of fling scroll (40 units/frame, decaying 5% a frame, repeated) from N/2 | 7.4b |
| W3 | 600 frames of one wheel notch (3 × 24 units) per frame | 7.4a |
| W4 | PageDown 3600 times | 7.4c (r growth) |
| W5 | a full sequential traversal measuring every row | worst-case memory (gap 9.1) |
| W6 | 1000 random `offset_of` jumps plus 100 thumb positions (`row_at(total × u)`), each followed by a settle | 7.5 |
| W7 | append bursts of 10^4 estimated rows every 120 frames with follow-tail, plus measured appends of 10 rows per frame | 7.6 |
| W8 | 1000 `set_height` on random measured rows, and 1000 `erase(row, k)` with k ∈ [1, 10^4] | 7.7 |
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

All operation sequences of length ≤ 6 over at most 6 rows. Heights come
from {0, 1, 2, 64, 2^39 - 1}, and runs are forced through split boundaries
by setting node capacities to their minimum in a test configuration. After
every operation, check every invariant I1-I4, the packed-field bounds and
every query against the flat model. This enumerates every case up to that
size, so it is complete for that size rather than sampled; the general case
rests on §4's proofs.

### 5.5 Commands (the integrator runs them; nothing here was run)

The research worktree is not one of `m4ws`'s workspaces, so it is copied by
rsync, as `m4ws` itself syncs:

```sh
/usr/bin/rsync -rl --checksum --no-times --delete --omit-dir-times --exclude='/build*/' \
  ~/Developer/gui_forms_workers/research/stx_auxlib/ \
  m4mini:Developer/Projects/gui_forms_workers/research/stx_auxlib/
ssh m4mini 'cd Developer/Projects/gui_forms_workers/research/stx_auxlib && \
  /opt/homebrew/bin/cmake -S btree/research/order_statistic -B build/os-release \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=/opt/homebrew/opt/llvm@22/bin/clang++ \
    -DGUI_FORMS_ROOT=$HOME/Developer/Projects/gui_forms_workers/integration/gui_forms && \
  /opt/homebrew/bin/cmake --build build/os-release -j 8 && \
  build/os-release/order_statistic_exhaustive && \
  build/os-release/order_statistic_bench --rows 100000,1000000,10000000 --variants all \
    --workloads all --runs 7 --out build/os-release/results.csv'
ssh m4mini 'cd Developer/Projects/gui_forms_workers/research/stx_auxlib && \
  /opt/homebrew/bin/cmake -S btree/research/order_statistic -B build/os-sanitize \
    -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=/opt/homebrew/opt/llvm@22/bin/clang++ \
    -DORDER_STATISTIC_SANITIZE=ON \
    -DGUI_FORMS_ROOT=$HOME/Developer/Projects/gui_forms_workers/integration/gui_forms && \
  /opt/homebrew/bin/cmake --build build/os-sanitize -j 8 && \
  ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
    build/os-sanitize/order_statistic_exhaustive && \
  ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
    build/os-sanitize/order_statistic_bench --rows 100000 --variants all --workloads all --runs 1 --check-only'
```

- Builds, the sanitizer run and the timed run must not overlap (AGENTS.md:
  trials sequential).
- The integration tree must be at the commit recorded with the results.
- The RunPod repeat uses the same two commands with GCC and Clang from that
  host and `perf stat` around the bench.

## 6. Open questions for the owner

1. **The target.** Accept that worst-case Θ(log n) per operation is already
   at the proven lower bound, and aim at adaptive bounds (log r, log d,
   k + log n), memory and constants? (Recommended.)
2. **Fixed-point heights** in 1/64 logical units (26.6, as FreeType) inside
   the index, with GUI.Forms converting at the boundary? (Recommended: it is
   what makes C1's proofs and host-independence possible.)
3. **Where it lives and who wires it:** generic `SequenceTree` plus a
   concrete `RowHeightIndex` in stx_btree, with GUI.Forms depending on
   stx_auxlib for it. This touches the Windows comptroller's ground and the
   later GUI.Forms wiring.
4. **Memory policy for chat:** the C2 coarsening budget (bounded, offsets
   stable) as the default, dense leaves (C4) as well, or both? A default R?
5. **Per-row estimates (CHAT decision 5):** adopt C7's estimate source, or
   keep one estimate?

## 7. Track state (resume here)

- **2026-11-12, done:** read the GUI.Forms code (integration
  `claude/batch-cp29-2b-b`), CHAT_STRESS_SPEC §4.2, §7 and §9, the backlog
  entry, stx_btree's DESIGN.md, OPTIMAL_TREES.md and node search; checked
  the cell-probe bounds against the Pătraşcu-Demaine text (ar5iv HTML) and
  Slotin's segment-tree article. Wrote this document.
- **Decisions so far (proposals until the owner answers §6):** integer
  heights; a sequence tree beside the maps reusing bbtree's pages, count
  search and node stacks; runs as (count, total, state); cursors with a
  structural revision.
- **Next step:** write `btree/research/order_statistic/` (E0 exhaustive
  check, P1 first, then the driver and B0 baseline) on this branch, then
  hand the commands in §5.5 to the integrator. Code only after the
  coordinator confirms §6.1-6.3, since they decide the API.
- **To verify** (marked *(recalled)*): Huddleston-Mehlhorn amortization and
  finger bounds; B^ε-tree bounds (Brodal-Fagerberg; Bender et al.);
  cache-oblivious B-tree bounds; Iacono's working-set bound.

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
  dictionaries", SODA 2003.
- M. A. Bender, M. Farach-Colton, W. Jannen, R. Johnson, B. C. Kuszmaul,
  D. E. Porter, J. Yuan and Y. Zhan, "An introduction to Bε-trees and
  write-optimization", ;login: 40(5), 2015.
- M. A. Bender, E. D. Demaine and M. Farach-Colton, "Cache-oblivious
  B-trees", FOCS 2000; SIAM J. Computing 35(2), 2005.
- S. Huddleston and K. Mehlhorn, "A new data structure for representing
  sorted lists", Acta Informatica 17, 1982.
- D. D. Sleator and R. E. Tarjan, "Self-adjusting binary search trees",
  J. ACM 32(3), 1985; R. Cole, "On the dynamic finger conjecture for splay
  trees", SIAM J. Computing 30(1), 2000; J. Iacono, "Alternatives to splay
  trees with O(log n) worst-case access times", SODA 2001.
- P. Bille, I. L. Gørtz and F. R. Skjoldjensen, "Partial Sums on the
  Ultra-Wide Word RAM", TAMC 2020; arXiv 1908.10159.
- S. Slotin, *Algorithms for Modern Hardware*, en.algorithmica.org/hpc/:
  "Segment Trees" (wide segment tree, prefix sums per node, masked SIMD
  update), "Binary Search" (Eytzinger), "Static B-Trees" (S-tree, B+).
- X. Hao, bbtree, design/03-decisions.md at 8adae97 (via stx_btree
  DESIGN.md).
- Zed `sum_tree` crate source documentation (git.secluded.site/zed,
  `crates/sum_tree/src/sum_tree.rs`), design precedent only.
