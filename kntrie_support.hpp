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
// Allocations <= FREE_MAX u64s use 7 size-class bins for freelisting.
// Allocations > FREE_MAX use power-of-2 with midpoints for in-place growth.
//
// Bins: {4, 6, 8, 12, 18, 26, 34, 48, 68}
// Bins 7-8 capture medium bitmask nodes (up to 61 children)
// ==========================================================================

inline constexpr size_t FREE_MAX  = 68;
inline constexpr size_t NUM_BINS  = 9;
inline constexpr size_t BIN_SIZES[NUM_BINS] = {4, 6, 8, 12, 18, 26, 34, 48, 68};

// Page size for slab allocation (4K = 512 u64s)
inline constexpr size_t PAGE_U64S = 512;

// Map requested u64 count to bin index. Returns NUM_BINS if > FREE_MAX.
inline constexpr int bin_for(size_t n) noexcept {
    if (n <= 4)  return 0;
    if (n <= 6)  return 1;
    if (n <= 8)  return 2;
    if (n <= 12) return 3;
    if (n <= 18) return 4;
    if (n <= 26) return 5;
    if (n <= 34) return 6;
    if (n <= 48) return 7;
    if (n <= 68) return 8;
    return NUM_BINS;
}

// round_up_u64: "actual allocation size" for any request.
//   <= FREE_MAX: returns bin class size (freelist handles it)
//   >  FREE_MAX: power-of-2 with midpoints (in-place growth for large bitmask nodes)
inline constexpr size_t round_up_u64(size_t n) noexcept {
    if (n <= FREE_MAX) return BIN_SIZES[bin_for(n)];
    int bit  = static_cast<int>(std::bit_width(n - 1));
    size_t pow2 = size_t{1} << bit;
    size_t mid  = pow2 / 2 + pow2 / 4 + 2;
    return (n <= mid) ? mid : pow2;
}

// Shrink when allocated exceeds the class for 2x the needed size.
// Works for both binned (≤ FREE_MAX) and large (> FREE_MAX) allocations.
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

// Base builder: trivial values (A-type, bool) — page-based slab allocator
//
// Three-level allocation:
//   mega (growing) → pages (4K) → blocks (bin-sized)
//
// Hot path: pop from bins_v[b].
// Cold path: grow_bin → pop page from page_free_v → carve blocks.
// Colder:    grow_pages → allocate mega → thread pages into page_free_v.
template<typename VALUE, bool IS_TRIVIAL, typename ALLOC>
struct builder;

template<typename VALUE, typename ALLOC>
struct builder<VALUE, true, ALLOC> {
    ALLOC      alloc_v;
    uint64_t*  alloc_head_v = nullptr;   // intrusive chain of mega allocations
    uint64_t*  page_free_v  = nullptr;   // uncarved pages
    uint64_t*  bins_v[NUM_BINS] = {};    // per-bin block freelists
    double     mega_pages_v = 1.0;       // pages in next mega (grows 1.25x)
    size_t     mem_in_use_v = 0;         // block bytes handed out (bin-padded)
    size_t     mem_needed_v = 0;         // block bytes requested (before padding)

    builder() : alloc_v() {}
    explicit builder(const ALLOC& a) : alloc_v(a) {}

    builder(const builder&) = delete;
    builder& operator=(const builder&) = delete;

    builder(builder&& o) noexcept
        : alloc_v(std::move(o.alloc_v))
        , alloc_head_v(o.alloc_head_v)
        , page_free_v(o.page_free_v)
        , mega_pages_v(o.mega_pages_v)
        , mem_in_use_v(o.mem_in_use_v)
        , mem_needed_v(o.mem_needed_v)
    {
        std::memcpy(bins_v, o.bins_v, sizeof(bins_v));
        o.alloc_head_v = nullptr;
        o.page_free_v  = nullptr;
        std::memset(o.bins_v, 0, sizeof(o.bins_v));
        o.mega_pages_v = 1.0;
        o.mem_in_use_v = 0;
        o.mem_needed_v = 0;
    }

    builder& operator=(builder&& o) noexcept {
        if (this != &o) {
            drain();
            alloc_v = std::move(o.alloc_v);
            alloc_head_v = o.alloc_head_v;
            page_free_v  = o.page_free_v;
            mega_pages_v = o.mega_pages_v;
            mem_in_use_v = o.mem_in_use_v;
            mem_needed_v = o.mem_needed_v;
            std::memcpy(bins_v, o.bins_v, sizeof(bins_v));
            o.alloc_head_v = nullptr;
            o.page_free_v  = nullptr;
            std::memset(o.bins_v, 0, sizeof(o.bins_v));
            o.mega_pages_v = 1.0;
            o.mem_in_use_v = 0;
            o.mem_needed_v = 0;
        }
        return *this;
    }

    void swap(builder& o) noexcept {
        using std::swap;
        swap(alloc_v, o.alloc_v);
        swap(alloc_head_v, o.alloc_head_v);
        swap(page_free_v, o.page_free_v);
        swap(mega_pages_v, o.mega_pages_v);
        swap(mem_in_use_v, o.mem_in_use_v);
        swap(mem_needed_v, o.mem_needed_v);
        for (size_t i = 0; i < NUM_BINS; ++i)
            swap(bins_v[i], o.bins_v[i]);
    }

    const ALLOC& get_allocator() const noexcept { return alloc_v; }
    size_t memory_in_use() const noexcept { return mem_in_use_v; }
    size_t memory_needed() const noexcept { return mem_needed_v; }

    // --- Mega allocation: grow page_free_v ---
    void grow_pages() {
        size_t pages = static_cast<size_t>(mega_pages_v);
        if (pages < 1) pages = 1;
        size_t u64s = 2 + pages * PAGE_U64S;

        uint64_t* mega = alloc_v.allocate(u64s);
        mega[0] = reinterpret_cast<uint64_t>(alloc_head_v);
        mega[1] = u64s;
        alloc_head_v = mega;

        uint64_t* base = mega + 2;
        for (size_t i = 0; i < pages; ++i) {
            uint64_t* page = base + i * PAGE_U64S;
            page[0] = reinterpret_cast<uint64_t>(page_free_v);
            page_free_v = page;
        }

        mega_pages_v *= 1.25;
    }

    // --- Carve a page into blocks for bin, thread into freelist ---
    void grow_bin(int bin) {
        if (!page_free_v) grow_pages();

        uint64_t* page = page_free_v;
        page_free_v = reinterpret_cast<uint64_t*>(page[0]);

        size_t bs = BIN_SIZES[bin];
        size_t count = PAGE_U64S / bs;

        for (size_t i = 0; i < count; ++i) {
            uint64_t* block = page + i * bs;
            if (i + 1 < count)
                block[0] = reinterpret_cast<uint64_t>(page + (i + 1) * bs);
            else
                block[0] = reinterpret_cast<uint64_t>(bins_v[bin]);
        }
        bins_v[bin] = page;
    }

    // --- Allocate a node ---
    uint64_t* alloc_node(size_t u64_count) {
        int bin = bin_for(u64_count);
        if (bin < static_cast<int>(NUM_BINS)) {
            size_t actual = BIN_SIZES[bin];
            if (!bins_v[bin]) [[unlikely]]
                grow_bin(bin);
            uint64_t* p = bins_v[bin];
            bins_v[bin] = reinterpret_cast<uint64_t*>(p[0]);
            std::memset(p, 0, actual * 8);
            mem_in_use_v += actual * 8;
            mem_needed_v += u64_count * 8;
            return p;
        }
        // Large allocation — direct malloc
        uint64_t* p = alloc_v.allocate(u64_count);
        std::memset(p, 0, u64_count * 8);
        mem_in_use_v += u64_count * 8;
        mem_needed_v += u64_count * 8;
        return p;
    }

    // --- Return a node to its bin freelist ---
    void dealloc_node(uint64_t* p, size_t u64_count) noexcept {
        int bin = bin_for(u64_count);
        if (bin < static_cast<int>(NUM_BINS)) {
            p[0] = reinterpret_cast<uint64_t>(bins_v[bin]);
            bins_v[bin] = p;
            mem_in_use_v -= BIN_SIZES[bin] * 8;
            mem_needed_v -= u64_count * 8;
        } else {
            alloc_v.deallocate(p, u64_count);
            mem_in_use_v -= u64_count * 8;
            mem_needed_v -= u64_count * 8;
        }
    }

    // --- Free all megas. Only safe when all nodes are dead. ---
    void drain() noexcept {
        uint64_t* m = alloc_head_v;
        while (m) {
            uint64_t* next = reinterpret_cast<uint64_t*>(m[0]);
            alloc_v.deallocate(m, m[1]);
            m = next;
        }
        alloc_head_v = nullptr;
        page_free_v = nullptr;
        std::memset(bins_v, 0, sizeof(bins_v));
        mega_pages_v = 1.0;
        mem_in_use_v = 0;
        mem_needed_v = 0;
    }

    // --- shrink_to_fit: no-op (pages cannot be partially freed) ---
    void shrink_to_fit() noexcept {}

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

    uint64_t* alloc_node(size_t u64_count) { return base_v.alloc_node(u64_count); }
    void dealloc_node(uint64_t* p, size_t u64_count) noexcept { base_v.dealloc_node(p, u64_count); }

    slot_type store_value(const VALUE& val) {
        if constexpr (VAL_U64 <= FREE_MAX) {
            uint64_t* p = base_v.alloc_node(round_up_u64(VAL_U64));
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
            base_v.dealloc_node(reinterpret_cast<uint64_t*>(s), round_up_u64(VAL_U64));
        } else {
            VA va(base_v.get_allocator());
            std::allocator_traits<VA>::deallocate(va, s, 1);
        }
    }

    void drain() noexcept {
        base_v.drain();
    }

    void shrink_to_fit() noexcept { base_v.shrink_to_fit(); }
    size_t memory_in_use() const noexcept { return base_v.memory_in_use(); }
    size_t memory_needed() const noexcept { return base_v.memory_needed(); }
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
