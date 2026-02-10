#ifndef KNTRIE_BITMASK_HPP
#define KNTRIE_BITMASK_HPP

#include "kntrie_support.hpp"
#include "kntrie_compact.hpp"

namespace kn3 {

// ==========================================================================
// 256-bit bitmap
// ==========================================================================

enum class SlotMode { FAST_EXIT, BRANCHLESS, UNFILTERED };

struct Bitmap256 {
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
    template<SlotMode MODE>
    int find_slot(uint8_t index) const noexcept {
        const int w = index >> 6, b = index & 63;
        uint64_t before = words[w] << (63 - b);
        if constexpr (MODE == SlotMode::FAST_EXIT) {
            if (!(before & (1ULL << 63))) [[unlikely]] return -1;
        }

        int slot = std::popcount(before);
        slot += std::popcount(words[0]) & -int(w > 0);
        slot += std::popcount(words[1]) & -int(w > 1);
        slot += std::popcount(words[2]) & -int(w > 2);

        if constexpr (MODE == SlotMode::BRANCHLESS)
            slot &= -int(bool(before & (1ULL << 63)));
        else if constexpr (MODE == SlotMode::FAST_EXIT)
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

    // =================================================================
    // Factory
    // =================================================================

    static Bitmap256 from_indices(const uint8_t* indices, int count) noexcept {
        Bitmap256 bm{};
        for (int i = 0; i < count; ++i) bm.set_bit(indices[i]);
        return bm;
    }

    // =================================================================
    // Generic bitmap+array operations
    // =================================================================

    // Fill dest in bitmap order from unsorted (indices, ptrs) — O(n) vs O(n²)
    static void arr_fill_sorted(const Bitmap256& bm, uint64_t* dest,
                                const uint8_t* indices, uint64_t* const* ptrs,
                                int count) noexcept {
        for (int i = 0; i < count; ++i)
            dest[bm.find_slot<SlotMode::UNFILTERED>(indices[i])] =
                reinterpret_cast<uint64_t>(ptrs[i]);
    }

    // In-place insert: memmove right, write new entry, set bit
    static void arr_insert(Bitmap256& bm, uint64_t* arr, int count,
                           uint8_t idx, uint64_t val) noexcept {
        int isl = bm.find_slot<SlotMode::UNFILTERED>(idx);
        std::memmove(arr + isl + 1, arr + isl, (count - isl) * sizeof(uint64_t));
        arr[isl] = val;
        bm.set_bit(idx);
    }

    // In-place remove: memmove left, clear bit
    static void arr_remove(Bitmap256& bm, uint64_t* arr, int count,
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
// BitmaskOps  -- split-top, bot-leaf, bot-internal operations
//
// All node types use 2-u64 header: [NodeHeader][prefix]
// ==========================================================================

template<typename KEY, typename VALUE, typename ALLOC>
struct BitmaskOps {
    using KOps = KeyOps<KEY>;
    using VT   = ValueTraits<VALUE, ALLOC>;
    using VST  = typename VT::slot_type;
    using CO   = CompactOps<KEY, VALUE, ALLOC>;

    // ==================================================================
    // Size calculations
    // ==================================================================

    template<int BITS>
    static constexpr size_t split_top_size_u64(size_t n_entries) noexcept {
        if constexpr (BITS == 16)
            return HEADER_U64 + BITMAP256_U64 + n_entries;
        else
            return HEADER_U64 + BITMAP256_U64 + BITMAP256_U64 + 1 + n_entries;
    }

    template<int BITS>
    static constexpr size_t bot_leaf_size_u64(size_t count) noexcept {
        if constexpr (BITS == 16) {
            size_t vb = count * sizeof(VST); vb = (vb + 7) & ~size_t{7};
            return HEADER_U64 + BITMAP256_U64 + vb / 8;
        } else {
            return CO::template size_u64<BITS - 8>(count);
        }
    }

    static constexpr size_t bot_internal_size_u64(size_t count) noexcept {
        return HEADER_U64 + BITMAP256_U64 + 1 + count;
    }

    // ==================================================================
    // Split-top: lookup (for insert/erase)
    // ==================================================================

    struct TopLookup {
        uint64_t* bot;
        int       slot;
        bool      found;
        bool      is_leaf;
    };

    template<int BITS>
    static TopLookup lookup_top(const uint64_t* node, uint8_t ti) noexcept {
        const Bitmap256& tbm = bm_(node);
        int slot = tbm.find_slot<SlotMode::FAST_EXIT>(ti);
        if (slot < 0) [[unlikely]] return {nullptr, -1, false, false};

        auto* bot = reinterpret_cast<uint64_t*>(top_real_children_<BITS>(node)[slot]);
        bool is_leaf;
        if constexpr (BITS == 16) is_leaf = true;
        else is_leaf = !bot_is_internal_bm_(node).has_bit(ti);
        return {bot, slot, true, is_leaf};
    }

    // ==================================================================
    // Split-top: branchless descent (for find)
    // ==================================================================

    template<int BITS>
    static const uint64_t* branchless_top_child(const uint64_t* node, uint8_t ti) noexcept {
        static_assert(BITS > 16);
        const Bitmap256& tbm = bm_(node);
        int slot = tbm.find_slot<SlotMode::BRANCHLESS>(ti);
        return reinterpret_cast<const uint64_t*>(top_children_<BITS>(node)[slot]);
    }

    template<int BITS>
    static bool is_top_entry_leaf(const uint64_t* node, uint8_t ti) noexcept {
        if constexpr (BITS == 16) return true;
        else return !bot_is_internal_bm_(node).has_bit(ti);
    }

    // ==================================================================
    // Split-top: set child pointer (0-based slot into real children)
    // ==================================================================

    template<int BITS>
    static void set_top_child(uint64_t* node, int slot, uint64_t* ptr) noexcept {
        top_real_children_<BITS>(node)[slot] = reinterpret_cast<uint64_t>(ptr);
    }

    // ==================================================================
    // Split-top: add a new top slot
    // ==================================================================

    template<int BITS>
    static uint64_t* add_top_slot(uint64_t* node, NodeHeader* h,
                                   uint8_t ti, uint64_t* bot_ptr,
                                   bool is_leaf, ALLOC& alloc) {
        Bitmap256& tbm = bm_(node);
        size_t otc = h->entries, ntc = otc + 1;
        size_t needed = split_top_size_u64<BITS>(ntc);

        // --- In-place ---
        if (needed <= h->alloc_u64) {
            Bitmap256::arr_insert(tbm, top_real_children_<BITS>(node),
                                  static_cast<int>(otc), ti,
                                  reinterpret_cast<uint64_t>(bot_ptr));
            if constexpr (BITS > 16) {
                if (!is_leaf) bot_is_internal_bm_(node).set_bit(ti);
            }
            h->entries = static_cast<uint16_t>(ntc);
            h->add_descendants(1);
            return node;
        }

        // --- Realloc ---
        int isl = tbm.find_slot<SlotMode::UNFILTERED>(ti);
        size_t au64 = round_up_u64(needed);
        uint64_t* nn = alloc_node(alloc, au64);
        auto* nh = get_header(nn); *nh = *h;
        nh->entries = static_cast<uint16_t>(ntc);
        nh->add_descendants(1);
        nh->alloc_u64 = static_cast<uint16_t>(au64);
        if (h->skip() > 0) set_prefix(nn, get_prefix(node));

        bm_(nn) = tbm;
        bm_(nn).set_bit(ti);

        if constexpr (BITS > 16) {
            bot_is_internal_bm_(nn) = bot_is_internal_bm_(node);
            if (!is_leaf) bot_is_internal_bm_(nn).set_bit(ti);
            top_children_<BITS>(nn)[0] = reinterpret_cast<uint64_t>(SENTINEL_NODE);
        }

        Bitmap256::arr_copy_insert(top_real_children_<BITS>(node),
                                    top_real_children_<BITS>(nn),
                                    static_cast<int>(otc), isl,
                                    reinterpret_cast<uint64_t>(bot_ptr));

        dealloc_node(alloc, node, h->alloc_u64);
        return nn;
    }

    // ==================================================================
    // Split-top: remove a top slot
    // ==================================================================

    template<int BITS>
    static uint64_t* remove_top_slot(uint64_t* node, NodeHeader* h,
                                      int slot, uint8_t ti, ALLOC& alloc) {
        size_t otc = h->entries, ntc = otc - 1;
        if (ntc == 0) {
            dealloc_node(alloc, node, h->alloc_u64);
            return nullptr;
        }

        size_t needed = split_top_size_u64<BITS>(ntc);

        // --- In-place ---
        if (!should_shrink_u64(h->alloc_u64, needed)) {
            Bitmap256::arr_remove(bm_(node), top_real_children_<BITS>(node),
                                  static_cast<int>(otc), slot, ti);
            if constexpr (BITS > 16) {
                bot_is_internal_bm_(node).clear_bit(ti);
            }
            h->entries = static_cast<uint16_t>(ntc);
            h->sub_descendants(1);
            return node;
        }

        // --- Realloc ---
        size_t au64 = round_up_u64(needed);
        uint64_t* nn = alloc_node(alloc, au64);
        auto* nh = get_header(nn); *nh = *h;
        nh->entries = static_cast<uint16_t>(ntc);
        nh->sub_descendants(1);
        nh->alloc_u64 = static_cast<uint16_t>(au64);
        if (h->skip() > 0) set_prefix(nn, get_prefix(node));

        bm_(nn) = bm_(node);
        bm_(nn).clear_bit(ti);
        if constexpr (BITS > 16) {
            bot_is_internal_bm_(nn) = bot_is_internal_bm_(node);
            bot_is_internal_bm_(nn).clear_bit(ti);
            top_children_<BITS>(nn)[0] = reinterpret_cast<uint64_t>(SENTINEL_NODE);
        }

        Bitmap256::arr_copy_remove(top_real_children_<BITS>(node),
                                    top_real_children_<BITS>(nn),
                                    static_cast<int>(otc), slot);

        dealloc_node(alloc, node, h->alloc_u64);
        return nn;
    }

    // ==================================================================
    // Split-top: mark a bot entry as internal
    // ==================================================================

    template<int BITS>
    static void mark_bot_internal(uint64_t* node, uint8_t ti) noexcept {
        if constexpr (BITS > 16)
            bot_is_internal_bm_(node).set_bit(ti);
    }

    // ==================================================================
    // Split-top: iterate  cb(uint8_t ti, int slot, uint64_t* bot, bool is_leaf)
    // ==================================================================

    template<int BITS, typename Fn>
    static void for_each_top(const uint64_t* node, Fn&& cb) {
        const Bitmap256& tbm = bm_(node);
        const uint64_t* ch = top_real_children_<BITS>(node);
        int slot = 0;
        for (int ti = tbm.find_next_set(0); ti >= 0; ti = tbm.find_next_set(ti + 1)) {
            auto* bot = reinterpret_cast<uint64_t*>(ch[slot]);
            bool is_leaf;
            if constexpr (BITS == 16) is_leaf = true;
            else is_leaf = !bot_is_internal_bm_(node).has_bit(ti);
            cb(static_cast<uint8_t>(ti), slot, bot, is_leaf);
            ++slot;
        }
    }

    // ==================================================================
    // Split-top: make from arrays of (ti, bot_ptr, is_leaf)
    // ==================================================================

    template<int BITS>
    static uint64_t* make_split_top(const uint8_t* top_indices,
                                     uint64_t* const* bot_ptrs,
                                     const bool* is_leaf_flags,
                                     int n_tops, uint8_t skip, uint64_t prefix,
                                     uint32_t total_descendants, ALLOC& alloc) {
        Bitmap256 tbm = Bitmap256::from_indices(top_indices, n_tops);

        size_t needed = split_top_size_u64<BITS>(n_tops);
        size_t au64 = round_up_u64(needed);
        uint64_t* nn = alloc_node(alloc, au64);
        auto* nh = get_header(nn);
        nh->entries = static_cast<uint16_t>(n_tops);
        nh->descendants = total_descendants > NodeHeader::DESC_CAP
            ? NodeHeader::DESC_CAP : static_cast<uint16_t>(total_descendants);
        nh->alloc_u64 = static_cast<uint16_t>(au64);
        nh->set_skip(skip);
        nh->set_bitmask();
        if (skip > 0) set_prefix(nn, prefix);

        bm_(nn) = tbm;

        if constexpr (BITS > 16) {
            Bitmap256 iibm{};
            for (int i = 0; i < n_tops; ++i)
                if (!is_leaf_flags[i]) iibm.set_bit(top_indices[i]);
            bot_is_internal_bm_(nn) = iibm;
            top_children_<BITS>(nn)[0] = reinterpret_cast<uint64_t>(SENTINEL_NODE);
        }

        uint64_t* rch = top_real_children_<BITS>(nn);
        Bitmap256::arr_fill_sorted(tbm, rch, top_indices, bot_ptrs, n_tops);

        return nn;
    }

    // ==================================================================
    // Split-top: deallocate (top node only, not children)
    // ==================================================================

    template<int BITS>
    static void dealloc_split_top(uint64_t* node, ALLOC& alloc) noexcept {
        auto* h = get_header(node);
        dealloc_node(alloc, node, h->alloc_u64);
    }

    // ==================================================================
    // Bot-leaf: count
    // ==================================================================

    template<int BITS>
    static uint32_t bot_leaf_count(const uint64_t* bot) noexcept {
        return get_header(bot)->entries;
    }

    // ==================================================================
    // Bot-leaf: find
    // ==================================================================

    template<int BITS>
    static const VALUE* find_in_bot_leaf(const uint64_t* bot, uint64_t ik) noexcept {
        if constexpr (BITS == 16) {
            uint8_t suffix = static_cast<uint8_t>(KOps::template extract_suffix<8>(ik));
            const Bitmap256& bm = bm_(bot);
            int slot = bm.find_slot<SlotMode::FAST_EXIT>(suffix);
            if (slot < 0) [[unlikely]] return nullptr;
            return VT::as_ptr(bot_leaf_vals_16_(bot)[slot]);
        } else {
            NodeHeader h = *get_header(bot);
            return CO::template find<BITS - 8>(bot, h, ik);
        }
    }

    // ==================================================================
    // Bot-leaf: insert
    // ==================================================================

    struct BotLeafInsertResult {
        uint64_t* new_bot;
        bool inserted;
        bool overflow;
    };

    template<int BITS, bool INSERT = true, bool ASSIGN = true>
    requires (INSERT || ASSIGN)
    static BotLeafInsertResult insert_into_bot_leaf(
            uint64_t* bot, uint64_t ik, VST value, ALLOC& alloc) {
        if constexpr (BITS == 16)
            return insert_bl_16_<INSERT, ASSIGN>(bot, ik, value, alloc);
        else {
            auto r = CO::template insert<BITS - 8, INSERT, ASSIGN>(
                bot, get_header(bot), ik, value, alloc);
            return {r.node, r.inserted, r.needs_split};
        }
    }

    // ==================================================================
    // Bot-leaf: erase
    // ==================================================================

    template<int BITS>
    static EraseResult erase_from_bot_leaf(uint64_t* bot, uint64_t ik, ALLOC& alloc) {
        if constexpr (BITS == 16)
            return erase_bl_16_(bot, ik, alloc);
        else
            return CO::template erase<BITS - 8>(bot, get_header(bot), ik, alloc);
    }

    // ==================================================================
    // Bot-leaf: make single entry
    // ==================================================================

    template<int BITS>
    static uint64_t* make_single_bot_leaf(uint64_t ik, VST value, ALLOC& alloc) {
        if constexpr (BITS == 16) {
            size_t sz = bot_leaf_size_u64<BITS>(1);
            uint64_t* bot = alloc_node(alloc, sz);
            auto* bh = get_header(bot);
            bh->entries = 1;
            bh->alloc_u64 = static_cast<uint16_t>(sz);
            uint8_t suffix = static_cast<uint8_t>(KOps::template extract_suffix<8>(ik));
            bm_(bot).set_bit(suffix);
            VT::write_slot(&bot_leaf_vals_16_(bot)[0], value);
            return bot;
        } else {
            constexpr int sb = BITS - 8;
            using S = typename suffix_traits<sb>::type;
            S suffix = static_cast<S>(KOps::template extract_suffix<sb>(ik));
            return CO::template make_leaf<sb>(&suffix, &value, 1, 0, 0, alloc);
        }
    }

    // ==================================================================
    // Bot-leaf: make from sorted working arrays
    // ==================================================================

    template<int BITS>
    static uint64_t* make_bot_leaf(
            const typename suffix_traits<(BITS > 16 ? BITS - 8 : 8)>::type* sorted_suffixes,
            const VST* values, uint32_t count, ALLOC& alloc) {
        if constexpr (BITS == 16) {
            size_t sz = bot_leaf_size_u64<BITS>(count);
            uint64_t* bot = alloc_node(alloc, sz);
            auto* bh = get_header(bot);
            bh->entries = static_cast<uint16_t>(count);
            bh->alloc_u64 = static_cast<uint16_t>(sz);
            Bitmap256& bm = bm_(bot);
            bm = Bitmap256{};
            for (uint32_t i = 0; i < count; ++i) bm.set_bit(sorted_suffixes[i]);
            auto* vd = bot_leaf_vals_16_(bot);
            for (uint32_t i = 0; i < count; ++i)
                vd[bm.find_slot<SlotMode::UNFILTERED>(sorted_suffixes[i])] = values[i];
            return bot;
        } else {
            return CO::template make_leaf<BITS - 8>(
                sorted_suffixes, values, count, 0, 0, alloc);
        }
    }

    // ==================================================================
    // Bot-leaf: iterate  cb(suffix, value_slot)
    // ==================================================================

    template<int BITS, typename Fn>
    static void for_each_bot_leaf(const uint64_t* bot, Fn&& cb) {
        if constexpr (BITS == 16) {
            const Bitmap256& bm = bm_(bot);
            const VST* vd = bot_leaf_vals_16_(bot);
            int slot = 0;
            for (int i = bm.find_next_set(0); i >= 0; i = bm.find_next_set(i + 1))
                cb(static_cast<uint8_t>(i), vd[slot++]);
        } else {
            CO::template for_each<BITS - 8>(bot, get_header(bot), std::forward<Fn>(cb));
        }
    }

    // ==================================================================
    // Bot-leaf: destroy values + deallocate
    // ==================================================================

    template<int BITS>
    static void destroy_bot_leaf_and_dealloc(uint64_t* bot, ALLOC& alloc) {
        if constexpr (BITS == 16) {
            auto* bh = get_header(bot);
            uint32_t count = bh->entries;
            if constexpr (!VT::is_inline) {
                auto* vd = bot_leaf_vals_16_(bot);
                for (uint32_t i = 0; i < count; ++i) VT::destroy(vd[i], alloc);
            }
            dealloc_node(alloc, bot, bh->alloc_u64);
        } else {
            CO::template destroy_and_dealloc<BITS - 8>(bot, alloc);
        }
    }

    template<int BITS>
    static void dealloc_bot_leaf(uint64_t* bot, uint32_t /*count*/, ALLOC& alloc) noexcept {
        dealloc_node(alloc, bot, get_header(bot)->alloc_u64);
    }

    // ==================================================================
    // Bot-internal: make from arrays
    // ==================================================================

    static uint64_t* make_bot_internal(const uint8_t* indices,
                                        uint64_t* const* child_ptrs,
                                        int n_children, ALLOC& alloc) {
        Bitmap256 bm = Bitmap256::from_indices(indices, n_children);

        size_t needed = bot_internal_size_u64(n_children);
        size_t au64 = round_up_u64(needed);
        uint64_t* bot = alloc_node(alloc, au64);
        auto* h = get_header(bot);
        h->entries = static_cast<uint16_t>(n_children);
        h->alloc_u64 = static_cast<uint16_t>(au64);
        h->set_bitmask();

        bm_(bot) = bm;
        bot_int_children_(bot)[0] = reinterpret_cast<uint64_t>(SENTINEL_NODE);

        uint64_t* rch = bot_int_real_children_(bot);
        Bitmap256::arr_fill_sorted(bm, rch, indices, child_ptrs, n_children);
        return bot;
    }

    // ==================================================================
    // Bot-internal: lookup child
    // ==================================================================

    struct BotChildLookup {
        uint64_t* child;
        int       slot;
        bool      found;
    };

    static BotChildLookup lookup_bot_child(const uint64_t* bot, uint8_t bi) noexcept {
        const Bitmap256& bm = bm_(bot);
        int slot = bm.find_slot<SlotMode::FAST_EXIT>(bi);
        if (slot < 0) return {nullptr, -1, false};
        auto* child = reinterpret_cast<uint64_t*>(bot_int_real_children_(bot)[slot]);
        return {child, slot, true};
    }

    // ==================================================================
    // Bot-internal: branchless descent (for find)
    // ==================================================================

    static const uint64_t* branchless_bot_child(const uint64_t* bot, uint8_t bi) noexcept {
        const Bitmap256& bm = bm_(bot);
        int slot = bm.find_slot<SlotMode::BRANCHLESS>(bi);
        return reinterpret_cast<const uint64_t*>(bot_int_children_(bot)[slot]);
    }

    // ==================================================================
    // Bot-internal: set child
    // ==================================================================

    static void set_bot_child(uint64_t* bot, int slot, uint64_t* child) noexcept {
        bot_int_real_children_(bot)[slot] = reinterpret_cast<uint64_t>(child);
    }

    // ==================================================================
    // Bot-internal: add child
    // ==================================================================

    static uint64_t* add_bot_child(uint64_t* bot, uint8_t bi,
                                    uint64_t* child_ptr, ALLOC& alloc) {
        auto* h = get_header(bot);
        Bitmap256& bm = bm_(bot);
        int oc = h->entries;
        int nc = oc + 1;
        size_t needed = bot_internal_size_u64(nc);

        // --- In-place ---
        if (needed <= h->alloc_u64) {
            Bitmap256::arr_insert(bm, bot_int_real_children_(bot), oc, bi,
                                  reinterpret_cast<uint64_t>(child_ptr));
            h->entries = static_cast<uint16_t>(nc);
            return bot;
        }

        // --- Realloc ---
        int isl = bm.find_slot<SlotMode::UNFILTERED>(bi);
        size_t au64 = round_up_u64(needed);
        uint64_t* nb = alloc_node(alloc, au64);
        auto* nh = get_header(nb);
        nh->entries = static_cast<uint16_t>(nc);
        nh->alloc_u64 = static_cast<uint16_t>(au64);
        nh->set_bitmask();

        bm_(nb) = bm;
        bm_(nb).set_bit(bi);
        bot_int_children_(nb)[0] = reinterpret_cast<uint64_t>(SENTINEL_NODE);

        Bitmap256::arr_copy_insert(bot_int_real_children_(bot),
                                    bot_int_real_children_(nb),
                                    oc, isl,
                                    reinterpret_cast<uint64_t>(child_ptr));

        dealloc_node(alloc, bot, h->alloc_u64);
        return nb;
    }

    // ==================================================================
    // Bot-internal: remove child
    // ==================================================================

    static uint64_t* remove_bot_child(uint64_t* bot, int slot, uint8_t bi,
                                       ALLOC& alloc) {
        auto* h = get_header(bot);
        int oc = h->entries;
        int nc = oc - 1;
        size_t needed = bot_internal_size_u64(nc);

        // --- In-place ---
        if (!should_shrink_u64(h->alloc_u64, needed)) {
            Bitmap256::arr_remove(bm_(bot), bot_int_real_children_(bot),
                                  oc, slot, bi);
            h->entries = static_cast<uint16_t>(nc);
            return bot;
        }

        // --- Realloc ---
        size_t au64 = round_up_u64(needed);
        uint64_t* nb = alloc_node(alloc, au64);
        auto* nh = get_header(nb);
        nh->entries = static_cast<uint16_t>(nc);
        nh->alloc_u64 = static_cast<uint16_t>(au64);
        nh->set_bitmask();

        bm_(nb) = bm_(bot);
        bm_(nb).clear_bit(bi);
        bot_int_children_(nb)[0] = reinterpret_cast<uint64_t>(SENTINEL_NODE);

        Bitmap256::arr_copy_remove(bot_int_real_children_(bot),
                                    bot_int_real_children_(nb),
                                    oc, slot);

        dealloc_node(alloc, bot, h->alloc_u64);
        return nb;
    }

    // ==================================================================
    // Bot-internal: child count / alloc
    // ==================================================================

    static int bot_internal_child_count(const uint64_t* bot) noexcept {
        return get_header(bot)->entries;
    }

    static size_t bot_internal_alloc_u64(const uint64_t* bot) noexcept {
        return get_header(bot)->alloc_u64;
    }

    // ==================================================================
    // Bot-internal: iterate  cb(uint8_t bi, uint64_t* child)
    // ==================================================================

    template<typename Fn>
    static void for_each_bot_child(const uint64_t* bot, Fn&& cb) {
        const Bitmap256& bm = bm_(bot);
        const uint64_t* rch = bot_int_real_children_(bot);
        int slot = 0;
        for (int bi = bm.find_next_set(0); bi >= 0; bi = bm.find_next_set(bi + 1))
            cb(static_cast<uint8_t>(bi), reinterpret_cast<uint64_t*>(rch[slot++]));
    }

    // ==================================================================
    // Bot-internal: deallocate
    // ==================================================================

    static void dealloc_bot_internal(uint64_t* bot, ALLOC& alloc) noexcept {
        dealloc_node(alloc, bot, get_header(bot)->alloc_u64);
    }

private:
    // --- shared: bitmap is always at HEADER_U64 offset ---
    template<class T>
    static auto& bm_(T* n) noexcept {
        using R = std::conditional_t<std::is_const_v<T>, const Bitmap256, Bitmap256>;
        return *reinterpret_cast<R*>(n + HEADER_U64);
    }

    // --- split-top only: second bitmap for bot-is-internal flags ---
    template<class T>
    static auto& bot_is_internal_bm_(T* n) noexcept {
        using R = std::conditional_t<std::is_const_v<T>, const Bitmap256, Bitmap256>;
        return *reinterpret_cast<R*>(n + HEADER_U64 + BITMAP256_U64);
    }

    template<int BITS, class T>
    static T* top_children_(T* n) noexcept {
        if constexpr (BITS == 16) return n + HEADER_U64 + BITMAP256_U64;
        else return n + HEADER_U64 + BITMAP256_U64 + BITMAP256_U64;
    }

    template<int BITS, class T>
    static T* top_real_children_(T* n) noexcept {
        if constexpr (BITS == 16) return top_children_<BITS>(n);
        else return top_children_<BITS>(n) + 1;
    }

    // --- bot-leaf-16 layout: values after bitmap ---
    template<class T>
    static auto bot_leaf_vals_16_(T* b) noexcept {
        using R = std::conditional_t<std::is_const_v<T>, const VST, VST>;
        return reinterpret_cast<R*>(b + HEADER_U64 + BITMAP256_U64);
    }

    // --- bot-internal layout: sentinel + children after bitmap ---
    template<class T>
    static T* bot_int_children_(T* b) noexcept {
        return b + HEADER_U64 + BITMAP256_U64;
    }

    template<class T>
    static T* bot_int_real_children_(T* b) noexcept {
        return bot_int_children_(b) + 1;
    }

    // ==================================================================
    // Bot-leaf-16 insert/erase
    // ==================================================================

    template<bool INSERT = true, bool ASSIGN = true>
    requires (INSERT || ASSIGN)
    static BotLeafInsertResult insert_bl_16_(
            uint64_t* bot, uint64_t ik, VST value, ALLOC& alloc) {
        uint8_t suffix = static_cast<uint8_t>(KOps::template extract_suffix<8>(ik));
        auto* bh = get_header(bot);
        Bitmap256& bm = bm_(bot);
        uint32_t count = bh->entries;
        VST* vd = bot_leaf_vals_16_(bot);

        if (bm.has_bit(suffix)) {
            if constexpr (ASSIGN) {
                int slot = bm.find_slot<SlotMode::UNFILTERED>(suffix);
                VT::destroy(vd[slot], alloc);
                VT::write_slot(&vd[slot], value);
            }
            return {bot, false, false};
        }

        if constexpr (!INSERT) return {bot, false, false};

        uint32_t nc = count + 1;
        size_t new_sz = bot_leaf_size_u64<16>(nc);

        // --- In-place ---
        if (new_sz <= bh->alloc_u64) {
            int isl = bm.find_slot<SlotMode::UNFILTERED>(suffix);
            bm.set_bit(suffix);
            std::memmove(vd + isl + 1, vd + isl, (count - isl) * sizeof(VST));
            VT::write_slot(&vd[isl], value);
            bh->entries = static_cast<uint16_t>(nc);
            return {bot, true, false};
        }

        uint64_t* nb = alloc_node(alloc, new_sz);
        auto* nbh = get_header(nb);
        nbh->entries = static_cast<uint16_t>(nc);
        nbh->alloc_u64 = static_cast<uint16_t>(new_sz);
        Bitmap256& nbm = bm_(nb);
        nbm = bm; nbm.set_bit(suffix);
        VST* nvd = bot_leaf_vals_16_(nb);
        int isl = nbm.find_slot<SlotMode::UNFILTERED>(suffix);
        std::memcpy(nvd, vd, isl * sizeof(VST));
        VT::write_slot(&nvd[isl], value);
        std::memcpy(nvd + isl + 1, vd + isl, (count - isl) * sizeof(VST));

        dealloc_node(alloc, bot, bh->alloc_u64);
        return {nb, true, false};
    }

    static EraseResult erase_bl_16_(uint64_t* bot, uint64_t ik, ALLOC& alloc) {
        uint8_t suffix = static_cast<uint8_t>(KOps::template extract_suffix<8>(ik));
        auto* bh = get_header(bot);
        Bitmap256& bm = bm_(bot);
        if (!bm.has_bit(suffix)) return {bot, false};
        uint32_t count = bh->entries;
        int slot = bm.find_slot<SlotMode::UNFILTERED>(suffix);
        VT::destroy(bot_leaf_vals_16_(bot)[slot], alloc);

        uint32_t nc = count - 1;
        if (nc == 0) {
            dealloc_node(alloc, bot, bh->alloc_u64);
            return {nullptr, true};
        }

        size_t new_sz = bot_leaf_size_u64<16>(nc);
        if (!should_shrink_u64(bh->alloc_u64, new_sz)) {
            VST* vd = bot_leaf_vals_16_(bot);
            bm.clear_bit(suffix);
            std::memmove(vd + slot, vd + slot + 1, (nc - slot) * sizeof(VST));
            bh->entries = static_cast<uint16_t>(nc);
            return {bot, true};
        }

        uint64_t* nb = alloc_node(alloc, new_sz);
        auto* nbh = get_header(nb);
        nbh->entries = static_cast<uint16_t>(nc);
        nbh->alloc_u64 = static_cast<uint16_t>(new_sz);
        Bitmap256& nbm = bm_(nb);
        nbm = bm; nbm.clear_bit(suffix);
        const VST* ov = bot_leaf_vals_16_(bot);
        VST*       nv = bot_leaf_vals_16_(nb);
        std::memcpy(nv, ov, slot * sizeof(VST));
        std::memcpy(nv + slot, ov + slot + 1, (nc - slot) * sizeof(VST));

        dealloc_node(alloc, bot, bh->alloc_u64);
        return {nb, true};
    }

};

} // namespace kn3

#endif // KNTRIE_BITMASK_HPP
