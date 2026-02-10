#ifndef KNTRIE_BITMASK_HPP
#define KNTRIE_BITMASK_HPP

#include "kntrie_support.hpp"
#include "kntrie_compact.hpp"

namespace kn3 {

// ==========================================================================
// 256-bit bitmap
// ==========================================================================

struct Bitmap256 {
    uint64_t words[4] = {0, 0, 0, 0};

    bool has_bit(uint8_t i) const noexcept { return words[i >> 6] & (1ULL << (i & 63)); }
    void set_bit(uint8_t i) noexcept { words[i >> 6] |= (1ULL << (i & 63)); }
    void clear_bit(uint8_t i) noexcept { words[i >> 6] &= ~(1ULL << (i & 63)); }

    int popcount() const noexcept {
        return std::popcount(words[0]) + std::popcount(words[1]) +
               std::popcount(words[2]) + std::popcount(words[3]);
    }

    bool find_slot(uint8_t index, int& slot) const noexcept {
        const int w = index >> 6, b = index & 63;
        uint64_t before = words[w] << (63 - b);
        if (!(before & (1ULL << 63))) [[unlikely]] return false;
        int pc0 = std::popcount(words[0]);
        int pc1 = std::popcount(words[1]);
        int pc2 = std::popcount(words[2]);
        slot = std::popcount(before) - 1;
        slot += pc0 & -int(w > 0);
        slot += pc1 & -int(w > 1);
        slot += pc2 & -int(w > 2);
        return true;
    }

    int find_slot_1(uint8_t index) const noexcept {
        const int w = index >> 6, b = index & 63;
        uint64_t before = words[w] << (63 - b);
        int pc0 = std::popcount(words[0]);
        int pc1 = std::popcount(words[1]);
        int pc2 = std::popcount(words[2]);
        int slot = std::popcount(before);
        slot += pc0 & -int(w > 0);
        slot += pc1 & -int(w > 1);
        slot += pc2 & -int(w > 2);
        bool found = before & (1ULL << 63);
        slot &= -int(found);
        return slot;
    }

    int slot_for_insert(uint8_t index) const noexcept {
        const int w = index >> 6, b = index & 63;
        int pc0 = std::popcount(words[0]);
        int pc1 = std::popcount(words[1]);
        int pc2 = std::popcount(words[2]);
        int slot = std::popcount(words[w] & ((1ULL << b) - 1));
        slot += pc0 & -int(w > 0);
        slot += pc1 & -int(w > 1);
        slot += pc2 & -int(w > 2);
        return slot;
    }

    int count_below(uint8_t index) const noexcept { return slot_for_insert(index); }

    int find_next_set(int start) const noexcept {
        if (start >= 256) return -1;
        int w = start >> 6, b = start & 63;
        uint64_t masked = words[w] & ~((1ULL << b) - 1);
        if (masked) return (w << 6) + std::countr_zero(masked);
        for (int ww = w + 1; ww < 4; ++ww)
            if (words[ww]) return (ww << 6) + std::countr_zero(words[ww]);
        return -1;
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
    // Split-top: lookup
    // ==================================================================

    struct TopLookup {
        uint64_t* bot;
        int       slot;
        bool      found;
        bool      is_leaf;
    };

    template<int BITS>
    static TopLookup lookup_top(const uint64_t* node, uint8_t ti) noexcept {
        const Bitmap256& tbm = top_bitmap_(node);
        int slot;
        bool found = tbm.find_slot(ti, slot);
        if (!found) [[unlikely]] return {nullptr, -1, false, false};

        auto* bot = reinterpret_cast<uint64_t*>(top_real_children_<BITS>(node)[slot]);
        bool is_leaf;
        if constexpr (BITS == 16) is_leaf = true;
        else is_leaf = !bot_is_internal_bm_(node).has_bit(ti);
        return {bot, slot, true, is_leaf};
    }

    // ==================================================================
    // Split-top: branchless descent
    // ==================================================================

    template<int BITS>
    static const uint64_t* branchless_top_child(const uint64_t* node, uint8_t ti) noexcept {
        static_assert(BITS > 16);
        const Bitmap256& tbm = top_bitmap_(node);
        int slot = tbm.find_slot_1(ti);
        return reinterpret_cast<const uint64_t*>(top_children_<BITS>(node)[slot]);
    }

    template<int BITS>
    static bool is_top_entry_leaf(const uint64_t* node, uint8_t ti) noexcept {
        if constexpr (BITS == 16) return true;
        else return !bot_is_internal_bm_(node).has_bit(ti);
    }

    // ==================================================================
    // Split-top: set child pointer
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
        Bitmap256& tbm = top_bitmap_(node);
        size_t otc = h->entries, ntc = otc + 1;
        int isl = tbm.slot_for_insert(ti);
        size_t needed = split_top_size_u64<BITS>(ntc);

        if (needed <= h->alloc_u64) {
            uint64_t* rc = top_real_children_<BITS>(node);
            std::memmove(rc + isl + 1, rc + isl, (otc - isl) * sizeof(uint64_t));
            rc[isl] = reinterpret_cast<uint64_t>(bot_ptr);
            tbm.set_bit(ti);
            if constexpr (BITS > 16) {
                if (!is_leaf) bot_is_internal_bm_(node).set_bit(ti);
            }
            h->entries = static_cast<uint16_t>(ntc);
            h->add_descendants(1);
            return node;
        }

        size_t au64 = round_up_u64(needed);
        uint64_t* nn = alloc_node(alloc, au64);
        auto* nh = get_header(nn); *nh = *h;
        nh->entries = static_cast<uint16_t>(ntc);
        nh->add_descendants(1);
        nh->alloc_u64 = static_cast<uint16_t>(au64);
        if (h->skip() > 0) set_prefix(nn, get_prefix(node));

        top_bitmap_(nn) = tbm;
        top_bitmap_(nn).set_bit(ti);

        if constexpr (BITS > 16) {
            bot_is_internal_bm_(nn) = bot_is_internal_bm_(node);
            if (!is_leaf) bot_is_internal_bm_(nn).set_bit(ti);
            top_children_<BITS>(nn)[0] = reinterpret_cast<uint64_t>(SENTINEL_NODE);
        }

        const uint64_t* oc = top_real_children_<BITS>(node);
        uint64_t*       nc = top_real_children_<BITS>(nn);
        for (int i = 0; i < isl; ++i)            nc[i] = oc[i];
        nc[isl] = reinterpret_cast<uint64_t>(bot_ptr);
        for (size_t i = isl; i < otc; ++i)       nc[i + 1] = oc[i];

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

        if (!should_shrink_u64(h->alloc_u64, needed)) {
            uint64_t* rc = top_real_children_<BITS>(node);
            std::memmove(rc + slot, rc + slot + 1, (ntc - slot) * sizeof(uint64_t));
            top_bitmap_(node).clear_bit(ti);
            if constexpr (BITS > 16) {
                bot_is_internal_bm_(node).clear_bit(ti);
            }
            h->entries = static_cast<uint16_t>(ntc);
            h->sub_descendants(1);
            return node;
        }

        size_t au64 = round_up_u64(needed);
        uint64_t* nn = alloc_node(alloc, au64);
        auto* nh = get_header(nn); *nh = *h;
        nh->entries = static_cast<uint16_t>(ntc);
        nh->sub_descendants(1);
        nh->alloc_u64 = static_cast<uint16_t>(au64);
        if (h->skip() > 0) set_prefix(nn, get_prefix(node));

        top_bitmap_(nn) = top_bitmap_(node);
        top_bitmap_(nn).clear_bit(ti);
        if constexpr (BITS > 16) {
            bot_is_internal_bm_(nn) = bot_is_internal_bm_(node);
            bot_is_internal_bm_(nn).clear_bit(ti);
            top_children_<BITS>(nn)[0] = reinterpret_cast<uint64_t>(SENTINEL_NODE);
        }

        const uint64_t* oc = top_real_children_<BITS>(node);
        uint64_t*       nc = top_real_children_<BITS>(nn);
        for (int i = 0; i < slot; ++i)            nc[i] = oc[i];
        for (size_t i = slot; i < ntc; ++i)       nc[i] = oc[i + 1];

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
    // Split-top: iterate
    // ==================================================================

    template<int BITS, typename Fn>
    static void for_each_top(const uint64_t* node, Fn&& cb) {
        const Bitmap256& tbm = top_bitmap_(node);
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
    // Split-top: make from arrays
    // ==================================================================

    template<int BITS>
    static uint64_t* make_split_top(const uint8_t* top_indices,
                                     uint64_t* const* bot_ptrs,
                                     const bool* is_leaf_flags,
                                     int n_tops, uint8_t skip, uint64_t prefix,
                                     uint32_t total_descendants, ALLOC& alloc) {
        Bitmap256 tbm{};
        for (int i = 0; i < n_tops; ++i) tbm.set_bit(top_indices[i]);

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

        top_bitmap_(nn) = tbm;

        if constexpr (BITS > 16) {
            Bitmap256 iibm{};
            for (int i = 0; i < n_tops; ++i)
                if (!is_leaf_flags[i]) iibm.set_bit(top_indices[i]);
            bot_is_internal_bm_(nn) = iibm;
            top_children_<BITS>(nn)[0] = reinterpret_cast<uint64_t>(SENTINEL_NODE);
        }

        uint64_t* rch = top_real_children_<BITS>(nn);
        int slot = 0;
        for (int ti = tbm.find_next_set(0); ti >= 0; ti = tbm.find_next_set(ti + 1)) {
            for (int i = 0; i < n_tops; ++i) {
                if (top_indices[i] == static_cast<uint8_t>(ti)) {
                    rch[slot] = reinterpret_cast<uint64_t>(bot_ptrs[i]);
                    break;
                }
            }
            ++slot;
        }

        return nn;
    }

    // ==================================================================
    // Split-top: deallocate
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
            const Bitmap256& bm = bot_leaf_bm_(bot);
            if (!bm.has_bit(suffix)) [[unlikely]] return nullptr;
            int slot = bm.count_below(suffix);
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
            bot_leaf_bm_(bot).set_bit(suffix);
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
            auto& bm = bot_leaf_bm_(bot);
            bm = Bitmap256{};
            for (uint32_t i = 0; i < count; ++i) bm.set_bit(sorted_suffixes[i]);
            auto* vd = bot_leaf_vals_16_(bot);
            for (uint32_t i = 0; i < count; ++i)
                vd[bm.count_below(sorted_suffixes[i])] = values[i];
            return bot;
        } else {
            return CO::template make_leaf<BITS - 8>(
                sorted_suffixes, values, count, 0, 0, alloc);
        }
    }

    // ==================================================================
    // Bot-leaf: iterate
    // ==================================================================

    template<int BITS, typename Fn>
    static void for_each_bot_leaf(const uint64_t* bot, Fn&& cb) {
        if constexpr (BITS == 16) {
            const Bitmap256& bm = bot_leaf_bm_(bot);
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
        Bitmap256 bm{};
        for (int i = 0; i < n_children; ++i) bm.set_bit(indices[i]);

        size_t needed = bot_internal_size_u64(n_children);
        size_t au64 = round_up_u64(needed);
        uint64_t* bot = alloc_node(alloc, au64);
        auto* h = get_header(bot);
        h->entries = static_cast<uint16_t>(n_children);
        h->alloc_u64 = static_cast<uint16_t>(au64);
        h->set_bitmask();

        bot_bitmap_(bot) = bm;
        bot_int_children_(bot)[0] = reinterpret_cast<uint64_t>(SENTINEL_NODE);

        uint64_t* rch = bot_int_real_children_(bot);
        int slot = 0;
        for (int bi = bm.find_next_set(0); bi >= 0; bi = bm.find_next_set(bi + 1)) {
            for (int i = 0; i < n_children; ++i) {
                if (indices[i] == static_cast<uint8_t>(bi)) {
                    rch[slot] = reinterpret_cast<uint64_t>(child_ptrs[i]);
                    break;
                }
            }
            ++slot;
        }
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
        const Bitmap256& bm = bot_bitmap_(bot);
        int slot;
        bool found = bm.find_slot(bi, slot);
        if (!found) return {nullptr, -1, false};
        auto* child = reinterpret_cast<uint64_t*>(bot_int_real_children_(bot)[slot]);
        return {child, slot, true};
    }

    // ==================================================================
    // Bot-internal: branchless descent
    // ==================================================================

    static const uint64_t* branchless_bot_child(const uint64_t* bot, uint8_t bi) noexcept {
        const Bitmap256& bm = bot_bitmap_(bot);
        int slot = bm.find_slot_1(bi);
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
        Bitmap256& bm = bot_bitmap_(bot);
        int oc = h->entries;
        int isl = bm.slot_for_insert(bi);
        int nc = oc + 1;
        size_t needed = bot_internal_size_u64(nc);

        if (needed <= h->alloc_u64) {
            uint64_t* rch = bot_int_real_children_(bot);
            std::memmove(rch + isl + 1, rch + isl, (oc - isl) * sizeof(uint64_t));
            rch[isl] = reinterpret_cast<uint64_t>(child_ptr);
            bm.set_bit(bi);
            h->entries = static_cast<uint16_t>(nc);
            return bot;
        }

        size_t au64 = round_up_u64(needed);
        uint64_t* nb = alloc_node(alloc, au64);
        auto* nh = get_header(nb);
        nh->entries = static_cast<uint16_t>(nc);
        nh->alloc_u64 = static_cast<uint16_t>(au64);
        nh->set_bitmask();

        bot_bitmap_(nb) = bm;
        bot_bitmap_(nb).set_bit(bi);

        bot_int_children_(nb)[0] = reinterpret_cast<uint64_t>(SENTINEL_NODE);

        const uint64_t* orc = bot_int_real_children_(bot);
        uint64_t*       nrc = bot_int_real_children_(nb);
        for (int i = 0; i < isl; ++i)        nrc[i] = orc[i];
        nrc[isl] = reinterpret_cast<uint64_t>(child_ptr);
        for (int i = isl; i < oc; ++i)       nrc[i + 1] = orc[i];

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

        if (!should_shrink_u64(h->alloc_u64, needed)) {
            uint64_t* rch = bot_int_real_children_(bot);
            std::memmove(rch + slot, rch + slot + 1, (nc - slot) * sizeof(uint64_t));
            bot_bitmap_(bot).clear_bit(bi);
            h->entries = static_cast<uint16_t>(nc);
            return bot;
        }

        size_t au64 = round_up_u64(needed);
        uint64_t* nb = alloc_node(alloc, au64);
        auto* nh = get_header(nb);
        nh->entries = static_cast<uint16_t>(nc);
        nh->alloc_u64 = static_cast<uint16_t>(au64);
        nh->set_bitmask();

        bot_bitmap_(nb) = bot_bitmap_(bot);
        bot_bitmap_(nb).clear_bit(bi);

        bot_int_children_(nb)[0] = reinterpret_cast<uint64_t>(SENTINEL_NODE);

        const uint64_t* orc = bot_int_real_children_(bot);
        uint64_t*       nrc = bot_int_real_children_(nb);
        for (int i = 0; i < slot; ++i)        nrc[i] = orc[i];
        for (int i = slot; i < nc; ++i)       nrc[i] = orc[i + 1];

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
    // Bot-internal: iterate
    // ==================================================================

    template<typename Fn>
    static void for_each_bot_child(const uint64_t* bot, Fn&& cb) {
        const Bitmap256& bm = bot_bitmap_(bot);
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
    static Bitmap256& top_bitmap_(uint64_t* n) noexcept {
        return *reinterpret_cast<Bitmap256*>(n + HEADER_U64);
    }
    static const Bitmap256& top_bitmap_(const uint64_t* n) noexcept {
        return *reinterpret_cast<const Bitmap256*>(n + HEADER_U64);
    }
    static Bitmap256& bot_is_internal_bm_(uint64_t* n) noexcept {
        return *reinterpret_cast<Bitmap256*>(n + HEADER_U64 + BITMAP256_U64);
    }
    static const Bitmap256& bot_is_internal_bm_(const uint64_t* n) noexcept {
        return *reinterpret_cast<const Bitmap256*>(n + HEADER_U64 + BITMAP256_U64);
    }

    template<int BITS>
    static uint64_t* top_children_(uint64_t* n) noexcept {
        if constexpr (BITS == 16) return n + HEADER_U64 + BITMAP256_U64;
        else return n + HEADER_U64 + BITMAP256_U64 + BITMAP256_U64;
    }
    template<int BITS>
    static const uint64_t* top_children_(const uint64_t* n) noexcept {
        if constexpr (BITS == 16) return n + HEADER_U64 + BITMAP256_U64;
        else return n + HEADER_U64 + BITMAP256_U64 + BITMAP256_U64;
    }

    template<int BITS>
    static uint64_t* top_real_children_(uint64_t* n) noexcept {
        if constexpr (BITS == 16) return top_children_<BITS>(n);
        else return top_children_<BITS>(n) + 1;
    }
    template<int BITS>
    static const uint64_t* top_real_children_(const uint64_t* n) noexcept {
        if constexpr (BITS == 16) return top_children_<BITS>(n);
        else return top_children_<BITS>(n) + 1;
    }

    static Bitmap256& bot_leaf_bm_(uint64_t* b) noexcept {
        return *reinterpret_cast<Bitmap256*>(b + HEADER_U64);
    }
    static const Bitmap256& bot_leaf_bm_(const uint64_t* b) noexcept {
        return *reinterpret_cast<const Bitmap256*>(b + HEADER_U64);
    }

    static VST* bot_leaf_vals_16_(uint64_t* b) noexcept {
        return reinterpret_cast<VST*>(b + HEADER_U64 + BITMAP256_U64);
    }
    static const VST* bot_leaf_vals_16_(const uint64_t* b) noexcept {
        return reinterpret_cast<const VST*>(b + HEADER_U64 + BITMAP256_U64);
    }

    static Bitmap256& bot_bitmap_(uint64_t* b) noexcept {
        return *reinterpret_cast<Bitmap256*>(b + HEADER_U64);
    }
    static const Bitmap256& bot_bitmap_(const uint64_t* b) noexcept {
        return *reinterpret_cast<const Bitmap256*>(b + HEADER_U64);
    }
    static uint64_t* bot_int_children_(uint64_t* b) noexcept {
        return b + HEADER_U64 + BITMAP256_U64;
    }
    static const uint64_t* bot_int_children_(const uint64_t* b) noexcept {
        return b + HEADER_U64 + BITMAP256_U64;
    }
    static uint64_t* bot_int_real_children_(uint64_t* b) noexcept {
        return b + HEADER_U64 + BITMAP256_U64 + 1;
    }
    static const uint64_t* bot_int_real_children_(const uint64_t* b) noexcept {
        return b + HEADER_U64 + BITMAP256_U64 + 1;
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
        Bitmap256& bm = bot_leaf_bm_(bot);
        uint32_t count = bh->entries;
        VST* vd = bot_leaf_vals_16_(bot);

        if (bm.has_bit(suffix)) {
            if constexpr (ASSIGN) {
                int slot = bm.count_below(suffix);
                VT::destroy(vd[slot], alloc);
                VT::write_slot(&vd[slot], value);
            }
            return {bot, false, false};
        }

        if constexpr (!INSERT) return {bot, false, false};

        uint32_t nc = count + 1;
        size_t new_sz = bot_leaf_size_u64<16>(nc);

        if (new_sz <= bh->alloc_u64) {
            int isl = bm.count_below(suffix);
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
        Bitmap256& nbm = bot_leaf_bm_(nb);
        nbm = bm; nbm.set_bit(suffix);
        VST* nvd = bot_leaf_vals_16_(nb);
        int isl = nbm.count_below(suffix);
        std::memcpy(nvd, vd, isl * sizeof(VST));
        VT::write_slot(&nvd[isl], value);
        std::memcpy(nvd + isl + 1, vd + isl, (count - isl) * sizeof(VST));

        dealloc_node(alloc, bot, bh->alloc_u64);
        return {nb, true, false};
    }

    static EraseResult erase_bl_16_(uint64_t* bot, uint64_t ik, ALLOC& alloc) {
        uint8_t suffix = static_cast<uint8_t>(KOps::template extract_suffix<8>(ik));
        auto* bh = get_header(bot);
        Bitmap256& bm = bot_leaf_bm_(bot);
        if (!bm.has_bit(suffix)) return {bot, false};
        uint32_t count = bh->entries;
        int slot = bm.count_below(suffix);
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
        Bitmap256& nbm = bot_leaf_bm_(nb);
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
