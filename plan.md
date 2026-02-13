# kntrie Refactor: Loop-Based Traversal + Native-Width Internal Keys

## 1. Goals

1. Replace recursive template dispatch with iterative loops using shift-based key consumption.
2. Use native-width internal keys: `uint32_t` for keys ≤32 bits, `uint64_t` for keys >32 bits.
3. All three hot paths (find, insert, erase) get converted. No hybrid.
4. compact_ops templated on suffix type (u8/u16/u32/u64) instead of every BITS value.
5. Suffix type stored in node header — no `bits_remaining` tracking needed for leaf dispatch in find.
6. Preserve all existing allocation strategies, dup seeding, skip compression.

## 2. Naming Conventions

- **Constants, enum values, macros:** ALL_CAPS (e.g., `HEADER_U64`, `COMPACT_MAX`, `SENTINEL_NODE`)
- **Everything else:** lowercase with underscores (e.g., `find_value`, `node_header`, `branchless_child`)
- **Class/struct member data:** ends with `_` (e.g., `top_`, `size_`, `alloc_`)
- **Function names:** do NOT end with `_` (e.g., `keys()`, `vals()`, `main_bm()`)
- **Data-only structs (return types, POD):** name ends with `_t` (e.g., `insert_result_t`, `erase_result_t`, `child_lookup_t`)
- **Main class:** `kntrie` (was `kntrie3`)
- **Namespace:** `gteitelbaum` (was `kn3`) — all implementation types live here
- **No separate wrapper class.** The old two-class design (public `kntrie` wrapping `kntrie3`) is eliminated.

## 3. Internal Key Representation

### Type Selection
```cpp
using internal_key_t = std::conditional_t<sizeof(KEY) <= 4, uint32_t, uint64_t>;
static constexpr int IK_BITS = sizeof(internal_key_t) * 8;  // 32 or 64
static constexpr int KEY_BITS = sizeof(KEY) * 8;
```

### Conversion (left-aligned in native type)
```cpp
static constexpr internal_key_t to_internal(KEY k) noexcept {
    internal_key_t r;
    if constexpr (sizeof(KEY) == 1)      r = static_cast<uint8_t>(k);
    else if constexpr (sizeof(KEY) == 2) r = static_cast<uint16_t>(k);
    else if constexpr (sizeof(KEY) == 4) r = static_cast<uint32_t>(k);
    else                                 r = static_cast<uint64_t>(k);
    if constexpr (IS_SIGNED) r ^= internal_key_t{1} << (KEY_BITS - 1);
    r <<= (IK_BITS - KEY_BITS);  // left-align
    return r;
}
```

Key is **left-aligned** in `internal_key_t`. Top bits consumed first via shift.

### Bit Extraction (always from top, then shift)
```cpp
uint8_t  ti = static_cast<uint8_t>(ik >> (IK_BITS - 8));   // top 8
uint16_t pc = static_cast<uint16_t>(ik >> (IK_BITS - 16));  // top 16 (prefix)
ik <<= 8;   // consume 8 bits
ik <<= 16;  // consume 16 bits (skip chunk)
```

### Root Index
```cpp
uint8_t ri = static_cast<uint8_t>(ik >> (IK_BITS - 8));
ik <<= 8;
```

## 4. Node Types

### 4a. Compact Leaf
- `[header (1 u64)][sorted K[] (aligned)][VST[] (aligned)]`
- `is_leaf() == true` (bit 15 of top_ is 0)
- K determined by `suffix_type()` in header (bits 14-13)
- **Only u16/u32/u64 suffix types.** When ≤8 bits remain, bitmap256 is used instead.
  - suffix_type 0 (u8) is dead/unused for compact leaves
  - suffix_type 1 = u16, 2 = u32, 3 = u64
- Can have skip/prefix for compression

### 4b. Split Node (has is_internal_bitmap)
- `[header (1 u64)][main_bitmap (4 u64)][is_internal_bitmap (4 u64)][sentinel (1 u64)][children[]]`
- `is_leaf() == false` (bit 15 = 1)
- `main_bitmap`: which children exist
- `is_internal_bitmap`: bit set = child is fan; bit clear = child is leaf (compact or bitmap256)
- `sentinel`: `children[0]` = SENTINEL_NODE for branchless miss
- `children[]`: real child pointers at offset 10
- Size: `10 + n_children` u64s
- Can have skip/prefix

### 4c. Fan Node (no is_internal_bitmap)
- `[header (1 u64)][main_bitmap (4 u64)][sentinel (1 u64)][children[]]`
- `is_leaf() == false` (bit 15 = 1)
- All children go back to top of loop (compact leaf or split)
- `sentinel`: `children[0]` = SENTINEL_NODE
- `children[]`: real child pointers at offset 6
- Size: `6 + n_children` u64s
- No skip/prefix (always one level under a split or at root)

### 4d. Bitmap256 Leaf
- `[header (1 u64)][bitmap256 (4 u64)][VST[] (aligned)]`
- `is_leaf() == false` (bit 15 = 1) — this is the key insight: bitmap256 uses is_bitmask=1
- Distinguished from compact leaf by the child's own `is_leaf()` flag
- Parent's `is_internal_bitmap` says `is_leaf=true` (it's a leaf child, not a fan child)
- Direct O(1) lookup via popcount
- 8-bit suffix space
- Always terminal

### 4e. Root Array
- `root_[256]` indexed by top 8 bits of internal key
- Each slot: SENTINEL_NODE, compact_leaf, or fan_node

## 5. Node Type Identification

### Key insight: no extra header bits needed

The type of every node is determined by context + `is_leaf()`:

| Context | `is_leaf()=true` | `is_leaf()=false` |
|---------|-------------------|---------------------|
| Root slot | Compact leaf | Fan node |
| Fan child | Compact leaf | Split node |
| Split leaf-child (`is_internal_bitmap`=0) | Compact leaf | Bitmap256 leaf |
| Split internal-child (`is_internal_bitmap`=1) | — (never) | Fan node |

The only "surprising" case: bitmap256 leaves have `is_leaf()=false` (is_bitmask=1 in header). The parent's `is_internal_bitmap` having bit=0 (leaf child) combined with child's `is_leaf()=false` uniquely identifies bitmap256.

### For remove_all / stats

Walk with context:
- Root children: `is_leaf()` → compact, else fan
- Fan children: `is_leaf()` → compact, else split
- Split children: use `is_internal_bitmap`
  - Leaf children: `child.is_leaf()` → compact, else bitmap256
  - Internal children: always fan

### For find loop

```
loop top → hdr.is_leaf() → compact leaf (break)
         → split node:
             branchless_top_child → is_leaf from is_internal_bitmap
               is_leaf=true → read child hdr:
                   child.is_leaf()=true → compact leaf (break)
                   child.is_leaf()=false → bitmap256 (break)
               is_leaf=false → fan node:
                   branchless_child → next node
                   back to loop top
```

## 6. Header Layout

```
top_ (uint16_t):
  bit 15    = is_bitmask (0=compact leaf, 1=split/fan/bitmap256)
  bit 14-13 = suffix_type (compact leaf only: 00=reserved/sentinel, 01=u16, 10=u32, 11=u64)
  bit 12-0  = entries (0-8191)

bottom_ (uint16_t):
  bit 15-14 = skip (0-2)
  bit 13-0  = alloc_u64 (0-16383)

prefix_[2] (uint16_t each):
  skip chunks
```

COMPACT_MAX = 4096, BOT_LEAF_MAX = 4096 (13-bit entries, unchanged from original).

Sentinel (all zeros): `is_bitmask=0, suffix_type=0, entries=0` → compact leaf, 0 entries → find returns nullptr. suffix_type=0 is unused for real nodes but harmless for sentinel. ✓

```cpp
struct node_header {
    uint16_t top_;
    uint16_t bottom_;
    uint16_t prefix_[2];

    bool is_leaf() const noexcept { return !(top_ & 0x8000); }
    void set_bitmask() noexcept { top_ |= 0x8000; }

    uint8_t suffix_type() const noexcept { return (top_ >> 13) & 0x3; }
    void set_suffix_type(uint8_t t) noexcept { top_ = (top_ & ~0x6000) | ((t & 0x3) << 13); }

    uint16_t entries() const noexcept { return top_ & 0x1FFF; }
    void set_entries(uint16_t n) noexcept { top_ = (top_ & 0xE000) | (n & 0x1FFF); }

    uint16_t alloc_u64() const noexcept { return bottom_ & 0x3FFF; }
    void set_alloc_u64(uint16_t n) noexcept { bottom_ = (bottom_ & 0xC000) | (n & 0x3FFF); }

    uint8_t skip() const noexcept { return static_cast<uint8_t>(bottom_ >> 14); }
    void set_skip(uint8_t s) noexcept { bottom_ = (bottom_ & 0x3FFF) | (uint16_t(s & 0x3) << 14); }

    prefix_t prefix() const noexcept { return {prefix_[0], prefix_[1]}; }
    void set_prefix(prefix_t p) noexcept { prefix_[0] = p[0]; prefix_[1] = p[1]; }
};
```

Suffix type in header is set during `compact_ops::make_leaf`. Only meaningful for compact leaves.

## 7. Loop-Based Find

```cpp
const VALUE* find_value(const KEY& key) const noexcept {
    internal_key_t ik = key_ops::to_internal(key);
    uint8_t ri = static_cast<uint8_t>(ik >> (IK_BITS - 8));
    ik <<= 8;

    const uint64_t* node = root_[ri];
    node_header hdr = *get_header(node);

    // Root: compact leaf → handle immediately
    if (hdr.is_leaf()) [[unlikely]]
        return compact_find(node, hdr, ik);

    // Root is fan: descend one level
    node = fan_ops::branchless_child(node, static_cast<uint8_t>(ik >> (IK_BITS - 8)));
    ik <<= 8;
    hdr = *get_header(node);

    // Main descent loop
    bool child_leaf = false;
    while (true) {
        // Handle skip/prefix
        if (hdr.skip()) [[unlikely]] {
            prefix_t actual = hdr.prefix();
            int skip = hdr.skip();
            for (int i = 0; i < skip; ++i) {
                uint16_t expected = static_cast<uint16_t>(ik >> (IK_BITS - 16));
                if (expected != actual[i]) [[unlikely]] return nullptr;
                ik <<= 16;
            }
        }

        // Compact leaf: exit (only reached once — termination)
        if (hdr.is_leaf()) [[unlikely]] break;

        // Split node: top child + leaf check
        uint8_t ti = static_cast<uint8_t>(ik >> (IK_BITS - 8));
        ik <<= 8;
        auto [child, is_leaf] = split_ops::branchless_top_child(node, ti);
        node = child;
        hdr = *get_header(node);

        if (is_leaf) [[unlikely]] {
            child_leaf = true;
            break;
        }

        // Child is fan: consume 8 more bits
        node = fan_ops::branchless_child(node, static_cast<uint8_t>(ik >> (IK_BITS - 8)));
        ik <<= 8;
        hdr = *get_header(node);
    }

    // Split's leaf children are always bitmap256
    if (child_leaf)
        return bitmap256_find(node, static_cast<uint8_t>(ik >> (IK_BITS - 8)));

    // Exited via is_leaf() at top of loop: always compact
    return compact_find(node, hdr, ik);
}
```

### split_ops::branchless_top_child

```cpp
struct top_child_result_t {
    const uint64_t* child;
    bool is_leaf;
};

static top_child_result_t branchless_top_child(const uint64_t* node, uint8_t ti) noexcept {
    const bitmap256& tbm = main_bm(node);
    int slot = tbm.find_slot<BRANCHLESS>(ti);
    auto* child = reinterpret_cast<const uint64_t*>(real_children(node)[slot]);
    bool is_leaf = !internal_bm(node).has_bit(ti);
    return {child, is_leaf};
}
```

On miss (bit not set): slot=0 → sentinel. `is_leaf` from `internal_bm` is arbitrary. If `is_leaf=true`: break, `hdr.is_leaf()` on sentinel = true (zeroed) → compact_find returns nullptr. If `is_leaf=false`: fan branchless_child on sentinel → sentinel → zeroed header → `is_leaf()=true` → compact_find returns nullptr. Both paths safe.

### fan_ops::branchless_child

```cpp
static const uint64_t* branchless_child(const uint64_t* node, uint8_t bi) noexcept {
    const bitmap256& bm = main_bm(node);
    int slot = bm.find_slot<BRANCHLESS>(bi);
    return reinterpret_cast<const uint64_t*>(children(node)[slot]);
}
```

### compact_find

```cpp
// Nested bit tests: u16 is most common (fallthrough), u32/u64 behind one branch.
const VALUE* compact_find(const uint64_t* node, node_header hdr,
                           internal_key_t ik) const noexcept {
    uint8_t st = hdr.suffix_type();
    if (st & 0b10) {
        if (st & 0b01)
            return compact_ops::find<uint64_t>(node, hdr, static_cast<uint64_t>(ik));
        else
            return compact_ops::find<uint32_t>(node, hdr,
                       static_cast<uint32_t>(ik >> (IK_BITS - 32)));
    }
    return compact_ops::find<uint16_t>(node, hdr,
               static_cast<uint16_t>(ik >> (IK_BITS - 16)));
}
```

### bitmap256_find

```cpp
const VALUE* bitmap256_find(const uint64_t* node, uint8_t suffix) const noexcept {
    const bitmap256& bm = main_bm(node);  // bitmap at same offset as bitmask nodes
    int slot = bm.find_slot<FAST_EXIT>(suffix);
    if (slot < 0) [[unlikely]] return nullptr;
    return value_traits::as_ptr(bitmap_leaf_vals(node)[slot]);
}
```

## 8. Loop-Based Insert

### Descent Stack

```cpp
enum class parent_type : uint8_t { ROOT, SPLIT, FAN };

struct descent_entry_t {
    uint64_t*   node;
    parent_type type;
    uint8_t     index;
    int16_t     slot;
};

static constexpr int MAX_DEPTH = 10;
```

### bits tracking

Insert/erase need `bits` for creating new leaves with correct suffix type:
```cpp
int bits = KEY_BITS - 8;
// each ik <<= 8:  bits -= 8
// each ik <<= 16: bits -= 16
```

### Suffix type helpers

```cpp
// Only for compact leaves. bits <= 8 uses bitmap256 instead.
static uint8_t suffix_type_for(int bits) noexcept {
    if (bits <= 16) return 1;  // u16
    if (bits <= 32) return 2;  // u32
    return 3;                  // u64
}

// Dispatch compact leaf operations. Only 3 types (no u8).
template<typename Fn>
static auto dispatch_suffix(uint8_t stype, Fn&& fn) {
    if (stype & 0b10) {
        if (stype & 0b01) return fn(uint64_t{});
        else              return fn(uint32_t{});
    }
    return fn(uint16_t{});
}
```

### Algorithm

```cpp
template<bool INSERT, bool ASSIGN>
std::pair<bool, bool> insert_impl(const KEY& key, const VALUE& value) {
    internal_key_t ik = key_ops::to_internal(key);
    vst sv = value_traits::store(value, alloc_);
    uint8_t ri = static_cast<uint8_t>(ik >> (IK_BITS - 8));
    ik <<= 8;
    int bits = KEY_BITS - 8;

    uint64_t* node = root_[ri];

    // Empty root slot
    if (node == SENTINEL_NODE) {
        if constexpr (!INSERT) { value_traits::destroy(sv, alloc_); return {true, false}; }
        root_[ri] = make_single_leaf(ik, sv, bits);
        ++size_;
        return {true, true};
    }

    descent_entry_t stack[MAX_DEPTH];
    int depth = 0;
    node_header* hdr = get_header(node);

    // Root compact leaf
    if (hdr->is_leaf()) {
        auto r = compact_insert<INSERT, ASSIGN>(node, hdr, ik, sv);
        if (r.needs_split) {
            if constexpr (!INSERT) { value_traits::destroy(sv, alloc_); return {true, false}; }
            root_[ri] = convert_to_split(node, hdr, ik, sv, bits);
            ++size_;
            return {true, true};
        }
        root_[ri] = r.node;
        if (r.inserted) { ++size_; return {true, true}; }
        value_traits::destroy(sv, alloc_);
        return {true, false};
    }

    // Root is fan: descend
    uint8_t bi = static_cast<uint8_t>(ik >> (IK_BITS - 8));
    auto blk = fan_ops::lookup_child(node, bi);
    if (!blk.found) {
        if constexpr (!INSERT) { value_traits::destroy(sv, alloc_); return {true, false}; }
        ik <<= 8; bits -= 8;
        auto* leaf = make_single_leaf(ik, sv, bits);
        auto* new_fan = fan_ops::add_child(node, bi, leaf, alloc_);
        if (new_fan != node) root_[ri] = new_fan;
        ++size_;
        return {true, true};
    }
    stack[depth++] = {node, parent_type::FAN, bi, static_cast<int16_t>(blk.slot)};
    ik <<= 8; bits -= 8;
    node = blk.child;
    hdr = get_header(node);

    // Main descent loop
    while (true) {
        // Handle skip/prefix
        int skip = hdr->skip();
        if (skip > 0) {
            prefix_t actual = hdr->prefix();
            for (int i = 0; i < skip; ++i) {
                uint16_t expected = static_cast<uint16_t>(ik >> (IK_BITS - 16));
                if (expected != actual[i]) {
                    if constexpr (!INSERT) { value_traits::destroy(sv, alloc_); return {true, false}; }
                    auto* nn = split_on_prefix(node, hdr, ik, sv, bits, i, expected, actual);
                    propagate(stack, depth, nn, node);
                    ++size_;
                    return {true, true};
                }
                ik <<= 16; bits -= 16;
            }
        }

        // Compact leaf: insert here
        if (hdr->is_leaf()) {
            auto r = compact_insert<INSERT, ASSIGN>(node, hdr, ik, sv);
            if (r.needs_split) {
                if constexpr (!INSERT) { value_traits::destroy(sv, alloc_); return {true, false}; }
                auto* nn = convert_to_split(node, hdr, ik, sv, bits);
                propagate(stack, depth, nn, node);
                ++size_;
                return {true, true};
            }
            if (r.node != node) propagate(stack, depth, r.node, node);
            if (r.inserted) { ++size_; return {true, true}; }
            value_traits::destroy(sv, alloc_);
            return {true, false};
        }

        // Split node: lookup top child
        uint8_t ti = static_cast<uint8_t>(ik >> (IK_BITS - 8));
        auto lk = split_ops::lookup_child(node, ti);

        if (!lk.found) {
            if constexpr (!INSERT) { value_traits::destroy(sv, alloc_); return {true, false}; }
            ik <<= 8; bits -= 8;
            auto* leaf = make_single_bot_leaf(ik, sv, bits);
            auto* nn = split_ops::add_child_as_leaf(node, ti, leaf, alloc_);
            if (nn != node) propagate(stack, depth, nn, node);
            ++size_;
            return {true, true};
        }

        bool child_is_leaf = !split_ops::is_internal(node, ti);

        if (child_is_leaf) {
            ik <<= 8; bits -= 8;
            auto r = bot_leaf_insert<INSERT, ASSIGN>(lk.child, ik, sv);
            if (r.needs_split) {
                if constexpr (!INSERT) { value_traits::destroy(sv, alloc_); return {true, false}; }
                convert_bot_leaf_to_fan(node, ti, lk.slot, lk.child, ik, sv, bits);
                ++size_;
                return {true, true};
            }
            if (r.node != lk.child)
                split_ops::set_child(node, lk.slot, r.node);
            if (r.inserted) { ++size_; return {true, true}; }
            value_traits::destroy(sv, alloc_);
            return {true, false};
        }

        // Child is fan: descend
        ik <<= 8; bits -= 8;
        stack[depth++] = {node, parent_type::SPLIT, ti, static_cast<int16_t>(lk.slot)};
        uint64_t* fan = lk.child;

        bi = static_cast<uint8_t>(ik >> (IK_BITS - 8));
        auto blk2 = fan_ops::lookup_child(fan, bi);

        if (!blk2.found) {
            if constexpr (!INSERT) { value_traits::destroy(sv, alloc_); return {true, false}; }
            ik <<= 8; bits -= 8;
            auto* leaf = make_single_leaf(ik, sv, bits);
            auto* new_fan = fan_ops::add_child(fan, bi, leaf, alloc_);
            if (new_fan != fan) split_ops::set_child(node, lk.slot, new_fan);
            ++size_;
            return {true, true};
        }

        stack[depth++] = {fan, parent_type::FAN, bi, static_cast<int16_t>(blk2.slot)};
        ik <<= 8; bits -= 8;
        node = blk2.child;
        hdr = get_header(node);
    }
}
```

### Pointer Propagation

```cpp
void propagate(descent_entry_t* stack, int depth, uint64_t* new_node, uint64_t* old_node) {
    if (new_node == old_node) return;
    if (depth == 0) {
        root_[stack[0].index] = new_node;
        return;
    }
    auto& parent = stack[depth - 1];
    switch (parent.type) {
        case parent_type::ROOT:
            root_[parent.index] = new_node; break;
        case parent_type::SPLIT:
            split_ops::set_child(parent.node, parent.slot, new_node); break;
        case parent_type::FAN:
            fan_ops::set_child(parent.node, parent.slot, new_node); break;
    }
}
```

### compact_insert dispatch

```cpp
// Same nested bit test pattern as compact_find. No u8 — only u16/u32/u64.
template<bool INSERT, bool ASSIGN>
compact_insert_result_t compact_insert(uint64_t* node, node_header* hdr,
                                        internal_key_t ik, vst value) {
    uint8_t st = hdr->suffix_type();
    if (st & 0b10) {
        if (st & 0b01)
            return compact_ops::insert<uint64_t, INSERT, ASSIGN>(
                node, hdr, static_cast<uint64_t>(ik), value, alloc_);
        else
            return compact_ops::insert<uint32_t, INSERT, ASSIGN>(
                node, hdr, static_cast<uint32_t>(ik >> (IK_BITS - 32)), value, alloc_);
    }
    return compact_ops::insert<uint16_t, INSERT, ASSIGN>(
        node, hdr, static_cast<uint16_t>(ik >> (IK_BITS - 16)), value, alloc_);
}
```

### bot_leaf_insert dispatch

Bot-leaf under a split is bitmap256 or compact. Bitmap256 never overflows
(max 256 entries), so `needs_split` is only possible from compact leaves.
```cpp
template<bool INSERT, bool ASSIGN>
compact_insert_result_t bot_leaf_insert(uint64_t* bot, internal_key_t ik,
                                         vst value) {
    auto* bh = get_header(bot);
    if (!bh->is_leaf()) {
        // Bitmap256 leaf — never overflows
        auto r = bitmap_leaf_ops::insert<INSERT, ASSIGN>(
            bot, static_cast<uint8_t>(ik >> (IK_BITS - 8)), value, alloc_);
        return {r.node, r.inserted, false};  // needs_split always false
    }
    // Compact leaf — can trigger needs_split at COMPACT_MAX
    return compact_insert<INSERT, ASSIGN>(bot, bh, ik, value);
}
```

## 9. Loop-Based Erase

Same descent structure as insert.

```cpp
bool erase(const KEY& key) {
    internal_key_t ik = key_ops::to_internal(key);
    uint8_t ri = static_cast<uint8_t>(ik >> (IK_BITS - 8));
    ik <<= 8;
    int bits = KEY_BITS - 8;

    uint64_t* node = root_[ri];
    if (node == SENTINEL_NODE) return false;

    descent_entry_t stack[MAX_DEPTH];
    int depth = 0;
    node_header* hdr = get_header(node);

    // Root compact leaf
    if (hdr->is_leaf()) {
        auto r = compact_erase(node, hdr, ik);
        if (!r.erased) return false;
        root_[ri] = r.node ? r.node : SENTINEL_NODE;
        --size_;
        return true;
    }

    // Root is fan: descend
    uint8_t bi = static_cast<uint8_t>(ik >> (IK_BITS - 8));
    auto blk = fan_ops::lookup_child(node, bi);
    if (!blk.found) return false;
    stack[depth++] = {node, parent_type::FAN, bi, static_cast<int16_t>(blk.slot)};
    ik <<= 8; bits -= 8;
    node = blk.child;
    hdr = get_header(node);

    // Main descent loop
    while (true) {
        int skip = hdr->skip();
        if (skip > 0) {
            prefix_t actual = hdr->prefix();
            for (int i = 0; i < skip; ++i) {
                uint16_t expected = static_cast<uint16_t>(ik >> (IK_BITS - 16));
                if (expected != actual[i]) return false;
                ik <<= 16; bits -= 16;
            }
        }

        if (hdr->is_leaf()) {
            auto r = compact_erase(node, hdr, ik);
            if (!r.erased) return false;
            if (r.node) {
                if (r.node != node) propagate(stack, depth, r.node, node);
            } else {
                remove_from_parent(stack, depth);
            }
            --size_;
            return true;
        }

        // Split node
        uint8_t ti = static_cast<uint8_t>(ik >> (IK_BITS - 8));
        auto lk = split_ops::lookup_child(node, ti);
        if (!lk.found) return false;

        bool child_is_leaf = !split_ops::is_internal(node, ti);

        if (child_is_leaf) {
            ik <<= 8; bits -= 8;
            auto r = bot_leaf_erase(lk.child, ik);
            if (!r.erased) return false;
            if (r.node) {
                if (r.node != lk.child)
                    split_ops::set_child(node, lk.slot, r.node);
            } else {
                auto* nn = split_ops::remove_child(node, lk.slot, ti, alloc_);
                if (!nn) {
                    remove_from_parent(stack, depth);
                } else if (nn != node) {
                    propagate(stack, depth, nn, node);
                }
            }
            --size_;
            return true;
        }

        // Fan child: descend
        ik <<= 8; bits -= 8;
        stack[depth++] = {node, parent_type::SPLIT, ti, static_cast<int16_t>(lk.slot)};
        uint64_t* fan = lk.child;

        bi = static_cast<uint8_t>(ik >> (IK_BITS - 8));
        auto blk2 = fan_ops::lookup_child(fan, bi);
        if (!blk2.found) return false;

        stack[depth++] = {fan, parent_type::FAN, bi, static_cast<int16_t>(blk2.slot)};
        ik <<= 8; bits -= 8;
        node = blk2.child;
        hdr = get_header(node);
    }
}
```

### compact_erase dispatch

```cpp
// Same nested bit test pattern. No u8.
erase_result_t compact_erase(uint64_t* node, node_header* hdr, internal_key_t ik) {
    uint8_t st = hdr->suffix_type();
    if (st & 0b10) {
        if (st & 0b01)
            return compact_ops::erase<uint64_t>(node, hdr, static_cast<uint64_t>(ik), alloc_);
        else
            return compact_ops::erase<uint32_t>(node, hdr,
                       static_cast<uint32_t>(ik >> (IK_BITS - 32)), alloc_);
    }
    return compact_ops::erase<uint16_t>(node, hdr,
               static_cast<uint16_t>(ik >> (IK_BITS - 16)), alloc_);
}
```

### bot_leaf_erase dispatch

```cpp
erase_result_t bot_leaf_erase(uint64_t* bot, internal_key_t ik) {
    auto* bh = get_header(bot);
    if (!bh->is_leaf()) {
        // Bitmap256 leaf
        return bitmap_leaf_ops::erase(bot, static_cast<uint8_t>(ik >> (IK_BITS - 8)), alloc_);
    }
    return compact_erase(bot, bh, ik);
}
```

### remove_from_parent

```cpp
void remove_from_parent(descent_entry_t* stack, int depth) {
    while (depth > 0) {
        auto& entry = stack[depth - 1];
        if (entry.type == parent_type::ROOT) {
            root_[entry.index] = SENTINEL_NODE;
            return;
        }

        uint64_t* parent = entry.node;
        int cc;
        uint64_t* nn;

        if (entry.type == parent_type::SPLIT) {
            cc = split_ops::child_count(parent);
            if (cc > 1) {
                nn = split_ops::remove_child(parent, entry.slot, entry.index, alloc_);
                if (nn != parent) propagate(stack, depth - 1, nn, parent);
                return;
            }
            split_ops::dealloc(parent, alloc_);
        } else {
            cc = fan_ops::child_count(parent);
            if (cc > 1) {
                nn = fan_ops::remove_child(parent, entry.slot, entry.index, alloc_);
                if (nn != parent) propagate(stack, depth - 1, nn, parent);
                return;
            }
            fan_ops::dealloc(parent, alloc_);
        }
        depth--;
    }
    root_[stack[0].index] = SENTINEL_NODE;
}
```

## 10. split_ops

### Layout
```
[header (1 u64)]              offset 0
[main_bitmap (4 u64)]         offset 1
[is_internal_bitmap (4 u64)]  offset 5
[sentinel (1 u64)]            offset 9
[children[]]                  offset 10
```

Size: `10 + n_children` u64s.

### API
```cpp
template<typename KEY, typename VALUE, typename ALLOC>
struct split_ops {
    struct child_lookup_t { uint64_t* child; int slot; bool found; };
    static child_lookup_t lookup_child(const uint64_t* node, uint8_t index) noexcept;

    struct top_child_result_t { const uint64_t* child; bool is_leaf; };
    static top_child_result_t branchless_top_child(const uint64_t* node, uint8_t ti) noexcept;

    static bool is_internal(const uint64_t* node, uint8_t index) noexcept;
    static int child_count(const uint64_t* node) noexcept;

    static void set_child(uint64_t* node, int slot, uint64_t* child) noexcept;
    static uint64_t* add_child_as_leaf(uint64_t* node, uint8_t index, uint64_t* child, ALLOC& alloc);
    static uint64_t* add_child_as_internal(uint64_t* node, uint8_t index, uint64_t* child, ALLOC& alloc);
    static uint64_t* remove_child(uint64_t* node, int slot, uint8_t index, ALLOC& alloc);
    static void mark_internal(uint64_t* node, uint8_t index) noexcept;

    static uint64_t* make_split(const uint8_t* indices, uint64_t* const* children,
                                 const bool* is_internal_flags, int n_children,
                                 uint8_t skip, prefix_t prefix, ALLOC& alloc);

    template<typename Fn>
    static void for_each_child(const uint64_t* node, Fn&& cb);
    // cb(uint8_t index, int slot, uint64_t* child, bool is_leaf)

    static void dealloc(uint64_t* node, ALLOC& alloc) noexcept;
};
```

## 11. fan_ops

### Layout
```
[header (1 u64)]              offset 0
[main_bitmap (4 u64)]         offset 1
[sentinel (1 u64)]            offset 5
[children[]]                  offset 6
```

Size: `6 + n_children` u64s.

### API
```cpp
template<typename KEY, typename VALUE, typename ALLOC>
struct fan_ops {
    struct child_lookup_t { uint64_t* child; int slot; bool found; };
    static child_lookup_t lookup_child(const uint64_t* node, uint8_t index) noexcept;

    static const uint64_t* branchless_child(const uint64_t* node, uint8_t bi) noexcept;

    static int child_count(const uint64_t* node) noexcept;

    static void set_child(uint64_t* node, int slot, uint64_t* child) noexcept;
    static uint64_t* add_child(uint64_t* node, uint8_t index, uint64_t* child, ALLOC& alloc);
    static uint64_t* remove_child(uint64_t* node, int slot, uint8_t index, ALLOC& alloc);

    static uint64_t* make_fan(const uint8_t* indices, uint64_t* const* children,
                               int n_children, ALLOC& alloc);

    template<typename Fn>
    static void for_each_child(const uint64_t* node, Fn&& cb);
    // cb(uint8_t index, int slot, uint64_t* child)

    static void dealloc(uint64_t* node, ALLOC& alloc) noexcept;
};
```

## 12. bitmap_leaf_ops

### Layout
```
[header (1 u64)]              offset 0
[bitmap256 (4 u64)]           offset 1
[VST[]]                       offset 5
```

Header has `is_bitmask=1` set (so `is_leaf()` returns false). This distinguishes bitmap256 from compact leaves when checking a child's own header.

### API
```cpp
template<typename KEY, typename VALUE, typename ALLOC>
struct bitmap_leaf_ops {
    using vst = typename value_traits<VALUE, ALLOC>::slot_type;

    static const VALUE* find(const uint64_t* node, uint8_t suffix) noexcept;

    // No overflow — bitmap256 has max 256 entries, always fits.
    struct insert_result_t { uint64_t* node; bool inserted; };
    template<bool INSERT, bool ASSIGN>
    static insert_result_t insert(uint64_t* node, uint8_t suffix, vst value, ALLOC& alloc);

    static erase_result_t erase(uint64_t* node, uint8_t suffix, ALLOC& alloc);

    static uint64_t* make_single(uint8_t suffix, vst value, ALLOC& alloc);
    static uint64_t* make_from_sorted(const uint8_t* suffixes, const vst* values,
                                       uint32_t count, ALLOC& alloc);

    template<typename Fn>
    static void for_each(const uint64_t* node, Fn&& cb);
    // cb(uint8_t suffix, vst value)

    static void destroy_and_dealloc(uint64_t* node, ALLOC& alloc);
    static uint32_t count(const uint64_t* node) noexcept;
};
```

## 13. compact_ops

### Template on K instead of BITS

```cpp
template<typename KEY, typename VALUE, typename ALLOC>
struct compact_ops {
    using vt = value_traits<VALUE, ALLOC>;
    using vst = typename vt::slot_type;

    template<typename K>
    static const VALUE* find(const uint64_t* node, node_header h, K suffix) noexcept;

    struct compact_insert_result_t { uint64_t* node; bool inserted; bool needs_split; };

    template<typename K, bool INSERT, bool ASSIGN>
    static compact_insert_result_t insert(uint64_t* node, node_header* h,
                                           K suffix, vst value, ALLOC& alloc);

    template<typename K>
    static erase_result_t erase(uint64_t* node, node_header* h, K suffix, ALLOC& alloc);

    template<typename K>
    static uint64_t* make_leaf(const K* sorted_keys, const vst* values,
                                uint32_t count, uint8_t skip, prefix_t prefix,
                                uint8_t suffix_type, ALLOC& alloc);
    // Sets suffix_type in header

    template<typename K, typename Fn>
    static void for_each(const uint64_t* node, const node_header* h, Fn&& cb);

    template<typename K>
    static void destroy_and_dealloc(uint64_t* node, ALLOC& alloc);

    template<typename K> static K* keys(uint64_t* node) noexcept;
    template<typename K> static vst* vals(uint64_t* node, size_t total) noexcept;
    template<typename K> static uint16_t total_slots(uint16_t alloc_u64) noexcept;
    template<typename K> static size_t size_u64(size_t count) noexcept;
};
```

### slot_table<K, VST>

```cpp
template<typename K, typename VST>
struct slot_table {
    static constexpr size_t MAX_ALLOC = 10240;
    static constexpr auto build() { /* same logic, sizeof(K) */ }
    static constexpr auto table_ = build();
    static constexpr uint16_t max_slots(size_t alloc_u64) noexcept {
        return table_[alloc_u64];
    }
};
```

Remove `suffix_traits` entirely.

## 14. Build / Conversion Functions

Called when compact leaf hits COMPACT_MAX. Take `bits` as runtime parameter.

### build_node_from_arrays(uint64_t* suf, vst* vals, size_t count, int bits)

```
if count <= COMPACT_MAX:
    dispatch_suffix(bits) → sort as K, create compact leaf with suffix_type_for(bits)
    return leaf

if bits > 16:
    check skip compression (all share top 16 bits)
    if compressible:
        strip top 16, recurse with bits-16, set skip/prefix on result
        return child

return build_split_from_arrays(suf, vals, count, bits)
```

### build_split_from_arrays(suf, vals, count, bits)

Group by top 8 bits. For each group:
- If count ≤ BOT_LEAF_MAX:
  - `bits - 8 <= 8` → bitmap256 leaf (is_internal_bitmap bit = 0, child is_leaf()=false)
  - else → compact leaf at bits-8 (is_internal_bitmap bit = 0, child is_leaf()=true)
- If count > BOT_LEAF_MAX:
  - Build fan node (is_internal_bitmap bit = 1)

Create split node.

### convert_to_split

Collect entries from compact leaf + new entry → `build_node_from_arrays`.

### convert_bot_leaf_to_fan

Collect entries from bot-leaf + new entry → group by 8 bits → children → `fan_ops::make_fan`. Update parent split's `is_internal_bitmap` to mark this child as internal.

### split_on_prefix

Create new split at divergence. Old node and new single-entry leaf as children.

## 15. Remove-All

Walk with context:

```cpp
void remove_all() noexcept {
    for (int i = 0; i < 256; ++i) {
        uint64_t* child = root_[i];
        if (child == SENTINEL_NODE) continue;
        auto* h = get_header(child);
        if (h->is_leaf()) {
            // Compact leaf at root
            destroy_compact(child);
        } else {
            // Fan at root
            remove_fan_children(child);
            fan_ops::dealloc(child, alloc_);
        }
        root_[i] = SENTINEL_NODE;
    }
    size_ = 0;
}

void remove_fan_children(uint64_t* fan) noexcept {
    fan_ops::for_each_child(fan, [&](uint8_t, int, uint64_t* child) {
        auto* h = get_header(child);
        if (h->is_leaf()) {
            // Compact leaf
            destroy_compact(child);
        } else {
            // Split node (fan children are compact or split)
            remove_split_children(child);
            split_ops::dealloc(child, alloc_);
        }
    });
}

void remove_split_children(uint64_t* split) noexcept {
    split_ops::for_each_child(split, [&](uint8_t, int, uint64_t* child, bool is_leaf) {
        if (is_leaf) {
            auto* h = get_header(child);
            if (h->is_leaf()) {
                // Compact leaf
                destroy_compact(child);
            } else {
                // Bitmap256 leaf
                bitmap_leaf_ops::destroy_and_dealloc(child, alloc_);
            }
        } else {
            // Fan node
            remove_fan_children(child);
            fan_ops::dealloc(child, alloc_);
        }
    });
}

void destroy_compact(uint64_t* node) noexcept {
    auto* h = get_header(node);
    uint8_t st = h->suffix_type();
    if (st & 0b10) {
        if (st & 0b01) compact_ops::destroy_and_dealloc<uint64_t>(node, alloc_);
        else           compact_ops::destroy_and_dealloc<uint32_t>(node, alloc_);
    } else {
        compact_ops::destroy_and_dealloc<uint16_t>(node, alloc_);
    }
}
```

## 16. Stats Collection

Same context-based walk:

```cpp
struct debug_stats_t {
    size_t compact_leaves = 0;
    size_t bitmap_leaves = 0;
    size_t split_nodes = 0;
    size_t fan_nodes = 0;
    size_t total_entries = 0;
    size_t total_bytes = 0;
};
```

Follow same root → fan → split → (compact/bitmap256/fan) pattern.

## 17. File Organization

### kntrie_support.hpp
- Namespace: `gteitelbaum`
- `key_ops<KEY>` with `internal_key_t`, `to_internal`, `to_key`
- `node_header` (8 bytes, unchanged size)
- Constants: `HEADER_U64`, `BITMAP256_U64`, `COMPACT_MAX`, `BOT_LEAF_MAX`, `SENTINEL_NODE`
- `slot_table<K, VST>`
- `value_traits<VALUE, ALLOC>`
- `alloc_node`, `dealloc_node`
- `round_up_u64`, `step_up_u64`, `should_shrink_u64`
- `suffix_type_for()`, `dispatch_suffix()`
- Result types: `erase_result_t`, `prefix_t`

### kntrie_compact.hpp
- `jump_search<K>`
- `compact_ops<KEY, VALUE, ALLOC>` templated on `<typename K>`

### kntrie_bitmask.hpp
- `bitmap256`
- `split_ops<KEY, VALUE, ALLOC>`
- `fan_ops<KEY, VALUE, ALLOC>`
- `bitmap_leaf_ops<KEY, VALUE, ALLOC>`

### kntrie.hpp
- `gteitelbaum::kntrie<KEY, VALUE, ALLOC>` — single class
- Loop-based find/insert/erase
- Build/conversion functions
- Remove-all, stats
- Iterator stubs

No `kntrie_impl.hpp`. Four headers total.

## 18. Key Depth Bounds

| Key type | KEY_BITS | IK_BITS | Max split depth | Max fan depth | Max stack |
|----------|----------|---------|-----------------|---------------|-----------|
| uint16_t | 16       | 32      | 0               | 1 (root)      | 2         |
| int32_t  | 32       | 32      | 1               | 2             | 4         |
| uint64_t | 64       | 64      | 3               | 4             | 8         |

Fixed stack: 10 entries.

## 19. Implementation Order

1. Header changes (suffix_type bits 14-13, entries 13-bit unchanged)
2. key_ops refactor (internal_key_t, left-aligned)
3. slot_table + compact_ops (re-template on K, make_leaf sets suffix_type)
4. bitmap256 + bitmap_leaf_ops
5. split_ops
6. fan_ops
7. Loop-based find
8. Loop-based insert with descent stack
9. Loop-based erase with descent stack
10. Build/conversion functions (runtime bits + dispatch_suffix)
11. Remove-all + stats
12. Cleanup

## 20. Risk Areas

1. **Suffix extraction with shift:** `static_cast<K>(ik >> (IK_BITS - sizeof(K)*8))`. Verify all K/IK combinations.
2. **Sentinel path:** Zeroed header → `is_leaf()=true`, suffix_type=0, entries=0 → compact_find dispatches to u16 (fallthrough), 0 entries → search returns nullptr. ✓
3. **Bitmap256 identification:** Parent's `is_internal_bitmap=0` + child's `is_leaf()=false` = bitmap256. Parent's `is_internal_bitmap=1` + child's `is_leaf()=false` = fan. Verify no confusion.
4. **Split vs fan layout:** Different children offsets (10 vs 6). Must use correct ops everywhere.
5. **Stack propagation:** All realloc paths propagate through descent stack.
6. **Skip + shift:** Each prefix chunk: check, then `ik <<= 16`. Verify alignment.
7. **Bitmap256 header:** Must have `is_bitmask=1` set. `set_bitmask()` in `bitmap_leaf_ops::make_*`.
