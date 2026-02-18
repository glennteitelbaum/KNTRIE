# kntrie Concepts

## TRIE

A TRIE (from "retrieval") is a tree structure where each node represents a portion of a key rather than the whole key. In a string TRIE, each level might branch on one character. In a numeric TRIE, each level branches on some chunk of bits. The path from root to leaf spells out the complete key.

This gives TRIE's a fundamental property that distinguishes them from comparison-based trees: lookup cost depends on the key's length, not the number of entries. A TRIE with 100 entries and a TRIE with 100 million entries traverse the same number of levels for the same key.

The classic problems with TRIE's are well known. A naïve implementation that allocates a 256-entry child array at every level wastes enormous memory — most slots are empty, especially near the leaves. Sparse levels dominate. The structure also suffers from pointer chasing: each level requires following a pointer to the next node, and those nodes are scattered across the heap with no cache locality guarantees. For small key populations, the overhead of multiple levels can exceed the cost of a flat sorted search. And for variable-depth TRIE's, the bookkeeping to know what type of node you're looking at, how deep you are, and when you've reached a leaf adds complexity at every step.

## KTRIE

The KTRIE is a TRIE variant designed for compact data and fast reads (implemented with integer keys for kntrie, and variable length strings as keys for kstrie). It introduces three structural ideas that address the classic TRIE problems described above.

A key in the KTRIE is decomposed into three logical regions:

```
KEY = [PREFIX] [BRANCH ...] [SUFFIX]
```

**PREFIX** is a run of key bytes that all entries in a subtree share. Rather than creating a chain of single-child nodes to traverse this common prefix — as a naïve TRIE would — the KTRIE captures the entire shared prefix in a single node. This eliminates the wasted memory and latency of redundant intermediate levels. When a lookup reaches a prefix-captured node, it compares the full prefix in one step and either continues or exits immediately.

**BRANCH** nodes are the KTRIE's equivalent of TRIE nodes — they fan out to up to N children based on a fixed-width chunk of the key. A TRIE node that branches on one byte has up to 256 children. The KTRIE may use one or more BRANCH levels depending on key width and data distribution. Each BRANCH node routes the lookup one step closer to the leaf by consuming a fixed number of key bits. The key difference from a naïve TRIE is that BRANCH nodes only exist where the data actually fans out — PREFIX capture absorbs the levels where it doesn't.

**SUFFIX** is the remaining portion of the key after all BRANCH levels have been consumed. Rather than continuing to subdivide into deeper BRANCH nodes, the KTRIE collects entries with different suffixes into a **compact node** — a flat sorted array of suffix/value pairs stored in a single allocation. This directly addresses two TRIE problems: it eliminates pointer chasing for the tail of the key, and it avoids the memory overhead of sparsely-populated BRANCH nodes near the leaves. A compact node holding 100 entries in a contiguous sorted array is far more cache-friendly and memory-efficient than 100 entries scattered across a tree of BRANCH nodes.

**How this improves on a generic TRIE:**

The naïve TRIE has a fixed structure: every level creates a node, every node fans out by the same width, and every key traverses every level. The KTRIE adapts its structure to the data:

- Where keys share a common prefix, PREFIX capture collapses the redundant levels into a single comparison. A subtree where 1000 keys share the same top 4 bytes uses one node instead of four.

- Where the key space is sparse at a given level, BRANCH nodes use compressed representations rather than allocating the full fan-out width. Only children that actually exist consume memory.

- Where a subtree is small enough, compact nodes absorb all remaining SUFFIX's into a flat array, avoiding further branching entirely. The threshold for "small enough" determines how deep the TRIE actually goes for a given dataset — small datasets may never create BRANCH nodes at all.

The result is a structure whose depth and memory usage adapt to the actual key distribution rather than being fixed by the key width. Dense, clustered key ranges are captured by PREFIX nodes and compact leaves. Sparse, spread-out ranges are handled by BRANCH nodes that only allocate for children that exist.

---

## OPTIMIZATIONS

### VALUE STORAGE

Values stored in the kntrie are handled through a compile-time trait, `value_traits<VALUE, ALLOC>`, that selects one of three categories based on size, trivial-copyability, and move semantics. The selection is entirely `constexpr` — the compiler generates specialized code for each category with zero runtime dispatch overhead.

| Cat | IS_INLINE | HAS_DESTRUCTOR | Storage | Example |
|-----|-----------|----------------|---------|---------|
| A | yes | no | normalized u8/u16/u32/u64/array<u64,N> | int, float, POD |
| B | yes | yes | real T | string, vector, map |
| C | no | yes | pointer to T | >64B or !nothrow_move |

**Category A: Trivial inline.** Applies when `sizeof(VALUE) <= 64` and the type is trivially copyable. The value is **normalized** to a fixed-width type to reduce template instantiations: 1 byte → `uint8_t`, 2 → `uint16_t`, 3–4 → `uint32_t`, 5–8 → `uint64_t`, 9–64 → `std::array<uint64_t, ceil(sz/8)>`. This means `kntrie<uint64_t, int>` and `kntrie<uint64_t, float>` share the same internal implementation. Values are stored directly in the slot array — no indirection, no heap allocation, no destructor call. Conversion between `VALUE` and `slot_type` uses `std::bit_cast`. For the common case of `kntrie<uint64_t, uint64_t>`, each value is just 8 bytes in a contiguous array with excellent cache behavior.

**Category B: Non-trivial inline.** Applies when `sizeof(VALUE) <= 64` and the type is nothrow-move-constructible but NOT trivially copyable (e.g. `std::string` at 32 bytes). The value is stored directly in the slot array as the real `T` to avoid a pointer indirection on read. `slot_type` is `VALUE`. The trade-off: internal moves must use per-element `std::move` (which zeroes the source — measured at ~1.6× the cost of memmove), and destructors must be called on erase and node teardown. This cost is paid on writes to save one pointer chase on every read.

**Category C: Heap-allocated pointer.** Applies when `sizeof(VALUE) > 64` or the type is not nothrow-move-constructible. The kntrie allocates a `VALUE*` on the heap via the rebind allocator, constructs the value in place, and stores the 8-byte pointer in the slot array. `slot_type` is `VALUE*`. Since a pointer is trivially copyable, the compiler optimizes `std::move` of pointer arrays to memmove automatically. Destroy must deallocate the heap object on erase or node teardown.

The two key compile-time booleans that drive all internal dispatch:

```
IS_INLINE      = (trivially_copyable || nothrow_move) && sizeof <= 64   // A,B: true   C: false
HAS_DESTRUCTOR = !trivially_copyable || sizeof > 64                     // A: false     B,C: true
```

The `slot_type` is either a normalized type (A), `VALUE` (B), or `VALUE*` (C), and all node code operates on `slot_type` uniformly. The `as_ptr` helper returns a `const VALUE*` regardless — for A, it's a `bit_cast` of the slot address; for B, it's `&slot`; for C, it's the pointer itself.

#### Slot movement: std::copy vs std::move

Two cases based on whether source and destination can overlap:

**No overlap (realloc, new node, split):** `std::copy` — compiler optimizes to `memcpy` for trivial types. Faster than memmove since it doesn't need to check direction.

**Overlap (in-place insert gap, erase compaction):** `std::move` / `std::move_backward` — compiler optimizes to `memmove` for trivial types (handles overlap).

* A: compiler optimizes `std::copy` to `memcpy`, `std::move` to `memmove` (trivially copyable)
* B: real copy/move-assigns; C++26 trivial relocation may optimize further
* C: `std::copy`/`std::move` of pointers, compiler optimizes to `memcpy`/`memmove`

All dispatch is `if constexpr` — dead branches are eliminated at compile time.

#### Insert / erase / destroy splits

| Operation | A | B | C |
|---|---|---|---|
| Insert (store) | raw write / `bit_cast` | placement-new | alloc + placement-new |
| Erase (single) | nothing | dtor | dtor + dealloc |
| Destroy leaf | nothing | dtor all live slots | dtor + dealloc all live slots |

#### Consumer responsibilities

**kntrie** (API boundary): Normalizes both key and value before forwarding to `kntrie_impl`. Calls `store(std::move(val), alloc)` to get a `slot_type` on insert. Calls `as_ptr(slot)` / `bit_cast` to get `const VALUE*` on find/iterator deref. Never destroys directly.

**_impl** (node ownership): Owns all destruction. On erase: calls `destroy(slot, alloc)` on the removed slot, then compacts. On clear/destructor: walks all leaf nodes, calls `destroy` on every occupied slot if `HAS_DESTRUCTOR`, then frees node memory.

**_ops** (layout/sizing): Uses `sizeof(slot_type)` for node size calculations. Does not care about move or destroy semantics.

**_compact / _bitmask** (node internals): No-overlap operations (realloc, new node builds) use `std::copy`. In-place shifts (insert gap, erase compaction) use `std::move` / `std::move_backward`. Insert and destroy dispatch on `if constexpr (IS_INLINE && !HAS_DESTRUCTOR)` (A), `if constexpr (IS_INLINE && HAS_DESTRUCTOR)` (B), `if constexpr (!IS_INLINE)` (C).

### MEMORY HYSTERESIS

Bitmask node allocations are managed through a size-class system that balances memory waste against reallocation frequency. Compact nodes use exact sizing — slot counts are rounded to the next power of two via `std::bit_ceil`, and the u64 allocation is computed exactly from `(header + aligned_keys + aligned_values) / 8`.

**Quarter-step size classes (bitmask nodes only).** Up to 8 u64s, allocations are exact. Beyond that, sizes snap to quarter-steps within each power-of-two range:

| Range | Step | Classes |
|-------|------|---------|
| 1–8 | 1 | 1, 2, 3, 4, 5, 6, 7, 8 |
| 9–16 | 2 | 10, 12, 14, 16 |
| 17–32 | 4 | 20, 24, 28, 32 |
| 33–64 | 8 | 40, 48, 56, 64 |
| 65–128 | 16 | 80, 96, 112, 128 |

The worst-case overhead is roughly 25%. This padding creates room for in-place insert and erase operations — a node allocated at 48 u64s when it only needs 40 has extra slots that can absorb mutations without reallocation.

**Shrink hysteresis (bitmask nodes).** A bitmask node only shrinks when its allocated size exceeds the needed size by more than 2 size-class steps. This prevents oscillation: if a node sits right at a boundary, alternating insert/erase won't trigger repeated realloc cycles. The `should_shrink_u64(allocated, needed)` function computes the threshold by stepping up twice from `round_up_u64(needed)` and checking whether the current allocation exceeds that.

Compact nodes shrink when entries drop below half the current power-of-two slot count.

### KEY REPRESENTATION

All key types — `uint16_t`, `int32_t`, `uint64_t`, signed or unsigned — are transformed into a canonical 64-bit internal representation before any kntrie operation. This transformation is the foundation that makes the entire structure work uniformly.

**Signed key handling.** Signed integers have a problem: their binary representation doesn't sort in the same order as their numeric value. Negative numbers have the high bit set but should sort before positives. The fix is an XOR flip of the sign bit:

```
r ^= 1ULL << (key_bits - 1);
```

This maps `INT_MIN → 0`, `0 → 0x80000000`, `INT_MAX → 0xFFFFFFFF` — a monotonically increasing sequence that matches numeric order. The flip is its own inverse, so converting back is the same XOR.

**Left-alignment into 64 bits.** After the sign flip (if signed), the key is shifted left so its most significant bit sits at bit 63:

```
r <<= (64 - key_bits);
```

A `uint16_t` key `0xABCD` becomes `0xABCD000000000000`. A `uint32_t` key `0x12345678` becomes `0x1234567800000000`. A `uint64_t` stays as-is.

This left-alignment is what makes the 8-bit chunking work. The root always extracts bits 63–56 (byte at position `ik >> 56`). The next level extracts the next 8 bits below that. Regardless of whether the original key was 16, 32, or 64 bits wide, the same bit extraction logic applies — shorter keys simply have fewer meaningful levels before they bottom out. A `uint16_t` key has 2 bytes of real data followed by zeros; a `uint64_t` has 8. The template parameter `BITS` tracks how many significant bits remain at each level, and the recursion stops when `BITS` reaches the terminal threshold.

Without this normalization, every node operation would need key-width-specific logic. With it, `extract_top8<BITS>(ik)` and `extract_suffix<BITS>(ik)` work identically for all key types — the bits are always in the same place.

### SKIP PREFIX

A traditional TRIE creates a node at every level, even when a long run of keys shares the same intermediate bytes. If 1000 keys all have the same top 3 bytes, a naïve TRIE still creates 3 levels of single-child nodes to traverse that common prefix. This wastes memory and adds latency.

Skip prefix compression is how the kntrie implements the PREFIX capability of the KTRIE data structure. Each node stores a `skip` count (0–3, encoded in 2 bits of the header flags) and a `prefix` value (up to 48 bits in the second header u64). A skip of 1 means this node captures 16 bits of the key that would normally require an entire BRANCH level. A skip of 2 collapses two levels (32 bits), and so on.

During lookup, the prefix is checked against the corresponding bits of the search key. If they don't match, the key isn't in this subtree — an early exit. If they match, the search continues at the effective BITS level, having skipped past the captured prefix.

**The shortcircuit tradeoff.** For `find`, skip handling has a performance tension with code size. The ideal fast path would check `skip == 1` and jump directly to the BITS level that represents one skip — for a 48-bit node, that means jumping to BITS=32 logic. This avoids the overhead of the generic recursive descent through levels that the skip already validated. The code does this:

```cpp
if constexpr (BITS >= 48) {
    if (h.skip() == 1) [[unlikely]]
        return find_dispatch_<32>(node, h, ik);
}
return find_dispatch_<16>(node, h, ik);
```

The `[[unlikely]]` annotation reflects that most nodes don't have skips, keeping the common no-skip path fast. But this shortcircuit instantiates additional template specializations — each explicit jump target generates code. The tradeoff is real: skip shortcuts make find faster on compressed paths at the cost of larger binary size. For insert and erase, where the operation cost is dominated by allocation and memmove, the code uses convergent dispatch instead (see next section), accepting the extra branch overhead to keep code size manageable.

### RECURSIVE TEMPLATES

The kntrie is parameterized by `BITS` — the number of significant key bits remaining at each level. A `uint64_t` key starts at BITS=56 (after the root consumes the top 8), and each branch level consumes 16 bits (8 for the top index, 8 for the bottom index), so levels go 56 → 40 → 24 → 16 terminal. For `uint32_t`: 24 → 16 terminal. For `uint16_t`: 8 → terminal immediately.

Every function that operates on a node — `find`, `insert`, `erase`, `remove_all`, `collect_stats` — is templated on `BITS` and recursively instantiates itself at `BITS - 16` when descending into a child. This gives the compiler full knowledge of the layout at each level, enabling it to compute offsets, inline aggressively, and eliminate dead code for impossible cases.

The cost is template bloat. Each level instantiates a complete set of functions. For `uint64_t`, there are 4 levels (56, 40, 24, 16), so every operation gets 4 copies. Multiply by the number of operations (find, insert variants, erase, remove_all, stats), and the binary grows quickly.

**Convergent dispatch** is the mitigation. Instead of each level having its own fully-specialized implementation, operations that aren't performance-critical converge through a dispatch function that peels off one skip level at a time:

```cpp
template<int BITS, bool INSERT, bool ASSIGN>
InsertResult insert_dispatch_(uint64_t* node, NodeHeader* h,
                              uint64_t ik, VST value, int skip)
    requires (BITS > 16)
{
    if (skip > 0) [[unlikely]]
        return insert_dispatch_<BITS - 16, INSERT, ASSIGN>(
            node, h, ik, value, skip - 1);
    return insert_at_bits<BITS, INSERT, ASSIGN>(node, h, ik, value);
}
```

This still instantiates at each BITS level, but the heavy lifting (`insert_at_bits`) only runs when the skip counter reaches zero. The compiler can see that the skip-decrement path is a thin trampoline.

**The selective approach.** Find stays on the fast path — fully specialized at each BITS level with direct shortcircuit jumps for skip. The branch predictor sees one predictable pattern per call. Insert and erase use convergent dispatch because their cost is dominated by allocation, memmove, and node restructuring. Saving one branch in a function that calls `alloc_node` and does a bulk copy is noise. But saving one branch in `find`, which is often just a few pointer dereferences and a linear scan, is measurable.

This is a real engineering tradeoff: the find path is roughly 2x the code size it would be with convergent dispatch, but the performance gain justifies it for the operation that matters most.

---

## NODES

### ROOT

The root of the kntrie is a flat array of 256 `uint64_t*` pointers, indexed by the top 8 bits of the internal key (`ik >> 56`). This is not a node in the usual sense — it has no header, no allocation, no bitmap. It's a fixed-size array embedded directly in the `kntrie3` object.

This design eliminates an entire level of indirection and branching. The first step of every find is a direct array lookup — no bitmap popcount, no branch on "is this slot occupied." The compiler emits a single indexed load. For insert, it's the same: compute the index, load the pointer, and either descend or create a new child.

The 256-pointer array costs 2 KB, which is negligible for any dataset of meaningful size. It avoids the code complexity of a root node that could be either a compact leaf or a branch node — the root is always the same thing, always the same size, always accessed the same way. This simplifies every entry point (find, insert, erase) by one level of type dispatch, which matters both for code size and for branch predictor pressure.

### HEADER

Every allocated node in the kntrie — compact leaves, split-top nodes, bot-internal nodes, bot-leaf-16 bitmap nodes — begins with the same 16-byte header. This uniformity is what allows generic code to inspect any node without knowing its type in advance.

The header occupies 2 u64s (node[0] and node[1]):

**node[0]: `NodeHeader` (8 bytes)**

| Field | Size | Purpose |
|-------|------|---------|
| `entries` | uint16 | Compact: key/value count. Split/bitmap: child count. |
| `descendants` | uint16 | Total key/value pairs in subtree, capped at 65535. |
| `alloc_u64` | uint16 | Allocation size in u64s (may exceed what entries require). |
| `flags_` | uint16 | Bit 0: is_leaf (0) vs is_bitmask (1). Bits 1–2: skip (0–3). |

**node[1]: prefix (uint64)**

Stores the skip prefix when `skip > 0`. Zero otherwise. Holds up to 3 × 16 = 48 bits of captured key prefix.

**`entries`** means different things depending on node type, but it always answers "how many things are directly stored here." For a compact leaf, it's the count of unique key/value pairs (distinct from the total physical slots, which includes padding). For a split-top or bot-internal, it's the number of occupied bitmap slots (child count).

**`descendants`** tracks the total number of key/value pairs in the entire subtree rooted at this node. This is maintained incrementally: insert adds 1, erase subtracts 1. It's capped at 65535 because the field is 16 bits — once a subtree exceeds that size, the counter saturates and stays at the cap.

**`alloc_u64`** records how many u64s were actually allocated for this node. This may be larger than what the current entry count strictly requires, due to the quarter-step size classes.

**`flags_`** encodes the node type and skip count in a single uint16:
- Bit 0: 0 = compact leaf, 1 = bitmask/split node. This is the primary type discriminator — `is_leaf()` checks this single bit.
- Bits 1–2: skip count (0–3). How many 16-bit prefix chunks are stored in node[1].
- Bits 3–15: reserved.

### BITMASK

Bitmask nodes are the implementation of the KTRIE's BRANCH nodes — they handle the levels where the TRIE fans out by 256, dispatching on 8-bit key chunks. Like a traditional TRIE node, each bitmask node routes lookup by one key chunk, but the representation is compressed.

**The core problem.** A naïve 256-way fan-out as in a traditional TRIE would require a 256-entry array of pointers (2 KB) at every level, with most slots empty. The bitmap compresses this: a 256-bit bitmap (32 bytes, 4 u64s) records which of the 256 possible children exist, and a dense array holds only the pointers for children that are present. Given an index into the 256 space, the bitmap tells you whether that child exists and, if so, where it sits in the dense array.

**The 256-bit bitmap.** The bitmap is stored as 4 uint64 words. Bit `i` of the bitmap represents child index `i`. To locate which word and which bit within that word:

```
word_index = i >> 6       // which of the 4 u64s (i / 64)
bit_index  = i & 63       // which bit within that word (i % 64)
```

Checking presence is straightforward: `words[word_index] & (1ULL << bit_index)`.

**Computing the dense array position.** The hard part is: given that child `i` exists, what is its position in the packed array? The answer is the number of set bits *before* index `i` in the bitmap — the popcount of all bits below position `i`.

This is where the shift-left trick comes in. Consider just the word containing the target bit:

```cpp
uint64_t before = words[w] << (63 - b);
```

This left-shift moves the target bit to bit 63 (the most significant position) and shifts everything above the target bit out of the word entirely. What remains in `before` is: the target bit at position 63, and all bits at or below the target's original position occupying the lower positions. Everything above is gone.

For example, if `w = 1` and `b = 5`, and `words[1] = 0b...101011`:

```
Original word:     ...  bit7  bit6  bit5  bit4  bit3  bit2  bit1  bit0
                   ...   1     0     1     0     1     0     1     1

Shift left by (63 - 5) = 58 positions:

Result (before):   bit5  bit4  bit3  bit2  bit1  bit0  0  0  0  ...  0
                    1     0     1     0     1     1    0  0  0  ...  0
```

Bits 6 and 7 (and everything above) are gone — shifted out the top. Bits 0 through 5 are now in the upper positions. The target bit (bit 5) is at position 63.

`std::popcount(before)` now counts exactly the set bits from position 0 through position `b` in the original word — including the target bit itself if it was set. In this example: bits 0, 1, 3, and 5 are set → popcount = 4.

To get the full position across all 4 words, add the popcounts of all complete words below the target word. This is done branchlessly:

```cpp
int slot = std::popcount(before);
slot += std::popcount(words[0]) & -int(w > 0);
slot += std::popcount(words[1]) & -int(w > 1);
slot += std::popcount(words[2]) & -int(w > 2);
```

The `& -int(w > N)` trick: when `w > N` is true, `-int(true)` is `-1` which is all-ones in two's complement, so the popcount passes through. When false, `-int(false)` is `0`, so the popcount is masked to zero. No branches.

At this point, `slot` holds a count that includes the target bit (if set). This raw count is then adjusted depending on the operation:

**FAST_EXIT mode** (used by insert and erase): First check whether the target bit is actually set — that's bit 63 of `before`. If it's not set, the child doesn't exist: return -1. If it is set, `slot` includes the target in its count, so subtract 1 to get the 0-based index into the dense array.

```cpp
if (!(before & (1ULL << 63))) return -1;  // not present
slot--;                                     // 0-based dense index
```

**BRANCHLESS mode** (used by find): Don't check the bit with a branch. If the target bit is set, `slot` is the count including the target — this is a 1-based index, and the dense array is laid out with a not-found entry at position 0, so the 1-based index directly addresses the correct child. If the target bit is not set, `slot` is masked to 0:

```cpp
slot &= -int(bool(before & (1ULL << 63)));  // 0 if miss, 1-based if hit
```

Position 0 in the array leads to a guaranteed not-found result. The find continues through it, discovers no matching key, and returns nullptr — all without a conditional branch at the bitmap level.

**UNFILTERED mode** (used for insertion position): The count of set bits strictly *before* the target gives the correct insertion point in the dense array. Subtract 1 if the target bit itself is set, since we want "bits before" not "bits through":

```cpp
slot -= int(bool(before & (1ULL << 63)));
```

**Node layouts at each level.** The kntrie implements each branch level as a two-tier structure: a split-top that dispatches on the upper 8 bits, and a bottom tier that handles the lower 8 bits. This means each branch level consumes 16 bits of the key.

**Split-top node (BITS > 16):**

```
[Header: 2 u64] [Top bitmap: 4 u64] [Bot-is-internal bitmap: 4 u64] [Slot 0: 1 u64] [Children...]
```

The top bitmap tracks which of the 256 top-index values have children. The bot-is-internal bitmap records which of those children are bot-internal nodes (as opposed to bot-leaves — compact nodes holding suffix/value pairs). Slot 0 holds the not-found pointer used by BRANCHLESS lookups. The real children follow at positions 1, 2, 3, ..., packed densely in bitmap order.

**Split-top node (BITS == 16):**

```
[Header: 2 u64] [Top bitmap: 4 u64] [Children...]
```

At BITS=16, every child is a bot-leaf. The bot-is-internal bitmap and slot 0 are omitted since there are no internal children and FAST_EXIT is used instead of BRANCHLESS at this terminal level.

**Bot-internal node:**

```
[Header: 2 u64] [Bitmap: 4 u64] [Slot 0: 1 u64] [Children...]
```

Dispatches on the bottom 8 bits of the 16-bit chunk. Each child is a node at BITS-16 — the next level down. Slot 0 holds the not-found pointer for BRANCHLESS lookups, same as split-top.

**Bot-leaf (BITS > 16):**

A compact node at BITS-8 — the KTRIE's SUFFIX collection. Same layout as any compact leaf (header + sorted keys + values), but the suffix width is 8 bits narrower than the parent's BITS. These are managed by `CompactOps` — the bitmask layer delegates to compact operations.

**Bot-leaf-16 (BITS == 16):**

```
[Header: 2 u64] [Bitmap: 4 u64] [Values...]
```

At the terminal level, the bottom 8 bits are the final key suffix. Since there are only 256 possible suffixes, a bitmap directly encodes presence/absence — no sorted key array needed. The bitmap itself *is* the key storage. Values are packed densely in bitmap order. Lookup is a single `find_slot<FAST_EXIT>`: check the bit, compute the popcount for the slot index, return the value. This is the tightest possible representation for the final level.

### COMPACT

Compact leaf nodes are the implementation of the KTRIE's SUFFIX collection — they store key/value pairs in sorted arrays within a single allocation. They handle the case where a subtree's keys are few enough that a flat sorted structure outperforms further BRANCH subdivision.

The threshold is COMPACT_MAX = 4096, chosen for two reasons. First, 4096 = 256 × 16, which is exactly the maximum that the two-tier jump search (stride 256, then stride 16) can cover — the stride-256 loop touches at most 16 positions, and the stride-16 loop narrows to a 16-element window. Going beyond 4096 would require a third search tier. Second, when a compact node overflows and splits into a BRANCH node with up to 256 children, the average child gets 4096/256 ≈ 16 entries. This avoids creating wastefully small children — 16 entries is a reasonable minimum for a compact node to be worth its header overhead.

**Layout:**

```
[Header: 2 u64] [Sorted keys (aligned to 8)] [Values (aligned to 8)]
```

The keys and values arrays have `total` physical slots, where `total = SlotTable<BITS, VST>::max_slots(alloc_u64)`. This is typically larger than the entry count due to the quarter-step allocation padding. The extra slots are filled with duplicates of adjacent entries — padding that enables in-place mutation.

**Search: nested jump search.** Finding a key in a compact node uses a three-level jump search rather than `std::lower_bound`:

```cpp
const K* p = keys;
for (q = p + 256; q < end; q += 256) { if (*q > key) break; p = q; }
for (q = p + 16;  q < end; q += 16)  { if (*q > key) break; p = q; }
for (q = p + 1;   q < end; ++q)      { if (*q > key) break; p = q; }
```

This performs at most `N/256 + 16 + 16` comparisons, scanning in strides of 256, then 16, then 1. For typical node sizes (up to 4096 entries), this outperforms binary search because:

- The stride-256 loop touches at most 16 cache lines, and they're sequential — the hardware prefetcher tracks this pattern.
- The stride-16 loop narrows to a 16-element window, which fits in a single cache line for most key sizes.
- The final linear scan is branch-predictor-friendly — it's a simple forward sweep with one comparison per iteration.

Binary search (`std::lower_bound`) on the same data would make `log2(4096) = 12` comparisons but access 12 different cache lines in an unpredictable pattern. The jump search trades more comparisons for sequential access, which is the right tradeoff on modern hardware where cache misses cost 50–100x more than a predicted branch.

**Dup tombstone / padding strategy.** The quarter-step allocation classes mean a compact node almost always has more physical slots than entries. Rather than leaving the extra space unused, the node fills it with **dup slots** — copies of adjacent real entries. These dups serve two purposes:

1. **In-place insert.** When a new key arrives and dups exist, one dup is consumed: the nearest dup to the insertion point is found, the intervening entries are shifted by one position to close the gap, and the new key is written into the freed slot. No reallocation. The entry count increments, the total stays the same, and the dup count decreases by one.

2. **In-place erase.** When a key is erased, its run of slots (the real entry plus any adjacent dups of the same key) is overwritten with copies of the neighboring key's value. This converts real entries into dups in O(1) — no memmove of the remaining array. The entry count decrements, the total stays the same, and the dup count increases.

Dups are seeded by `seed_from_real_`, which distributes them evenly among real entries. With `n_entries` real entries and `n_dups` dup slots to fill, it places one dup after every `n_entries / (n_dups + 1)` real entries. This ensures that the nearest dup to any insertion point is at most about `n_entries / n_dups` positions away, bounding the memmove cost.

The dup count is never stored — it's always derived: `total - entries`. The `total` comes from the `SlotTable` lookup using `alloc_u64`, and `entries` is in the header. This keeps the header clean and avoids any possibility of the stored count drifting from reality.

For heap-allocated values (T\*), all dup slots in a run share the same pointer as their neighbor. This means destroy must be called exactly once per unique key, not once per physical slot. The `for_each` and `destroy_and_dealloc` operations skip adjacent duplicate keys to avoid double-free.

**SlotTable.** The mapping from `alloc_u64` to the maximum number of physical slots is computed at compile time via a constexpr lookup table:

```cpp
template<int BITS, typename VST>
struct SlotTable {
    static constexpr auto build() {
        std::array<uint16_t, MAX_ALLOC + 1> tbl{};
        for (size_t au64 = 2; au64 <= MAX_ALLOC; ++au64) {
            size_t avail = (au64 - HEADER_U64) * 8;
            size_t total = avail / (sizeof(K) + sizeof(VST));
            // adjust for alignment...
            tbl[au64] = total;
        }
        return tbl;
    }
    static constexpr auto table = build();
};
```

This is parameterized by `BITS` (which determines the key type's size) and `VST` (the value slot type). For a given allocation size, it computes how many key+value pairs fit after the header, accounting for alignment padding. The result is a direct array lookup: `SlotTable<BITS, VST>::max_slots(alloc_u64)` — no division, no rounding at runtime.

---

## ADDITIONAL OPTIMIZATIONS

### SENTINEL

The kntrie uses a global sentinel node — a statically-allocated, zeroed block of 8 u64s — as the "not found" fallback:

```cpp
alignas(64) inline constinit uint64_t SENTINEL_NODE[8] = {};
```

A zeroed header has `entries = 0`, `flags_ = 0` (which means `is_leaf() == true`, `skip() == 0`). This makes the sentinel a valid empty compact leaf — any operation that reads it sees zero entries and returns "not found."

**At the root:** Empty root slots point to `SENTINEL_NODE` rather than null. The find path loads `root_[ti]`, reads the header, sees `is_leaf()` with 0 entries, and the compact find returns nullptr. No null check needed.

**In bitmask nodes:** Slot 0 in the children array of split-top and bot-internal nodes holds a pointer to `SENTINEL_NODE`. This is the not-found pointer used by BRANCHLESS lookups (described in the bitmask section). When a bitmap lookup misses — the target bit isn't set — the BRANCHLESS popcount logic returns index 0, which dereferences the sentinel. The find continues into the sentinel, finds nothing, and returns nullptr. No conditional branch at the bitmap level.

The sentinel is cache-line-aligned (64 bytes) and `constinit` — it exists at program start with no dynamic initialization. It's never deallocated. Because it's a single global instance, repeated misses on different nodes all hit the same cache line, which stays hot.

### TERMINAL NODES

At BITS=16, the kntrie reaches its terminal level and the structure changes in several ways.

**Split nodes are always created.** A compact leaf at BITS=16 would hold 16-bit suffixes — up to 65536 possible values. But the split-top at BITS=16 only needs a top bitmap + bot-leaf-16 children, which is already an efficient structure. The code forces conversion to split at this level rather than allowing large compact leaves.

**Bot-leaf-16 uses bitmap storage.** The bottom 8 bits of the suffix have only 256 possible values, so a 256-bit bitmap directly records which suffixes are present. There's no sorted key array — the bitmap *is* the key index. Values are stored in a dense array ordered by bitmap position. Lookup is a single `find_slot<FAST_EXIT>`: check the bit, compute the popcount for the slot index, return the value. This is the tightest possible representation for the final level.

**No bot-is-internal bitmap or slot 0.** At BITS=16, every child of the split-top is a bot-leaf-16. There are no bot-internal nodes at this level because there are no further levels to recurse into. The split-top layout drops the second bitmap and slot 0, saving 40 bytes per terminal split node. FAST_EXIT is used instead of BRANCHLESS since the simplified layout has no need for the branchless fallback.

**No dup tombstones.** Bot-leaf-16 nodes are too small for it to pay off. However, allocations use the same quarter-step rounding as all other node types, and insert/erase use in-place paths when the current allocation has room. This gives the same reallocation hysteresis that other nodes enjoy without the dup machinery.

---

## PERFORMANCE

### STD MAP

`std::map` is a red-black tree — a self-balancing binary search tree where each node contains one key-value pair, two child pointers, a parent pointer, and a color bit. On most implementations, each node is a separate heap allocation of about 72 bytes (key + value + 3 pointers + color + alignment padding).

Red-black trees guarantee O(log N) worst-case lookup, insert, and erase through rotation and recoloring rules that keep the tree approximately balanced. The maximum height is 2 × log₂(N), so a million-entry map requires at most ~40 comparisons per lookup.

The fundamental costs are:

**Memory.** Every entry costs ~72 bytes regardless of key or value size. A million uint64_t→uint64_t entries consume ~69 MB in `std::map` versus ~16 MB in kntrie — a 4.3× difference.

**Cache behavior.** Each comparison in a tree traversal follows a pointer to a separately-allocated node. These nodes are scattered across the heap in allocation order, not key order. At scale, nearly every level of the tree is a cache miss. A 20-level traversal in a million-entry map touches 20 random cache lines.

**Sorted iteration.** The red-black tree provides naturally sorted in-order traversal, with O(1) amortized iterator increment and decrement. kntrie provides the same sorted iteration through its structure, but the constant factors differ — kntrie iteration must descend and ascend through multiple node types, while tree iteration follows parent/child pointers.

### KNTRIE

The kntrie's lookup complexity is O(M) where M is the key width in bits — a constant determined at compile time. For `uint64_t`, M=64 means at most 4 levels of BRANCH nodes (56 → 40 → 24 → 16). For `int32_t`, M=32 means at most 2 levels (24 → 16). This is independent of N, the number of entries.

In practice, the effective depth is often less than the maximum because of two mechanisms:

**PREFIX capture** collapses levels where all keys in a subtree share a common prefix. If every key in a subtree has the same bytes at positions 2–3, those two BRANCH levels are replaced by a skip=1 prefix check — a single comparison instead of two bitmap lookups and two pointer chases.

**Compact node absorption** catches entire subtrees in a flat sorted array. When a subtree has ≤ 4096 entries, it remains a compact node rather than being split into BRANCH nodes. The search within that compact node is a jump search — fast, cache-friendly, and often faster than descending further into the kntrie structure.

These two mechanisms create a dependency on N for the effective depth:

**N ≤ 4096 (worst case: all keys share the same top byte):**

```
root[ti] → compact node (up to 4096 entries, BITS=56)
```

One pointer dereference, then a jump search. If keys are spread across multiple top bytes, even fewer entries per compact node. Total depth: 1 level.

**N ≤ 4096 × 256 ≈ 1M (worst case: keys share top byte, spread across second byte):**

```
root[ti] → bot-internal → compact node (up to 4096 entries, BITS=40)
```

Two pointer dereferences plus a jump search. If the first byte varies (which it usually does for random keys), the 256 root slots each hold independent subtrees and compact nodes absorb at N/256 ≈ 4K entries each — this regime extends to N ≈ 1M with just one level of real work per lookup.

**N ≤ 4096 × 256 × 256 ≈ 268M:**

```
root[ti] → bot-internal → split-top → bot → compact node (BITS=24)
```

Four levels of dispatch plus a jump search. This covers datasets up to hundreds of millions of entries — well beyond what most in-memory use cases require.

**Full depth (N approaching 2⁶⁴ for uint64_t):**

```
root[ti] → bot-internal → split-top → bot → split-top → bot → split-top → bot (TERMINAL)
```

Seven levels of branch dispatch, reached only when subtrees are dense enough that compact nodes at every intermediate level have overflowed their 4096-entry limit. In practice, even a billion-entry dataset with random uint64_t keys rarely reaches full depth because the keys distribute across the 256 root slots and the 256 second-level slots, keeping individual subtree sizes manageable.

The key insight: O(M) is the theoretical bound, but the compact node mechanism makes the practical behavior closer to O(1) with a jump search whose size grows slowly with N. The kntrie's depth only increases when the dataset is dense enough in a particular key range to overflow compact nodes — and even then, each additional level only adds two pointer dereferences (bitmap lookup + child load).

### BENCHMARKS

#### Benchmark Summary vs std::map

All ratios are conservative (rounded down). Range spans random and sequential workloads.

**uint64_t**

| N | Find | Insert | Erase | B/entry |
|---|------|--------|-------|---------|
| 1K | 1.5x–3x | SAME–1.25x | 1.75x–2x | 3x |
| 10K | 1.5x–2x | SAME–1.5x | 2x | 4x–7x |
| 100K | 2x–6x | 2x–3x | 4x | 4x–7x |

**int32_t**

| N | Find | Insert | Erase | B/entry |
|---|------|--------|-------|---------|
| 1K | 3x–8x | SAME | 1.75x | 4x–5x |
| 10K | 3x–10x | 1.5x | 2x–3x | 5x–7x |
| 100K | 7x–25x | 2x–4x | 5x–6x | 5x–7x |

#### The Real Complexity: O(N) × O(M)

Textbook complexity treats memory access as uniform cost. In practice, every pointer chase or array lookup pays a cost determined by where the data lives in the memory hierarchy — L1, L2, L3, or DRAM. The cost of a cache miss depends on the total memory footprint M, because M determines which cache level the working set occupies.

This makes the true cost of N lookups closer to O(N × miss_cost(M)) for all three structures. The table below shows how per-lookup cost scales when N (and M) grow 10× from 10K to 100K entries:

**Cost-per-lookup growth (10K → 100K, ~10× memory growth)**

| | map (per hop, logN removed) | umap | kntrie |
|---|---|---|---|
| Theoretical | 1.0× | 1.0× | 1.0× |
| Measured | 2.2–2.7× | 2.4–3.7× | 1.1–1.8× |

Map's O(log N) is well known, but after factoring out the log N tree depth, each individual hop still gets 2–3× more expensive as N grows — pure memory hierarchy effect. The unordered_map, supposedly O(1), degrades just as badly: its hash-then-chase-pointer pattern scatters across the same oversized heap.

The kntrie's advantage is twofold. First, 4–7× smaller memory footprint means the working set stays in faster cache levels longer — at 100K int32_t entries, the trie fits in ~1MB (L2/L3 boundary) while map and umap sit at 7MB (deep L3 or DRAM). Second, the trie's dense node layout gives spatial locality — adjacent keys share cache lines, so one fetch services multiple lookups. The sequential patterns barely degrade at all (1.1–1.2×) because the trie naturally groups nearby keys into the same nodes.

In short: all three data structures pay an O(M) tax on every operation. The kntrie just has a much smaller M.

### Summary

- Real-world lookup cost is O(N) × O(miss_cost(M)), not O(N log N) or O(N)
- std::map and std::unordered_map both degrade 2–4× per 10× memory growth
- kntrie degrades only 1.1–1.8× over the same range
- The difference comes from 4–7× smaller memory footprint and dense node layout
- At 100K int32_t entries: kntrie ~1MB (L2/L3), map/umap ~7MB (L3/DRAM)
