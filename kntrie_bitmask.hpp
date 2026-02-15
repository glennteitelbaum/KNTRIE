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

    int find_next_set(int start) const noexcept {
        if (start >= 256) return -1;
        int w = start >> 6, b = start & 63;
        uint64_t masked = words[w] & ~((1ULL << b) - 1);
        if (masked) return (w << 6) + std::countr_zero(masked);
        for (int ww = w + 1; ww < 4; ++ww)
            if (words[ww]) return (ww << 6) + std::countr_zero(words[ww]);
        return -1;
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
// Bitmask node (internal): [header(1)][bitmap(4)][sentinel(1)][children(n)]
//   - Parent pointer targets &node[1] (bitmap), no LEAF_BIT
//   - sentinel at offset 5 from bitmap = SENTINEL_TAGGED for branchless miss
//   - real children at offset 5 from bitmap (after sentinel)
//   - All children are tagged uint64_t values
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
        return hu + BITMAP256_U64 + 1 + n_children;
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
        constexpr size_t hs = 1;
        const bitmap256& bm = bm_(node, hs);
        int slot = bm.find_slot<slot_mode::FAST_EXIT>(idx);
        if (slot < 0) return {0, -1, false};
        uint64_t child = real_children_(node, hs)[slot];  // tagged value
        return {child, slot, true};
    }

    // ==================================================================
    // Bitmask node: set child pointer (tagged)
    // ==================================================================

    static void set_child(uint64_t* node, int slot, uint64_t tagged_ptr) noexcept {
        real_children_mut_(node, 1)[slot] = tagged_ptr;
    }

    // ==================================================================
    // Bitmask node: add child (tagged)
    // ==================================================================

    static uint64_t* add_child(uint64_t* node, node_header* h,
                                uint8_t idx, uint64_t child_tagged, ALLOC& alloc) {
        constexpr size_t hs = 1;
        bitmap256& bm = bm_mut_(node, hs);
        unsigned oc = h->entries();
        unsigned nc = oc + 1;
        size_t needed = bitmask_size_u64(nc, hs);

        // In-place
        if (needed <= h->alloc_u64()) {
            bitmap256::arr_insert(bm, real_children_mut_(node, hs), oc, idx,
                                  child_tagged);
            h->set_entries(nc);
            return node;
        }

        // Realloc
        int isl = bm.find_slot<slot_mode::UNFILTERED>(idx);
        size_t au64 = round_up_u64(needed);
        uint64_t* nn = alloc_node(alloc, au64);
        auto* nh = get_header(nn);
        *nh = *h;
        nh->set_entries(nc);
        nh->set_alloc_u64(au64);

        bm_mut_(nn, hs) = bm;
        bm_mut_(nn, hs).set_bit(idx);
        children_mut_(nn, hs)[0] = SENTINEL_TAGGED;

        bitmap256::arr_copy_insert(real_children_(node, hs), real_children_mut_(nn, hs),
                                    oc, isl, child_tagged);

        dealloc_node(alloc, node, h->alloc_u64());
        return nn;
    }

    // ==================================================================
    // Bitmask node: remove child
    // Returns nullptr if node becomes empty.
    // ==================================================================

    static uint64_t* remove_child(uint64_t* node, node_header* h,
                                   int slot, uint8_t idx, ALLOC& alloc) {
        unsigned oc = h->entries();
        unsigned nc = oc - 1;
        if (nc == 0) {
            dealloc_node(alloc, node, h->alloc_u64());
            return nullptr;
        }

        constexpr size_t hs = 1;
        size_t needed = bitmask_size_u64(nc, hs);

        // In-place
        if (!should_shrink_u64(h->alloc_u64(), needed)) {
            bitmap256::arr_remove(bm_mut_(node, hs), real_children_mut_(node, hs),
                                  oc, slot, idx);
            h->set_entries(nc);
            return node;
        }

        // Realloc
        size_t au64 = round_up_u64(needed);
        uint64_t* nn = alloc_node(alloc, au64);
        auto* nh = get_header(nn);
        *nh = *h;
        nh->set_entries(nc);
        nh->set_alloc_u64(au64);

        bm_mut_(nn, hs) = bm_(node, hs);
        bm_mut_(nn, hs).clear_bit(idx);
        children_mut_(nn, hs)[0] = SENTINEL_TAGGED;

        bitmap256::arr_copy_remove(real_children_(node, hs), real_children_mut_(nn, hs),
                                    oc, slot);

        dealloc_node(alloc, node, h->alloc_u64());
        return nn;
    }

    // ==================================================================
    // Bitmask node: make from arrays (tagged children)
    // ==================================================================

    static uint64_t* make_bitmask(const uint8_t* indices,
                                   const uint64_t* child_tagged_ptrs,
                                   unsigned n_children, ALLOC& alloc) {
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
        int slot = 0;
        for (int i = bm.find_next_set(0); i >= 0; i = bm.find_next_set(i + 1))
            cb(static_cast<uint8_t>(i), slot, rch[slot++]);
    }

    // ==================================================================
    // Bitmask node: child count / alloc
    // ==================================================================

    static int child_count(const uint64_t* node) noexcept {
        return get_header(node)->entries();
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
        if (!bm.has_bit(suffix)) return {tag_leaf(node), false};

        unsigned count = h->entries();
        int slot = bm.find_slot<slot_mode::UNFILTERED>(suffix);
        VT::destroy(bl_vals_mut_(node, hs)[slot], alloc);

        unsigned nc = count - 1;
        if (nc == 0) {
            dealloc_node(alloc, node, h->alloc_u64());
            return {0, true};
        }

        size_t new_sz = bitmap_leaf_size_u64(nc, hs);

        // In-place
        if (!should_shrink_u64(h->alloc_u64(), new_sz)) {
            VST* vd = bl_vals_mut_(node, hs);
            bm.clear_bit(suffix);
            std::memmove(vd + slot, vd + slot + 1, (nc - slot) * sizeof(VST));
            h->set_entries(nc);
            return {tag_leaf(node), true};
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
        return {tag_leaf(nn), true};
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
        int slot = 0;
        for (int i = bm.find_next_set(0); i >= 0; i = bm.find_next_set(i + 1))
            cb(static_cast<uint8_t>(i), vd[slot++]);
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

private:
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
};

} // namespace gteitelbaum

#endif // KNTRIE_BITMASK_HPP
