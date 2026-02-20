#ifndef KNTRIE_SUPPORT_HPP
#define KNTRIE_SUPPORT_HPP

#include <cstdint>
#include <cstring>
#include <bit>
#include <bitset>
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
// Buddy allocator: pow2 sizes from 4 u64 (32 bytes) to 128 u64 (1024 bytes).
// Allocations > FREE_MAX u64s use direct malloc.
// ==========================================================================

inline constexpr size_t FREE_MAX  = 128;

// round_up_u64: actual pow2 allocation size for any u64 request.
inline constexpr size_t round_up_u64(size_t n) noexcept {
    if (n <= 4) return 4;
    return size_t{1} << std::bit_width(n - 1);
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
    // --- Buddy allocator: pow2 sizes, coalescing free ---
    // NumBuckets=10 → MAX_SIZE=1024 bytes (128 u64s)
    // PAGE_SIZE=65536 (64KB) → 64 chunks per page
    static constexpr size_t BUDDY_PAGE   = 4096;
    static constexpr size_t NUM_BUCKETS  = 10;
    static constexpr size_t MAX_BUDDY    = size_t{1} << NUM_BUCKETS;  // 1024 bytes
    static constexpr size_t MAX_BUDDY_U64 = MAX_BUDDY / 8;           // 128 u64s
    static constexpr size_t MIN_BUDDY    = 32;  // 4 u64s minimum
    static constexpr size_t MIN_BUCKET   = 5;   // log2(32)
    static constexpr size_t CHUNKS_PER_PAGE = BUDDY_PAGE / MAX_BUDDY;
    static constexpr size_t META_SIZE    = sizeof(void*) * 2 + sizeof(std::bitset<CHUNKS_PER_PAGE>);
    static constexpr size_t META_CHUNKS  = (META_SIZE + MAX_BUDDY - 1) / MAX_BUDDY;
    static constexpr size_t USABLE_CHUNKS = CHUNKS_PER_PAGE - META_CHUNKS;
    static constexpr size_t MIN_EMPTY_PAGES = 4;

    struct Block {
        union {
            Block* next;
            char data[MAX_BUDDY];
        };
    };

    struct Page;
    struct PageMeta {
        Page* next;
        std::bitset<USABLE_CHUNKS> used_bitmap;
    };
    struct Page {
        Block chunk[USABLE_CHUNKS];
        PageMeta meta;
    };
    static_assert(sizeof(Page) <= BUDDY_PAGE);

    // Aligned wrapper for allocator-based page allocation
    struct alignas(BUDDY_PAGE) aligned_page_t { char data[BUDDY_PAGE]; };
    using PAGE_ALLOC = typename std::allocator_traits<ALLOC>::template rebind_alloc<aligned_page_t>;

    ALLOC      alloc_v;
    PAGE_ALLOC page_alloc_v;
    Block*     free_lists_v[NUM_BUCKETS + 1] = {};
    Page*      pages_v      = nullptr;
    size_t     num_empty_v  = 0;
    size_t     mem_in_use_v = 0;

    Page* alloc_page() {
        auto* raw = page_alloc_v.allocate(1);
        auto* page = reinterpret_cast<Page*>(raw);
        std::memset(page, 0, sizeof(Page));
        return page;
    }

    void dealloc_page(Page* page) {
        page_alloc_v.deallocate(reinterpret_cast<aligned_page_t*>(page), 1);
    }

    static constexpr size_t bucket_for(size_t bytes) noexcept {
        if (bytes <= MIN_BUDDY) return MIN_BUCKET;
        return std::bit_width(bytes - 1);
    }

    static constexpr size_t size_for_bucket(size_t i) noexcept {
        return size_t{1} << i;
    }

    void* buddy_alloc(size_t bytes) {
        size_t i = bucket_for(bytes);

        size_t j = i;
        while (j <= NUM_BUCKETS && !free_lists_v[j])
            j++;

        if (j > NUM_BUCKETS) {
            auto* page = alloc_page();
            page->meta.next = pages_v;
            pages_v = page;

            for (size_t c = USABLE_CHUNKS; c > 0; c--) {
                page->chunk[c-1].next = free_lists_v[NUM_BUCKETS];
                free_lists_v[NUM_BUCKETS] = &page->chunk[c-1];
            }
            j = NUM_BUCKETS;
        }

        while (j > i) {
            Block* block = free_lists_v[j];
            free_lists_v[j] = block->next;

            if (j == NUM_BUCKETS) {
                auto* page = reinterpret_cast<Page*>(
                    reinterpret_cast<uintptr_t>(block) & ~(BUDDY_PAGE - 1));
                size_t ci = block - &page->chunk[0];
                page->meta.used_bitmap.set(ci);
            }

            j--;
            auto* first = block;
            auto* second = reinterpret_cast<Block*>(
                reinterpret_cast<char*>(block) + size_for_bucket(j));

            Block** cursor = &free_lists_v[j];
            while (*cursor && *cursor < first)
                cursor = &(*cursor)->next;
            first->next = second;
            second->next = *cursor;
            *cursor = first;
        }

        Block* ret = free_lists_v[i];
        free_lists_v[i] = ret->next;

        if (i == NUM_BUCKETS) {
            auto* page = reinterpret_cast<Page*>(
                reinterpret_cast<uintptr_t>(ret) & ~(BUDDY_PAGE - 1));
            size_t ci = ret - &page->chunk[0];
            page->meta.used_bitmap.set(ci);
        }

        return ret;
    }

    void buddy_free(void* ptr, size_t bytes) {
        size_t i = bucket_for(bytes);
        auto* block = static_cast<Block*>(ptr);

        while (i < NUM_BUCKETS) {
            auto* buddy = reinterpret_cast<Block*>(
                reinterpret_cast<uintptr_t>(block) ^ size_for_bucket(i));

            Block** cursor = &free_lists_v[i];
            while (*cursor && *cursor < buddy)
                cursor = &(*cursor)->next;

            if (*cursor == buddy) {
                *cursor = buddy->next;
                if (buddy < block) block = buddy;
                i++;
            } else {
                break;
            }
        }

        Block** cursor = &free_lists_v[i];
        while (*cursor && *cursor < block)
            cursor = &(*cursor)->next;
        block->next = *cursor;
        *cursor = block;

        if (i == NUM_BUCKETS) {
            auto* page = reinterpret_cast<Page*>(
                reinterpret_cast<uintptr_t>(block) & ~(BUDDY_PAGE - 1));
            size_t ci = block - &page->chunk[0];
            page->meta.used_bitmap.reset(ci);

            if (page->meta.used_bitmap.none()) {
                num_empty_v++;
                if (num_empty_v > MIN_EMPTY_PAGES) {
                    cursor = &free_lists_v[NUM_BUCKETS];
                    while (*cursor) {
                        auto pa = reinterpret_cast<uintptr_t>(page);
                        auto ba = reinterpret_cast<uintptr_t>(*cursor);
                        if ((ba & ~(BUDDY_PAGE - 1)) == pa)
                            *cursor = (*cursor)->next;
                        else
                            cursor = &(*cursor)->next;
                    }
                    Page** pcursor = &pages_v;
                    while (*pcursor != page)
                        pcursor = &(*pcursor)->meta.next;
                    *pcursor = page->meta.next;
                    num_empty_v--;
                    dealloc_page(page);
                }
            }
        }
    }

    builder() : alloc_v(), page_alloc_v(alloc_v) {}
    explicit builder(const ALLOC& a) : alloc_v(a), page_alloc_v(alloc_v) {}

    builder(const builder&) = delete;
    builder& operator=(const builder&) = delete;

    builder(builder&& o) noexcept
        : alloc_v(std::move(o.alloc_v))
        , page_alloc_v(alloc_v)
        , pages_v(o.pages_v)
        , num_empty_v(o.num_empty_v)
        , mem_in_use_v(o.mem_in_use_v)
    {
        std::memcpy(free_lists_v, o.free_lists_v, sizeof(free_lists_v));
        o.pages_v = nullptr;
        o.num_empty_v = 0;
        std::memset(o.free_lists_v, 0, sizeof(o.free_lists_v));
        o.mem_in_use_v = 0;
    }

    builder& operator=(builder&& o) noexcept {
        if (this != &o) {
            drain();
            alloc_v = std::move(o.alloc_v);
            page_alloc_v = PAGE_ALLOC(alloc_v);
            pages_v      = o.pages_v;
            num_empty_v  = o.num_empty_v;
            mem_in_use_v = o.mem_in_use_v;
            std::memcpy(free_lists_v, o.free_lists_v, sizeof(free_lists_v));
            o.pages_v = nullptr;
            o.num_empty_v = 0;
            std::memset(o.free_lists_v, 0, sizeof(o.free_lists_v));
            o.mem_in_use_v = 0;
        }
        return *this;
    }

    void swap(builder& o) noexcept {
        using std::swap;
        swap(alloc_v, o.alloc_v);
        swap(page_alloc_v, o.page_alloc_v);
        swap(pages_v, o.pages_v);
        swap(num_empty_v, o.num_empty_v);
        swap(mem_in_use_v, o.mem_in_use_v);
        for (size_t i = 0; i <= NUM_BUCKETS; ++i)
            swap(free_lists_v[i], o.free_lists_v[i]);
    }

    const ALLOC& get_allocator() const noexcept { return alloc_v; }
    size_t memory_in_use() const noexcept { return mem_in_use_v; }

    // --- Allocate a node ---
    // u64_count is updated to actual pow2 size allocated
    uint64_t* alloc_node(size_t& u64_count) {
        size_t bytes = u64_count * 8;
        if (bytes > MAX_BUDDY) {
            // Large allocation — direct malloc
            uint64_t* p = alloc_v.allocate(u64_count);
            std::memset(p, 0, bytes);
            mem_in_use_v += bytes;
            return p;
        }
        size_t actual_bytes = size_for_bucket(bucket_for(bytes));
        uint64_t* p = static_cast<uint64_t*>(buddy_alloc(bytes));
        std::memset(p, 0, actual_bytes);
        mem_in_use_v += actual_bytes;
        u64_count = actual_bytes / 8;
        return p;
    }

    // --- Return a node ---
    void dealloc_node(uint64_t* p, size_t u64_count) noexcept {
        size_t bytes = u64_count * 8;
        if (bytes > MAX_BUDDY) {
            alloc_v.deallocate(p, u64_count);
            mem_in_use_v -= bytes;
            return;
        }
        size_t actual_bytes = size_for_bucket(bucket_for(bytes));
        buddy_free(p, bytes);
        mem_in_use_v -= actual_bytes;
    }

    // --- Free all pages. Only safe when all nodes are dead. ---
    void drain() noexcept {
        for (auto& list : free_lists_v)
            list = nullptr;
        while (pages_v) {
            auto* next = pages_v->meta.next;
            dealloc_page(pages_v);
            pages_v = next;
        }
        num_empty_v  = 0;
        mem_in_use_v = 0;
    }

    // --- shrink_to_fit: buddy can return empty pages ---
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

    uint64_t* alloc_node(size_t& u64_count) { return base_v.alloc_node(u64_count); }
    void dealloc_node(uint64_t* p, size_t u64_count) noexcept { base_v.dealloc_node(p, u64_count); }

    slot_type store_value(const VALUE& val) {
        if constexpr (VAL_U64 <= FREE_MAX) {
            size_t sz = round_up_u64(VAL_U64);
            uint64_t* p = base_v.alloc_node(sz);
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
