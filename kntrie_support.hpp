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

namespace kn3 {

// ==========================================================================
// Constants
// ==========================================================================

inline constexpr size_t BITMAP256_U64 = 4;   // 32 bytes
inline constexpr size_t COMPACT_MAX   = 4096;
inline constexpr size_t BOT_LEAF_MAX  = 4096;

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
// Node Header  (8 bytes; prefix in node[1] when skip > 0)
//
// flags_ layout:
//   bit 0:     is_bitmask (0 = compact leaf, 1 = bitmask/split)
//   bits 2-3:  skip (0-3)
//   bits 4-15: reserved
//
// Zeroed header -> compact leaf with entries=0 (sentinel-compatible)
// ==========================================================================

struct NodeHeader {
    uint16_t entries;      // compact: k/v count; bitmask: child count
    uint16_t descendants;  // total k/v pairs in subtree, capped at DESC_CAP
    uint16_t alloc_u64;    // allocation size in u64s (may be padded)
    uint16_t flags_;

    bool is_leaf() const noexcept { return !(flags_ & 1); }
    void set_bitmask() noexcept { flags_ |= 1; }

    uint8_t skip() const noexcept { return (flags_ >> 2) & 0x3; }
    void set_skip(uint8_t s) noexcept {
        flags_ = (flags_ & ~uint16_t(0x000C)) | (uint16_t(s & 0x3) << 2);
    }

    static constexpr uint16_t DESC_CAP = 65535;

    void add_descendants(uint16_t n) noexcept {
        uint32_t d = static_cast<uint32_t>(descendants) + n;
        descendants = static_cast<uint16_t>(d < DESC_CAP ? d : DESC_CAP);
    }
    void sub_descendants(uint16_t n) noexcept {
        if (descendants < DESC_CAP)
            descendants = descendants >= n ? static_cast<uint16_t>(descendants - n) : 0;
    }
};
static_assert(sizeof(NodeHeader) == 8);

inline NodeHeader*       get_header(uint64_t* n)       noexcept { return reinterpret_cast<NodeHeader*>(n); }
inline const NodeHeader* get_header(const uint64_t* n) noexcept { return reinterpret_cast<const NodeHeader*>(n); }

inline uint64_t  get_prefix(const uint64_t* n) noexcept { return n[1]; }
inline void      set_prefix(uint64_t* n, uint64_t p)   noexcept { n[1] = p; }

inline constexpr size_t header_u64(uint8_t skip) noexcept { return skip > 0 ? 2 : 1; }

// ==========================================================================
// Global sentinel -- zeroed block, valid as:
//   - Compact leaf (entries=0, flags_=0 -> is_leaf)
//   - Bot-leaf BITS==16 (header entries=0, bitmap all zeros)
// No allocation on construction; never deallocated.
// ==========================================================================

alignas(64) inline constinit uint64_t SENTINEL_NODE[8] = {};

// ==========================================================================
// Suffix traits
// ==========================================================================

template<int BITS>
struct suffix_traits {
    using type = std::conditional_t<BITS <= 8,  uint8_t,
                 std::conditional_t<BITS <= 16, uint16_t,
                 std::conditional_t<BITS <= 32, uint32_t, uint64_t>>>;
    static constexpr size_t size = sizeof(type);
};

// ==========================================================================
// Key operations
// ==========================================================================

template<typename KEY>
struct KeyOps {
    static_assert(std::is_integral_v<KEY>);

    static constexpr bool   is_signed = std::is_signed_v<KEY>;
    static constexpr size_t key_bits  = sizeof(KEY) * 8;

    static constexpr uint64_t to_internal(KEY k) noexcept {
        uint64_t r;
        if constexpr (sizeof(KEY) == 1)      r = static_cast<uint8_t>(k);
        else if constexpr (sizeof(KEY) == 2) r = static_cast<uint16_t>(k);
        else if constexpr (sizeof(KEY) == 4) r = static_cast<uint32_t>(k);
        else                                 r = static_cast<uint64_t>(k);
        if constexpr (is_signed) r ^= 1ULL << (key_bits - 1);
        r <<= (64 - key_bits);
        return r;
    }

    static constexpr KEY to_key(uint64_t ik) noexcept {
        ik >>= (64 - key_bits);
        if constexpr (is_signed) ik ^= 1ULL << (key_bits - 1);
        return static_cast<KEY>(ik);
    }

    template<int BITS>
    static constexpr uint8_t extract_top8(uint64_t ik) noexcept {
        static_assert(BITS >= 8 && BITS <= 64);
        if constexpr (BITS > static_cast<int>(key_bits)) return 0;
        else {
            constexpr int shift = 56 - static_cast<int>(key_bits) + BITS;
            return static_cast<uint8_t>((ik >> shift) & 0xFF);
        }
    }

    template<int BITS>
    static constexpr uint16_t extract_top16(uint64_t ik) noexcept {
        static_assert(BITS >= 16 && BITS <= 64);
        constexpr int shift = 64 - static_cast<int>(key_bits) + BITS - 16;
        return static_cast<uint16_t>((ik >> shift) & 0xFFFF);
    }

    template<int BITS>
    static constexpr uint64_t extract_suffix(uint64_t ik) noexcept {
        if constexpr (BITS >= 64) return ik;
        else if constexpr (BITS > static_cast<int>(key_bits)) return 0;
        else {
            constexpr int shift = 64 - static_cast<int>(key_bits);
            constexpr uint64_t mask = (1ULL << BITS) - 1;
            return (ik >> shift) & mask;
        }
    }

    template<int BITS>
    static constexpr uint64_t extract_prefix(uint64_t ik, int skip) noexcept {
        if constexpr (BITS > static_cast<int>(key_bits)) return 0;
        else {
            int prefix_bits = skip * 16;
            int shift = 64 - static_cast<int>(key_bits) + BITS - prefix_bits;
            uint64_t mask = (1ULL << prefix_bits) - 1;
            return (ik >> shift) & mask;
        }
    }

    static constexpr uint16_t get_skip_chunk(uint64_t prefix, int /*skip*/, int skip_left) noexcept {
        return static_cast<uint16_t>((prefix >> ((skip_left - 1) * 16)) & 0xFFFF);
    }
};

// ==========================================================================
// Value traits
// ==========================================================================

template<typename VALUE, typename ALLOC>
struct ValueTraits {
    static constexpr bool is_inline =
        sizeof(VALUE) <= 8 && std::is_trivially_copyable_v<VALUE>;

    using slot_type = std::conditional_t<is_inline, VALUE, VALUE*>;

    static slot_type store(const VALUE& val, ALLOC& alloc) {
        if constexpr (is_inline) {
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
        if constexpr (is_inline) return reinterpret_cast<const VALUE*>(&s);
        else                     return s;
    }

    static void destroy(slot_type s, ALLOC& alloc) noexcept {
        if constexpr (!is_inline) {
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
// Insert / Erase result types
// ==========================================================================

struct InsertResult {
    uint64_t* node;
    bool      inserted;
};

struct EraseResult {
    uint64_t* node;
    bool erased;
};

} // namespace kn3

#endif // KNTRIE_SUPPORT_HPP
