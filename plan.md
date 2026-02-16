# kntrie Code Restructure Plan

## Architecture

```
kntrie.hpp          KEY → UK conversion, iterator with KEY ↔ UK
    ↓ calls
kntrie_impl.hpp     root_, size_, alloc_, public API (~200 lines)
    ↓ calls
kntrie_ops.hpp      template<NK> recursive dispatch, stateless (~1400 lines)
    ↓ calls
kntrie_compact.hpp  CO<NK>: compact leaf operations
kntrie_bitmask.hpp  BM: bitmask node operations
kntrie_support.hpp  shared types, node_header, bitmap256, etc.
```

**Include order**: support ← compact ← bitmask ← **ops** ← impl ← kntrie

**Key principle**: ops is stateless. Every function is `static`, takes
`ALLOC& alloc` for mutation, no alloc for read-only (find, iter).
Templated on `<NK>`. No KEY dependency anywhere.

---

## What NK buys us

NK (narrowed key type) replaces ALL runtime suffix_type dispatch:

```cpp
// BEFORE — runtime dispatch, 4 branches
uint8_t st = hdr->suffix_type();
if (st == 0)      BO::bitmap_xxx(...)
else if (st == 1) CO16::xxx(...)
else if (st == 2) CO32::xxx(...)
else              CO64::xxx(...)

// AFTER — compile-time, zero branches
if constexpr (sizeof(NK) == 1)
    BO::bitmap_xxx(...)        // only at BITS==8 base case
else
    compact_ops<NK>::xxx(...)  // NK ∈ {u16, u32, u64}
```

**Functions where this eliminates dispatch** (20+ call sites):
- `leaf_insert_` → `CO<NK>::insert`
- `leaf_erase_` → `CO<NK>::erase`
- `make_single_leaf_` → `CO<NK>::make_leaf`
- `build_leaf_from_arrays_` → `CO<NK>::make_leaf`
- `leaf_for_each_u64_` → `CO<NK>::for_each`
- `leaf_first_/last_/next_/prev_dispatch_` → `CO<NK>::iter_xxx`
- `combine_suffix_` → compile-time shift

---

## NK narrowing mechanics

NK starts matching KEY width, narrows at byte-consumption boundaries:

```
u64 key: NK=u64 → (at bits=32) → u32 → (at bits=16) → u16 → (at bits=8) → BM
u32 key: NK=u32 → (at bits=16) → u16 → (at bits=8) → BM
u16 key: NK=u16 → (at bits=8) → BM
```

### For find (already done): template<BITS, NK>, fully unrolled

### For insert/erase/iter: template<NK> with runtime `int bits`

Narrowing check after consuming each byte in bitmask descent:
```cpp
template<typename NK>
static result_t insert_node_(uint64_t ptr, NK ik, VST val,
                              int bits, ALLOC& alloc) {
    constexpr int NK_BITS = sizeof(NK) * 8;
    // ... process node, consume a byte, bits -= 8 ...

    // Narrow if remaining bits == half NK width
    if constexpr (NK_BITS > 16) {
        if (bits == NK_BITS / 2) {
            using NNK = next_narrow_t<NK>;
            NNK nik = static_cast<NNK>(
                static_cast<NK>(ik << 8) >> (NK_BITS / 2));
            return insert_node_<NNK>(child, nik, val, bits, alloc);
        }
    }
    return insert_node_<NK>(child, static_cast<NK>(ik << 8), val, bits, alloc);
}
```

Helper types:
```cpp
template<typename T> struct next_narrow;
template<> struct next_narrow<uint64_t> { using type = uint32_t; };
template<> struct next_narrow<uint32_t> { using type = uint16_t; };
template<> struct next_narrow<uint16_t> { using type = uint8_t;  };
template<typename T> using next_narrow_t = typename next_narrow<T>::type;
```

Max 4 instantiations per function (NK ∈ {u64, u32, u16, u8}).
u32 keys only instantiate {u32, u16, u8}. Shared across key types
via find_ops pattern (already proven).

---

## Narrowing in skip chains

Skip chains consume N bytes. Narrowing might cross boundaries mid-skip.
The skip loop itself just compares bytes — it doesn't call CO/BM, so NK
doesn't matter during the walk. NK only matters when:

1. **Recursing into a child** (bitmask descent after skip)
2. **Dispatching to a leaf** (determining CO<NK> type)

After walking all skip bytes, `bits` has been decremented. NK may be stale
(too wide for current bits). Fix with a narrowing redirect:

```cpp
// After skip walk, bits may have crossed boundary
template<typename NK>
static auto narrow_for_bits_(NK ik, int bits, auto&& fn) {
    constexpr int NK_BITS = sizeof(NK) * 8;
    if constexpr (NK_BITS > 16) {
        if (bits <= NK_BITS / 2) {
            using NNK = next_narrow_t<NK>;
            NNK nik = static_cast<NNK>(ik);
            return narrow_for_bits_<NNK>(nik, bits, fn);
        }
    }
    return fn.template operator()<NK>(ik, bits);
}
```

This is max 3 comparisons, only on the skip path (rare). Non-skip common
path: NK is already correct from template recursion.

---

## Result types

Already in kntrie_support.hpp, unchanged:
```cpp
struct insert_result_t {
    uint64_t tagged_ptr;
    bool inserted;
    bool needs_split;
};
struct erase_result_t {
    uint64_t tagged_ptr;
    bool erased;
    uint16_t subtree_entries;
};
```

iter_result_t moves from kntrie_impl to kntrie_support.hpp.
Since ops doesn't know KEY, use IK (= unsigned KEY type):
```cpp
template<typename IK, typename VALUE>
struct iter_result_t {
    IK key;
    VALUE value;
    bool found;
};
```

kntrie_impl converts with `KO::to_key()`, kntrie converts with
`from_unsigned()`.

---

## kntrie_ops.hpp structure

```cpp
template<typename VALUE, typename ALLOC>
struct kntrie_ops {
    using BO  = bitmask_ops<VALUE, ALLOC>;
    using VT  = value_traits<VALUE, ALLOC>;
    using VST = typename VT::slot_type;
    template<typename NK> using CO = compact_ops<NK, VALUE, ALLOC>;

    // ── Find (move from current find_ops) ─────────────────────────
    template<int BITS, typename NK>
    static const VALUE* find_node_(uint64_t ptr, NK ik) noexcept;
    template<int BITS, typename NK>
    static const VALUE* find_leaf_(...) noexcept;

    // ── Insert ────────────────────────────────────────────────────
    template<bool INSERT, bool ASSIGN, typename NK>
    static insert_result_t insert_node_(uint64_t ptr, NK ik, VST val,
                                         int bits, ALLOC& alloc);
    template<typename NK>
    static uint64_t* make_single_leaf_(NK ik, VST val,
                                        int bits, ALLOC& alloc);
    template<typename NK>
    static uint64_t convert_to_bitmask_tagged_(uint64_t* node,
        node_header* hdr, NK ik, VST val, int bits, ALLOC& alloc);
    template<typename NK>
    static uint64_t build_node_from_arrays_tagged_(uint64_t* suf,
        VST* vals, size_t count, int bits, ALLOC& alloc);
    template<typename NK>
    static uint64_t build_bitmask_from_arrays_tagged_(uint64_t* suf,
        VST* vals, size_t count, int bits, ALLOC& alloc);
    template<typename NK>
    static uint64_t split_on_prefix_tagged_(..., ALLOC& alloc);
    template<typename NK>
    static uint64_t split_skip_at_(..., ALLOC& alloc);

    // ── Erase ─────────────────────────────────────────────────────
    template<typename NK>
    static erase_result_t erase_node_(uint64_t ptr, NK ik,
                                       int bits, ALLOC& alloc);
    template<typename NK>
    static erase_result_t do_coalesce_(..., ALLOC& alloc);
    template<typename NK>
    static void collect_entries_tagged_(uint64_t tagged,
        uint64_t prefix, int prefix_bits,
        uint64_t* keys, VST* vals, size_t& wi);
    static void dealloc_bitmask_subtree_(uint64_t tagged, ALLOC& alloc);
    template<typename NK>
    static uint64_t* build_leaf_from_arrays_(uint64_t* suf, VST* vals,
        size_t count, int bits, ALLOC& alloc);

    // ── Iteration (read-only, no alloc) ───────────────────────────
    template<typename IK, typename NK>
    static iter_result_t<IK,VALUE> iter_next_node_(uint64_t ptr,
        NK ik, IK prefix, int bits);
    template<typename IK, typename NK>
    static iter_result_t<IK,VALUE> iter_prev_node_(...);
    template<typename IK, typename NK>
    static iter_result_t<IK,VALUE> descend_min_(uint64_t ptr,
        IK prefix, int bits);
    template<typename IK, typename NK>
    static iter_result_t<IK,VALUE> descend_max_(...);

    template<typename IK, typename NK>
    static IK combine_suffix_(IK prefix, int bits, NK suffix) {
        constexpr int IK_BITS = sizeof(IK) * 8;
        constexpr int NK_BITS = sizeof(NK) * 8;
        IK suffix_ik = IK(suffix) << (IK_BITS - NK_BITS);
        return prefix | (suffix_ik >> bits);
    }

    // ── Descriptor tracking (no NK, no alloc) ────────────────────
    static uint16_t tagged_count_(uint64_t tagged) noexcept;
    static uint16_t sum_children_desc_(const uint64_t* node, uint8_t sc);
    static void     set_desc_capped_(uint64_t* node, size_t count);
    static void     inc_descendants_(node_header* h);
    static uint16_t dec_or_recompute_desc_(uint64_t* node, uint8_t sc);
    static uint16_t sum_tagged_array_(const uint64_t* children, unsigned nc);

    // ── Skip/node helpers ─────────────────────────────────────────
    static uint64_t* prepend_skip_(..., ALLOC& alloc);
    static uint64_t* remove_skip_(..., ALLOC& alloc);
    // NOTE: wrap_bitmask_chain_ moved to BM as wrap_in_chain
    // NOTE: build_remainder_tagged_ moved to BM as build_remainder
    // NOTE: add_child_to_chain_ moved to BM as chain_add_child

    // ── Destroy / Stats ───────────────────────────────────────────
    static void remove_node_(uint64_t tagged, ALLOC& alloc);
    static void destroy_leaf_(uint64_t* node, node_header* hdr, ALLOC& alloc);
    template<typename NK>
    static void collect_stats_(uint64_t tagged, debug_stats_t& s);
};
```

---

## kntrie_impl.hpp after refactor (~200 lines)

```cpp
template<typename KEY, typename VALUE, typename ALLOC>
class kntrie_impl {
    // KEY is always unsigned (kntrie handles signed→unsigned)
    using KO  = key_ops<KEY>;
    using IK  = typename KO::IK;
    using Ops = kntrie_ops<VALUE, ALLOC>;
    using VT  = value_traits<VALUE, ALLOC>;
    using VST = typename VT::slot_type;

    static constexpr int KEY_BITS = KO::KEY_BITS;
    static constexpr int IK_BITS  = KO::IK_BITS;

    using NK0 = std::conditional_t<KEY_BITS <= 8,  uint8_t,
                std::conditional_t<KEY_BITS <= 16, uint16_t,
                std::conditional_t<KEY_BITS <= 32, uint32_t, uint64_t>>>;

    uint64_t root_;
    size_t   size_;
    [[no_unique_address]] ALLOC alloc_;

public:
    kntrie_impl() : root_(SENTINEL_TAGGED), size_(0), alloc_() {}
    ~kntrie_impl() {
        if (root_ != SENTINEL_TAGGED)
            Ops::remove_node_(root_, alloc_);
    }

    // ── Find ──
    const VALUE* find_value(const KEY& key) const noexcept {
        IK ik = KO::to_internal(key);
        return Ops::template find_node_<KEY_BITS, NK0>(root_,
            static_cast<NK0>(ik >> (IK_BITS - KEY_BITS)));
    }
    bool contains(const KEY& key) const noexcept {
        return find_value(key) != nullptr;
    }

    // ── Insert ──
    std::pair<bool, bool> insert(const KEY& key, const VALUE& value) {
        return insert_dispatch_<true, false>(key, value);
    }
    std::pair<bool, bool> insert_or_assign(const KEY& key, const VALUE& value) {
        return insert_dispatch_<true, true>(key, value);
    }
    std::pair<bool, bool> assign(const KEY& key, const VALUE& value) {
        return insert_dispatch_<false, true>(key, value);
    }

    // ── Erase ──
    bool erase(const KEY& key) {
        IK ik = KO::to_internal(key);
        NK0 nk = static_cast<NK0>(ik >> (IK_BITS - KEY_BITS));
        auto r = Ops::template erase_node_<NK0>(root_, nk, KEY_BITS, alloc_);
        if (r.erased) { root_ = r.tagged_ptr; --size_; return true; }
        return false;
    }

    // ── Iteration ──
    using ir_t = iter_result_t<IK, VALUE>;
    struct iter_result_pub { KEY key; VALUE value; bool found; };

    iter_result_pub iter_first_() const noexcept {
        auto r = Ops::template descend_min_<IK, NK0>(root_, IK{0}, 0);
        return {KO::to_key(r.key), r.value, r.found};
    }
    iter_result_pub iter_last_() const noexcept {
        auto r = Ops::template descend_max_<IK, NK0>(root_, IK{0}, 0);
        return {KO::to_key(r.key), r.value, r.found};
    }
    iter_result_pub iter_next_(KEY key) const noexcept {
        IK ik = KO::to_internal(key);
        NK0 nk = static_cast<NK0>(ik >> (IK_BITS - KEY_BITS));
        auto r = Ops::template iter_next_node_<IK, NK0>(root_, nk, IK{0}, 0);
        return {KO::to_key(r.key), r.value, r.found};
    }
    iter_result_pub iter_prev_(KEY key) const noexcept {
        IK ik = KO::to_internal(key);
        NK0 nk = static_cast<NK0>(ik >> (IK_BITS - KEY_BITS));
        auto r = Ops::template iter_prev_node_<IK, NK0>(root_, nk, IK{0}, 0);
        return {KO::to_key(r.key), r.value, r.found};
    }

    // ── Size / Stats ──
    bool   empty() const noexcept { return size_ == 0; }
    size_t size()  const noexcept { return size_; }
    void   clear() noexcept {
        if (root_ != SENTINEL_TAGGED) Ops::remove_node_(root_, alloc_);
        root_ = SENTINEL_TAGGED; size_ = 0;
    }
    size_t memory_usage() const noexcept { /* ... */ }
    auto   debug_stats() const noexcept  { /* call Ops::collect_stats_ */ }

private:
    template<bool INSERT, bool ASSIGN>
    std::pair<bool, bool> insert_dispatch_(const KEY& key, const VALUE& value) {
        IK ik = KO::to_internal(key);
        VST sv = VT::store(value, alloc_);
        NK0 nk = static_cast<NK0>(ik >> (IK_BITS - KEY_BITS));
        auto r = Ops::template insert_node_<INSERT, ASSIGN, NK0>(
            root_, nk, sv, KEY_BITS, alloc_);
        if (r.inserted) { root_ = r.tagged_ptr; ++size_; return {true, true}; }
        VT::destroy(sv, alloc_);
        if (r.tagged_ptr != root_) root_ = r.tagged_ptr;
        return {true, false};
    }
};
```

---

## Detailed function migrations

### insert_node_ (current ~120 lines → ops, ~100 lines)

Currently handles SENTINEL + LEAF + BITMASK (skip and non-skip).
After refactor: one function, skip loop integrated into bitmask case.

```cpp
template<bool INSERT, bool ASSIGN, typename NK>
static insert_result_t insert_node_(uint64_t ptr, NK ik, VST val,
                                     int bits, ALLOC& alloc) {
    constexpr int NK_BITS = sizeof(NK) * 8;

    // SENTINEL
    if (ptr == SENTINEL_TAGGED) {
        if constexpr (!INSERT) return {ptr, false, false};
        return {tag_leaf(make_single_leaf_<NK>(ik, val, bits, alloc)),
                true, false};
    }

    // LEAF
    if (ptr & LEAF_BIT) {
        uint64_t* node = untag_leaf_mut(ptr);
        auto* hdr = get_header(node);

        // Walk skip prefix
        uint8_t skip = hdr->skip();
        if (skip) [[unlikely]] {
            const uint8_t* actual = hdr->prefix_bytes();
            int save_bits = bits;
            for (uint8_t i = 0; i < skip; ++i) {
                uint8_t expected = static_cast<uint8_t>(ik >> (NK_BITS - 8));
                if (expected != actual[i]) {
                    if constexpr (!INSERT) return {ptr, false, false};
                    return {split_on_prefix_tagged_<NK>(
                        node, hdr, ik, val, actual, skip, i,
                        bits, alloc), true, false};
                }
                ik = static_cast<NK>(ik << 8);
                bits -= 8;
            }
            // After skip: narrow NK if crossed boundary
            // (see narrow_for_bits_ helper)
        }

        // Leaf insert — NK determines CO type
        if constexpr (sizeof(NK) == 1) {
            auto r = BO::template bitmap_insert<INSERT, ASSIGN>(
                node, static_cast<uint8_t>(ik), val, alloc);
            if (r.needs_split) { /* never for bitmap */ }
            return r;
        } else {
            auto r = CO<NK>::template insert<INSERT, ASSIGN>(
                node, hdr, ik, val, alloc);
            if (r.needs_split) {
                if constexpr (!INSERT) return {ptr, false, false};
                return {convert_to_bitmask_tagged_<NK>(
                    node, hdr, ik, val, bits, alloc), true, false};
            }
            return r;
        }
    }

    // BITMASK
    uint64_t* node = bm_to_node(ptr);
    auto* hdr = get_header(node);
    uint8_t sc = hdr->skip();

    // Walk skip chain
    for (uint8_t e = 0; e < sc; ++e) {
        uint64_t* embed = node + 1 + e * 6;
        uint8_t actual = reinterpret_cast<const bitmap256*>(embed)
                             ->single_bit_index();
        uint8_t expected = static_cast<uint8_t>(ik >> (NK_BITS - 8));
        if (expected != actual) {
            if constexpr (!INSERT) return {tag_bitmask(node), false, false};
            return {split_skip_at_<NK>(node, hdr, sc, e, ik, val,
                                        bits, alloc), true, false};
        }
        ik = static_cast<NK>(ik << 8);
        bits -= 8;
    }

    // Final bitmap lookup
    // ... (find slot, recurse with narrowing, update desc)
    // Narrowing: if (bits - 8 == NK_BITS / 2) → use NNK for child recursion
}
```

**Key change**: `insert_skip_chain_` merged into `insert_node_` bitmask case.
The skip loop is just a prefix of the bitmask handling.

### make_single_leaf_ (current 22 lines → ops, ~12 lines)

```cpp
template<typename NK>
static uint64_t* make_single_leaf_(NK ik, VST val, int bits, ALLOC& alloc) {
    if constexpr (sizeof(NK) == 1) {
        return BO::make_single_bitmap(static_cast<uint8_t>(ik), val, alloc);
    } else {
        return CO<NK>::make_leaf(&ik, &val, 1, 0, nullptr, alloc);
    }
}
```

No `suffix_type_for()` needed. NK IS the type.

### leaf_for_each_u64_ replacement (current 22 lines → ops, ~12 lines)

```cpp
template<typename NK, typename Fn>
static void leaf_for_each_aligned_(const uint64_t* node,
                                    const node_header* hdr, Fn&& cb) {
    if constexpr (sizeof(NK) == 1) {
        BO::for_each_bitmap(node, [&](uint8_t s, VST v) {
            cb(uint64_t(s) << 56, v);
        });
    } else {
        CO<NK>::for_each(node, hdr, [&](NK s, VST v) {
            cb(uint64_t(s) << (64 - sizeof(NK) * 8), v);
        });
    }
}
```

### build_leaf_from_arrays_ (current 30 lines → ops, ~12 lines)

```cpp
template<typename NK>
static uint64_t* build_leaf_from_arrays_(uint64_t* suf, VST* vals,
                                          size_t count, int bits,
                                          ALLOC& alloc) {
    if constexpr (sizeof(NK) == 1) {
        auto bk = std::make_unique<uint8_t[]>(count);
        for (size_t i = 0; i < count; ++i)
            bk[i] = static_cast<uint8_t>(suf[i] >> 56);
        return BO::make_bitmap_leaf(bk.get(), vals, count, alloc);
    } else {
        auto tk = std::make_unique<NK[]>(count);
        constexpr int shift = 64 - sizeof(NK) * 8;
        for (size_t i = 0; i < count; ++i)
            tk[i] = static_cast<NK>(suf[i] >> shift);
        return CO<NK>::make_leaf(tk.get(), vals, count, 0, nullptr, alloc);
    }
}
```

### collect_entries_tagged_ (current 50 lines → ops with NK, ~50 lines)

Recursive — at leaf nodes uses `leaf_for_each_aligned_<NK>`.
At bitmask nodes, recurses into children. Narrowing happens when
child bits cross boundary — same mechanism as insert.

```cpp
template<typename NK>
static void collect_entries_tagged_(uint64_t tagged, uint64_t prefix,
                                     int prefix_bits,
                                     uint64_t* keys, VST* vals,
                                     size_t& wi) {
    if (tagged & LEAF_BIT) {
        // NK from template matches this leaf's suffix type
        const uint64_t* node = untag_leaf(tagged);
        auto* hdr = get_header(node);
        // Handle skip prefix
        uint8_t skip = hdr->skip();
        if (skip) {
            const uint8_t* pb = hdr->prefix_bytes();
            for (uint8_t i = 0; i < skip; ++i) {
                prefix |= uint64_t(pb[i]) << (56 - prefix_bits);
                prefix_bits += 8;
            }
        }
        leaf_for_each_aligned_<NK>(node, hdr, [&](uint64_t aligned, VST v) {
            keys[wi] = prefix | (aligned >> prefix_bits);
            vals[wi] = v;
            wi++;
        });
        return;
    }

    // Bitmask — recurse into children
    // ... walk skip, accumulate prefix ...
    // For each child: narrow NK when crossing boundary
    bitmap.for_each_set([&](uint8_t idx, int slot) {
        uint64_t cp = cur_prefix | (uint64_t(idx) << (56 - cur_bits));
        int child_bits = cur_bits + 8;
        // Narrow check
        constexpr int NK_BITS = sizeof(NK) * 8;
        if constexpr (NK_BITS > 16) {
            if (child_bits >= NK_BITS) {
                // This child's leaves are at narrower type
                using NNK = next_narrow_t<NK>;
                collect_entries_tagged_<NNK>(child, cp, child_bits,
                                              keys, vals, wi);
                return;
            }
        }
        collect_entries_tagged_<NK>(child, cp, child_bits,
                                     keys, vals, wi);
    });
}
```

### Iteration functions (current ~310 lines → ops ~250 lines)

The big win: leaf_first_/last_/next_/prev_dispatch_ (4 functions × 25 lines
each = 100 lines of suffix_type dispatch) become:

```cpp
// leaf_next in ops
template<typename IK, typename NK>
static iter_result_t<IK,VALUE> leaf_next_(const uint64_t* node,
    node_header hdr, NK ik, IK prefix, int bits, size_t hs) {
    if constexpr (sizeof(NK) == 1) {
        auto r = BO::bitmap_iter_next(node, static_cast<uint8_t>(ik), hs);
        if (!r.found) return {{}, {}, false};
        return {combine_suffix_<IK>(prefix, bits, uint8_t(r.suffix)),
                *VT::as_ptr(*r.value), true};
    } else {
        auto r = CO<NK>::iter_next(node, &hdr, ik);
        if (!r.found) return {{}, {}, false};
        return {combine_suffix_<IK>(prefix, bits, r.suffix),
                *VT::as_ptr(*r.value), true};
    }
}
```

One function per operation instead of 4-way dispatch. All compile-time.

---

## Implementation phases

### Phase 1: Create kntrie_ops.hpp, move find_ops

- Create `kntrie_ops.hpp` with struct `kntrie_ops<VALUE, ALLOC>`
- Move `find_ops` body into it (rename to kntrie_ops, keep find methods)
- Update include chain: bitmask ← ops ← impl
- kntrie_impl::find_value calls `Ops::find_node_<BITS, NK0>`
- **Test**: compile + run all tests. Zero behavior change.

### Phase 1.5: Add BM read accessors, use in impl

Add to bitmask_ops:
- `skip_byte(node, e)`, `skip_bytes(node, sc, out)`
- `chain_lookup(node, sc, idx)`, `chain_child(node, sc, slot)`
- `chain_set_child(node, sc, slot, tagged)`
- `chain_desc_array(node, sc)` (const + mutable)
- `chain_for_each_child(node, sc, cb)`
- `bitmap_ref(bm_tagged)`, `child_at(bm_tagged, slot)`, `first_child(bm_tagged)`

Replace all raw BM layout access in kntrie_impl.hpp with these calls.
No algorithmic changes — pure mechanical substitution.

**Test**: compile + run all tests. Zero behavior change.

### Phase 2: Move helpers to ops + chain mutations to BM

**Move to ops** (KEY-independent, no NK):
- `tagged_count_`, `sum_children_desc_`, `set_desc_capped_`
- `inc_descendants_`, `dec_or_recompute_desc_`, `sum_tagged_array_`
- `prepend_skip_`, `remove_skip_`
- `remove_node_`, `destroy_leaf_`, `dealloc_bitmask_subtree_`

**Move to BM** (layout-owning operations):
- `add_child_to_chain_` → `BO::chain_add_child`
- erase chain removal block → `BO::chain_remove_child`
- erase chain collapse extraction → `BO::chain_collapse_info`
- `build_remainder_tagged_` → `BO::build_remainder`
- `wrap_bitmask_chain_` → `BO::wrap_in_chain`

All mutating functions add `ALLOC& alloc` parameter.
Move `iter_result_t` to kntrie_support.hpp, templated on `<IK, VALUE>`.

**Test**: compile + run all tests after each batch of moves.

### Phase 3: Template insert on NK

Convert insert path to NK dispatch:

1. Move `insert_node_` → `Ops::insert_node_<INSERT, ASSIGN, NK>`
   - Merge skip chain handling into bitmask case (use BO::skip_byte etc.)
   - Replace `IK ik` with `NK ik`, `IK_BITS` with `NK_BITS`
   - Replace `leaf_insert_` dispatch with `CO<NK>::insert`
   - Skip chain ops via `BO::chain_lookup`, `BO::chain_add_child`

2. Move `make_single_leaf_` → `Ops::make_single_leaf_<NK>`
   - NK determines leaf type, no suffix_type_for

3. Move `convert_to_bitmask_tagged_` → `Ops::convert_to_bitmask_<NK>`
   - Uses `leaf_for_each_aligned_<NK>` instead of `leaf_for_each_u64_`

4. Move `build_node_from_arrays_tagged_` → `Ops::build_node_<NK>`
   - Recursive builder with narrowing

5. Move `split_on_prefix_tagged_`, `split_skip_at_` → ops with NK
   - split_skip_at_ calls `BO::build_remainder` + `BO::make_bitmask` + `BO::wrap_in_chain`

6. Impl's insert_dispatch_ becomes ~10 lines calling Ops

**Test**: full test suite + ASAN after each sub-function.

### Phase 4: Template erase on NK

1. Move `erase_node_` → `Ops::erase_node_<NK>`
   - Merge skip chain handling (use BO::skip_byte, chain_lookup)
   - Replace `leaf_erase_` dispatch with `CO<NK>::erase`
   - Chain child removal via `BO::chain_remove_child`
   - Collapse via `BO::chain_collapse_info` + `BO::wrap_in_chain`

2. Move `do_coalesce_` → `Ops::do_coalesce_<NK>`
3. Move `collect_entries_tagged_` → `Ops::collect_entries_<NK>`
   - Uses `leaf_for_each_aligned_<NK>` with narrowing at boundaries
   - Uses `BO::chain_for_each_child` for bitmask iteration
4. Move `build_leaf_from_arrays_` → `Ops::build_leaf_from_arrays_<NK>`

**Test**: full test suite + ASAN.

### Phase 5: Template iteration on NK

1. Move iter_next_node_, iter_prev_node_ → ops with `<IK, NK>`
   - Use `BO::bitmap_ref(ptr)`, `BO::child_at(ptr, slot)` instead of raw access
2. Move descend_min_, descend_max_ → ops
   - Use `BO::first_child(ptr)` instead of `bm[BITMAP256_U64 + 1]`
3. Replace leaf_first_/last_/next_/prev_dispatch_ (4×25 lines of dispatch)
   with 4 compact functions using `CO<NK>::iter_xxx`
4. Replace combine_suffix_ runtime switch with compile-time shift

**Test**: full iteration test suite.

### Phase 6: Clean up

- Remove dead code from kntrie_impl.hpp
- Remove `suffix_type_for()` if no longer used (kept in support for
  header encoding, but runtime dispatch eliminated)
- Remove `CO16/CO32/CO64` type aliases from impl (ops uses `CO<NK>`)
- Verify kntrie_impl.hpp is ~200 lines

---

## BM interface expansion

### Problem

ops currently has ~55 direct accesses to BM internal layout:
- `node + 1 + e * 6` for skip embed access (15 sites)
- `final_offset = 1 + sc * 6` calculations (10 sites)
- `node + final_offset + 5` for chain children (8 sites)
- `reinterpret_cast<uint16_t*>(real_ch + nc)` for chain desc (6 sites)
- `bm[BITMAP256_U64 + 1 + slot]` for iteration child access (6 sites)
- Embed pointer fixup during realloc (4 sites)
- Skip byte extraction via `bitmap256::single_bit_index()` (8 sites)

### New BM public methods

#### Skip chain read access

```cpp
// Read skip byte at position e
static uint8_t skip_byte(const uint64_t* node, uint8_t e) noexcept;

// Copy all skip bytes into caller's buffer
static void skip_bytes(const uint64_t* node, uint8_t sc,
                        uint8_t* out) noexcept;

// Lookup byte in chain's final bitmap (analogous to standalone lookup)
static child_lookup chain_lookup(const uint64_t* node, uint8_t sc,
                                   uint8_t idx) noexcept;

// Get tagged child at slot in chain's final bitmap
static uint64_t chain_child(const uint64_t* node, uint8_t sc,
                              int slot) noexcept;

// Set child at slot in chain's final bitmap
static void chain_set_child(uint64_t* node, uint8_t sc, int slot,
                              uint64_t tagged) noexcept;

// Desc array for chain's final bitmap
static uint16_t* chain_desc_array(uint64_t* node, uint8_t sc) noexcept;
static const uint16_t* chain_desc_array(const uint64_t* node,
                                          uint8_t sc) noexcept;

// Iterate chain's final bitmap children
template<typename Fn>
static void chain_for_each_child(const uint64_t* node, uint8_t sc,
                                   Fn&& cb);
```

#### Skip chain mutation (move FROM ops TO BM)

```cpp
// Add child to chain's final bitmap (handles realloc + embed fixup)
// Returns new node pointer (may change due to realloc)
static uint64_t* chain_add_child(uint64_t* node, node_header* hdr,
                                   uint8_t sc, uint8_t idx,
                                   uint64_t child_tagged,
                                   uint16_t child_desc,
                                   ALLOC& alloc);

// Remove child from chain's final bitmap
// Handles: shrink/realloc, desc copy, embed fixup
// Returns new node + remaining child count
struct chain_remove_result {
    uint64_t* node;      // new node pointer
    unsigned  nc;        // remaining child count in final bitmap
};
static chain_remove_result chain_remove_child(
    uint64_t* node, node_header* hdr, uint8_t sc,
    int slot, uint8_t idx, ALLOC& alloc);

// Extract [from_pos..sc-1] + final bitmap into new allocation
// Returns tagged pointer (bitmask or skip chain)
static uint64_t build_remainder(uint64_t* node, node_header* hdr,
                                  uint8_t sc, uint8_t from_pos,
                                  ALLOC& alloc);

// Wrap child bitmask in skip chain, merging existing skip
// child: RAW untagged bitmask. Returns tagged bitmask.
static uint64_t wrap_in_chain(uint64_t* child, const uint8_t* bytes,
                                uint8_t count, ALLOC& alloc);

// Collapse info for single-child chain after removal
struct collapse_info {
    uint64_t  sole_child;      // tagged
    uint8_t   sole_idx;        // byte index of sole child
    uint16_t  sole_ent;        // entry count
    uint8_t   all_skip[7];     // all skip bytes + sole_idx combined
    uint8_t   total_skip;      // length of all_skip
};
static collapse_info chain_collapse_info(const uint64_t* node,
                                           uint8_t sc) noexcept;
```

#### Standalone bitmask access for iteration

```cpp
// Get bitmap reference from tagged bitmask pointer (no untag)
static const bitmap256& bitmap_ref(uint64_t bm_tagged) noexcept;

// Get child at slot from tagged bitmask pointer
static uint64_t child_at(uint64_t bm_tagged, int slot) noexcept;

// Get first child (slot 0) from tagged bitmask pointer
static uint64_t first_child(uint64_t bm_tagged) noexcept;
```

### What moves where

#### Functions that move entirely FROM impl/ops TO BM:

| Function | Current location | Lines | Why BM owns it |
|----------|-----------------|-------|----------------|
| `add_child_to_chain_` | impl:775-849 | 75 | Realloc + embed fixup + desc copy |
| `build_remainder_tagged_` | impl:913-954 | 42 | Extracts partial skip chain |
| `wrap_bitmask_chain_` | impl:1536-1569 | 34 | Merges skip chains |
| erase chain removal block | impl:1119-1179 | 61 | Shrink/realloc + desc + embed fixup |
| erase chain collapse | impl:1182-1209 | 28 | Single-child extraction from chain |

**Total moved to BM: ~240 lines**

#### What ops keeps (BM-layout-free):

| Responsibility | How ops calls BM |
|---------------|------------------|
| Walk skip bytes (match key) | `BO::skip_byte(node, e)` per byte |
| Lookup in chain final bitmap | `BO::chain_lookup(node, sc, byte)` |
| Read/set child in chain | `BO::chain_child()`, `BO::chain_set_child()` |
| Update chain desc | `BO::chain_desc_array(node, sc)[slot] = val` |
| Add child to chain | `BO::chain_add_child(node, hdr, sc, ...)` |
| Remove child from chain | `BO::chain_remove_child(node, hdr, sc, ...)` |
| Split skip chain | `BO::build_remainder(node, hdr, sc, pos, alloc)` |
| Wrap in chain | `BO::wrap_in_chain(child, bytes, count, alloc)` |
| Collapse info | `BO::chain_collapse_info(node, sc)` |
| Iterate bitmask children | `BO::bitmap_ref(ptr)`, `BO::child_at(ptr, slot)` |

#### What ops STILL owns (algorithm, not layout):

- **Recursive descent**: which child to visit, when to narrow NK
- **Split decisions**: key mismatch → ops calls `BO::build_remainder` + `BO::make_bitmask` + `BO::wrap_in_chain`
- **Coalesce decisions**: desc ≤ COMPACT_MAX → ops calls `BO::chain_for_each_child`, collects entries, builds leaf
- **Collapse after erase**: ops gets `chain_remove_result{.nc=1}`, calls `BO::chain_collapse_info()`, then decides to `prepend_skip_` or `wrap_in_chain`
- **Descriptor bookkeeping**: when to increment, recompute, cap at COALESCE_CAP

### Example: erase chain path (before → after)

**BEFORE** (impl, ~100 lines of BM layout math):
```cpp
erase_result_t erase_skip_chain_(...) {
    // Walk skip bytes — raw embed access
    for (uint8_t e = 0; e < sc; ++e) {
        uint64_t* embed = node + 1 + e * 6;                    // LAYOUT
        uint8_t actual = reinterpret_cast<const bitmap256*>(    // LAYOUT
            embed)->single_bit_index();                          // LAYOUT
        ...
    }
    // Final bitmap lookup — raw offset calc
    size_t final_offset = 1 + sc * 6;                          // LAYOUT
    const bitmap256& fbm = *reinterpret_cast<...>(              // LAYOUT
        node + final_offset);                                    // LAYOUT
    uint64_t* real_ch = node + final_offset + 5;                // LAYOUT

    // Remove child — 60 lines of raw realloc/shrink/embed fixup
    size_t needed = final_offset + 5 + nc + desc_u64(nc);      // LAYOUT
    if (should_shrink_u64(...)) {                               // LAYOUT
        // alloc new, copy header + embeds, fix embed ptrs...   // LAYOUT ×20
    } else {
        // in-place removal, save desc, shift arrays...          // LAYOUT ×10
    }

    // Collapse — 28 lines extracting sole child + skip bytes
    if (nc == 1) {
        for (uint8_t i = 0; i < sc; ++i) {
            uint64_t* eb = node + 1 + i * 6;                   // LAYOUT
            all_bytes[i] = reinterpret_cast<...>(eb)->...;      // LAYOUT
        }
    }
}
```

**AFTER** (ops, ~40 lines, zero layout knowledge):
```cpp
template<typename NK>
static erase_result_t erase_bitmask_(...) {
    // Walk skip bytes
    for (uint8_t e = 0; e < sc; ++e) {
        uint8_t actual = BO::skip_byte(node, e);
        uint8_t expected = static_cast<uint8_t>(ik >> (NK_BITS - 8));
        if (expected != actual) return {tag_bitmask(node), false, 0};
        ik = static_cast<NK>(ik << 8);
        bits -= 8;
    }

    // Final bitmap lookup
    auto lk = BO::chain_lookup(node, sc, top_byte(ik));
    if (!lk.found) return {tag_bitmask(node), false, 0};

    // Recurse into child (narrow NK if crossing boundary)
    auto cr = erase_node_<NK_or_NNK>(lk.child, ...);
    if (!cr.erased) return {tag_bitmask(node), false, 0};

    if (cr.tagged_ptr) {
        // Child survived — update
        if (cr.tagged_ptr != lk.child)
            BO::chain_set_child(node, sc, lk.slot, cr.tagged_ptr);
        BO::chain_desc_array(node, sc)[lk.slot] = cr.subtree_entries;
        // desc bookkeeping stays in ops (algorithm, not layout)
        ...
        return ...;
    }

    // Child erased — BM handles the removal mechanics
    auto rr = BO::chain_remove_child(node, hdr, sc, lk.slot, ti, alloc);
    if (rr.nc == 0) return {0, true, 0};

    if (rr.nc == 1) {
        // Ops decides how to collapse, BM provides the info
        auto ci = BO::chain_collapse_info(rr.node, sc);
        size_t node_au64 = get_header(rr.node)->alloc_u64();
        if (ci.sole_child & LEAF_BIT) {
            auto* leaf = untag_leaf_mut(ci.sole_child);
            leaf = prepend_skip_(leaf, ci.total_skip, ci.all_skip, alloc);
            dealloc_node(alloc, rr.node, node_au64);
            return {tag_leaf(leaf), true, ci.sole_ent};
        }
        auto* cn = bm_to_node(ci.sole_child);
        dealloc_node(alloc, rr.node, node_au64);
        return {BO::wrap_in_chain(cn, ci.all_skip, ci.total_skip, alloc),
                true, ci.sole_ent};
    }

    // Multi-child — desc bookkeeping + coalesce check (ops algorithm)
    ...
}
```

**~60% less code in ops, zero BM layout leakage.**

### Example: iteration bitmask descent (before → after)

**BEFORE**:
```cpp
const auto* bm = reinterpret_cast<const uint64_t*>(ptr);
const auto& bitmap = *reinterpret_cast<const bitmap256*>(bm);
uint8_t byte = static_cast<uint8_t>(ik >> (IK_BITS - 8));
if (bitmap.has_bit(byte)) {
    int slot = bitmap.find_slot<slot_mode::UNFILTERED>(byte);
    auto r = iter_next_node_(bm[BITMAP256_U64 + 1 + slot], ...);
```

**AFTER**:
```cpp
const auto& bitmap = BO::bitmap_ref(ptr);
uint8_t byte = static_cast<uint8_t>(ik >> (NK_BITS - 8));
if (bitmap.has_bit(byte)) {
    int slot = bitmap.find_slot<slot_mode::UNFILTERED>(byte);
    auto r = iter_next_node_<NK>(BO::child_at(ptr, slot), ...);
```

Same number of lines, but no BITMAP256_U64 constant or raw pointer casts.

### Updated phases and sizes

See **Implementation phases** and **Estimated sizes** sections below —
updated to reflect BM expansion.

---

## Risk areas

1. **Skip chain narrowing**: Most complex interaction. Skip can consume
   bytes that cross NK boundaries. Test extensively with sequential data
   (creates skip chains) and signed keys.

2. **Coalesce collect_entries**: Must narrow NK at subtree boundaries.
   A bitmask at bits=32 (NK=u32) can have CO16 grandchildren. The
   recursive template handles this, but verify with mixed-depth subtrees.

3. **Build paths**: `build_node_from_arrays_tagged_` recurses and may
   create leaves at different NK levels. NK must match what insert would
   create at that depth.

4. **Iterator prefix arithmetic**: Currently uses IK (uint32_t or uint64_t).
   After refactor, IK is always the unsigned key type. Prefix shifts must
   use IK_BITS, not NK_BITS. NK is only for leaf dispatch.

5. **Assign path**: `<false, true>` (assign-only, no insert) still needs
   correct NK dispatch for the ptr update when CO reallocs.

---

## Estimated sizes after refactor

| File | Current | After |
|------|---------|-------|
| kntrie.hpp | 227 | ~230 (unchanged) |
| kntrie_impl.hpp | 1942 | ~200 |
| kntrie_ops.hpp | (new) | ~1100 |
| kntrie_bitmask.hpp | 816 | ~1060 (+240 from moved functions) |
| kntrie_compact.hpp | (unchanged) | (unchanged) |
| kntrie_support.hpp | 307 | ~330 (iter_result_t, next_narrow_t) |
