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

**Enables node_header simplification**: with suffix_type no longer read
at runtime, and BITMASK_BIT redundant with tagged pointer dispatch,
node_header drops from bit-packed flags to 4 clean uint16 fields.
See **node_header redesign** section.

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
            return Narrow::template insert_node_<INSERT, ASSIGN>(
                child, nik, val, bits, alloc);
        }
    }
    return insert_node_(child, static_cast<NK>(ik << 8), val, bits, alloc);
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
template<typename NK, typename VALUE, typename ALLOC>
struct kntrie_ops {
    using BO  = bitmask_ops<VALUE, ALLOC>;
    using VT  = value_traits<VALUE, ALLOC>;
    using VST = typename VT::slot_type;
    using CO  = compact_ops<NK, VALUE, ALLOC>;

    static constexpr int NK_BITS = sizeof(NK) * 8;

    // Narrowing: recurse into a different struct specialization
    using NNK = next_narrow_t<NK>;  // u64→u32→u16→u8
    using Narrow = kntrie_ops<NNK, VALUE, ALLOC>;

    // ── Find (BITS is the only method-level template param) ───────
    template<int BITS>
    static const VALUE* find_node_(uint64_t ptr, NK ik) noexcept;
    template<int BITS>
    static const VALUE* find_leaf_(const uint64_t* node,
                                    node_header h, NK ik) noexcept;

    // ── Insert ────────────────────────────────────────────────────
    template<bool INSERT, bool ASSIGN>
    static insert_result_t insert_node_(uint64_t ptr, NK ik, VST val,
                                         int bits, ALLOC& alloc);

    static uint64_t* make_single_leaf_(NK ik, VST val,
                                        int bits, ALLOC& alloc);

    static uint64_t convert_to_bitmask_tagged_(uint64_t* node,
        node_header* hdr, NK ik, VST val, int bits, ALLOC& alloc);

    static uint64_t build_node_from_arrays_tagged_(uint64_t* suf,
        VST* vals, size_t count, int bits, ALLOC& alloc);

    static uint64_t build_bitmask_from_arrays_tagged_(uint64_t* suf,
        VST* vals, size_t count, int bits, ALLOC& alloc);

    static uint64_t split_on_prefix_tagged_(..., ALLOC& alloc);

    static uint64_t split_skip_at_(..., ALLOC& alloc);

    // ── Erase ─────────────────────────────────────────────────────
    static erase_result_t erase_node_(uint64_t ptr, NK ik,
                                       int bits, ALLOC& alloc);

    static erase_result_t do_coalesce_(..., ALLOC& alloc);

    static void collect_entries_tagged_(uint64_t tagged,
        uint64_t prefix, int prefix_bits,
        uint64_t* keys, VST* vals, size_t& wi);

    static void dealloc_bitmask_subtree_(uint64_t tagged, ALLOC& alloc);

    static uint64_t* build_leaf_from_arrays_(uint64_t* suf, VST* vals,
        size_t count, int bits, ALLOC& alloc);

    // ── Iteration (read-only, no alloc) ───────────────────────────
    // IK = full-width unsigned key type, passed through for prefix
    // accumulation. NK may be narrower.
    template<typename IK>
    static iter_result_t<IK,VALUE> iter_next_node_(uint64_t ptr,
        NK ik, IK prefix, int bits);
    template<typename IK>
    static iter_result_t<IK,VALUE> iter_prev_node_(...);
    template<typename IK>
    static iter_result_t<IK,VALUE> descend_min_(uint64_t ptr,
        IK prefix, int bits);
    template<typename IK>
    static iter_result_t<IK,VALUE> descend_max_(...);

    template<typename IK>
    static IK combine_suffix_(IK prefix, int bits, NK suffix) {
        constexpr int IK_BITS = sizeof(IK) * 8;
        IK suffix_ik = IK(suffix) << (IK_BITS - NK_BITS);
        return prefix | (suffix_ik >> bits);
    }

    // ── Leaf dispatch (NK determines CO type) ─────────────────────
    template<typename Fn>
    static void leaf_for_each_aligned_(const uint64_t* node,
                                        const node_header* hdr, Fn&& cb);

    // ── Descriptor tracking (NK-independent, link-time dedup) ─────
    static uint16_t tagged_count_(uint64_t tagged) noexcept;
    static uint16_t sum_children_desc_(const uint64_t* node, uint8_t sc);
    static void     set_desc_capped_(uint64_t* node, size_t count);
    static void     inc_descendants_(node_header* h);
    static uint16_t dec_or_recompute_desc_(uint64_t* node, uint8_t sc);
    static uint16_t sum_tagged_array_(const uint64_t* children, unsigned nc);

    // ── Skip/node helpers (NK-independent) ────────────────────────
    static uint64_t* prepend_skip_(..., ALLOC& alloc);
    static uint64_t* remove_skip_(..., ALLOC& alloc);

    // ── Destroy / Stats ───────────────────────────────────────────
    static void remove_node_(uint64_t tagged, ALLOC& alloc);
    static void destroy_leaf_(uint64_t* node, node_header* hdr, ALLOC& alloc);
    static void collect_stats_(uint64_t tagged, debug_stats_t& s);
};
```

**Narrowing pattern**: when recursion crosses an NK boundary, ops calls
into a different specialization:
```cpp
// Inside kntrie_ops<uint64_t, V, A>::insert_node_:
if (bits == NK_BITS / 2) {
    NNK nik = static_cast<NNK>(static_cast<NK>(ik << 8) >> (NK_BITS / 2));
    return Narrow::template insert_node_<INSERT, ASSIGN>(
        child, nik, val, bits, alloc);
}
```

This means `kntrie_ops<u64>` calls `kntrie_ops<u32>` calls `kntrie_ops<u16>`.
Each is a different struct — no method-level NK template parameter needed.

---

## kntrie_impl.hpp after refactor (~200 lines)

```cpp
template<typename KEY, typename VALUE, typename ALLOC>
class kntrie_impl {
    // KEY is always unsigned (kntrie handles signed→unsigned)
    using KO  = key_ops<KEY>;
    using IK  = typename KO::IK;
    using VT  = value_traits<VALUE, ALLOC>;
    using VST = typename VT::slot_type;

    static constexpr int KEY_BITS = KO::KEY_BITS;
    static constexpr int IK_BITS  = KO::IK_BITS;

    using NK0 = std::conditional_t<KEY_BITS <= 8,  uint8_t,
                std::conditional_t<KEY_BITS <= 16, uint16_t,
                std::conditional_t<KEY_BITS <= 32, uint32_t, uint64_t>>>;

    // Ops specialized on NK0 — narrowing happens inside ops automatically
    using Ops = kntrie_ops<NK0, VALUE, ALLOC>;
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
        return Ops::template find_node_<KEY_BITS>(root_,
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
        auto r = Ops::erase_node_(root_, nk, KEY_BITS, alloc_);
        if (r.erased) { root_ = r.tagged_ptr; --size_; return true; }
        return false;
    }

    // ── Iteration ──
    using ir_t = iter_result_t<IK, VALUE>;
    struct iter_result_pub { KEY key; VALUE value; bool found; };

    iter_result_pub iter_first_() const noexcept {
        auto r = Ops::template descend_min_<IK>(root_, IK{0}, 0);
        return {KO::to_key(r.key), r.value, r.found};
    }
    iter_result_pub iter_last_() const noexcept {
        auto r = Ops::template descend_max_<IK>(root_, IK{0}, 0);
        return {KO::to_key(r.key), r.value, r.found};
    }
    iter_result_pub iter_next_(KEY key) const noexcept {
        IK ik = KO::to_internal(key);
        NK0 nk = static_cast<NK0>(ik >> (IK_BITS - KEY_BITS));
        auto r = Ops::template iter_next_node_<IK>(root_, nk, IK{0}, 0);
        return {KO::to_key(r.key), r.value, r.found};
    }
    iter_result_pub iter_prev_(KEY key) const noexcept {
        IK ik = KO::to_internal(key);
        NK0 nk = static_cast<NK0>(ik >> (IK_BITS - KEY_BITS));
        auto r = Ops::template iter_prev_node_<IK>(root_, nk, IK{0}, 0);
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
        auto r = Ops::template insert_node_<INSERT, ASSIGN>(
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
                Narrow::collect_entries_tagged_(child, cp, child_bits,
                                                keys, vals, wi);
                return;
            }
        }
        collect_entries_tagged_(child, cp, child_bits,
                                 keys, vals, wi);
    });
}
```

### Iteration functions (current ~310 lines → ops ~250 lines)

The big win: leaf_first_/last_/next_/prev_dispatch_ (4 functions × 25 lines
each = 100 lines of suffix_type dispatch) become compile-time via NK:

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

## node_header redesign

### Current (8 bytes, awkward bit packing)

```
byte 0:  flags_        uint8   [bit 0: BITMASK_BIT, bits 1-3: skip, bits 4-7: unused]
byte 1:  suffix_type_  uint8   [0=bitmap, 1=CO16, 2=CO32, 3=CO64]
byte 2-3: entries_     uint16
byte 4-5: alloc_u64_   uint16
byte 6-7: total_slots_ uint16
```

### New (8 bytes, 4 aligned uint16s)

```
byte 0-1: skip_count_   uint16  [max 6 in practice, bits 3-15 free for future flags]
byte 2-3: entries_       uint16
byte 4-5: alloc_u64_     uint16
byte 6-7: total_slots_   uint16
```

### What's removed and why

**BITMASK_BIT** — Every caller already branches on the tagged pointer
(`ptr & LEAF_BIT`), never the header flag. `is_leaf()` / `set_bitmask()`
are dead code. Verified: find_leaf_, insert_node_, erase_node_,
remove_node_, collect_stats_ — all check tagged pointer first.

**suffix_type_** — With NK templating, `CO<NK>` is resolved at compile
time. `NK=u8` → bitmap leaf. `NK=u16/u32/u64` → compact leaf with that
suffix width. The node never needs to self-describe its format at runtime.
`suffix_type_for()` and all 4-way dispatch on suffix_type are eliminated.

**skip_count_ as uint16** — Current 3-bit extraction
`(flags_ >> 1) & 0x07` becomes a single aligned uint16 load. Simpler
access, no bit manipulation. Max value is 6, so bits 3–15 are free
for future use (type flags, format version, etc.) without changing
the skip_count access pattern (just mask low bits if needed).

### Debug concern

After removing suffix_type_, raw node dumps without template context
can't determine leaf format. Acceptable trade-off: the spare bits in
skip_count_ (bits 3–15) can store a format tag if debug inspection
is ever needed.

### Changes to code

**Prerequisite**: all NK templating complete (Phases 3-5) so no code
reads `suffix_type_` or `is_leaf()` anymore. Implemented as Phase 5.5.

**kntrie_support.hpp** — replace node_header struct:
```cpp
struct node_header {
    uint16_t skip_count_  = 0;
    uint16_t entries_     = 0;
    uint16_t alloc_u64_   = 0;
    uint16_t total_slots_ = 0;

    // --- skip ---
    uint16_t skip()    const noexcept { return skip_count_; }
    bool     is_skip() const noexcept { return skip_count_ != 0; }
    void set_skip(uint16_t s) noexcept { skip_count_ = s; }

    // --- entries / alloc ---
    unsigned entries()   const noexcept { return entries_; }
    void set_entries(unsigned n) noexcept { entries_ = static_cast<uint16_t>(n); }

    unsigned alloc_u64() const noexcept { return alloc_u64_; }
    void set_alloc_u64(unsigned n) noexcept { alloc_u64_ = static_cast<uint16_t>(n); }

    unsigned total_slots() const noexcept { return total_slots_; }
    void set_total_slots(unsigned n) noexcept { total_slots_ = static_cast<uint16_t>(n); }

    // Bitmask-only: total_slots_ as descendant count
    uint16_t descendants() const noexcept { return total_slots_; }
    void set_descendants(uint16_t n) noexcept { total_slots_ = n; }
};
static_assert(sizeof(node_header) == 8);
```

**Remove from kntrie_support.hpp:**
- `BITMASK_BIT` constant
- `is_leaf()`, `set_bitmask()` methods
- `suffix_type()`, `set_suffix_type()` methods
- `suffix_type_for()` free function (if present)

**kntrie_compact.hpp** — remove all `set_suffix_type()` calls in
`make_leaf`, `grow_and_insert`, etc. CO already knows its suffix width
from its NK template parameter.

**kntrie_bitmask.hpp** — remove `set_bitmask()` calls in `make_bitmask`,
`make_skip_chain`, etc. Bitmask vs leaf is determined by tagged pointer.

**kntrie_impl.hpp** — remove all `suffix_type()` reads in dispatch code.
This is eliminated by NK anyway, but the header cleanup comes first.

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

1. Move `insert_node_` → `Ops::insert_node_<INSERT, ASSIGN>`
   - Merge skip chain handling into bitmask case (use BO::skip_byte etc.)
   - `NK ik` parameter, narrowing via `Narrow::insert_node_<INSERT, ASSIGN>`
   - Replace `leaf_insert_` dispatch with `CO::insert` (CO is struct alias)
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

1. Move `erase_node_` → `Ops::erase_node_`
   - Merge skip chain handling (use BO::skip_byte, chain_lookup)
   - Replace `leaf_erase_` dispatch with `CO::erase`
   - Narrowing via `Narrow::erase_node_`
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

### Phase 5.5: Simplify node_header

Prerequisite: all suffix_type_ reads eliminated by NK templating (Phases 3-5).
All is_leaf() / set_bitmask() calls replaced by tagged pointer checks.

- Replace node_header struct: 4 aligned uint16 fields
- Remove `BITMASK_BIT`, `is_leaf()`, `set_bitmask()`, `suffix_type()`,
  `set_suffix_type()` from header and all call sites
- Replace `skip()` 3-bit extraction with uint16 field access
- Remove `set_bitmask()` from BM's make_bitmask/make_skip_chain
- Remove `set_suffix_type()` from CO's make_leaf/grow_and_insert
- **Test**: compile + run all tests + ASAN. Zero behavior change
  (all removed fields are now dead code).

### Phase 6: Clean up

- Remove dead code from kntrie_impl.hpp
- Remove `CO16/CO32/CO64` type aliases from impl (ops uses `CO<NK>`)
- Verify kntrie_impl.hpp is ~200 lines
- Final audit: grep for any remaining raw BM layout access in ops

---

## BM interface expansion

### Core insight: hs-parameterized shared helpers

BM's private helpers already take `header_size` (hs):
```cpp
static const bitmap256& bm_(const uint64_t* n, size_t hs);
static uint64_t* real_children_mut_(uint64_t* n, size_t hs);
static uint16_t* desc_array_mut_(uint64_t* n, size_t hs, unsigned nc);
```

Standalone hardcodes `hs = 1`. Chain uses `hs = 1 + sc * 6`.
**Same bitmap, same children layout, same desc layout — just at a different offset.**

The existing public methods (`lookup`, `add_child`, `remove_child`) become
thin wrappers around `_at_` private helpers parameterized on hs:

```cpp
// Private shared core
static child_lookup lookup_at_(const uint64_t* node, size_t hs, uint8_t idx);
static uint64_t* add_child_at_(uint64_t* node, node_header* h, size_t hs,
                                uint8_t idx, uint64_t child_tagged,
                                uint16_t child_desc, ALLOC& alloc);
static uint64_t* remove_child_at_(uint64_t* node, node_header* h, size_t hs,
                                   int slot, uint8_t idx, ALLOC& alloc);

// Public standalone (hs=1)
static child_lookup lookup(const uint64_t* node, uint8_t idx) {
    return lookup_at_(node, 1, idx);
}
static uint64_t* add_child(...) {
    return add_child_at_(node, h, 1, idx, child_tagged, child_desc, alloc);
}
static uint64_t* remove_child(...) {
    return remove_child_at_(node, h, 1, slot, idx, alloc);
}

// Public chain (hs = chain_hs_(sc), + embed fixup on realloc)
static child_lookup chain_lookup(const uint64_t* node, uint8_t sc, uint8_t idx) {
    return lookup_at_(node, chain_hs_(sc), idx);
}
static uint64_t* chain_add_child(uint64_t* node, node_header* h,
                                   uint8_t sc, uint8_t idx,
                                   uint64_t child_tagged, uint16_t child_desc,
                                   ALLOC& alloc) {
    size_t hs = chain_hs_(sc);
    auto* nn = add_child_at_(node, h, hs, idx, child_tagged, child_desc, alloc);
    if (nn != node && sc > 0) fix_embeds_(nn, sc);
    return nn;
}
static uint64_t* chain_remove_child(uint64_t* node, node_header* h,
                                      uint8_t sc, int slot, uint8_t idx,
                                      ALLOC& alloc) {
    size_t hs = chain_hs_(sc);
    auto* nn = remove_child_at_(node, h, hs, slot, idx, alloc);
    if (nn && nn != node && sc > 0) fix_embeds_(nn, sc);
    return nn;
}
```

### Why this works for realloc

The `_at_` core's realloc path copies `hs * sizeof(uint64_t)` bytes as prefix:
- Standalone `hs=1`: copies 8 bytes (header only)
- Chain `hs=1+sc*6`: copies header + all embeds

Then bitmap/sentinel/children/desc are at offset `hs` — same code either way.
The chain wrapper's only extra step: `fix_embeds_` to update embed child
pointers that still reference the old allocation.

```cpp
private:
    static constexpr size_t chain_hs_(uint8_t sc) noexcept {
        return 1 + static_cast<size_t>(sc) * 6;
    }
    static void fix_embeds_(uint64_t* node, uint8_t sc) noexcept {
        for (uint8_t e = 0; e < sc; ++e) {
            uint64_t* embed_child = node + 1 + e * 6 + 5;
            uint64_t* next_bm = node + 1 + (e + 1) * 6;
            *embed_child = reinterpret_cast<uint64_t>(next_bm);
        }
    }
```

### The full BM public interface after refactor

#### Read accessors (new, ~30 lines)

```cpp
// Skip chain byte access
static uint8_t skip_byte(const uint64_t* node, uint8_t e) noexcept;
static void skip_bytes(const uint64_t* node, uint8_t sc, uint8_t* out) noexcept;

// Chain final bitmap — same as standalone but hs-parameterized
static child_lookup chain_lookup(const uint64_t* node, uint8_t sc, uint8_t idx);
static uint64_t chain_child(const uint64_t* node, uint8_t sc, int slot);
static void chain_set_child(uint64_t* node, uint8_t sc, int slot, uint64_t tagged);
static uint16_t* chain_desc_array(uint64_t* node, uint8_t sc);
static const uint16_t* chain_desc_array(const uint64_t* node, uint8_t sc);

template<typename Fn>
static void chain_for_each_child(const uint64_t* node, uint8_t sc, Fn&& cb);

// Tagged pointer access for iteration (avoids BITMAP256_U64 in caller)
static const bitmap256& bitmap_ref(uint64_t bm_tagged) noexcept;
static uint64_t child_at(uint64_t bm_tagged, int slot) noexcept;
static uint64_t first_child(uint64_t bm_tagged) noexcept;
```

#### Mutation — shared core with chain wrappers (refactored, ~0 net new lines)

Existing `add_child`, `remove_child` refactored to call `_at_` core.
Chain wrappers added — thin, call same core + embed fixup.

#### Chain structural operations (moved from impl, ~120 lines)

```cpp
// Extract [from_pos..sc-1] + final bitmap into new allocation
// Returns tagged pointer (bitmask or skip chain)
static uint64_t build_remainder(uint64_t* node, node_header* hdr,
                                  uint8_t sc, uint8_t from_pos, ALLOC& alloc);

// Wrap child bitmask in skip chain, merging existing skip
// child: RAW untagged bitmask. Returns tagged bitmask.
static uint64_t wrap_in_chain(uint64_t* child, const uint8_t* bytes,
                                uint8_t count, ALLOC& alloc);

// Collapse info for single-child chain/standalone after removal
struct collapse_info {
    uint64_t  sole_child;      // tagged
    uint8_t   sole_idx;        // byte index
    uint16_t  sole_ent;        // entry count
    uint8_t   all_skip[7];     // all skip bytes + sole_idx
    uint8_t   total_skip;      // length of all_skip
};
static collapse_info chain_collapse_info(const uint64_t* node,
                                           uint8_t sc) noexcept;
// Standalone version (sc=0, just the single remaining child)
static collapse_info standalone_collapse_info(const uint64_t* node) noexcept;
```

### What moves where

#### Moves from impl TO BM's shared core:

| Function | Lines | Becomes |
|----------|-------|---------|
| `add_child_to_chain_` | 75 | `chain_add_child` (wraps `add_child_at_`) |
| erase chain removal | 61 | `chain_remove_child` (wraps `remove_child_at_`) |
| erase chain collapse | 28 | `chain_collapse_info` |
| erase standalone collapse | 22 | `standalone_collapse_info` |
| `build_remainder_tagged_` | 42 | `build_remainder` |
| `wrap_bitmask_chain_` | 34 | `wrap_in_chain` |

**~240 lines move from impl → BM**, but BM only grows ~120 net because
add/remove child code is deduplicated via `_at_` core (standalone + chain
share the same implementation instead of two separate 55-line copies).

#### What ops calls:

| Ops responsibility | BM call |
|-------------------|---------|
| Walk skip bytes | `BO::skip_byte(node, e)` |
| Lookup in chain | `BO::chain_lookup(node, sc, byte)` |
| Read/set child | `BO::chain_child()`, `BO::chain_set_child()` |
| Update desc | `BO::chain_desc_array(node, sc)[slot] = val` |
| Add child | `BO::chain_add_child(node, hdr, sc, ...)` |
| Remove child | `BO::chain_remove_child(node, hdr, sc, ...)` |
| Split chain | `BO::build_remainder(...)` + `BO::make_bitmask(...)` + `BO::wrap_in_chain(...)` |
| Collapse info | `BO::chain_collapse_info(node, sc)` |
| Iterate bitmask | `BO::bitmap_ref(ptr)`, `BO::child_at(ptr, slot)` |

#### What ops STILL owns (algorithm, not layout):

- **Recursive descent**: which child to visit, when to narrow NK
- **Split decisions**: key mismatch → ops calls `BO::build_remainder` + `BO::make_bitmask` + `BO::wrap_in_chain`
- **Coalesce decisions**: desc ≤ COMPACT_MAX → ops calls `BO::chain_for_each_child`, collects entries, builds leaf
- **Collapse after erase**: ops gets `remove_child` result, calls `BO::chain_collapse_info()`, then decides to `prepend_skip_` or `wrap_in_chain`
- **Descriptor bookkeeping**: when to increment, recompute, cap at COALESCE_CAP

### Deduplication example: add_child

**BEFORE** — two separate implementations (~130 lines total):

```cpp
// In BM: standalone add_child (~55 lines)
static uint64_t* add_child(node, h, idx, child, desc, alloc) {
    constexpr size_t hs = 1;
    // ... in-place or realloc with bm_, real_children_, desc_array_ at hs=1
}

// In impl: add_child_to_chain_ (~75 lines, mostly same logic)
uint64_t* add_child_to_chain_(node, hdr, sc, idx, child, desc) {
    size_t final_offset = 1 + sc * 6;
    // ... in-place or realloc with raw offset math at final_offset
    // ... plus embed fixup on realloc
}
```

**AFTER** — one core, two thin wrappers (~60 lines total):

```cpp
// In BM: shared core (~55 lines, hs-parameterized)
static uint64_t* add_child_at_(node, h, hs, idx, child, desc, alloc) {
    // ... in-place or realloc with bm_, real_children_, desc_array_ at hs
    // realloc copies hs*8 bytes as prefix — works for both
}

// Standalone wrapper (1 line)
static uint64_t* add_child(node, h, idx, child, desc, alloc) {
    return add_child_at_(node, h, 1, idx, child, desc, alloc);
}

// Chain wrapper (3 lines)
static uint64_t* chain_add_child(node, h, sc, idx, child, desc, alloc) {
    size_t hs = chain_hs_(sc);
    auto* nn = add_child_at_(node, h, hs, idx, child, desc, alloc);
    if (nn != node && sc > 0) fix_embeds_(nn, sc);
    return nn;
}
```

Same pattern for `remove_child` / `chain_remove_child`.
Net: **~130 lines of duplicated logic → ~60 lines of shared code.**

### Ops erase example (before → after)

**BEFORE** (impl, ~100 lines of BM layout math):
```cpp
erase_result_t erase_skip_chain_(...) {
    for (uint8_t e = 0; e < sc; ++e) {
        uint64_t* embed = node + 1 + e * 6;                    // LAYOUT
        uint8_t actual = reinterpret_cast<const bitmap256*>(    // LAYOUT
            embed)->single_bit_index();                          // LAYOUT
        ...
    }
    size_t final_offset = 1 + sc * 6;                          // LAYOUT
    const bitmap256& fbm = *reinterpret_cast<...>(              // LAYOUT
        node + final_offset);                                    // LAYOUT
    uint64_t* real_ch = node + final_offset + 5;                // LAYOUT

    // 60 lines of raw realloc/shrink/embed fixup                // LAYOUT ×30
    // 28 lines extracting sole child + skip bytes               // LAYOUT ×10
}
```

**AFTER** (ops, ~40 lines, zero layout knowledge):
```cpp
template<typename NK>
static erase_result_t erase_bitmask_(...) {
    for (uint8_t e = 0; e < sc; ++e) {
        uint8_t actual = BO::skip_byte(node, e);     // BM accessor
        ...
    }
    auto lk = BO::chain_lookup(node, sc, byte);      // BM accessor

    // Recurse with NK narrowing...

    // Child erased → BM handles mechanics
    auto* nn = BO::chain_remove_child(node, hdr, sc, slot, idx, alloc);
    if (!nn) return {0, true, 0};
    if (get_header(nn)->entries() == 1) {
        auto ci = BO::chain_collapse_info(nn, sc);    // BM accessor
        // Ops decides: prepend_skip_ or wrap_in_chain
        ...
    }
}
```

### Ops iteration example (before → after)

**BEFORE**:
```cpp
const auto* bm = reinterpret_cast<const uint64_t*>(ptr);
const auto& bitmap = *reinterpret_cast<const bitmap256*>(bm);
auto r = iter_next_node_(bm[BITMAP256_U64 + 1 + slot], ...);
```

**AFTER**:
```cpp
const auto& bitmap = BO::bitmap_ref(ptr);
auto r = iter_next_node_<NK>(BO::child_at(ptr, slot), ...);
```

### BM estimated growth

| Source | Lines |
|--------|-------|
| New read accessors | +30 |
| Chain structural ops (from impl) | +120 |
| Dedup savings (add/remove shared core) | -70 |
| **Net BM growth** | **~80** |

BM: 816 → ~900 lines.

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
| kntrie_bitmask.hpp | 816 | ~900 (+120 from impl, -70 dedup) |
| kntrie_compact.hpp | (unchanged) | (unchanged) |
| kntrie_support.hpp | 307 | ~330 (iter_result_t, next_narrow_t) |
