# Plan: Tagged Pointers + Bitmask Skip Chains (v4)

## Goal
1. Move is_leaf test from header memory load to pointer tag bit (bit 63)
2. Bitmask pointers point to bitmap, not header — no offset in read loop
3. Single-child bitmask chains embed in one allocation via "bitmask skip"

## Tagged Pointer Rules

**LEAF_BIT = 1ULL << 63** (sign bit — one-instruction test)

Every pointer in the tree is a tagged uint64_t:
- **Bitmask pointer**: bit 63 = 0. This is a RAW USABLE POINTER. No
  stripping, no masking, no untag. Use it directly as `(uint64_t*)ptr`.
  It points to the bitmap, not the header.
- **Leaf pointer**: bit 63 = 1. ALWAYS strip unconditionally with
  `ptr & ~LEAF_BIT` (or `ptr ^ LEAF_BIT`). No conditional — we know
  the bit is set because that's why we identified it as a leaf.

In the hot loop, ptr is always a bitmask pointer (loop continues while
bit 63 is 0). Use it directly. When the loop exits, ptr is always a
leaf pointer. Strip unconditionally.

```cpp
// HOT LOOP — ptr is bitmask, use as-is
while (!(ptr & LEAF_BIT)) {
    const uint64_t* bm = reinterpret_cast<const uint64_t*>(ptr);  // direct use
    ...
    ptr = bm[4 + slot];
}
// EXIT — ptr is leaf, strip unconditionally
const uint64_t* node = reinterpret_cast<const uint64_t*>(ptr ^ LEAF_BIT);
```

## Unified Skip Concept

The header `skip()` field means the same thing for both node types:
"how many byte-levels are consumed before the real content."

- **Leaf skip**: prefix bytes stored compactly in node[1] (up to 6 bytes
  in 1 u64). Unchanged from current implementation.
- **Bitmask skip**: number of embedded bo<1> nodes before the final
  multi-child bitmask. Each embedded node is a full bitmap256 structure
  (6 u64: bitmap(4) + sentinel(1) + child_ptr(1)).

Same header field, same `skip()` / `set_skip()` methods. The write path
knows which encoding to use based on `is_leaf()`. The read path doesn't
care — it never touches the header.

No new header fields needed. Drop embed_count from earlier plans.

## Design Invariants
- Read path never touches a header. Never.
- Bitmask skip chain ALWAYS ends in a 2+ child final bitmask.
  - If final drops to 1 child on erase → collapse:
    - Sole child is leaf → all skip bytes + final byte → leaf skip prefix
    - Sole child is bitmask → merge skip chains
- `entries` in header = final bitmask's child count (always ≥ 2)
- `skip()` in header = number of embedded bo<1> nodes (0 for standalone)

## Memory Layouts

### Standalone bitmask (skip=0, N≥2 children)
```
node[0]:       header (1 u64, skip=0, entries=N)
node[1..4]:    bitmap256 (4 u64)    ← parent's child ptr points HERE
node[5]:       sentinel (SENTINEL_TAGGED)
node[6..5+N]:  children (N tagged uint64_t)
```
Allocation: 6 + N u64. Parent stores: `(uint64_t)&node[1]` (no LEAF_BIT).

### Bitmask skip chain (skip=S, final has N≥2 children)
```
node[0]:                 header (1 u64, skip=S, entries=N)
node[1..4]:              bitmap_0 (4 u64)     ← parent ptr points HERE
node[5]:                 sentinel_0 (SENTINEL_TAGGED)
node[6]:                 ptr → &node[7] (no LEAF_BIT, next bitmap)
node[7..10]:             bitmap_1 (4 u64)     ← child of embed_0
node[11]:                sentinel_1 (SENTINEL_TAGGED)
node[12]:                ptr → &node[13]
  ...
node[1+S*6 .. 4+S*6]:   bitmap_final (4 u64)  ← child of last embed
node[5+S*6]:             sentinel_final (SENTINEL_TAGGED)
node[6+S*6 .. 5+S*6+N]: children (N tagged uint64_t)
```
Each bo<1> = 6 u64. Allocation: 1 + S*6 + 5 + 1 + N = 6 + S*6 + N u64.
Parent stores: `(uint64_t)&node[1]` (no LEAF_BIT).

**In-place child add**: if adding a child to the final bitmask and
alloc_u64 has room, insert directly in the children array at the end
of the allocation. Embeds don't move. Only realloc the whole chain
when children outgrow the allocation.

### Leaf (any type: compact or bitmap256)
```
node[0]:    header (1 u64)    ← parent stores (uint64_t)&node[0] | LEAF_BIT
node[1]:    optional skip data (if skip > 0)
node[2+]:   leaf-specific data
```
Leaf skip stores up to 6 prefix bytes compactly in node[1]. Unchanged.

### SENTINEL
```
SENTINEL_NODE[8] = {0}    (unchanged, all zeros)
SENTINEL_TAGGED = (uint64_t)&SENTINEL_NODE[0] | LEAF_BIT
```
Sentinel is a valid zeroed leaf: is_leaf=true, skip=0, entries=0,
suffix_type=0. find_value needs no special check — leaf dispatch
returns nullptr naturally for entries=0. Insert and erase detect
sentinel via `ptr == SENTINEL_TAGGED` (tagged pointer equality).

---

## File: kntrie_support.hpp

### S1. Add LEAF_BIT constant
```cpp
static constexpr uint64_t LEAF_BIT = uint64_t(1) << 63;
```

### S2. Add SENTINEL_TAGGED
```cpp
inline const uint64_t SENTINEL_TAGGED =
    reinterpret_cast<uint64_t>(&SENTINEL_NODE[0]) | LEAF_BIT;
```

### S3. Header — no new fields needed
`skip()` already exists. For bitmask nodes, skip = number of embedded
bo<1> nodes. For leaves, skip = number of prefix bytes. Same field.
Max skip = 6 (fits in existing 3-bit encoding, stored in prefix_bytes[7]).

Actually check: current skip encoding stores the count in the first
byte of the skip u64 (node[1]). For bitmask skip chains, there IS no
node[1] in that sense — the embeds ARE the skip. So for bitmask nodes,
skip is stored differently:

- `set_skip(n)` sets the SKIP_BIT flag and stores n in the header.
  Currently uses node[1] byte 7 for the count and bytes 0-5 for data.
- For bitmask: we only need the count, not the bytes (the bytes are
  encoded in the bitmap256 of each embed). Store count in header only.

Current encoding: `skip()` reads `reinterpret_cast<uint8_t*>(&node[1])[7]`.
This won't work for bitmask since node[1] IS the first bitmap word.

**Solution**: store skip count directly in the header struct.
Change `skip()` to read from a header field, not from node[1].

New header layout (8 bytes):
```
flags_(1) suffix_type_(1) entries_(2) alloc_u64_(2) skip_(1) pad(1)
```

Methods:
```cpp
uint8_t skip() const noexcept { return skip_; }
void set_skip(uint8_t n) noexcept {
    skip_ = n;
    if (n > 0) flags_ |= SKIP_BIT; else flags_ &= ~SKIP_BIT;
}
bool is_skip() const noexcept { return flags_ & SKIP_BIT; }
```

For leaves: `prefix_bytes()` still returns `&node[1]` bytes 0-5.
The count comes from `skip_` in the header, not from node[1] byte 7.
Leaf skip data: node[1] stores just the bytes (up to 6), count in header.

For bitmask: `skip()` returns embed count. No prefix_bytes access.

**Migration**: remove the byte-7 count from leaf skip u64. Just store
the prefix bytes in node[1] bytes 0-5. Count lives in header `skip_`.
Update `set_prefix()` and `prefix_bytes()` accordingly.

### S4. Update prefix_bytes / set_prefix
```cpp
const uint8_t* prefix_bytes() const noexcept {
    // Returns pointer to node[1] byte 0. Count comes from skip().
    // Only valid for leaves. Caller gets bytes from this pointer.
    return reinterpret_cast<const uint8_t*>(this + 1);
}
void set_prefix(const uint8_t* bytes, uint8_t count) noexcept {
    uint8_t* dst = reinterpret_cast<uint8_t*>(this + 1);
    std::memcpy(dst, bytes, count);
}
```
Note: `this + 1` = `&node[1]` since header is at node[0].

### S5. Helper functions
```cpp
// Tag a leaf node pointer (adds LEAF_BIT)
static uint64_t tag_leaf(const uint64_t* node) {
    return reinterpret_cast<uint64_t>(node) | LEAF_BIT;
}

// Tag a bitmask node pointer (points to bitmap = node+1, no LEAF_BIT)
static uint64_t tag_bitmask(const uint64_t* node) {
    return reinterpret_cast<uint64_t>(node + 1);
}

// Untag a leaf pointer — UNCONDITIONAL strip, caller knows it's a leaf
static const uint64_t* untag_leaf(uint64_t tagged) {
    return reinterpret_cast<const uint64_t*>(tagged ^ LEAF_BIT);
}
static uint64_t* untag_leaf_mut(uint64_t tagged) {
    return reinterpret_cast<uint64_t*>(tagged ^ LEAF_BIT);
}

// Bitmask tagged ptr → node header (back up 1 u64 from bitmap)
// Only needed by write path. ptr is raw (no LEAF_BIT to strip).
static uint64_t* bm_to_node(uint64_t ptr) {
    return reinterpret_cast<uint64_t*>(ptr) - 1;
}
```

### S6. bitmap256::single_bit_index — NEW helper
Extract the index of the single set bit in a bitmap256.
Used by write path to read which byte an embed represents.

```cpp
uint8_t single_bit_index() const noexcept {
    for (int i = 0; i < 4; ++i)
        if (w[i]) return static_cast<uint8_t>(i * 64 + __builtin_ctzll(w[i]));
    __builtin_unreachable();
}
```

---

## File: kntrie_bitmask.hpp

### B1. Read-path: branchless_find_tagged — NEW
Takes bitmap pointer directly. Returns tagged uint64_t.

```cpp
static uint64_t branchless_find_tagged(const uint64_t* bm_ptr, uint8_t idx) noexcept {
    const bitmap256& bm = *reinterpret_cast<const bitmap256*>(bm_ptr);
    int slot = bm.find_slot<slot_mode::BRANCHLESS>(idx);
    return bm_ptr[4 + slot];  // [0-3]=bitmap, [4]=sentinel, [5+]=children
}
```

### B2. Keep existing node-based accessors for write path
`bm_(node, 1)`, `children_(node, 1)`, `real_children_(node, 1)` unchanged.
Write path uses node pointers (header at node[0]).

### B3. Sentinel → SENTINEL_TAGGED
All locations that write sentinel in children arrays:
```cpp
children_mut_(nn, hs)[0] = SENTINEL_TAGGED;
```
Locations: make_bitmask, add_child realloc, remove_child realloc.

### B4. child_lookup returns tagged uint64_t
```cpp
struct child_lookup {
    uint64_t child;    // tagged value (was uint64_t*)
    int slot;
    bool found;
};
```
lookup() returns raw tagged value from children array.

### B5. set_child stores tagged uint64_t
```cpp
static void set_child(uint64_t* node, int slot, uint64_t tagged_ptr) noexcept {
    real_children_mut_(node, 1)[slot] = tagged_ptr;
}
```

### B6. add_child takes tagged child
Parameter: `uint64_t child_tagged` (was `uint64_t* child_ptr`).
All `reinterpret_cast<uint64_t>(child_ptr)` → just `child_tagged`.

### B7. remove_child — unchanged internally
Operates on children array values which happen to be tagged. Doesn't
interpret them. No change needed.

### B8. for_each_child yields tagged uint64_t
Callback: `(uint8_t idx, int slot, uint64_t tagged_child)`
Was: `(uint8_t idx, int slot, uint64_t* child)`

### B9. make_bitmask takes tagged children
```cpp
static uint64_t* make_bitmask(const uint8_t* indices,
                               const uint64_t* child_tagged_ptrs,
                               unsigned n_children, ALLOC& alloc)
```
Was: `uint64_t* const* child_ptrs`. Now: `const uint64_t*`.
Fills children array with tagged values directly.

### B10. make_skip_chain — NEW
Create one allocation with S embedded bo<1> + final multi-child bitmask.

```cpp
static uint64_t* make_skip_chain(
    const uint8_t* skip_bytes, uint8_t skip_count,
    const uint8_t* final_indices, const uint64_t* final_children_tagged,
    unsigned final_n_children, ALLOC& alloc)
```

Allocation: `6 + skip_count * 6 + final_n_children` u64.
Header: skip = skip_count, entries = final_n_children, set_bitmask.
Each embed: bitmap with 1 bit, sentinel = SENTINEL_TAGGED, child ptr =
address of next embed's bitmap (or final bitmap). No LEAF_BIT on
internal pointers.
Final: bitmap from indices, sentinel, children from tagged array.
Returns node pointer. Caller uses `tag_bitmask(node)` to get first
embed's bitmap address.

### B11. bitmap256 leaf operations — minimal changes
bitmap_insert, bitmap_erase, make_bitmap_leaf, make_single_bitmap:
- Return untagged node pointers. Caller tags with LEAF_BIT.
- Skip copy lines `if (h->is_skip()) nn[1] = ...` stay for leaves.
- header_size parameter stays for leaves.

---

## File: kntrie_impl.hpp

### I1. find_value — new hot loop

```cpp
const VALUE* find_value(const KEY& key) const noexcept {
    IK ik = KO::to_internal(key);
    uint64_t ptr = root_;

    // Bitmask descent — ptr is raw usable pointer (no LEAF_BIT)
    while (!(ptr & LEAF_BIT)) [[likely]] {
        const uint64_t* bm = reinterpret_cast<const uint64_t*>(ptr);
        uint8_t ti = static_cast<uint8_t>(ik >> (IK_BITS - 8));
        ik <<= 8;
        int slot = reinterpret_cast<const bitmap256*>(bm)->
                       find_slot<slot_mode::BRANCHLESS>(ti);
        ptr = bm[4 + slot];
    }

    // Leaf — strip LEAF_BIT unconditionally (we know it's set)
    const uint64_t* node = reinterpret_cast<const uint64_t*>(ptr ^ LEAF_BIT);
    node_header hdr = *get_header(node);
    // No sentinel check — sentinel is a valid zeroed leaf (entries=0,
    // suffix_type=0). Leaf dispatch naturally returns nullptr.

    // Leaf skip check
    size_t hs = 1;
    if (hdr.is_skip()) [[unlikely]] {
        hs = 2;
        const uint8_t* actual = hdr.prefix_bytes_from(node);
        uint8_t skip = hdr.skip();
        uint8_t i = 0;
        do {
            if (static_cast<uint8_t>(ik >> (IK_BITS - 8)) != actual[i])
                [[unlikely]] return nullptr;
            ik <<= 8;
        } while (++i < skip);
    }

    // Leaf dispatch by suffix_type (unchanged)
    ...
}
```

Note: `prefix_bytes_from(node)` may be needed since `prefix_bytes()`
uses `this + 1` which requires header to be at `&node[0]`. If header
is a stack copy, we need the node pointer. Alternative: just use
`reinterpret_cast<const uint8_t*>(&node[1])` directly as before.

### I2. root_ type change
```cpp
uint64_t root_ = SENTINEL_TAGGED;
```

### I3. insert_result_t update
```cpp
struct insert_result_t {
    uint64_t tagged_ptr;    // was: uint64_t* node
    bool inserted;
    bool needs_split;       // only used by leaf_insert_
};
```

### I4. insert_node_ — tagged version

```cpp
template<bool INSERT, bool ASSIGN>
insert_result_t insert_node_(uint64_t ptr, IK ik, VST value, int bits) {

    // --- SENTINEL ---
    if (ptr == SENTINEL_TAGGED) {
        if constexpr (!INSERT) return {ptr, false, false};
        return {make_single_leaf_tagged_(ik, value, bits), true, false};
    }

    // --- LEAF ---
    if (ptr & LEAF_BIT) {
        uint64_t* node = untag_leaf_mut(ptr);
        auto* hdr = get_header(node);

        // Leaf skip check
        uint8_t skip = hdr->skip();
        if (skip) [[unlikely]] {
            const uint8_t* actual = reinterpret_cast<const uint8_t*>(&node[1]);
            for (uint8_t i = 0; i < skip; ++i) {
                uint8_t expected = static_cast<uint8_t>(ik >> (IK_BITS - 8));
                if (expected != actual[i]) {
                    if constexpr (!INSERT) return {ptr, false, false};
                    return {split_on_prefix_tagged_(node, hdr, ik, value,
                                                     actual, skip, i, bits), true, false};
                }
                ik = static_cast<IK>(ik << 8);
                bits -= 8;
            }
        }

        auto result = leaf_insert_<INSERT, ASSIGN>(node, hdr, ik, value, bits);
        if (result.needs_split) {
            if constexpr (!INSERT) return {ptr, false, false};
            return {convert_to_bitmask_tagged_(node, hdr, ik, value, bits), true, false};
        }
        return {tag_leaf(result.node), result.inserted, false};
    }

    // --- BITMASK ---
    uint64_t* node = bm_to_node(ptr);
    auto* hdr = get_header(node);
    uint8_t sc = hdr->skip();

    if (sc > 0) {
        return insert_skip_chain_<INSERT, ASSIGN>(node, hdr, sc, ik, value, bits);
    }

    // Standalone bitmask
    uint8_t ti = static_cast<uint8_t>(ik >> (IK_BITS - 8));
    auto lk = BO::lookup(node, ti);

    if (!lk.found) {
        if constexpr (!INSERT) return {tag_bitmask(node), false, false};
        uint64_t leaf_tagged = make_single_leaf_tagged_(
            static_cast<IK>(ik << 8), value, bits - 8);
        auto* nn = BO::add_child(node, hdr, ti, leaf_tagged, alloc_);
        return {tag_bitmask(nn), true, false};
    }

    auto cr = insert_node_<INSERT, ASSIGN>(
        lk.child, static_cast<IK>(ik << 8), value, bits - 8);
    if (cr.tagged_ptr != lk.child)
        BO::set_child(node, lk.slot, cr.tagged_ptr);
    return {tag_bitmask(node), cr.inserted, false};
}
```

### I5. insert_skip_chain_ — NEW

Walk bitmask skip (embedded bo<1> nodes) matching key bytes.
If all match, operate on final bitmask. If mismatch, split.

```cpp
template<bool INSERT, bool ASSIGN>
insert_result_t insert_skip_chain_(uint64_t* node, node_header* hdr,
                                    uint8_t sc, IK ik, VST value, int bits) {
    for (uint8_t e = 0; e < sc; ++e) {
        uint64_t* bm_at = node + 1 + e * 6;
        const bitmap256& bm = *reinterpret_cast<const bitmap256*>(bm_at);
        uint8_t actual_byte = bm.single_bit_index();
        uint8_t expected = static_cast<uint8_t>(ik >> (IK_BITS - 8));

        if (expected != actual_byte) {
            if constexpr (!INSERT) return {tag_bitmask(node), false, false};
            return {split_skip_at_(node, hdr, sc, e, ik, value, bits), true, false};
        }
        ik = static_cast<IK>(ik << 8);
        bits -= 8;
    }

    // All skip matched — operate on final bitmask
    uint8_t ti = static_cast<uint8_t>(ik >> (IK_BITS - 8));
    uint64_t* final_bm = node + 1 + sc * 6;
    const bitmap256& bm = *reinterpret_cast<const bitmap256*>(final_bm);
    int slot = bm.find_slot<slot_mode::FAST_EXIT>(ti);

    if (slot < 0) {
        if constexpr (!INSERT) return {tag_bitmask(node), false, false};
        uint64_t leaf_tagged = make_single_leaf_tagged_(
            static_cast<IK>(ik << 8), value, bits - 8);
        auto* nn = add_child_to_chain_(node, hdr, sc, ti, leaf_tagged);
        return {tag_bitmask(nn), true, false};
    }

    uint64_t* real_ch = final_bm + 5;
    uint64_t old_child = real_ch[slot];
    auto cr = insert_node_<INSERT, ASSIGN>(
        old_child, static_cast<IK>(ik << 8), value, bits - 8);
    if (cr.tagged_ptr != old_child)
        real_ch[slot] = cr.tagged_ptr;
    return {tag_bitmask(node), cr.inserted, false};
}
```

### I6. add_child_to_chain_ — NEW

Add child to final bitmask of a skip chain. In-place if room.

```cpp
uint64_t* add_child_to_chain_(uint64_t* node, node_header* hdr,
                                uint8_t sc, uint8_t idx,
                                uint64_t child_tagged) {
    unsigned oc = hdr->entries();
    unsigned nc = oc + 1;
    size_t final_offset = 1 + sc * 6;
    size_t needed = final_offset + 5 + 1 + nc;

    if (needed <= hdr->alloc_u64()) {
        bitmap256& bm = *reinterpret_cast<bitmap256*>(node + final_offset);
        uint64_t* children = node + final_offset + 5;
        bitmap256::arr_insert(bm, children, oc, idx, child_tagged);
        hdr->set_entries(nc);
        return node;
    }

    // Realloc whole chain
    size_t au64 = round_up_u64(needed);
    uint64_t* nn = alloc_node(alloc_, au64);
    size_t prefix_u64 = final_offset + 5;
    std::memcpy(nn, node, prefix_u64 * 8);

    auto* nh = get_header(nn);
    nh->set_entries(nc);
    nh->set_alloc_u64(au64);

    // Fix embed internal pointers
    for (uint8_t e = 0; e < sc; ++e) {
        uint64_t* slot = nn + 1 + e * 6 + 5;
        size_t next_offset = (e < sc - 1) ? 1 + (e + 1) * 6 : final_offset;
        *slot = reinterpret_cast<uint64_t>(nn + next_offset);
    }

    // Sentinel
    nn[final_offset + 4] = SENTINEL_TAGGED;

    // Copy-insert children
    bitmap256& old_bm = *reinterpret_cast<bitmap256*>(node + final_offset);
    int isl = old_bm.find_slot<slot_mode::UNFILTERED>(idx);
    bitmap256& new_bm = *reinterpret_cast<bitmap256*>(nn + final_offset);
    new_bm.set_bit(idx);
    bitmap256::arr_copy_insert(
        node + final_offset + 5, nn + final_offset + 5,
        oc, isl, child_tagged);

    dealloc_node(alloc_, node, hdr->alloc_u64());
    return nn;
}
```

### I7. split_skip_at_ — NEW

Key diverges at skip position e. Split the chain.

1. Extract old byte at position e and new byte from key.
2. Build new leaf for divergent key.
3. Build remainder: skip [e+1..sc-1] + final bitmask → new skip chain
   (or standalone if e == sc-1).
4. Create 2-child bitmask: old_byte→remainder, new_byte→new_leaf.
5. If e > 0: wrap in skip chain for bytes [0..e-1].
6. Dealloc old chain. Return tagged ptr.

```cpp
uint64_t split_skip_at_(uint64_t* node, node_header* hdr,
                          uint8_t sc, uint8_t split_pos,
                          IK ik, VST value, int bits) {
    uint8_t expected = static_cast<uint8_t>(ik >> (IK_BITS - 8));
    uint64_t* bm_at = node + 1 + split_pos * 6;
    uint8_t actual_byte = reinterpret_cast<const bitmap256*>(bm_at)->single_bit_index();

    // Build new leaf
    uint64_t new_leaf = make_single_leaf_tagged_(
        static_cast<IK>(ik << 8), value, bits - 8);

    // Build remainder from [split_pos+1..sc-1] + final bitmask
    uint64_t remainder = build_remainder_tagged_(node, hdr, sc, split_pos + 1);

    // Create 2-child bitmask
    uint8_t bi[2];
    uint64_t cp[2];
    if (expected < actual_byte) {
        bi[0] = expected; cp[0] = new_leaf;
        bi[1] = actual_byte; cp[1] = remainder;
    } else {
        bi[0] = actual_byte; cp[0] = remainder;
        bi[1] = expected; cp[1] = new_leaf;
    }
    uint64_t* new_bm_node = BO::make_bitmask(bi, cp, 2, alloc_);

    // Wrap in skip chain for prefix bytes [0..split_pos-1]
    uint64_t result;
    if (split_pos > 0) {
        uint8_t prefix_bytes[6];
        for (uint8_t i = 0; i < split_pos; ++i) {
            uint64_t* eb = node + 1 + i * 6;
            prefix_bytes[i] = reinterpret_cast<const bitmap256*>(eb)->single_bit_index();
        }
        uint64_t* chain = make_skip_chain_for_bitmask_(
            prefix_bytes, split_pos, new_bm_node);
        result = tag_bitmask(chain);
    } else {
        result = tag_bitmask(new_bm_node);
    }

    dealloc_node(alloc_, node, hdr->alloc_u64());
    return result;
}
```

### I8. build_remainder_tagged_ — NEW

Extract skip [from..sc-1] + final bitmask into new allocation.

```cpp
uint64_t build_remainder_tagged_(uint64_t* old_node, node_header* old_hdr,
                                   uint8_t old_sc, uint8_t from_pos) {
    uint8_t rem_skip = old_sc - from_pos;
    unsigned final_nc = old_hdr->entries();
    uint64_t* old_final = old_node + 1 + old_sc * 6;

    if (rem_skip == 0) {
        // Just the final bitmask — extract as standalone
        bitmap256& bm = *reinterpret_cast<bitmap256*>(old_final);
        uint64_t* old_ch = old_final + 5;
        uint8_t indices[256];
        uint64_t children[256];
        int si = 0;
        for (int i = bm.find_next_set(0); i >= 0; i = bm.find_next_set(i + 1)) {
            indices[si] = static_cast<uint8_t>(i);
            children[si] = old_ch[si];
            si++;
        }
        return tag_bitmask(BO::make_bitmask(indices, children, final_nc, alloc_));
    }

    // rem_skip > 0: extract skip bytes + final
    uint8_t skip_bytes[6];
    for (uint8_t i = 0; i < rem_skip; ++i) {
        uint64_t* eb = old_node + 1 + (from_pos + i) * 6;
        skip_bytes[i] = reinterpret_cast<const bitmap256*>(eb)->single_bit_index();
    }

    // Extract final indices + children
    bitmap256& bm = *reinterpret_cast<bitmap256*>(old_final);
    uint64_t* old_ch = old_final + 5;
    uint8_t indices[256];
    uint64_t children[256];
    int si = 0;
    for (int i = bm.find_next_set(0); i >= 0; i = bm.find_next_set(i + 1)) {
        indices[si] = static_cast<uint8_t>(i);
        children[si] = old_ch[si];
        si++;
    }

    uint64_t* chain = BO::make_skip_chain(
        skip_bytes, rem_skip, indices, children, final_nc, alloc_);
    return tag_bitmask(chain);
}
```

### I9. erase_node_ — tagged version

```cpp
erase_result_t erase_node_(uint64_t ptr, IK ik, int bits) {
    // --- SENTINEL ---
    if (ptr == SENTINEL_TAGGED) return {ptr, false};

    // --- LEAF ---
    if (ptr & LEAF_BIT) {
        uint64_t* node = untag_leaf_mut(ptr);
        auto* hdr = get_header(node);

        uint8_t skip = hdr->skip();
        if (skip) [[unlikely]] {
            const uint8_t* actual = reinterpret_cast<const uint8_t*>(&node[1]);
            for (uint8_t i = 0; i < skip; ++i) {
                uint8_t expected = static_cast<uint8_t>(ik >> (IK_BITS - 8));
                if (expected != actual[i]) return {ptr, false};
                ik = static_cast<IK>(ik << 8);
                bits -= 8;
            }
        }
        auto [new_node, erased] = leaf_erase_(node, hdr, ik);
        if (!erased) return {ptr, false};
        if (!new_node) return {0, true};
        return {tag_leaf(new_node), true};
    }

    // --- BITMASK ---
    uint64_t* node = bm_to_node(ptr);
    auto* hdr = get_header(node);
    uint8_t sc = hdr->skip();

    if (sc > 0)
        return erase_skip_chain_(node, hdr, sc, ik, bits);

    // Standalone bitmask
    uint8_t ti = static_cast<uint8_t>(ik >> (IK_BITS - 8));
    auto lk = BO::lookup(node, ti);
    if (!lk.found) return {tag_bitmask(node), false};

    auto [new_child, erased] = erase_node_(
        lk.child, static_cast<IK>(ik << 8), bits - 8);
    if (!erased) return {tag_bitmask(node), false};

    if (new_child) {
        if (new_child != lk.child)
            BO::set_child(node, lk.slot, new_child);
        return maybe_coalesce_(node, hdr, bits);
    }

    // Child erased — remove from bitmask
    auto* nn = BO::remove_child(node, hdr, lk.slot, ti, alloc_);
    if (!nn) return {0, true};

    if (get_header(nn)->entries() == 1)
        return collapse_single_child_(nn, bits);

    return maybe_coalesce_(nn, get_header(nn), bits);
}
```

### I10. erase_skip_chain_ — NEW

Walk skip chain matching bytes. Erase from final bitmask.
Handle collapse when final drops to 1 child.

```cpp
erase_result_t erase_skip_chain_(uint64_t* node, node_header* hdr,
                                   uint8_t sc, IK ik, int bits) {
    for (uint8_t e = 0; e < sc; ++e) {
        uint64_t* bm_at = node + 1 + e * 6;
        uint8_t actual = reinterpret_cast<const bitmap256*>(bm_at)->single_bit_index();
        uint8_t expected = static_cast<uint8_t>(ik >> (IK_BITS - 8));
        if (expected != actual) return {tag_bitmask(node), false};
        ik = static_cast<IK>(ik << 8);
        bits -= 8;
    }

    // Final bitmask
    uint64_t* final_bm = node + 1 + sc * 6;
    uint8_t ti = static_cast<uint8_t>(ik >> (IK_BITS - 8));
    const bitmap256& bm = *reinterpret_cast<const bitmap256*>(final_bm);
    int slot = bm.find_slot<slot_mode::FAST_EXIT>(ti);
    if (slot < 0) return {tag_bitmask(node), false};

    uint64_t* real_ch = final_bm + 5;
    uint64_t old_child = real_ch[slot];

    auto [new_child, erased] = erase_node_(
        old_child, static_cast<IK>(ik << 8), bits - 8);
    if (!erased) return {tag_bitmask(node), false};

    if (new_child) {
        if (new_child != old_child) real_ch[slot] = new_child;
        return maybe_coalesce_chain_(node, hdr, sc, bits);
    }

    // Child erased — remove from final bitmask
    unsigned nc = hdr->entries() - 1;

    if (nc == 0) {
        dealloc_node(alloc_, node, hdr->alloc_u64());
        return {0, true};
    }

    if (nc == 1) {
        // Final down to 1 child → collapse chain
        return collapse_chain_final_(node, hdr, sc, slot, ti, bits);
    }

    // nc >= 2: remove in-place or realloc chain
    remove_child_from_chain_(node, hdr, sc, slot, ti);
    return maybe_coalesce_chain_(node, hdr, sc, bits);
}
```

### I11. collapse_single_child_ — NEW

Standalone bitmask (skip=0) with 1 child remaining.

```cpp
erase_result_t collapse_single_child_(uint64_t* node, int bits) {
    uint64_t sole_child = 0;
    uint8_t sole_idx = 0;
    BO::for_each_child(node, [&](uint8_t idx, int, uint64_t tagged) {
        sole_child = tagged;
        sole_idx = idx;
    });

    if (sole_child & LEAF_BIT) {
        // Child is leaf → absorb byte as leaf skip
        uint64_t* leaf = untag_leaf_mut(sole_child);
        uint8_t byte_arr[1] = {sole_idx};
        leaf = prepend_skip_(leaf, 1, byte_arr);
        dealloc_node(alloc_, node, get_header(node)->alloc_u64());
        return {tag_leaf(leaf), true};
    }

    // Child is bitmask → prepend sole_idx to child's skip chain
    uint64_t* child_node = bm_to_node(sole_child);
    auto* child_hdr = get_header(child_node);
    uint8_t child_sc = child_hdr->skip();

    // Collect: sole_idx + child's skip bytes + child's final bitmask
    uint8_t all_bytes[7];
    all_bytes[0] = sole_idx;
    for (uint8_t i = 0; i < child_sc; ++i) {
        uint64_t* eb = child_node + 1 + i * 6;
        all_bytes[1 + i] = reinterpret_cast<const bitmap256*>(eb)->single_bit_index();
    }
    uint8_t new_sc = 1 + child_sc;

    // Extract child's final bitmask indices + children
    uint64_t* child_final = child_node + 1 + child_sc * 6;
    bitmap256& cfbm = *reinterpret_cast<bitmap256*>(child_final);
    uint64_t* cfch = child_final + 5;
    uint8_t indices[256];
    uint64_t children[256];
    unsigned cnc = child_hdr->entries();
    int si = 0;
    for (int i = cfbm.find_next_set(0); i >= 0; i = cfbm.find_next_set(i + 1)) {
        indices[si] = static_cast<uint8_t>(i);
        children[si] = cfch[si];
        si++;
    }

    uint64_t* chain = BO::make_skip_chain(
        all_bytes, new_sc, indices, children, cnc, alloc_);
    dealloc_node(alloc_, node, get_header(node)->alloc_u64());
    dealloc_node(alloc_, child_node, child_hdr->alloc_u64());
    return {tag_bitmask(chain), true};
}
```

### I12. collapse_chain_final_ — NEW

Skip chain's final bitmask drops to 1 child. All skip bytes + sole
remaining byte → prefix for that child.

```cpp
erase_result_t collapse_chain_final_(uint64_t* node, node_header* hdr,
                                       uint8_t sc, int removed_slot,
                                       uint8_t removed_idx, int bits) {
    // Find sole remaining child
    uint64_t* final_bm_ptr = node + 1 + sc * 6;
    bitmap256 bm = *reinterpret_cast<const bitmap256*>(final_bm_ptr);
    bm.clear_bit(removed_idx);
    uint8_t sole_idx = static_cast<uint8_t>(bm.find_next_set(0));
    // Compute slot of sole_idx in original array (it's before or after removed_slot)
    uint64_t* real_ch = final_bm_ptr + 5;
    int orig_slot = reinterpret_cast<const bitmap256*>(final_bm_ptr)->
                        find_slot<slot_mode::FAST_EXIT>(sole_idx);
    uint64_t sole_child = real_ch[orig_slot];

    // Collect bytes: skip[0..sc-1] + sole_idx
    uint8_t all_bytes[7];
    for (uint8_t i = 0; i < sc; ++i) {
        uint64_t* eb = node + 1 + i * 6;
        all_bytes[i] = reinterpret_cast<const bitmap256*>(eb)->single_bit_index();
    }
    all_bytes[sc] = sole_idx;
    uint8_t total = sc + 1;

    size_t node_au64 = hdr->alloc_u64();

    if (sole_child & LEAF_BIT) {
        // All bytes → leaf skip
        uint64_t* leaf = untag_leaf_mut(sole_child);
        leaf = prepend_skip_(leaf, total, all_bytes);
        dealloc_node(alloc_, node, node_au64);
        return {tag_leaf(leaf), true};
    }

    // Sole child is bitmask → merge: all_bytes + child's skip + child's final
    uint64_t* child_node = bm_to_node(sole_child);
    auto* child_hdr = get_header(child_node);
    uint8_t child_sc = child_hdr->skip();

    uint8_t merged_bytes[13];
    std::memcpy(merged_bytes, all_bytes, total);
    for (uint8_t i = 0; i < child_sc; ++i) {
        uint64_t* eb = child_node + 1 + i * 6;
        merged_bytes[total + i] = reinterpret_cast<const bitmap256*>(eb)->single_bit_index();
    }
    uint8_t merged_sc = total + child_sc;

    // Extract child's final bitmask
    uint64_t* child_final = child_node + 1 + child_sc * 6;
    bitmap256& cfbm = *reinterpret_cast<bitmap256*>(child_final);
    uint64_t* cfch = child_final + 5;
    uint8_t indices[256];
    uint64_t children[256];
    unsigned cnc = child_hdr->entries();
    int si = 0;
    for (int i = cfbm.find_next_set(0); i >= 0; i = cfbm.find_next_set(i + 1)) {
        indices[si] = static_cast<uint8_t>(i);
        children[si] = cfch[si];
        si++;
    }

    uint64_t* chain = BO::make_skip_chain(
        merged_bytes, merged_sc, indices, children, cnc, alloc_);
    dealloc_node(alloc_, node, node_au64);
    dealloc_node(alloc_, child_node, child_hdr->alloc_u64());
    return {tag_bitmask(chain), true};
}
```

### I13. maybe_coalesce_ / maybe_coalesce_chain_ — NEW

Count total entries in bitmask subtree. If ≤ COMPACT_MAX, collapse
to CO leaf. Always check on erase.

```cpp
erase_result_t maybe_coalesce_(uint64_t* node, node_header* hdr, int bits) {
    size_t total = count_entries_tagged_(tag_bitmask(node));
    if (total > COMPACT_MAX)
        return {tag_bitmask(node), true};

    // Collect all entries as bit-63-aligned u64, build CO leaf
    auto wk = std::make_unique<uint64_t[]>(total);
    auto wv = std::make_unique<VST[]>(total);
    size_t wi = 0;
    collect_entries_tagged_(tag_bitmask(node), 0, wk.get(), wv.get(), wi);

    uint64_t* leaf = build_leaf_from_arrays_(wk.get(), wv.get(), total, bits);
    // Transfer skip from bitmask to leaf if applicable
    uint8_t sc = hdr->skip();
    if (sc > 0) {
        uint8_t skip_bytes[6];
        for (uint8_t i = 0; i < sc; ++i) {
            uint64_t* eb = node + 1 + i * 6;
            skip_bytes[i] = reinterpret_cast<const bitmap256*>(eb)->single_bit_index();
        }
        leaf = prepend_skip_(leaf, sc, skip_bytes);
    }

    destroy_bitmask_subtree_(node, hdr);
    return {tag_leaf(leaf), true};
}
```

Note: `build_leaf_from_arrays_` is the leaf-building part of
`build_node_from_arrays_` (just the count ≤ COMPACT_MAX branch).
`collect_entries_tagged_` recursively collects (suffix, value) pairs
from the subtree.

### I14. convert_to_bitmask_tagged_ — returns tagged uint64_t

build_node_from_arrays_ returns untagged node. Determine type, tag:

```cpp
uint64_t convert_to_bitmask_tagged_(uint64_t* node, node_header* hdr,
                                      IK ik, VST value, int bits) {
    // ... collect entries, build node via build_node_from_arrays_ ...
    auto* child = build_node_from_arrays_(wk.get(), wv.get(), total, bits);
    uint64_t child_tagged = get_header(child)->is_leaf()
        ? tag_leaf(child) : tag_bitmask(child);

    // Reattach old skip
    uint8_t ps = hdr->skip();
    if (ps > 0) {
        if (child_tagged & LEAF_BIT) {
            child = untag_leaf_mut(child_tagged);
            child = prepend_skip_(child, ps, reinterpret_cast<const uint8_t*>(&node[1]));
            child_tagged = tag_leaf(child);
        } else {
            // Wrap bitmask in skip chain
            uint64_t* bm_node = bm_to_node(child_tagged);
            uint64_t* chain = make_skip_chain_for_bitmask_(
                reinterpret_cast<const uint8_t*>(&node[1]), ps, bm_node);
            child_tagged = tag_bitmask(chain);
        }
    }

    dealloc_node(alloc_, node, hdr->alloc_u64());
    return child_tagged;
}
```

### I15. split_on_prefix_tagged_ — returns tagged uint64_t

Creates 2-child bitmask, wraps in skip chain for common prefix.

### I16. build_node_from_arrays_ inner skip routing

When all share top byte, recurse, then:
- Leaf → prepend_skip_, return tag_leaf
- Bitmask → make skip chain wrapping it, return tag_bitmask

### I17. build_bitmask_from_arrays_ — tagged children

Each child from recursion is tagged. make_bitmask gets tagged array.

### I18. for_each / iteration — tagged

Sentinel: `ptr == SENTINEL_TAGGED` → return.
Leaf: untag, iterate entries.
Bitmask: walk skip embeds silently (no callback), then iterate
final bitmask's children (recurse with tagged ptrs).

### I19. destroy — tagged

For sentinel: `ptr == SENTINEL_TAGGED` → return (don't free sentinel).
For leaf: untag, destroy as before.
For bitmask: get node via bm_to_node, read skip. Only recurse into
final bitmask's children (embeds point within same allocation).
Dealloc whole chain as one.

### I20. make_single_leaf_tagged_
```cpp
uint64_t make_single_leaf_tagged_(IK ik, VST value, int bits) {
    return tag_leaf(make_single_leaf_(ik, value, bits));
}
```

### I21. make_skip_chain_for_bitmask_ — NEW helper
Wrap an existing standalone bitmask in a skip chain.
Extracts its bitmap + children, creates new allocation via make_skip_chain,
deallocates the standalone.

### I22. remove_child_from_chain_ — NEW
Remove child from final bitmask of a skip chain. In-place if possible,
realloc chain if shrink needed.

---

## File: kntrie_compact.hpp

### C1. No structural changes
Functions receive/return untagged node pointers. Caller tags.

---

## File: kntrie.hpp

### K1. `uint64_t root_ = SENTINEL_TAGGED;`
### K2. `empty()`: `return root_ == SENTINEL_TAGGED;`
### K3. Public methods use tagged root_
### K4. swap/move/copy: swap/copy uint64_t values

---

## Implementation Order

### Phase 1: Tagged pointers (skip=0 only, no chains)
1. S1-S6: LEAF_BIT, SENTINEL_TAGGED, header skip_ field, helpers
2. K1-K4: root_ type, public API
3. B1, B3-B9: bitmask ops tagged children
4. I1: find_value hot loop
5. I2-I4: insert_node_ tagged (leaf + standalone bitmask only)
6. I9: erase_node_ tagged (leaf + standalone only)
7. I14-I17: convert, split_on_prefix, build arrays — tagged
8. I18-I19: for_each, destroy — tagged
9. I20: make_single_leaf_tagged_
Compile + test. Skip chains don't form yet.

### Phase 2: Bitmask skip chains
10. B10: make_skip_chain
11. I21: make_skip_chain_for_bitmask_
12. Replace wrap_bitmask_chain_ → skip chain creation
13. I5: insert_skip_chain_
14. I6: add_child_to_chain_
15. I7: split_skip_at_
16. I8: build_remainder_tagged_
Compile + test. Chains form on insert.

### Phase 3: Erase collapse
17. I10: erase_skip_chain_
18. I11: collapse_single_child_
19. I12: collapse_chain_final_
20. I13: maybe_coalesce_ (subtree → CO leaf)
21. I22: remove_child_from_chain_
22. Thread `bits` through erase
Compile + test.

---

## Key Invariants
- Bitmask skip chain always ends in 2+ child final bitmask
- skip() in header = number of embedded bo<1> nodes (bitmask) or
  number of prefix bytes (leaf). Same field, different encoding.
- entries() = final bitmask child count (≥2 when skip>0)
- All children array values are tagged uint64_t
- Bitmask pointers are raw — no masking ever in hot loop
- Leaf pointers always have LEAF_BIT — strip unconditionally
- Round-trip insert/erase produces identical tree structure
