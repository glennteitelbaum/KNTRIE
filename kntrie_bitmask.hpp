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

    // Original find_slot: 0-based, returns false if not found
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

    // Branchless find_slot: returns 1-based slot on hit, 0 on miss.
    // Designed for children arrays with sentinel at index 0.
    int find_slot_1(uint8_t index) const noexcept {
        const int w = index >> 6, b = index & 63;
        uint64_t before = words[w] << (63 - b);
        int pc0 = std::popcount(words[0]);
        int pc1 = std::popcount(words[1]);
        int pc2 = std::popcount(words[2]);
        int slot = std::popcount(before);  // 1-based when found
        slot += pc0 & -int(w > 0);
        slot += pc1 & -int(w > 1);
        slot += pc2 & -int(w > 2);
        bool found = before & (1ULL << 63);
        slot &= -int(found);              // 0 if not found
        return slot;
    }

    // Count set bits below index (0-based dense position for insert)
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
// BitmaskOps  – split-top, bot-leaf, bot-internal operations
//
// Children layout for BITS > 16 (split-top and bot-internal):
//   [sentinel_ptr, child_0, child_1, ...]
//   find_slot_1 returns 1-based → children[slot] is correct
//   insert/erase use top_real_children_ (0-based, skips sentinel)
//
// Children layout for BITS == 16 (split-top only):
//   [child_0, child_1, ...]   (no sentinel — terminal level)
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
    static constexpr size_t split_top_size_u64(size_t top_count, uint8_t skip) noexcept {
        if constexpr (BITS == 16)
            return header_u64(skip) + BITMAP256_U64 + top_count;
        else
            return header_u64(skip) + BITMAP256_U64 + BITMAP256_U64 + 1 + top_count;
            //                        top_bm          bot_is_int_bm   sentinel  children
    }

    template<int BITS>
    static constexpr size_t bot_leaf_size_u64(size_t count) noexcept {
        if constexpr (BITS == 16) {
            size_t vb = count * sizeof(VST); vb = (vb + 7) & ~size_t{7};
            return BITMAP256_U64 + vb / 8;
        } else {
            constexpr int sb = BITS - 8;
            using S = typename suffix_traits<sb>::type;
            int ex = KnSearch<S>::extra(static_cast<int>(count));
            size_t skb = (static_cast<size_t>(ex) + count) * sizeof(S);
            skb = (skb + 7) & ~size_t{7};
            size_t vb = count * sizeof(VST); vb = (vb + 7) & ~size_t{7};
            return 1 + (skb + vb) / 8;  // 1 u64 for count
        }
    }

    static constexpr size_t bot_internal_size_u64(size_t count) noexcept {
        return BITMAP256_U64 + 1 + count;  // bitmap + sentinel + children
    }

    // ==================================================================
    // Split-top: lookup (for insert/erase — 0-based slot via real_children)
    // ==================================================================

    struct TopLookup {
        uint64_t* bot;
        int       slot;   // 0-based into real children
        bool      found;
        bool      is_leaf;
    };

    template<int BITS>
    static TopLookup lookup_top(const uint64_t* node, uint8_t ti) noexcept {
        const Bitmap256& tbm = top_bitmap_(node);
        int slot;
        bool found = tbm.find_slot(ti, slot);
        if (!found) return {nullptr, -1, false, false};

        auto* bot = reinterpret_cast<uint64_t*>(top_real_children_<BITS>(node)[slot]);
        bool is_leaf;
        if constexpr (BITS == 16) is_leaf = true;
        else is_leaf = !bot_is_internal_bm_(node).has_bit(ti);
        return {bot, slot, true, is_leaf};
    }

    // ==================================================================
    // Split-top: branchless descent (for find — uses sentinel children)
    //   Returns child pointer; sentinel on miss. BITS > 16 only.
    // ==================================================================

    template<int BITS>
    static const uint64_t* branchless_top_child(const uint64_t* node, uint8_t ti) noexcept {
        static_assert(BITS > 16);
        const Bitmap256& tbm = top_bitmap_(node);
        int slot = tbm.find_slot_1(ti);
        return reinterpret_cast<const uint64_t*>(top_children_<BITS>(node)[slot]);
    }

    // Check if top entry is a leaf (needed for the one unavoidable branch)
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
            bot_is_internal_bm_(nn) = bot_is_internal_bm_(node);
            if (!is_leaf) bot_is_internal_bm_(nn).set_bit(ti);
            // Write sentinel
            top_children_<BITS>(nn)[0] = reinterpret_cast<uint64_t>(SENTINEL_NODE);
        }

        const uint64_t* oc = top_real_children_<BITS>(node);
        uint64_t*       nc = top_real_children_<BITS>(nn);
        for (int i = 0; i < isl; ++i)            nc[i] = oc[i];
        nc[isl] = reinterpret_cast<uint64_t>(bot_ptr);
        for (size_t i = isl; i < otc; ++i)       nc[i + 1] = oc[i];

        dealloc_node(alloc, node, split_top_size_u64<BITS>(otc, h->skip));
        return nn;
    }

    // ==================================================================
    // Split-top: remove a top slot
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
            bot_is_internal_bm_(nn) = bot_is_internal_bm_(node);
            bot_is_internal_bm_(nn).clear_bit(ti);
            top_children_<BITS>(nn)[0] = reinterpret_cast<uint64_t>(SENTINEL_NODE);
        }

        const uint64_t* oc = top_real_children_<BITS>(node);
        uint64_t*       nc = top_real_children_<BITS>(nn);
        for (int i = 0; i < slot; ++i)            nc[i] = oc[i];
        for (size_t i = slot; i < ntc; ++i)       nc[i] = oc[i + 1];

        dealloc_node(alloc, node, split_top_size_u64<BITS>(otc, h->skip));
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
    // Split-top: recompute header internal flag from bot_is_internal bitmap
    // ==================================================================

    template<int BITS>
    static void update_internal_flag(uint64_t* node) noexcept {
        if constexpr (BITS > 16) {
            auto* h = get_header(node);
            const Bitmap256& tbm = top_bitmap_(node);
            const Bitmap256& iibm = bot_is_internal_bm_(node);
            bool all_internal = true;
            for (int i = tbm.find_next_set(0); i >= 0; i = tbm.find_next_set(i + 1))
                if (!iibm.has_bit(i)) { all_internal = false; break; }
            h->set_internal(all_internal);
        }
    }

    // ==================================================================
    // Split-top: iterate  cb(uint8_t ti, int slot, uint64_t* bot, bool is_leaf)
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
            // bot_is_internal: set bit for internal (non-leaf) bots
            Bitmap256 iibm{};
            for (int i = 0; i < n_tops; ++i)
                if (!is_leaf_flags[i]) iibm.set_bit(top_indices[i]);
            bot_is_internal_bm_(nn) = iibm;
            // Sentinel at children[0]
            top_children_<BITS>(nn)[0] = reinterpret_cast<uint64_t>(SENTINEL_NODE);
        }

        // Header internal flag: set if ALL bots are internal
        bool any_leaf = false;
        for (int i = 0; i < n_tops; ++i)
            if (is_leaf_flags[i]) { any_leaf = true; break; }
        nh->set_internal(!any_leaf);

        // Write real children in bitmap order
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
    // Bot-leaf: insert
    // ==================================================================

    struct BotLeafInsertResult {
        uint64_t* new_bot;
        bool inserted;
        bool overflow;
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
    // Bot-leaf: erase
    // ==================================================================

    template<int BITS>
    static EraseResult erase_from_bot_leaf(uint64_t* bot, uint64_t ik, ALLOC& alloc) {
        if constexpr (BITS == 16)
            return erase_bl_16_(bot, ik, alloc);
        else
            return erase_bl_list_<BITS>(bot, ik, alloc);
    }

    // ==================================================================
    // Bot-leaf: make single entry
    // ==================================================================

    template<int BITS>
    static uint64_t* make_single_bot_leaf(uint64_t ik, VST value, ALLOC& alloc) {
        uint64_t* bot = alloc_node(alloc, bot_leaf_size_u64<BITS>(1));
        if constexpr (BITS == 16) {
            uint8_t suffix = static_cast<uint8_t>(KOps::template extract_suffix<8>(ik));
            bot_leaf_bm_(bot).set_bit(suffix);
            VT::write_slot(&bot_leaf_vals_<16>(bot, 1)[0], value);
        } else {
            constexpr int sb = BITS - 8;
            using S = typename suffix_traits<sb>::type;
            S suffix = static_cast<S>(KOps::template extract_suffix<sb>(ik));
            set_bl_count_<BITS>(bot, 1);
            auto* kd = bot_leaf_keys_data_<BITS>(bot, 1);
            kd[0] = suffix;
            VT::write_slot(&bot_leaf_vals_<BITS>(bot, 1)[0], value);
            KnSearch<S>::build(bot_leaf_search_start_<BITS>(bot), kd, 1);
        }
        return bot;
    }

    // ==================================================================
    // Bot-leaf: make from sorted working arrays
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
            auto* vd = bot_leaf_vals_<BITS>(bot, count);
            for (uint32_t i = 0; i < count; ++i) VT::destroy(vd[i], alloc);
        }
        dealloc_node(alloc, bot, bot_leaf_size_u64<BITS>(count));
    }

    // Deallocate without destroying values (ownership transferred)
    template<int BITS>
    static void dealloc_bot_leaf(uint64_t* bot, uint32_t count, ALLOC& alloc) noexcept {
        dealloc_node(alloc, bot, bot_leaf_size_u64<BITS>(count));
    }

    // ==================================================================
    // Bot-internal: make from arrays
    // ==================================================================

    static uint64_t* make_bot_internal(const uint8_t* indices,
                                        uint64_t* const* child_ptrs,
                                        int n_children, ALLOC& alloc) {
        Bitmap256 bm{};
        for (int i = 0; i < n_children; ++i) bm.set_bit(indices[i]);

        uint64_t* bot = alloc_node(alloc, bot_internal_size_u64(n_children));
        bot_bitmap_(bot) = bm;

        // Sentinel at children[0]
        bot_int_children_(bot)[0] = reinterpret_cast<uint64_t>(SENTINEL_NODE);

        // Real children at [1..]
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
    // Bot-internal: lookup child (0-based slot, for insert/erase)
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
    // Bot-internal: branchless descent (for find — uses sentinel)
    // ==================================================================

    static const uint64_t* branchless_bot_child(const uint64_t* bot, uint8_t bi) noexcept {
        const Bitmap256& bm = bot_bitmap_(bot);
        int slot = bm.find_slot_1(bi);
        return reinterpret_cast<const uint64_t*>(bot_int_children_(bot)[slot]);
    }

    // ==================================================================
    // Bot-internal: set child (0-based slot)
    // ==================================================================

    static void set_bot_child(uint64_t* bot, int slot, uint64_t* child) noexcept {
        bot_int_real_children_(bot)[slot] = reinterpret_cast<uint64_t>(child);
    }

    // ==================================================================
    // Bot-internal: add child
    // ==================================================================

    static uint64_t* add_bot_child(uint64_t* bot, uint8_t bi,
                                    uint64_t* child_ptr, ALLOC& alloc) {
        Bitmap256& bm = bot_bitmap_(bot);
        int oc = bm.popcount();
        int isl = bm.slot_for_insert(bi);
        int nc = oc + 1;

        uint64_t* nb = alloc_node(alloc, bot_internal_size_u64(nc));
        bot_bitmap_(nb) = bm;
        bot_bitmap_(nb).set_bit(bi);

        // Sentinel
        bot_int_children_(nb)[0] = reinterpret_cast<uint64_t>(SENTINEL_NODE);

        const uint64_t* orc = bot_int_real_children_(bot);
        uint64_t*       nrc = bot_int_real_children_(nb);
        for (int i = 0; i < isl; ++i)        nrc[i] = orc[i];
        nrc[isl] = reinterpret_cast<uint64_t>(child_ptr);
        for (int i = isl; i < oc; ++i)       nrc[i + 1] = orc[i];

        dealloc_node(alloc, bot, bot_internal_size_u64(oc));
        return nb;
    }

    // ==================================================================
    // Bot-internal: remove child
    // ==================================================================

    static uint64_t* remove_bot_child(uint64_t* bot, int slot, uint8_t bi,
                                       ALLOC& alloc) {
        Bitmap256& bm = bot_bitmap_(bot);
        int oc = bm.popcount();
        int nc = oc - 1;

        uint64_t* nb = alloc_node(alloc, bot_internal_size_u64(nc));
        bot_bitmap_(nb) = bm;
        bot_bitmap_(nb).clear_bit(bi);

        bot_int_children_(nb)[0] = reinterpret_cast<uint64_t>(SENTINEL_NODE);

        const uint64_t* orc = bot_int_real_children_(bot);
        uint64_t*       nrc = bot_int_real_children_(nb);
        for (int i = 0; i < slot; ++i)        nrc[i] = orc[i];
        for (int i = slot; i < nc; ++i)       nrc[i] = orc[i + 1];

        dealloc_node(alloc, bot, bot_internal_size_u64(oc));
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
        const uint64_t* rch = bot_int_real_children_(bot);
        int slot = 0;
        for (int bi = bm.find_next_set(0); bi >= 0; bi = bm.find_next_set(bi + 1))
            cb(static_cast<uint8_t>(bi), reinterpret_cast<uint64_t*>(rch[slot++]));
    }

    // ==================================================================
    // Bot-internal: deallocate
    // ==================================================================

    static void dealloc_bot_internal(uint64_t* bot, ALLOC& alloc) noexcept {
        int c = bot_bitmap_(bot).popcount();
        dealloc_node(alloc, bot, bot_internal_size_u64(c));
    }

    // ==================================================================
    // Private layout accessors
    // ==================================================================

private:
    // --- split-top layout ---

    static Bitmap256& top_bitmap_(uint64_t* n) noexcept {
        return *reinterpret_cast<Bitmap256*>(n + header_u64(get_header(n)->skip));
    }
    static const Bitmap256& top_bitmap_(const uint64_t* n) noexcept {
        return *reinterpret_cast<const Bitmap256*>(n + header_u64(get_header(n)->skip));
    }
    static Bitmap256& bot_is_internal_bm_(uint64_t* n) noexcept {
        return *reinterpret_cast<Bitmap256*>(n + header_u64(get_header(n)->skip) + BITMAP256_U64);
    }
    static const Bitmap256& bot_is_internal_bm_(const uint64_t* n) noexcept {
        return *reinterpret_cast<const Bitmap256*>(n + header_u64(get_header(n)->skip) + BITMAP256_U64);
    }

    // Full children array (includes sentinel at [0] for BITS>16)
    template<int BITS>
    static uint64_t* top_children_(uint64_t* n) noexcept {
        if constexpr (BITS == 16)
            return n + header_u64(get_header(n)->skip) + BITMAP256_U64;
        else
            return n + header_u64(get_header(n)->skip) + BITMAP256_U64 + BITMAP256_U64;
    }
    template<int BITS>
    static const uint64_t* top_children_(const uint64_t* n) noexcept {
        if constexpr (BITS == 16)
            return n + header_u64(get_header(n)->skip) + BITMAP256_U64;
        else
            return n + header_u64(get_header(n)->skip) + BITMAP256_U64 + BITMAP256_U64;
    }

    // Real children (skips sentinel for BITS>16)
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

    // --- bot-leaf layout ---

    static Bitmap256& bot_leaf_bm_(uint64_t* b) noexcept {
        return *reinterpret_cast<Bitmap256*>(b);
    }
    static const Bitmap256& bot_leaf_bm_(const uint64_t* b) noexcept {
        return *reinterpret_cast<const Bitmap256*>(b);
    }

    template<int BITS>
    static void set_bl_count_(uint64_t* b, uint32_t c) noexcept {
        static_assert(BITS != 16);
        *reinterpret_cast<uint32_t*>(b) = c;
    }
    template<int BITS>
    static auto bot_leaf_search_start_(uint64_t* b) noexcept {
        static_assert(BITS > 16);
        constexpr int sb = BITS - 8;
        using S = typename suffix_traits<sb>::type;
        return reinterpret_cast<S*>(b + 1);
    }
    template<int BITS>
    static auto bot_leaf_search_start_(const uint64_t* b) noexcept {
        static_assert(BITS > 16);
        constexpr int sb = BITS - 8;
        using S = typename suffix_traits<sb>::type;
        return reinterpret_cast<const S*>(b + 1);
    }
    template<int BITS>
    static auto bot_leaf_keys_data_(uint64_t* b, size_t count) noexcept {
        static_assert(BITS > 16);
        constexpr int sb = BITS - 8;
        using S = typename suffix_traits<sb>::type;
        return KnSearch<S>::keys_ptr(bot_leaf_search_start_<BITS>(b), static_cast<int>(count));
    }
    template<int BITS>
    static auto bot_leaf_keys_data_(const uint64_t* b, size_t count) noexcept {
        static_assert(BITS > 16);
        constexpr int sb = BITS - 8;
        using S = typename suffix_traits<sb>::type;
        return KnSearch<S>::keys_ptr(bot_leaf_search_start_<BITS>(b), static_cast<int>(count));
    }

    template<int BITS>
    static VST* bot_leaf_vals_(uint64_t* b, [[maybe_unused]] size_t count) noexcept {
        if constexpr (BITS == 16) {
            return reinterpret_cast<VST*>(b + BITMAP256_U64);
        } else {
            constexpr int sb = BITS - 8;
            using S = typename suffix_traits<sb>::type;
            int ex = KnSearch<S>::extra(static_cast<int>(count));
            size_t skb = (static_cast<size_t>(ex) + count) * sizeof(S);
            skb = (skb + 7) & ~size_t{7};
            return reinterpret_cast<VST*>(reinterpret_cast<char*>(b + 1) + skb);
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
            size_t skb = (static_cast<size_t>(ex) + count) * sizeof(S);
            skb = (skb + 7) & ~size_t{7};
            return reinterpret_cast<const VST*>(reinterpret_cast<const char*>(b + 1) + skb);
        }
    }

    // --- bot-internal layout ---

    static Bitmap256& bot_bitmap_(uint64_t* b) noexcept {
        return *reinterpret_cast<Bitmap256*>(b);
    }
    static const Bitmap256& bot_bitmap_(const uint64_t* b) noexcept {
        return *reinterpret_cast<const Bitmap256*>(b);
    }
    // Full children (includes sentinel at [0])
    static uint64_t* bot_int_children_(uint64_t* b) noexcept {
        return b + BITMAP256_U64;
    }
    static const uint64_t* bot_int_children_(const uint64_t* b) noexcept {
        return b + BITMAP256_U64;
    }
    // Real children (skips sentinel)
    static uint64_t* bot_int_real_children_(uint64_t* b) noexcept {
        return b + BITMAP256_U64 + 1;
    }
    static const uint64_t* bot_int_real_children_(const uint64_t* b) noexcept {
        return b + BITMAP256_U64 + 1;
    }

    // ==================================================================
    // Bot-leaf insert/erase internals
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
        Bitmap256& nbm = bot_leaf_bm_(nb);
        nbm = bm; nbm.set_bit(suffix);
        VST* nvd = bot_leaf_vals_<16>(nb, nc);
        int isl = nbm.count_below(suffix);
        std::memcpy(nvd, vd, isl * sizeof(VST));
        VT::write_slot(&nvd[isl], value);
        std::memcpy(nvd + isl + 1, vd + isl, (count - isl) * sizeof(VST));

        dealloc_node(alloc, bot, bot_leaf_size_u64<16>(count));
        return {nb, true, false};
    }

    template<int BITS>
    static BotLeafInsertResult insert_bl_list_(
            uint64_t* bot, uint64_t ik, VST value, ALLOC& alloc) {
        static_assert(BITS > 16);
        constexpr int sb = BITS - 8;
        using S = typename suffix_traits<sb>::type;
        S suffix = static_cast<S>(KOps::template extract_suffix<sb>(ik));
        uint32_t count = bot_leaf_count<BITS>(bot);
        S*   kd = bot_leaf_keys_data_<BITS>(bot, count);
        VST* vd = bot_leaf_vals_<BITS>(bot, count);

        int idx = binary_search_for_insert(kd, static_cast<size_t>(count), suffix);
        if (idx >= 0) {
            VT::destroy(vd[idx], alloc);
            VT::write_slot(&vd[idx], value);
            return {bot, false, false};
        }
        size_t ins = static_cast<size_t>(-(idx + 1));
        if (count >= BOT_LEAF_MAX) return {bot, false, true};

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
    // Bot-leaf erase internals
    // ==================================================================

    static EraseResult erase_bl_16_(uint64_t* bot, uint64_t ik, ALLOC& alloc) {
        uint8_t suffix = static_cast<uint8_t>(KOps::template extract_suffix<8>(ik));
        Bitmap256& bm = bot_leaf_bm_(bot);
        if (!bm.has_bit(suffix)) return {bot, false};
        uint32_t count = bm.popcount();
        int slot = bm.count_below(suffix);
        VT::destroy(bot_leaf_vals_<16>(bot, count)[slot], alloc);

        uint32_t nc = count - 1;
        if (nc == 0) {
            dealloc_node(alloc, bot, bot_leaf_size_u64<16>(count));
            return {nullptr, true};
        }
        uint64_t* nb = alloc_node(alloc, bot_leaf_size_u64<16>(nc));
        Bitmap256& nbm = bot_leaf_bm_(nb);
        nbm = bm; nbm.clear_bit(suffix);
        const VST* ov = bot_leaf_vals_<16>(bot, count);
        VST*       nv = bot_leaf_vals_<16>(nb, nc);
        std::memcpy(nv, ov, slot * sizeof(VST));
        std::memcpy(nv + slot, ov + slot + 1, (nc - slot) * sizeof(VST));

        dealloc_node(alloc, bot, bot_leaf_size_u64<16>(count));
        return {nb, true};
    }

    template<int BITS>
    static EraseResult erase_bl_list_(uint64_t* bot, uint64_t ik, ALLOC& alloc) {
        static_assert(BITS > 16);
        constexpr int sb = BITS - 8;
        using S = typename suffix_traits<sb>::type;
        S suffix = static_cast<S>(KOps::template extract_suffix<sb>(ik));
        uint32_t count = bot_leaf_count<BITS>(bot);
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
        std::memcpy(nk, kd, idx * sizeof(S));
        std::memcpy(nv, vd, idx * sizeof(VST));
        std::memcpy(nk + idx, kd + idx + 1, (nc - idx) * sizeof(S));
        std::memcpy(nv + idx, vd + idx + 1, (nc - idx) * sizeof(VST));
        KnSearch<S>::build(bot_leaf_search_start_<BITS>(nb), nk, static_cast<int>(nc));

        dealloc_node(alloc, bot, bot_leaf_size_u64<BITS>(count));
        return {nb, true};
    }
};

} // namespace kn3

#endif // KNTRIE_BITMASK_HPP
