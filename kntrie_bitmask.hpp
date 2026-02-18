#ifndef KNTRIE_BITMASK_HPP
#define KNTRIE_BITMASK_HPP

#include "kntrie_support.hpp"

namespace gteitelbaum {

// ==========================================================================
// 256-bit bitmap
// ==========================================================================

enum class slot_mode { FAST_EXIT, BRANCHLESS, UNFILTERED };

struct bitmap_256_t {
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
        uint64_t w0 = words[0], w1 = words[1], w2 = words[2];
        int idx = !w0 + (!w0 & !w1) + (!w0 & !w1 & !w2);
        return static_cast<uint8_t>((idx << 6) + std::countr_zero(words[idx]));
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
        uint64_t w0 = words[0], w1 = words[1], w2 = words[2];
        int idx = !w0 + (!w0 & !w1) + (!w0 & !w1 & !w2);
        return static_cast<uint8_t>((idx << 6) + std::countr_zero(words[idx]));
    }

    // Return the highest set bit index. Undefined if bitmap is empty.
    uint8_t last_set_bit() const noexcept {
        uint64_t w1 = words[1], w2 = words[2], w3 = words[3];
        int idx = 3 - (!w3 + (!w3 & !w2) + (!w3 & !w2 & !w1));
        return static_cast<uint8_t>((idx << 6) + 63 - std::countl_zero(words[idx]));
    }

    struct adj_result { uint8_t idx; uint16_t slot; bool found; };

    // Find smallest set bit > idx, with its slot.
    adj_result next_set_after(uint8_t idx) const noexcept {
        if (idx == 255) return {0, 0, false};
        int start = idx + 1;
        int w = start >> 6, b = start & 63;

        // Branchless prefix popcount (same pattern as find_slot)
        int slot = std::popcount(words[0]) & -int(w > 0);
        slot += std::popcount(words[1]) & -int(w > 1);
        slot += std::popcount(words[2]) & -int(w > 2);

        // Mask out bits below start in starting word
        uint64_t m = words[w] & (~0ULL << b);
        if (m) {
            int bit = (w << 6) + std::countr_zero(m);
            slot += std::popcount(words[w] & ((1ULL << (bit & 63)) - 1));
            return {static_cast<uint8_t>(bit), static_cast<uint16_t>(slot), true};
        }
        slot += std::popcount(words[w]);

        // Search remaining words (at most 3 iterations, typically 0-1)
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

    // Find largest set bit < idx, with its slot.
    adj_result prev_set_before(uint8_t idx) const noexcept {
        if (idx == 0) return {0, 0, false};
        int last = idx - 1;
        int w = last >> 6, b = last & 63;

        // Mask off bits above 'last' in starting word
        uint64_t m = words[w] & ((2ULL << b) - 1);
        if (m) {
            int bit = (w << 6) + 63 - std::countl_zero(m);
            // Branchless prefix popcount
            int slot = std::popcount(words[0]) & -int(w > 0);
            slot += std::popcount(words[1]) & -int(w > 1);
            slot += std::popcount(words[2]) & -int(w > 2);
            slot += std::popcount(m & ((1ULL << (bit & 63)) - 1));
            return {static_cast<uint8_t>(bit), static_cast<uint16_t>(slot), true};
        }

        // Search lower words
        for (int ww = w - 1; ww >= 0; --ww) {
            if (words[ww]) {
                int bit = (ww << 6) + 63 - std::countl_zero(words[ww]);
                int slot = std::popcount(words[0]) & -int(ww > 0);
                slot += std::popcount(words[1]) & -int(ww > 1);
                slot += std::popcount(words[2]) & -int(ww > 2);
                slot += std::popcount(words[ww] & ((1ULL << (bit & 63)) - 1));
                return {static_cast<uint8_t>(bit), static_cast<uint16_t>(slot), true};
            }
        }
        return {0, 0, false};
    }

    static bitmap_256_t from_indices(const uint8_t* indices, unsigned count) noexcept {
        bitmap_256_t bm{};
        for (unsigned i = 0; i < count; ++i) bm.set_bit(indices[i]);
        return bm;
    }

    // Fill dest in bitmap order from unsorted (indices, tagged_ptrs)
    static void arr_fill_sorted(const bitmap_256_t& bm, uint64_t* dest,
                                const uint8_t* indices, const uint64_t* tagged_ptrs,
                                unsigned count) noexcept {
        for (unsigned i = 0; i < count; ++i)
            dest[bm.find_slot<slot_mode::UNFILTERED>(indices[i])] = tagged_ptrs[i];
    }

    // In-place insert: memmove right, write new entry, set bit
    static void arr_insert(bitmap_256_t& bm, uint64_t* arr, unsigned count,
                           uint8_t idx, uint64_t val) noexcept {
        int isl = bm.find_slot<slot_mode::UNFILTERED>(idx);
        std::memmove(arr + isl + 1, arr + isl, (count - isl) * sizeof(uint64_t));
        arr[isl] = val;
        bm.set_bit(idx);
    }

    // In-place remove: memmove left, clear bit
    static void arr_remove(bitmap_256_t& bm, uint64_t* arr, unsigned count,
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
// bitmask_ops  -- unified bitmask node + bitmap_256_t leaf operations
//
// Bitmask node (internal): [header(1)][bitmap(4)][sentinel(1)][children(n)][desc(n u16)]
//   - Parent pointer targets &node[1] (bitmap), no LEAF_BIT
//   - sentinel at offset 5 from bitmap = SENTINEL_TAGGED for branchless miss
//   - real children at offset 5 from bitmap (after sentinel)
//   - All children are tagged uint64_t values
//   - desc array: uint64_t per child, stores exact child descendant count
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
        return hu + BITMAP_256_U64 + 1 + n_children + desc_u64(n_children);
    }

    static constexpr size_t bitmap_leaf_size_u64(size_t count, size_t hu = HEADER_U64) noexcept {
        size_t vb = count * sizeof(VST);
        vb = (vb + 7) & ~size_t{7};
        return hu + BITMAP_256_U64 + vb / 8;
    }

    // ==================================================================
    // Bitmask node: branchless descent (for find) — tagged version
    // Takes bitmap pointer directly (not node pointer).
    // Returns tagged uint64_t (next child or SENTINEL_TAGGED).
    // ==================================================================

    static uint64_t branchless_find_tagged(const uint64_t* bm_ptr, uint8_t idx) noexcept {
        const bitmap_256_t& bm = *reinterpret_cast<const bitmap_256_t*>(bm_ptr);
        int slot = bm.find_slot<slot_mode::BRANCHLESS>(idx);  // 0 on miss -> sentinel
        return bm_ptr[BITMAP_256_U64 + slot];  // [0-3]=bitmap, [4]=sentinel, [5+]=children
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
        return lookup_at(node, 1, idx);
    }

    // ==================================================================
    // Bitmask node: set child pointer (tagged)
    // ==================================================================

    static void set_child(uint64_t* node, int slot, uint64_t tagged_ptr) noexcept {
        real_children_mut(node, 1)[slot] = tagged_ptr;
    }

    // ==================================================================
    // Skip chain read accessors
    // ==================================================================

    // Read single skip byte at embed position e (0-based)
    static uint8_t skip_byte(const uint64_t* node, uint8_t e) noexcept {
        const auto* embed_bm = reinterpret_cast<const bitmap_256_t*>(node + 1 + static_cast<size_t>(e) * 6);
        return embed_bm->single_bit_index();
    }

    // Copy all skip bytes to buffer
    static void skip_bytes(const uint64_t* node, uint8_t sc, uint8_t* out) noexcept {
        for (uint8_t e = 0; e < sc; ++e)
            out[e] = skip_byte(node, e);
    }

    // Lookup in the final bitmap of a skip chain
    static child_lookup chain_lookup(const uint64_t* node, uint8_t sc, uint8_t idx) noexcept {
        return lookup_at(node, chain_hs(sc), idx);
    }

    // Read tagged child at slot in final bitmap of skip chain
    static uint64_t chain_child(const uint64_t* node, uint8_t sc, int slot) noexcept {
        return real_children(node, chain_hs(sc))[slot];
    }

    // Write tagged child at slot in final bitmap of skip chain
    static void chain_set_child(uint64_t* node, uint8_t sc, int slot, uint64_t tagged) noexcept {
        real_children_mut(node, chain_hs(sc))[slot] = tagged;
    }

    // Descendants count of a skip chain (const)
    static uint64_t chain_descendants(const uint64_t* node, uint8_t sc, unsigned nc) noexcept {
        return *descendants_ptr(node, chain_hs(sc), nc);
    }

    // Descendants count of a skip chain (mutable ref)
    static uint64_t& chain_descendants_mut(uint64_t* node, uint8_t sc, unsigned nc) noexcept {
        return *descendants_ptr_mut(node, chain_hs(sc), nc);
    }

    // Final bitmap reference (const)
    static const bitmap_256_t& chain_bitmap(const uint64_t* node, uint8_t sc) noexcept {
        return bm(node, chain_hs(sc));
    }

    // Number of children in final bitmap
    static unsigned chain_child_count(const uint64_t* node, uint8_t sc) noexcept {
        return static_cast<unsigned>(chain_bitmap(node, sc).popcount());
    }

    // Pointer to real children array in final bitmap
    static const uint64_t* chain_children(const uint64_t* node, uint8_t sc) noexcept {
        return real_children(node, chain_hs(sc));
    }
    static uint64_t* chain_children_mut(uint64_t* node, uint8_t sc) noexcept {
        return real_children_mut(node, chain_hs(sc));
    }

    // Iterate final bitmap children: cb(slot, tagged_child)
    template<typename Fn>
    static void chain_for_each_child(const uint64_t* node, uint8_t sc, Fn&& cb) noexcept {
        size_t hs = chain_hs(sc);
        unsigned nc = static_cast<unsigned>(bm(node, hs).popcount());
        const uint64_t* ch = real_children(node, hs);
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
    static const bitmap_256_t& bitmap_ref(uint64_t bm_tagged) noexcept {
        return *reinterpret_cast<const bitmap_256_t*>(bm_tagged);
    }

    // Read tagged child at slot from a bitmask tagged pointer
    static uint64_t child_at(uint64_t bm_tagged, int slot) noexcept {
        const uint64_t* bm = reinterpret_cast<const uint64_t*>(bm_tagged);
        return bm[BITMAP_256_U64 + 1 + slot];
    }

    // First child (slot 0) from a bitmask tagged pointer
    static uint64_t first_child(uint64_t bm_tagged) noexcept {
        const uint64_t* bm = reinterpret_cast<const uint64_t*>(bm_tagged);
        return bm[BITMAP_256_U64 + 1];
    }

    // ==================================================================
    // Bitmask node: add child (tagged) — standalone
    // ==================================================================

    static uint64_t* add_child(uint64_t* node, node_header_t* h,
                                uint8_t idx, uint64_t child_tagged,
                                ALLOC& alloc) {
        return add_child_at(node, h, 1, idx, child_tagged, alloc);
    }

    // ==================================================================
    // Skip chain: add child to final bitmask
    // ==================================================================

    static uint64_t* chain_add_child(uint64_t* node, node_header_t* h,
                                      uint8_t sc, uint8_t idx,
                                      uint64_t child_tagged,
                                      ALLOC& alloc) {
        uint64_t* nn = add_child_at(node, h, chain_hs(sc), idx,
                                      child_tagged, alloc);
        if (nn != node && sc > 0) fix_embeds(nn, sc);
        return nn;
    }

    // ==================================================================
    // Bitmask node: remove child — standalone
    // Returns nullptr if node becomes empty.
    // ==================================================================

    static uint64_t* remove_child(uint64_t* node, node_header_t* h,
                                   int slot, uint8_t idx, ALLOC& alloc) {
        return remove_child_at(node, h, 1, slot, idx, alloc);
    }

    // ==================================================================
    // Skip chain: remove child from final bitmask
    // ==================================================================

    static uint64_t* chain_remove_child(uint64_t* node, node_header_t* h,
                                         uint8_t sc, int slot, uint8_t idx,
                                         ALLOC& alloc) {
        uint64_t* nn = remove_child_at(node, h, chain_hs(sc), slot, idx, alloc);
        if (nn && nn != node && sc > 0) fix_embeds(nn, sc);
        return nn;
    }

    // ==================================================================
    // Bitmask node: make from arrays (tagged children)
    // ==================================================================

    static uint64_t* make_bitmask(const uint8_t* indices,
                                   const uint64_t* child_tagged_ptrs,
                                   unsigned n_children, ALLOC& alloc,
                                   uint64_t descendants_ = 0) {
        bitmap_256_t bm = bitmap_256_t::from_indices(indices, n_children);

        constexpr size_t hs = 1;
        size_t needed = bitmask_size_u64(n_children, hs);
        size_t au64 = round_up_u64(needed);
        uint64_t* nn = alloc_node(alloc, au64);
        auto* nh = get_header(nn);
        nh->set_entries(n_children);
        nh->set_alloc_u64(au64);
        nh->set_skip(0);

        bm_mut(nn, hs) = bm;
        children_mut(nn, hs)[0] = SENTINEL_TAGGED;

        bitmap_256_t::arr_fill_sorted(bm, real_children_mut(nn, hs),
                                    indices, child_tagged_ptrs, n_children);

        *descendants_ptr_mut(nn, hs, n_children) = descendants_;
        return nn;
    }

    // ==================================================================
    // Bitmask node: make skip chain (one allocation)
    //
    // Layout: [header(1)][embed_0(6)]...[embed_{S-1}(6)][final_bm(4)][sent(1)][children(N)]
    // Each embed = bitmap_256_t(4) + sentinel(1) + child_ptr(1)
    // child_ptr points to next embed's bitmap (or final bitmap).
    // Total: 1 + S*6 + 4 + 1 + 1 + N = 7 + S*6 + N  (but we use 6 + S*6 + N since
    //        final_bm(4)+sent(1)+first_child_slot = 6, and remaining N-1 children follow,
    //        but actually: header(1) + S*6 + 4 + 1 + N = 6 + S*6 + N)
    // ==================================================================

    static uint64_t* make_skip_chain(const uint8_t* skip_bytes, uint8_t skip_count,
                                      const uint8_t* final_indices,
                                      const uint64_t* final_children_tagged,
                                      unsigned final_n_children, ALLOC& alloc,
                                      uint64_t descendants_ = 0) {
        // Allocation: header(1) + skip_count*6 + bitmap(4) + sentinel(1) + children(N) + desc(1)
        size_t needed = 1 + static_cast<size_t>(skip_count) * 6 + 5 + final_n_children
                       + desc_u64(final_n_children);
        size_t au64 = round_up_u64(needed);
        uint64_t* nn = alloc_node(alloc, au64);

        auto* nh = get_header(nn);
        nh->set_entries(final_n_children);
        nh->set_alloc_u64(au64);
        nh->set_skip(skip_count);

        // Build each embed: bitmap(4) + sentinel(1) + child_ptr(1)
        for (uint8_t e = 0; e < skip_count; ++e) {
            uint64_t* embed = nn + 1 + e * 6;
            // bitmap with single bit
            bitmap_256_t& bm = *reinterpret_cast<bitmap_256_t*>(embed);
            bm = bitmap_256_t{};
            bm.set_bit(skip_bytes[e]);
            // sentinel
            embed[4] = SENTINEL_TAGGED;
            // child ptr → next embed's bitmap (or final bitmap)
            uint64_t* next_bm = nn + 1 + (e + 1) * 6;
            embed[5] = reinterpret_cast<uint64_t>(next_bm);  // no LEAF_BIT
        }

        // Final bitmask
        size_t final_offset = 1 + static_cast<size_t>(skip_count) * 6;
        bitmap_256_t fbm = bitmap_256_t::from_indices(final_indices, final_n_children);
        *reinterpret_cast<bitmap_256_t*>(nn + final_offset) = fbm;
        nn[final_offset + 4] = SENTINEL_TAGGED;
        bitmap_256_t::arr_fill_sorted(fbm, nn + final_offset + 5,
                                    final_indices, final_children_tagged,
                                    final_n_children);

        // Single descendants count (after children)
        nn[final_offset + 5 + final_n_children] = descendants_;

        return nn;
    }

    // ==================================================================
    // build_remainder: extract embeds [from..sc-1] + final bitmask
    //                  into a new standalone bitmask or skip chain.
    //
    // Returns: tagged pointer (bitmask or skip chain).
    // ==================================================================

    static uint64_t build_remainder(uint64_t* old_node, uint8_t old_sc,
                                     uint8_t from_pos, ALLOC& alloc) {
        uint8_t rem_skip = old_sc - from_pos;
        unsigned final_nc = get_header(old_node)->entries();
        uint64_t old_descendants = chain_descendants(old_node, old_sc, final_nc);

        // Extract final bitmask indices + children
        const bitmap_256_t& fbm = chain_bitmap(old_node, old_sc);
        const uint64_t* old_ch = chain_children(old_node, old_sc);

        uint8_t indices[256];
        uint64_t children[256];
        fbm.for_each_set([&](uint8_t idx, int slot) {
            indices[slot] = idx;
            children[slot] = old_ch[slot];
        });

        if (rem_skip == 0) {
            return tag_bitmask(
                make_bitmask(indices, children, final_nc, alloc, old_descendants));
        }

        // rem_skip > 0: extract skip bytes for [from_pos..old_sc-1]
        uint8_t sb[6];
        for (uint8_t i = 0; i < rem_skip; ++i)
            sb[i] = skip_byte(old_node, from_pos + i);

        return tag_bitmask(
            make_skip_chain(sb, rem_skip, indices, children, final_nc, alloc, old_descendants));
    }

    // ==================================================================
    // wrap_in_chain: wrap an existing bitmask node in a skip chain
    //               with prepended skip bytes.
    //
    // Deallocates the old child node. Returns tagged pointer.
    // ==================================================================

    static uint64_t wrap_in_chain(uint64_t* child, const uint8_t* bytes,
                                   uint8_t count, ALLOC& alloc) {
        auto* ch = get_header(child);
        uint8_t child_sc = ch->skip();
        unsigned nc = ch->entries();
        uint64_t old_descendants = chain_descendants(child, child_sc, nc);

        // Collect all skip bytes: new prefix + child's existing skip
        uint8_t all_bytes[12];
        std::memcpy(all_bytes, bytes, count);
        skip_bytes(child, child_sc, all_bytes + count);
        uint8_t total_skip = count + child_sc;

        // Extract final bitmask indices + children
        const bitmap_256_t& fbm = chain_bitmap(child, child_sc);
        const uint64_t* cch = chain_children(child, child_sc);

        uint8_t indices[256];
        uint64_t children[256];
        fbm.for_each_set([&](uint8_t idx, int slot) {
            indices[slot] = idx;
            children[slot] = cch[slot];
        });

        auto* new_chain = make_skip_chain(all_bytes, total_skip, indices, children,
                                          nc, alloc, old_descendants);
        dealloc_node(alloc, child, ch->alloc_u64());
        return tag_bitmask(new_chain);
    }

    // ==================================================================
    // collapse_info: info needed for single-child collapse
    // ==================================================================

    struct collapse_info {
        uint64_t sole_child;       // tagged child pointer
        uint8_t  bytes[7];         // skip bytes to prepend (skip_bytes[0..sc-1] + sole_idx)
        uint8_t  total_skip;       // sc + 1
        uint64_t sole_entries;     // exact subtree count of sole_child
    };

    // Extract collapse info from a skip chain (sc > 0, entries() == 1)
    static collapse_info chain_collapse_info(const uint64_t* node, uint8_t sc) noexcept {
        collapse_info ci;
        skip_bytes(node, sc, ci.bytes);
        ci.bytes[sc] = chain_bitmap(node, sc).first_set_bit();
        ci.total_skip = sc + 1;
        ci.sole_child = chain_children(node, sc)[0];
        ci.sole_entries = chain_descendants(node, sc, 1);
        return ci;
    }

    // Extract collapse info from a standalone bitmask (sc == 0, entries() == 1)
    static collapse_info standalone_collapse_info(const uint64_t* node) noexcept {
        collapse_info ci;
        const bitmap_256_t& bmp = bm(node, 1);
        ci.bytes[0] = bmp.first_set_bit();
        ci.total_skip = 1;
        ci.sole_child = real_children(node, 1)[0];
        ci.sole_entries = *descendants_ptr(node, 1, 1);
        return ci;
    }

    // ==================================================================
    // Bitmask node: iterate  cb(uint8_t idx, int slot, uint64_t tagged_child)
    // ==================================================================

    // Exact subtree count from a tagged pointer.
    // Leaf: entries (exact). Bitmask: descendants (exact).
    static uint64_t exact_subtree_count(uint64_t tagged) noexcept {
        if (tagged & LEAF_BIT)
            return get_header(untag_leaf(tagged))->entries();
        const uint64_t* node = bm_to_node_const(tagged);
        auto* hdr = get_header(node);
        return chain_descendants(node, hdr->skip(), hdr->entries());
    }

    // ==================================================================

    template<typename Fn>
    static void for_each_child(const uint64_t* node, Fn&& cb) {
        constexpr size_t hs = 1;
        const bitmap_256_t& bmp = bm(node, hs);
        const uint64_t* rch = real_children(node, hs);
        bmp.for_each_set([&](uint8_t idx, int slot) {
            cb(idx, slot, rch[slot]);
        });
    }

    // ==================================================================
    // Bitmask node: child count / alloc
    // ==================================================================

    static int child_count(const uint64_t* node) noexcept {
        return get_header(node)->entries();
    }

    // --- Descendants count accessors (standalone bitmask, hs=1) ---
    static uint64_t node_descendants(const uint64_t* node) noexcept {
        constexpr size_t hs = 1;
        unsigned nc = get_header(node)->entries();
        return *descendants_ptr(node, hs, nc);
    }
    static uint64_t& node_descendants_mut(uint64_t* node) noexcept {
        constexpr size_t hs = 1;
        unsigned nc = get_header(node)->entries();
        return *descendants_ptr_mut(node, hs, nc);
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

    static const VALUE* bitmap_find(const uint64_t* node, node_header_t h,
                                     uint8_t suffix, size_t header_size) noexcept {
        const bitmap_256_t& bmp = bm(node, header_size);
        int slot = bmp.find_slot<slot_mode::FAST_EXIT>(suffix);
        if (slot < 0) [[unlikely]] return nullptr;
        return VT::as_ptr(bl_vals(node, header_size)[slot]);
    }

    // ==================================================================
    // Bitmap256 leaf: iterator helpers
    // ==================================================================

    struct iter_bm_result { uint8_t suffix; const VST* value; bool found; };

    static iter_bm_result bitmap_iter_first(const uint64_t* node,
                                             size_t header_size) noexcept {
        const bitmap_256_t& bmp = bm(node, header_size);
        const VST* vd = bl_vals(node, header_size);
        return {bmp.first_set_bit(), &vd[0], true};
    }

    static iter_bm_result bitmap_iter_last(const uint64_t* node,
                                            node_header_t h,
                                            size_t header_size) noexcept {
        const bitmap_256_t& bmp = bm(node, header_size);
        const VST* vd = bl_vals(node, header_size);
        unsigned count = h.entries();
        return {bmp.last_set_bit(), &vd[count - 1], true};
    }

    static iter_bm_result bitmap_iter_next(const uint64_t* node,
                                            uint8_t suffix,
                                            size_t header_size) noexcept {
        const bitmap_256_t& bmp = bm(node, header_size);
        auto r = bmp.next_set_after(suffix);
        if (!r.found) return {0, nullptr, false};
        const VST* vd = bl_vals(node, header_size);
        return {r.idx, &vd[r.slot], true};
    }

    static iter_bm_result bitmap_iter_prev(const uint64_t* node,
                                            uint8_t suffix,
                                            size_t header_size) noexcept {
        const bitmap_256_t& bmp = bm(node, header_size);
        auto r = bmp.prev_set_before(suffix);
        if (!r.found) return {0, nullptr, false};
        const VST* vd = bl_vals(node, header_size);
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
        bitmap_256_t& bm = bm_mut(node, hs);
        unsigned count = h->entries();
        VST* vd = bl_vals_mut(node, hs);

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
        bitmap_256_t& nbm = bm_mut(nn, hs);
        nbm = bm;
        nbm.set_bit(suffix);
        VST* nvd = bl_vals_mut(nn, hs);
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
        bitmap_256_t& bm = bm_mut(node, hs);
        if (!bm.has_bit(suffix)) return {tag_leaf(node), false, 0};

        unsigned count = h->entries();
        int slot = bm.find_slot<slot_mode::UNFILTERED>(suffix);
        VT::destroy(bl_vals_mut(node, hs)[slot], alloc);

        unsigned nc = count - 1;
        if (nc == 0) {
            dealloc_node(alloc, node, h->alloc_u64());
            return {0, true, 0};
        }

        size_t new_sz = bitmap_leaf_size_u64(nc, hs);

        // In-place
        if (!should_shrink_u64(h->alloc_u64(), new_sz)) {
            VST* vd = bl_vals_mut(node, hs);
            bm.clear_bit(suffix);
            std::memmove(vd + slot, vd + slot + 1, (nc - slot) * sizeof(VST));
            h->set_entries(nc);
            return {tag_leaf(node), true, nc};
        }

        // Realloc
        size_t au64 = round_up_u64(new_sz);
        uint64_t* nn = alloc_node(alloc, au64);
        auto* nh = get_header(nn);
        *nh = *h;
        if (h->is_skip()) nn[1] = reinterpret_cast<const uint64_t*>(h)[1];
        nh->set_entries(nc);
        nh->set_alloc_u64(au64);
        bm_mut(nn, hs) = bm;
        bm_mut(nn, hs).clear_bit(suffix);
        const VST* ov = bl_vals(node, hs);
        VST*       nv = bl_vals_mut(nn, hs);
        std::memcpy(nv, ov, slot * sizeof(VST));
        std::memcpy(nv + slot, ov + slot + 1, (nc - slot) * sizeof(VST));

        dealloc_node(alloc, node, h->alloc_u64());
        return {tag_leaf(nn), true, nc};
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
        bitmap_256_t& bm = bm_mut(node, hs);
        bm = bitmap_256_t{};
        for (unsigned i = 0; i < count; ++i) bm.set_bit(sorted_suffixes[i]);
        VST* vd = bl_vals_mut(node, hs);
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
        bm_mut(node, hs).set_bit(suffix);
        VT::write_slot(&bl_vals_mut(node, hs)[0], value);
        return node;
    }

    // ==================================================================
    // Bitmap256 leaf: iterate  cb(uint8_t suffix, VST value_slot)
    // ==================================================================

    template<typename Fn>
    static void for_each_bitmap(const uint64_t* node, Fn&& cb) {
        size_t hs = hdr_u64(node);
        const bitmap_256_t& bmp = bm(node, hs);
        const VST* vd = bl_vals(node, hs);
        bmp.for_each_set([&](uint8_t idx, int slot) {
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
            VST* vd = bl_vals_mut(node, hdr_u64(node));
            for (uint16_t i = 0; i < count; ++i)
                VT::destroy(vd[i], alloc);
        }
        dealloc_node(alloc, node, h->alloc_u64());
    }

    // --- Chain header size: 1 (base header) + sc * 6 (embed slots) ---
    static constexpr size_t chain_hs(uint8_t sc) noexcept {
        return 1 + static_cast<size_t>(sc) * 6;
    }

private:
    // --- Fix embed internal pointers after reallocation ---
    static void fix_embeds(uint64_t* nn, uint8_t sc) noexcept {
        for (uint8_t e = 0; e < sc; ++e) {
            uint64_t* next_bm = nn + 1 + static_cast<size_t>(e + 1) * 6;
            nn[1 + static_cast<size_t>(e) * 6 + 5] = reinterpret_cast<uint64_t>(next_bm);
        }
        // Fix sentinel of final bitmap
        size_t fo = chain_hs(sc);
        nn[fo + BITMAP_256_U64] = SENTINEL_TAGGED;
    }

    // --- Shared add child core: works for any header size ---
    static uint64_t* add_child_at(uint64_t* node, node_header_t* h, size_t hs,
                                    uint8_t idx, uint64_t child_tagged,
                                    ALLOC& alloc) {
        bitmap_256_t& bm = bm_mut(node, hs);
        unsigned oc = h->entries();
        unsigned nc = oc + 1;
        int isl = bm.find_slot<slot_mode::UNFILTERED>(idx);
        size_t needed = bitmask_size_u64(nc, hs);

        // In-place
        if (needed <= h->alloc_u64()) {
            // Save descendants (children shift will overwrite it)
            uint64_t saved = *descendants_ptr(node, hs, oc);

            // Insert child
            uint64_t* rch = real_children_mut(node, hs);
            std::memmove(rch + isl + 1, rch + isl, (oc - isl) * sizeof(uint64_t));
            rch[isl] = child_tagged;
            bm.set_bit(idx);
            h->set_entries(nc);

            // Write descendants at new position
            *descendants_ptr_mut(node, hs, nc) = saved;
            return node;
        }

        // Realloc
        uint64_t saved = *descendants_ptr(node, hs, oc);
        size_t au64 = round_up_u64(needed);
        uint64_t* nn = alloc_node(alloc, au64);

        // Copy header + embeds/bitmap + sentinel (everything before children)
        size_t prefix_u64 = hs + BITMAP_256_U64 + 1;
        std::memcpy(nn, node, prefix_u64 * 8);

        auto* nh = get_header(nn);
        nh->set_entries(nc);
        nh->set_alloc_u64(au64);

        bm_mut(nn, hs).set_bit(idx);
        children_mut(nn, hs)[0] = SENTINEL_TAGGED;

        bitmap_256_t::arr_copy_insert(real_children(node, hs), real_children_mut(nn, hs),
                                    oc, isl, child_tagged);

        *descendants_ptr_mut(nn, hs, nc) = saved;

        dealloc_node(alloc, node, h->alloc_u64());
        return nn;
    }

    // --- Shared remove child core: works for any header size ---
    static uint64_t* remove_child_at(uint64_t* node, node_header_t* h, size_t hs,
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
            uint64_t saved = *descendants_ptr(node, hs, oc);

            bitmap_256_t::arr_remove(bm_mut(node, hs), real_children_mut(node, hs),
                                  oc, slot, idx);
            h->set_entries(nc);

            *descendants_ptr_mut(node, hs, nc) = saved;
            return node;
        }

        // Realloc
        uint64_t saved = *descendants_ptr(node, hs, oc);
        size_t au64 = round_up_u64(needed);
        uint64_t* nn = alloc_node(alloc, au64);

        // Copy header + embeds/bitmap + sentinel
        size_t prefix_u64 = hs + BITMAP_256_U64 + 1;
        std::memcpy(nn, node, prefix_u64 * 8);

        auto* nh = get_header(nn);
        nh->set_entries(nc);
        nh->set_alloc_u64(au64);

        bm_mut(nn, hs).clear_bit(idx);
        children_mut(nn, hs)[0] = SENTINEL_TAGGED;

        bitmap_256_t::arr_copy_remove(real_children(node, hs), real_children_mut(nn, hs),
                                    oc, slot);

        *descendants_ptr_mut(nn, hs, nc) = saved;

        dealloc_node(alloc, node, h->alloc_u64());
        return nn;
    }

    // --- Shared lookup core: works for any header size ---
    static child_lookup lookup_at(const uint64_t* node, size_t hs, uint8_t idx) noexcept {
        const bitmap_256_t& bmp = bm(node, hs);
        int slot = bmp.find_slot<slot_mode::FAST_EXIT>(idx);
        if (slot < 0) return {0, -1, false};
        uint64_t child = real_children(node, hs)[slot];
        return {child, slot, true};
    }

    // --- Shared: bitmap starts after header (1 or 2 u64s) ---
    static const bitmap_256_t& bm(const uint64_t* n, size_t header_size) noexcept {
        return *reinterpret_cast<const bitmap_256_t*>(n + header_size);
    }
    static bitmap_256_t& bm_mut(uint64_t* n, size_t header_size) noexcept {
        return *reinterpret_cast<bitmap_256_t*>(n + header_size);
    }

    // --- Bitmask node: children array (includes sentinel at [0]) ---
    static const uint64_t* children(const uint64_t* n, size_t header_size) noexcept {
        return n + header_size + BITMAP_256_U64;
    }
    static uint64_t* children_mut(uint64_t* n, size_t header_size) noexcept {
        return n + header_size + BITMAP_256_U64;
    }

    // --- Bitmask node: real children (past sentinel) ---
    static const uint64_t* real_children(const uint64_t* n, size_t header_size) noexcept {
        return n + header_size + BITMAP_256_U64 + 1;
    }
    static uint64_t* real_children_mut(uint64_t* n, size_t header_size) noexcept {
        return n + header_size + BITMAP_256_U64 + 1;
    }

    // --- Bitmap256 leaf: values after bitmap ---
    static const VST* bl_vals(const uint64_t* n, size_t header_size) noexcept {
        return reinterpret_cast<const VST*>(n + header_size + BITMAP_256_U64);
    }
    static VST* bl_vals_mut(uint64_t* n, size_t header_size) noexcept {
        return reinterpret_cast<VST*>(n + header_size + BITMAP_256_U64);
    }

    // --- Bitmask node: descendants count (single u64 after children) ---
    static const uint64_t* descendants_ptr(const uint64_t* n, size_t header_size, unsigned nc) noexcept {
        return n + header_size + BITMAP_256_U64 + 1 + nc;
    }
    static uint64_t* descendants_ptr_mut(uint64_t* n, size_t header_size, unsigned nc) noexcept {
        return n + header_size + BITMAP_256_U64 + 1 + nc;
    }
};

} // namespace gteitelbaum

#endif // KNTRIE_BITMASK_HPP
