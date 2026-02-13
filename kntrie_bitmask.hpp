#ifndef KNTRIE_BITMASK_HPP
#define KNTRIE_BITMASK_HPP

#include "kntrie_support.hpp"
#include "kntrie_compact.hpp"

namespace gteitelbaum {

// ==========================================================================
// 256-bit bitmap
// ==========================================================================

enum class slot_mode { FAST_EXIT, BRANCHLESS, UNFILTERED };

struct bitmap256 {
    uint64_t words_[4] = {0, 0, 0, 0};

    bool has_bit(uint8_t i) const noexcept { return words_[i >> 6] & (1ULL << (i & 63)); }
    void set_bit(uint8_t i) noexcept { words_[i >> 6] |= (1ULL << (i & 63)); }
    void clear_bit(uint8_t i) noexcept { words_[i >> 6] &= ~(1ULL << (i & 63)); }

    int popcount() const noexcept {
        return std::popcount(words_[0]) + std::popcount(words_[1]) +
               std::popcount(words_[2]) + std::popcount(words_[3]);
    }

    // FAST_EXIT:   returns slot (>=0) if bit set, -1 if not set
    // BRANCHLESS:  returns slot if bit set, 0 (sentinel) if not set
    // UNFILTERED:  returns count of set bits below index (for insert position)
    template<slot_mode MODE>
    int find_slot(uint8_t index) const noexcept {
        const int w = index >> 6, b = index & 63;
        uint64_t before = words_[w] << (63 - b);
        if constexpr (MODE == slot_mode::FAST_EXIT) {
            if (!(before & (1ULL << 63))) [[unlikely]] return -1;
        }

        int slot = std::popcount(before);
        slot += std::popcount(words_[0]) & -int(w > 0);
        slot += std::popcount(words_[1]) & -int(w > 1);
        slot += std::popcount(words_[2]) & -int(w > 2);

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
        uint64_t masked = words_[w] & ~((1ULL << b) - 1);
        if (masked) return (w << 6) + std::countr_zero(masked);
        for (int ww = w + 1; ww < 4; ++ww)
            if (words_[ww]) return (ww << 6) + std::countr_zero(words_[ww]);
        return -1;
    }

    // =================================================================
    // Factory
    // =================================================================

    static bitmap256 from_indices(const uint8_t* indices, int count) noexcept {
        bitmap256 bm{};
        for (int i = 0; i < count; ++i) bm.set_bit(indices[i]);
        return bm;
    }

    // =================================================================
    // Generic bitmap+array operations
    // =================================================================

    // Fill dest in bitmap order from unsorted (indices, ptrs) — O(n) vs O(n²)
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
// split_ops -- split node with is_internal_bitmap
//
// Layout:
//   [header (1 u64)]              offset 0
//   [main_bitmap (4 u64)]         offset 1
//   [is_internal_bitmap (4 u64)]  offset 5
//   [sentinel (1 u64)]            offset 9    (= ptr to SENTINEL_NODE)
//   [children[]]                  offset 10   (real children, 0-based)
//
// Size: 10 + n_children u64s
//
// Children are leaf (compact or bitmap256) or internal (fan).
// is_internal_bitmap distinguishes: bit set = fan, bit clear = leaf.
// ==========================================================================

template<typename KEY, typename VALUE, typename ALLOC>
struct split_ops {
    using VT  = value_traits<VALUE, ALLOC>;
    using VST = typename VT::slot_type;

    static constexpr size_t MAIN_BM_OFF    = HEADER_U64;          // 1
    static constexpr size_t INT_BM_OFF     = HEADER_U64 + 4;      // 5
    static constexpr size_t SENTINEL_OFF   = HEADER_U64 + 8;      // 9
    static constexpr size_t CHILDREN_OFF   = HEADER_U64 + 9;      // 10

    static constexpr size_t size_u64(size_t n_children) noexcept {
        return 10 + n_children;
    }

    // --- bitmap accessors ---

    template<typename T>
    static auto& main_bm(T* node) noexcept {
        using R = std::conditional_t<std::is_const_v<T>, const bitmap256, bitmap256>;
        return *reinterpret_cast<R*>(node + MAIN_BM_OFF);
    }

    template<typename T>
    static auto& internal_bm(T* node) noexcept {
        using R = std::conditional_t<std::is_const_v<T>, const bitmap256, bitmap256>;
        return *reinterpret_cast<R*>(node + INT_BM_OFF);
    }

    // sentinel + children: children_base[0] = sentinel, [1+] = real children
    template<typename T>
    static T* children_base(T* node) noexcept { return node + SENTINEL_OFF; }

    template<typename T>
    static T* real_children(T* node) noexcept { return node + CHILDREN_OFF; }

    // ==================================================================
    // Lookup (for insert/erase)
    // ==================================================================

    struct child_lookup_t {
        uint64_t* child;
        int       slot;
        bool      found;
    };

    static child_lookup_t lookup_child(const uint64_t* node, uint8_t index) noexcept {
        const bitmap256& bm = main_bm(node);
        int slot = bm.find_slot<slot_mode::FAST_EXIT>(index);
        if (slot < 0) [[unlikely]] return {nullptr, -1, false};
        auto* child = reinterpret_cast<uint64_t*>(real_children(node)[slot]);
        return {child, slot, true};
    }

    // ==================================================================
    // Branchless descent (for find)
    // ==================================================================

    struct top_child_result_t {
        const uint64_t* child;
        bool            is_leaf;
    };

    static top_child_result_t branchless_top_child(const uint64_t* node,
                                                    uint8_t ti) noexcept {
        const bitmap256& bm = main_bm(node);
        int slot = bm.find_slot<slot_mode::BRANCHLESS>(ti);
        auto* child = reinterpret_cast<const uint64_t*>(children_base(node)[slot]);
        bool is_leaf = !internal_bm(node).has_bit(ti);
        return {child, is_leaf};
    }

    static bool is_internal(const uint64_t* node, uint8_t index) noexcept {
        return internal_bm(node).has_bit(index);
    }

    static int child_count(const uint64_t* node) noexcept {
        return get_header(node)->entries();
    }

    // ==================================================================
    // Set child pointer
    // ==================================================================

    static void set_child(uint64_t* node, int slot, uint64_t* child) noexcept {
        real_children(node)[slot] = reinterpret_cast<uint64_t>(child);
    }

    // ==================================================================
    // Add child (leaf or internal)
    // ==================================================================

    static uint64_t* add_child_as_leaf(uint64_t* node, uint8_t index,
                                        uint64_t* child, ALLOC& alloc) {
        return add_child_impl(node, index, child, false, alloc);
    }

    static uint64_t* add_child_as_internal(uint64_t* node, uint8_t index,
                                            uint64_t* child, ALLOC& alloc) {
        return add_child_impl(node, index, child, true, alloc);
    }

    // ==================================================================
    // Remove child
    // ==================================================================

    static uint64_t* remove_child(uint64_t* node, int slot, uint8_t index,
                                   ALLOC& alloc) {
        auto* h = get_header(node);
        size_t otc = h->entries(), ntc = otc - 1;
        if (ntc == 0) {
            dealloc_node(alloc, node, h->alloc_u64());
            return nullptr;
        }

        size_t needed = size_u64(ntc);

        if (!should_shrink_u64(h->alloc_u64(), needed)) {
            bitmap256::arr_remove(main_bm(node), real_children(node),
                                  static_cast<int>(otc), slot, index);
            internal_bm(node).clear_bit(index);
            h->set_entries(static_cast<uint16_t>(ntc));
            return node;
        }

        size_t au64 = round_up_u64(needed);
        uint64_t* nn = alloc_node(alloc, au64);
        auto* nh = get_header(nn); *nh = *h;
        nh->set_entries(static_cast<uint16_t>(ntc));
        nh->set_alloc_u64(static_cast<uint16_t>(au64));

        main_bm(nn) = main_bm(node);
        main_bm(nn).clear_bit(index);
        internal_bm(nn) = internal_bm(node);
        internal_bm(nn).clear_bit(index);
        children_base(nn)[0] = reinterpret_cast<uint64_t>(SENTINEL_NODE);

        bitmap256::arr_copy_remove(real_children(node), real_children(nn),
                                    static_cast<int>(otc), slot);

        dealloc_node(alloc, node, h->alloc_u64());
        return nn;
    }

    // ==================================================================
    // Mark a leaf child as internal (bot-leaf → fan conversion)
    // ==================================================================

    static void mark_internal(uint64_t* node, uint8_t index) noexcept {
        internal_bm(node).set_bit(index);
    }

    // ==================================================================
    // Make from arrays
    // ==================================================================

    static uint64_t* make_split(const uint8_t* indices, uint64_t* const* children,
                                 const bool* is_leaf_flags, int n_children,
                                 uint8_t skip, prefix_t prefix, ALLOC& alloc) {
        bitmap256 tbm = bitmap256::from_indices(indices, n_children);

        size_t needed = size_u64(n_children);
        size_t au64 = round_up_u64(needed);
        uint64_t* nn = alloc_node(alloc, au64);
        auto* nh = get_header(nn);
        nh->set_entries(static_cast<uint16_t>(n_children));
        nh->set_alloc_u64(static_cast<uint16_t>(au64));
        nh->set_skip(skip);
        nh->set_bitmask();
        if (skip > 0) nh->set_prefix(prefix);

        main_bm(nn) = tbm;

        bitmap256 iibm{};
        for (int i = 0; i < n_children; ++i)
            if (!is_leaf_flags[i]) iibm.set_bit(indices[i]);
        internal_bm(nn) = iibm;
        children_base(nn)[0] = reinterpret_cast<uint64_t>(SENTINEL_NODE);

        bitmap256::arr_fill_sorted(tbm, real_children(nn), indices, children, n_children);

        return nn;
    }

    // ==================================================================
    // Iterate: cb(uint8_t index, int slot, uint64_t* child, bool is_leaf)
    // ==================================================================

    template<typename Fn>
    static void for_each_child(const uint64_t* node, Fn&& cb) {
        const bitmap256& bm = main_bm(node);
        const uint64_t* ch = real_children(node);
        int slot = 0;
        for (int i = bm.find_next_set(0); i >= 0; i = bm.find_next_set(i + 1)) {
            auto* child = reinterpret_cast<uint64_t*>(ch[slot]);
            bool is_leaf = !internal_bm(node).has_bit(i);
            cb(static_cast<uint8_t>(i), slot, child, is_leaf);
            ++slot;
        }
    }

    // ==================================================================
    // Dealloc (node only, not children)
    // ==================================================================

    static void dealloc(uint64_t* node, ALLOC& alloc) noexcept {
        dealloc_node(alloc, node, get_header(node)->alloc_u64());
    }

private:
    static uint64_t* add_child_impl(uint64_t* node, uint8_t index,
                                     uint64_t* child, bool is_internal_flag,
                                     ALLOC& alloc) {
        auto* h = get_header(node);
        bitmap256& bm = main_bm(node);
        size_t otc = h->entries(), ntc = otc + 1;
        size_t needed = size_u64(ntc);

        if (needed <= h->alloc_u64()) {
            bitmap256::arr_insert(bm, real_children(node),
                                  static_cast<int>(otc), index,
                                  reinterpret_cast<uint64_t>(child));
            if (is_internal_flag) internal_bm(node).set_bit(index);
            h->set_entries(static_cast<uint16_t>(ntc));
            return node;
        }

        int isl = bm.find_slot<slot_mode::UNFILTERED>(index);
        size_t au64 = round_up_u64(needed);
        uint64_t* nn = alloc_node(alloc, au64);
        auto* nh = get_header(nn); *nh = *h;
        nh->set_entries(static_cast<uint16_t>(ntc));
        nh->set_alloc_u64(static_cast<uint16_t>(au64));

        main_bm(nn) = bm;
        main_bm(nn).set_bit(index);
        internal_bm(nn) = internal_bm(node);
        if (is_internal_flag) internal_bm(nn).set_bit(index);
        children_base(nn)[0] = reinterpret_cast<uint64_t>(SENTINEL_NODE);

        bitmap256::arr_copy_insert(real_children(node), real_children(nn),
                                    static_cast<int>(otc), isl,
                                    reinterpret_cast<uint64_t>(child));

        dealloc_node(alloc, node, h->alloc_u64());
        return nn;
    }
};

// ==========================================================================
// fan_ops -- fan node without is_internal_bitmap
//
// Layout:
//   [header (1 u64)]              offset 0
//   [main_bitmap (4 u64)]         offset 1
//   [sentinel (1 u64)]            offset 5    (= ptr to SENTINEL_NODE)
//   [children[]]                  offset 6    (real children, 0-based)
//
// Size: 6 + n_children u64s
//
// All children go back to loop top (compact leaf or split node).
// No skip/prefix.
// ==========================================================================

template<typename KEY, typename VALUE, typename ALLOC>
struct fan_ops {
    using VT  = value_traits<VALUE, ALLOC>;
    using VST = typename VT::slot_type;

    static constexpr size_t MAIN_BM_OFF    = HEADER_U64;          // 1
    static constexpr size_t SENTINEL_OFF   = HEADER_U64 + 4;      // 5
    static constexpr size_t CHILDREN_OFF   = HEADER_U64 + 5;      // 6

    static constexpr size_t size_u64(size_t n_children) noexcept {
        return 6 + n_children;
    }

    // --- bitmap accessors ---

    template<typename T>
    static auto& main_bm(T* node) noexcept {
        using R = std::conditional_t<std::is_const_v<T>, const bitmap256, bitmap256>;
        return *reinterpret_cast<R*>(node + MAIN_BM_OFF);
    }

    template<typename T>
    static T* children_base(T* node) noexcept { return node + SENTINEL_OFF; }

    template<typename T>
    static T* real_children(T* node) noexcept { return node + CHILDREN_OFF; }

    // ==================================================================
    // Lookup (for insert/erase)
    // ==================================================================

    struct child_lookup_t {
        uint64_t* child;
        int       slot;
        bool      found;
    };

    static child_lookup_t lookup_child(const uint64_t* node, uint8_t index) noexcept {
        const bitmap256& bm = main_bm(node);
        int slot = bm.find_slot<slot_mode::FAST_EXIT>(index);
        if (slot < 0) return {nullptr, -1, false};
        auto* child = reinterpret_cast<uint64_t*>(real_children(node)[slot]);
        return {child, slot, true};
    }

    // ==================================================================
    // Branchless descent (for find)
    // ==================================================================

    static const uint64_t* branchless_child(const uint64_t* node,
                                             uint8_t bi) noexcept {
        const bitmap256& bm = main_bm(node);
        int slot = bm.find_slot<slot_mode::BRANCHLESS>(bi);
        return reinterpret_cast<const uint64_t*>(children_base(node)[slot]);
    }

    static int child_count(const uint64_t* node) noexcept {
        return get_header(node)->entries();
    }

    // ==================================================================
    // Set child
    // ==================================================================

    static void set_child(uint64_t* node, int slot, uint64_t* child) noexcept {
        real_children(node)[slot] = reinterpret_cast<uint64_t>(child);
    }

    // ==================================================================
    // Add child
    // ==================================================================

    static uint64_t* add_child(uint64_t* node, uint8_t index,
                                uint64_t* child, ALLOC& alloc) {
        auto* h = get_header(node);
        bitmap256& bm = main_bm(node);
        int oc = h->entries();
        int nc = oc + 1;
        size_t needed = size_u64(nc);

        if (needed <= h->alloc_u64()) {
            bitmap256::arr_insert(bm, real_children(node), oc, index,
                                  reinterpret_cast<uint64_t>(child));
            h->set_entries(static_cast<uint16_t>(nc));
            return node;
        }

        int isl = bm.find_slot<slot_mode::UNFILTERED>(index);
        size_t au64 = round_up_u64(needed);
        uint64_t* nn = alloc_node(alloc, au64);
        auto* nh = get_header(nn);
        nh->set_entries(static_cast<uint16_t>(nc));
        nh->set_alloc_u64(static_cast<uint16_t>(au64));
        nh->set_bitmask();

        main_bm(nn) = bm;
        main_bm(nn).set_bit(index);
        children_base(nn)[0] = reinterpret_cast<uint64_t>(SENTINEL_NODE);

        bitmap256::arr_copy_insert(real_children(node), real_children(nn),
                                    oc, isl,
                                    reinterpret_cast<uint64_t>(child));

        dealloc_node(alloc, node, h->alloc_u64());
        return nn;
    }

    // ==================================================================
    // Remove child
    // ==================================================================

    static uint64_t* remove_child(uint64_t* node, int slot, uint8_t index,
                                   ALLOC& alloc) {
        auto* h = get_header(node);
        int oc = h->entries();
        int nc = oc - 1;
        size_t needed = size_u64(nc);

        if (!should_shrink_u64(h->alloc_u64(), needed)) {
            bitmap256::arr_remove(main_bm(node), real_children(node),
                                  oc, slot, index);
            h->set_entries(static_cast<uint16_t>(nc));
            return node;
        }

        size_t au64 = round_up_u64(needed);
        uint64_t* nn = alloc_node(alloc, au64);
        auto* nh = get_header(nn);
        nh->set_entries(static_cast<uint16_t>(nc));
        nh->set_alloc_u64(static_cast<uint16_t>(au64));
        nh->set_bitmask();

        main_bm(nn) = main_bm(node);
        main_bm(nn).clear_bit(index);
        children_base(nn)[0] = reinterpret_cast<uint64_t>(SENTINEL_NODE);

        bitmap256::arr_copy_remove(real_children(node), real_children(nn),
                                    oc, slot);

        dealloc_node(alloc, node, h->alloc_u64());
        return nn;
    }

    // ==================================================================
    // Make from arrays
    // ==================================================================

    static uint64_t* make_fan(const uint8_t* indices, uint64_t* const* children,
                               int n_children, ALLOC& alloc) {
        bitmap256 bm = bitmap256::from_indices(indices, n_children);

        size_t needed = size_u64(n_children);
        size_t au64 = round_up_u64(needed);
        uint64_t* nn = alloc_node(alloc, au64);
        auto* h = get_header(nn);
        h->set_entries(static_cast<uint16_t>(n_children));
        h->set_alloc_u64(static_cast<uint16_t>(au64));
        h->set_bitmask();

        main_bm(nn) = bm;
        children_base(nn)[0] = reinterpret_cast<uint64_t>(SENTINEL_NODE);

        bitmap256::arr_fill_sorted(bm, real_children(nn), indices, children, n_children);
        return nn;
    }

    // ==================================================================
    // Iterate: cb(uint8_t index, int slot, uint64_t* child)
    // ==================================================================

    template<typename Fn>
    static void for_each_child(const uint64_t* node, Fn&& cb) {
        const bitmap256& bm = main_bm(node);
        const uint64_t* ch = real_children(node);
        int slot = 0;
        for (int i = bm.find_next_set(0); i >= 0; i = bm.find_next_set(i + 1))
            cb(static_cast<uint8_t>(i), slot, reinterpret_cast<uint64_t*>(ch[slot++]));
    }

    // ==================================================================
    // Dealloc
    // ==================================================================

    static void dealloc(uint64_t* node, ALLOC& alloc) noexcept {
        dealloc_node(alloc, node, get_header(node)->alloc_u64());
    }
};

// ==========================================================================
// bitmap_leaf_ops -- bitmap256 leaf node
//
// Layout:
//   [header (1 u64)]              offset 0
//   [bitmap256 (4 u64)]           offset 1
//   [VST[]]                       offset 5
//
// Header has is_bitmask=1 (so is_leaf()=false). Distinguished from fan/split
// by context: parent's is_internal_bitmap says "leaf child" + child's
// is_leaf()=false → bitmap256.
//
// Direct O(1) lookup via popcount. 8-bit suffix space.
// ==========================================================================

template<typename KEY, typename VALUE, typename ALLOC>
struct bitmap_leaf_ops {
    using VT  = value_traits<VALUE, ALLOC>;
    using VST = typename VT::slot_type;

    static constexpr size_t BM_OFF   = HEADER_U64;      // 1
    static constexpr size_t VALS_OFF = HEADER_U64 + 4;   // 5

    static constexpr size_t size_u64(size_t count) noexcept {
        size_t vb = count * sizeof(VST);
        vb = (vb + 7) & ~size_t{7};
        return HEADER_U64 + BITMAP256_U64 + vb / 8;
    }

    // --- accessors ---

    template<typename T>
    static auto& bm(T* node) noexcept {
        using R = std::conditional_t<std::is_const_v<T>, const bitmap256, bitmap256>;
        return *reinterpret_cast<R*>(node + BM_OFF);
    }

    template<typename T>
    static auto vals(T* node) noexcept {
        using R = std::conditional_t<std::is_const_v<T>, const VST, VST>;
        return reinterpret_cast<R*>(node + VALS_OFF);
    }

    // ==================================================================
    // Find
    // ==================================================================

    static const VALUE* find(const uint64_t* node, uint8_t suffix) noexcept {
        int slot = bm(node).template find_slot<slot_mode::FAST_EXIT>(suffix);
        if (slot < 0) [[unlikely]] return nullptr;
        return VT::as_ptr(vals(node)[slot]);
    }

    // ==================================================================
    // Count
    // ==================================================================

    static uint32_t count(const uint64_t* node) noexcept {
        return get_header(node)->entries();
    }

    // ==================================================================
    // Insert
    // ==================================================================

    struct insert_result_t {
        uint64_t* node;
        bool      inserted;
    };

    template<bool INSERT = true, bool ASSIGN = true>
    requires (INSERT || ASSIGN)
    static insert_result_t insert(uint64_t* node, uint8_t suffix,
                                   VST value, ALLOC& alloc) {
        auto* h = get_header(node);
        bitmap256& bmap = bm(node);
        uint32_t cnt = h->entries();
        VST* vd = vals(node);

        if (bmap.has_bit(suffix)) {
            if constexpr (ASSIGN) {
                int slot = bmap.find_slot<slot_mode::UNFILTERED>(suffix);
                VT::destroy(vd[slot], alloc);
                VT::write_slot(&vd[slot], value);
            }
            return {node, false};
        }

        if constexpr (!INSERT) return {node, false};

        uint32_t nc = cnt + 1;
        size_t new_sz = size_u64(nc);

        if (new_sz <= h->alloc_u64()) {
            int isl = bmap.find_slot<slot_mode::UNFILTERED>(suffix);
            bmap.set_bit(suffix);
            std::memmove(vd + isl + 1, vd + isl, (cnt - isl) * sizeof(VST));
            VT::write_slot(&vd[isl], value);
            h->set_entries(static_cast<uint16_t>(nc));
            return {node, true};
        }

        size_t au64 = round_up_u64(new_sz);
        uint64_t* nn = alloc_node(alloc, au64);
        auto* nh = get_header(nn);
        nh->set_entries(static_cast<uint16_t>(nc));
        nh->set_alloc_u64(static_cast<uint16_t>(au64));
        nh->set_bitmask();
        bitmap256& nbm = bm(nn);
        nbm = bmap; nbm.set_bit(suffix);
        VST* nvd = vals(nn);
        int isl = nbm.find_slot<slot_mode::UNFILTERED>(suffix);
        std::memcpy(nvd, vd, isl * sizeof(VST));
        VT::write_slot(&nvd[isl], value);
        std::memcpy(nvd + isl + 1, vd + isl, (cnt - isl) * sizeof(VST));

        dealloc_node(alloc, node, h->alloc_u64());
        return {nn, true};
    }

    // ==================================================================
    // Erase
    // ==================================================================

    static erase_result_t erase(uint64_t* node, uint8_t suffix, ALLOC& alloc) {
        auto* h = get_header(node);
        bitmap256& bmap = bm(node);
        if (!bmap.has_bit(suffix)) return {node, false};
        uint32_t cnt = h->entries();
        int slot = bmap.find_slot<slot_mode::UNFILTERED>(suffix);
        VT::destroy(vals(node)[slot], alloc);

        uint32_t nc = cnt - 1;
        if (nc == 0) {
            dealloc_node(alloc, node, h->alloc_u64());
            return {nullptr, true};
        }

        size_t new_sz = size_u64(nc);
        if (!should_shrink_u64(h->alloc_u64(), new_sz)) {
            VST* vd = vals(node);
            bmap.clear_bit(suffix);
            std::memmove(vd + slot, vd + slot + 1, (nc - slot) * sizeof(VST));
            h->set_entries(static_cast<uint16_t>(nc));
            return {node, true};
        }

        size_t au64 = round_up_u64(new_sz);
        uint64_t* nn = alloc_node(alloc, au64);
        auto* nh = get_header(nn);
        nh->set_entries(static_cast<uint16_t>(nc));
        nh->set_alloc_u64(static_cast<uint16_t>(au64));
        nh->set_bitmask();
        bitmap256& nbm = bm(nn);
        nbm = bmap; nbm.clear_bit(suffix);
        const VST* ov = vals(node);
        VST*       nv = vals(nn);
        std::memcpy(nv, ov, slot * sizeof(VST));
        std::memcpy(nv + slot, ov + slot + 1, (nc - slot) * sizeof(VST));

        dealloc_node(alloc, node, h->alloc_u64());
        return {nn, true};
    }

    // ==================================================================
    // Make single entry
    // ==================================================================

    static uint64_t* make_single(uint8_t suffix, VST value, ALLOC& alloc) {
        size_t sz = round_up_u64(size_u64(1));
        uint64_t* node = alloc_node(alloc, sz);
        auto* h = get_header(node);
        h->set_entries(1);
        h->set_alloc_u64(static_cast<uint16_t>(sz));
        h->set_bitmask();
        bm(node).set_bit(suffix);
        VT::write_slot(&vals(node)[0], value);
        return node;
    }

    // ==================================================================
    // Make from sorted arrays
    // ==================================================================

    static uint64_t* make_from_sorted(const uint8_t* suffixes, const VST* values,
                                       uint32_t cnt, ALLOC& alloc) {
        size_t sz = round_up_u64(size_u64(cnt));
        uint64_t* node = alloc_node(alloc, sz);
        auto* h = get_header(node);
        h->set_entries(static_cast<uint16_t>(cnt));
        h->set_alloc_u64(static_cast<uint16_t>(sz));
        h->set_bitmask();
        bitmap256& bmap = bm(node);
        bmap = bitmap256{};
        for (uint32_t i = 0; i < cnt; ++i) bmap.set_bit(suffixes[i]);
        auto* vd = vals(node);
        for (uint32_t i = 0; i < cnt; ++i)
            vd[bmap.find_slot<slot_mode::UNFILTERED>(suffixes[i])] = values[i];
        return node;
    }

    // ==================================================================
    // Iterate: cb(uint8_t suffix, VST value)
    // ==================================================================

    template<typename Fn>
    static void for_each(const uint64_t* node, Fn&& cb) {
        const bitmap256& bmap = bm(node);
        const VST* vd = vals(node);
        int slot = 0;
        for (int i = bmap.find_next_set(0); i >= 0; i = bmap.find_next_set(i + 1))
            cb(static_cast<uint8_t>(i), vd[slot++]);
    }

    // ==================================================================
    // Destroy + dealloc
    // ==================================================================

    static void destroy_and_dealloc(uint64_t* node, ALLOC& alloc) {
        auto* h = get_header(node);
        if constexpr (!VT::IS_INLINE) {
            uint32_t cnt = h->entries();
            auto* vd = vals(node);
            for (uint32_t i = 0; i < cnt; ++i) VT::destroy(vd[i], alloc);
        }
        dealloc_node(alloc, node, h->alloc_u64());
    }

    // ==================================================================
    // Dealloc (no value destruction — for when values moved elsewhere)
    // ==================================================================

    static void dealloc_only(uint64_t* node, ALLOC& alloc) noexcept {
        dealloc_node(alloc, node, get_header(node)->alloc_u64());
    }
};

} // namespace gteitelbaum

#endif // KNTRIE_BITMASK_HPP
