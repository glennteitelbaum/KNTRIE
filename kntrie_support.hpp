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
inline constexpr size_t HEADER_U64    = 1;   // header is 1 u64

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

inline constexpr size_t round_up_u64(size_t n) noexcept {
    if (n <= 8) return n;
    int bits = std::bit_width(n - 1);     // ceil(log2(n))
    int k    = bits - 1;                   // lower bound exponent
    if (k < 2) return n;
    size_t step = size_t{1} << (k - 2);   // quarter of the range
    return ((n + step - 1) / step) * step;
}

// Step up `steps` size classes from `cls` (for shrink hysteresis).
inline constexpr size_t step_up_u64(size_t cls, int steps) noexcept {
    size_t n = cls;
    for (int i = 0; i < steps; ++i) {
        if (n < 8) { ++n; continue; }
        int bits = std::bit_width(n);
        int k    = bits - 1;
        size_t step = size_t{1} << (k >= 2 ? k - 2 : 0);
        n += step;
    }
    return n;
}

// Shrink when allocated is more than 2 size-class steps above needed.
inline constexpr bool should_shrink_u64(size_t allocated, size_t needed) noexcept {
    size_t threshold = step_up_u64(round_up_u64(needed), 2);
    return allocated > threshold;
}

// ==========================================================================
// Prefix type — two 16-bit skip chunks
//
// prefix[0] = outer chunk (consumed first during descent)
// prefix[1] = inner chunk
// Unused slots = 0.
// ==========================================================================

using prefix_t = std::array<uint16_t, 2>;

// ==========================================================================
// Node Header  (8 bytes = 1 u64)
//
// top_:    bit 15 = is_bitmask (0=compact leaf, 1=split/fan/bitmap256)
//          bit 14-13 = suffix_type (compact leaf only: 00=reserved, 01=u16, 10=u32, 11=u64)
//          bit 12-0  = entries (0-8191)
// bottom_: bit 15-14 = skip (0-2)
//          bit 13-0  = alloc_u64 (0-16383)
// prefix_: two u16 skip chunks (0 when skip==0)
//
// Zeroed header → compact leaf, suffix_type=0, entries=0. Sentinel-safe.
// ==========================================================================

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
static_assert(sizeof(node_header) == 8);

inline node_header*       get_header(uint64_t* n)       noexcept { return reinterpret_cast<node_header*>(n); }
inline const node_header* get_header(const uint64_t* n) noexcept { return reinterpret_cast<const node_header*>(n); }

// ==========================================================================
// Global sentinel -- zeroed block, valid as:
//   - Compact leaf (entries=0, top_=0 → is_leaf, suffix_type=0)
//   - Bitmap256 leaf (entries=0, bitmap all zeros)
// No allocation on construction; never deallocated.
// ==========================================================================

alignas(64) inline constinit uint64_t SENTINEL_NODE[4] = {};

// ==========================================================================
// key_ops<KEY> — internal key representation
//
// internal_key_t: uint32_t for KEY ≤ 32 bits, uint64_t otherwise.
// Key is left-aligned in internal_key_t. Top bits consumed first via shift.
// ==========================================================================

template<typename KEY>
struct key_ops {
    static_assert(std::is_integral_v<KEY>);

    static constexpr bool   IS_SIGNED = std::is_signed_v<KEY>;
    static constexpr size_t KEY_BITS  = sizeof(KEY) * 8;

    using internal_key_t = std::conditional_t<sizeof(KEY) <= 4, uint32_t, uint64_t>;
    static constexpr int IK_BITS = sizeof(internal_key_t) * 8;

    static constexpr internal_key_t to_internal(KEY k) noexcept {
        internal_key_t r;
        if constexpr (sizeof(KEY) == 1)      r = static_cast<uint8_t>(k);
        else if constexpr (sizeof(KEY) == 2) r = static_cast<uint16_t>(k);
        else if constexpr (sizeof(KEY) == 4) r = static_cast<uint32_t>(k);
        else                                 r = static_cast<uint64_t>(k);
        if constexpr (IS_SIGNED) r ^= internal_key_t{1} << (KEY_BITS - 1);
        r <<= (IK_BITS - KEY_BITS);
        return r;
    }

    static constexpr KEY to_key(internal_key_t ik) noexcept {
        ik >>= (IK_BITS - KEY_BITS);
        if constexpr (IS_SIGNED) ik ^= internal_key_t{1} << (KEY_BITS - 1);
        return static_cast<KEY>(ik);
    }
};

// ==========================================================================
// Suffix type helpers
//
// Only for compact leaves. When ≤8 bits remain, bitmap256 is used instead.
// suffix_type: 1=u16, 2=u32, 3=u64. 0 is reserved (sentinel only).
// ==========================================================================

inline constexpr uint8_t suffix_type_for(int bits) noexcept {
    if (bits <= 16) return 1;  // u16
    if (bits <= 32) return 2;  // u32
    return 3;                  // u64
}

// Dispatch on suffix_type via nested bit tests. u16 is most common (fallthrough).
template<typename Fn>
inline auto dispatch_suffix(uint8_t stype, Fn&& fn) {
    if (stype & 0b10) {
        if (stype & 0b01) return fn(uint64_t{});
        else              return fn(uint32_t{});
    }
    return fn(uint16_t{});
}

// ==========================================================================
// slot_table -- constexpr lookup: max total slots for a given alloc_u64
//
// Templated on K (suffix type) and VST (value slot type).
// Indexed directly by alloc_u64. table[0] = table[1] = 0.
// Total slots = entries + dups. Dups are derived: total - entries.
// ==========================================================================

template<typename K, typename VST>
struct slot_table {
    static constexpr size_t MAX_ALLOC = 10240;

    static constexpr auto build() {
        std::array<uint16_t, MAX_ALLOC + 1> tbl{};
        tbl[0] = 0; tbl[1] = 0;
        for (size_t au64 = 2; au64 <= MAX_ALLOC; ++au64) {
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
// Erase result type
// ==========================================================================

struct erase_result_t {
    uint64_t* node;
    bool erased;
};

} // namespace gteitelbaum

#endif // KNTRIE_SUPPORT_HPP
