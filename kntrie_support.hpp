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
// Node Header  (8 bytes; prefix in node[1] when skip > 0)
//
// flags bit 0: is_internal  (0 = leaf/compact, 1 = internal)
// flags bit 1: is_split     (0 = compact,      1 = split/bitmask)
//
// Zeroed header → leaf compact with count=0 (sentinel-compatible)
// ==========================================================================

struct NodeHeader {
    uint32_t count;
    uint16_t top_count;
    uint8_t  skip;
    uint8_t  flags;

    bool is_internal() const noexcept { return flags & 1; }
    bool is_split()    const noexcept { return flags & 2; }
    void set_internal(bool v) noexcept { flags = (flags & ~1) | (v ? 1 : 0); }
    void set_split(bool v)    noexcept { flags = (flags & ~2) | (v ? 2 : 0); }
};
static_assert(sizeof(NodeHeader) == 8);

inline NodeHeader*       get_header(uint64_t* n)       noexcept { return reinterpret_cast<NodeHeader*>(n); }
inline const NodeHeader* get_header(const uint64_t* n) noexcept { return reinterpret_cast<const NodeHeader*>(n); }

inline uint64_t  get_prefix(const uint64_t* n) noexcept { return n[1]; }
inline void      set_prefix(uint64_t* n, uint64_t p)   noexcept { n[1] = p; }

inline constexpr size_t header_u64(uint8_t skip) noexcept { return skip > 0 ? 2 : 1; }

// ==========================================================================
// Global sentinel — zeroed block, valid as:
//   - Compact leaf (count=0, flags=0 → not internal, not split)
//   - Bot-leaf BITS==16 (bitmap all zeros → no entries)
//   - Bot-leaf BITS>16  (count u32 = 0 → no entries)
// No allocation on construction; never deallocated.
// ==========================================================================

inline constinit alignas(64) uint64_t SENTINEL_NODE[4] = {};

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

// ==========================================================================
// binary_search_for_insert
// ==========================================================================

template<typename K>
inline int binary_search_for_insert(const K* data, size_t count, K target) noexcept {
    auto it = std::lower_bound(data, data + count, target);
    if (it != data + count && *it == target)
        return static_cast<int>(it - data);
    return -(static_cast<int>(it - data) + 1);
}

} // namespace kn3

#endif // KNTRIE_SUPPORT_HPP