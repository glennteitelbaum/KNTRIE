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
//   [0]      flags       (bit 0: is_bitmask, bit 1: is_skip)
//   [1]      suffix_type (leaf only: 0=bitmap256, 1=u16, 2=u32, 3=u64)
//   [2..3]   entries     (uint16_t)
//   [4..5]   alloc_u64   (uint16_t)
//   [6..7]   pad
//
// Zeroed header -> is_leaf=true, is_skip=false, suffix_type=0,
//                  entries=0. Sentinel-safe.
//
// Skip data lives in node[1] (header2, only when is_skip=1):
//   bytes [0..5]  prefix[6]   -- skip prefix chunks (outer first)
//   byte  [6]     pad
//   byte  [7]     skip_len    -- number of valid prefix bytes (1-6)
//
// skip()/prefix_bytes()/set_skip()/set_prefix() access node[1]
// via this-pointer arithmetic. Only valid through get_header(node)->
// NOT on value copies.
// ==========================================================================

struct node_header {
    uint8_t  flags_       = 0;
    uint8_t  suffix_type_ = 0;
    uint16_t entries_     = 0;
    uint16_t alloc_u64_   = 0;
    uint16_t pad_         = 0;

    static constexpr uint8_t BITMASK_BIT = 1 << 0;
    static constexpr uint8_t SKIP_BIT    = 1 << 1;

    // --- header1 accessors (safe on value copies) ---

    bool is_leaf()    const noexcept { return !(flags_ & BITMASK_BIT); }
    bool is_skip()    const noexcept { return flags_ & SKIP_BIT; }
    void set_bitmask()      noexcept { flags_ |= BITMASK_BIT; }

    uint8_t suffix_type() const noexcept { return suffix_type_; }
    void set_suffix_type(uint8_t t) noexcept { suffix_type_ = t; }

    unsigned entries()   const noexcept { return entries_; }
    void set_entries(unsigned n) noexcept { entries_ = static_cast<uint16_t>(n); }

    unsigned alloc_u64() const noexcept { return alloc_u64_; }
    void set_alloc_u64(unsigned n) noexcept { alloc_u64_ = static_cast<uint16_t>(n); }

    // --- header2 accessors (node[1]) -- only valid via get_header(node)-> ---

    uint8_t skip() const noexcept {
        if (!(flags_ & SKIP_BIT)) return 0;
        auto* p = reinterpret_cast<const uint64_t*>(this);
        return reinterpret_cast<const uint8_t*>(p + 1)[7];
    }

    const uint8_t* prefix_bytes() const noexcept {
        auto* p = reinterpret_cast<const uint64_t*>(this);
        return reinterpret_cast<const uint8_t*>(p + 1);
    }

    void set_skip(uint8_t s) noexcept {
        if (s > 0) {
            flags_ |= SKIP_BIT;
            auto* p = reinterpret_cast<uint64_t*>(this);
            reinterpret_cast<uint8_t*>(p + 1)[7] = s;
        } else {
            flags_ &= ~SKIP_BIT;
        }
    }

    void set_prefix(const uint8_t* pfx, uint8_t len) noexcept {
        auto* p = reinterpret_cast<uint64_t*>(this);
        uint8_t* dst = reinterpret_cast<uint8_t*>(p + 1);
        for (uint8_t i = 0; i < len; ++i) dst[i] = pfx[i];
    }
};
static_assert(sizeof(node_header) == 8);

inline node_header*       get_header(uint64_t* n)       noexcept { return reinterpret_cast<node_header*>(n); }
inline const node_header* get_header(const uint64_t* n) noexcept { return reinterpret_cast<const node_header*>(n); }

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

alignas(64) inline constinit uint64_t SENTINEL_NODE[6] = {};

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
    uint64_t* node;
    bool inserted;
    bool needs_split;
};

struct erase_result_t {
    uint64_t* node;
    bool erased;
};

} // namespace gteitelbaum

#endif // KNTRIE_SUPPORT_HPP
