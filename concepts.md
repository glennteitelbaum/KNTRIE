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

The kntrie is a concrete implementation of the KTRIE for integer keys (`uint8_t` through `uint64_t`). It's what would happen if an array-of-arrays had a child with a trie — the trie gives you O(key-width) depth and prefix sharing, the nested arrays give you dense packed leaves with sequential access and branchless search. The trie routes you through 2-3 levels of bitmap dispatch, then hands you off to a fat sorted array where hundreds of entries sit contiguous in memory.

The worst parts of each parent cancel out — tries waste space on sparse branching (solved by bitmap compression), and nested arrays waste time on deep indexing (solved by the trie collapsing shared prefixes). What survives is the trie's logarithmic-in-key-width routing and the array's cache-line-friendly density.

---

### FILE ORGANIZATION

The implementation spans 7 header files:

| File | Lines | Role |
|------|-------|------|
| `kntrie_support.hpp` | ~330 | `node_header_t`, `key_ops`, tagged pointers, `value_traits`, allocator helpers |
| `kntrie_compact.hpp` | ~610 | Compact leaf operations: `adaptive_search`, `compact_ops` |
| `kntrie_bitmask.hpp` | ~1090 | `bitmap_256_t`, `bitmask_ops`: bitmap arithmetic, internal node layout/accessors |
| `kntrie_ops.hpp` | ~1060 | `kntrie_ops`: find, insert, erase, coalesce — compile-time NK recursion |
| `kntrie_iter_ops.hpp` | ~740 | `kntrie_iter_ops`: iteration, destroy, stats — compile-time NK recursion |
| `kntrie_impl.hpp` | ~260 | `kntrie_impl`: thin wrapper dispatching public API to ops |
| `kntrie.hpp` | ~240 | `kntrie`: STL-compatible interface, `const_iterator` |

Each `.hpp` is self-contained (verified by corresponding `.cpp` compilation units). Dependencies flow downward: `kntrie.hpp` → `kntrie_impl.hpp` → `kntrie_ops.hpp` / `kntrie_iter_ops.hpp` → `kntrie_bitmask.hpp` / `kntrie_compact.hpp` → `kntrie_support.hpp`.

---

### COMPILE-TIME NK NARROWING

This is the central architectural pattern. Every operation that descends the trie carries a **narrowed key type** `NK` as a template parameter. As descent consumes bytes of the key, `NK` shrinks at half-width boundaries:

```
uint64_t → uint32_t → uint16_t → uint8_t
```

This happens via `if constexpr` at compile time:

```cpp
template<int BITS> requires (BITS >= 8)
static const VALUE* find_node(uint64_t ptr, NK ik) {
    // ... extract byte, descend ...
    if constexpr (BITS > 8) {
        NK shifted = static_cast<NK>(ik << 8);
        if constexpr (BITS - 8 == NK_BITS / 2 && NK_BITS > 8)
            // Narrow: uint64_t→uint32_t, uint32_t→uint16_t, etc.
            return NARROW::template find_node<BITS - 8>(child,
                static_cast<NNK>(shifted >> (NK_BITS / 2)));
        else
            return find_node<BITS - 8>(child, shifted);
    }
}
```

At each level, `BITS` decreases by 8 (one byte consumed). When `BITS - 8` equals half the current NK width, the recursion narrows to `NARROW` — a `kntrie_ops` (or `kntrie_iter_ops`) instantiation with the next-smaller type. The narrowing point triggers the correct `compact_ops<NK>` specialization automatically.

This eliminates all runtime `suffix_type` dispatch. Where the old code had:

```cpp
switch (hdr->suffix_type()) {
    case 0: bitmap_ops(...); break;
    case 1: compact_ops<uint16_t>(...); break;
    case 2: compact_ops<uint32_t>(...); break;
    case 3: compact_ops<uint64_t>(...); break;
}
```

The new code knows at compile time which leaf type it will find: when `NK` is `uint8_t`, it's a bitmap leaf; otherwise it's a `compact_ops<NK>` leaf. The compiler generates specialized code for each path.

Every operation uses this pattern: `find_node`, `insert_node`, `erase_node`, `iter_next_node`, `descend_min`, `remove_subtree`, `collect_stats` — all recursive with the same narrowing logic.

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
byte 0:    skip_count (bits 0-2: skip count 0-6, bits 3-7: reserved)
byte 1:    reserved
bytes 2-3: entries (uint16)
bytes 4-5: alloc_u64 (uint16)
bytes 6-7: total_slots (compact leaf) / descendants (internal) (uint16)
```

If skip > 0, `node[1]` stores up to 6 prefix bytes (outer byte first). Header size is 1 u64 (no skip) or 2 u64 (with skip).

The header contains no type information — no `is_leaf` flag, no `suffix_type` field. Node type is determined entirely by two mechanisms:

1. **Tagged pointers** (bit 63) distinguish leaf from internal at the call site.
2. **Compile-time NK narrowing** determines the leaf variant (`bitmap_256_t` leaf when `NK=uint8_t`, `compact_ops<NK>` otherwise).

This means the header is pure data — no dispatch metadata. All type dispatch is resolved at compile time.

#### Compact Leaves (KTRIE SUFFIX)

Compact leaves implement the KTRIE's SUFFIX collection — sorted arrays of suffix/value pairs in a single allocation. They handle subtrees with ≤ COMPACT_MAX entries (4096 for 16-bit suffixes, 512 for 32/64-bit).

**Layout:**

```
[header (1 or 2 u64)] [sorted keys K[] (8-aligned)] [values VST[] (8-aligned)]
```

The suffix type (`compact_ops<uint16_t>`, `compact_ops<uint32_t>`, or `compact_ops<uint64_t>`) is determined at compile time by the NK narrowing — no runtime dispatch needed.

##### Power-of-2 Slots and Branchless Search

Compact leaves always allocate `total_slots = next_power_of_2(entries)` physical slots. This enables a branchless binary search that adapts to non-power-of-2 counts:

```cpp
const K* base = keys;
unsigned n = total_slots;
// First step: adjust to align remaining steps to power-of-2
unsigned half = n >> 1;
base += (base[half] <= key) ? n - half : 0;
n = half;
// Remaining steps: pure halving, each a cmov
while (n > 1) {
    n >>= 1;
    base += (base[n] <= key) ? n : 0;  // cmov, no branch
}
```

The first step is special — it handles the non-power-of-2 alignment so all subsequent iterations are exact halvings with a single compare-and-conditional-move. For 512 slots: 9 iterations, 9 cmovs, zero branches.

This outperforms `std::lower_bound`, which generates unpredictable branches that stall the pipeline on random data. The cmov loop has the same latency regardless of data distribution.

##### Dup Tombstone Strategy

The gap between `entries` and `total_slots` is filled with **dup slots** — copies of adjacent real entries, evenly interleaved throughout the sorted array. With `n_entries` real and `n_dups` extra slots, one dup is placed after every `n_entries / (n_dups + 1)` real entries.

Because dups replicate their neighbor's key AND value, the sorted order is preserved. The branchless search lands correctly regardless of whether it hits a real entry or a dup.

**In-place insert.** When a new key arrives and dups exist: locate the nearest dup to the insertion point, shift the intervening entries by one position to close the gap, write the new key into the freed slot. One `memmove`, no reallocation. Only when zero dups remain does the node reallocate to the next power-of-2.

**In-place erase.** Overwrite the erased slot with a copy of its neighbor's key and value. This converts a real entry into a dup in O(1) — no memmove. Shrink only when entries drop below half the slot count.

**Heap-allocated values.** All dup slots in a run share the same pointer as their neighbor. Destroy must be called exactly once per unique key, not per physical slot.

#### Bitmask Nodes (KTRIE BRANCH)

Bitmask nodes implement the KTRIE's BRANCH — they dispatch on one byte of the key, fanning out to up to 256 children.

##### Sentinel

Internal nodes need a target for branchless misses — when a lookup asks for a child that doesn't exist, something must be returned without taking a branch. The kntrie uses a global **sentinel**: a zeroed, cache-line-aligned block of 8 u64, tagged as a leaf. Its header reads as a valid empty node, so any find that lands here naturally returns nullptr.

Every internal node stores a tagged pointer to the sentinel at a fixed offset after its bitmap. When the branchless popcount returns slot 0, it reads this pointer, and find descends into the sentinel as if it were a real leaf. The sentinel is also used as the root pointer of an empty trie.

Because there is exactly one sentinel shared across all nodes, repeated misses across different internal nodes all hit the same cache line.

##### The 256-Bit Bitmap

A `bitmap_256_t` (4 × u64) records which of 256 possible children exist. Children are stored in a dense array — only occupied slots consume memory.

Given a key byte, the bitmap answers two questions: does this child exist, and if so, what is its index in the dense array?

**Computing the dense position.** The index equals the number of set bits *before* the target bit. For the u64 word containing the target:

```cpp
uint64_t before = words[w] << (63 - b);  // shift target to MSB, discard bits above
int slot = popcount(before);              // count bits at and below target position
```

For the full 256-bit position, add popcounts of complete words below the target word, branchlessly:

```cpp
slot += popcount(words[0]) & -int(w > 0);
slot += popcount(words[1]) & -int(w > 1);
slot += popcount(words[2]) & -int(w > 2);
```

The `& -int(cond)` trick: when `cond` is true, `-int(true)` is all-ones in two's complement (the popcount passes through). When false, it's zero (the popcount is masked out). No branches.

**Three modes** use this raw count differently:

**Branchless** (find path): If the target bit is set, the count is a 1-based index — and the dense array is laid out with a sentinel pointer at position 0, so 1-based indexing directly addresses the correct child. If the bit is *not* set, the count is masked to 0, which reads the sentinel. No conditional branch at the bitmap level.

**Fast-exit** (insert/erase, bitmap leaves): Check whether the target bit is set. If not, return -1 immediately. If set, subtract 1 from the count to get a 0-based dense index.

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

When the remaining suffix is ≤ 8 bits (256 possible values), the kntrie uses a bitmap leaf instead of a compact leaf. The same `bitmap_256_t` used by internal nodes serves a different role here — the bitmap itself IS the key storage. Bit `i` set means suffix `i` is present. Values are packed densely in bitmap order.

**Layout:**

```
[header (1 or 2 u64)] [256-bit bitmap (4 u64)] [values (dense)]
```

Lookup: check the bit, popcount for the dense index (fast-exit mode), return the value. One cache line for the bitmap, one for the value.

The compile-time NK narrowing determines when bitmap leaves are used: when `NK` has narrowed to `uint8_t` (8 bits remaining), `compact_ops` is not instantiated — `bitmask_ops` bitmap leaf functions handle the terminal case.

---

### OPERATIONS

#### Find

The find path is fully compile-time recursive via `kntrie_ops::find_node<BITS>`:

1. Transform the key — XOR sign bit (if signed), left-align. Extract `NK` from the top bits.
2. Recursive descent — `find_node<BITS>(tagged, ik)`:
   - If tagged is a leaf: call `find_leaf<BITS>`, check skip prefix, then:
     - `NK=uint8_t` → bitmap leaf: check bit, popcount for dense index
     - `NK` wider → compact leaf: branchless binary search via `compact_ops<NK>`
   - If tagged is internal: extract top byte, bitmap popcount (branchless), load child, recurse with `BITS - 8` and `NK shifted << 8`. Narrow `NK` at half-width boundaries.

The recursion bottoms out when `BITS` reaches the leaf level. The compiler unrolls the recursion into a specialized code path for each key width. For random `uint64_t` keys with 100K entries, typical depth is 2-3 bitmask levels + one leaf search.

#### Insert / Erase

Insert and erase follow the same recursive descent with mutation. Both are in `kntrie_ops` and use NK narrowing:

**Insert** (`insert_node<BITS>`): descends to the target leaf. If dup slots are available, insert is in-place (memmove + write). If not, the leaf reallocates to the next power-of-2. If the leaf exceeds COMPACT_MAX entries, it converts to an internal node with up to 256 compact leaf children.

Skip chains and prefix mismatches are handled by `insert_leaf_skip`, `insert_chain_skip`, and `split_on_prefix` — all compile-time recursive with NK narrowing at the same boundaries.

**Erase** (`erase_node<BITS>`): converts the erased entry's slot into a dup (O(1) overwrite). If entries drop below half the slot count, the leaf shrinks.

**Coalesce** (`do_coalesce<BITS>`): after erase reduces an internal node's descendant count below COMPACT_MAX, collects all entries from the subtree via `collect_entries<BITS>` into NK-native arrays, then rebuilds as a single compact leaf. The collection is itself recursive with NK narrowing — entries are gathered as `(NK, VST)` pairs at their natural width, and parent levels widen at narrowing boundaries.

These paths are not optimized for raw speed — they involve allocation, memmove, and occasionally node restructuring. Correctness and readability take priority since the costs are dominated by alloc/dealloc and algorithmic complexity.

#### Iteration

The kntrie provides bidirectional sorted iteration via `begin()`/`end()`. Iteration is in `kntrie_iter_ops`, a standalone struct with the same NK narrowing pattern.

**Iterators are snapshots.** An iterator stores a copy of the key and value at its current position, not a pointer into the trie. This means iterators remain valid through inserts and erases — the only thing that invalidates them is destroying the kntrie itself. The tradeoff is that `++` and `--` re-traverse from root to find the next/prev key, rather than following a pointer.

**Traversal structure.** Four families of functions, all compile-time recursive:

- `descend_min<BITS>` / `descend_max<BITS>`: walk the always-leftmost or always-rightmost path, no key comparison needed. Used to find the first/last entry in a subtree after locating the next sibling.
- `iter_next_node<BITS>` / `iter_prev_node<BITS>`: find the smallest key > ik (or largest < ik). Compare byte-by-byte through skip chains, then recurse into bitmap children or try the next/prev sibling.

Each family has three sub-functions for the three traversal phases: `*_leaf_skip` (consume leaf prefix bytes), `*_chain_skip` (consume bitmask chain bytes), `*_bm_final` (dispatch at the final bitmap). All narrow NK at compile-time boundaries.

**Key reconstruction.** The iteration accumulates a prefix (`IK prefix, int bits`) as it descends, then combines the leaf suffix using:

```cpp
prefix | ((IK(suffix) << (IK_BITS - NK_BITS)) >> bits)
```

This positions the suffix in the correct bit range within the internal key, handling all key widths uniformly.

**Leaf helpers.** At the leaf level, `leaf_first` / `leaf_last` (no ik needed) and `leaf_next` / `leaf_prev` (take NK suffix) dispatch at compile time: `sizeof(NK)==1` → bitmap ops, else → `compact_ops<NK>`.

Per-entry iteration cost is roughly equivalent to a find. For sequential data, cache locality across dense leaves provides practical speedup over pointer-chasing structures at scale.

**Range-for usage.** Because iterators return copies, `auto` and `const auto&` both work in range-for loops:

```cpp
for (const auto& [k, v] : trie) { ... }  // works — binds to the snapshot
for (auto [k, v] : trie) { ... }          // works — copies the snapshot
```

`auto&` will not compile — the iterator's dereference returns a temporary pair (the snapshot), and a non-const lvalue reference cannot bind to a temporary.

#### Destroy / Stats

`remove_subtree<BITS>` and `collect_stats<BITS>` (both in `kntrie_iter_ops`) use the same NK narrowing recursion to walk the entire trie. Destroy calls the appropriate `destroy_and_dealloc` for each leaf type — `bitmask_ops` when `NK=uint8_t`, `compact_ops<NK>` otherwise — resolved at compile time.

---

### COMPLEXITY

All kntrie operations are O(M) where M is the key width in bytes (1-8), independent of N (number of entries). This is the fundamental trie property — depth is fixed by the key, not the data. Since M ≤ 8 for all supported key types, every operation is effectively constant time, but the distinction matters: doubling N does not change any operation's cost.

**read O(M)** — at most M bitmask levels of bitmap dispatch, each a fixed-cost popcount + indexed load. The terminal leaf search is O(log COMPACT_MAX) but COMPACT_MAX is a compile-time constant (512 or 4096), so it contributes a fixed cost independent of both M and N.

**iterate O(M)** — each `++`/`--` re-traverses from root to leaf. The traversal depth is bounded by M. Sequential entries within the same compact leaf benefit from cache locality, but the algorithmic cost per step is still O(M).

**insert O(M)** — descent is O(M). The typical insert consumes one dup slot in the compact leaf — a single memmove of a few entries to close the gap. Only when all dups are exhausted does the leaf reallocate to the next power-of-2. When a leaf exceeds COMPACT_MAX entries, it splits into an internal node with up to 256 children — a rare spike amortized across thousands of inserts. Allocation dominates real-world cost.

**erase O(M)** — descent is O(M). The typical erase overwrites the target slot with its neighbor's key and value — O(1), no memmove. The leaf only shrinks when entries drop below half the slot count. When an internal node's subtree drops below COMPACT_MAX, a coalesce collects all remaining entries and rebuilds a single compact leaf — a rare spike amortized across many erases. Deallocation dominates real-world cost.

**memory O(N × M)** — each entry contributes its suffix (up to M bytes) plus value storage, and internal nodes add structural overhead along the M-deep path. In practice, prefix sharing and compact leaf packing compress this significantly below the theoretical N × M bound. Per-entry memory exhibits a sawtooth pattern as compact leaves fill and split — a leaf at 50% capacity wastes dup slots, while one at 100% is maximally packed. But total memory scales linearly with N and the sawtooth amplitude is bounded. Unlike hash tables, there is no sudden 2× rehash spike.

**at theoretical max** — when the keyspace is fully or near-fully populated, compact leaves are eliminated entirely. The tree becomes pure bitmask nodes all the way down to depth M, with bitmap leaves at the terminal level. Reads traverse exactly M levels of bitmap dispatch. Writes simply set or clear a bit in the terminal bitmap leaf — no splits, no coalesces, no dup management. At this point the kntrie resembles a pure trie.

---

### PERFORMANCE

#### vs std::map

`std::map` is a red-black tree where each node is a separate heap allocation (~48 bytes per entry). Each comparison follows a pointer to a heap-scattered node. At scale, nearly every tree level is a cache miss.

The kntrie's advantage is structural:

**Memory.** 12-25 bytes/entry (sequential-random) vs 48 bytes/entry. Smaller footprint means the working set stays in faster cache levels longer.

**Locality.** Compact leaves store hundreds of entries in contiguous sorted arrays. Adjacent keys share cache lines. Sequential access patterns benefit from hardware prefetch.

**Depth.** Lookup depth is bounded by key width (≤8 levels for u64), not log₂(N). In practice, compact leaf absorption means most lookups traverse 2-3 internal levels + one leaf search, regardless of N.

**Find speed.** At 100K entries: kntrie ~32ns/op vs map ~411ns/op (random u64). The 12× advantage comes from fewer cache misses and branchless search.

#### vs std::unordered_map

`std::unordered_map` is O(1) amortized with ~32 bytes/entry. The hash is cheap but the pointer chase after it hits DRAM at scale.

The kntrie is ordered (unlike unordered_map) while achieving comparable find performance through cache-friendly layout and branchless search. At 100K entries: kntrie ~32ns/op vs umap ~26ns/op (random u64). For misses, kntrie wins: ~19ns vs ~32ns.

Memory advantage is significant: 21 bytes/entry vs 32 bytes/entry (random), and 12 bytes/entry vs 32 bytes/entry (sequential).

#### The Real Cost: O(N) × O(miss_cost(M))

Textbook complexity treats memory access as uniform. In practice, every pointer chase pays a cost determined by where data sits in the hierarchy — L1, L2, L3, DRAM. The true cost of N lookups is closer to `O(N × miss_cost(M))` where M is total memory footprint.

All three structures pay this tax. The kntrie just has a much smaller M — and its dense layout means sequential prefetch works, while tree/hash structures defeat it.

---

### NAMING CONVENTIONS

See `naming.md` for the full naming convention. Key points:

- Data members end in `_v`: `root_v`, `size_v`, `entries_v`
- Pointers end in `_p`, references in `_r`, lambdas in `_l`
- Booleans start with `is_` or `has_`
- Functions never end in `_`
- `using` aliases, enum values, `constexpr`, template params: `ALL_CAPS`
- Classes, functions, variables: `lower_snake`
- POD structs: `struct x_t`. Non-POD: `class x`
- Always `template<typename T>`, never `template<class T>`
