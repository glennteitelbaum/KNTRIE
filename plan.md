# kntrie Refactor: Unified Node Design

## 1. Goals

1. Replace recursive template dispatch: **find = iterative loop**, **insert/erase = runtime recursion** (not template recursion).
2. Use native-width internal keys: `uint32_t` for keys ≤32 bits, `uint64_t` for keys >32 bits.
3. compact_ops templated on suffix type (u8/u16/u32/u64) instead of every BITS value.
4. Suffix type stored in node header — nodes are fully self-describing.
5. **One bitmask node type** — no split vs fan distinction, no internal_bitmap.
6. **16-byte header** with plain fields — no bit-packing for skip/prefix.
7. **u8 skip chunks** — skip up to 6 levels (48 bits), covers all key widths.
8. Preserve allocation strategies. Below 256 entries: unused slots (no dups). Above 256: spread dups.

## 2. Naming Conventions

- **Constants, enum values, macros:** ALL_CAPS (e.g., `HEADER_U64`, `COMPACT_MAX`, `SENTINEL_NODE`)
- **Everything else:** lowercase with underscores (e.g., `find_value`, `node_header`, `branchless_child`)
- **Class/struct member data:** ends with `_` (e.g., `flags_`, `size_`, `alloc_`)
- **Function names:** do NOT end with `_` (e.g., `keys()`, `vals()`)
- **Data-only structs (return types, POD):** name ends with `_t` (e.g., `insert_result_t`, `erase_result_t`)
- **Main class:** `kntrie` (was `kntrie3`)
- **Namespace:** `gteitelbaum` (was `kn3`) — all implementation types live here
- **No separate wrapper class.**

## 3. Internal Key Representation

### Type Selection
```cpp
using IK = std::conditional_t<sizeof(KEY) <= 4, uint32_t, uint64_t>;
static constexpr int IK_BITS = sizeof(IK) * 8;  // 32 or 64
static constexpr int KEY_BITS = sizeof(KEY) * 8;
```

### Conversion (left-aligned in native type)
```cpp
static constexpr IK to_internal(KEY k) noexcept {
    IK r;
    if constexpr (sizeof(KEY) == 1)      r = static_cast<uint8_t>(k);
    else if constexpr (sizeof(KEY) == 2) r = static_cast<uint16_t>(k);
    else if constexpr (sizeof(KEY) == 4) r = static_cast<uint32_t>(k);
    else                                 r = static_cast<uint64_t>(k);
    if constexpr (IS_SIGNED) r ^= IK{1} << (KEY_BITS - 1);
    r <<= (IK_BITS - KEY_BITS);
    return r;
}
```

### Bit Extraction (always from top, then shift)
```cpp
uint8_t byte = static_cast<uint8_t>(ik >> (IK_BITS - 8));
ik <<= 8;   // consume 8 bits
```

## 4. Node Header (16 bytes = 2 u64)

```
flags_:      uint16_t   [15] is_bitmask  [1:0] suffix_type
entries_:    uint16_t
alloc_u64_:  uint16_t
skip_len_:   uint8_t    (0-6)
prefix_[6]:  uint8_t    skip prefix bytes (u8 chunks)
pad_[3]:     uint8_t    reserved
```

Total: 2+2+2+1+6+3 = 16 bytes. HEADER_U64 = 2.

```cpp
struct node_header {
    uint16_t flags_;       // [15] is_bitmask, [1:0] suffix_type
    uint16_t entries_;
    uint16_t alloc_u64_;
    uint8_t  skip_len_;    // 0-6 for real nodes
    uint8_t  prefix_[6];   // u8 skip chunks
    uint8_t  pad_[3];      // reserved

    bool is_leaf() const noexcept { return !(flags_ & 0x8000); }
    void set_bitmask() noexcept { flags_ |= 0x8000; }

    uint8_t suffix_type() const noexcept { return flags_ & 0x3; }
    void set_suffix_type(uint8_t t) noexcept { flags_ = (flags_ & ~0x3) | (t & 0x3); }

    uint16_t entries() const noexcept { return entries_; }
    void set_entries(uint16_t n) noexcept { entries_ = n; }

    uint16_t alloc_u64() const noexcept { return alloc_u64_; }
    void set_alloc_u64(uint16_t n) noexcept { alloc_u64_ = n; }

    uint8_t skip() const noexcept { return skip_len_; }
    void set_skip(uint8_t s) noexcept { skip_len_ = s; }

    const uint8_t* prefix_bytes() const noexcept { return prefix_; }
    void set_prefix(const uint8_t* p, uint8_t len) noexcept {
        for (uint8_t i = 0; i < len; ++i) prefix_[i] = p[i];
    }
};
static_assert(sizeof(node_header) == 16);
```

### Suffix type encoding

| suffix_type | K type   | Used by        |
|-------------|----------|----------------|
| 0           | uint8_t  | Bitmap256 leaf |
| 1           | uint16_t | Compact leaf   |
| 2           | uint32_t | Compact leaf   |
| 3           | uint64_t | Compact leaf   |

### Sentinel

Must be large enough for branchless_child to safely read bitmap at `node + 2`.
Minimum: header(2) + bitmap(4) = 6 u64s.

```cpp
alignas(64) inline constinit uint64_t SENTINEL_NODE[6] = {};
```

Zeroed: is_bitmask=0, suffix_type=0, entries=0, bitmap all zeros.
- As leaf: is_leaf()=true, suffix_type=0 → bitmap_find with empty bitmap → nullptr. ✓
- As branchless miss target: bitmap all zeros → FAST_EXIT returns -1. ✓

### Skip capacity

| Key type  | Max levels after root | Max skip needed | Fits in prefix_[6]? |
|-----------|-----------------------|-----------------|----------------------|
| uint16_t  | 1                     | 0               | ✓                    |
| int32_t   | 3                     | 2               | ✓                    |
| uint64_t  | 7                     | 6               | ✓                    |

## 5. Node Types

### 5a. Compact Leaf (is_leaf=true, suffix_type=1/2/3)
- `[header (2 u64)][sorted K[] (aligned)][VST[] (aligned)]`
- K determined by suffix_type: 1=u16, 2=u32, 3=u64
- Can have skip/prefix
- Dup seeding for in-place insert/erase (unchanged)

### 5b. Bitmask Node (is_leaf=false)
- `[header (2 u64)][bitmap256 (4 u64)][sentinel (1 u64)][children[]]`
- bitmap256: which children exist (up to 256)
- sentinel: slot 0 = SENTINEL_NODE for branchless miss
- children[]: real child pointers in bitmap order
- Size: `7 + n_children` u64s
- **No internal_bitmap.** Every child is self-describing via its own header.
- **branchless_child returns just a pointer.** Caller reads child header.
- Can have skip/prefix

### 5c. Bitmap256 Leaf (is_leaf=true, suffix_type=0)
- `[header (2 u64)][bitmap256 (4 u64)][VST[] (aligned)]`
- 8-bit suffix space, O(1) lookup via popcount
- Always terminal
- Size: `6 + ceil(n_entries * sizeof(VST) / 8)` u64s

### 5d. Root Array
- `root_[256]` indexed by top 8 bits of internal key
- Each slot: SENTINEL_NODE or any self-describing node

## 6. Node Type Identification

**Every node is self-describing from its own header alone.**

| `is_leaf()` | `suffix_type()` | Node type        |
|-------------|-----------------|------------------|
| true        | 0               | Bitmap256 leaf   |
| true        | 1               | Compact leaf u16 |
| true        | 2               | Compact leaf u32 |
| true        | 3               | Compact leaf u64 |
| false       | —               | Bitmask node     |

No parent context needed. Eliminates split/fan/bot-leaf distinction entirely.

## 7. Trie Structure (u8 stride at every level)

### u64 key (IK_BITS = 64)

```
root_[ri]   consumes 8 bits  → 56 remaining
level 1     bitmask or leaf  → 48 remaining
level 2     bitmask or leaf  → 40 remaining
level 3     bitmask or leaf  → 32 remaining
level 4     bitmask or leaf  → 24 remaining
level 5     bitmask or leaf  → 16 remaining
level 6     bitmask or leaf  → 8 remaining
level 7     bitmap256 leaf   → 0 remaining
```

Compact leaves can appear at any level:
- 48-56 bits → u64 (type 3)
- 24-32 bits → u32 (type 2)
- 16 bits    → u16 (type 1)
- 8 bits     → bitmap256 (type 0)

### i32 key (IK_BITS = 32)

```
root_[ri]   consumes 8 bits  → 24 remaining
level 1     bitmask or leaf  → 16 remaining
level 2     bitmask or leaf  → 8 remaining
level 3     bitmap256 leaf   → 0 remaining
```

Compact leaves: 24 → u32, 16 → u16, 8 → bitmap256.

## 8. Loop-Based Find

```cpp
const VALUE* find_value(const KEY& key) const noexcept {
    IK ik = KO::to_internal(key);
    uint8_t ri = static_cast<uint8_t>(ik >> (IK_BITS - 8));
    ik <<= 8;  // consume root byte

    const uint64_t* node = root_[ri];
    node_header hdr = *get_header(node);

    // First node after root never has skip — jump past skip check
    goto noskip;

    while (true) {
        // Skip/prefix (u8 chunks) — ik already shifted past parent's byte
        {
            uint8_t skip = hdr.skip();
            if (skip) [[unlikely]] {
                const uint8_t* actual = hdr.prefix_bytes();
                for (uint8_t i = 0; i < skip; ++i) {
                    uint8_t expected = static_cast<uint8_t>(ik >> (IK_BITS - 8));
                    if (expected != actual[i]) [[unlikely]] return nullptr;
                    ik <<= 8;
                }
            }
        }

    noskip:
        if (hdr.is_leaf()) break;

        // Bitmask node: extract next byte, descend
        uint8_t ti = static_cast<uint8_t>(ik >> (IK_BITS - 8));
        ik <<= 8;  // consume this byte
        node = BO::branchless_child(node, ti);
        hdr = *get_header(node);
    }

    // Leaf dispatch — suffix_type tells us width
    // ik has been shifted so remaining suffix is at the top
    uint8_t st = hdr.suffix_type();

    if (st == 0)
        return BO::bitmap_find(node, hdr,
            static_cast<uint8_t>(ik >> (IK_BITS - 8)));

    if constexpr (KEY_BITS > 16) {
        if (st & 0b10) {
            if constexpr (KEY_BITS > 32) {
                if (st & 0b01)
                    return CO64::find(node, hdr,
                        static_cast<uint64_t>(ik));
            }
            return CO32::find(node, hdr,
                static_cast<uint32_t>(ik >> (IK_BITS - 32)));
        }
    }

    return CO16::find(node, hdr,
        static_cast<uint16_t>(ik >> (IK_BITS - 16)));
}
```

**ik shifting convention:** `ik` is always shifted so the next byte to process
is at position `(IK_BITS - 8)`. Each skip byte or bitmask descent shifts ik
left by 8. The root caller shifts once for ri. Find doesn't track `bits`
explicitly — suffix_type encodes the width.

For insert/erase, `bits` tracks remaining bits alongside ik shifts.

## 9. Bitmask Node Layout

```
[header: 2 u64][bitmap256: 4 u64][sentinel: 1 u64][children: n u64]
```

Offsets: bitmap at +2, sentinel at +6, real children at +7.
Size: `7 + n_children` u64s.

### branchless_child
```cpp
static const uint64_t* branchless_child(const uint64_t* node, uint8_t idx) noexcept {
    const bitmap256& bm = *reinterpret_cast<const bitmap256*>(node + 2);
    int slot = bm.find_slot<BRANCHLESS>(idx);  // 0 on miss → sentinel
    return reinterpret_cast<const uint64_t*>(children(node)[slot]);
}

static uint64_t* children(uint64_t* node) { return node + 6; }       // includes sentinel
static uint64_t* real_children(uint64_t* node) { return node + 7; }  // past sentinel
```

### Operations
- **lookup(node, idx):** FAST_EXIT → {child, slot, found}
- **branchless_child(node, idx):** BRANCHLESS → child ptr (sentinel on miss)
- **add_child(node, idx, child_ptr, alloc):** in-place or realloc
- **remove_child(node, slot, idx, alloc):** in-place or realloc
- **make(indices, ptrs, count, skip, prefix, alloc):** create from arrays
- **for_each_child(node, cb):** iterate → cb(idx, slot, child_ptr)
- **dealloc(node, alloc):** free node only

## 10. Bitmap256 Leaf Layout

```
[header: 2 u64][bitmap256: 4 u64][values: ceil(n * sizeof(VST) / 8) u64]
```

Offsets: bitmap at +2, values at +6.
Size: `6 + ceil(n * sizeof(VST) / 8)` u64s.

is_leaf()=true, suffix_type()=0. Self-describing.

### Operations
- **bitmap_find(node, hdr, suffix_u8):** bitmap FAST_EXIT → value slot
- **bitmap_insert(node, suffix_u8, value, alloc):** in-place or realloc
- **bitmap_erase(node, suffix_u8, alloc):** in-place or realloc
- **make_bitmap_leaf(sorted_suffixes, values, count, alloc):** from arrays
- **for_each_bitmap(node, cb):** iterate → cb(suffix_u8, value_slot)
- **bitmap_destroy_and_dealloc(node, alloc):** destroy values + free

## 11. Compact Leaf (unchanged except HEADER_U64=2)

Layout: `[header: 2 u64][sorted K[] (8-aligned)][VST[] (8-aligned)]`

Operations templated on K (u16/u32/u64):
- **find<K>(node, hdr, suffix):** jump_search → value
- **insert<K>(node, hdr, suffix, value, alloc):** use unused slot or realloc
- **erase<K>(node, hdr, suffix, alloc):** shift left or realloc
- **make_leaf<K>(keys, vals, count, skip, prefix, alloc):** from sorted arrays
- **for_each<K>(node, hdr, cb):** iterate over entries (entries count only)
- **destroy_and_dealloc<K>(node, alloc):** destroy values + free

### Slot strategy

`total_slots` = max entries that fit in `alloc_u64`. `entries` = actual count.
`unused = total_slots - entries`.

**Below 256 entries:** Unused slots are simply empty (no dups, no tombstones).
Insert: `memmove` right into unused tail, write at insertion point.
Erase: `memmove` left, decrement entries.
When no unused slots available: realloc to next size class.

**Above 256 entries:** Spread dups evenly (as current design).
Bounds shift distance to O(n/dups). Insert consumes a dup. Erase creates a dup.

## 12. Insert (Runtime Recursion)

Find uses a loop (hot path). Insert and erase use runtime recursion — max depth
is 8 for u64, 4 for i32. The call stack gives natural parent propagation: callee
returns new pointer, caller updates its child slot. No parent_slot gymnastics.

```cpp
std::pair<bool, bool> insert(const KEY& key, const VALUE& value) {
    IK ik = KO::to_internal(key);
    VST sv = VT::store(value, alloc_);
    uint8_t ri = static_cast<uint8_t>(ik >> (IK_BITS - 8));
    ik <<= 8;  // consume root byte

    uint64_t* node = root_[ri];
    if (node == SENTINEL_NODE) {
        root_[ri] = make_single_leaf(ik, sv, KEY_BITS - 8);
        ++size_;
        return {true, true};
    }

    auto [new_node, inserted] = insert_node(node, ik, sv, KEY_BITS - 8);
    if (new_node != node) root_[ri] = new_node;
    if (inserted) { ++size_; return {true, true}; }
    VT::destroy(sv, alloc_);
    return {true, false};
}
```

### insert_node (recursive)

```cpp
// ik: shifted so next byte to process is at (IK_BITS - 8)
// bits: how many bits of key remain at this node's level
insert_result_t insert_node(uint64_t* node, IK ik, VST value, int bits) {
    auto* hdr = get_header(node);

    // Skip check
    uint8_t skip = hdr->skip();
    if (skip) [[unlikely]] {
        const uint8_t* actual = hdr->prefix_bytes();
        for (uint8_t i = 0; i < skip; ++i) {
            uint8_t expected = static_cast<uint8_t>(ik >> (IK_BITS - 8));
            if (expected != actual[i]) {
                // Mismatch → split_on_prefix
                return {split_on_prefix(node, hdr, ik, value,
                                         actual, skip, i, bits), true};
            }
            ik <<= 8;
            bits -= 8;
        }
    }

    if (hdr->is_leaf()) {
        auto result = leaf_insert(node, hdr, ik, value, bits);
        if (result.needs_split) {
            return {convert_to_bitmask(node, hdr, ik, value, bits), true};
        }
        return {result.node, result.inserted};
    }

    // Bitmask node: extract next byte, descend
    uint8_t ti = static_cast<uint8_t>(ik >> (IK_BITS - 8));
    auto [child, slot, found] = BO::lookup(node, ti);

    if (!found) {
        auto* leaf = make_single_leaf(ik << 8, value, bits - 8);
        auto* nn = BO::add_child(node, hdr, ti, leaf, alloc_);
        return {nn, true};
    }

    // Recurse into child (shift ik past this byte)
    auto [new_child, inserted] = insert_node(child, ik << 8, value, bits - 8);
    if (new_child != child)
        BO::set_child(node, slot, new_child);
    return {node, inserted};
}
```

Callee returns updated pointer; caller writes it into parent's child slot.
Natural call-stack propagation — no frame array needed.

## 13. Erase (Runtime Recursion)

```cpp
bool erase(const KEY& key) {
    IK ik = KO::to_internal(key);
    uint8_t ri = static_cast<uint8_t>(ik >> (IK_BITS - 8));
    ik <<= 8;  // consume root byte

    uint64_t* node = root_[ri];
    if (node == SENTINEL_NODE) return false;

    auto [new_node, erased] = erase_node(node, ik, KEY_BITS - 8);
    if (!erased) return false;

    root_[ri] = new_node ? new_node : SENTINEL_NODE;
    --size_;
    return true;
}
```

### erase_node (recursive)

```cpp
// ik: shifted so next byte to process is at (IK_BITS - 8)
// bits: how many bits of key remain at this node's level
erase_result_t erase_node(uint64_t* node, IK ik, int bits) {
    auto* hdr = get_header(node);

    // Skip check
    uint8_t skip = hdr->skip();
    if (skip) [[unlikely]] {
        const uint8_t* actual = hdr->prefix_bytes();
        for (uint8_t i = 0; i < skip; ++i) {
            uint8_t expected = static_cast<uint8_t>(ik >> (IK_BITS - 8));
            if (expected != actual[i]) return {node, false};
            ik <<= 8;
            bits -= 8;
        }
    }

    if (hdr->is_leaf())
        return leaf_erase(node, hdr, ik);

    // Bitmask node: extract next byte, descend
    uint8_t ti = static_cast<uint8_t>(ik >> (IK_BITS - 8));
    auto [child, slot, found] = BO::lookup(node, ti);
    if (!found) return {node, false};

    // Recurse into child (shift ik past this byte)
    auto [new_child, erased] = erase_node(child, ik << 8, bits - 8);
    if (!erased) return {node, false};

    if (new_child) {
        // Child still exists — update pointer
        if (new_child != child)
            BO::set_child(node, slot, new_child);
        return {node, true};
    }

    // Child fully erased — remove from bitmask
    auto* nn = BO::remove_child(node, hdr, slot, ti, alloc_);
    // nn is nullptr if bitmask is now empty → cascades up naturally
    return {nn, true};
}
```

Cascade removal happens naturally via the call stack: if remove_child returns
nullptr (empty bitmask), the caller sees new_child=nullptr and removes that
slot from its own parent. No explicit stack or cascade loop needed.

## 14. Build / Conversion Functions

### suffix_type_for(int bits_remaining)
```cpp
static constexpr uint8_t suffix_type_for(int bits) noexcept {
    if (bits <= 8)  return 0;  // bitmap256
    if (bits <= 16) return 1;  // u16
    if (bits <= 32) return 2;  // u32
    return 3;                  // u64
}
```

### make_initial_leaf / make_single_leaf

Create single-entry leaf at current bits_remaining. Routes to bitmap256
(suffix_type=0) or compact leaf (suffix_type=1/2/3).

### convert_to_bitmask(node, hdr, ik, value, bits_remaining)

Compact leaf hit COMPACT_MAX. Collect all entries + new entry, group by top 8 bits,
create child nodes at bits_remaining-8, create bitmask node.

### build_node_from_arrays(suf, vals, count, bits)

```
if count <= COMPACT_MAX:
    st = suffix_type_for(bits)
    if st == 0 → build bitmap256 leaf
    else → sort as K, create compact leaf with suffix_type=st
    return leaf

if bits > 8:
    check skip compression (all share top 8 bits)
    if compressible:
        strip top 8, recurse with bits-8, set skip on result
        return child

return build_bitmask_from_arrays(suf, vals, count, bits)
```

Skip compression checks u8 chunks (finer granularity than old u16).

### build_bitmask_from_arrays(suf, vals, count, bits)

Group by top 8 bits. For each group:
- recurse with build_node_from_arrays(group_suf, group_vals, group_count, bits-8)

Create bitmask node with child pointers.

### split_on_prefix

Create new bitmask node at divergence point. Old node (with reduced skip) and
new single-entry leaf as children under diverging u8 indices.

## 15. Remove-All

Self-describing — no context needed:

```cpp
void remove_all() noexcept {
    for (int i = 0; i < 256; ++i) {
        if (root_[i] != SENTINEL_NODE) {
            remove_node(root_[i]);
            root_[i] = SENTINEL_NODE;
        }
    }
    size_ = 0;
}

void remove_node(uint64_t* node) noexcept {
    auto* hdr = get_header(node);
    if (hdr->is_leaf()) {
        destroy_leaf(node, hdr);
    } else {
        BO::for_each_child(node, [&](uint8_t, int, uint64_t* child) {
            remove_node(child);
        });
        BO::dealloc_bitmask(node, alloc_);
    }
}

void destroy_leaf(uint64_t* node, node_header* hdr) noexcept {
    switch (hdr->suffix_type()) {
        case 0: BO::bitmap_destroy_and_dealloc(node, alloc_); break;
        case 1: CO16::destroy_and_dealloc(node, alloc_); break;
        case 2: CO32::destroy_and_dealloc(node, alloc_); break;
        case 3: CO64::destroy_and_dealloc(node, alloc_); break;
    }
}
```

## 16. Stats / Memory

```cpp
struct debug_stats_t {
    size_t compact_leaves = 0;   // suffix_type 1/2/3
    size_t bitmap_leaves = 0;    // suffix_type 0
    size_t bitmask_nodes = 0;    // is_leaf=false
    size_t total_entries = 0;
    size_t total_bytes = 0;
};
```

Self-describing walk — same structure as remove_all:

```cpp
debug_stats_t debug_stats() const noexcept {
    debug_stats_t s{};
    s.total_bytes = 256 * sizeof(uint64_t*);  // root_ array
    for (int i = 0; i < 256; ++i) {
        if (root_[i] != SENTINEL_NODE)
            collect_stats(root_[i], s);
    }
    return s;
}

void collect_stats(const uint64_t* node, debug_stats_t& s) const noexcept {
    auto* hdr = get_header(node);
    s.total_bytes += static_cast<size_t>(hdr->alloc_u64()) * 8;

    if (hdr->is_leaf()) {
        s.total_entries += hdr->entries();
        if (hdr->suffix_type() == 0)
            s.bitmap_leaves++;
        else
            s.compact_leaves++;
    } else {
        s.bitmask_nodes++;
        BO::for_each_child(node, [&](uint8_t, int, uint64_t* child) {
            collect_stats(child, s);
        });
    }
}

size_t memory_usage() const noexcept { return debug_stats().total_bytes; }
```

debug_root_info() returns the count of occupied root slots.

## 17. File Organization

### kntrie_support.hpp
- Namespace: `gteitelbaum`
- `key_ops<KEY>` with `IK`, `to_internal`, `to_key`
- `node_header` (16 bytes)
- Constants: `HEADER_U64=2`, `BITMAP256_U64=4`, `COMPACT_MAX`, `SENTINEL_NODE`
- `slot_table<K, VST>`
- `value_traits<VALUE, ALLOC>`
- `alloc_node`, `dealloc_node`
- `round_up_u64`, `step_up_u64`, `should_shrink_u64`
- `suffix_type_for()`
- Result types

### kntrie_compact.hpp
- `jump_search<K>`
- `compact_ops<K, VALUE, ALLOC>` — K is suffix type (uint16_t/uint32_t/uint64_t)

### kntrie_bitmask.hpp
- `bitmap256`
- `bitmask_ops<VALUE, ALLOC>` — unified bitmask node + bitmap256 leaf ops
  (not templated on KEY; works with raw pointers and byte indices)

### kntrie_impl.hpp
- `gteitelbaum::kntrie_impl<KEY, VALUE, ALLOC>` — implementation class
- Loop-based find
- Recursive insert/erase
- Build/conversion functions
- Remove-all, stats

### kntrie.hpp
- `gteitelbaum::kntrie<KEY, VALUE, ALLOC>` — thin public map-like wrapper
- Delegates to `kntrie_impl` for all operations
- Iterator stubs, operator[], at(), count(), etc.
- Unchanged from current design except includes new kntrie_impl.hpp

Five headers total.

## 18. Key Depth Bounds

`bits` progression per key type (each bitmask hop consumes 8 KEY bits):

| Key type  | KEY_BITS | IK_BITS | bits sequence (root→leaf) | Max bitmask nodes | Max stack |
|-----------|----------|---------|--------------------------|-------------------|-----------|
| uint16_t  | 16       | 32      | 8→bitmap                 | 0                 | 1         |
| int16_t   | 16       | 32      | 8→bitmap                 | 0                 | 1         |
| int32_t   | 32       | 32      | 24→16→8→bitmap           | 2                 | 3         |
| uint32_t  | 32       | 32      | 24→16→8→bitmap           | 2                 | 3         |
| int64_t   | 64       | 64      | 56→48→40→32→24→16→8→bm   | 6                 | 7         |
| uint64_t  | 64       | 64      | 56→48→40→32→24→16→8→bm   | 6                 | 7         |

Max recursion depth for insert_node/erase_node: 7 (uint64_t worst case).
Skip compression typically reduces effective depth to 2-3 for random data.

**Suffix type at each level:**
- bits=56..33: st=3 (u64 suffix)
- bits=32..17: st=2 (u32 suffix)
- bits=16..9:  st=1 (u16 suffix)
- bits=8..1:   st=0 (bitmap256 leaf)

## 19. Changes from Old Design

### Removed
- **Split node type** — replaced by unified bitmask node
- **Fan node type** — replaced by same bitmask node
- **is_internal_bitmap** — children are self-describing
- **branchless_top_child returning (ptr, is_leaf)** — returns just pointer
- **bot_leaf concept** — bitmap256 leaf is self-describing (suffix_type=0)
- **u16 prefix chunks** — replaced by u8 chunks
- **Bit-packed skip/alloc fields** — plain struct members
- **prefix_t = array<uint16_t, 2>** — replaced by uint8_t prefix_[8]
- **Template recursion on BITS** — replaced by loop (find) / runtime recursion (insert/erase)
- **suffix_traits<BITS>** — replaced by direct K template parameter
- **kntrie_impl.hpp** — merged into kntrie.hpp

### Added
- **Self-describing nodes** — suffix_type in header identifies node fully
- **16-byte header** — plain fields, no bit packing for skip/prefix
- **u8 skip chunks** — finer granularity, max skip=6
- **Unified bitmask node** — one layout for all internal nodes (7+n u64s)
- **Single find loop** — no pre-loop special cases, single exit
- **Runtime recursion** for insert/erase — natural parent propagation via call stack

### Changed
- HEADER_U64: 1 → 2 (+8 bytes per node)
- Skip granularity: 16 bits → 8 bits (more compression opportunities)
- Bitmask node size: was 10+n (split) or 6+n (fan) → now 7+n (unified)
- Max trie depth for u64: ~4 (split+fan) → 7 (uniform u8 stride)
  - But skip compression keeps effective depth similar or better

## 20. kntrie_impl Implementation Details

### Type aliases

```cpp
using KO  = key_ops<KEY>;
using IK   = typename KO::IK;
using VT   = value_traits<VALUE, ALLOC>;
using VST  = typename VT::slot_type;
using BO   = bitmask_ops<VALUE, ALLOC>;
using CO16 = compact_ops<uint16_t, VALUE, ALLOC>;
using CO32 = compact_ops<uint32_t, VALUE, ALLOC>;
using CO64 = compact_ops<uint64_t, VALUE, ALLOC>;

static constexpr int IK_BITS  = KO::IK_BITS;   // always 32 or 64
static constexpr int KEY_BITS = KO::KEY_BITS;
```

Note: bitmask_ops is templated on VALUE, ALLOC only (not KEY) since it doesn't
need key operations — it just stores child pointers and bitmap values.

### CRITICAL: `bits` tracks KEY_BITS remaining, not IK_BITS

Throughout insert_node and erase_node, the `bits` parameter tracks how many
**KEY bits** remain to be resolved, NOT how many IK bits remain. This matters
because IK may be wider than KEY (e.g. uint16_t key → uint32_t IK).

```
bits = KEY_BITS - (number of key bits consumed by root + bitmask hops + skip)
```

suffix_type_for(bits) maps remaining key bits to suffix type:
- bits ≤ 8  → st=0 (bitmap256)
- bits ≤ 16 → st=1 (u16)
- bits ≤ 32 → st=2 (u32)
- bits > 32 → st=3 (u64)

Suffix extraction always uses: `ik >> (IK_BITS - suffix_width)`
where suffix_width = 8/16/32/64 depending on st.

This works because ik is left-aligned: after consuming N key bits via
shifts, the remaining key bits sit at positions [IK_BITS-1 .. IK_BITS-bits]
and positions [IK_BITS-bits-1 .. 0] are all zero. Extracting more bits than
remaining (e.g. u32 from 24 remaining) just includes trailing zeros, which
are consistent across all entries.

### Worked Example: uint16_t key = 0x1234

```
KEY_BITS = 16, IK_BITS = 32

to_internal(0x1234):
  ik = 0x1234 << 16 = 0x12340000

insert():
  ri = ik >> 24 = 0x12
  ik <<= 8  → ik = 0x34000000
  bits = KEY_BITS - 8 = 8

  root_[0x12] empty → make_single_leaf(ik=0x34000000, sv, bits=8)
    suffix_type_for(8) = 0 (bitmap256)
    suffix = uint8_t(ik >> 24) = 0x34
    → bitmap leaf with bit 0x34 set

find_value(0x1234):
  ik = 0x12340000
  ri = 0x12
  ik <<= 8  → ik = 0x34000000

  node = root_[0x12], hdr.is_leaf() = true
  st = 0 → bitmap_find(node, suffix = uint8_t(ik >> 24) = 0x34)  ✓
```

### Worked Example: uint64_t key = 0xAABBCCDD11223344

```
KEY_BITS = 64, IK_BITS = 64

to_internal(0xAABBCCDD11223344):
  ik = 0xAABBCCDD11223344 (unsigned, no flip)

insert():
  ri = ik >> 56 = 0xAA
  ik <<= 8  → ik = 0xBBCCDD1122334400
  bits = 64 - 8 = 56

  Suppose root_[0xAA] has a bitmask node:

insert_node(node, ik=0xBBCCDD1122334400, value, bits=56):
  hdr is bitmask → ti = ik >> 56 = 0xBB
  ik <<= 8  → ik = 0xCCDD112233440000
  Recurse: insert_node(child, ik, value, bits=48)

  ... eventually reaches a leaf with bits=16, suffix_type=1:
    suffix = uint16_t(ik >> 48)  ← extracts the 16 remaining key bits
```

### Suffix extraction in find leaf dispatch

After the find loop breaks on is_leaf(), `ik` has been shifted left so that
the remaining suffix bits are at the top. The suffix_type tells us how wide:

```cpp
uint8_t st = hdr.suffix_type();
if (st == 0)
    return BO::bitmap_find(node, hdr,
        static_cast<uint8_t>(ik >> (IK_BITS - 8)));

if constexpr (KEY_BITS > 16) {
    if (st & 0b10) {   // st==2 or st==3
        if constexpr (KEY_BITS > 32) {
            if (st & 0b01)   // st==3 → u64
                return CO64::find(node, hdr, static_cast<uint64_t>(ik));
        }
        // st==2 → u32
        return CO32::find(node, hdr,
            static_cast<uint32_t>(ik >> (IK_BITS - 32)));
    }
}
// st==1 → u16
return CO16::find(node, hdr,
    static_cast<uint16_t>(ik >> (IK_BITS - 16)));
```

The `if constexpr` guards prevent generating u32/u64 paths for small key types
where those suffix types can never occur.

### leaf_insert dispatch

Same suffix_type switch, but returns insert_result_t:

```cpp
insert_result_t leaf_insert(uint64_t* node, node_header* hdr,
                            IK ik, VST value, int bits) {
    uint8_t st = hdr->suffix_type();
    if (st == 0) {
        auto r = BO::bitmap_insert<INSERT, ASSIGN>(
            node, static_cast<uint8_t>(ik >> (IK_BITS - 8)), value, alloc_);
        return r;   // bitmap never needs_split (max 256 entries)
    }
    if constexpr (KEY_BITS > 16) {
        if (st & 0b10) {
            if constexpr (KEY_BITS > 32) {
                if (st & 0b01)
                    return CO64::insert<INSERT, ASSIGN>(
                        node, hdr, static_cast<uint64_t>(ik), value, alloc_);
            }
            return CO32::insert<INSERT, ASSIGN>(
                node, hdr, static_cast<uint32_t>(ik >> (IK_BITS - 32)),
                value, alloc_);
        }
    }
    return CO16::insert<INSERT, ASSIGN>(
        node, hdr, static_cast<uint16_t>(ik >> (IK_BITS - 16)),
        value, alloc_);
}
```

### leaf_erase dispatch

Same pattern, returns erase_result_t:

```cpp
erase_result_t leaf_erase(uint64_t* node, node_header* hdr, IK ik) {
    uint8_t st = hdr->suffix_type();
    if (st == 0)
        return BO::bitmap_erase(node,
            static_cast<uint8_t>(ik >> (IK_BITS - 8)), alloc_);
    if constexpr (KEY_BITS > 16) {
        if (st & 0b10) {
            if constexpr (KEY_BITS > 32) {
                if (st & 0b01)
                    return CO64::erase(node, hdr,
                        static_cast<uint64_t>(ik), alloc_);
            }
            return CO32::erase(node, hdr,
                static_cast<uint32_t>(ik >> (IK_BITS - 32)), alloc_);
        }
    }
    return CO16::erase(node, hdr,
        static_cast<uint16_t>(ik >> (IK_BITS - 16)), alloc_);
}
```

### INSERT/ASSIGN through runtime recursion

insert_node is templated on INSERT and ASSIGN:

```cpp
template<bool INSERT, bool ASSIGN>
insert_result_t insert_node(uint64_t* node, IK ik, VST value, int bits);
```

The three public methods dispatch:
- `insert()` → `insert_node<true, false>`
- `insert_or_assign()` → `insert_node<true, true>`
- `assign()` → `insert_node<false, true>`

This generates 2-3 specializations but keeps the code clean. leaf_insert is
also templated on INSERT/ASSIGN and forwards to CO/BO which already have them.

### make_single_leaf(ik, value, bits)

Creates a 1-entry leaf at the given bits_remaining. `ik` is IK-typed,
left-aligned to IK_BITS:

```cpp
uint64_t* make_single_leaf(IK ik, VST value, int bits) {
    uint8_t st = suffix_type_for(bits);
    if (st == 0) {
        uint8_t s = static_cast<uint8_t>(ik >> (IK_BITS - 8));
        return BO::make_single_bitmap(s, value, alloc_);
    }
    if (st == 1) {
        uint16_t s = static_cast<uint16_t>(ik >> (IK_BITS - 16));
        return CO16::make_leaf(&s, &value, 1, 0, nullptr, alloc_);
    }
    if (st == 2) {
        uint32_t s = static_cast<uint32_t>(ik >> (IK_BITS - 32));
        return CO32::make_leaf(&s, &value, 1, 0, nullptr, alloc_);
    }
    // st == 3
    uint64_t s = static_cast<uint64_t>(ik);
    return CO64::make_leaf(&s, &value, 1, 0, nullptr, alloc_);
}
```

Note: suffix extraction uses `ik >> (IK_BITS - W)` where W = 8/16/32/64.
This is the same formula used in find and leaf_insert dispatch.

### make_initial_leaf(ik, value)

Same as make_single_leaf with bits = KEY_BITS - 8 (after root index consumed).
Can just call make_single_leaf(ik, value, KEY_BITS - 8).

### convert_to_bitmask(node, hdr, ik, value, bits)

Compact leaf hit COMPACT_MAX. Collect all entries + new entry into working
arrays (uint64_t, bit-63-aligned), then call build_node_from_arrays.
The old node's skip/prefix must be prepended to the result.

```cpp
uint64_t* convert_to_bitmask(uint64_t* node, node_header* hdr,
                              IK ik, VST value, int bits) {
    uint16_t old_count = hdr->entries();
    size_t total = old_count + 1;
    auto wk = std::make_unique<uint64_t[]>(total);
    auto wv = std::make_unique<VST[]>(total);

    // ik is IK-aligned; promote to bit-63-aligned uint64_t
    uint64_t new_suf = uint64_t(ik) << (64 - IK_BITS);
    size_t wi = 0;
    bool ins = false;
    // leaf_for_each_u64 emits bit-63-aligned uint64_t
    leaf_for_each_u64(node, hdr, [&](uint64_t s, VST v) {
        if (!ins && new_suf < s) {
            wk[wi] = new_suf; wv[wi] = value; wi++; ins = true;
        }
        wk[wi] = s; wv[wi] = v; wi++;
    });
    if (!ins) { wk[wi] = new_suf; wv[wi] = value; }

    auto* child = build_node_from_arrays(wk.get(), wv.get(), total, bits);

    // Propagate old skip/prefix to new child
    if (hdr->skip() > 0) {
        auto* ch = get_header(child);
        uint8_t os = ch->skip();
        uint8_t ps = hdr->skip();
        uint8_t ns = ps + os;
        uint8_t combined[6] = {};
        std::memcpy(combined, hdr->prefix_bytes(), ps);
        std::memcpy(combined + ps, ch->prefix_bytes(), os);
        ch->set_skip(ns);
        ch->set_prefix(combined, ns);
    }

    dealloc_node(alloc_, node, hdr->alloc_u64());
    return child;
}
```

### Working Array Convention: Bit-63-Aligned

All working arrays (suf[] in build_node_from_arrays, convert_to_bitmask,
etc.) use **uint64_t values left-aligned to bit 63**. This matches the ik
shift convention:

```
Byte extraction:  top_byte = suf >> 56           (always bit 56)
Strip top byte:   child_suf = suf << 8           (always shift left 8)
Extract for CO:   K = narrow_cast<K>(suf >> (64 - W))  where W = suffix width
```

No masks needed. Partitioning and skip compression use the same `>> 56` and
`<< 8` regardless of bits remaining.

### leaf_for_each_u64

Iterates leaf entries, emitting bit-63-aligned uint64_t values:

```cpp
template<typename Fn>
void leaf_for_each_u64(const uint64_t* node, const node_header* hdr, Fn&& cb) {
    uint8_t st = hdr->suffix_type();
    if (st == 0) {
        BO::for_each_bitmap(node, [&](uint8_t s, VST v) {
            cb(uint64_t(s) << 56, v);
        });
    } else if (st == 1) {
        CO16::for_each(node, hdr, [&](uint16_t s, VST v) {
            cb(uint64_t(s) << 48, v);
        });
    } else if (st == 2) {
        CO32::for_each(node, hdr, [&](uint32_t s, VST v) {
            cb(uint64_t(s) << 32, v);
        });
    } else {
        CO64::for_each(node, hdr, [&](uint64_t s, VST v) {
            cb(s, v);
        });
    }
}
```

The bit shifts (56/48/32/0) are independent of bits_remaining. This works
because stored K values are always left-aligned within their type width —
zero-extending to uint64_t and shifting to bit 63 produces consistent values.

### build_node_from_arrays(suf, vals, count, bits)

Suffixes are uint64_t, bit-63-aligned. `bits` tracks remaining KEY bits.

```cpp
uint64_t* build_node_from_arrays(uint64_t* suf, VST* vals,
                                  size_t count, int bits) {
    // Leaf case
    if (count <= COMPACT_MAX) {
        uint8_t st = suffix_type_for(bits);
        if (st == 0) {
            auto bk = std::make_unique<uint8_t[]>(count);
            for (size_t i = 0; i < count; ++i)
                bk[i] = static_cast<uint8_t>(suf[i] >> 56);
            return BO::make_bitmap_leaf(bk.get(), vals, count, alloc_);
        }
        if (st == 1) {
            auto tk = std::make_unique<uint16_t[]>(count);
            for (size_t i = 0; i < count; ++i)
                tk[i] = static_cast<uint16_t>(suf[i] >> 48);
            return CO16::make_leaf(tk.get(), vals, count, 0, nullptr, alloc_);
        }
        if (st == 2) {
            auto tk = std::make_unique<uint32_t[]>(count);
            for (size_t i = 0; i < count; ++i)
                tk[i] = static_cast<uint32_t>(suf[i] >> 32);
            return CO32::make_leaf(tk.get(), vals, count, 0, nullptr, alloc_);
        }
        // st == 3
        return CO64::make_leaf(suf, vals, count, 0, nullptr, alloc_);
    }

    // Skip compression: check if all entries share same top byte
    if (bits > 8) {
        uint8_t first_top = static_cast<uint8_t>(suf[0] >> 56);
        bool all_same = true;
        for (size_t i = 1; i < count; ++i)
            if (static_cast<uint8_t>(suf[i] >> 56) != first_top)
                { all_same = false; break; }

        if (all_same) {
            for (size_t i = 0; i < count; ++i) suf[i] <<= 8;  // strip top byte

            uint64_t* child = build_node_from_arrays(suf, vals, count, bits - 8);

            auto* ch = get_header(child);
            uint8_t os = ch->skip();
            uint8_t ns = os + 1;
            uint8_t combined[6] = {};
            combined[0] = first_top;
            std::memcpy(combined + 1, ch->prefix_bytes(), os);
            ch->set_skip(ns);
            ch->set_prefix(combined, ns);
            return child;
        }
    }

    return build_bitmask_from_arrays(suf, vals, count, bits);
}
```

### build_bitmask_from_arrays(suf, vals, count, bits)

```cpp
uint64_t* build_bitmask_from_arrays(uint64_t* suf, VST* vals,
                                     size_t count, int bits) {
    uint8_t   indices[256];
    uint64_t* child_ptrs[256];
    int       n_children = 0;

    size_t i = 0;
    while (i < count) {
        uint8_t ti = static_cast<uint8_t>(suf[i] >> 56);
        size_t start = i;
        while (i < count && static_cast<uint8_t>(suf[i] >> 56) == ti) ++i;
        size_t cc = i - start;

        // Strip top byte for children (shift left 8)
        auto cs = std::make_unique<uint64_t[]>(cc);
        for (size_t j = 0; j < cc; ++j)
            cs[j] = suf[start + j] << 8;

        indices[n_children]    = ti;
        child_ptrs[n_children] = build_node_from_arrays(
            cs.get(), vals + start, cc, bits - 8);
        n_children++;
    }

    return BO::make_bitmask(indices, child_ptrs, n_children,
                             0, nullptr, alloc_);
}
```

### split_on_prefix

With u8 chunks this is simpler than the old u16 version. Divergence is at
a single byte index `common`. At entry, ik has been shifted past `common`
matching prefix bytes; bits has been decremented by 8 for each.

```cpp
uint64_t* split_on_prefix(uint64_t* node, node_header* hdr,
                           IK ik, VST value,
                           const uint8_t* actual, uint8_t skip,
                           uint8_t common, int bits) {
    // ik >> (IK_BITS - 8) = diverging expected byte
    // actual[common] = diverging actual byte

    uint8_t new_idx = static_cast<uint8_t>(ik >> (IK_BITS - 8));
    uint8_t old_idx = actual[common];
    uint8_t old_rem = skip - 1 - common;

    // Update old node: strip consumed prefix, keep remainder
    hdr->set_skip(old_rem);
    if (old_rem > 0) {
        uint8_t rem[6] = {};
        std::memcpy(rem, actual + common + 1, old_rem);
        hdr->set_prefix(rem, old_rem);
    }

    // Advance ik and bits past divergence byte + remaining prefix
    IK leaf_ik = ik << 8;
    int leaf_bits = bits - 8;
    uint8_t new_prefix[6] = {};
    for (uint8_t j = 0; j < old_rem; ++j) {
        new_prefix[j] = static_cast<uint8_t>(leaf_ik >> (IK_BITS - 8));
        leaf_ik <<= 8;
        leaf_bits -= 8;
    }

    // Build new leaf at same depth as old node
    uint64_t* new_leaf = make_single_leaf(leaf_ik, value, leaf_bits);
    if (old_rem > 0) {
        auto* nlh = get_header(new_leaf);
        nlh->set_skip(old_rem);
        nlh->set_prefix(new_prefix, old_rem);
    }

    // Create parent bitmask with 2 children
    uint8_t   bi[2];
    uint64_t* cp[2];
    if (new_idx < old_idx) {
        bi[0] = new_idx; cp[0] = new_leaf;
        bi[1] = old_idx; cp[1] = node;
    } else {
        bi[0] = old_idx; cp[0] = node;
        bi[1] = new_idx; cp[1] = new_leaf;
    }

    return BO::make_bitmask(bi, cp, 2, common,
                             common > 0 ? actual : nullptr, alloc_);
}
```

**Invariant:** Both children (old node and new leaf) have the same skip value
(`old_rem`) and sit at the same effective depth. The parent bitmask has
skip=`common` with the shared prefix bytes.

## 21. Implementation Order

1. kntrie_support.hpp — new 16-byte header, key_ops, constants
2. kntrie_compact.hpp — compact_ops with HEADER_U64=2, templated on K
3. kntrie_bitmask.hpp — bitmap256, unified bitmask_ops + bitmap leaf ops
4. kntrie_impl.hpp — implementation class: find loop, recursive insert/erase, build, remove_all, stats
5. kntrie.hpp — update thin wrapper to use new kntrie_impl
6. Benchmark verification

## 22. Risk Areas

1. **HEADER_U64=2 slot_table impact:** +8 bytes per node overhead. Verify
   bytes-per-entry at small N doesn't regress badly. The old design used
   HEADER_U64=1. For a 10-entry compact leaf, this is ~5% overhead.

2. **Sentinel:** 6 u64s, all zeros → is_leaf=true (flags_==0), suffix_type=0,
   entries=0, bitmap all zeros. bitmap_find → nullptr. branchless_child miss
   → sentinel. ✓

3. **Max depth u64:** 7 bitmask hops vs old ~4. Skip compression should keep
   effective depth at 2-4 for typical data. Must benchmark.

4. **Suffix extraction consistency:** All paths (find, insert, erase,
   make_single_leaf, leaf_for_each_u64, build_node_from_arrays) must use
   consistent formulas:
   - IK→K: `K(ik >> (IK_BITS - W))` where W = suffix_width
   - K→u64: `uint64_t(K) << (64 - W)` in leaf_for_each_u64
   - u64→K: `K(suf >> (64 - W))` in build_node_from_arrays
   Must verify round-trip: K→u64→K preserves value for all K types.

5. **bits tracks KEY_BITS not IK_BITS:** Insert/erase pass `KEY_BITS - 8`
   to first insert_node/erase_node call. Each bitmask hop decrements by 8.
   Each skip byte decrements by 8. suffix_type_for(bits) must always match
   what the node was created with. Critical invariant.

6. **Recursion depth:** Max 7 for uint64_t. Stack frame ~64 bytes (node ptr,
   ik, value, bits, hdr ptr + locals). 7 × 64 = 448 bytes. Well within limits.

7. **IK promotion in convert_to_bitmask:** `uint64_t(ik) << (64 - IK_BITS)`
   correctly promotes IK=uint32_t to bit-63-aligned uint64_t. For IK=uint64_t,
   shift is 0. Verify no UB for shift-by-0.

8. **Bitmap leaf at bits=8:** Only possible for uint16_t keys (KEY_BITS=16,
   root consumes 8). For larger keys, bitmap leafs appear only after enough
   bitmask hops. Verify bitmap path is reachable and correct for all key types.
