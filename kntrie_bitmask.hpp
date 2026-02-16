#ifndef KNTRIE_BITMASK_HPP
#define KNTRIE_BITMASK_HPP

#include "kntrie_support.hpp"

namespace gteitelbaum {

// ==========================================================================
// 256-bit bitmap
// ==========================================================================

enum class slot_mode { FAST_EXIT, BRANCHLESS, UNFILTERED };

struct bitmap256 {
    uint64_t words[4] = {0, 0, 0, 0};

    bool has_bit(uint8_t i) const noexcept { return words[i >> 6] & (1ULL << (i & 63)); }
    void set_bit(uint8_t i) noexcept { words[i >> 6] |= (1ULL << (i & 63)); }
    void clear_bit(uint8_t i) noexcept { words[i >> 6] &= ~(1ULL << (i & 63)); }

    int popcount() const noexcept {
        return std::popcount(words[0]) + std::popcount(words[1]) +
               std::popcount(words[2]) + std::popcount(words[3]);
    }

    // Extract the index of the single set bit (for embed chain walking)
    uint8_t single_bit_index() const noexcept {
        for (int i = 0; i < 4; ++i)
            if (words[i]) return static_cast<uint8_t>(i * 64 + std::countr_zero(words[i]));
        __builtin_unreachable();
    }

    // FAST_EXIT:   returns slot (>=0) if bit set, -1 if not set
    // BRANCHLESS:  returns slot if bit set, 0 (sentinel) if not set
    // UNFILTERED:  returns count of set bits below index (for insert position)
    template<slot_mode MODE>
    int find_slot(uint8_t index) const noexcept {
        const int w = index >> 6, b = index & 63;
        uint64_t before = words[w] << (63 - b);
        if constexpr (MODE == slot_mode::FAST_EXIT) {
            if (!(before & (1ULL << 63))) [[unlikely]] return -1;
        }

        int slot = std::popcount(before);
        slot += std::popcount(words[0]) & -int(w > 0);
        slot += std::popcount(words[1]) & -int(w > 1);
        slot += std::popcount(words[2]) & -int(w > 2);

        if constexpr (MODE == slot_mode::BRANCHLESS)
            slot &= -int(bool(before & (1ULL << 63)));
        else if constexpr (MODE == slot_mode::FAST_EXIT)
            slot--;
        else
            slot -= int(bool(before & (1ULL << 63)));

        return slot;
    }

    // Iterate all set bits, calling fn(uint8_t bit_index, int slot) in order.
    // Single pass: each word visited once, each bit popped with clear-lowest.
    template<typename F>
    void for_each_set(F&& fn) const noexcept {
        int slot = 0;
        for (int w = 0; w < 4; ++w) {
            uint64_t bits = words[w];
            while (bits) {
                int b = std::countr_zero(bits);
                fn(static_cast<uint8_t>((w << 6) + b), slot++);
                bits &= bits - 1;
            }
        }
    }

    // Return the lowest set bit index. Undefined if bitmap is empty.
    uint8_t first_set_bit() const noexcept {
        for (int w = 0; w < 4; ++w)
            if (words[w]) return static_cast<uint8_t>((w << 6) + std::countr_zero(words[w]));
        __builtin_unreachable();
    }

    // Return the highest set bit index. Undefined if bitmap is empty.
    uint8_t last_set_bit() const noexcept {
        for (int w = 3; w >= 0; --w)
            if (words[w]) return static_cast<uint8_t>((w << 6) + 63 - std::countl_zero(words[w]));
        __builtin_unreachable();
    }

    struct adj_result { uint8_t idx; uint16_t slot; bool found; };

    // Find smallest set bit > idx, with its slot. Single pass.
    adj_result next_set_after(uint8_t idx) const noexcept {
        if (idx == 255) return {0, 0, false};
        int start = idx + 1;
        int w = start >> 6, b = start & 63;

        int slot = 0;
        for (int i = 0; i < w; ++i)
            slot += std::popcount(words[i]);

        uint64_t m = words[w] & (~0ULL << b);
        if (m) {
            int bit = (w << 6) + std::countr_zero(m);
            slot += std::popcount(words[w] & ((1ULL << (bit & 63)) - 1));
            return {static_cast<uint8_t>(bit), static_cast<uint16_t>(slot), true};
        }
        slot += std::popcount(words[w]);

        for (int ww = w + 1; ww < 4; ++ww) {
            if (words[ww]) {
                int bit = (ww << 6) + std::countr_zero(words[ww]);
                slot += std::popcount(words[ww] & ((1ULL << (bit & 63)) - 1));
                return {static_cast<uint8_t>(bit), static_cast<uint16_t>(slot), true};
            }
            slot += std::popcount(words[ww]);
        }
        return {0, 0, false};
    }

    // Find largest set bit < idx, with its slot. Single pass backward.
    adj_result prev_set_before(uint8_t idx) const noexcept {
        if (idx == 0) return {0, 0, false};
        int last = idx - 1;
        int w = last >> 6, b = last & 63;

        uint64_t m = words[w] & ((2ULL << b) - 1);

        for (int ww = w; ww >= 0; --ww) {
            uint64_t bits = (ww == w) ? m : words[ww];
            if (bits) {
                int bit = (ww << 6) + 63 - std::countl_zero(bits);
                int slot = 0;
                for (int i = 0; i < ww; ++i)
                    slot += std::popcount(words[i]);
                slot += std::popcount(words[ww] & ((1ULL << (bit & 63)) - 1));
                return {static_cast<uint8_t>(bit), static_cast<uint16_t>(slot), true};
            }
        }
        return {0, 0, false};
    }

    static bitmap256 from_indices(const uint8_t* indices, unsigned count) noexcept {
        bitmap256 bm{};
        for (unsigned i = 0; i < count; ++i) bm.set_bit(indices[i]);
        return bm;
    }

    // Fill dest in bitmap order from unsorted (indices, tagged_ptrs)
    static void arr_fill_sorted(const bitmap256& bm, uint64_t* dest,
                                const uint8_t* indices, const uint64_t* tagged_ptrs,
                                unsigned count) noexcept {
        for (unsigned i = 0; i < count; ++i)
            dest[bm.find_slot<slot_mode::UNFILTERED>(indices[i])] = tagged_ptrs[i];
    }

    // In-place insert: memmove right, write new entry, set bit
    static void arr_insert(bitmap256& bm, uint64_t* arr, unsigned count,
                           uint8_t idx, uint64_t val) noexcept {
        int isl = bm.find_slot<slot_mode::UNFILTERED>(idx);
        std::memmove(arr + isl + 1, arr + isl, (count - isl) * sizeof(uint64_t));
        arr[isl] = val;
        bm.set_bit(idx);
    }

    // In-place remove: memmove left, clear bit
    static void arr_remove(bitmap256& bm, uint64_t* arr, unsigned count,
                           int slot, uint8_t idx) noexcept {
        std::memmove(arr + slot, arr + slot + 1, (count - 1 - slot) * sizeof(uint64_t));
        bm.clear_bit(idx);
    }

    // Copy old array into new array with a new entry inserted
    static void arr_copy_insert(const uint64_t* old_arr, uint64_t* new_arr,
                                unsigned old_count, int isl, uint64_t val) noexcept {
        std::memcpy(new_arr, old_arr, isl * sizeof(uint64_t));
        new_arr[isl] = val;
        std::memcpy(new_arr + isl + 1, old_arr + isl,
                     (old_count - isl) * sizeof(uint64_t));
    }

    // Copy old array into new array with one entry removed
    static void arr_copy_remove(const uint64_t* old_arr, uint64_t* new_arr,
                                unsigned old_count, int slot) noexcept {
        std::memcpy(new_arr, old_arr, slot * sizeof(uint64_t));
        std::memcpy(new_arr + slot, old_arr + slot + 1,
                     (old_count - 1 - slot) * sizeof(uint64_t));
    }
};

// ==========================================================================
// bitmask_ops  -- unified bitmask node + bitmap256 leaf operations
//
// Bitmask node (internal): [header(1)][bitmap(4)][sentinel(1)][children(n)][desc(n u16)]
//   - Parent pointer targets &node[1] (bitmap), no LEAF_BIT
//   - sentinel at offset 5 from bitmap = SENTINEL_TAGGED for branchless miss
//   - real children at offset 5 from bitmap (after sentinel)
//   - All children are tagged uint64_t values
//   - desc array: uint16_t per child, stores child descendant count (capped)
//
// Bitmap256 leaf (suffix_type=0): [header(1 or 2)][bitmap(4)][values(n)]
//   - Parent pointer targets &node[0] | LEAF_BIT
//   - header_size = 1 (no skip) or 2 (with skip, prefix in node[1])
//   - values at header_size + 4
// ==========================================================================

template<typename VALUE, typename ALLOC>
struct bitmask_ops {
    using VT   = value_traits<VALUE, ALLOC>;
    using VST  = typename VT::slot_type;

    // ==================================================================
    // Size calculations
    // ==================================================================

    static constexpr size_t bitmask_size_u64(size_t n_children, size_t hu = HEADER_U64) noexcept {
        return hu + BITMAP256_U64 + 1 + n_children + desc_u64(n_children);
    }

    static constexpr size_t bitmap_leaf_size_u64(size_t count, size_t hu = HEADER_U64) noexcept {
        size_t vb = count * sizeof(VST);
        vb = (vb + 7) & ~size_t{7};
        return hu + BITMAP256_U64 + vb / 8;
    }

    // ==================================================================
    // Bitmask node: branchless descent (for find) — tagged version
    // Takes bitmap pointer directly (not node pointer).
    // Returns tagged uint64_t (next child or SENTINEL_TAGGED).
    // ==================================================================

    static uint64_t branchless_find_tagged(const uint64_t* bm_ptr, uint8_t idx) noexcept {
        const bitmap256& bm = *reinterpret_cast<const bitmap256*>(bm_ptr);
        int slot = bm.find_slot<slot_mode::BRANCHLESS>(idx);  // 0 on miss -> sentinel
        return bm_ptr[BITMAP256_U64 + slot];  // [0-3]=bitmap, [4]=sentinel, [5+]=children
    }

    // ==================================================================
    // Bitmask node: lookup child (returns tagged uint64_t)
    // ==================================================================

    struct child_lookup {
        uint64_t child;   // tagged value
        int       slot;
        bool      found;
    };

    static child_lookup lookup(const uint64_t* node, uint8_t idx) noexcept {
        return lookup_at_(node, 1, idx);
    }

    // ==================================================================
    // Bitmask node: set child pointer (tagged)
    // ==================================================================

    static void set_child(uint64_t* node, int slot, uint64_t tagged_ptr) noexcept {
        real_children_mut_(node, 1)[slot] = tagged_ptr;
    }

    // ==================================================================
    // Skip chain read accessors
    // ==================================================================

    // Read single skip byte at embed position e (0-based)
    static uint8_t skip_byte(const uint64_t* node, uint8_t e) noexcept {
        const auto* embed_bm = reinterpret_cast<const bitmap256*>(node + 1 + static_cast<size_t>(e) * 6);
        return embed_bm->single_bit_index();
    }

    // Copy all skip bytes to buffer
    static void skip_bytes(const uint64_t* node, uint8_t sc, uint8_t* out) noexcept {
        for (uint8_t e = 0; e < sc; ++e)
            out[e] = skip_byte(node, e);
    }

    // Lookup in the final bitmap of a skip chain
    static child_lookup chain_lookup(const uint64_t* node, uint8_t sc, uint8_t idx) noexcept {
        return lookup_at_(node, chain_hs_(sc), idx);
    }

    // Read tagged child at slot in final bitmap of skip chain
    static uint64_t chain_child(const uint64_t* node, uint8_t sc, int slot) noexcept {
        return real_children_(node, chain_hs_(sc))[slot];
    }

    // Write tagged child at slot in final bitmap of skip chain
    static void chain_set_child(uint64_t* node, uint8_t sc, int slot, uint64_t tagged) noexcept {
        real_children_mut_(node, chain_hs_(sc))[slot] = tagged;
    }

    // Desc array of final bitmap (const)
    static const uint16_t* chain_desc_array(const uint64_t* node, uint8_t sc, unsigned nc) noexcept {
        return desc_array_(node, chain_hs_(sc), nc);
    }

    // Desc array of final bitmap (mutable)
    static uint16_t* chain_desc_array_mut(uint64_t* node, uint8_t sc, unsigned nc) noexcept {
        return desc_array_mut_(node, chain_hs_(sc), nc);
    }

    // Final bitmap reference (const)
    static const bitmap256& chain_bitmap(const uint64_t* node, uint8_t sc) noexcept {
        return bm_(node, chain_hs_(sc));
    }

    // Number of children in final bitmap
    static unsigned chain_child_count(const uint64_t* node, uint8_t sc) noexcept {
        return static_cast<unsigned>(chain_bitmap(node, sc).popcount());
    }

    // Pointer to real children array in final bitmap
    static const uint64_t* chain_children(const uint64_t* node, uint8_t sc) noexcept {
        return real_children_(node, chain_hs_(sc));
    }
    static uint64_t* chain_children_mut(uint64_t* node, uint8_t sc) noexcept {
        return real_children_mut_(node, chain_hs_(sc));
    }

    // Iterate final bitmap children: cb(slot, tagged_child)
    template<typename Fn>
    static void chain_for_each_child(const uint64_t* node, uint8_t sc, Fn&& cb) noexcept {
        size_t hs = chain_hs_(sc);
        unsigned nc = static_cast<unsigned>(bm_(node, hs).popcount());
        const uint64_t* ch = real_children_(node, hs);
        for (unsigned i = 0; i < nc; ++i)
            cb(i, ch[i]);
    }

    // Embed child pointer (the pointer in embed e that links to next embed or final bitmap)
    static uint64_t embed_child(const uint64_t* node, uint8_t e) noexcept {
        return node[1 + static_cast<size_t>(e) * 6 + 5];
    }
    static void set_embed_child(uint64_t* node, uint8_t e, uint64_t tagged) noexcept {
        node[1 + static_cast<size_t>(e) * 6 + 5] = tagged;
    }

    // ==================================================================
    // Tagged pointer accessors (for iteration on standalone bitmask)
    // ==================================================================

    // Bitmap256 ref from a bitmask tagged pointer (ptr points at bitmap)
    static const bitmap256& bitmap_ref(uint64_t bm_tagged) noexcept {
        return *reinterpret_cast<const bitmap256*>(bm_tagged);
    }

    // Read tagged child at slot from a bitmask tagged pointer
    static uint64_t child_at(uint64_t bm_tagged, int slot) noexcept {
        const uint64_t* bm = reinterpret_cast<const uint64_t*>(bm_tagged);
        return bm[BITMAP256_U64 + 1 + slot];
    }

    // First child (slot 0) from a bitmask tagged pointer
    static uint64_t first_child(uint64_t bm_tagged) noexcept {
        const uint64_t* bm = reinterpret_cast<const uint64_t*>(bm_tagged);
        return bm[BITMAP256_U64 + 1];
    }

    // ==================================================================
    // Bitmask node: add child (tagged) — standalone
    // ==================================================================

    static uint64_t* add_child(uint64_t* node, node_header* h,
                                uint8_t idx, uint64_t child_tagged,
                                uint16_t child_desc, ALLOC& alloc) {
        return add_child_at_(node, h, 1, idx, child_tagged, child_desc, alloc);
    }

    // ==================================================================
    // Skip chain: add child to final bitmask
    // ==================================================================

    static uint64_t* chain_add_child(uint64_t* node, node_header* h,
                                      uint8_t sc, uint8_t idx,
                                      uint64_t child_tagged,
                                      uint16_t child_desc, ALLOC& alloc) {
        uint64_t* nn = add_child_at_(node, h, chain_hs_(sc), idx,
                                      child_tagged, child_desc, alloc);
        if (nn != node && sc > 0) fix_embeds_(nn, sc);
        return nn;
    }

    // ==================================================================
    // Bitmask node: remove child — standalone
    // Returns nullptr if node becomes empty.
    // ==================================================================

    static uint64_t* remove_child(uint64_t* node, node_header* h,
                                   int slot, uint8_t idx, ALLOC& alloc) {
        return remove_child_at_(node, h, 1, slot, idx, alloc);
    }

    // ==================================================================
    // Skip chain: remove child from final bitmask
    // ==================================================================

    static uint64_t* chain_remove_child(uint64_t* node, node_header* h,
                                         uint8_t sc, int slot, uint8_t idx,
                                         ALLOC& alloc) {
        uint64_t* nn = remove_child_at_(node, h, chain_hs_(sc), slot, idx, alloc);
        if (nn && nn != node && sc > 0) fix_embeds_(nn, sc);
        return nn;
    }

    // ==================================================================
    // Bitmask node: make from arrays (tagged children)
    // ==================================================================

    static uint64_t* make_bitmask(const uint8_t* indices,
                                   const uint64_t* child_tagged_ptrs,
                                   unsigned n_children, ALLOC& alloc,
                                   const uint16_t* descs = nullptr) {
        bitmap256 bm = bitmap256::from_indices(indices, n_children);

        constexpr size_t hs = 1;
        size_t needed = bitmask_size_u64(n_children, hs);
        size_t au64 = round_up_u64(needed);
        uint64_t* nn = alloc_node(alloc, au64);
        auto* nh = get_header(nn);
        nh->set_entries(n_children);
        nh->set_alloc_u64(au64);
        nh->set_skip(0);
        nh->set_bitmask();

        bm_mut_(nn, hs) = bm;
        children_mut_(nn, hs)[0] = SENTINEL_TAGGED;

        bitmap256::arr_fill_sorted(bm, real_children_mut_(nn, hs),
                                    indices, child_tagged_ptrs, n_children);

        // Fill desc array
        uint16_t* nd = desc_array_mut_(nn, hs, n_children);
        if (descs) {
            std::memcpy(nd, descs, n_children * sizeof(uint16_t));
        } else {
            std::memset(nd, 0, n_children * sizeof(uint16_t));
        }
        return nn;
    }

    // ==================================================================
    // Bitmask node: make skip chain (one allocation)
    //
    // Layout: [header(1)][embed_0(6)]...[embed_{S-1}(6)][final_bm(4)][sent(1)][children(N)]
    // Each embed = bitmap256(4) + sentinel(1) + child_ptr(1)
    // child_ptr points to next embed's bitmap (or final bitmap).
    // Total: 1 + S*6 + 4 + 1 + 1 + N = 7 + S*6 + N  (but we use 6 + S*6 + N since
    //        final_bm(4)+sent(1)+first_child_slot = 6, and remaining N-1 children follow,
    //        but actually: header(1) + S*6 + 4 + 1 + N = 6 + S*6 + N)
    // ==================================================================

    static uint64_t* make_skip_chain(const uint8_t* skip_bytes, uint8_t skip_count,
                                      const uint8_t* final_indices,
                                      const uint64_t* final_children_tagged,
                                      unsigned final_n_children, ALLOC& alloc,
                                      const uint16_t* descs = nullptr) {
        // Allocation: header(1) + skip_count*6 + bitmap(4) + sentinel(1) + children(N) + desc(N)
        size_t needed = 1 + static_cast<size_t>(skip_count) * 6 + 5 + final_n_children
                       + desc_u64(final_n_children);
        size_t au64 = round_up_u64(needed);
        uint64_t* nn = alloc_node(alloc, au64);

        auto* nh = get_header(nn);
        nh->set_entries(final_n_children);
        nh->set_alloc_u64(au64);
        nh->set_skip(skip_count);
        nh->set_bitmask();

        // Build each embed: bitmap(4) + sentinel(1) + child_ptr(1)
        for (uint8_t e = 0; e < skip_count; ++e) {
            uint64_t* embed = nn + 1 + e * 6;
            // bitmap with single bit
            bitmap256& bm = *reinterpret_cast<bitmap256*>(embed);
            bm = bitmap256{};
            bm.set_bit(skip_bytes[e]);
            // sentinel
            embed[4] = SENTINEL_TAGGED;
            // child ptr → next embed's bitmap (or final bitmap)
            uint64_t* next_bm = nn + 1 + (e + 1) * 6;
            embed[5] = reinterpret_cast<uint64_t>(next_bm);  // no LEAF_BIT
        }

        // Final bitmask
        size_t final_offset = 1 + static_cast<size_t>(skip_count) * 6;
        bitmap256 fbm = bitmap256::from_indices(final_indices, final_n_children);
        *reinterpret_cast<bitmap256*>(nn + final_offset) = fbm;
        nn[final_offset + 4] = SENTINEL_TAGGED;
        bitmap256::arr_fill_sorted(fbm, nn + final_offset + 5,
                                    final_indices, final_children_tagged,
                                    final_n_children);

        // Fill desc array (after children)
        uint16_t* nd = reinterpret_cast<uint16_t*>(nn + final_offset + 5 + final_n_children);
        if (descs) {
            std::memcpy(nd, descs, final_n_children * sizeof(uint16_t));
        } else {
            std::memset(nd, 0, final_n_children * sizeof(uint16_t));
        }

        return nn;
    }

    // ==================================================================
    // Bitmask node: iterate  cb(uint8_t idx, int slot, uint64_t tagged_child)
    // ==================================================================

    template<typename Fn>
    static void for_each_child(const uint64_t* node, Fn&& cb) {
        constexpr size_t hs = 1;
        const bitmap256& bm = bm_(node, hs);
        const uint64_t* rch = real_children_(node, hs);
        bm.for_each_set([&](uint8_t idx, int slot) {
            cb(idx, slot, rch[slot]);
        });
    }

    // ==================================================================
    // Bitmask node: child count / alloc
    // ==================================================================

    static int child_count(const uint64_t* node) noexcept {
        return get_header(node)->entries();
    }

    // --- Desc array accessors (standalone bitmask, hs=1) ---
    static uint16_t* child_desc_array(uint64_t* node) noexcept {
        constexpr size_t hs = 1;
        unsigned nc = get_header(node)->entries();
        return desc_array_mut_(node, hs, nc);
    }
    static const uint16_t* child_desc_array(const uint64_t* node) noexcept {
        constexpr size_t hs = 1;
        unsigned nc = get_header(node)->entries();
        return desc_array_(node, hs, nc);
    }

    static size_t node_alloc_u64(const uint64_t* node) noexcept {
        return get_header(node)->alloc_u64();
    }

    // ==================================================================
    // Bitmask node: deallocate (node only, not children)
    // ==================================================================

    static void dealloc_bitmask(uint64_t* node, ALLOC& alloc) noexcept {
        dealloc_node(alloc, node, get_header(node)->alloc_u64());
    }

    // ==================================================================
    // Bitmap256 leaf: find
    // ==================================================================

    static const VALUE* bitmap_find(const uint64_t* node, node_header h,
                                     uint8_t suffix, size_t header_size) noexcept {
        const bitmap256& bm = bm_(node, header_size);
        int slot = bm.find_slot<slot_mode::FAST_EXIT>(suffix);
        if (slot < 0) [[unlikely]] return nullptr;
        return VT::as_ptr(bl_vals_(node, header_size)[slot]);
    }

    // ==================================================================
    // Bitmap256 leaf: iterator helpers
    // ==================================================================

    struct iter_bm_result { uint8_t suffix; const VST* value; bool found; };

    static iter_bm_result bitmap_iter_first(const uint64_t* node,
                                             size_t header_size) noexcept {
        const bitmap256& bm = bm_(node, header_size);
        const VST* vd = bl_vals_(node, header_size);
        return {bm.first_set_bit(), &vd[0], true};
    }

    static iter_bm_result bitmap_iter_last(const uint64_t* node,
                                            node_header h,
                                            size_t header_size) noexcept {
        const bitmap256& bm = bm_(node, header_size);
        const VST* vd = bl_vals_(node, header_size);
        unsigned count = h.entries();
        return {bm.last_set_bit(), &vd[count - 1], true};
    }

    static iter_bm_result bitmap_iter_next(const uint64_t* node,
                                            uint8_t suffix,
                                            size_t header_size) noexcept {
        const bitmap256& bm = bm_(node, header_size);
        auto r = bm.next_set_after(suffix);
        if (!r.found) return {0, nullptr, false};
        const VST* vd = bl_vals_(node, header_size);
        return {r.idx, &vd[r.slot], true};
    }

    static iter_bm_result bitmap_iter_prev(const uint64_t* node,
                                            uint8_t suffix,
                                            size_t header_size) noexcept {
        const bitmap256& bm = bm_(node, header_size);
        auto r = bm.prev_set_before(suffix);
        if (!r.found) return {0, nullptr, false};
        const VST* vd = bl_vals_(node, header_size);
        return {r.idx, &vd[r.slot], true};
    }

    // ==================================================================
    // Bitmap256 leaf: insert
    // ==================================================================

    template<bool INSERT = true, bool ASSIGN = true>
    requires (INSERT || ASSIGN)
    static insert_result_t bitmap_insert(uint64_t* node, uint8_t suffix,
                                          VST value, ALLOC& alloc) {
        auto* h = get_header(node);
        size_t hs = hdr_u64(node);
        bitmap256& bm = bm_mut_(node, hs);
        unsigned count = h->entries();
        VST* vd = bl_vals_mut_(node, hs);

        if (bm.has_bit(suffix)) {
            if constexpr (ASSIGN) {
                int slot = bm.find_slot<slot_mode::UNFILTERED>(suffix);
                VT::destroy(vd[slot], alloc);
                VT::write_slot(&vd[slot], value);
            }
            return {tag_leaf(node), false, false};
        }

        if constexpr (!INSERT) return {tag_leaf(node), false, false};

        unsigned nc = count + 1;
        size_t new_sz = bitmap_leaf_size_u64(nc, hs);

        // In-place
        if (new_sz <= h->alloc_u64()) {
            int isl = bm.find_slot<slot_mode::UNFILTERED>(suffix);
            bm.set_bit(suffix);
            std::memmove(vd + isl + 1, vd + isl, (count - isl) * sizeof(VST));
            VT::write_slot(&vd[isl], value);
            h->set_entries(nc);
            return {tag_leaf(node), true, false};
        }

        // Realloc
        size_t au64 = round_up_u64(new_sz);
        uint64_t* nn = alloc_node(alloc, au64);
        auto* nh = get_header(nn);
        *nh = *h;
        if (h->is_skip()) nn[1] = reinterpret_cast<const uint64_t*>(h)[1];
        nh->set_entries(nc);
        nh->set_alloc_u64(au64);
        bitmap256& nbm = bm_mut_(nn, hs);
        nbm = bm;
        nbm.set_bit(suffix);
        VST* nvd = bl_vals_mut_(nn, hs);
        int isl = nbm.find_slot<slot_mode::UNFILTERED>(suffix);
        std::memcpy(nvd, vd, isl * sizeof(VST));
        VT::write_slot(&nvd[isl], value);
        std::memcpy(nvd + isl + 1, vd + isl, (count - isl) * sizeof(VST));

        dealloc_node(alloc, node, h->alloc_u64());
        return {tag_leaf(nn), true, false};
    }

    // ==================================================================
    // Bitmap256 leaf: erase
    // ==================================================================

    static erase_result_t bitmap_erase(uint64_t* node, uint8_t suffix,
                                        ALLOC& alloc) {
        auto* h = get_header(node);
        size_t hs = hdr_u64(node);
        bitmap256& bm = bm_mut_(node, hs);
        if (!bm.has_bit(suffix)) return {tag_leaf(node), false, 0};

        unsigned count = h->entries();
        int slot = bm.find_slot<slot_mode::UNFILTERED>(suffix);
        VT::destroy(bl_vals_mut_(node, hs)[slot], alloc);

        unsigned nc = count - 1;
        if (nc == 0) {
            dealloc_node(alloc, node, h->alloc_u64());
            return {0, true, 0};
        }

        size_t new_sz = bitmap_leaf_size_u64(nc, hs);

        // In-place
        if (!should_shrink_u64(h->alloc_u64(), new_sz)) {
            VST* vd = bl_vals_mut_(node, hs);
            bm.clear_bit(suffix);
            std::memmove(vd + slot, vd + slot + 1, (nc - slot) * sizeof(VST));
            h->set_entries(nc);
            return {tag_leaf(node), true, static_cast<uint16_t>(nc)};
        }

        // Realloc
        size_t au64 = round_up_u64(new_sz);
        uint64_t* nn = alloc_node(alloc, au64);
        auto* nh = get_header(nn);
        *nh = *h;
        if (h->is_skip()) nn[1] = reinterpret_cast<const uint64_t*>(h)[1];
        nh->set_entries(nc);
        nh->set_alloc_u64(au64);
        bm_mut_(nn, hs) = bm;
        bm_mut_(nn, hs).clear_bit(suffix);
        const VST* ov = bl_vals_(node, hs);
        VST*       nv = bl_vals_mut_(nn, hs);
        std::memcpy(nv, ov, slot * sizeof(VST));
        std::memcpy(nv + slot, ov + slot + 1, (nc - slot) * sizeof(VST));

        dealloc_node(alloc, node, h->alloc_u64());
        return {tag_leaf(nn), true, static_cast<uint16_t>(nc)};
    }

    // ==================================================================
    // Bitmap256 leaf: make from sorted suffixes
    // ==================================================================

    static uint64_t* make_bitmap_leaf(const uint8_t* sorted_suffixes,
                                       const VST* values, unsigned count,
                                       ALLOC& alloc) {
        constexpr size_t hs = 1;
        size_t sz = round_up_u64(bitmap_leaf_size_u64(count));
        uint64_t* node = alloc_node(alloc, sz);
        auto* h = get_header(node);
        h->set_entries(count);
        h->set_alloc_u64(sz);
        h->set_suffix_type(0);  // bitmap256 leaf
        bitmap256& bm = bm_mut_(node, hs);
        bm = bitmap256{};
        for (unsigned i = 0; i < count; ++i) bm.set_bit(sorted_suffixes[i]);
        VST* vd = bl_vals_mut_(node, hs);
        for (unsigned i = 0; i < count; ++i)
            vd[bm.find_slot<slot_mode::UNFILTERED>(sorted_suffixes[i])] = values[i];
        return node;
    }

    // ==================================================================
    // Bitmap256 leaf: make single entry
    // ==================================================================

    static uint64_t* make_single_bitmap(uint8_t suffix, VST value, ALLOC& alloc) {
        constexpr size_t hs = 1;
        size_t sz = round_up_u64(bitmap_leaf_size_u64(1));
        uint64_t* node = alloc_node(alloc, sz);
        auto* h = get_header(node);
        h->set_entries(1);
        h->set_alloc_u64(sz);
        h->set_suffix_type(0);
        bm_mut_(node, hs).set_bit(suffix);
        VT::write_slot(&bl_vals_mut_(node, hs)[0], value);
        return node;
    }

    // ==================================================================
    // Bitmap256 leaf: iterate  cb(uint8_t suffix, VST value_slot)
    // ==================================================================

    template<typename Fn>
    static void for_each_bitmap(const uint64_t* node, Fn&& cb) {
        size_t hs = hdr_u64(node);
        const bitmap256& bm = bm_(node, hs);
        const VST* vd = bl_vals_(node, hs);
        bm.for_each_set([&](uint8_t idx, int slot) {
            cb(idx, vd[slot]);
        });
    }

    // ==================================================================
    // Bitmap256 leaf: count
    // ==================================================================

    static uint32_t bitmap_count(const uint64_t* node) noexcept {
        return get_header(node)->entries();
    }

    // ==================================================================
    // Bitmap256 leaf: destroy values + deallocate
    // ==================================================================

    static void bitmap_destroy_and_dealloc(uint64_t* node, ALLOC& alloc) {
        auto* h = get_header(node);
        if constexpr (!VT::IS_INLINE) {
            uint16_t count = h->entries();
            VST* vd = bl_vals_mut_(node, hdr_u64(node));
            for (uint16_t i = 0; i < count; ++i)
                VT::destroy(vd[i], alloc);
        }
        dealloc_node(alloc, node, h->alloc_u64());
    }

    // --- Chain header size: 1 (base header) + sc * 6 (embed slots) ---
    static constexpr size_t chain_hs_(uint8_t sc) noexcept {
        return 1 + static_cast<size_t>(sc) * 6;
    }

private:
    // --- Fix embed internal pointers after reallocation ---
    static void fix_embeds_(uint64_t* nn, uint8_t sc) noexcept {
        for (uint8_t e = 0; e < sc; ++e) {
            uint64_t* next_bm = nn + 1 + static_cast<size_t>(e + 1) * 6;
            nn[1 + static_cast<size_t>(e) * 6 + 5] = reinterpret_cast<uint64_t>(next_bm);
        }
        // Fix sentinel of final bitmap
        size_t fo = chain_hs_(sc);
        nn[fo + BITMAP256_U64] = SENTINEL_TAGGED;
    }

    // --- Shared add child core: works for any header size ---
    static uint64_t* add_child_at_(uint64_t* node, node_header* h, size_t hs,
                                    uint8_t idx, uint64_t child_tagged,
                                    uint16_t child_desc, ALLOC& alloc) {
        bitmap256& bm = bm_mut_(node, hs);
        unsigned oc = h->entries();
        unsigned nc = oc + 1;
        int isl = bm.find_slot<slot_mode::UNFILTERED>(idx);
        size_t needed = bitmask_size_u64(nc, hs);

        // In-place
        if (needed <= h->alloc_u64()) {
            // Save desc array (children shift will overwrite it)
            uint16_t saved_desc[256];
            const uint16_t* od = desc_array_(node, hs, oc);
            std::memcpy(saved_desc, od, oc * sizeof(uint16_t));

            // Insert child
            uint64_t* rch = real_children_mut_(node, hs);
            std::memmove(rch + isl + 1, rch + isl, (oc - isl) * sizeof(uint64_t));
            rch[isl] = child_tagged;
            bm.set_bit(idx);
            h->set_entries(nc);

            // Write desc with insertion
            uint16_t* nd = desc_array_mut_(node, hs, nc);
            std::memcpy(nd, saved_desc, isl * sizeof(uint16_t));
            nd[isl] = child_desc;
            std::memcpy(nd + isl + 1, saved_desc + isl, (oc - isl) * sizeof(uint16_t));
            return node;
        }

        // Realloc
        size_t au64 = round_up_u64(needed);
        uint64_t* nn = alloc_node(alloc, au64);

        // Copy header + embeds/bitmap + sentinel (everything before children)
        size_t prefix_u64 = hs + BITMAP256_U64 + 1;
        std::memcpy(nn, node, prefix_u64 * 8);

        auto* nh = get_header(nn);
        nh->set_entries(nc);
        nh->set_alloc_u64(au64);

        bm_mut_(nn, hs).set_bit(idx);
        children_mut_(nn, hs)[0] = SENTINEL_TAGGED;

        bitmap256::arr_copy_insert(real_children_(node, hs), real_children_mut_(nn, hs),
                                    oc, isl, child_tagged);

        // Copy desc with insertion
        const uint16_t* od = desc_array_(node, hs, oc);
        uint16_t* nd = desc_array_mut_(nn, hs, nc);
        std::memcpy(nd, od, isl * sizeof(uint16_t));
        nd[isl] = child_desc;
        std::memcpy(nd + isl + 1, od + isl, (oc - isl) * sizeof(uint16_t));

        dealloc_node(alloc, node, h->alloc_u64());
        return nn;
    }

    // --- Shared remove child core: works for any header size ---
    static uint64_t* remove_child_at_(uint64_t* node, node_header* h, size_t hs,
                                       int slot, uint8_t idx, ALLOC& alloc) {
        unsigned oc = h->entries();
        unsigned nc = oc - 1;
        if (nc == 0) {
            dealloc_node(alloc, node, h->alloc_u64());
            return nullptr;
        }

        size_t needed = bitmask_size_u64(nc, hs);

        // In-place
        if (!should_shrink_u64(h->alloc_u64(), needed)) {
            // Save desc excluding slot
            uint16_t saved_desc[256];
            const uint16_t* od = desc_array_(node, hs, oc);
            std::memcpy(saved_desc, od, slot * sizeof(uint16_t));
            std::memcpy(saved_desc + slot, od + slot + 1, (nc - slot) * sizeof(uint16_t));

            // Remove child
            bitmap256::arr_remove(bm_mut_(node, hs), real_children_mut_(node, hs),
                                  oc, slot, idx);
            h->set_entries(nc);

            // Write back desc at new position
            uint16_t* nd = desc_array_mut_(node, hs, nc);
            std::memcpy(nd, saved_desc, nc * sizeof(uint16_t));
            return node;
        }

        // Realloc
        size_t au64 = round_up_u64(needed);
        uint64_t* nn = alloc_node(alloc, au64);

        // Copy header + embeds/bitmap + sentinel
        size_t prefix_u64 = hs + BITMAP256_U64 + 1;
        std::memcpy(nn, node, prefix_u64 * 8);

        auto* nh = get_header(nn);
        nh->set_entries(nc);
        nh->set_alloc_u64(au64);

        bm_mut_(nn, hs).clear_bit(idx);
        children_mut_(nn, hs)[0] = SENTINEL_TAGGED;

        bitmap256::arr_copy_remove(real_children_(node, hs), real_children_mut_(nn, hs),
                                    oc, slot);

        // Copy desc excluding slot
        const uint16_t* od = desc_array_(node, hs, oc);
        uint16_t* nd = desc_array_mut_(nn, hs, nc);
        std::memcpy(nd, od, slot * sizeof(uint16_t));
        std::memcpy(nd + slot, od + slot + 1, (nc - slot) * sizeof(uint16_t));

        dealloc_node(alloc, node, h->alloc_u64());
        return nn;
    }

    // --- Shared lookup core: works for any header size ---
    static child_lookup lookup_at_(const uint64_t* node, size_t hs, uint8_t idx) noexcept {
        const bitmap256& bm = bm_(node, hs);
        int slot = bm.find_slot<slot_mode::FAST_EXIT>(idx);
        if (slot < 0) return {0, -1, false};
        uint64_t child = real_children_(node, hs)[slot];
        return {child, slot, true};
    }

    // --- Shared: bitmap starts after header (1 or 2 u64s) ---
    static const bitmap256& bm_(const uint64_t* n, size_t header_size) noexcept {
        return *reinterpret_cast<const bitmap256*>(n + header_size);
    }
    static bitmap256& bm_mut_(uint64_t* n, size_t header_size) noexcept {
        return *reinterpret_cast<bitmap256*>(n + header_size);
    }

    // --- Bitmask node: children array (includes sentinel at [0]) ---
    static const uint64_t* children_(const uint64_t* n, size_t header_size) noexcept {
        return n + header_size + BITMAP256_U64;
    }
    static uint64_t* children_mut_(uint64_t* n, size_t header_size) noexcept {
        return n + header_size + BITMAP256_U64;
    }

    // --- Bitmask node: real children (past sentinel) ---
    static const uint64_t* real_children_(const uint64_t* n, size_t header_size) noexcept {
        return n + header_size + BITMAP256_U64 + 1;
    }
    static uint64_t* real_children_mut_(uint64_t* n, size_t header_size) noexcept {
        return n + header_size + BITMAP256_U64 + 1;
    }

    // --- Bitmap256 leaf: values after bitmap ---
    static const VST* bl_vals_(const uint64_t* n, size_t header_size) noexcept {
        return reinterpret_cast<const VST*>(n + header_size + BITMAP256_U64);
    }
    static VST* bl_vals_mut_(uint64_t* n, size_t header_size) noexcept {
        return reinterpret_cast<VST*>(n + header_size + BITMAP256_U64);
    }

    // --- Bitmask node: desc array (after real children) ---
    static const uint16_t* desc_array_(const uint64_t* n, size_t header_size, unsigned nc) noexcept {
        return reinterpret_cast<const uint16_t*>(n + header_size + BITMAP256_U64 + 1 + nc);
    }
    static uint16_t* desc_array_mut_(uint64_t* n, size_t header_size, unsigned nc) noexcept {
        return reinterpret_cast<uint16_t*>(n + header_size + BITMAP256_U64 + 1 + nc);
    }
};

} // namespace gteitelbaum

#endif // KNTRIE_BITMASK_HPP
