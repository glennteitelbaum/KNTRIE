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

    // Returns true if bit is set; writes dense slot index into `slot`.
    bool find_slot(uint8_t index, int& slot) const noexcept {
        const int w = index >> 6;
        const int b = index & 63;
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

    // Slot where a NEW bit would be inserted (count of set bits below `index`).
    int slot_for_insert(uint8_t index) const noexcept {
        const int w = index >> 6;
        const int b = index & 63;
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
// BitmaskOps  – split-top / bot-leaf / bot-internal layout + operations
// ==========================================================================

template<typename KEY, typename VALUE, typename ALLOC>
struct BitmaskOps {
    using KOps = KeyOps<KEY>;
    using VT   = ValueTraits<VALUE, ALLOC>;
    using VST  = typename VT::slot_type;

    // ======================================================================
    // Size calculations
    // ======================================================================

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
            return 1 + (search_bytes) / 8 + vb / 8;
        }
    }

    static constexpr size_t bot_internal_size_u64(size_t count) noexcept {
        return BITMAP256_U64 + count;
    }

    // ======================================================================
    // Split-top accessors
    // ======================================================================

    static Bitmap256&       top_bitmap(uint64_t* n)       noexcept {
        return *reinterpret_cast<Bitmap256*>(n + header_u64(get_header(n)->skip));
    }
    static const Bitmap256& top_bitmap(const uint64_t* n) noexcept {
        return *reinterpret_cast<const Bitmap256*>(n + header_u64(get_header(n)->skip));
    }

    static Bitmap256&       bot_is_leaf_bitmap(uint64_t* n)       noexcept {
        return *reinterpret_cast<Bitmap256*>(n + header_u64(get_header(n)->skip) + BITMAP256_U64);
    }
    static const Bitmap256& bot_is_leaf_bitmap(const uint64_t* n) noexcept {
        return *reinterpret_cast<const Bitmap256*>(n + header_u64(get_header(n)->skip) + BITMAP256_U64);
    }

    template<int BITS>
    static uint64_t* top_children(uint64_t* n) noexcept {
        size_t off = header_u64(get_header(n)->skip) + BITMAP256_U64;
        if constexpr (BITS != 16) off += BITMAP256_U64;
        return n + off;
    }
    template<int BITS>
    static const uint64_t* top_children(const uint64_t* n) noexcept {
        size_t off = header_u64(get_header(n)->skip) + BITMAP256_U64;
        if constexpr (BITS != 16) off += BITMAP256_U64;
        return n + off;
    }

    // ======================================================================
    // Bot-leaf accessors
    // ======================================================================

    // --- BITS == 16: bitmap-based ---

    template<int BITS>
    static Bitmap256& bot_leaf_bitmap(uint64_t* b) noexcept {
        static_assert(BITS == 16);
        return *reinterpret_cast<Bitmap256*>(b);
    }
    template<int BITS>
    static const Bitmap256& bot_leaf_bitmap(const uint64_t* b) noexcept {
        static_assert(BITS == 16);
        return *reinterpret_cast<const Bitmap256*>(b);
    }

    template<int BITS>
    static uint32_t bot_leaf_count(const uint64_t* b) noexcept {
        if constexpr (BITS == 16) return bot_leaf_bitmap<16>(b).popcount();
        else                      return *reinterpret_cast<const uint32_t*>(b);
    }

    template<int BITS>
    static void set_bot_leaf_count(uint64_t* b, uint32_t c) noexcept {
        static_assert(BITS != 16);
        *reinterpret_cast<uint32_t*>(b) = c;
    }

    // --- BITS > 16: list-based  [count:u32 pad][search overlay][values] ---

    template<int BITS>
    static auto bot_leaf_search_start(uint64_t* b) noexcept {
        static_assert(BITS > 16);
        constexpr int sb = BITS - 8;
        using S = typename suffix_traits<sb>::type;
        return reinterpret_cast<S*>(b + 1);
    }
    template<int BITS>
    static auto bot_leaf_search_start(const uint64_t* b) noexcept {
        static_assert(BITS > 16);
        constexpr int sb = BITS - 8;
        using S = typename suffix_traits<sb>::type;
        return reinterpret_cast<const S*>(b + 1);
    }

    template<int BITS>
    static auto bot_leaf_keys_data(uint64_t* b, size_t count) noexcept {
        static_assert(BITS > 16);
        constexpr int sb = BITS - 8;
        using S = typename suffix_traits<sb>::type;
        return KnSearch<S>::keys_ptr(bot_leaf_search_start<BITS>(b), static_cast<int>(count));
    }
    template<int BITS>
    static auto bot_leaf_keys_data(const uint64_t* b, size_t count) noexcept {
        static_assert(BITS > 16);
        constexpr int sb = BITS - 8;
        using S = typename suffix_traits<sb>::type;
        return KnSearch<S>::keys_ptr(bot_leaf_search_start<BITS>(b), static_cast<int>(count));
    }

    template<int BITS>
    static VST* bot_leaf_values(uint64_t* b, [[maybe_unused]] size_t count) noexcept {
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
    static const VST* bot_leaf_values(const uint64_t* b, [[maybe_unused]] size_t count) noexcept {
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

    // ======================================================================
    // Bot-internal accessors
    // ======================================================================

    static Bitmap256&       bot_bitmap(uint64_t* b)       noexcept { return *reinterpret_cast<Bitmap256*>(b); }
    static const Bitmap256& bot_bitmap(const uint64_t* b) noexcept { return *reinterpret_cast<const Bitmap256*>(b); }

    static uint64_t*       bot_internal_children(uint64_t* b)       noexcept { return b + BITMAP256_U64; }
    static const uint64_t* bot_internal_children(const uint64_t* b) noexcept { return b + BITMAP256_U64; }

    // ======================================================================
    // Find in bot-leaf
    // ======================================================================

    template<int BITS>
    static const VALUE* find_in_bot_leaf(const uint64_t* bot, uint64_t ik) noexcept {
        if constexpr (BITS == 16) {
            uint8_t suffix = static_cast<uint8_t>(KOps::template extract_suffix<8>(ik));
            const Bitmap256& bm = bot_leaf_bitmap<16>(bot);
            if (!bm.has_bit(suffix)) return nullptr;
            int slot = bm.count_below(suffix);
            const VST* val = bot_leaf_values<16>(bot, 0);
            return VT::as_ptr(val[slot]);
        } else {
            uint32_t count = bot_leaf_count<BITS>(bot);
            constexpr int sb = BITS - 8;
            using S = typename suffix_traits<sb>::type;
            S suffix = static_cast<S>(KOps::template extract_suffix<sb>(ik));
            const S*   ss  = bot_leaf_search_start<BITS>(bot);
            const VST* val = bot_leaf_values<BITS>(bot, count);
            int idx = KnSearch<S>::search(ss, static_cast<int>(count), suffix);
            if (idx < 0) return nullptr;
            return VT::as_ptr(val[idx]);
        }
    }

    // Find through a split-leaf at BITS==16  (top bitmap → bot bitmap leaf)
    static const VALUE* find_in_split_leaf_16(const uint64_t* node, uint64_t ik) noexcept {
        uint8_t top_idx = KOps::template extract_top8<16>(ik);
        const Bitmap256& tbm = top_bitmap(node);
        int ts;
        if (!tbm.find_slot(top_idx, ts)) return nullptr;
        const uint64_t* bot = reinterpret_cast<const uint64_t*>(top_children<16>(node)[ts]);
        return find_in_bot_leaf<16>(bot, ik);
    }

    // Find through a split-leaf at BITS>16  (all bottoms are leaves)
    template<int BITS>
    static const VALUE* find_in_split_leaf(const uint64_t* node, uint64_t ik) noexcept {
        uint8_t top_idx = KOps::template extract_top8<BITS>(ik);
        const Bitmap256& tbm = top_bitmap(node);
        int ts;
        if (!tbm.find_slot(top_idx, ts)) return nullptr;
        const uint64_t* bot = reinterpret_cast<const uint64_t*>(top_children<BITS>(node)[ts]);
        return find_in_bot_leaf<BITS>(bot, ik);
    }

    // ======================================================================
    // Insert into bot-leaf  (leaf-level only; returns needs_convert on overflow)
    // ======================================================================

    struct BotLeafInsertResult {
        bool inserted;
        bool needs_convert;   // overflow → caller converts to bot_internal
    };

    template<int BITS>
    static BotLeafInsertResult insert_into_bot_leaf(
            uint64_t* node, NodeHeader* h, uint8_t top_idx, int top_slot,
            uint64_t* bot, uint64_t ik, VST value, ALLOC& alloc) {
        if constexpr (BITS == 16) {
            return insert_bot_leaf_16(node, h, top_slot, bot, ik, value, alloc);
        } else {
            return insert_bot_leaf_list<BITS>(node, h, top_idx, top_slot, bot, ik, value, alloc);
        }
    }

    // ======================================================================
    // Add a brand-new single-entry bottom leaf  (no top-node realloc here;
    // the caller builds the new top node and places the returned bot ptr.)
    // ======================================================================

    template<int BITS>
    static uint64_t* make_single_bot_leaf(uint64_t ik, VST value, ALLOC& alloc) {
        uint64_t* bot = alloc_node(alloc, bot_leaf_size_u64<BITS>(1));
        if constexpr (BITS == 16) {
            Bitmap256& bm = bot_leaf_bitmap<16>(bot);
            bm = Bitmap256{};
            uint8_t suffix = static_cast<uint8_t>(KOps::template extract_suffix<8>(ik));
            bm.set_bit(suffix);
            VT::write_slot(&bot_leaf_values<16>(bot, 1)[0], value);
        } else {
            constexpr int sb = BITS - 8;
            using S = typename suffix_traits<sb>::type;
            set_bot_leaf_count<BITS>(bot, 1);
            auto* kd = bot_leaf_keys_data<BITS>(bot, 1);
            kd[0] = static_cast<S>(KOps::template extract_suffix<sb>(ik));
            VT::write_slot(&bot_leaf_values<BITS>(bot, 1)[0], value);
            KnSearch<S>::build(bot_leaf_search_start<BITS>(bot), kd, 1);
        }
        return bot;
    }

    // ======================================================================
    // Erase from bot-leaf  (reallocates smaller, or returns nullptr if empty)
    // ======================================================================

    template<int BITS>
    static EraseResult erase_from_bot_leaf(uint64_t* bot, uint64_t ik, ALLOC& alloc) {
        if constexpr (BITS == 16)
            return erase_bot_leaf_16(bot, ik, alloc);
        else
            return erase_bot_leaf_list<BITS>(bot, ik, alloc);
    }

private:

    // --- BITS==16 bitmap-based erase ---

    static EraseResult erase_bot_leaf_16(uint64_t* bot, uint64_t ik, ALLOC& alloc) {
        uint8_t suffix = static_cast<uint8_t>(KOps::template extract_suffix<8>(ik));
        Bitmap256& bm = bot_leaf_bitmap<16>(bot);

        if (!bm.has_bit(suffix)) return {bot, false};

        uint32_t count = bm.popcount();
        int slot = bm.count_below(suffix);
        VST* vd = bot_leaf_values<16>(bot, count);

        VT::destroy(vd[slot], alloc);

        uint32_t nc = count - 1;
        if (nc == 0) {
            dealloc_node(alloc, bot, bot_leaf_size_u64<16>(count));
            return {nullptr, true};
        }

        // Reallocate with one fewer value
        uint64_t* nb = alloc_node(alloc, bot_leaf_size_u64<16>(nc));
        Bitmap256& nbm = bot_leaf_bitmap<16>(nb);
        nbm = bm;
        nbm.clear_bit(suffix);
        VST* nv = bot_leaf_values<16>(nb, nc);

        // Copy values, skipping the erased slot
        std::memcpy(nv,        vd,          slot * sizeof(VST));
        std::memcpy(nv + slot, vd + slot + 1, (nc - slot) * sizeof(VST));

        dealloc_node(alloc, bot, bot_leaf_size_u64<16>(count));
        return {nb, true};
    }

    // --- BITS>16 list-based erase ---

    template<int BITS>
    static EraseResult erase_bot_leaf_list(uint64_t* bot, uint64_t ik, ALLOC& alloc) {
        constexpr int sb = BITS - 8;
        using S = typename suffix_traits<sb>::type;

        uint32_t count = bot_leaf_count<BITS>(bot);
        S suffix = static_cast<S>(KOps::template extract_suffix<sb>(ik));
        S*   kd = bot_leaf_keys_data<BITS>(bot, count);
        VST* vd = bot_leaf_values<BITS>(bot, count);

        int idx = binary_search_for_insert(kd, static_cast<size_t>(count), suffix);
        if (idx < 0) return {bot, false};

        VT::destroy(vd[idx], alloc);

        uint32_t nc = count - 1;
        if (nc == 0) {
            dealloc_node(alloc, bot, bot_leaf_size_u64<BITS>(count));
            return {nullptr, true};
        }

        // Reallocate smaller
        uint64_t* nb = alloc_node(alloc, bot_leaf_size_u64<BITS>(nc));
        set_bot_leaf_count<BITS>(nb, nc);
        S*   nk = bot_leaf_keys_data<BITS>(nb, nc);
        VST* nv = bot_leaf_values<BITS>(nb, nc);

        size_t pos = static_cast<size_t>(idx);
        std::memcpy(nk,       kd,         pos * sizeof(S));
        std::memcpy(nv,       vd,         pos * sizeof(VST));
        std::memcpy(nk + pos, kd + pos + 1, (nc - pos) * sizeof(S));
        std::memcpy(nv + pos, vd + pos + 1, (nc - pos) * sizeof(VST));

        KnSearch<S>::build(bot_leaf_search_start<BITS>(nb), nk, static_cast<int>(nc));

        dealloc_node(alloc, bot, bot_leaf_size_u64<BITS>(count));
        return {nb, true};
    }

    // --- BITS==16 bitmap-based bot-leaf insert ---

    static BotLeafInsertResult insert_bot_leaf_16(
            uint64_t* node, NodeHeader* h, int top_slot,
            uint64_t* bot, uint64_t ik, VST value, ALLOC& alloc) {
        uint8_t suffix = static_cast<uint8_t>(KOps::template extract_suffix<8>(ik));
        Bitmap256& bm = bot_leaf_bitmap<16>(bot);
        uint32_t count = bm.popcount();
        VST* val = bot_leaf_values<16>(bot, count);

        if (bm.has_bit(suffix)) {
            int slot = bm.count_below(suffix);
            VT::destroy(val[slot], alloc);
            VT::write_slot(&val[slot], value);
            return {false, false};
        }

        uint32_t nc = count + 1;
        uint64_t* nb = alloc_node(alloc, bot_leaf_size_u64<16>(nc));
        Bitmap256& nbm = bot_leaf_bitmap<16>(nb);
        nbm = bm;
        nbm.set_bit(suffix);
        VST* nv = bot_leaf_values<16>(nb, nc);
        int is = nbm.count_below(suffix);
        std::memcpy(nv, val, is * sizeof(VST));
        VT::write_slot(&nv[is], value);
        std::memcpy(nv + is + 1, val + is, (count - is) * sizeof(VST));

        top_children<16>(node)[top_slot] = reinterpret_cast<uint64_t>(nb);
        h->count++;
        dealloc_node(alloc, bot, bot_leaf_size_u64<16>(count));
        return {true, false};
    }

    // --- BITS>16 list-based bot-leaf insert ---

    template<int BITS>
    static BotLeafInsertResult insert_bot_leaf_list(
            uint64_t* node, NodeHeader* h, uint8_t top_idx, int top_slot,
            uint64_t* bot, uint64_t ik, VST value, ALLOC& alloc) {
        constexpr int sb = BITS - 8;
        using S = typename suffix_traits<sb>::type;

        uint32_t count = bot_leaf_count<BITS>(bot);
        S suffix = static_cast<S>(KOps::template extract_suffix<sb>(ik));
        S*   kd = bot_leaf_keys_data<BITS>(bot, count);
        VST* vd = bot_leaf_values<BITS>(bot, count);

        int idx = binary_search_for_insert(kd, static_cast<size_t>(count), suffix);
        if (idx >= 0) {
            VT::destroy(vd[idx], alloc);
            VT::write_slot(&vd[idx], value);
            return {false, false};
        }
        size_t ins = static_cast<size_t>(-(idx + 1));

        if (count >= BOT_LEAF_MAX)
            return {false, true};   // needs convert

        uint32_t nc = count + 1;
        uint64_t* nb = alloc_node(alloc, bot_leaf_size_u64<BITS>(nc));
        set_bot_leaf_count<BITS>(nb, nc);
        S*   nk = bot_leaf_keys_data<BITS>(nb, nc);
        VST* nv = bot_leaf_values<BITS>(nb, nc);

        std::memcpy(nk, kd, ins * sizeof(S));
        std::memcpy(nv, vd, ins * sizeof(VST));
        nk[ins] = suffix;
        VT::write_slot(&nv[ins], value);
        std::memcpy(nk + ins + 1, kd + ins, (count - ins) * sizeof(S));
        std::memcpy(nv + ins + 1, vd + ins, (count - ins) * sizeof(VST));

        KnSearch<S>::build(bot_leaf_search_start<BITS>(nb), nk, static_cast<int>(nc));

        top_children<BITS>(node)[top_slot] = reinterpret_cast<uint64_t>(nb);
        h->count++;
        dealloc_node(alloc, bot, bot_leaf_size_u64<BITS>(count));
        return {true, false};
    }
};

} // namespace kn3

#endif // KNTRIE_BITMASK_HPP
