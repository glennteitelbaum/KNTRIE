#ifndef KNTRIE_BITMASK_HPP
#define KNTRIE_BITMASK_HPP

#include "kntrie_compact.hpp"

namespace kn3 {

// ==========================================================================
// 256-bit bitmap
// ==========================================================================

struct Bitmap256 {
    uint64_t words[4] = {0, 0, 0, 0};

    bool has_bit(uint8_t i) const noexcept {
        return words[i >> 6] & (1ULL << (i & 63));
    }
    void set_bit(uint8_t i) noexcept {
        words[i >> 6] |= (1ULL << (i & 63));
    }
    void clear_bit(uint8_t i) noexcept {
        words[i >> 6] &= ~(1ULL << (i & 63));
    }
    int popcount() const noexcept {
        return std::popcount(words[0]) + std::popcount(words[1])
             + std::popcount(words[2]) + std::popcount(words[3]);
    }
    bool find_slot(uint8_t index, int& slot) const noexcept {
        const int w = index >> 6, b = index & 63;
        uint64_t before = words[w] << (63 - b);
        if (!(before & (1ULL << 63))) return false;
        int pc0 = std::popcount(words[0]);
        int pc1 = std::popcount(words[1]);
        int pc2 = std::popcount(words[2]);
        slot = std::popcount(before) - 1;
        slot += pc0 & -int(w > 0);
        slot += pc1 & -int(w > 1);
        slot += pc2 & -int(w > 2);
        return true;
    }
    int slot_for_insert(uint8_t index) const noexcept {
        const int w = index >> 6, b = index & 63;
        int pc0 = std::popcount(words[0]);
        int pc1 = std::popcount(words[1]);
        int pc2 = std::popcount(words[2]);
        int s = std::popcount(words[w] & ((1ULL << b) - 1));
        s += pc0 & -int(w > 0);
        s += pc1 & -int(w > 1);
        s += pc2 & -int(w > 2);
        return s;
    }
    int count_below(uint8_t index) const noexcept { return slot_for_insert(index); }
    int find_next_set(int start) const noexcept {
        if (start >= 256) return -1;
        int w = start >> 6, b = start & 63;
        uint64_t masked = words[w] & ~((1ULL << b) - 1);
        if (masked) return (w << 6) + std::countr_zero(masked);
        for (int i = w + 1; i < 4; ++i)
            if (words[i]) return (i << 6) + std::countr_zero(words[i]);
        return -1;
    }
};

// ==========================================================================
// BitmaskOps  – split-top / bot-leaf / bot-internal abstraction
//
//   Public interface only — all layout details are private.
//
//   Split-top: [header][top_bm_256][bot_is_leaf_bm_256 (BITS>16)][bot_ptrs...]
//   Bot-leaf BITS==16:  [bm_256][values...]
//   Bot-leaf BITS>16:   [count:u32 pad][search overlay][values...]
//   Bot-internal:       [bm_256][child_ptrs...]
// ==========================================================================

template<typename KEY, typename VALUE, typename ALLOC>
struct BitmaskOps {
    using KOps = KeyOps<KEY>;
    using VT   = ValueTraits<VALUE, ALLOC>;
    using VST  = typename VT::slot_type;
    using CO   = CompactOps<KEY, VALUE, ALLOC>;

    // ==================================================================
    // Size calculations  (for stats / memory accounting)
    // ==================================================================

    template<int BITS>
    static constexpr size_t split_top_size_u64(size_t top_count, uint8_t skip) noexcept {
        if constexpr (BITS == 16)
            return header_u64(skip) + BITMAP256_U64 + top_count;
        else
            return header_u64(skip) + BITMAP256_U64 + BITMAP256_U64 + top_count;
    }

    template<int BITS>
    static constexpr size_t bot_leaf_size_u64(size_t count) noexcept {
        if constexpr (BITS == 16) {
            size_t vb = count * sizeof(VST);
            vb = (vb + 7) & ~size_t{7};
            return BITMAP256_U64 + vb / 8;
        } else {
            constexpr int sb = BITS - 8;
            using S = typename suffix_traits<sb>::type;
            int ex = KnSearch<S>::extra(static_cast<int>(count));
            size_t search_bytes = (static_cast<size_t>(ex) + count) * sizeof(S);
            search_bytes = (search_bytes + 7) & ~size_t{7};
            size_t vb = count * sizeof(VST);
            vb = (vb + 7) & ~size_t{7};
            return 1 + search_bytes / 8 + vb / 8;
        }
    }

    static constexpr size_t bot_internal_size_u64(size_t count) noexcept {
        return BITMAP256_U64 + count;
    }

    // ==================================================================
    // Split-top: lookup
    // ==================================================================

    struct TopLookup {
        bool found;
        int  slot;
        uint64_t* bot;
        bool is_leaf;
    };

    template<int BITS>
    static TopLookup lookup_top(const uint64_t* node, uint8_t ti) noexcept {
        const Bitmap256& tbm = top_bitmap_(node);
        int slot;
        if (!tbm.find_slot(ti, slot))
            return {false, -1, nullptr, false};

        auto* bot = reinterpret_cast<uint64_t*>(top_children_<BITS>(node)[slot]);
        bool is_leaf;
        if constexpr (BITS == 16) is_leaf = true;
        else is_leaf = bot_is_leaf_bm_(node).has_bit(ti);

        return {true, slot, bot, is_leaf};
    }

    // ==================================================================
    // Split-top: mutate child pointer in-place
    // ==================================================================

    template<int BITS>
    static void set_top_child(uint64_t* node, int slot, uint64_t* ptr) noexcept {
        top_children_<BITS>(node)[slot] = reinterpret_cast<uint64_t>(ptr);
    }

    // ==================================================================
    // Split-top: add a new top slot  (returns new node, old deallocated)
    // ==================================================================

    template<int BITS>
    static uint64_t* add_top_slot(uint64_t* node, NodeHeader* h,
                                   uint8_t ti, uint64_t* bot_ptr,
                                   bool is_leaf, ALLOC& alloc) {
        Bitmap256& tbm = top_bitmap_(node);
        size_t otc = h->top_count, ntc = otc + 1;
        int isl = tbm.slot_for_insert(ti);

        uint64_t* nn = alloc_node(alloc, split_top_size_u64<BITS>(ntc, h->skip));
        auto* nh = get_header(nn); *nh = *h;
        nh->count = h->count + 1;
        nh->top_count = static_cast<uint16_t>(ntc);
        if (h->skip > 0) set_prefix(nn, get_prefix(node));

        top_bitmap_(nn) = tbm;
        top_bitmap_(nn).set_bit(ti);

        if constexpr (BITS > 16) {
            bot_is_leaf_bm_(nn) = bot_is_leaf_bm_(node);
            if (is_leaf) bot_is_leaf_bm_(nn).set_bit(ti);
        }

        const uint64_t* oc = top_children_<BITS>(node);
        uint64_t*       nc = top_children_<BITS>(nn);
        for (int i = 0; i < isl; ++i)            nc[i] = oc[i];
        nc[isl] = reinterpret_cast<uint64_t>(bot_ptr);
        for (size_t i = isl; i < otc; ++i)       nc[i + 1] = oc[i];

        dealloc_node(alloc, node, split_top_size_u64<BITS>(otc, h->skip));
        return nn;
    }

    // ==================================================================
    // Split-top: remove a top slot  (returns new node or nullptr if empty)
    // ==================================================================

    template<int BITS>
    static uint64_t* remove_top_slot(uint64_t* node, NodeHeader* h,
                                      int slot, uint8_t ti, ALLOC& alloc) {
        size_t otc = h->top_count, ntc = otc - 1;

        if (ntc == 0) {
            dealloc_node(alloc, node, split_top_size_u64<BITS>(otc, h->skip));
            return nullptr;
        }

        uint64_t* nn = alloc_node(alloc, split_top_size_u64<BITS>(ntc, h->skip));
        auto* nh = get_header(nn); *nh = *h;
        nh->count = h->count - 1;
        nh->top_count = static_cast<uint16_t>(ntc);
        if (h->skip > 0) set_prefix(nn, get_prefix(node));

        top_bitmap_(nn) = top_bitmap_(node);
        top_bitmap_(nn).clear_bit(ti);
        if constexpr (BITS > 16) {
            bot_is_leaf_bm_(nn) = bot_is_leaf_bm_(node);
            bot_is_leaf_bm_(nn).clear_bit(ti);
        }

        const uint64_t* oc = top_children_<BITS>(node);
        uint64_t*       nc = top_children_<BITS>(nn);
        for (int i = 0; i < slot; ++i)            nc[i] = oc[i];
        for (size_t i = slot; i < ntc; ++i)       nc[i] = oc[i + 1];

        dealloc_node(alloc, node, split_top_size_u64<BITS>(otc, h->skip));
        return nn;
    }

    // ==================================================================
    // Split-top: mark a bot entry as no longer leaf
    // ==================================================================

    template<int BITS>
    static void mark_bot_not_leaf(uint64_t* node, uint8_t ti) noexcept {
        if constexpr (BITS > 16)
            bot_is_leaf_bm_(node).clear_bit(ti);
    }

    // ==================================================================
    // Split-top: recompute h->is_leaf from bot_is_leaf bitmap
    // ==================================================================

    template<int BITS>
    static void update_any_leaf_flag(uint64_t* node) noexcept {
        if constexpr (BITS > 16) {
            auto* h = get_header(node);
            const Bitmap256& tbm = top_bitmap_(node);
            const Bitmap256& ilbm = bot_is_leaf_bm_(node);
            bool any = false;
            for (int i = tbm.find_next_set(0); i >= 0; i = tbm.find_next_set(i + 1))
                if (ilbm.has_bit(i)) { any = true; break; }
            if (!any) h->set_leaf(false);
        }
    }

    // ==================================================================
    // Split-top: iterate  cb(uint8_t ti, int slot, uint64_t* bot, bool is_leaf)
    // ==================================================================

    template<int BITS, typename Fn>
    static void for_each_top(const uint64_t* node, Fn&& cb) {
        const Bitmap256& tbm = top_bitmap_(node);
        const uint64_t* ch = top_children_<BITS>(node);
        int slot = 0;
        for (int ti = tbm.find_next_set(0); ti >= 0; ti = tbm.find_next_set(ti + 1)) {
            auto* bot = reinterpret_cast<uint64_t*>(ch[slot]);
            bool is_leaf;
            if constexpr (BITS == 16) is_leaf = true;
            else is_leaf = bot_is_leaf_bm_(node).has_bit(ti);
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
                                     uint32_t total_count, ALLOC& alloc) {
        Bitmap256 tbm{};
        for (int i = 0; i < n_tops; ++i) tbm.set_bit(top_indices[i]);

        uint64_t* nn = alloc_node(alloc, split_top_size_u64<BITS>(n_tops, skip));
        auto* nh = get_header(nn);
        nh->count = total_count;
        nh->top_count = static_cast<uint16_t>(n_tops);
        nh->skip = skip;
        nh->set_split(true);
        if (skip > 0) set_prefix(nn, prefix);

        top_bitmap_(nn) = tbm;

        if constexpr (BITS > 16) {
            Bitmap256 ilbm{};
            for (int i = 0; i < n_tops; ++i)
                if (is_leaf_flags[i]) ilbm.set_bit(top_indices[i]);
            bot_is_leaf_bm_(nn) = ilbm;
        }

        // Set header leaf flag based on whether any bot is a leaf
        bool any_leaf = false;
        for (int i = 0; i < n_tops; ++i)
            if (is_leaf_flags[i]) { any_leaf = true; break; }
        nh->set_leaf(any_leaf);

        // Write bot pointers in bitmap order
        uint64_t* ch = top_children_<BITS>(nn);
        for (int i = 0; i < n_tops; ++i) {
            int slot = tbm.slot_for_insert(top_indices[i]);
            // Since we build the bitmap from these same indices,
            // count_below gives the correct dense slot.
            // But actually we need to handle the fact that set_bit changes
            // the bitmap. Since we already built tbm, slot_for_insert
            // on the final bitmap gives count of set bits below, which
            // equals the dense position. But we might have inserted bits
            // that confuse the count. Let me just iterate in order.
        }
        // Simpler: iterate in bitmap order and assign
        int slot = 0;
        for (int ti = tbm.find_next_set(0); ti >= 0; ti = tbm.find_next_set(ti + 1)) {
            // Find which input index matches this ti
            for (int i = 0; i < n_tops; ++i) {
                if (top_indices[i] == static_cast<uint8_t>(ti)) {
                    ch[slot] = reinterpret_cast<uint64_t>(bot_ptrs[i]);
                    break;
                }
            }
            ++slot;
        }

        return nn;
    }

    // ==================================================================
    // Split-top: deallocate (top node only, not children)
    // ==================================================================

    template<int BITS>
    static void dealloc_split_top(uint64_t* node, ALLOC& alloc) noexcept {
        auto* h = get_header(node);
        dealloc_node(alloc, node, split_top_size_u64<BITS>(h->top_count, h->skip));
    }

    // ==================================================================
    // Bot-leaf: count
    // ==================================================================

    template<int BITS>
    static uint32_t bot_leaf_count(const uint64_t* bot) noexcept {
        if constexpr (BITS == 16) return bot_leaf_bm_(bot).popcount();
        else return *reinterpret_cast<const uint32_t*>(bot);
    }

    // ==================================================================
    // Bot-leaf: find
    // ==================================================================

    template<int BITS>
    static const VALUE* find_in_bot_leaf(const uint64_t* bot, uint64_t ik) noexcept {
        if constexpr (BITS == 16) {
            uint8_t suffix = static_cast<uint8_t>(KOps::template extract_suffix<8>(ik));
            const Bitmap256& bm = bot_leaf_bm_(bot);
            if (!bm.has_bit(suffix)) return nullptr;
            int slot = bm.count_below(suffix);
            return VT::as_ptr(bot_leaf_vals_<16>(bot, 0)[slot]);
        } else {
            constexpr int sb = BITS - 8;
            using S = typename suffix_traits<sb>::type;
            S suffix = static_cast<S>(KOps::template extract_suffix<sb>(ik));
            uint32_t count = bot_leaf_count<BITS>(bot);
            int idx = KnSearch<S>::search(
                bot_leaf_search_start_<BITS>(bot),
                static_cast<int>(count), suffix);
            if (idx < 0) return nullptr;
            return VT::as_ptr(bot_leaf_vals_<BITS>(bot, count)[idx]);
        }
    }

    // ==================================================================
    // Bot-leaf: insert  (returns new bot, doesn't touch split-top)
    // ==================================================================

    struct BotLeafInsertResult {
        uint64_t* new_bot;
        bool inserted;
        bool overflow;   // count was >= BOT_LEAF_MAX, needs convert
    };

    template<int BITS>
    static BotLeafInsertResult insert_into_bot_leaf(
            uint64_t* bot, uint64_t ik, VST value, ALLOC& alloc) {
        if constexpr (BITS == 16)
            return insert_bl_16_(bot, ik, value, alloc);
        else
            return insert_bl_list_<BITS>(bot, ik, value, alloc);
    }

    // ==================================================================
    // Bot-leaf: erase  ({nullptr,true} when empty)
    // ==================================================================

    template<int BITS>
    static EraseResult erase_from_bot_leaf(uint64_t* bot, uint64_t ik, ALLOC& alloc) {
        if constexpr (BITS == 16)
            return erase_bl_16_(bot, ik, alloc);
        else
            return erase_bl_list_<BITS>(bot, ik, alloc);
    }

    // ==================================================================
    // Bot-leaf: make single-entry
    // ==================================================================

    template<int BITS>
    static uint64_t* make_single_bot_leaf(uint64_t ik, VST value, ALLOC& alloc) {
        uint64_t* bot = alloc_node(alloc, bot_leaf_size_u64<BITS>(1));
        if constexpr (BITS == 16) {
            auto& bm = bot_leaf_bm_(bot);
            bm = Bitmap256{};
            uint8_t suffix = static_cast<uint8_t>(KOps::template extract_suffix<8>(ik));
            bm.set_bit(suffix);
            VT::write_slot(&bot_leaf_vals_<16>(bot, 1)[0], value);
        } else {
            constexpr int sb = BITS - 8;
            using S = typename suffix_traits<sb>::type;
            set_bl_count_<BITS>(bot, 1);
            auto* kd = bot_leaf_keys_data_<BITS>(bot, 1);
            kd[0] = static_cast<S>(KOps::template extract_suffix<sb>(ik));
            VT::write_slot(&bot_leaf_vals_<BITS>(bot, 1)[0], value);
            KnSearch<S>::build(bot_leaf_search_start_<BITS>(bot), kd, 1);
        }
        return bot;
    }

    // ==================================================================
    // Bot-leaf: make from sorted working arrays
    //   BITS==16: suffixes are uint8_t
    //   BITS>16:  suffixes are suffix_traits<BITS-8>::type (already sorted)
    // ==================================================================

    template<int BITS>
    static uint64_t* make_bot_leaf(
            const typename suffix_traits<(BITS > 16 ? BITS - 8 : 8)>::type* sorted_suffixes,
            const VST* values, uint32_t count, ALLOC& alloc) {
        uint64_t* bot = alloc_node(alloc, bot_leaf_size_u64<BITS>(count));
        if constexpr (BITS == 16) {
            auto& bm = bot_leaf_bm_(bot);
            bm = Bitmap256{};
            for (uint32_t i = 0; i < count; ++i) bm.set_bit(sorted_suffixes[i]);
            auto* vd = bot_leaf_vals_<16>(bot, count);
            // Values must be written in bitmap order
            for (uint32_t i = 0; i < count; ++i)
                vd[bm.count_below(sorted_suffixes[i])] = values[i];
        } else {
            constexpr int sb = BITS - 8;
            using S = typename suffix_traits<sb>::type;
            set_bl_count_<BITS>(bot, count);
            auto* kd = bot_leaf_keys_data_<BITS>(bot, count);
            auto* vd = bot_leaf_vals_<BITS>(bot, count);
            std::memcpy(kd, sorted_suffixes, count * sizeof(S));
            std::memcpy(vd, values, count * sizeof(VST));
            KnSearch<S>::build(bot_leaf_search_start_<BITS>(bot), kd, static_cast<int>(count));
        }
        return bot;
    }

    // ==================================================================
    // Bot-leaf: iterate  cb(suffix, value_slot)
    //   BITS==16: suffix is uint8_t
    //   BITS>16:  suffix is suffix_traits<BITS-8>::type
    // ==================================================================

    template<int BITS, typename Fn>
    static void for_each_bot_leaf(const uint64_t* bot, Fn&& cb) {
        if constexpr (BITS == 16) {
            const Bitmap256& bm = bot_leaf_bm_(bot);
            uint32_t count = bm.popcount();
            const VST* vd = bot_leaf_vals_<16>(bot, count);
            int slot = 0;
            for (int i = bm.find_next_set(0); i >= 0; i = bm.find_next_set(i + 1))
                cb(static_cast<uint8_t>(i), vd[slot++]);
        } else {
            constexpr int sb = BITS - 8;
            using S = typename suffix_traits<sb>::type;
            uint32_t count = bot_leaf_count<BITS>(bot);
            const S*   kd = bot_leaf_keys_data_<BITS>(bot, count);
            const VST* vd = bot_leaf_vals_<BITS>(bot, count);
            for (uint32_t i = 0; i < count; ++i) cb(kd[i], vd[i]);
        }
    }

    // ==================================================================
    // Bot-leaf: destroy values + deallocate
    // ==================================================================

    template<int BITS>
    static void destroy_bot_leaf_and_dealloc(uint64_t* bot, ALLOC& alloc) {
        uint32_t count = bot_leaf_count<BITS>(bot);
        if constexpr (!VT::is_inline) {
            if constexpr (BITS == 16) {
                auto* vd = bot_leaf_vals_<16>(bot, count);
                for (uint32_t i = 0; i < count; ++i) VT::destroy(vd[i], alloc);
            } else {
                auto* vd = bot_leaf_vals_<BITS>(bot, count);
                for (uint32_t i = 0; i < count; ++i) VT::destroy(vd[i], alloc);
            }
        }
        dealloc_node(alloc, bot, bot_leaf_size_u64<BITS>(count));
    }

    // Deallocate without destroying values (ownership transferred via working arrays)
    template<int BITS>
    static void dealloc_bot_leaf(uint64_t* bot, uint32_t count, ALLOC& alloc) noexcept {
        dealloc_node(alloc, bot, bot_leaf_size_u64<BITS>(count));
    }

    // ==================================================================
    // Bot-internal: make from arrays  (indices must be sorted or not — bitmap handles order)
    // ==================================================================

    static uint64_t* make_bot_internal(const uint8_t* indices,
                                        uint64_t* const* child_ptrs,
                                        int count, ALLOC& alloc) {
        Bitmap256 bm{};
        for (int i = 0; i < count; ++i) bm.set_bit(indices[i]);

        uint64_t* bot = alloc_node(alloc, bot_internal_size_u64(count));
        bot_bitmap_(bot) = bm;
        uint64_t* ch = bot_int_children_(bot);
        // Write children in bitmap order
        int slot = 0;
        for (int bi = bm.find_next_set(0); bi >= 0; bi = bm.find_next_set(bi + 1)) {
            for (int i = 0; i < count; ++i) {
                if (indices[i] == static_cast<uint8_t>(bi)) {
                    ch[slot] = reinterpret_cast<uint64_t>(child_ptrs[i]);
                    break;
                }
            }
            ++slot;
        }
        return bot;
    }

    // ==================================================================
    // Bot-internal: lookup
    // ==================================================================

    struct BotChildLookup {
        bool found;
        int  slot;
        uint64_t* child;
    };

    static BotChildLookup lookup_bot_child(const uint64_t* bot, uint8_t bi) noexcept {
        const Bitmap256& bm = bot_bitmap_(bot);
        int slot;
        if (!bm.find_slot(bi, slot))
            return {false, -1, nullptr};
        auto* child = reinterpret_cast<uint64_t*>(bot_int_children_(bot)[slot]);
        return {true, slot, child};
    }

    // ==================================================================
    // Bot-internal: set child pointer in-place
    // ==================================================================

    static void set_bot_child(uint64_t* bot, int slot, uint64_t* ptr) noexcept {
        bot_int_children_(bot)[slot] = reinterpret_cast<uint64_t>(ptr);
    }

    // ==================================================================
    // Bot-internal: add child  (returns new bot)
    // ==================================================================

    static uint64_t* add_bot_child(uint64_t* bot, uint8_t bi,
                                    uint64_t* child_ptr, ALLOC& alloc) {
        Bitmap256& bm = bot_bitmap_(bot);
        int bc = bm.popcount();
        int isl = bm.slot_for_insert(bi);

        uint64_t* nb = alloc_node(alloc, bot_internal_size_u64(bc + 1));
        bot_bitmap_(nb) = bm;
        bot_bitmap_(nb).set_bit(bi);

        const uint64_t* och = bot_int_children_(bot);
        uint64_t*       nch = bot_int_children_(nb);
        for (int i = 0; i < isl; ++i)     nch[i] = och[i];
        nch[isl] = reinterpret_cast<uint64_t>(child_ptr);
        for (int i = isl; i < bc; ++i)    nch[i + 1] = och[i];

        dealloc_node(alloc, bot, bot_internal_size_u64(bc));
        return nb;
    }

    // ==================================================================
    // Bot-internal: remove child  (returns new bot, nullptr if was last)
    // ==================================================================

    static uint64_t* remove_bot_child(uint64_t* bot, int slot, uint8_t bi,
                                       ALLOC& alloc) {
        Bitmap256& bm = bot_bitmap_(bot);
        int bc = bm.popcount();

        if (bc == 1) {
            dealloc_node(alloc, bot, bot_internal_size_u64(bc));
            return nullptr;
        }

        uint64_t* nb = alloc_node(alloc, bot_internal_size_u64(bc - 1));
        bot_bitmap_(nb) = bm;
        bot_bitmap_(nb).clear_bit(bi);

        const uint64_t* och = bot_int_children_(bot);
        uint64_t*       nch = bot_int_children_(nb);
        for (int i = 0; i < slot; ++i)         nch[i] = och[i];
        for (int i = slot; i < bc - 1; ++i)    nch[i] = och[i + 1];

        dealloc_node(alloc, bot, bot_internal_size_u64(bc));
        return nb;
    }

    // ==================================================================
    // Bot-internal: child count
    // ==================================================================

    static int bot_internal_child_count(const uint64_t* bot) noexcept {
        return bot_bitmap_(bot).popcount();
    }

    // ==================================================================
    // Bot-internal: iterate  cb(uint8_t bi, uint64_t* child)
    // ==================================================================

    template<typename Fn>
    static void for_each_bot_child(const uint64_t* bot, Fn&& cb) {
        const Bitmap256& bm = bot_bitmap_(bot);
        const uint64_t* ch = bot_int_children_(bot);
        int slot = 0;
        for (int bi = bm.find_next_set(0); bi >= 0; bi = bm.find_next_set(bi + 1))
            cb(static_cast<uint8_t>(bi), reinterpret_cast<uint64_t*>(ch[slot++]));
    }

    // ==================================================================
    // Bot-internal: deallocate (no children — caller handles them)
    // ==================================================================

    static void dealloc_bot_internal(uint64_t* bot, ALLOC& alloc) noexcept {
        int bc = bot_bitmap_(bot).popcount();
        dealloc_node(alloc, bot, bot_internal_size_u64(bc));
    }

private:
    // ==================================================================
    // Layout internals — NOT accessible outside BitmaskOps
    // ==================================================================

    // --- split-top ---

    static Bitmap256& top_bitmap_(uint64_t* n) noexcept {
        return *reinterpret_cast<Bitmap256*>(n + header_u64(get_header(n)->skip));
    }
    static const Bitmap256& top_bitmap_(const uint64_t* n) noexcept {
        return *reinterpret_cast<const Bitmap256*>(n + header_u64(get_header(n)->skip));
    }
    static Bitmap256& bot_is_leaf_bm_(uint64_t* n) noexcept {
        return *reinterpret_cast<Bitmap256*>(n + header_u64(get_header(n)->skip) + BITMAP256_U64);
    }
    static const Bitmap256& bot_is_leaf_bm_(const uint64_t* n) noexcept {
        return *reinterpret_cast<const Bitmap256*>(n + header_u64(get_header(n)->skip) + BITMAP256_U64);
    }
    template<int BITS>
    static uint64_t* top_children_(uint64_t* n) noexcept {
        size_t off = header_u64(get_header(n)->skip) + BITMAP256_U64;
        if constexpr (BITS != 16) off += BITMAP256_U64;
        return n + off;
    }
    template<int BITS>
    static const uint64_t* top_children_(const uint64_t* n) noexcept {
        size_t off = header_u64(get_header(n)->skip) + BITMAP256_U64;
        if constexpr (BITS != 16) off += BITMAP256_U64;
        return n + off;
    }

    // --- bot-leaf BITS==16 ---

    static Bitmap256& bot_leaf_bm_(uint64_t* b) noexcept {
        return *reinterpret_cast<Bitmap256*>(b);
    }
    static const Bitmap256& bot_leaf_bm_(const uint64_t* b) noexcept {
        return *reinterpret_cast<const Bitmap256*>(b);
    }

    // --- bot-leaf BITS>16 ---

    template<int BITS>
    static void set_bl_count_(uint64_t* b, uint32_t c) noexcept {
        static_assert(BITS != 16);
        *reinterpret_cast<uint32_t*>(b) = c;
    }
    template<int BITS>
    static auto bot_leaf_search_start_(uint64_t* b) noexcept {
        static_assert(BITS > 16);
        using S = typename suffix_traits<BITS - 8>::type;
        return reinterpret_cast<S*>(b + 1);
    }
    template<int BITS>
    static auto bot_leaf_search_start_(const uint64_t* b) noexcept {
        static_assert(BITS > 16);
        using S = typename suffix_traits<BITS - 8>::type;
        return reinterpret_cast<const S*>(b + 1);
    }
    template<int BITS>
    static auto bot_leaf_keys_data_(uint64_t* b, size_t count) noexcept {
        static_assert(BITS > 16);
        using S = typename suffix_traits<BITS - 8>::type;
        return KnSearch<S>::keys_ptr(bot_leaf_search_start_<BITS>(b), static_cast<int>(count));
    }
    template<int BITS>
    static auto bot_leaf_keys_data_(const uint64_t* b, size_t count) noexcept {
        static_assert(BITS > 16);
        using S = typename suffix_traits<BITS - 8>::type;
        return KnSearch<S>::keys_ptr(bot_leaf_search_start_<BITS>(b), static_cast<int>(count));
    }

    // --- bot-leaf values ---

    template<int BITS>
    static VST* bot_leaf_vals_(uint64_t* b, [[maybe_unused]] size_t count) noexcept {
        if constexpr (BITS == 16) {
            return reinterpret_cast<VST*>(b + BITMAP256_U64);
        } else {
            constexpr int sb = BITS - 8;
            using S = typename suffix_traits<sb>::type;
            int ex = KnSearch<S>::extra(static_cast<int>(count));
            size_t search_bytes = (static_cast<size_t>(ex) + count) * sizeof(S);
            search_bytes = (search_bytes + 7) & ~size_t{7};
            return reinterpret_cast<VST*>(reinterpret_cast<char*>(b + 1) + search_bytes);
        }
    }
    template<int BITS>
    static const VST* bot_leaf_vals_(const uint64_t* b, [[maybe_unused]] size_t count) noexcept {
        if constexpr (BITS == 16) {
            return reinterpret_cast<const VST*>(b + BITMAP256_U64);
        } else {
            constexpr int sb = BITS - 8;
            using S = typename suffix_traits<sb>::type;
            int ex = KnSearch<S>::extra(static_cast<int>(count));
            size_t search_bytes = (static_cast<size_t>(ex) + count) * sizeof(S);
            search_bytes = (search_bytes + 7) & ~size_t{7};
            return reinterpret_cast<const VST*>(reinterpret_cast<const char*>(b + 1) + search_bytes);
        }
    }

    // --- bot-internal ---

    static Bitmap256& bot_bitmap_(uint64_t* b) noexcept {
        return *reinterpret_cast<Bitmap256*>(b);
    }
    static const Bitmap256& bot_bitmap_(const uint64_t* b) noexcept {
        return *reinterpret_cast<const Bitmap256*>(b);
    }
    static uint64_t* bot_int_children_(uint64_t* b) noexcept {
        return b + BITMAP256_U64;
    }
    static const uint64_t* bot_int_children_(const uint64_t* b) noexcept {
        return b + BITMAP256_U64;
    }

    // ==================================================================
    // Bot-leaf insert implementations
    // ==================================================================

    static BotLeafInsertResult insert_bl_16_(
            uint64_t* bot, uint64_t ik, VST value, ALLOC& alloc) {
        uint8_t suffix = static_cast<uint8_t>(KOps::template extract_suffix<8>(ik));
        Bitmap256& bm = bot_leaf_bm_(bot);
        uint32_t count = bm.popcount();
        VST* vd = bot_leaf_vals_<16>(bot, count);

        if (bm.has_bit(suffix)) {
            int slot = bm.count_below(suffix);
            VT::destroy(vd[slot], alloc);
            VT::write_slot(&vd[slot], value);
            return {bot, false, false};
        }

        uint32_t nc = count + 1;
        uint64_t* nb = alloc_node(alloc, bot_leaf_size_u64<16>(nc));
        auto& nbm = bot_leaf_bm_(nb);
        nbm = bm; nbm.set_bit(suffix);
        VST* nv = bot_leaf_vals_<16>(nb, nc);
        int is = nbm.count_below(suffix);
        std::memcpy(nv, vd, is * sizeof(VST));
        VT::write_slot(&nv[is], value);
        std::memcpy(nv + is + 1, vd + is, (count - is) * sizeof(VST));

        dealloc_node(alloc, bot, bot_leaf_size_u64<16>(count));
        return {nb, true, false};
    }

    template<int BITS>
    static BotLeafInsertResult insert_bl_list_(
            uint64_t* bot, uint64_t ik, VST value, ALLOC& alloc) {
        constexpr int sb = BITS - 8;
        using S = typename suffix_traits<sb>::type;

        uint32_t count = bot_leaf_count<BITS>(bot);
        S suffix = static_cast<S>(KOps::template extract_suffix<sb>(ik));
        S*   kd = bot_leaf_keys_data_<BITS>(bot, count);
        VST* vd = bot_leaf_vals_<BITS>(bot, count);

        int idx = binary_search_for_insert(kd, static_cast<size_t>(count), suffix);
        if (idx >= 0) {
            VT::destroy(vd[idx], alloc);
            VT::write_slot(&vd[idx], value);
            return {bot, false, false};
        }
        size_t ins = static_cast<size_t>(-(idx + 1));

        if (count >= BOT_LEAF_MAX)
            return {bot, false, true};

        uint32_t nc = count + 1;
        uint64_t* nb = alloc_node(alloc, bot_leaf_size_u64<BITS>(nc));
        set_bl_count_<BITS>(nb, nc);
        S*   nk = bot_leaf_keys_data_<BITS>(nb, nc);
        VST* nv = bot_leaf_vals_<BITS>(nb, nc);

        std::memcpy(nk, kd, ins * sizeof(S));
        std::memcpy(nv, vd, ins * sizeof(VST));
        nk[ins] = suffix;
        VT::write_slot(&nv[ins], value);
        std::memcpy(nk + ins + 1, kd + ins, (count - ins) * sizeof(S));
        std::memcpy(nv + ins + 1, vd + ins, (count - ins) * sizeof(VST));

        KnSearch<S>::build(bot_leaf_search_start_<BITS>(nb), nk, static_cast<int>(nc));

        dealloc_node(alloc, bot, bot_leaf_size_u64<BITS>(count));
        return {nb, true, false};
    }

    // ==================================================================
    // Bot-leaf erase implementations
    // ==================================================================

    static EraseResult erase_bl_16_(uint64_t* bot, uint64_t ik, ALLOC& alloc) {
        uint8_t suffix = static_cast<uint8_t>(KOps::template extract_suffix<8>(ik));
        Bitmap256& bm = bot_leaf_bm_(bot);
        if (!bm.has_bit(suffix)) return {bot, false};

        uint32_t count = bm.popcount();
        int slot = bm.count_below(suffix);
        VST* vd = bot_leaf_vals_<16>(bot, count);
        VT::destroy(vd[slot], alloc);

        uint32_t nc = count - 1;
        if (nc == 0) {
            dealloc_node(alloc, bot, bot_leaf_size_u64<16>(count));
            return {nullptr, true};
        }

        uint64_t* nb = alloc_node(alloc, bot_leaf_size_u64<16>(nc));
        auto& nbm = bot_leaf_bm_(nb);
        nbm = bm; nbm.clear_bit(suffix);
        VST* nv = bot_leaf_vals_<16>(nb, nc);
        std::memcpy(nv,        vd,          slot * sizeof(VST));
        std::memcpy(nv + slot, vd + slot + 1, (nc - slot) * sizeof(VST));

        dealloc_node(alloc, bot, bot_leaf_size_u64<16>(count));
        return {nb, true};
    }

    template<int BITS>
    static EraseResult erase_bl_list_(uint64_t* bot, uint64_t ik, ALLOC& alloc) {
        constexpr int sb = BITS - 8;
        using S = typename suffix_traits<sb>::type;

        uint32_t count = bot_leaf_count<BITS>(bot);
        S suffix = static_cast<S>(KOps::template extract_suffix<sb>(ik));
        S*   kd = bot_leaf_keys_data_<BITS>(bot, count);
        VST* vd = bot_leaf_vals_<BITS>(bot, count);

        int idx = binary_search_for_insert(kd, static_cast<size_t>(count), suffix);
        if (idx < 0) return {bot, false};
        VT::destroy(vd[idx], alloc);

        uint32_t nc = count - 1;
        if (nc == 0) {
            dealloc_node(alloc, bot, bot_leaf_size_u64<BITS>(count));
            return {nullptr, true};
        }

        uint64_t* nb = alloc_node(alloc, bot_leaf_size_u64<BITS>(nc));
        set_bl_count_<BITS>(nb, nc);
        S*   nk = bot_leaf_keys_data_<BITS>(nb, nc);
        VST* nv = bot_leaf_vals_<BITS>(nb, nc);
        size_t pos = static_cast<size_t>(idx);
        std::memcpy(nk,       kd,           pos * sizeof(S));
        std::memcpy(nv,       vd,           pos * sizeof(VST));
        std::memcpy(nk + pos, kd + pos + 1, (nc - pos) * sizeof(S));
        std::memcpy(nv + pos, vd + pos + 1, (nc - pos) * sizeof(VST));
        KnSearch<S>::build(bot_leaf_search_start_<BITS>(nb), nk, static_cast<int>(nc));

        dealloc_node(alloc, bot, bot_leaf_size_u64<BITS>(count));
        return {nb, true};
    }
};

} // namespace kn3

#endif // KNTRIE_BITMASK_HPP
