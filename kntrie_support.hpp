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
#include <array>

namespace gteitelbaum {

// ==========================================================================
// Constants
// ==========================================================================

inline constexpr size_t BITMAP256_U64 = 4;   // 32 bytes
inline constexpr size_t COMPACT_MAX   = 4096;
inline constexpr size_t BOT_LEAF_MAX  = 4096;
inline constexpr size_t HEADER_U64    = 2;   // header is 2 u64 (16 bytes)

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

// Size classes: exact up to 8, then powers-of-2 with midpoints.
// 1..8, 12,16, 24,32, 48,64, 96,128, 192,256, ...
// Max waste: 33%.

inline constexpr size_t round_up_u64(size_t n) noexcept {
    if (n < 12) return ((n + 3) / 4) * 4;
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
// Node Header  (16 bytes = 2 u64)
//
// flags_:      [15] is_bitmask (0=leaf, 1=bitmask node)
//              [1:0] suffix_type (leaf only: 0=bitmap256, 1=u16, 2=u32, 3=u64)
// entries_:    entry count
// alloc_u64_:  allocation size in u64s
// skip_len_:   number of u8 prefix chunks (0-6)
// prefix_[6]:  u8 skip chunks (outer first)
// pad_[3]:     reserved (zeroed)
//
// Zeroed header -> is_leaf=true, suffix_type=0, entries=0. Sentinel-safe.
// ==========================================================================

struct node_header {
    uint16_t flags_;
    uint16_t entries_;
    uint16_t alloc_u64_;
    uint8_t  skip_len_;
    uint8_t  prefix_[6];
    uint8_t  pad_[3];

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

inline node_header*       get_header(uint64_t* n)       noexcept { return reinterpret_cast<node_header*>(n); }
inline const node_header* get_header(const uint64_t* n) noexcept { return reinterpret_cast<const node_header*>(n); }

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
// slot_table -- constexpr lookup: max total slots for a given alloc_u64
//
// Templated on K (suffix type) and VST (value slot type).
// Indexed directly by alloc_u64. table[0..HEADER_U64] = 0.
// ==========================================================================

template<typename K, typename VST>
struct slot_table {
    static constexpr size_t MAX_ALLOC = 16384;

    static constexpr auto build() {
        std::array<uint16_t, MAX_ALLOC + 1> tbl{};
        for (size_t i = 0; i <= HEADER_U64; ++i) tbl[i] = 0;
        for (size_t au64 = HEADER_U64 + 1; au64 <= MAX_ALLOC; ++au64) {
            size_t avail = (au64 - HEADER_U64) * 8;
            size_t total = avail / (sizeof(K) + sizeof(VST));
            while (total > 0) {
                size_t kb = (total * sizeof(K) + 7) & ~size_t{7};
                size_t vb = (total * sizeof(VST) + 7) & ~size_t{7};
                if (kb + vb <= avail) break;
                --total;
            }
            tbl[au64] = static_cast<uint16_t>(total);
        }
        return tbl;
    }

    static constexpr auto table_ = build();

    static constexpr uint16_t max_slots(size_t alloc_u64) noexcept {
        return table_[alloc_u64];
    }
};

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
