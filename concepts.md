# kntrie — Design & Internals

## 1. Trie Fundamentals

A trie decomposes keys into a sequence of fixed-width chunks and uses each chunk to
select a branch at the corresponding tree level. For integer keys the natural
decomposition is by bit groups — an *n*-bit radix trie uses *n* bits per level.

A naive 8-bit radix trie over 64-bit keys has 8 levels of 256-entry pointer arrays.
This yields O(1) lookup (bounded by key width) but catastrophic memory waste: a
single root-to-leaf path with no branching allocates 8 × 2048 = 16 KB of pointer
arrays to store one entry.

kntrie eliminates this waste through three cooperating mechanisms: adaptive node
representations that match structure to population density, bitmap-compressed sparse
dispatch that stores only occupied slots, and prefix compression that collapses
uniform trie levels into metadata.

---

## 2. Key Encoding

User keys are converted to a 64-bit **internal key** by left-aligning into a uint64_t:

```
internal = static_cast<uint64_t>(key) << (64 - key_bits)
```

This places the MSB at bit 63 regardless of key width, so 8/16/32/64-bit keys share
identical extraction logic parameterized only by bit count.

### Signed key ordering

Signed keys undergo a sign-bit XOR before alignment:

```
result ^= (1ULL << (key_bits - 1))
```

This maps the signed range monotonically onto unsigned order: `INT64_MIN → 0`,
`0 → 0x8000000000000000`, `INT64_MAX → 0xFFFFFFFFFFFFFFFF`. All subsequent trie
operations use unsigned comparison, and iterator traversal produces correct signed
ordering.

### Suffix type narrowing

At each trie level, the remaining key bits are stored in the narrowest integer type
that fits:

| Remaining bits | Storage type | Sizeof |
|---------------|-------------|--------|
| 1–8           | `uint8_t`   | 1      |
| 9–16          | `uint16_t`  | 2      |
| 17–32         | `uint32_t`  | 4      |
| 33–64         | `uint64_t`  | 8      |

This is selected at compile time via `suffix_traits<BITS>::type`. At a leaf storing
entries with 8 remaining bits, each key costs 1 byte. Combined with inline value
storage (see §9), this is the primary driver of kntrie's memory density.

---

## 3. Level Architecture

kntrie processes keys **16 bits per level**. For a 64-bit key this gives a maximum
depth of 4. Each 16-bit chunk is split into two 8-bit halves for dispatch:

```
64-bit internal key:
┌────────────┬────────────┬────────────┬────────────┐
│  Level 0   │  Level 1   │  Level 2   │  Level 3   │
│ bits[63:48]│ bits[47:32]│ bits[31:16]│ bits[15:0] │
└────────────┴────────────┴────────────┴────────────┘

Each 16-bit level:
┌──────────┬──────────┐
│  Top 8   │  Bot 8   │
│  (0xHH)  │  (0xLL)  │
└──────────┴──────────┘
```

The top 8 bits index into a bitmap-compressed 256-way fanout at the split node level.
The bottom 8 bits either resolve directly (via a second bitmap at the terminal level)
or select a child within a bottom-internal node.

At each level a node is either a **compact leaf** (flat sorted array) or a **split
node** (bitmap-indexed two-tier structure). The transition between them is governed
by the COMPACT_MAX threshold.

---

## 4. The 4096 Threshold

Both COMPACT_MAX and BOT_LEAF_MAX are set to 4096. This value is the product of
two complementary constraints:

**Optimal split fanout.** When a compact leaf of 4096 entries splits on its top
8 bits (256 possible values), the expected bucket size under uniform distribution
is 4096 / 256 = **16 entries per bucket**. This places each bucket squarely in the
range where a single-pass linear scan is optimal — no index layers needed, fitting
entirely in one or two cache lines.

**Three-level indexed search.** The idx_search structure (§7) uses two index layers
sampling every 256th and every 16th key. The maximum array size for exactly three
scan passes of 16 elements each is 16 × 16 × 16 = **4096**. Beyond this, a fourth
index layer would be needed, adding complexity for diminishing returns. At 4096,
every lookup requires at most 3 sequential scans of ≤16 elements.

The convergence of these two constraints at the same value is not coincidental — it
reflects the 8-bit (256-way) fanout interacting with the 16-element scan width.

---

## 5. Node Types

### 5.1 Node Header

Every node begins with an 8-byte header:

```
struct NodeHeader {        // 8 bytes, naturally aligned
    uint32_t count;        // total entry count in this subtree
    uint16_t top_count;    // occupied top-8 buckets (split nodes only)
    uint8_t  skip;         // prefix-compressed levels (each = 16 bits)
    uint8_t  flags;        // bit 0: is_leaf, bit 1: is_split
};
```

When `skip > 0`, the header extends to 16 bytes with a second uint64_t storing the
compressed prefix. All subsequent offsets are computed from `header_u64(skip)`:
1 for skip=0, 2 for skip>0.

### 5.2 Compact Leaf

A flat sorted array of (suffix, value) pairs. Used when entry count ≤ 4096.

```
┌───────────────────┐
│ Header (8–16 B)   │
├───────────────────┤
│ idx1[ ] samples   │  ⌈count/256⌉ entries, only if count > 256
│ idx2[ ] samples   │  ⌈count/16⌉ entries, only if count > 16
│ keys[ ] sorted    │  suffix_type × count
├───────────────────┤  (8-byte aligned boundary)
│ values[ ]         │  value_slot_type × count
└───────────────────┘
```

Keys store the **full remaining suffix** — all bits below the current level. A
compact leaf at level 0 of a 64-bit key holds 64-bit suffixes; at level 2, 32-bit
suffixes. This means a compact leaf can represent an arbitrarily deep subtree with
no child pointers.

All byte regions are 8-byte aligned via padding, ensuring uint64_t-aligned access
for all accesses through the `uint64_t*` node pointer.

**Flags:** `is_leaf=1, is_split=0`

### 5.3 Split Node (Top Level)

Created when a compact leaf exceeds 4096 entries. Uses the top 8 bits to dispatch
into bitmap-compressed buckets:

```
┌─────────────────────┐
│ Header (8–16 B)     │
├─────────────────────┤
│ top_bitmap (32 B)   │  Bitmap256: which top-8 values are occupied
├─────────────────────┤
│ bot_is_leaf (32 B)  │  Bitmap256: which bottoms are leaves vs internal
│                     │  (elided at BITS=16 — all bottoms are leaves)
├─────────────────────┤
│ child_ptrs[ ]       │  uint64_t × popcount(top_bitmap)
│ (dense, packed)     │  each points to a bot_leaf or bot_internal
└─────────────────────┘
```

The child pointer array is **dense**: only occupied slots are stored. Mapping a
top-8 value to an array index requires a bitmap presence check plus popcount
(see §6).

**Flags:** `is_split=1`; `is_leaf` tracks whether any bottom children are leaf-type.

### 5.4 Bottom Leaf (bot_leaf)

Stores entries sharing the same top-8 prefix within a split node.

**Terminal case (BITS = 16):** Only 8 bits remain after the top-8 split. Uses a
bitmap for direct O(1) membership test:

```
┌────────────────────┐
│ bitmap (32 B)      │  Bitmap256: which bottom-8 values are present
├────────────────────┤
│ values[ ] (dense)  │  value_slot_type × popcount(bitmap)
└────────────────────┘
```

Lookup: `bitmap.has_bit(suffix)` → `bitmap.count_below(suffix)` → `values[slot]`.
No key storage at all — the bitmap *is* the key.

**Non-terminal case (BITS > 16):** (BITS−8) suffix bits remain. Uses a sorted
list with indexed search:

```
┌────────────────────┐
│ count (8 B padded) │
├────────────────────┤
│ idx1[ ] samples    │
│ idx2[ ] samples    │
│ suffixes[ ] sorted │  (BITS−8)-bit suffix type × count
├────────────────────┤
│ values[ ]          │
└────────────────────┘
```

### 5.5 Bottom Internal (bot_internal)

Replaces a bot_leaf when it exceeds 4096 entries. Adds a second 8-bit bitmap
dispatch, completing the full 16-bit level:

```
┌────────────────────┐
│ bitmap (32 B)      │  Bitmap256: occupied bottom-8 values
├────────────────────┤
│ child_ptrs[ ]      │  uint64_t × popcount(bitmap)
│                    │  each → node at (BITS − 16)
└────────────────────┘
```

Each child is a node at the next level down — a compact leaf, split node, or
prefix-compressed descendant.

---

## 6. Bitmap256 — Compressed 256-Way Dispatch

`Bitmap256` is 4 × `uint64_t` = 32 bytes. It replaces the 2048-byte pointer array
a full 256-way node would require, storing only presence information.

### find_slot: combined presence + position

The critical operation is `find_slot(index, &slot)` — simultaneously testing
membership and computing the dense array index:

```cpp
int word = index >> 6;           // which uint64_t (0–3)
int bit  = index & 63;          // position within word

uint64_t before = words[word] << (63 - bit);
if (!(before & (1ULL << 63)))   // target bit not set?
    return false;

slot = popcount(before) - 1;    // bits set in same word, at or below target
slot += popcount(words[0]) & -int(word > 0);  // add full words before
slot += popcount(words[1]) & -int(word > 1);
slot += popcount(words[2]) & -int(word > 2);
```

The accumulation uses branchless masking: `-int(true)` is `0xFFFFFFFF` (identity
under bitwise AND), `-int(false)` is `0x00000000` (zeroes the term). On x86-64-v3
targets, `popcount` compiles to a single `POPCNT` instruction.

### Memory savings

A sparse node with *k* occupied slots costs 32 (bitmap) + 8*k* (pointers) bytes.
A full pointer array costs 2048 bytes. Break-even is at *k* = 252; below that,
bitmaps save space. In practice, typical top-8 distributions have 10–100 occupied
values, yielding 5–20× compression versus full arrays.

At the terminal level (BITS = 16), the bottom bitmap stores key presence directly
with no pointer array at all — 32 bytes for up to 256 entries.

---

## 7. Indexed Linear Search (idx_search)

### Design rationale

Binary search is O(log *n*) in comparisons but each comparison accesses a
non-sequential memory location. For an array of 4096 32-bit keys (16 KB), a
binary search touches 12 cache lines across the full array. On modern hardware,
each L1 miss costs ~4ns and each L2 miss ~12ns, dominating comparison costs.

kntrie replaces binary search with a **three-tier sequential scan** optimized for
hardware prefetch.

### Memory layout

Sorted keys are stored with two prepended index layers:

```
┌───────────────────────────────────────────────────────────────┐
│ idx1[⌈n/256⌉]  │  idx2[⌈n/16⌉]  │  keys[n]                 │
└───────────────────────────────────────────────────────────────┘
```

`idx1[i]` = `keys[i × 256]` — samples every 256th key.
`idx2[i]` = `keys[i × 16]` — samples every 16th key.

For 4096 entries: idx1 has 16 entries, idx2 has 256 entries, keys has 4096 entries.

### Search procedure

```
1. Linear scan idx1 (≤16 entries) → identifies 256-element block
2. Linear scan idx2 (≤16 entries within block) → identifies 16-element block
3. Linear scan keys (≤16 entries) → exact match test
```

Each scan uses `idx_subsearch`, a tight forward loop over at most 16 elements:

```cpp
template<typename K>
static int idx_subsearch(const K* start, int count, K key) noexcept {
    [[assume(count <= 16)]];
    const K* run = start;
    const K* end = start + count;
    do {
        if (*run > key) break;
        run++;
    } while (run < end);
    return static_cast<int>(run - start) - 1;
}
```

`[[assume(count <= 16)]]` enables the compiler to emit tight unrolled code. Each
scan touches at most 2 cache lines (16 × 8 bytes = 128 bytes for uint64_t keys),
and the sequential access pattern triggers hardware prefetch.

### Complexity

Worst case: 16 + 16 + 16 = 48 comparisons, all sequential. For arrays ≤ 16 entries,
both index layers are absent and a single direct scan suffices — zero structural
overhead. The idx layers themselves consume `(⌈n/256⌉ + ⌈n/16⌉) × sizeof(K)` bytes
of overhead, which at 4096 entries is 272 × sizeof(K) — 6.6% for uint64_t keys.

---

## 8. Skip (Prefix) Compression

When all entries in a subtree share the same 16-bit chunk at some level, there is no
branching — the level is structurally redundant. kntrie collapses it.

### Mechanism

Instead of creating a split node with one bucket:

1. Store the shared 16-bit chunk in the node's prefix field
2. Increment the `skip` counter
3. Store data as if the level didn't exist

Multiple consecutive uniform levels combine: `skip = 3` means 48 bits of shared
prefix occupy a single uint64_t, and lookup jumps 3 levels forward after a
constant-time prefix comparison.

### Detection

During compact-to-split conversion, if all entries bucket to the same top-8 value
AND the same bottom-8 value, the conversion is replaced by skip accumulation.
The entries' suffixes are shifted down by 16 bits and the process recurses at
(BITS − 16). This repeats until a non-uniform level is found.

The same logic applies in `create_child_no_prefix` during bulk node construction —
skip compression is applied recursively bottom-up.

### Prefix mismatch on insert

When a new key diverges from the stored prefix, the skip structure splits:

1. Find the first differing 16-bit chunk (scanning high to low)
2. Create a new split node at the divergence point
3. The common prefix (chunks above the divergence) becomes the new node's skip
4. The old subtree and new key become children, each carrying their own remaining
   prefix (chunks below the divergence)

This preserves all existing entries without copying their data — only the routing
metadata changes.

### Impact

Sequential keys 0..99999 share 48 upper bits. kntrie stores `skip = 3` with one
compact leaf of 16-bit suffixes: **1.2 bytes/entry** for `kntrie<uint64_t, char>`.

---

## 9. Value Storage

Compile-time selection between two modes:

**Inline** (`sizeof(VALUE) ≤ 8 && is_trivially_copyable_v<VALUE>`): values are
stored directly in the value slot array. No per-value heap allocation. A
`kntrie<uint64_t, char>` stores 1-byte values inline; a `kntrie<uint64_t, uint64_t>`
stores 8-byte values inline.

**Indirect** (larger or non-trivially-copyable values): each value is heap-allocated
via the rebound allocator, and a pointer is stored in the slot. Construction and
destruction are properly managed through allocator traits.

```cpp
using value_slot_type = std::conditional_t<value_inline, VALUE, VALUE*>;
```

---

## 10. Node Lifecycle

```
                   count > 4096
Compact Leaf  ─────────────────►  Split Node
   (sorted                          │
    array)                          │ per top-8 bucket:
                                    ▼
                                 Bot Leaf
                                    │
                                    │ count > 4096
                                    ▼
                               Bot Internal
                                    │
                                    │ each child →
                                    ▼
                              Node at (BITS−16)
                            (compact leaf or split)
```

**Compact → Split:** Entries are bucketed by top 8 bits. With 4096 entries and
256 possible buckets, the average bucket holds 16 entries — the optimal scan width.
If all entries share a common 16-bit prefix (single-bucket case), skip compression
is applied instead (see §8).

**Bot Leaf → Bot Internal:** Analogous second-stage split. Each sub-bucket becomes a
full node at the next trie level, enabling recursive descent.

---

## 11. Lookup Path

```
find(key):
  ik = key_to_internal(key)
  node = root, h = header(root)

  for each 16-bit level (BITS = key_bits, key_bits−16, ...):
    if h.skip > 0:
      compare 16-bit chunk against stored prefix
      mismatch → return not_found
      match    → consume chunk, decrement skip_left, continue

    if compact_leaf:
      idx_search(suffix) → found/not_found

    if split:
      top_8 = extract_top8(ik)
      top_bitmap.find_slot(top_8) → slot or not_found

      if bot_leaf:
        BITS=16: bot_bitmap.has_bit(bot_8) → value
        BITS>16: idx_search(suffix) → value

      if bot_internal:
        bot_8 = extract_top8(ik)  (next 8 bits)
        bot_bitmap.find_slot(bot_8) → child
        node = child, continue at BITS−16
```

This is implemented as a tail-recursive template chain: `find_impl<BITS>` calls
`find_impl<BITS−16>`. The compiler unrolls the recursion into a flat sequence of
specialized code — for 64-bit keys, four static code paths for BITS ∈ {64, 48, 32, 16}.

---

## 12. Template Specialization Strategy

All node operations are parameterized by `template<int BITS>`. This enables:

- **`suffix_traits<BITS>::type`** — compile-time selection of the narrowest integer
  type for key storage at each level.
- **`if constexpr (BITS == 16)`** — terminal-level specializations (bitmap-based
  bottom leaves, no bot_is_leaf_bitmap) are eliminated from non-terminal code paths.
- **`requires (BITS > 16)` / `requires (BITS > 0)`** — prevent invalid template
  instantiation at boundary conditions. Without these guards, the compiler would
  attempt to instantiate `suffix_traits<-16>` or `insert_impl<0>`.

The result: for `kntrie<uint64_t, VALUE>` the compiler generates four specialized
insert paths and four specialized find paths, each with correct types, shifts,
bitmap logic, and layout offsets resolved at compile time.

---

## 13. Interface & Iteration

kntrie provides the same interface as `std::map<KEY, VALUE>` for integer key types:
`insert`, `find`, `erase`, `operator[]`, `lower_bound`, `upper_bound`, iterators
(`begin`/`end`), `size`, `empty`, `clear`.

Iteration traverses the trie in internal-key order, which corresponds to:
- Unsigned natural order for unsigned keys
- Correct signed order for signed keys (due to the sign-bit XOR in §2)

Iterators maintain a stack of (node, position) pairs to track the current path
through the trie. Advancing to the next entry is amortized O(1): most advances
are within a leaf's sorted array, with occasional stack pops/pushes at node
boundaries.

---

## 14. Performance Characteristics

### Memory (bytes/entry, `kntrie<uint64_t, char>`, benchmarked)

| Pattern         | kntrie | std::map | std::unordered_map | Ratio (map) |
|-----------------|--------|----------|--------------------|-------------|
| Random 100K     | 9.6    | 59.5     | 64.6               | 6.2×        |
| Sequential 100K | 1.2    | 62.2     | 64.6               | 51.8×       |
| Dense16 79K     | 1.5    | 61.9     | 64.0               | 41.3×       |
| Random 1M       | 9.5    | 63.7     | 64.5               | 6.7×        |

### Read latency (ns/lookup, `kntrie<uint64_t, uint64_t>`, 100K random keys)

| Container          | ns/read |
|--------------------|---------|
| kntrie             | 63      |
| std::map           | 282     |
| std::unordered_map | 23      |

kntrie is 4.5× faster than std::map (no pointer chasing) and 2.7× slower than
unordered_map (hash + single probe vs. multi-level trie descent). On sequential
data, kntrie matches or beats unordered_map due to skip compression reducing
effective depth to 1.

### Where the density comes from

1. **Flat sorted arrays** — compact leaves have zero per-node pointer overhead
2. **Suffix narrowing** — keys stored in 1/2/4/8-byte types matched to remaining
   bit width
3. **Bitmap compression** — 32-byte bitmap + dense pointers vs. 2048-byte full arrays
4. **Skip compression** — shared prefixes stored once (8 bytes) instead of replicated
5. **Inline values** — trivially-copyable values ≤ 8 bytes stored directly, no
   indirection

### Trade-offs

- **Insert throughput** is lower than hash maps: each insert into a compact leaf
  requires binary search, sorted-array shift, index rebuild, and potentially
  reallocation. This is dominated by allocator cost at scale.
- **Constant factors** on lookup are higher than hash maps for uniformly random keys,
  where the trie's multi-level descent cannot be shortcut.
- **Key type restriction** — keys must be integral. Extending to fixed-width byte
  strings is architecturally possible but not implemented.
- **Memory fragmentation** — node reallocation on every insert (to maintain sorted
  flat arrays) creates allocator pressure. Custom arena/pool allocators via the ALLOC
  parameter mitigate this.
