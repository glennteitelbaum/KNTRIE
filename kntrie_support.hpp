#ifndef KNTRIE_SUPPORT_HPP
#define KNTRIE_SUPPORT_HPP

#include <cstdint>
#include <cstring>
#include <bit>
#include <memory>
#include <type_traits>
#include <utility>
#include <algorithm>
#include <cassert>

namespace gteitelbaum {

// ==========================================================================
// Constants
// ==========================================================================

inline constexpr size_t BITMAP256_U64 = 4;   // 32 bytes
inline constexpr size_t COMPACT_MAX   = 4096;
inline constexpr size_t BOT_LEAF_MAX  = 4096;
inline constexpr size_t HEADER_U64    = 1;   // base header is 1 u64 (8 bytes), +1 if skip

// u64s needed for N child descriptor entries (uint16_t each)
inline constexpr size_t desc_u64(size_t n) noexcept { return (n + 3) / 4; }

// Tagged pointer: bit 63 = leaf (sign bit for fast test)
static constexpr uint64_t LEAF_BIT = uint64_t(1) << 63;

// ==========================================================================
// Allocation size classes
//
// Up to 8 u64s: exact
// Then quarter-steps within each power-of-2 range:
//   [9,16]:   step 2  -> 10,12,14,16
//   [17,32]:  step 4  -> 20,24,28,32
//   [33,64]:  step 8  -> 40,48,56,64
//   [65,128]: step 16 -> 80,96,112,128
//   etc.
//
// Worst-case waste: ~25%.  Enables in-place insert/erase.
// ==========================================================================

// ==========================================================================
// Allocation size classes (bitmask nodes only)
//
// Compact leaves use power-of-2 slot counts with exact allocation.
// Bitmask nodes use these size classes:
//   Up to 12 u64s: step 4 -> 4,8,12
//   Then powers-of-2 with midpoints (+2 for header):
//     16, 26, 32, 50, 64, 98, 128, 194, ...
//   Max waste: ~33%.
// ==========================================================================

inline constexpr size_t round_up_u64(size_t n) noexcept {
    if (n < 12) return ((n + 3) / 4) * 4;
    int bit  = static_cast<int>(std::bit_width(n - 1));
    size_t pow2 = size_t{1} << bit;
    size_t mid  = pow2 / 2 + pow2 / 4 + 2;
    return (n <= mid) ? mid : pow2;
}

// Shrink when allocated exceeds the class for 2x the needed size.
// Bitmask nodes only — compact leaves use power-of-2 shrink logic.
inline constexpr bool should_shrink_u64(size_t allocated, size_t needed) noexcept {
    return allocated > round_up_u64(needed * 2);
}

// ==========================================================================
// Node Header  (8 bytes = 1 u64)
//
// Struct layout (little-endian):
//   [0]      flags       (bit 0: is_bitmask, bits 1-3: skip count 0-7)
//   [1]      suffix_type (leaf only: 0=bitmap256, 1=u16, 2=u32, 3=u64)
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

struct node_header {
    uint8_t  flags_       = 0;
    uint8_t  suffix_type_ = 0;
    uint16_t entries_     = 0;
    uint16_t alloc_u64_   = 0;
    uint16_t total_slots_ = 0;

    static constexpr uint8_t BITMASK_BIT = 1 << 0;

    // --- type ---
    bool is_leaf()    const noexcept { return !(flags_ & BITMASK_BIT); }
    void set_bitmask()      noexcept { flags_ |= BITMASK_BIT; }

    // --- skip (bits 1-3 of flags_) ---
    uint8_t skip()    const noexcept { return (flags_ >> 1) & 0x07; }
    bool    is_skip() const noexcept { return flags_ & 0x0E; }
    void set_skip(uint8_t s) noexcept { flags_ = (flags_ & 0x01) | ((s & 0x07) << 1); }

    // --- leaf prefix bytes (in node[1], only valid via get_header(node)->) ---
    const uint8_t* prefix_bytes() const noexcept {
        auto* p = reinterpret_cast<const uint64_t*>(this);
        return reinterpret_cast<const uint8_t*>(p + 1);
    }
    void set_prefix(const uint8_t* pfx, uint8_t len) noexcept {
        auto* p = reinterpret_cast<uint64_t*>(this);
        uint8_t* dst = reinterpret_cast<uint8_t*>(p + 1);
        for (uint8_t i = 0; i < len; ++i) dst[i] = pfx[i];
    }

    // --- entries / alloc ---
    uint8_t suffix_type() const noexcept { return suffix_type_; }
    void set_suffix_type(uint8_t t) noexcept { suffix_type_ = t; }

    unsigned entries()   const noexcept { return entries_; }
    void set_entries(unsigned n) noexcept { entries_ = static_cast<uint16_t>(n); }

    unsigned alloc_u64() const noexcept { return alloc_u64_; }
    void set_alloc_u64(unsigned n) noexcept { alloc_u64_ = static_cast<uint16_t>(n); }

    unsigned total_slots() const noexcept { return total_slots_; }
    void set_total_slots(unsigned n) noexcept { total_slots_ = static_cast<uint16_t>(n); }

    // Bitmask-only: total_slots_ repurposed as descendant count (saturating at 0xFFFF)
    uint16_t descendants() const noexcept { return total_slots_; }
    void set_descendants(uint16_t n) noexcept { total_slots_ = n; }
};
static_assert(sizeof(node_header) == 8);

inline node_header*       get_header(uint64_t* n)       noexcept { return reinterpret_cast<node_header*>(n); }
inline const node_header* get_header(const uint64_t* n) noexcept { return reinterpret_cast<const node_header*>(n); }

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

alignas(64) inline constinit uint64_t SENTINEL_NODE[8] = {};

// Tagged sentinel: SENTINEL_NODE with LEAF_BIT set (valid zeroed leaf)
inline const uint64_t SENTINEL_TAGGED =
    reinterpret_cast<uint64_t>(&SENTINEL_NODE[0]) | LEAF_BIT;

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
// Suffix type helpers
//
// suffix_type: 0=bitmap256 (<=8 bits), 1=u16, 2=u32, 3=u64.
// ==========================================================================

inline constexpr uint8_t suffix_type_for(int bits) noexcept {
    if (bits <= 8)  return 0;  // bitmap256
    if (bits <= 16) return 1;  // u16
    if (bits <= 32) return 2;  // u32
    return 3;                  // u64
}

// ==========================================================================
// Value traits
// ==========================================================================

template<typename VALUE, typename ALLOC>
struct value_traits {
    static constexpr bool IS_INLINE =
        sizeof(VALUE) <= 8 && std::is_trivially_copyable_v<VALUE>;

    using slot_type = std::conditional_t<IS_INLINE, VALUE, VALUE*>;

    static slot_type store(const VALUE& val, ALLOC& alloc) {
        if constexpr (IS_INLINE) {
            return val;
        } else {
            using VA = typename std::allocator_traits<ALLOC>::template rebind_alloc<VALUE>;
            VA va(alloc);
            VALUE* p = std::allocator_traits<VA>::allocate(va, 1);
            std::allocator_traits<VA>::construct(va, p, val);
            return p;
        }
    }

    static const VALUE* as_ptr(const slot_type& s) noexcept {
        if constexpr (IS_INLINE) return reinterpret_cast<const VALUE*>(&s);
        else                     return s;
    }

    static void destroy(slot_type s, ALLOC& alloc) noexcept {
        if constexpr (!IS_INLINE) {
            using VA = typename std::allocator_traits<ALLOC>::template rebind_alloc<VALUE>;
            VA va(alloc);
            std::allocator_traits<VA>::destroy(va, s);
            std::allocator_traits<VA>::deallocate(va, s, 1);
        }
    }

    static void write_slot(slot_type* dest, const slot_type& src) noexcept {
        std::memcpy(dest, &src, sizeof(slot_type));
    }
};

// ==========================================================================
// Allocation helpers
// ==========================================================================

template<typename ALLOC>
inline uint64_t* alloc_node(ALLOC& a, size_t u64_count) {
    uint64_t* p = a.allocate(u64_count);
    std::memset(p, 0, u64_count * 8);
    return p;
}

template<typename ALLOC>
inline void dealloc_node(ALLOC& a, uint64_t* p, size_t u64_count) noexcept {
    a.deallocate(p, u64_count);
}

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
    uint16_t subtree_entries;  // remaining entries in subtree (capped at COMPACT_MAX+1)
};

} // namespace gteitelbaum

#endif // KNTRIE_SUPPORT_HPP
