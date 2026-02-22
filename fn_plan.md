# fn_plan.md — u64 Key Relaxation + Function Pointer Dispatch

## Two Changes, One Plan

**Change 1: u64-everywhere keys.** All tree routing uses `uint64_t ik` (left-aligned).
NK narrowing eliminated. NK only matters at leaf storage boundary.

**Change 2: Function pointer dispatch.** Leaves store fn pointer + prefix.
Root stores fn pointer. Sentinel nodes for branchless empty/miss paths.

Change 1 first (simplifies everything). Change 2 builds on it.

---

## Change 1: u64-Everywhere Keys

### The Rule

Every function in the tree takes `uint64_t ik` — the full internal key,
left-aligned (byte 0 at bits 63..56). Always.

- Extract top byte: `ik >> 56`
- Shift down: `ik << 8`
- Same at every level. No narrowing boundaries. No `BITS - 8 == NK_BITS / 2`.

BITS is still the compile-time remaining key bits. It controls:
- When to stop descending (BITS == 8 → bottom bitmap leaf)
- What storage NK to use at leaf entry points
- Template recursion depth

### Where NK Still Matters

**Leaf storage only.** When entering a compact leaf, narrow to storage NK:

```cpp
// At leaf entry:
using SNK = nk_for_bits_t<REMAINING>;  // u8/u16/u32/u64
SNK suffix = static_cast<SNK>(ik >> (64 - sizeof(SNK) * 8));
// compact_ops<SNK, VALUE, ALLOC>::find(node, hdr, suffix, HS)
```

compact_ops<NK> stays templatized for memory density — u16 keys in sorted
arrays take half the space of u64. But the routing never sees NK.

### What Dies

| Thing | Count | Replacement |
|-------|-------|-------------|
| `kntrie_ops<NK, VALUE, ALLOC, IK, IK_OFF_V>` | 1 class | `kntrie_ops<VALUE, ALLOC>` |
| `kntrie_iter_ops<NK, VALUE, ALLOC, IK, IK_OFF_V>` | 1 class | `kntrie_iter_ops<VALUE, ALLOC>` |
| `next_narrow_t<NK>` | 1 alias | dead |
| `NNK`, `NARROW`, `NNNK0`, `NNNNK0` | 4+ aliases per class | dead |
| `if constexpr (BITS - 8 == NK_BITS/2 && NK_BITS > 8)` | ~40 occurrences | dead |
| `NARROW::template foo<BITS-8>(...)` calls | ~40 occurrences | `foo<BITS-8>(...)` |
| `root_dispatch<BITS>::narrow(nk)` | used in every root fn | dead — pass ik directly |
| `root_dispatch<BITS>::NK_TYPE` | complex conditional | dead |
| `IK_OFF`, `IK_OFF_V` template param | threaded everywhere | dead — ik is always u64 |
| `suffix_to_ik_ct` / `derive_prefix` | complex shift math | dead — `prefix_u64 \| suffix_to_u64` |

### kntrie_ops<VALUE, ALLOC> — new shape

```cpp
template<typename VALUE, typename ALLOC>
struct kntrie_ops {
    using BO  = bitmask_ops<VALUE, ALLOC>;
    using VT  = value_traits<VALUE, ALLOC>;
    using VST = typename VT::slot_type;
    using BLD = builder<VALUE, VT::IS_TRIVIAL, ALLOC>;

    using leaf_fn_t     = typename BO::leaf_fn_t;
    using leaf_result_t = typename BO::leaf_result_t;

    // All functions take uint64_t ik (left-aligned).
    // BITS is compile-time remaining key bits.

    template<int BITS> requires (BITS > 8)
    static const VALUE* find_node(uint64_t ptr, uint64_t ik) noexcept;

    template<int BITS, bool INSERT, bool ASSIGN> requires (BITS >= 8)
    static insert_result_t insert_node(uint64_t ptr, uint64_t ik, VST value, BLD& bld);

    template<int BITS> requires (BITS >= 8)
    static erase_result_t erase_node(uint64_t ptr, uint64_t ik, BLD& bld);

    // Leaf creation — narrow to storage NK at leaf boundary
    template<int BITS>
    static uint64_t* make_single_leaf(uint64_t ik, VST value, BLD& bld);
    // ...
};
```

### kntrie_iter_ops<VALUE, ALLOC> — new shape

```cpp
template<typename VALUE, typename ALLOC>
struct kntrie_iter_ops {
    using BO  = bitmask_ops<VALUE, ALLOC>;
    // ...

    // Only keep: remove_subtree, collect_stats, destroy_leaf
    // Iteration moved to fn-pointer dispatch (change 2)

    template<int BITS> requires (BITS >= 8)
    static void remove_subtree(uint64_t tagged, BLD& bld) noexcept;

    template<int BITS> requires (BITS >= 8)
    static void collect_stats(uint64_t tagged, stats_t& s) noexcept;
};
```

### find_node — no narrowing

```cpp
template<int BITS> requires (BITS > 8)
static const VALUE* find_node(uint64_t ptr, uint64_t ik) noexcept {
    if (ptr & LEAF_BIT) [[unlikely]] {
        const uint64_t* node = untag_leaf(ptr);
        return BO::leaf_fn(node)->find(node, ik);
    }

    const uint64_t* bm = reinterpret_cast<const uint64_t*>(ptr);
    uint8_t ti = static_cast<uint8_t>(ik >> 56);
    int slot = reinterpret_cast<const bitmap_256_t*>(bm)->
                   find_slot<slot_mode::BRANCHLESS>(ti);
    uint64_t child = bm[BITMAP_256_U64 + slot];

    return find_node<BITS - 8>(child, ik << 8);
}

template<int BITS> requires (BITS == 8)
static const VALUE* find_node(uint64_t ptr, uint64_t ik) noexcept {
    const uint64_t* node = untag_leaf(ptr);
    return BO::bitmap_find(node, *get_header(node),
                            static_cast<uint8_t>(ik >> 56), LEAF_HEADER_U64);
}
```

No NARROW. No NK_BITS. No conditional narrowing. Just `ik >> 56` and `ik << 8`.

### insert_node — no narrowing

```cpp
template<int BITS, bool INSERT, bool ASSIGN> requires (BITS >= 8)
static insert_result_t insert_node(uint64_t ptr, uint64_t ik, VST value, BLD& bld) {
    if (ptr == BO::SENTINEL_TAGGED) [[unlikely]] {
        if constexpr (!INSERT) return {ptr, false, false};
        return {tag_leaf(make_single_leaf<BITS>(ik, value, bld)), true, false};
    }

    if (ptr & LEAF_BIT) [[unlikely]] {
        uint64_t* node = untag_leaf_mut(ptr);
        auto* hdr = get_header(node);
        uint8_t skip = BO::leaf_fn(node)->skip;
        if (skip) [[unlikely]]
            return insert_leaf_skip<BITS, INSERT, ASSIGN>(node, hdr, ik, value, skip, 0, bld);
        return leaf_insert<BITS, INSERT, ASSIGN>(node, hdr, ik, value, bld);
    }

    // BITMASK
    uint64_t* node = bm_to_node(ptr);
    auto* hdr = get_header(node);
    uint8_t sc = hdr->skip();
    if (sc > 0) [[unlikely]]
        return insert_chain_skip<BITS, INSERT, ASSIGN>(node, hdr, sc, ik, value, 0, bld);
    return insert_final_bitmask<BITS, INSERT, ASSIGN>(node, hdr, 0, ik, value, bld);
}
```

### insert_leaf_skip — no narrowing

```cpp
template<int BITS, bool INSERT, bool ASSIGN> requires (BITS >= 8)
static insert_result_t insert_leaf_skip(
        uint64_t* node, node_header_t* hdr,
        uint64_t ik, VST value,
        uint8_t skip, uint8_t pos, BLD& bld) {
    if (pos >= skip) [[unlikely]]
        return leaf_insert<BITS, INSERT, ASSIGN>(node, hdr, ik, value, bld);

    uint8_t expected = static_cast<uint8_t>(ik >> 56);
    uint8_t actual = pfx_byte(leaf_prefix(node), pos);
    if (expected != actual) [[unlikely]] {
        if constexpr (!INSERT) return {tag_leaf(node), false, false};
        return {split_on_prefix<BITS>(node, hdr, ik, value, skip, pos, bld), true, false};
    }

    if constexpr (BITS > 8)
        return insert_leaf_skip<BITS - 8, INSERT, ASSIGN>(
            node, hdr, ik << 8, value, skip, pos + 1, bld);
    __builtin_unreachable();
}
```

No NARROW. No `if constexpr (BITS - 8 == NK_BITS / 2)`. Just recurse with `ik << 8`.

### make_single_leaf — narrow at storage boundary

```cpp
template<int BITS>
static uint64_t* make_single_leaf(uint64_t ik, VST value, BLD& bld) {
    if constexpr (BITS <= 8) {
        return BO::make_single_bitmap(static_cast<uint8_t>(ik >> 56), value, bld);
    } else {
        using SNK = nk_for_bits_t<BITS>;
        SNK suffix = static_cast<SNK>(ik >> (64 - sizeof(SNK) * 8));
        using CO = compact_ops<SNK, VALUE, ALLOC>;
        return CO::make_leaf(&suffix, &value, 1, bld);
    }
}
```

NK appears only here and in leaf_insert/leaf_erase — the storage boundary.

### leaf_insert — narrow at storage boundary

```cpp
template<int BITS, bool INSERT, bool ASSIGN>
static insert_result_t leaf_insert(uint64_t* node, node_header_t* hdr,
                                     uint64_t ik, VST value, BLD& bld) {
    if constexpr (BITS <= 8) {
        return BO::template bitmap_insert<INSERT, ASSIGN>(
            node, static_cast<uint8_t>(ik >> 56), value, bld);
    } else {
        using SNK = nk_for_bits_t<BITS>;
        SNK suffix = static_cast<SNK>(ik >> (64 - sizeof(SNK) * 8));
        using CO = compact_ops<SNK, VALUE, ALLOC>;
        auto result = CO::template insert<INSERT, ASSIGN>(node, hdr, suffix, value, bld);
        if (result.needs_split) [[unlikely]] {
            if constexpr (!INSERT) return {tag_leaf(node), false, false};
            return {convert_to_bitmask_tagged<BITS>(node, hdr, ik, value, bld), true, false};
        }
        return result;
    }
}
```

### prefix_u64 — trivially set at insert time

Insert sets `node[2]` to the full u64 IK with the suffix bits zeroed:

```cpp
// prefix_u64 = ik_at_root & (~0ULL << remaining_bits_at_leaf)
// leaf_fn functions reconstruct: leaf_prefix(node) | suffix_to_u64(suffix)
```

---

## Change 2: Function Pointer Dispatch

### Where everything lives

**leaf_fn_t, leaf_result_t, SENTINEL_FN, SENTINEL_NODE, SENTINEL_TAGGED**
all live in `bitmask_ops<VALUE, ALLOC>`.

Why: bitmask_ops is already templatized on `<VALUE, ALLOC>`. It already uses
SENTINEL_TAGGED in 6 places (make_bitmask, make_skip_chain, add_child,
remove_child, fix_embeds). leaf_fn_t function pointer signatures depend on
VALUE/ALLOC (return types use `const VALUE*` and `const VST*`) but NOT on NK.
One sentinel per `<VALUE, ALLOC>` instantiation, shared across all NK levels.

kntrie_ops uses `BO::leaf_fn_t`, `BO::leaf_result_t`, `BO::SENTINEL_TAGGED`.
kntrie_impl uses `BO::SENTINEL_TAGGED` for root_ptr/begin_v/end_v init.

**root_fn_t, SENTINEL_ROOT_FN, ROOT_FNS**
live in `kntrie_impl<KEY, VALUE, ALLOC>`.

Why: root fn signatures use `uint64_t ik` now (not NK0), but root_find_impl
is templatized on SKIP which depends on KEY_BITS.

### Sentinel structs in bitmask_ops<VALUE, ALLOC>

```cpp
// In bitmask_ops<VALUE, ALLOC>:

struct leaf_result_t {
    uint64_t     key;     // full IK, left-aligned in u64
    const VST*   value;
    bool         found;
};

struct leaf_fn_t {
    uint8_t skip;
    const VALUE* (*find)(const uint64_t*, uint64_t) noexcept;
    leaf_result_t (*next)(const uint64_t*, uint64_t) noexcept;
    leaf_result_t (*prev)(const uint64_t*, uint64_t) noexcept;
    leaf_result_t (*first)(const uint64_t*) noexcept;
    leaf_result_t (*last)(const uint64_t*) noexcept;
};

// leaf_fn accessor
static const leaf_fn_t* leaf_fn(const uint64_t* node) noexcept {
    return reinterpret_cast<const leaf_fn_t*>(node[1]);
}
static void set_leaf_fn(uint64_t* node, const leaf_fn_t* fn) noexcept {
    node[1] = reinterpret_cast<uint64_t>(fn);
}

static const VALUE* sentinel_find(const uint64_t*, uint64_t) noexcept {
    return nullptr;
}
static leaf_result_t sentinel_iter(const uint64_t*, uint64_t) noexcept {
    return {0, nullptr, false};
}
static leaf_result_t sentinel_bound(const uint64_t*) noexcept {
    return {0, nullptr, false};
}

static inline const leaf_fn_t SENTINEL_FN = {
    0, &sentinel_find, &sentinel_iter, &sentinel_iter,
    &sentinel_bound, &sentinel_bound,
};

// 3 u64s: header(entries=0), fn_ptr, prefix(0)
static inline const uint64_t SENTINEL_NODE[3] = {
    0,
    reinterpret_cast<uint64_t>(&SENTINEL_FN),
    0,
};

static inline const uint64_t SENTINEL_TAGGED = tag_leaf(SENTINEL_NODE);
```

### Sentinel root fn (in kntrie_impl)

```cpp
static const VALUE* sentinel_root_find(uint64_t, uint64_t, uint64_t) noexcept {
    return nullptr;
}
static const uint64_t* sentinel_root_findleaf(uint64_t, uint64_t, uint64_t) noexcept {
    return nullptr;
}

static inline const root_fn_t SENTINEL_ROOT_FN = {
    0, &sentinel_root_find, &sentinel_root_findleaf, &sentinel_root_findleaf,
};
```

### Root fn types (all u64 now)

```cpp
using root_find_fn_t     = const VALUE* (*)(uint64_t ptr, uint64_t prefix,
                                             uint64_t ik) noexcept;
using root_findnext_fn_t = const uint64_t* (*)(uint64_t ptr, uint64_t prefix,
                                                uint64_t ik) noexcept;

struct root_fn_t {
    uint8_t             skip;
    root_find_fn_t      find;
    root_findnext_fn_t  find_next;
    root_findnext_fn_t  find_prev;
};
```

### Root function implementations

```cpp
template<int SKIP>
static const VALUE* root_find_impl(uint64_t ptr, uint64_t prefix,
                                    uint64_t ik) noexcept {
    if constexpr (SKIP > 0) {
        constexpr uint64_t MASK = ~uint64_t(0) << (64 - 8 * SKIP);
        if ((ik ^ prefix) & MASK) [[unlikely]] return nullptr;
    }
    constexpr int BITS = KEY_BITS - 8 * SKIP;
    return OPS::template find_node<BITS>(ptr, ik << (8 * SKIP));
}
```

No root_dispatch. No NK0. No narrow(). Just `ik << (8 * SKIP)`.

### _impl members

```cpp
const root_fn_t* root_fn;       // points into ROOT_FNS[]
uint64_t         root_ptr;      // tagged child (SENTINEL, leaf, or bitmask)
uint64_t         root_prefix;   // shared prefix bytes, left-aligned
uint64_t         begin_v;       // tagged ptr to first leaf
uint64_t         end_v;         // tagged ptr to last leaf
size_t           size_v;
BLD              bld_v;
```

~56 bytes. One cache line.

### find_value

```cpp
const VALUE* find_value(const KEY& key) const noexcept {
    uint64_t ik = key_to_u64(key);  // left-align in u64
    return root_fn->find(root_ptr, root_prefix, ik);
}
```

### key_to_u64 helper (in kntrie_impl)

```cpp
static uint64_t key_to_u64(const KEY& key) noexcept {
    IK internal = KO::to_internal(key);
    return static_cast<uint64_t>(internal) << (64 - IK_BITS);
}
```

### iter_first / iter_last

```cpp
iter_result_t iter_first() const noexcept {
    const uint64_t* leaf = untag_leaf(begin_v);
    auto r = BO::leaf_fn(leaf)->first(leaf);
    if (!r.found) return {KEY{}, VALUE{}, false};
    return to_iter_result(r);
}
```

### to_iter_result helper

```cpp
iter_result_t to_iter_result(const typename BO::leaf_result_t& r) const noexcept {
    // r.key is u64 left-aligned. Convert back to IK then KEY.
    IK internal = static_cast<IK>(r.key >> (64 - IK_BITS));
    return {KO::to_key(internal), *VT::as_ptr(*r.value), true};
}
```

### iter_next(key)

```cpp
iter_result_t iter_next(KEY key) const noexcept {
    uint64_t ik = key_to_u64(key);
    const uint64_t* leaf = root_fn->find_next(root_ptr, root_prefix, ik);
    if (!leaf) return {KEY{}, VALUE{}, false};

    auto r = BO::leaf_fn(leaf)->next(leaf, ik);
    if (r.found) return to_iter_result(r);

    // Leaf exhausted — get max key, increment, second descent
    auto last = BO::leaf_fn(leaf)->last(leaf);
    uint64_t next_ik = last.key + (1ULL << (64 - KEY_BITS));
    if (next_ik == 0) return {KEY{}, VALUE{}, false};  // wrapped
    const uint64_t* next_leaf = root_fn->find_next(root_ptr, root_prefix, next_ik);
    if (!next_leaf) return {KEY{}, VALUE{}, false};
    auto r2 = BO::leaf_fn(next_leaf)->first(next_leaf);
    return to_iter_result(r2);
}
```

### iter_prev(key) — mirror

```cpp
iter_result_t iter_prev(KEY key) const noexcept {
    uint64_t ik = key_to_u64(key);
    const uint64_t* leaf = root_fn->find_prev(root_ptr, root_prefix, ik);
    if (!leaf) return {KEY{}, VALUE{}, false};

    auto r = BO::leaf_fn(leaf)->prev(leaf, ik);
    if (r.found) return to_iter_result(r);

    // Leaf exhausted — get min key, decrement, second descent
    auto first = BO::leaf_fn(leaf)->first(leaf);
    uint64_t prev_ik = first.key - (1ULL << (64 - KEY_BITS));
    if (prev_ik > first.key) return {KEY{}, VALUE{}, false};  // underflow
    const uint64_t* prev_leaf = root_fn->find_prev(root_ptr, root_prefix, prev_ik);
    if (!prev_leaf) return {KEY{}, VALUE{}, false};
    auto r2 = BO::leaf_fn(prev_leaf)->last(prev_leaf);
    return to_iter_result(r2);
}
```

---

## LEAF_FNS — static array per BITS

Lives in `kntrie_ops<VALUE, ALLOC>`. Indexed by skip at runtime.

All functions take `uint64_t ik` (left-aligned, already shifted past root).
For a leaf at depth BITS with skip S, ik's top S bytes are the skip prefix.
Suffix starts at byte S. Shift ik left by 8*S to reach suffix.

```cpp
template<int BITS>
struct leaf_ops_t {
    static constexpr int MAX_LEAF_SKIP = (BITS - 8) / 8;

    // narrow to storage NK for leaf operations
    template<int REMAINING>
    static auto to_suffix(uint64_t ik) noexcept {
        using SNK = nk_for_bits_t<REMAINING>;
        constexpr int SNK_BITS = sizeof(SNK) * 8;
        return static_cast<SNK>(ik >> (64 - SNK_BITS));
    }

    // place suffix back into u64 at correct position
    template<int REMAINING, typename SUF>
    static uint64_t suffix_to_u64(SUF suf) noexcept {
        constexpr int SUF_BITS = sizeof(SUF) * 8;
        return static_cast<uint64_t>(suf) << (64 - SUF_BITS);
    }

    template<int SKIP>
    static const VALUE* leaf_find_at(const uint64_t* node, uint64_t ik) noexcept {
        if constexpr (SKIP > 0) {
            constexpr uint64_t MASK = ~uint64_t(0) << (64 - 8 * SKIP);
            if ((ik ^ leaf_prefix(node)) & MASK) [[unlikely]] return nullptr;
        }
        constexpr int REMAINING = BITS - 8 * SKIP;
        uint64_t shifted = ik << (8 * SKIP);
        auto suf = to_suffix<REMAINING>(shifted);
        if constexpr (REMAINING <= 8)
            return BO::bitmap_find(node, *get_header(node), suf, LEAF_HEADER_U64);
        else {
            using CO = compact_ops<nk_for_bits_t<REMAINING>, VALUE, ALLOC>;
            return CO::find(node, *get_header(node), suf, LEAF_HEADER_U64);
        }
    }

    template<int SKIP>
    static leaf_result_t leaf_first_at(const uint64_t* node) noexcept {
        constexpr int REMAINING = BITS - 8 * SKIP;
        if constexpr (REMAINING <= 8) {
            auto r = BO::bitmap_iter_first(node, LEAF_HEADER_U64);
            return {leaf_prefix(node) | suffix_to_u64<REMAINING>(r.suffix),
                    r.value, true};
        } else {
            using CO = compact_ops<nk_for_bits_t<REMAINING>, VALUE, ALLOC>;
            auto r = CO::iter_first(node, get_header(node));
            return {leaf_prefix(node) | suffix_to_u64<REMAINING>(r.suffix),
                    r.value, true};
        }
    }

    template<int SKIP>
    static leaf_result_t leaf_last_at(const uint64_t* node) noexcept {
        constexpr int REMAINING = BITS - 8 * SKIP;
        if constexpr (REMAINING <= 8) {
            auto r = BO::bitmap_iter_last(node, *get_header(node), LEAF_HEADER_U64);
            return {leaf_prefix(node) | suffix_to_u64<REMAINING>(r.suffix),
                    r.value, true};
        } else {
            using CO = compact_ops<nk_for_bits_t<REMAINING>, VALUE, ALLOC>;
            auto r = CO::iter_last(node, get_header(node));
            return {leaf_prefix(node) | suffix_to_u64<REMAINING>(r.suffix),
                    r.value, true};
        }
    }

    template<int SKIP>
    static leaf_result_t leaf_next_at(const uint64_t* node, uint64_t ik) noexcept {
        constexpr int REMAINING = BITS - 8 * SKIP;
        if constexpr (SKIP > 0) {
            constexpr uint64_t MASK = ~uint64_t(0) << (64 - 8 * SKIP);
            uint64_t pfx = leaf_prefix(node);
            uint64_t diff = (ik ^ pfx) & MASK;
            if (diff) [[unlikely]] {
                int shift = std::countl_zero(diff) & ~7;
                uint8_t kb = static_cast<uint8_t>(ik >> (56 - shift));
                uint8_t pb = static_cast<uint8_t>(pfx >> (56 - shift));
                if (kb < pb) return leaf_first_at<SKIP>(node);
                return {0, nullptr, false};
            }
        }
        uint64_t shifted = ik << (8 * SKIP);
        auto suf = to_suffix<REMAINING>(shifted);
        if constexpr (REMAINING <= 8) {
            auto r = BO::bitmap_iter_next(node, suf, LEAF_HEADER_U64);
            if (!r.found) [[unlikely]] return {0, nullptr, false};
            return {leaf_prefix(node) | suffix_to_u64<REMAINING>(r.suffix),
                    r.value, true};
        } else {
            using CO = compact_ops<nk_for_bits_t<REMAINING>, VALUE, ALLOC>;
            auto r = CO::iter_next(node, get_header(node), suf);
            if (!r.found) [[unlikely]] return {0, nullptr, false};
            return {leaf_prefix(node) | suffix_to_u64<REMAINING>(r.suffix),
                    r.value, true};
        }
    }

    template<int SKIP>
    static leaf_result_t leaf_prev_at(const uint64_t* node, uint64_t ik) noexcept {
        constexpr int REMAINING = BITS - 8 * SKIP;
        if constexpr (SKIP > 0) {
            constexpr uint64_t MASK = ~uint64_t(0) << (64 - 8 * SKIP);
            uint64_t pfx = leaf_prefix(node);
            uint64_t diff = (ik ^ pfx) & MASK;
            if (diff) [[unlikely]] {
                int shift = std::countl_zero(diff) & ~7;
                uint8_t kb = static_cast<uint8_t>(ik >> (56 - shift));
                uint8_t pb = static_cast<uint8_t>(pfx >> (56 - shift));
                if (kb > pb) return leaf_last_at<SKIP>(node);
                return {0, nullptr, false};
            }
        }
        uint64_t shifted = ik << (8 * SKIP);
        auto suf = to_suffix<REMAINING>(shifted);
        if constexpr (REMAINING <= 8) {
            auto r = BO::bitmap_iter_prev(node, suf, LEAF_HEADER_U64);
            if (!r.found) [[unlikely]] return {0, nullptr, false};
            return {leaf_prefix(node) | suffix_to_u64<REMAINING>(r.suffix),
                    r.value, true};
        } else {
            using CO = compact_ops<nk_for_bits_t<REMAINING>, VALUE, ALLOC>;
            auto r = CO::iter_prev(node, get_header(node), suf);
            if (!r.found) [[unlikely]] return {0, nullptr, false};
            return {leaf_prefix(node) | suffix_to_u64<REMAINING>(r.suffix),
                    r.value, true};
        }
    }

    // Build LEAF_FNS array
    template<size_t... Is>
    static constexpr auto make_leaf_fns(std::index_sequence<Is...>) {
        return std::array<typename BO::leaf_fn_t, sizeof...(Is)>{
            typename BO::leaf_fn_t{
                static_cast<uint8_t>(Is),
                &leaf_find_at<static_cast<int>(Is)>,
                &leaf_next_at<static_cast<int>(Is)>,
                &leaf_prev_at<static_cast<int>(Is)>,
                &leaf_first_at<static_cast<int>(Is)>,
                &leaf_last_at<static_cast<int>(Is)>,
            }...
        };
    }

    static constexpr auto LEAF_FNS = make_leaf_fns(
        std::make_index_sequence<MAX_LEAF_SKIP + 1>{});
};
```

---

## Leaf Node Layout

```
node[0] = header          (entries, alloc_u64, total_slots — skip dead for leaves)
node[1] = fn_ptr          (pointer to static leaf_fn_t, encodes BITS+SKIP)
node[2] = prefix_u64      (full u64 IK with suffix bits zeroed)
node[3+] = keys/values    (unchanged layout)
```

LEAF_HEADER_U64 = 3 always.

---

## prepend_skip / remove_skip

Fixed 3-u64 header: node[2] always exists. No realloc ever.

### prepend_skip(node, new_len, new_pfx, new_fn)
```cpp
static void prepend_skip(uint64_t* node, uint8_t new_len,
                          uint64_t new_pfx, const typename BO::leaf_fn_t* new_fn) noexcept {
    uint8_t old_skip = BO::leaf_fn(node)->skip;
    uint64_t combined = new_pfx;
    if (old_skip > 0)
        combined |= leaf_prefix(node) >> (8 * new_len);
    set_leaf_prefix(node, combined);
    BO::set_leaf_fn(node, new_fn);
}
```
No realloc. One prefix merge + one pointer write.

### remove_skip(node, new_fn)
```cpp
static void remove_skip(uint64_t* node, const typename BO::leaf_fn_t* new_fn) noexcept {
    set_leaf_prefix(node, 0);
    BO::set_leaf_fn(node, new_fn);
}
```
No realloc. One zero + one pointer write.

---

## find_leaf_next / find_leaf_prev (in kntrie_ops)

No sentinel checks — sentinel has LEAF_BIT set, untag returns sentinel node,
caller invokes fn which returns not_found.

```cpp
template<int BITS> requires (BITS > 8)
static const uint64_t* find_leaf_next(uint64_t ptr, uint64_t ik) noexcept {
    if (ptr & LEAF_BIT) [[unlikely]]
        return untag_leaf(ptr);  // sentinel or real leaf — caller uses fn

    const uint64_t* bm = reinterpret_cast<const uint64_t*>(ptr);
    const auto* bmp = reinterpret_cast<const bitmap_256_t*>(bm);
    uint8_t ti = static_cast<uint8_t>(ik >> 56);

    int slot = bmp->find_slot<slot_mode::FAST_EXIT>(ti);

    if (slot < 0) [[unlikely]] {
        auto adj = bmp->next_set_after(ti);
        if (!adj.found) [[unlikely]] return nullptr;
        return descend_min_leaf<BITS - 8>(bm[BITMAP_256_U64 + adj.slot]);
    }

    return find_leaf_next<BITS - 8>(bm[BITMAP_256_U64 + slot], ik << 8);
}

template<int BITS> requires (BITS >= 8)
static const uint64_t* descend_min_leaf(uint64_t ptr) noexcept {
    if (ptr & LEAF_BIT) [[unlikely]] return untag_leaf(ptr);
    const uint64_t* bm = reinterpret_cast<const uint64_t*>(ptr);
    return descend_min_leaf<BITS - 8>(bm[BITMAP_256_U64]);
}

template<int BITS> requires (BITS > 8)
static const uint64_t* find_leaf_prev(uint64_t ptr, uint64_t ik) noexcept {
    if (ptr & LEAF_BIT) [[unlikely]]
        return untag_leaf(ptr);

    const uint64_t* bm = reinterpret_cast<const uint64_t*>(ptr);
    const auto* bmp = reinterpret_cast<const bitmap_256_t*>(bm);
    uint8_t ti = static_cast<uint8_t>(ik >> 56);

    int slot = bmp->find_slot<slot_mode::FAST_EXIT>(ti);

    if (slot < 0) [[unlikely]] {
        auto adj = bmp->prev_set_before(ti);
        if (!adj.found) [[unlikely]] return nullptr;
        return descend_max_leaf<BITS - 8>(bm[BITMAP_256_U64 + adj.slot]);
    }

    return find_leaf_prev<BITS - 8>(bm[BITMAP_256_U64 + slot], ik << 8);
}

template<int BITS> requires (BITS >= 8)
static const uint64_t* descend_max_leaf(uint64_t ptr) noexcept {
    if (ptr & LEAF_BIT) [[unlikely]] return untag_leaf(ptr);
    const uint64_t* bm = reinterpret_cast<const uint64_t*>(ptr);
    int last = get_header(bm_to_node_const(
        reinterpret_cast<uint64_t>(bm)))->entries() - 1;
    return descend_max_leaf<BITS - 8>(bm[BITMAP_256_U64 + last]);
}
```

No NARROW. No NK. Just `ik >> 56` and `ik << 8`.

---

## What Gets Removed

### From kntrie_impl.hpp
- `root_v[256]` → `root_ptr` (8 bytes)
- `root_skip_v` → lives in `root_fn->skip`
- `root_skip_ops_t` struct + `ROOT_OPS` table + `make_root_table`
- All `root_*_at` templates
- `skip_switch` — completely dead
- `reduce_root_skip` → simplified
- `make_prefix`, `nk_byte`, `nk_to_u64`
- `iter_first/iter_last` scanning loops → begin_v/end_v
- `iter_next/iter_prev` → root_fn dispatch + fn + second descent
- `NK0`, `NNK0`, `NNNK0`, `NNNNK0` — all dead
- `root_dispatch` — dead (was NK type selection for depth)
- `OPS`, `ITER_OPS`, `NARROW_OPS`, `NARROW_ITER` aliases → single `OPS`/`ITER_OPS`

### From kntrie_ops.hpp
- Template param `NK` — gone. `kntrie_ops<VALUE, ALLOC>`.
- Template params `IK`, `IK_OFF_V` — gone. ik is always u64.
- `NNK`, `NARROW` aliases — gone
- All `if constexpr (BITS - 8 == NK_BITS/2 && NK_BITS > 8)` — gone (~40)
- All `NARROW::template foo<BITS-8>(...)` — just `foo<BITS-8>(...)`
- `leaf_fn_t`, `leaf_result_t` → moved to bitmask_ops
- `leaf_ops_entry_t` + old TABLE → replaced by LEAF_FNS
- Old `prepend_skip` / `remove_skip` (realloc versions) → no-realloc versions
- `suffix_to_ik_ct`, `derive_prefix`, `compute_prefix` → dead

### From kntrie_iter_ops.hpp
- Template param `NK` — gone. `kntrie_iter_ops<VALUE, ALLOC>`.
- Template params `IK`, `IK_OFF_V` — gone.
- `NNK`, `NARROW` — gone
- `descend_min/max` + chain_skip/bm_final variants — replaced by descend_min_leaf/max_leaf
- `iter_next_node/prev_node` + chain_skip/bm_final — replaced by fn dispatch
- All narrowing conditionals — gone
- Keeps: `remove_subtree`, `collect_stats`, `destroy_leaf` (simplified, no narrowing)

### From kntrie_support.hpp
- `next_narrow_t<NK>` — dead
- `prefix_u64()`, `set_prefix_u64()`, `set_prefix()`, `prefix_byte()` on header — dead for leaves
- Old SENTINEL comment → real sentinel in bitmask_ops

## What Stays
- Bitmask nodes: unchanged (header = 1 u64, no fn/prefix)
- Bitmask descent: compile-time recursive, BRANCHLESS mode, children[0] = SENTINEL_TAGGED
- bitmask_ops: core logic unchanged PLUS now owns leaf_fn_t, leaf_result_t, sentinel
- compact_ops<NK>: unchanged — storage NK for memory density
- Builder: unchanged
- Iterator API: unchanged (stores parent + key)

## begin_v / end_v maintenance

- **Empty trie**: both = BO::SENTINEL_TAGGED
- **Insert**: if new key < current min → update begin_v. If > max → update end_v.
  First insert: set both.
- **Erase**: if erased key was min → find new min via root_fn->find_next.
  If max → root_fn->find_prev. Last erase: reset both to SENTINEL_TAGGED.
- **Leaf split**: check if new leaves contain min/max.
- Write-path only.

## Memory Impact

**Root**: ~56 bytes (root_fn + root_ptr + root_prefix + begin + end + size + bld).
Was 2048 + 16. Saves ~2KB.

**Per leaf**: Always 3 u64 header. +8 to +16 bytes per leaf.
Skip chain embeds waste a u64 per embed for the single child ptr. Acceptable.

## Performance Summary

- **find**: root_fn→find, one indirect call. find_node: no narrowing branches.
- **iter_next/prev**: root_fn→find_next → leaf → fn→next. Second descent on exhaustion.
- **begin/end**: O(1) via begin_v/end_v.
- **insert/erase**: prepend_skip/remove_skip are free (no realloc).
