# kntrie — Concepts & Design

## 1. What Is a Trie?

A trie (from "re**trie**val") is a tree where keys are decomposed into a sequence of
smaller pieces and each piece selects a branch at the corresponding level. For integer
keys, the natural decomposition is by bit groups.

### Simple radix trie (background)

Consider a 16-bit key `0xAB34`. A 4-bit radix trie would break it into four nibbles:

```
Level 0:  0xA  ──►  branch A
Level 1:  0xB  ──►  branch B
Level 2:  0x3  ──►  branch 3
Level 3:  0x4  ──►  leaf → value
```

Each internal node is a 16-element array (one slot per nibble value). This is simple
but wastes enormous space: most slots are NULL. For 64-bit keys you'd need up to 16
levels of 16-slot arrays.

### What kntrie does differently

kntrie uses an **adaptive, width-varying trie** that processes keys 16 bits at a time
(split into two 8-bit halves), with three key innovations:

1. **Compact leaves** — small populations are stored as sorted arrays, not tree nodes
2. **Bitmap-compressed split nodes** — sparse 256-way fanout uses bitmaps instead of
   pointer arrays, storing only the occupied slots
3. **Skip (prefix) compression** — when all entries in a subtree share a common
   prefix, levels are skipped entirely and the shared prefix is stored once

---

## 2. Key Encoding

Before any trie operations, user keys are converted to a 64-bit **internal key**:

```
internal = user_key << (64 - key_bits)
```

This left-aligns the key so the most-significant bits are always at bit 63. For a
32-bit key, the internal representation has the key in bits [63:32] and zeros in
[31:0].

### Signed key support

Signed keys are transformed so that their natural numeric order matches unsigned
comparison order. This is done by flipping the sign bit:

```
result ^= (1 << (key_bits - 1))
```

After this XOR, INT_MIN maps to 0x0000... and INT_MAX maps to 0xFFFF..., preserving
sort order for unsigned comparison.

### Suffix storage types

Remaining key bits at any level are stored in the **narrowest type** that fits:

| Remaining bits | Storage type |
|---------------|-------------|
| 1–8           | `uint8_t`   |
| 9–16          | `uint16_t`  |
| 17–32         | `uint32_t`  |
| 33–64         | `uint64_t`  |

This is critical for memory compression — at a leaf holding entries with only 8 bits
remaining, each suffix costs 1 byte instead of 8.

---

## 3. The 16-Bit Level Architecture

kntrie processes keys **16 bits per level**. For a 64-bit key, that gives a maximum
of 4 levels (64/16 = 4). Each 16-bit chunk is further split into two 8-bit halves:
a **top byte** and a **bottom byte**.

```
64-bit key:
┌──────────┬──────────┬──────────┬──────────┐
│ Level 0  │ Level 1  │ Level 2  │ Level 3  │
│ bits     │ bits     │ bits     │ bits     │
│ [63:48]  │ [47:32]  │ [31:16]  │ [15:0]   │
└──────────┴──────────┴──────────┴──────────┘

Each 16-bit level:
┌────────┬────────┐
│ Top 8  │ Bot 8  │
│ (0xHH) │ (0xLL) │
└────────┴────────┘
```

At each level, a node is in one of two states:

- **Compact leaf** — a flat sorted array of (suffix, value) pairs
- **Split node** — a bitmap-indexed two-tier structure: top-8 bitmap → bottom nodes

---

## 4. Node Types

### 4.1 Node Header

Every node begins with an 8-byte header:

```
struct NodeHeader {        // 8 bytes total
    uint32_t count;        // Total entry count
    uint16_t top_count;    // Number of occupied top-8 buckets (split nodes)
    uint8_t  skip;         // Number of 16-bit levels to skip (prefix compression)
    uint8_t  flags;        // Bit 0: is_leaf, Bit 1: is_split
};
```

When `skip > 0`, a second 8-byte word follows immediately, storing the compressed
prefix. So the header occupies either 8 or 16 bytes.

### 4.2 Compact Leaf

The simplest node type. Used when a subtree has ≤ 4096 entries.

```
┌─────────────────┐
│ Header (8–16B)  │
├─────────────────┤
│ idx1 samples    │  (only if count > 256)
│ idx2 samples    │  (only if count > 16)
│ sorted keys     │  (suffix_type × count)
├─────────────────┤
│ values          │  (value_slot × count)
└─────────────────┘
```

Keys are the **full remaining suffix** at this level — not just one 8-bit chunk but
all remaining bits. This is a critical design choice: a compact leaf at level 0 of a
64-bit key stores full 64-bit suffixes; a compact leaf at level 2 stores 32-bit
suffixes. This means a compact leaf can represent entries without any child pointers
at all.

**Flags:** `is_leaf = 1, is_split = 0`

### 4.3 Split Node (Top Level)

When a compact leaf exceeds COMPACT_MAX (4096) entries, it converts to a split node.
The split node uses the top 8 bits of the current 16-bit chunk to index into buckets:

```
┌─────────────────┐
│ Header (8–16B)  │
├─────────────────┤
│ top_bitmap      │  256-bit bitmap (32 bytes) — which top-8 values exist
├─────────────────┤
│ bot_is_leaf_bm  │  256-bit bitmap (32 bytes) — which bottoms are leaves
│                 │  (omitted at BITS=16, where all bottoms are leaves)
├─────────────────┤
│ child pointers  │  one uint64_t per set bit in top_bitmap
│ (packed dense)  │  points to bottom nodes
└─────────────────┘
```

The top_bitmap tells us which of the 256 possible top-8 values have entries.
The child pointer array is **dense** — only occupied slots are stored. To find
the pointer for top-8 value `T`, you check `top_bitmap.has_bit(T)` then compute
the array index via `popcount` of bits below `T`.

**Flags:** `is_split = 1`; `is_leaf` indicates whether any bottoms are leaf-type.

### 4.4 Bottom Leaf (bot_leaf)

Each child of a split node handles entries sharing the same top-8 bits. A bottom
leaf stores those entries with their remaining suffixes.

**At BITS = 16** (terminal level), only 8 bits remain after the top-8 split.
The bottom leaf uses a bitmap for O(1) lookup:

```
┌──────────────────┐
│ bitmap (32B)     │  256-bit: which bottom-8 values exist
├──────────────────┤
│ values (dense)   │  one value per set bit, in bitmap order
└──────────────────┘
```

**At BITS > 16**, more than 8 bits remain. The bottom leaf is list-based:

```
┌──────────────────┐
│ count (8B pad)   │
├──────────────────┤
│ idx1 samples     │
│ idx2 samples     │
│ sorted suffixes  │  (BITS-8)-bit suffixes
├──────────────────┤
│ values           │
└──────────────────┘
```

These suffixes are the remaining key bits **after** the top 8 have been consumed by
the split node's bitmap.

### 4.5 Bottom Internal (bot_internal)

When a bottom leaf exceeds BOT_LEAF_MAX (4096) entries, it converts to a bottom
internal node. This adds a second 8-bit bitmap level:

```
┌──────────────────┐
│ bitmap (32B)     │  256-bit: which bottom-8 values have children
├──────────────────┤
│ child pointers   │  one uint64_t per set bit → child nodes at next level
└──────────────────┘
```

Each child pointer leads to a node at (BITS - 16), completing the full 16-bit
level of trie descent.

---

## 5. Skip (Prefix) Compression

When all entries in a subtree share the same 16-bit chunk at some level, there's no
branching to be done — the level is redundant. kntrie detects this and **skips** the
level entirely.

### How it works

Instead of creating a split node with exactly one bucket, kntrie:

1. Records the shared 16-bit prefix in the node's prefix field
2. Increments the `skip` counter
3. Stores the data as if the level didn't exist

Multiple consecutive uniform levels can be combined: `skip = 2` means 32 bits of
shared prefix are stored in a single 64-bit prefix word, and lookup jumps forward
by 2 levels.

### Example: sequential keys 0..99999

All these keys share the same upper 48 bits (they all fit in ~17 bits). kntrie
detects three uniform 16-bit levels and stores `skip = 3` with the shared prefix.
The actual data is stored as a compact leaf with 16-bit suffixes. Result: **1.2
bytes/entry** instead of the 64+ bytes/entry that std::map uses.

### Prefix mismatch on insert

If a new key doesn't match the stored prefix, the skip structure must be split.
The algorithm finds the first differing 16-bit chunk, creates a new split node at
that point, and divides the old node and new key into separate branches. The
remaining common prefix (if any) is preserved as a shorter skip on both children.

---

## 6. Bitmap256 — Compressed 256-Way Dispatch

The `Bitmap256` struct is the core primitive for sparse 256-way branching. It's a
256-bit bitmap stored as four `uint64_t` words (32 bytes total).

### Operations

**`has_bit(index)`** — test if a value is present: single word load + bit test.

**`find_slot(index, &slot)`** — combined presence check + position calculation.
Given a value `index` (0–255), determine both (a) whether it's present and (b)
its position in the dense child array. This uses `popcount` to count set bits below
the target:

```cpp
// Which 64-bit word contains the target bit?
int word = index >> 6;      // 0..3
int bit  = index & 63;      // position within word

// Shift so target bit is at MSB; check it
uint64_t before = words[word] << (63 - bit);
if (!(before & (1ULL << 63))) return false;  // bit not set

// Count bits before target = array index
slot = popcount(before) - 1;              // bits in same word
slot += popcount(words[0]) if word > 0;   // full words before
slot += popcount(words[1]) if word > 1;
slot += popcount(words[2]) if word > 2;
```

The branchless accumulation (`slot += pc & -int(word > N)`) avoids conditional
branches — the expression `-int(true)` is all-ones (acts as no-op mask) and
`-int(false)` is zero (zeroes out the addition).

**`slot_for_insert(index)`** — like find_slot but counts bits strictly *below* the
target, for determining where to insert a new element.

**`find_next_set(start)`** — iterate set bits from a starting position, used for
traversal and cleanup. Masks out bits below `start` then uses `countr_zero` to find
the next set bit.

### Why bitmaps instead of arrays?

A naive 256-way node would use a 2048-byte pointer array (256 × 8 bytes). With
bitmaps, if only 10 of 256 values are present, we store 32 bytes (bitmap) +
80 bytes (10 pointers) = 112 bytes. That's 18× smaller.

At the terminal level (BITS = 16), the bottom bitmap stores presence of 8-bit
suffixes directly, enabling O(1) lookup with no child pointers at all — just a
bitmap + dense value array.

---

## 7. Indexed Linear Search (idx_search)

kntrie replaces classical binary search with a **two-level indexed linear scan**.
This is designed for sorted arrays up to 4096 elements and optimizes for CPU cache
behavior.

### The problem with binary search

Binary search on a sorted array is O(log n) in comparisons but has poor cache
behavior: each comparison jumps to a distant memory location. For arrays of
1000+ elements, this means several cache misses per lookup.

### Layout: `[idx1][idx2][keys]`

The sorted keys are stored with two layers of sampled indices prepended:

```
idx1:  samples every 256th key    (present only if count > 256)
idx2:  samples every 16th key     (present only if count > 16)
keys:  full sorted key array
```

For example, with 1000 entries:
- `idx1` is empty (1000 ≤ 256? No, so idx1 has ⌈1000/256⌉ = 4 entries)
- Actually: idx1 has 4 entries: `keys[0], keys[256], keys[512], keys[768]`
- `idx2` has ⌈1000/16⌉ = 63 entries: `keys[0], keys[16], keys[32], ...`
- `keys` has all 1000 entries

### Search procedure

```
1. Linear scan idx1 (≤16 entries) → narrows to a 256-element block
2. Linear scan idx2 (≤16 entries within that block) → narrows to a 16-element block
3. Linear scan keys (≤16 entries) → exact match check
```

Each step scans at most 16 elements with a tight forward loop:

```cpp
static int idx_subsearch(const K* start, int count, K key) {
    [[assume(count <= 16)]];
    const K* run = start;
    const K* end = start + count;
    do {
        if (*run > key) break;
        run++;
    } while (run < end);
    return (run - start) - 1;
}
```

### Why this is fast

- **Linear scans up to 16 elements** are extremely cache-friendly — the CPU
  prefetcher handles sequential access perfectly
- **The `[[assume(count <= 16)]]` hint** lets the compiler generate tight,
  potentially unrolled code with no loop overhead
- **Only 3 scans** are ever needed: worst case is 16 + 16 + 16 = 48 comparisons,
  but all sequential and within a few cache lines
- **idx1 and idx2 are small** and likely stay in L1/L2 cache across repeated lookups

For smaller arrays (≤ 16 elements), the index layers don't exist and it's a single
direct linear scan — zero overhead.

---

## 8. Value Storage

Values are stored in one of two modes, selected at compile time:

**Inline** (`sizeof(VALUE) ≤ 8` and trivially copyable): the value is stored
directly in the value slot alongside the keys. No heap allocation per value.

**Pointer** (larger or non-trivial values): a pointer to a heap-allocated copy
is stored. Each value requires a separate allocation.

The `value_slot_type` is either `VALUE` or `VALUE*`, determined by:

```cpp
static constexpr bool value_inline =
    sizeof(VALUE) <= 8 && std::is_trivially_copyable_v<VALUE>;
using value_slot_type = std::conditional_t<value_inline, VALUE, VALUE*>;
```

This means `kntrie<uint64_t, char>` stores char values inline using just 1 byte per
value slot (plus alignment padding at the array level), while
`kntrie<uint64_t, std::string>` would heap-allocate each string.

---

## 9. Node Transitions

Nodes evolve through a lifecycle as entries are added:

```
                   count > COMPACT_MAX (4096)
Compact Leaf  ─────────────────────────────────►  Split Node
                                                    │
                                                    │ per bucket:
                                                    ▼
                                                 Bot Leaf
                                                    │
                                   count > BOT_LEAF_MAX (4096)
                                                    │
                                                    ▼
                                               Bot Internal
                                                    │
                                                    │ children are nodes
                                                    │ at (BITS - 16)
                                                    ▼
                                              Next-level node
                                             (Compact Leaf or Split)
```

### Compact → Split conversion

When a compact leaf reaches 4096 entries:

1. All entries are bucketed by their top 8 bits
2. A split node is allocated with a bitmap and dense pointer array
3. Each bucket becomes a bottom leaf

**Special case:** if all entries share the same top 8 bits AND the same next 8 bits,
the conversion is replaced by **skip compression** — a single level is skipped and
the process recurses at (BITS - 16).

### Bot Leaf → Bot Internal conversion

When a bottom leaf reaches 4096 entries:

1. Entries are sub-bucketed by their next 8 bits
2. The bottom leaf is replaced with a bottom internal node
3. Each sub-bucket becomes a compact leaf at the next trie level

---

## 10. Lookup Path (find)

Finding a key follows this path:

```
1. Convert key to internal representation
2. Read root node header
3. At each level:
   a. If skip > 0: compare 16-bit chunk against stored prefix
      - Mismatch → not found
      - Match → advance to next 16-bit chunk (no node traversal)
   b. If compact leaf: idx_search on suffix array → found/not found
   c. If split node:
      - Extract top 8 bits → bitmap lookup → child pointer
      - If bottom is leaf:
        · BITS=16: bitmap lookup on bottom 8 bits → value
        · BITS>16: idx_search on bottom suffixes → value
      - If bottom is internal:
        · Extract next 8 bits → bitmap lookup → child pointer
        · Recurse into child at (BITS - 16)
```

The find path is implemented via tail-recursive template functions that unwind at
compile time. For a 64-bit key, the compiler generates specialized code for
BITS ∈ {64, 48, 32, 16}, with each level knowing exactly what types and shifts to
use.

---

## 11. Template Architecture

kntrie makes heavy use of C++ templates to specialize at compile time:

- **`template<int BITS>`** — all node operations are parameterized by remaining bit
  width. This eliminates runtime branching on level depth.
- **`suffix_traits<BITS>`** — selects the narrowest integer type for key storage at
  each level. At 48 remaining bits, suffixes are `uint64_t`; at 16 bits, `uint16_t`;
  at 8 bits, `uint8_t`.
- **`if constexpr`** — used extensively to eliminate dead code paths. For example,
  bot_is_leaf_bitmap doesn't exist when BITS = 16, and the compiler knows this.
- **`requires` clauses** — separate base cases (BITS ≤ 0, BITS = 16) from recursive
  cases (BITS > 16) to prevent invalid template instantiation.

The result is that for `kntrie<uint64_t, char>`, the compiler generates exactly four
specialized code paths (one per 16-bit level), each with the correct types, sizes,
and control flow baked in.

---

## 12. Memory Characteristics

### Bytes per entry (benchmarked, uint64_t key, char value)

| Pattern        | kntrie | std::map | std::unordered_map |
|----------------|--------|----------|--------------------|
| Random 100K    | 9.6    | 59.5     | 64.6               |
| Sequential 100K| 1.2    | 62.2     | 64.6               |
| Dense16 79K    | 1.5    | 61.9     | 64.0               |
| Random 1M      | 9.5    | 63.7     | 64.5               |

### Where the savings come from

1. **No per-node pointers** — compact leaves store keys and values in flat arrays
   with zero pointer overhead
2. **Narrow suffix types** — at lower levels, keys are stored in 1–4 byte types
   instead of always 8 bytes
3. **Bitmap compression** — sparse 256-way nodes cost 32 bytes + 8 bytes per
   occupied slot, not 2048 bytes for a full array
4. **Skip compression** — common prefixes are stored once (8 bytes) instead of
   repeated across thousands of entries
5. **Inline values** — small values are stored directly, no heap pointer per entry

### Trade-offs

- Insert is slower than hash maps (involves reallocation and copying of sorted arrays)
- No iteration support currently (no iterator)
- Delete is not implemented
- Keys must be integral types

---

## 13. Comparison with Other Structures

| Property | kntrie | std::map (RB-tree) | std::unordered_map |
|----------|--------|--------------------|--------------------|
| Lookup complexity | O(1)* | O(log n) | O(1) amortized |
| Lookup ns (100K random) | ~61 | ~274 | ~25 |
| Memory/entry | 1–17B | 54–64B | 64–72B |
| Cache behavior | Good (sequential scans) | Poor (pointer chasing) | Variable (hash chains) |
| Ordered iteration | Possible† | Yes | No |
| Key types | Integers | Any comparable | Any hashable |

\* O(key_bits / 16) which is bounded by 4 for 64-bit keys — effectively constant.

† The trie structure inherently stores keys in order, but an iterator is not yet
implemented.
