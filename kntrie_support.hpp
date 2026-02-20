#ifndef KNTRIE_SUPPORT_HPP
#define KNTRIE_SUPPORT_HPP

#include <cstdint>
#include <cstring>
#include <bit>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>
#include <algorithm>
#include <cassert>

namespace gteitelbaum {

// ==========================================================================
// Constants
// ==========================================================================

inline constexpr size_t BITMAP_256_U64 = 4;   // 32 bytes
inline constexpr size_t COMPACT_MAX   = 4096;
inline constexpr size_t BOT_LEAF_MAX  = 4096;
inline constexpr size_t HEADER_U64    = 1;   // base header is 1 u64 (8 bytes), +1 if skip

// u64s needed for descendants count (single u64 at end of bitmask node)
inline constexpr size_t desc_u64(size_t) noexcept { return 1; }

// Tagged pointer: bit 63 = leaf (sign bit for fast test)
static constexpr uint64_t LEAF_BIT = uint64_t(1) << 63;

// ==========================================================================
// NK narrowing: u64 → u32 → u16 → u8
// ==========================================================================

template<typename NK>
using next_narrow_t = std::conditional_t<sizeof(NK) == 8, uint32_t,
                      std::conditional_t<sizeof(NK) == 4, uint16_t, uint8_t>>;

// ==========================================================================
// Freelist size classes
//
// ==========================================================================
// Allocation size classes (1.5x growth scheme)
//
// ≤128 u64s: step sizes for in-place growth padding.
// >128 u64s: power-of-2 with midpoints.
// ==========================================================================

inline constexpr size_t FREE_MAX  = 128;
inline constexpr size_t NUM_BINS  = 12;
inline constexpr size_t BIN_SIZES[NUM_BINS] = {4, 6, 8, 10, 14, 18, 26, 34, 48, 69, 98, 128};

inline constexpr size_t round_up_u64(size_t n) noexcept {
    if (n <= FREE_MAX) {
        for (size_t i = 0; i < NUM_BINS; ++i)
            if (n <= BIN_SIZES[i]) return BIN_SIZES[i];
    }
    int bit  = static_cast<int>(std::bit_width(n - 1));
    size_t pow2 = size_t{1} << bit;
    size_t mid  = pow2 / 2 + pow2 / 4 + 2;
    return (n <= mid) ? mid : pow2;
}

// Shrink when allocated exceeds the class for 2x the needed size.
inline constexpr bool should_shrink_u64(size_t allocated, size_t needed) noexcept {
    return allocated > round_up_u64(needed * 2);
}

// ==========================================================================
// Node Header  (8 bytes = 1 u64)
//
// Struct layout (little-endian):
//   [0]      flags       (bit 0: is_bitmask, bits 1-3: skip count 0-7)
//   [1]      suffix_type (leaf only: 0=bitmap_256_t, 1=u16, 2=u32, 3=u64)
//   [2..3]   entries     (uint16_t)
//   [4..5]   alloc_u64   (uint16_t)
//   [6..7]   total_slots (uint16_t, compact leaf slot count)
//
// Skip semantics (via skip() / set_skip()):
//   - Leaf: # prefix bytes stored in node[1] bytes 0-5
//   - Bitmask: # embedded bo<1> nodes (skip chain length)
//
// Leaf skip data in node[1]: bytes [0..5] prefix (outer first).
// Count now in header, NOT in node[1] byte 7.
//
// Zeroed header -> is_leaf=true, skip=0, suffix_type=0,
//                  entries=0. Sentinel-safe.
// ==========================================================================

struct node_header_t {
    uint8_t  skip_count_v  = 0;  // max 6, bits 3-7 free
    uint8_t  reserved_v    = 0;
    uint16_t entries_v     = 0;
    uint16_t alloc_u64_v   = 0;
    uint16_t total_slots_v = 0;

    // --- skip ---
    uint8_t skip()    const noexcept { return skip_count_v & 0x07; }
    bool    is_skip() const noexcept { return skip_count_v & 0x07; }
    void set_skip(uint8_t s) noexcept { skip_count_v = (s & 0x07); }

    // --- leaf prefix: packed uint64_t in node[1], byte 0 at bits 63..56 ---
    uint64_t prefix_u64() const noexcept {
        auto* p = reinterpret_cast<const uint64_t*>(this);
        return p[1];
    }
    uint8_t prefix_byte(uint8_t i) const noexcept {
        return static_cast<uint8_t>(prefix_u64() >> (56 - 8 * i));
    }
    void set_prefix_u64(uint64_t v) noexcept {
        auto* p = reinterpret_cast<uint64_t*>(this);
        p[1] = v;
    }
    void set_prefix(const uint8_t* pfx, uint8_t len) noexcept {
        uint64_t v = 0;
        for (uint8_t i = 0; i < len; ++i)
            v |= uint64_t(pfx[i]) << (56 - 8 * i);
        set_prefix_u64(v);
    }

    // --- entries / alloc ---
    unsigned entries()   const noexcept { return entries_v; }
    void set_entries(unsigned n) noexcept { entries_v = static_cast<uint16_t>(n); }

    unsigned alloc_u64() const noexcept { return alloc_u64_v; }
    void set_alloc_u64(unsigned n) noexcept { alloc_u64_v = static_cast<uint16_t>(n); }

    unsigned total_slots() const noexcept { return total_slots_v; }
    void set_total_slots(unsigned n) noexcept { total_slots_v = static_cast<uint16_t>(n); }
};
static_assert(sizeof(node_header_t) == 8);

inline node_header_t*       get_header(uint64_t* n)       noexcept { return reinterpret_cast<node_header_t*>(n); }
inline const node_header_t* get_header(const uint64_t* n) noexcept { return reinterpret_cast<const node_header_t*>(n); }

// --- Prefix u64 helpers (byte 0 at bits 63..56) ---
inline uint8_t pfx_byte(uint64_t pfx, uint8_t i) noexcept {
    return static_cast<uint8_t>(pfx >> (56 - 8 * i));
}
inline uint64_t pack_prefix(const uint8_t* bytes, uint8_t len) noexcept {
    uint64_t v = 0;
    for (uint8_t i = 0; i < len; ++i)
        v |= uint64_t(bytes[i]) << (56 - 8 * i);
    return v;
}
// Extract bytes from packed u64 (for bitmask interface boundary)
inline void pfx_to_bytes(uint64_t pfx, uint8_t* out, uint8_t len) noexcept {
    for (uint8_t i = 0; i < len; ++i)
        out[i] = static_cast<uint8_t>(pfx >> (56 - 8 * i));
}

// --- NK type for a given remaining bit count ---
template<int BITS>
using nk_for_bits_t = std::conditional_t<(BITS > 32), uint64_t,
                      std::conditional_t<(BITS > 16), uint32_t,
                      std::conditional_t<(BITS > 8), uint16_t, uint8_t>>>;


// --- Tagged pointer helpers ---
// Bitmask ptr: points to bitmap (node+1), no LEAF_BIT. Use directly.
// Leaf ptr: points to header (node+0), has LEAF_BIT. Strip unconditionally.

inline uint64_t tag_leaf(const uint64_t* node) noexcept {
    return reinterpret_cast<uint64_t>(node) | LEAF_BIT;
}
inline uint64_t tag_bitmask(const uint64_t* node) noexcept {
    return reinterpret_cast<uint64_t>(node + 1);  // skip header, point at bitmap
}
inline const uint64_t* untag_leaf(uint64_t tagged) noexcept {
    return reinterpret_cast<const uint64_t*>(tagged ^ LEAF_BIT);
}
inline uint64_t* untag_leaf_mut(uint64_t tagged) noexcept {
    return reinterpret_cast<uint64_t*>(tagged ^ LEAF_BIT);
}
inline uint64_t* bm_to_node(uint64_t ptr) noexcept {
    return reinterpret_cast<uint64_t*>(ptr) - 1;  // back up from bitmap to header
}
inline const uint64_t* bm_to_node_const(uint64_t ptr) noexcept {
    return reinterpret_cast<const uint64_t*>(ptr) - 1;
}

// Dynamic header size: 1 (base) + 1 (if skip present)
inline size_t hdr_u64(const uint64_t* n) noexcept {
    return 1 + (get_header(n)->is_skip() ? 1 : 0);
}

// ==========================================================================
// Global sentinel -- zeroed block, valid as:
//   - Leaf with suffix_type=0, entries=0 -> bitmap_find returns nullptr
//   - Branchless miss target -> bitmap all zeros -> FAST_EXIT returns -1
// Must be large enough for safe bitmap read: header(2) + bitmap(4) = 6 u64s.
// ==========================================================================

// Sentinel: just the LEAF_BIT tag with no address.
// Any code entering a leaf path checks ptr == LEAF_BIT first.
static constexpr uint64_t SENTINEL_TAGGED = LEAF_BIT;

// ==========================================================================
// Tagged pointer entry counting (NK-independent)
// ==========================================================================

// ==========================================================================
// key_ops<KEY> -- internal key representation
//
// IK: uint32_t for KEY <= 32 bits, uint64_t otherwise.
// Key is left-aligned in IK. Top bits consumed first via shift.
// ==========================================================================

template<typename KEY>
struct key_ops {
    static_assert(std::is_integral_v<KEY>);

    static constexpr bool   IS_SIGNED = std::is_signed_v<KEY>;
    static constexpr int    KEY_BITS  = static_cast<int>(sizeof(KEY) * 8);

    using IK = std::conditional_t<sizeof(KEY) <= 4, uint32_t, uint64_t>;
    static constexpr int IK_BITS = static_cast<int>(sizeof(IK) * 8);

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

    static constexpr KEY to_key(IK ik) noexcept {
        ik >>= (IK_BITS - KEY_BITS);
        if constexpr (IS_SIGNED) ik ^= IK{1} << (KEY_BITS - 1);
        return static_cast<KEY>(ik);
    }
};

// ==========================================================================
// Iteration result (free struct, shared across all NK specializations)
// ==========================================================================

template<typename IK, typename VST>
struct iter_ops_result_t {
    IK key;
    const VST* value;
    bool found;
};

// ==========================================================================
// Bool slots — packed bit storage for VALUE=bool specialization
//
// Wraps a uint64_t* pointing into a node's packed bit region.
// Sentinels are private — all const bool* returns go through ptr/ptr_at.
// ==========================================================================

struct bool_slots {
private:
    static inline constexpr bool TRUE_VAL  = true;
    static inline constexpr bool FALSE_VAL = false;

public:
    uint64_t* data;

    static const bool* ptr(bool v) noexcept {
        return v ? &TRUE_VAL : &FALSE_VAL;
    }

    const bool* ptr_at(unsigned i) const noexcept {
        return ptr((data[i / 64] >> (i % 64)) & 1);
    }

    bool get(unsigned i) const noexcept {
        return (data[i / 64] >> (i % 64)) & 1;
    }

    void set(unsigned i, bool v) noexcept {
        unsigned word = i / 64, bit = i % 64;
        if (v) data[word] |=  (uint64_t{1} << bit);
        else   data[word] &= ~(uint64_t{1} << bit);
    }

    void clear_all(unsigned n) noexcept {
        std::memset(data, 0, u64_for(n) * 8);
    }

    void unpack_to(bool* dst, unsigned n) const noexcept {
        for (unsigned i = 0; i < n; ++i)
            dst[i] = get(i);
    }

    void pack_from(const bool* src, unsigned n) noexcept {
        clear_all(n);
        for (unsigned i = 0; i < n; ++i)
            if (src[i]) set(i, true);
    }

    // Shift bits [from, from+count) left by 1 (toward lower index).
    // Bit at 'from' is overwritten. Bit at from+count is unchanged.
    // Equivalent to memmove(vd + from - 1, vd + from, count) for 1-byte slots.
    void shift_left_1(unsigned from, unsigned count) noexcept {
        for (unsigned i = 0; i < count; ++i)
            set(from - 1 + i, get(from + i));
    }

    // Shift bits [from, from+count) right by 1 (toward higher index).
    // Bit at from+count is overwritten. Bit at 'from' is freed.
    // Equivalent to memmove(vd + from + 1, vd + from, count) for 1-byte slots.
    void shift_right_1(unsigned from, unsigned count) noexcept {
        for (unsigned i = count; i > 0; --i)
            set(from + i, get(from + i - 1));
    }

    // Fill bits [from, from+count) with value v.
    void fill_range(unsigned from, unsigned count, bool v) noexcept {
        for (unsigned i = 0; i < count; ++i)
            set(from + i, v);
    }

    static constexpr size_t u64_for(unsigned n) noexcept {
        return (n + 63) / 64;
    }

    static constexpr size_t bytes_for(unsigned n) noexcept {
        return u64_for(n) * 8;
    }
};

// ==========================================================================
// Value traits
// ==========================================================================

template<typename VALUE, typename ALLOC>
struct value_traits {
    // Two categories:
    //   A: trivially_copyable && sizeof <= 64  → inline, memcpy-safe, no dtor
    //   C: else                                → pointer, has dtor+dealloc
    static constexpr bool IS_TRIVIAL =
        std::is_trivially_copyable_v<VALUE> && sizeof(VALUE) <= 8;
    static constexpr bool IS_INLINE = IS_TRIVIAL;
    static constexpr bool HAS_DESTRUCTOR = !IS_TRIVIAL;
    static constexpr bool IS_BOOL = std::is_same_v<VALUE, bool>;

    using slot_type = std::conditional_t<IS_INLINE, VALUE, VALUE*>;

    // --- store: VALUE → slot_type (for insert) ---

    static slot_type store(const VALUE& val, ALLOC& alloc) {
        if constexpr (IS_TRIVIAL) {
            return val;
        } else {
            using VA = typename std::allocator_traits<ALLOC>::template rebind_alloc<VALUE>;
            VA va(alloc);
            VALUE* p = std::allocator_traits<VA>::allocate(va, 1);
            std::allocator_traits<VA>::construct(va, p, val);
            return p;
        }
    }

    // --- as_ptr: slot_type → const VALUE* ---

    static const VALUE* as_ptr(const slot_type& s) noexcept {
        if constexpr (IS_BOOL)        return bool_slots::ptr(s);
        else if constexpr (IS_TRIVIAL) return reinterpret_cast<const VALUE*>(&s);
        else                           return s;
    }

    // --- destroy: release resources held by slot ---
    //   A: noop.  C: call destructor + deallocate.

    static void destroy(slot_type& s, ALLOC& alloc) noexcept {
        if constexpr (!IS_TRIVIAL) {
            using VA = typename std::allocator_traits<ALLOC>::template rebind_alloc<VALUE>;
            VA va(alloc);
            std::allocator_traits<VA>::destroy(va, s);
            std::allocator_traits<VA>::deallocate(va, s, 1);
        }
    }

    // --- init_slot: write into UNINITIALIZED destination ---
    // Both A and C are pointer-sized or trivial — always memcpy.

    static void init_slot(slot_type* dst, const slot_type& val) {
        std::memcpy(dst, &val, sizeof(slot_type));
    }

    static void init_slot(slot_type* dst, slot_type&& val) {
        std::memcpy(dst, &val, sizeof(slot_type));
    }

    // --- write_slot: write into INITIALIZED (live or moved-from) destination ---

    static void write_slot(slot_type* dst, const slot_type& src) noexcept {
        std::memcpy(dst, &src, sizeof(slot_type));
    }

    static void write_slot(slot_type* dst, slot_type&& src) noexcept {
        std::memcpy(dst, &src, sizeof(slot_type));
    }

    // --- open_gap: shift right, create uninit hole at pos ---

    static void open_gap(slot_type* vd, size_t count, size_t pos) {
        std::memmove(vd + pos + 1, vd + pos,
                     (count - pos) * sizeof(slot_type));
    }

    // --- close_gap: remove element at pos, shift left ---
    // For C: caller MUST VT::destroy(vd[pos]) first (dealloc pointer).
    // After: count-1 live elements.

    static void close_gap(slot_type* vd, size_t count, size_t pos) {
        std::memmove(vd + pos, vd + pos + 1,
                     (count - 1 - pos) * sizeof(slot_type));
    }

    // --- copy_uninit: copy n slots to UNINIT destination, no overlap ---

    static void copy_uninit(const slot_type* src, size_t n, slot_type* dst) {
        std::memcpy(dst, src, n * sizeof(slot_type));
    }

    // --- move_uninit: move n slots to UNINIT destination, no overlap ---

    static void move_uninit(slot_type* src, size_t n, slot_type* dst) {
        std::memcpy(dst, src, n * sizeof(slot_type));
    }
};

// ==========================================================================
// Builder — owns allocator, handles node + value alloc/dealloc
// ==========================================================================

// Base builder: trivial values (A-type, bool) — bump-from-mega slab allocator
//
// Two-level allocation:
//   mega (growing) → blocks (bin-sized, bump or freelist)
//
// Hot path: pop from bins_v[b].
// Cold path: bump mega cursor by bin size.
// Colder:    allocate new mega when current is exhausted.
template<typename VALUE, bool IS_TRIVIAL, typename ALLOC>
struct builder;

template<typename VALUE, typename ALLOC>
struct builder<VALUE, true, ALLOC> {
    ALLOC alloc_v;

    builder() : alloc_v() {}
    explicit builder(const ALLOC& a) : alloc_v(a) {}

    builder(const builder&) = delete;
    builder& operator=(const builder&) = delete;

    builder(builder&& o) noexcept : alloc_v(std::move(o.alloc_v)) {}

    builder& operator=(builder&& o) noexcept {
        if (this != &o)
            alloc_v = std::move(o.alloc_v);
        return *this;
    }

    void swap(builder& o) noexcept {
        using std::swap;
        swap(alloc_v, o.alloc_v);
    }

    const ALLOC& get_allocator() const noexcept { return alloc_v; }

    // --- Allocate a node ---
    // pad=true: round up for in-place growth (bitmask nodes)
    // pad=false: exact allocation (compact leaves, VALUE*)
    uint64_t* alloc_node(size_t& u64_count, bool pad = true) {
        size_t actual = pad ? round_up_u64(u64_count) : u64_count;
        uint64_t* p = alloc_v.allocate(actual);
        std::memset(p, 0, actual * 8);
        u64_count = actual;
        return p;
    }

    // --- Return a node ---
    void dealloc_node(uint64_t* p, size_t u64_count) noexcept {
        alloc_v.deallocate(p, u64_count);
    }

    // --- drain: no-op, tree destructor frees nodes individually ---
    void drain() noexcept {}

    using VT = value_traits<VALUE, ALLOC>;
    using slot_type = typename VT::slot_type;

    slot_type store_value(const VALUE& val) { return val; }
    void destroy_value(slot_type&) noexcept {}
};

// Extended builder: C-type values — routes through base_v freelist bins
template<typename VALUE, typename ALLOC>
struct builder<VALUE, false, ALLOC> {
    using BASE = builder<VALUE, true, ALLOC>;
    using VA = typename std::allocator_traits<ALLOC>::template rebind_alloc<VALUE>;
    using VT = value_traits<VALUE, ALLOC>;
    using slot_type = typename VT::slot_type;  // VALUE*

    static constexpr size_t VAL_U64 = (sizeof(VALUE) + 7) / 8;

    BASE base_v;

    builder() = default;
    explicit builder(const ALLOC& a) : base_v(a) {}

    builder(const builder&) = delete;
    builder& operator=(const builder&) = delete;

    builder(builder&& o) noexcept
        : base_v(std::move(o.base_v)) {}

    builder& operator=(builder&& o) noexcept {
        if (this != &o) {
            base_v = std::move(o.base_v);
        }
        return *this;
    }

    ~builder() = default;

    void swap(builder& o) noexcept {
        base_v.swap(o.base_v);
    }

    const ALLOC& get_allocator() const noexcept { return base_v.get_allocator(); }

    uint64_t* alloc_node(size_t& u64_count, bool pad = true) { return base_v.alloc_node(u64_count, pad); }
    void dealloc_node(uint64_t* p, size_t u64_count) noexcept { base_v.dealloc_node(p, u64_count); }

    slot_type store_value(const VALUE& val) {
        if constexpr (VAL_U64 <= FREE_MAX) {
            size_t sz = VAL_U64;
            uint64_t* p = base_v.alloc_node(sz, false);
            std::construct_at(reinterpret_cast<VALUE*>(p), val);
            return reinterpret_cast<VALUE*>(p);
        } else {
            VA va(base_v.get_allocator());
            VALUE* p = std::allocator_traits<VA>::allocate(va, 1);
            std::construct_at(p, val);
            return p;
        }
    }

    void destroy_value(slot_type& s) noexcept {
        std::destroy_at(s);
        if constexpr (VAL_U64 <= FREE_MAX) {
            base_v.dealloc_node(reinterpret_cast<uint64_t*>(s), VAL_U64);
        } else {
            VA va(base_v.get_allocator());
            std::allocator_traits<VA>::deallocate(va, s, 1);
        }
    }

    void drain() noexcept {
        base_v.drain();
    }
};

// ==========================================================================
// Result types
// ==========================================================================

struct insert_result_t {
    uint64_t tagged_ptr;    // tagged pointer (LEAF_BIT for leaf, raw for bitmask)
    bool inserted;
    bool needs_split;
};

struct erase_result_t {
    uint64_t tagged_ptr;    // tagged pointer, or 0 if fully erased
    bool erased;
    uint64_t subtree_entries;  // remaining entries in subtree (exact)
};

} // namespace gteitelbaum

#endif // KNTRIE_SUPPORT_HPP
