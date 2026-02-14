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

    static bitmap256 from_indices(const uint8_t* indices, int count) noexcept {
        bitmap256 bm{};
        for (int i = 0; i < count; ++i) bm.set_bit(indices[i]);
        return bm;
    }

    // Fill dest in bitmap order from unsorted (indices, ptrs)
    static void arr_fill_sorted(const bitmap256& bm, uint64_t* dest,
                                const uint8_t* indices, uint64_t* const* ptrs,
                                int count) noexcept {
        for (int i = 0; i < count; ++i)
            dest[bm.find_slot<slot_mode::UNFILTERED>(indices[i])] =
                reinterpret_cast<uint64_t>(ptrs[i]);
    }

    // In-place insert: memmove right, write new entry, set bit
    static void arr_insert(bitmap256& bm, uint64_t* arr, int count,
                           uint8_t idx, uint64_t val) noexcept {
        int isl = bm.find_slot<slot_mode::UNFILTERED>(idx);
        std::memmove(arr + isl + 1, arr + isl, (count - isl) * sizeof(uint64_t));
        arr[isl] = val;
        bm.set_bit(idx);
    }

    // In-place remove: memmove left, clear bit
    static void arr_remove(bitmap256& bm, uint64_t* arr, int count,
                           int slot, uint8_t idx) noexcept {
        std::memmove(arr + slot, arr + slot + 1, (count - 1 - slot) * sizeof(uint64_t));
        bm.clear_bit(idx);
    }

    // Copy old array into new array with a new entry inserted
    static void arr_copy_insert(const uint64_t* old_arr, uint64_t* new_arr,
                                int old_count, int isl, uint64_t val) noexcept {
        std::memcpy(new_arr, old_arr, isl * sizeof(uint64_t));
        new_arr[isl] = val;
        std::memcpy(new_arr + isl + 1, old_arr + isl,
                     (old_count - isl) * sizeof(uint64_t));
    }

    // Copy old array into new array with one entry removed
    static void arr_copy_remove(const uint64_t* old_arr, uint64_t* new_arr,
                                int old_count, int slot) noexcept {
        std::memcpy(new_arr, old_arr, slot * sizeof(uint64_t));
        std::memcpy(new_arr + slot, old_arr + slot + 1,
                     (old_count - 1 - slot) * sizeof(uint64_t));
    }
};

// ==========================================================================
// bitmask_ops  -- unified bitmask node + bitmap256 leaf operations
//
// Bitmask node (internal): [header(1)][bitmap(4)][sentinel(1)][children(n)]
//   - sentinel at offset 5 = SENTINEL_NODE ptr for branchless miss
//   - real children at offset 6
//   - No internal_bitmap — children are self-describing
//
// Bitmap256 leaf (suffix_type=0): [header(1)][bitmap(4)][values(n)]
//   - values at offset 5
//   - 8-bit suffix space, O(1) lookup via popcount
// ==========================================================================

template<typename VALUE, typename ALLOC>
struct bitmask_ops {
    using VT   = value_traits<VALUE, ALLOC>;
    using VST  = typename VT::slot_type;

    // ==================================================================
    // Size calculations
    // ==================================================================

    static constexpr size_t bitmask_size_u64(size_t n_children) noexcept {
        return HEADER_U64 + BITMAP256_U64 + 1 + n_children;  // 7 + n
    }

    static constexpr size_t bitmap_leaf_size_u64(size_t count) noexcept {
        size_t vb = count * sizeof(VST);
        vb = (vb + 7) & ~size_t{7};
        return HEADER_U64 + BITMAP256_U64 + vb / 8;  // 6 + ceil(n*VST/8)
    }

    // ==================================================================
    // Bitmask node: branchless descent (for find)
    // ==================================================================

    static const uint64_t* branchless_child(const uint64_t* node, uint8_t idx) noexcept {
        const bitmap256& bm = bm_(node);
        int slot = bm.find_slot<slot_mode::BRANCHLESS>(idx);  // 0 on miss -> sentinel
        return reinterpret_cast<const uint64_t*>(children_(node)[slot]);
    }

    // ==================================================================
    // Bitmask node: lookup child
    // ==================================================================

    struct child_lookup {
        uint64_t* child;
        int       slot;
        bool      found;
    };

    static child_lookup lookup(const uint64_t* node, uint8_t idx) noexcept {
        const bitmap256& bm = bm_(node);
        int slot = bm.find_slot<slot_mode::FAST_EXIT>(idx);
        if (slot < 0) return {nullptr, -1, false};
        auto* child = reinterpret_cast<uint64_t*>(real_children_(node)[slot]);
        return {child, slot, true};
    }

    // ==================================================================
    // Bitmask node: set child pointer
    // ==================================================================

    static void set_child(uint64_t* node, int slot, uint64_t* ptr) noexcept {
        real_children_mut_(node)[slot] = reinterpret_cast<uint64_t>(ptr);
    }

    // ==================================================================
    // Bitmask node: add child
    // ==================================================================

    static uint64_t* add_child(uint64_t* node, node_header* h,
                                uint8_t idx, uint64_t* child_ptr, ALLOC& alloc) {
        bitmap256& bm = bm_mut_(node);
        int oc = h->entries();
        int nc = oc + 1;
        size_t needed = bitmask_size_u64(nc);

        // In-place
        if (needed <= h->alloc_u64()) {
            bitmap256::arr_insert(bm, real_children_mut_(node), oc, idx,
                                  reinterpret_cast<uint64_t>(child_ptr));
            h->set_entries(static_cast<uint16_t>(nc));
            return node;
        }

        // Realloc
        int isl = bm.find_slot<slot_mode::UNFILTERED>(idx);
        size_t au64 = round_up_u64(needed);
        uint64_t* nn = alloc_node(alloc, au64);
        auto* nh = get_header(nn);
        *nh = *h;
        nh->set_entries(static_cast<uint16_t>(nc));
        nh->set_alloc_u64(static_cast<uint16_t>(au64));

        bm_mut_(nn) = bm;
        bm_mut_(nn).set_bit(idx);
        children_mut_(nn)[0] = reinterpret_cast<uint64_t>(SENTINEL_NODE);

        bitmap256::arr_copy_insert(real_children_(node), real_children_mut_(nn),
                                    oc, isl,
                                    reinterpret_cast<uint64_t>(child_ptr));

        dealloc_node(alloc, node, h->alloc_u64());
        return nn;
    }

    // ==================================================================
    // Bitmask node: remove child
    // Returns nullptr if node becomes empty.
    // ==================================================================

    static uint64_t* remove_child(uint64_t* node, node_header* h,
                                   int slot, uint8_t idx, ALLOC& alloc) {
        int oc = h->entries();
        int nc = oc - 1;
        if (nc == 0) {
            dealloc_node(alloc, node, h->alloc_u64());
            return nullptr;
        }

        size_t needed = bitmask_size_u64(nc);

        // In-place
        if (!should_shrink_u64(h->alloc_u64(), needed)) {
            bitmap256::arr_remove(bm_mut_(node), real_children_mut_(node),
                                  oc, slot, idx);
            h->set_entries(static_cast<uint16_t>(nc));
            return node;
        }

        // Realloc
        size_t au64 = round_up_u64(needed);
        uint64_t* nn = alloc_node(alloc, au64);
        auto* nh = get_header(nn);
        *nh = *h;
        nh->set_entries(static_cast<uint16_t>(nc));
        nh->set_alloc_u64(static_cast<uint16_t>(au64));

        bm_mut_(nn) = bm_(node);
        bm_mut_(nn).clear_bit(idx);
        children_mut_(nn)[0] = reinterpret_cast<uint64_t>(SENTINEL_NODE);

        bitmap256::arr_copy_remove(real_children_(node), real_children_mut_(nn),
                                    oc, slot);

        dealloc_node(alloc, node, h->alloc_u64());
        return nn;
    }

    // ==================================================================
    // Bitmask node: make from arrays
    // ==================================================================

    static uint64_t* make_bitmask(const uint8_t* indices,
                                   uint64_t* const* child_ptrs,
                                   int n_children, ALLOC& alloc) {
        bitmap256 bm = bitmap256::from_indices(indices, n_children);

        size_t needed = bitmask_size_u64(n_children);
        size_t au64 = round_up_u64(needed);
        uint64_t* nn = alloc_node(alloc, au64);
        auto* nh = get_header(nn);
        nh->set_entries(static_cast<uint16_t>(n_children));
        nh->set_alloc_u64(static_cast<uint16_t>(au64));
        nh->set_bitmask();

        bm_mut_(nn) = bm;
        children_mut_(nn)[0] = reinterpret_cast<uint64_t>(SENTINEL_NODE);

        bitmap256::arr_fill_sorted(bm, real_children_mut_(nn),
                                    indices, child_ptrs, n_children);
        return nn;
    }

    // ==================================================================
    // Bitmask node: iterate  cb(uint8_t idx, int slot, uint64_t* child)
    // ==================================================================

    template<typename Fn>
    static void for_each_child(const uint64_t* node, Fn&& cb) {
        const bitmap256& bm = bm_(node);
        const uint64_t* rch = real_children_(node);
        int slot = 0;
        for (int i = bm.find_next_set(0); i >= 0; i = bm.find_next_set(i + 1))
            cb(static_cast<uint8_t>(i), slot, reinterpret_cast<uint64_t*>(rch[slot++]));
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
                                     uint8_t suffix) noexcept {
        const bitmap256& bm = bm_(node);
        int slot = bm.find_slot<slot_mode::FAST_EXIT>(suffix);
        if (slot < 0) [[unlikely]] return nullptr;
        return VT::as_ptr(bl_vals_(node)[slot]);
    }

    // ==================================================================
    // Bitmap256 leaf: insert
    // ==================================================================

    template<bool INSERT = true, bool ASSIGN = true>
    requires (INSERT || ASSIGN)
    static insert_result_t bitmap_insert(uint64_t* node, uint8_t suffix,
                                          VST value, ALLOC& alloc) {
        auto* h = get_header(node);
        bitmap256& bm = bm_mut_(node);
        uint16_t count = h->entries();
        VST* vd = bl_vals_mut_(node);

        if (bm.has_bit(suffix)) {
            if constexpr (ASSIGN) {
                int slot = bm.find_slot<slot_mode::UNFILTERED>(suffix);
                VT::destroy(vd[slot], alloc);
                VT::write_slot(&vd[slot], value);
            }
            return {node, false, false};
        }

        if constexpr (!INSERT) return {node, false, false};

        uint16_t nc = count + 1;
        size_t new_sz = bitmap_leaf_size_u64(nc);

        // In-place
        if (new_sz <= h->alloc_u64()) {
            int isl = bm.find_slot<slot_mode::UNFILTERED>(suffix);
            bm.set_bit(suffix);
            std::memmove(vd + isl + 1, vd + isl, (count - isl) * sizeof(VST));
            VT::write_slot(&vd[isl], value);
            h->set_entries(nc);
            return {node, true, false};
        }

        // Realloc
        size_t au64 = round_up_u64(new_sz);
        uint64_t* nn = alloc_node(alloc, au64);
        auto* nh = get_header(nn);
        *nh = *h;
        nh->set_entries(nc);
        nh->set_alloc_u64(static_cast<uint16_t>(au64));
        bitmap256& nbm = bm_mut_(nn);
        nbm = bm;
        nbm.set_bit(suffix);
        VST* nvd = bl_vals_mut_(nn);
        int isl = nbm.find_slot<slot_mode::UNFILTERED>(suffix);
        std::memcpy(nvd, vd, isl * sizeof(VST));
        VT::write_slot(&nvd[isl], value);
        std::memcpy(nvd + isl + 1, vd + isl, (count - isl) * sizeof(VST));

        dealloc_node(alloc, node, h->alloc_u64());
        return {nn, true, false};
    }

    // ==================================================================
    // Bitmap256 leaf: erase
    // ==================================================================

    static erase_result_t bitmap_erase(uint64_t* node, uint8_t suffix,
                                        ALLOC& alloc) {
        auto* h = get_header(node);
        bitmap256& bm = bm_mut_(node);
        if (!bm.has_bit(suffix)) return {node, false};

        uint16_t count = h->entries();
        int slot = bm.find_slot<slot_mode::UNFILTERED>(suffix);
        VT::destroy(bl_vals_mut_(node)[slot], alloc);

        uint16_t nc = count - 1;
        if (nc == 0) {
            dealloc_node(alloc, node, h->alloc_u64());
            return {nullptr, true};
        }

        size_t new_sz = bitmap_leaf_size_u64(nc);

        // In-place
        if (!should_shrink_u64(h->alloc_u64(), new_sz)) {
            VST* vd = bl_vals_mut_(node);
            bm.clear_bit(suffix);
            std::memmove(vd + slot, vd + slot + 1, (nc - slot) * sizeof(VST));
            h->set_entries(nc);
            return {node, true};
        }

        // Realloc
        size_t au64 = round_up_u64(new_sz);
        uint64_t* nn = alloc_node(alloc, au64);
        auto* nh = get_header(nn);
        *nh = *h;
        nh->set_entries(nc);
        nh->set_alloc_u64(static_cast<uint16_t>(au64));
        bm_mut_(nn) = bm;
        bm_mut_(nn).clear_bit(suffix);
        const VST* ov = bl_vals_(node);
        VST*       nv = bl_vals_mut_(nn);
        std::memcpy(nv, ov, slot * sizeof(VST));
        std::memcpy(nv + slot, ov + slot + 1, (nc - slot) * sizeof(VST));

        dealloc_node(alloc, node, h->alloc_u64());
        return {nn, true};
    }

    // ==================================================================
    // Bitmap256 leaf: make from sorted suffixes
    // ==================================================================

    static uint64_t* make_bitmap_leaf(const uint8_t* sorted_suffixes,
                                       const VST* values, uint32_t count,
                                       ALLOC& alloc) {
        size_t sz = round_up_u64(bitmap_leaf_size_u64(count));
        uint64_t* node = alloc_node(alloc, sz);
        auto* h = get_header(node);
        h->set_entries(static_cast<uint16_t>(count));
        h->set_alloc_u64(static_cast<uint16_t>(sz));
        h->set_suffix_type(0);  // bitmap256 leaf
        bitmap256& bm = bm_mut_(node);
        bm = bitmap256{};
        for (uint32_t i = 0; i < count; ++i) bm.set_bit(sorted_suffixes[i]);
        VST* vd = bl_vals_mut_(node);
        for (uint32_t i = 0; i < count; ++i)
            vd[bm.find_slot<slot_mode::UNFILTERED>(sorted_suffixes[i])] = values[i];
        return node;
    }

    // ==================================================================
    // Bitmap256 leaf: make single entry
    // ==================================================================

    static uint64_t* make_single_bitmap(uint8_t suffix, VST value, ALLOC& alloc) {
        size_t sz = round_up_u64(bitmap_leaf_size_u64(1));
        uint64_t* node = alloc_node(alloc, sz);
        auto* h = get_header(node);
        h->set_entries(1);
        h->set_alloc_u64(static_cast<uint16_t>(sz));
        h->set_suffix_type(0);
        bm_mut_(node).set_bit(suffix);
        VT::write_slot(&bl_vals_mut_(node)[0], value);
        return node;
    }

    // ==================================================================
    // Bitmap256 leaf: iterate  cb(uint8_t suffix, VST value_slot)
    // ==================================================================

    template<typename Fn>
    static void for_each_bitmap(const uint64_t* node, Fn&& cb) {
        const bitmap256& bm = bm_(node);
        const VST* vd = bl_vals_(node);
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
            VST* vd = bl_vals_mut_(node);
            for (uint16_t i = 0; i < count; ++i)
                VT::destroy(vd[i], alloc);
        }
        dealloc_node(alloc, node, h->alloc_u64());
    }

private:
    // --- Shared: bitmap is always at HEADER_U64 offset ---
    static const bitmap256& bm_(const uint64_t* n) noexcept {
        return *reinterpret_cast<const bitmap256*>(n + HEADER_U64);
    }
    static bitmap256& bm_mut_(uint64_t* n) noexcept {
        return *reinterpret_cast<bitmap256*>(n + HEADER_U64);
    }

    // --- Bitmask node: children array (includes sentinel at [0]) ---
    static const uint64_t* children_(const uint64_t* n) noexcept {
        return n + HEADER_U64 + BITMAP256_U64;  // offset 5
    }
    static uint64_t* children_mut_(uint64_t* n) noexcept {
        return n + HEADER_U64 + BITMAP256_U64;
    }

    // --- Bitmask node: real children (past sentinel) ---
    static const uint64_t* real_children_(const uint64_t* n) noexcept {
        return n + HEADER_U64 + BITMAP256_U64 + 1;  // offset 6
    }
    static uint64_t* real_children_mut_(uint64_t* n) noexcept {
        return n + HEADER_U64 + BITMAP256_U64 + 1;
    }

    // --- Bitmap256 leaf: values after bitmap ---
    static const VST* bl_vals_(const uint64_t* n) noexcept {
        return reinterpret_cast<const VST*>(n + HEADER_U64 + BITMAP256_U64);  // offset 5
    }
    static VST* bl_vals_mut_(uint64_t* n) noexcept {
        return reinterpret_cast<VST*>(n + HEADER_U64 + BITMAP256_U64);
    }
};

} // namespace gteitelbaum

#endif // KNTRIE_BITMASK_HPP
