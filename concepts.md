# kntrie Concepts

## TRIE

A TRIE (from "retrieval") is a tree structure where each node represents a portion of a key rather than the whole key. In a numeric TRIE, each level branches on some chunk of bits. The path from root to leaf spells out the complete key.

This gives TRIEs a fundamental property: lookup cost depends on the key's length, not the number of entries. A TRIE with 100 entries and one with 100 million entries traverse the same number of levels for the same key.

The classic problems: a naïve implementation allocates a 256-entry child array at every level — most slots empty. Pointer chasing between heap-scattered nodes kills cache locality. For small populations, multiple indirection levels exceed the cost of a flat sorted search.

## KTRIE

The KTRIE is a TRIE variant designed for compact data and fast reads. A key decomposes into three logical regions:

```
KEY = [PREFIX] [BRANCH ...] [SUFFIX]
```

**PREFIX** is a run of key bytes shared by all entries in a subtree. Rather than creating a chain of single-child nodes, the KTRIE captures the entire shared prefix in a single node. Lookup compares the full prefix in one step.

**BRANCH** nodes fan out to children based on a fixed-width key chunk (one byte = 256-way). Only levels where data actually fans out become BRANCH nodes — PREFIX capture absorbs the rest.

**SUFFIX** is the remaining key portion after all BRANCH levels. Entries with different suffixes are collected into a **compact leaf** — a flat sorted array of suffix/value pairs in a single allocation. This eliminates pointer chasing for the tail of the key and avoids the memory overhead of sparse BRANCH nodes near the leaves.

The KTRIE adapts its structure to the data. Dense prefix ranges collapse into single comparisons. Sparse levels use compressed bitmap representations. Small subtrees stay as flat arrays. Depth and memory adapt to the actual key distribution rather than being fixed by key width.

---

## kntrie IMPLEMENTATION

The kntrie is a concrete implementation of the KTRIE for integer keys (`uint16_t` through `int64_t`).

---

### REPRESENTATIONS

#### Key Representation

All key types are transformed into a canonical unsigned internal representation before any operation.

**Signed keys**: XOR the sign bit to fix sort order. `INT_MIN → 0`, `0 → 0x80000000`, `INT_MAX → 0xFFFFFFFF`.

**Left-alignment**: The key is shifted left so its MSB sits at the top of a 32- or 64-bit unsigned word (32-bit for keys ≤ 32 bits, 64-bit otherwise). A `uint16_t` key `0xABCD` becomes `0xABCD0000`. This means every level extracts the next byte from the same position — shift the top 8 bits out, shift the remainder up — and the same chunking logic works for all key widths.

#### Value Storage

Values are stored one of two ways, chosen at compile time:

**Inline** (`sizeof(VALUE) ≤ 8`, trivially copyable): stored directly in the slot array alongside keys. For `kntrie<uint64_t, uint64_t>`, each value is 8 bytes in a contiguous array.

**Heap-allocated** (larger or non-trivial types): a `VALUE*` allocated via the rebind allocator. The slot is still 8 bytes (a pointer), so node layout is identical — only interpretation changes.

#### Leaf vs Internal

The kntrie has two kinds of nodes: **leaves** (compact leaves and bitmap leaves) that store key/value data, and **internal nodes** (bitmask nodes) that route lookups by dispatching on one byte of the key.

Every pointer in the kntrie encodes its target type in **bit 63** (the sign bit):

- **Bit 63 set → leaf.** The pointer targets `node[0]` (the header). Strip the tag to get the real address.
- **Bit 63 clear → internal.** The pointer targets `node[1]` (the bitmap), skipping the header entirely.

Testing `ptr & (1 << 63)` compiles to a sign-bit test. The find hot loop uses this to distinguish internal descent from leaf arrival. Internal pointers target the bitmap directly so the find loop can feed them straight into the bitmap lookup with no offset arithmetic.

The root of the trie is a single tagged pointer. An empty trie points to a global sentinel — a zeroed block tagged as a leaf that reads as a valid empty node, so find returns nullptr without a null check.

---

### NODES

#### Node Header

Every allocated node begins with a single 8-byte header in `node[0]`:

```
byte 0:    flags (bit 0: is_internal; bits 1-3: skip count 0-7)
byte 1:    suffix_type (leaf only: 0=bitmap, 1=u16, 2=u32, 3=u64)
bytes 2-3: entries (uint16)
bytes 4-5: alloc_u64 (uint16)
bytes 6-7: total_slots (compact leaf) / descendants (internal) (uint16)
```

If skip > 0, `node[1]` stores up to 6 prefix bytes (outer byte first). Header size is 1 u64 (no skip) or 2 u64 (with skip).

The `is_internal` bit in the header is authoritative for node type, but the find hot loop never reads it — it uses the tagged pointer's bit 63 instead. The header bit exists for operations that receive a raw node pointer (erase, destroy).

#### Compact Leaves (KTRIE SUFFIX)

Compact leaves implement the KTRIE's SUFFIX collection — sorted arrays of suffix/value pairs in a single allocation. They handle subtrees with ≤ 4096 entries.

**Layout:**

```
[header (1 or 2 u64)] [sorted keys K[] (8-aligned)] [values VST[] (8-aligned)]
```

The suffix type (u16, u32, or u64) is determined by remaining key bits and recorded in the header.

##### Power-of-2 Slots and Branchless Search

Compact leaves always allocate `total_slots = next_power_of_2(entries)` physical slots. This enables a pure halving search with no alignment preamble:

```cpp
const K* base = keys;
unsigned count = total_slots;
do {
    count >>= 1;
    base += (base[count] <= key) ? count : 0;  // cmov, no branch
} while (count > 1);
// *base is now the last entry ≤ key (or the first entry if all > key)
```

Each iteration halves the search space with a single compare-and-conditional-move. For 512 slots: 9 iterations, 9 cmovs, zero branches. The compiler emits a tight `cmp; cmov; shr; jnz` loop.

**Why `count > 1` is safe.** The loop terminates when count reaches 1, at which point `base` points to the final candidate. The degenerate cases work naturally:

- **1 entry (total_slots = 1):** count starts at 1. The `do/while` condition `count > 1` is already false, so the loop body never executes. `base` stays on `keys[0]`, which is the only entry.
- **0 entries:** This case never reaches the search. Nodes with zero entries are the global sentinel — find checks the suffix type dispatch and returns nullptr before searching.

The `do/while` form is optimal here: it avoids the overhead of a pre-loop check that `while` would require, and the power-of-2 invariant guarantees count ≥ 1 on every entry (since `next_power_of_2(n) ≥ 1` for any `n ≥ 1`).

This outperforms `std::lower_bound`, which generates unpredictable branches that stall the pipeline on random data. The cmov loop has the same latency regardless of data distribution.

The `base` pointer lands on the last entry ≤ the search key. For find: check `*base == key`. For insert: the insertion point is `(base - keys) + (*base < key)`. Both are single branchless operations after the search.

##### Dup Tombstone Strategy

The gap between `entries` and `total_slots` is filled with **dup slots** — copies of adjacent real entries, evenly interleaved throughout the sorted array. With `n_entries` real and `n_dups` extra slots, one dup is placed after every `n_entries / (n_dups + 1)` real entries. This bounds the distance from any position to the nearest dup.

Because dups replicate their neighbor's key AND value, the sorted order is preserved. The branchless search lands correctly regardless of whether it hits a real entry or a dup.

**In-place insert.** When a new key arrives and dups exist: locate the nearest dup to the insertion point, shift the intervening entries by one position to close the gap, write the new key into the freed slot. One `memmove`, no reallocation. Only when zero dups remain does the node reallocate to the next power-of-2.

**In-place erase.** Overwrite the erased slot with a copy of its neighbor's key and value. This converts a real entry into a dup in O(1) — no memmove. Shrink only when entries drop below half the slot count.

**Heap-allocated values.** All dup slots in a run share the same pointer as their neighbor. Destroy must be called exactly once per unique key, not per physical slot.

#### Bitmask Nodes (KTRIE BRANCH)

Bitmask nodes implement the KTRIE's BRANCH — they dispatch on one byte of the key, fanning out to up to 256 children. This section covers the 256-bit bitmap used for compressed dispatch, then the internal node layout, skip chains for PREFIX compression, and bitmap leaves at the terminal level.

##### Sentinel

Internal nodes need a target for branchless misses — when a lookup asks for a child that doesn't exist, something must be returned without taking a branch. The kntrie uses a global **sentinel**: a zeroed, cache-line-aligned block of 8 u64, tagged as a leaf. Its header reads as a valid compact leaf with zero entries and suffix_type 0, so any find that lands here naturally returns nullptr.

Every internal node stores a tagged pointer to the sentinel at a fixed offset after its bitmap (described in the layout below). When the branchless popcount returns slot 0, it reads this pointer, and find descends into the sentinel as if it were a real leaf. The sentinel is also used as the root pointer of an empty trie.

Because there is exactly one sentinel shared across all nodes, repeated misses across different internal nodes all hit the same cache line.

##### The 256-Bit Bitmap

A 256-bit bitmap (4 × u64) records which of 256 possible children exist. Children are stored in a dense array — only occupied slots consume memory.

Given a key byte, the bitmap answers two questions: does this child exist, and if so, what is its index in the dense array?

**Computing the dense position.** The index equals the number of set bits *before* the target bit. For the u64 word containing the target:

```cpp
uint64_t before = words[w] << (63 - b);  // shift target to MSB, discard bits above
int slot = popcount(before);              // count bits at and below target position
```

The left-shift moves the target bit to position 63 and shifts everything above it out. What remains is the target bit and all bits below it — `popcount` counts exactly the set bits from position 0 through `b` in the original word, including the target itself if set.

For the full 256-bit position, add popcounts of complete words below the target word, branchlessly:

```cpp
slot += popcount(words[0]) & -int(w > 0);
slot += popcount(words[1]) & -int(w > 1);
slot += popcount(words[2]) & -int(w > 2);
```

The `& -int(cond)` trick: when `cond` is true, `-int(true)` is all-ones in two's complement (the popcount passes through). When false, it's zero (the popcount is masked out). No branches.

**Three modes** use this raw count differently:

**Branchless** (find path): If the target bit is set, the count is a 1-based index — and the dense array is laid out with a sentinel pointer at position 0, so 1-based indexing directly addresses the correct child. If the bit is *not* set, the count is masked to 0, which reads the sentinel. Find continues through the sentinel (a valid empty leaf), discovers nothing, returns nullptr. No conditional branch at the bitmap level.

```cpp
slot &= -int(bool(before & (1ULL << 63)));  // 0 if miss, 1-based if hit
```

**Fast-exit** (insert/erase, bitmap leaves): Check whether the target bit is set (bit 63 of `before`). If not, return -1 immediately. If set, subtract 1 from the count to get a 0-based dense index.

**Unfiltered** (insertion position): Count of set bits strictly before the target — subtract 1 if the target bit itself is set.

##### Internal Nodes

**Layout:**

```
node[0]:     header
node[1..4]:  256-bit bitmap
node[5]:     sentinel (tagged pointer to the global zeroed leaf)
node[6..]:   N children (tagged pointers), followed by N × uint16 descriptors
```

The sentinel at a fixed offset after the bitmap is the miss target for branchless lookups — when the popcount returns 0, it reads this slot, which leads find into the global empty leaf.

The **descriptor array** following the children stores one `uint16_t` per child: that child's subtree entry count (saturating at 0xFFFF). This enables erase to update ancestor counts by reading a sequential array rather than chasing child pointers into scattered nodes.

Tagged pointer convention: an internal node's pointer targets `node[1]` (the bitmap start), not `node[0]`. The find loop receives this and indexes forward into sentinel/children with no header offset. Insert and erase back up one u64 when they need the header.

**Size classes.** Internal nodes allocate in classes that trade ≤33% waste for in-place growth:

```
≤12 u64:  step 4  →  4, 8, 12
>12:      powers-of-2 with midpoints  →  16, 26, 32, 50, 64, 98, 128, ...
```

A node only shrinks when its allocation exceeds the class for 2× its actual need, preventing oscillation at boundaries.

##### Skip Chains (KTRIE PREFIX for branch nodes)

When a bitmask node has only one child, the kntrie compresses it. Rather than a chain of single-child internal nodes — each consuming 6+ u64 for one pointer — the kntrie packs them into a **skip chain** within one allocation.

This is how the kntrie implements the KTRIE's PREFIX for bitmask levels. Where a compact leaf stores prefix bytes in `node[1]`, a bitmask encodes them as a chain of embedded single-child bitmaps.

Each embedded level is a minimal bitmap: 4 u64 of bitmap (one bit set) + 1 u64 sentinel + 1 u64 child pointer to the next level. The final level is a full internal node with its own bitmap, sentinel, children, and descriptors.

The header's skip count stores how many embedded levels exist. The find loop processes these identically to standalone internal nodes — it just encounters single-child bitmaps that resolve in one popcount.

##### Bitmap Leaves

When the remaining suffix is ≤ 8 bits (256 possible values), the kntrie uses a bitmap leaf instead of a compact leaf. The same 256-bit bitmap used by internal nodes serves a different role here — the bitmap itself IS the key storage. Bit `i` set means suffix `i` is present. Values are packed densely in bitmap order.

**Layout:**

```
[header (1 or 2 u64)] [256-bit bitmap (4 u64)] [values (dense)]
```

Lookup: check the bit, popcount for the dense index (fast-exit mode), return the value. One cache line for the bitmap, one for the value.

---

### OPERATIONS

#### Find

The complete find path for a `uint64_t` key:

1. Transform the key — XOR sign bit (if signed), left-align
2. Bitmask descent — a single `while` loop:
   - Extract the top 8 bits of the internal key
   - Bitmap popcount (branchless mode): 0 on miss → sentinel, 1-based on hit → child
   - Load the child pointer, shift the key left 8 bits
   - Loop until the loaded pointer has bit 63 set (it's a leaf)
3. Strip bit 63 to recover the leaf pointer
4. Check skip prefix — byte-by-byte comparison (rare path, most nodes have no skip)
5. Dispatch by suffix type:
   - Bitmap leaf: check bit, popcount for dense index
   - Compact leaf: branchless binary search

The loop body is: extract byte → popcount → indexed load. For random `uint64_t` keys with 100K entries, typical depth is 2-3 bitmask levels + one leaf search.

#### Insert / Erase

Insert and erase are recursive, following the same descent but with mutation:

**Insert** descends to the target leaf. If dup slots are available, insert is in-place (memmove + write). If not, the leaf reallocates to the next power-of-2. If the leaf exceeds 4096 entries, it converts to an internal node with up to 256 compact leaf children.

**Erase** converts the erased entry's slot into a dup (O(1) overwrite). If entries drop below half the slot count, the leaf shrinks. If an internal node's child subtree becomes small enough, it can coalesce back into a compact leaf.

These paths are not optimized for raw speed — they involve allocation, memmove, and occasionally node restructuring. Correctness and readability take priority since the costs are dominated by alloc/dealloc and algorithmic complexity.

#### Iteration

The kntrie provides bidirectional sorted iteration via `begin()`/`end()`.

**Iterators are snapshots.** An iterator stores a copy of the key and value at its current position, not a pointer into the trie. This means iterators remain valid through inserts and erases — the only thing that invalidates them is destroying the kntrie itself. The tradeoff is that `++` and `--` re-traverse from root to find the next/prev key, rather than following a pointer.

The traversal is recursive (like insert). The call stack is the resume stack: when a leaf returns not-found, the parent tries the next/prev sibling in the bitmap, then descends to the min/max of that subtree.

At the compact leaf level, next/prev reuse the same branchless search. `next` adds one branchless step after the search lands: `pos = (base - keys) + (*base <= suffix)`. `prev` searches for `suffix - 1`, which lands directly on the last entry less than the target — skipping all interleaved dups in one shot.

Per-entry iteration cost is roughly equivalent to a find. For sequential data, cache locality across dense leaves provides practical speedup over pointer-chasing structures at scale.

**Range-for usage.** Because iterators return copies, `auto` and `const auto&` both work in range-for loops:

```cpp
for (const auto& [k, v] : trie) { ... }  // works — binds to the snapshot
for (auto [k, v] : trie) { ... }          // works — copies the snapshot
```

`auto&` will not compile — the iterator's dereference returns a temporary pair (the snapshot), and a non-const lvalue reference cannot bind to a temporary.

---

### PERFORMANCE

#### vs std::map

`std::map` is a red-black tree where each node is a separate heap allocation (~72 bytes: key + value + 3 pointers + color + padding). Each comparison follows a pointer to a heap-scattered node. At scale, nearly every tree level is a cache miss.

The kntrie's advantage is structural:

**Memory.** Significantly smaller per-entry footprint. Smaller footprint means the working set stays in faster cache levels longer — a kntrie that fits in L2/L3 will outperform a map that spills to DRAM, even if both have O(1)-ish per-entry cost.

**Locality.** Compact leaves store hundreds of entries in contiguous sorted arrays. Adjacent keys share cache lines. Sequential access patterns benefit from hardware prefetch.

**Depth.** Lookup depth is bounded by key width (≤8 levels for u64), not log₂(N). In practice, compact leaf absorption means most lookups traverse 2-3 internal levels + one leaf search, regardless of N.

#### vs std::unordered_map

`std::unordered_map` is O(1) amortized but hash-then-chase-pointer scatters across the heap. At scale, its per-lookup cost degrades similarly to map — the hash is cheap but the pointer chase after it hits DRAM.

The kntrie is ordered (unlike unordered_map) while achieving comparable or better find performance through cache-friendly layout and branchless search.

#### The Real Cost: O(N) × O(miss_cost(M))

Textbook complexity treats memory access as uniform. In practice, every pointer chase pays a cost determined by where data sits in the hierarchy — L1, L2, L3, DRAM. The true cost of N lookups is closer to `O(N × miss_cost(M))` where M is total memory footprint.

All three structures pay this tax. The kntrie just has a much smaller M — and its dense layout means sequential prefetch works, while tree/hash structures defeat it.
