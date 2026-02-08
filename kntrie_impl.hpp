#ifndef KNTRIE_IMPL_HPP
#define KNTRIE_IMPL_HPP

#include "kntrie_compact.hpp"
#include "kntrie_bitmask.hpp"

namespace kn3 {

template<typename KEY, typename VALUE, typename ALLOC = std::allocator<uint64_t>>
class kntrie_impl {
    static_assert(std::is_integral_v<KEY>, "KEY must be integral");

public:
    using key_type   = KEY;
    using mapped_type = VALUE;
    using size_type  = std::size_t;

private:
    using KO  = KeyOps<KEY>;
    using VT  = ValueTraits<VALUE, ALLOC>;
    using VST = typename VT::slot_type;
    using CO  = CompactOps<KEY, VALUE, ALLOC>;
    using BO  = BitmaskOps<KEY, VALUE, ALLOC>;

    static constexpr int KEY_BITS = static_cast<int>(KO::key_bits);

    uint64_t* root_;
    size_t    size_;
    [[no_unique_address]] ALLOC alloc_;

public:
    // ==================================================================
    // Construction / destruction
    // ==================================================================

    kntrie_impl() : size_(0), alloc_() {
        root_ = alloc_node(alloc_, CO::template size_u64<KEY_BITS>(0, 0));
        auto* h = get_header(root_);
        h->count = 0; h->skip = 0; h->set_leaf(true);
    }

    ~kntrie_impl() { remove_all(); }

    kntrie_impl(const kntrie_impl&) = delete;
    kntrie_impl& operator=(const kntrie_impl&) = delete;

    [[nodiscard]] bool      empty() const noexcept { return size_ == 0; }
    [[nodiscard]] size_type size()  const noexcept { return size_; }

    void clear() noexcept {
        remove_all();
        root_ = alloc_node(alloc_, CO::template size_u64<KEY_BITS>(0, 0));
        auto* h = get_header(root_);
        h->count = 0; h->skip = 0;
        h->set_leaf(true); h->set_split(false);
        size_ = 0;
    }

    // ==================================================================
    // Find
    // ==================================================================

    const VALUE* find_value(const KEY& key) const noexcept {
        uint64_t ik = KO::to_internal(key);
        NodeHeader h = *get_header(root_);
        return find_impl<KEY_BITS>(root_, ik, h, 0, 0);
    }

    bool contains(const KEY& key) const noexcept { return find_value(key) != nullptr; }

    // ==================================================================
    // Insert
    // ==================================================================

    // Returns {success, newly_inserted}.
    std::pair<bool,bool> insert(const KEY& key, const VALUE& value) {
        uint64_t ik = KO::to_internal(key);
        VST sv = VT::store(value, alloc_);
        auto [nr, ins] = insert_impl<KEY_BITS>(root_, ik, sv);
        root_ = nr;
        if (ins) { ++size_; return {true,true}; }
        VT::destroy(sv, alloc_);
        return {true,false};
    }

    // ==================================================================
    // Erase
    // ==================================================================

    // Returns true if the key was found and removed.
    bool erase(const KEY& key) {
        uint64_t ik = KO::to_internal(key);
        auto [new_root, erased] = erase_impl<KEY_BITS>(root_, ik);
        if (!erased) return false;
        --size_;
        if (new_root) {
            root_ = new_root;
        } else {
            // Root was removed (last entry) — create fresh empty root
            root_ = alloc_node(alloc_, CO::template size_u64<KEY_BITS>(0, 0));
            auto* h = get_header(root_);
            h->count = 0; h->skip = 0; h->set_leaf(true);
        }
        return true;
    }

    // ==================================================================
    // Memory stats
    // ==================================================================

    struct DebugStats {
        struct Level {
            size_t compact_leaf = 0, compact_leaf_compressed = 0;
            size_t split_nodes = 0, split_nodes_compressed = 0;
            size_t bot_leaf = 0, bot_internal = 0;
            size_t entries = 0, nodes = 0, bytes = 0;
            size_t compact_hist[4098] = {};  // [i] = # compact leaves with count==i
            size_t bot_leaf_hist[4098] = {};  // [i] = # bot leaves with count==i
        };
        Level  levels[4];
        size_t total_nodes = 0, total_bytes = 0, total_entries = 0;
    };

    DebugStats debug_stats() const noexcept {
        DebugStats s{};
        collect_stats<KEY_BITS>(root_, s);
        for (int i = 0; i < 4; ++i) {
            s.total_nodes += s.levels[i].nodes;
            s.total_bytes += s.levels[i].bytes;
            s.total_entries += s.levels[i].entries;
        }
        return s;
    }

    size_t memory_usage() const noexcept { return debug_stats().total_bytes; }

    // ==================================================================
    // Debug helpers (kept for benchmark compatibility)
    // ==================================================================

    struct RootInfo {
        uint32_t count; uint16_t top_count; uint8_t skip;
        bool is_leaf, is_split; uint64_t prefix;
    };
    RootInfo debug_root_info() const {
        auto* h = get_header(root_);
        return {h->count, h->top_count, h->skip,
                h->is_leaf(), h->is_split(),
                h->skip > 0 ? get_prefix(root_) : 0};
    }

private:
    // ==================================================================
    // Find – recursive dispatch
    // ==================================================================

    template<int BITS>
    const VALUE* find_impl(const uint64_t* node, uint64_t ik, NodeHeader h,
                           uint64_t skip_pf, int skip_left) const noexcept
        requires (BITS == 16)
    {
        if (!h.is_split())
            return CO::template find<16>(node, &h, ik);
        return BO::find_in_split_leaf_16(node, ik);
    }

    template<int BITS>
    const VALUE* find_impl(const uint64_t* node, uint64_t ik, NodeHeader h,
                           uint64_t skip_pf, int skip_left) const noexcept
        requires (BITS > 16)
    {
        uint16_t key_chunk = KO::template extract_top16<BITS>(ik);

        if (skip_left > 0) {
            if (key_chunk != KO::get_skip_chunk(skip_pf, h.skip, skip_left))
                return nullptr;
            skip_left--;
        } else if (h.skip >= 1) {
            uint64_t np = get_prefix(node);
            if (key_chunk != KO::get_skip_chunk(np, h.skip, h.skip))
                return nullptr;
            if (h.skip > 1) { skip_pf = np; skip_left = h.skip - 1; }
            h.skip = 0;
        } else if (h.is_leaf()) {
            if (!h.is_split())
                return CO::template find<BITS>(node, &h, ik);
            return find_in_split<BITS>(node, ik);
        } else {
            // internal → descend
            const uint64_t* child = get_child<BITS>(node, ik);
            if (!child) return nullptr;
            node = child; h = *get_header(child);
        }
        return find_impl<BITS - 16>(node, ik, h, skip_pf, skip_left);
    }

    // Walk a split that may contain both leaf and internal bottoms
    template<int BITS>
    const VALUE* find_in_split(const uint64_t* node, uint64_t ik) const noexcept
        requires (BITS > 16)
    {
        uint8_t ti = KO::template extract_top8<BITS>(ik);
        const Bitmap256& tbm = BO::top_bitmap(node);
        int ts; if (!tbm.find_slot(ti, ts)) return nullptr;
        const uint64_t* bot = reinterpret_cast<const uint64_t*>(
            BO::template top_children<BITS>(node)[ts]);

        if (BO::bot_is_leaf_bitmap(node).has_bit(ti))
            return BO::template find_in_bot_leaf<BITS>(bot, ik);

        // bot_internal → descend
        uint8_t bi = KO::template extract_top8<BITS - 8>(ik);
        const Bitmap256& bbm = BO::bot_bitmap(bot);
        int bs; if (!bbm.find_slot(bi, bs)) return nullptr;
        const uint64_t* child = reinterpret_cast<const uint64_t*>(
            BO::bot_internal_children(bot)[bs]);
        NodeHeader ch = *get_header(child);
        return find_impl<BITS - 16>(child, ik, ch, 0, 0);
    }

    // Get child through a split-internal node
    template<int BITS>
    const uint64_t* get_child(const uint64_t* node, uint64_t ik) const noexcept {
        uint8_t ti = KO::template extract_top8<BITS>(ik);
        const Bitmap256& tbm = BO::top_bitmap(node);
        int ts; if (!tbm.find_slot(ti, ts)) return nullptr;
        const uint64_t* bot = reinterpret_cast<const uint64_t*>(
            BO::template top_children<BITS>(node)[ts]);
        if (BO::bot_is_leaf_bitmap(node).has_bit(ti)) return nullptr;
        uint8_t bi = KO::template extract_top8<BITS - 8>(ik);
        const Bitmap256& bbm = BO::bot_bitmap(bot);
        int bs; if (!bbm.find_slot(bi, bs)) return nullptr;
        return reinterpret_cast<const uint64_t*>(BO::bot_internal_children(bot)[bs]);
    }

    // ==================================================================
    // Insert – recursive dispatch
    // ==================================================================

    template<int BITS>
    InsertResult insert_impl(uint64_t* node, uint64_t ik, VST value)
        requires (BITS <= 0)
    { return {node, false}; }

    template<int BITS>
    InsertResult insert_impl(uint64_t* node, uint64_t ik, VST value)
        requires (BITS > 0)
    {
        auto* h = get_header(node);
        if (h->skip > 0) [[unlikely]] {
            uint64_t exp = KO::template extract_prefix<BITS>(ik, h->skip);
            uint64_t act = get_prefix(node);
            if (exp != act) return split_on_prefix<BITS>(node, h, ik, value, exp);
            int ab = BITS - h->skip * 16;
            if (ab == 48) return insert_at_bits<48>(node, h, ik, value);
            if (ab == 32) return insert_at_bits<32>(node, h, ik, value);
            if (ab == 16) return insert_at_bits<16>(node, h, ik, value);
        }
        return insert_at_bits<BITS>(node, h, ik, value);
    }

    template<int BITS>
    InsertResult insert_at_bits(uint64_t* node, NodeHeader* h, uint64_t ik, VST value)
        requires (BITS <= 0)
    { return {node, false}; }

    template<int BITS>
    InsertResult insert_at_bits(uint64_t* node, NodeHeader* h, uint64_t ik, VST value)
        requires (BITS > 0)
    {
        if (h->is_leaf() && !h->is_split()) {
            auto r = CO::template insert<BITS>(node, h, ik, value, alloc_);
            if (r.needs_split) return convert_to_split<BITS>(node, h, ik, value);
            return {r.node, r.inserted};
        }
        if (h->is_split())
            return insert_into_split<BITS>(node, h, ik, value);
        return {node, false};
    }

    // ==================================================================
    // Insert into split node
    // ==================================================================

    template<int BITS>
    InsertResult insert_into_split(uint64_t* node, NodeHeader* h,
                                   uint64_t ik, VST value)
        requires (BITS > 0)
    {
        uint8_t ti = KO::template extract_top8<BITS>(ik);
        Bitmap256& tbm = BO::top_bitmap(node);
        int ts;
        bool exists = tbm.find_slot(ti, ts);

        if (!exists)
            return add_new_bottom_leaf<BITS>(node, h, ik, value, ti);

        bool is_leaf;
        if constexpr (BITS == 16) is_leaf = true;
        else is_leaf = BO::bot_is_leaf_bitmap(node).has_bit(ti);

        uint64_t* bot = reinterpret_cast<uint64_t*>(
            BO::template top_children<BITS>(node)[ts]);

        if (is_leaf) {
            auto r = BO::template insert_into_bot_leaf<BITS>(
                node, h, ti, ts, bot, ik, value, alloc_);
            if (r.needs_convert) {
                if constexpr (BITS > 16) {
                    uint32_t bc = BO::template bot_leaf_count<BITS>(bot);
                    return convert_bot_leaf_to_internal<BITS>(
                        node, h, ti, ts, bot, bc, ik, value);
                }
            }
            return {node, r.inserted};
        }
        if constexpr (BITS > 16)
            return insert_into_bot_internal<BITS>(node, h, ti, ts, bot, ik, value);
        return {node, false};
    }

    // ==================================================================
    // Add new top-level entry (bottom leaf with 1 element)
    // ==================================================================

    template<int BITS>
    InsertResult add_new_bottom_leaf(uint64_t* node, NodeHeader* h,
                                    uint64_t ik, VST value, uint8_t ti)
        requires (BITS > 0)
    {
        Bitmap256& tbm = BO::top_bitmap(node);
        size_t otc = h->top_count, ntc = otc + 1;
        int isl = tbm.slot_for_insert(ti);

        uint64_t* nn = alloc_node(alloc_, BO::template split_top_size_u64<BITS>(ntc, h->skip));
        auto* nh = get_header(nn); *nh = *h;
        nh->count = h->count + 1;
        nh->top_count = static_cast<uint16_t>(ntc);
        if (h->skip > 0) set_prefix(nn, get_prefix(node));

        Bitmap256& ntbm = BO::top_bitmap(nn);
        ntbm = tbm; ntbm.set_bit(ti);

        if constexpr (BITS > 16) {
            Bitmap256& nil = BO::bot_is_leaf_bitmap(nn);
            nil = BO::bot_is_leaf_bitmap(node);
            nil.set_bit(ti);
        }

        uint64_t* oc = BO::template top_children<BITS>(node);
        uint64_t* nc = BO::template top_children<BITS>(nn);
        for (int i = 0; i < isl; ++i) nc[i] = oc[i];
        for (size_t i = isl; i < otc; ++i) nc[i + 1] = oc[i];

        nc[isl] = reinterpret_cast<uint64_t>(
            BO::template make_single_bot_leaf<BITS>(ik, value, alloc_));

        dealloc_node(alloc_, node, BO::template split_top_size_u64<BITS>(otc, h->skip));
        return {nn, true};
    }

    // ==================================================================
    // Insert into bot_internal (recurse)
    // ==================================================================

    template<int BITS>
    InsertResult insert_into_bot_internal(uint64_t* node, NodeHeader* h,
            uint8_t ti, int ts, uint64_t* bot, uint64_t ik, VST value)
        requires (BITS > 16)
    {
        uint8_t bi = KO::template extract_top8<BITS - 8>(ik);
        Bitmap256& bbm = BO::bot_bitmap(bot);
        uint64_t* children = BO::bot_internal_children(bot);
        int bs;
        bool exists = bbm.find_slot(bi, bs);

        if (exists) {
            auto [nc, ins] = insert_impl<BITS - 16>(
                reinterpret_cast<uint64_t*>(children[bs]), ik, value);
            children[bs] = reinterpret_cast<uint64_t>(nc);
            if (ins) h->count++;
            return {node, ins};
        }

        // new child in bot_internal
        int bc = bbm.popcount();
        int isl = bbm.slot_for_insert(bi);
        uint64_t* nb = alloc_node(alloc_, BO::bot_internal_size_u64(bc + 1));
        Bitmap256& nbm = BO::bot_bitmap(nb);
        nbm = bbm; nbm.set_bit(bi);
        uint64_t* nch = BO::bot_internal_children(nb);
        for (int i = 0; i < isl; ++i) nch[i] = children[i];
        for (int i = isl; i < bc; ++i) nch[i + 1] = children[i];

        constexpr int CB = BITS - 16;
        uint64_t* child = alloc_node(alloc_, CO::template size_u64<CB>(1, 0));
        auto* ch = get_header(child);
        ch->count = 1; ch->skip = 0; ch->set_leaf(true);
        using CK = typename suffix_traits<CB>::type;
        auto* ckd = CO::template keys_data<CB>(child, 1);
        ckd[0] = static_cast<CK>(KO::template extract_suffix<CB>(ik));
        VT::write_slot(&CO::template values<CB>(child, 1)[0], value);
        KnSearch<CK>::build(CO::template search_start<CB>(child), ckd, 1);

        nch[isl] = reinterpret_cast<uint64_t>(child);
        BO::template top_children<BITS>(node)[ts] = reinterpret_cast<uint64_t>(nb);
        h->count++;
        dealloc_node(alloc_, bot, BO::bot_internal_size_u64(bc));
        return {node, true};
    }

    // ==================================================================
    // Convert compact leaf → split  (bridging)
    // ==================================================================

    template<int BITS>
    InsertResult convert_to_split(uint64_t* node, NodeHeader* h,
                                  uint64_t ik, VST value)
        requires (BITS > 0)
    {
        using K = typename suffix_traits<BITS>::type;
        K* ok = CO::template keys_data<BITS>(node, h->count);
        VST* ov = CO::template values<BITS>(node, h->count);
        K ns = static_cast<K>(KO::template extract_suffix<BITS>(ik));

        Bitmap256 tbm{};
        uint16_t bc[256] = {};
        for (uint32_t i = 0; i < h->count; ++i) {
            uint8_t ti = static_cast<uint8_t>(ok[i] >> (BITS - 8));
            tbm.set_bit(ti); bc[ti]++;
        }
        uint8_t nti = static_cast<uint8_t>(ns >> (BITS - 8));
        tbm.set_bit(nti); bc[nti]++;
        size_t tc = tbm.popcount();

        // --- prefix compression: if all in one 16-bit bucket, skip ---
        if constexpr (BITS > 16) {
            if (tc == 1) {
                Bitmap256 bbm{};
                constexpr int sb = BITS - 8;
                for (uint32_t i = 0; i < h->count; ++i) {
                    uint8_t bi = static_cast<uint8_t>(ok[i] >> (sb - 8));
                    bbm.set_bit(bi);
                }
                uint8_t nbi = static_cast<uint8_t>(ns >> (sb - 8));
                bbm.set_bit(nbi);

                if (bbm.popcount() == 1) {
                    uint16_t sp = (static_cast<uint16_t>(nti) << 8) | nbi;
                    constexpr int CB = BITS - 16;
                    constexpr uint64_t cm = (1ULL << CB) - 1;
                    size_t tot = h->count + 1;
                    std::unique_ptr<uint64_t[]> cs(new uint64_t[tot]);
                    std::unique_ptr<VST[]>      cv(new VST[tot]);
                    for (uint32_t i = 0; i < h->count; ++i) {
                        cs[i] = static_cast<uint64_t>(ok[i]) & cm;
                        cv[i] = ov[i];
                    }
                    cs[h->count] = static_cast<uint64_t>(ns) & cm;
                    cv[h->count] = value;

                    uint64_t cp = create_child_no_prefix<CB>(cs.get(), cv.get(), tot);
                    uint64_t* cn = reinterpret_cast<uint64_t*>(cp);
                    auto* ch2 = get_header(cn);
                    uint64_t ocp = ch2->skip > 0 ? get_prefix(cn) : 0;
                    uint8_t os = ch2->skip;
                    uint8_t nsk = h->skip + os + 1;
                    uint64_t pp = h->skip > 0 ? get_prefix(node) : 0;
                    uint64_t comb = (pp << 16) | sp;
                    comb = (comb << (16 * os)) | ocp;

                    if (os == 0 && nsk > 0) {
                        size_t oldsz = CO::template size_u64<CB>(tot, 0);
                        size_t newsz = CO::template size_u64<CB>(tot, nsk);
                        uint64_t* nc = alloc_node(alloc_, newsz);
                        *get_header(nc) = *ch2;
                        get_header(nc)->skip = nsk;
                        set_prefix(nc, comb);
                        using CK = typename suffix_traits<CB>::type;
                        auto* oss = CO::template search_start<CB>(cn);
                        auto* nss = CO::template search_start<CB>(nc);
                        int ex = KnSearch<CK>::extra(static_cast<int>(tot));
                        std::memcpy(nss, oss, (ex + tot) * sizeof(CK));
                        std::memcpy(CO::template values<CB>(nc, tot),
                                    CO::template values<CB>(cn, tot),
                                    tot * sizeof(VST));
                        dealloc_node(alloc_, cn, oldsz);
                        cn = nc;
                    } else {
                        ch2->skip = nsk;
                        if (nsk > 0) set_prefix(cn, comb);
                    }
                    dealloc_node(alloc_, node, CO::template size_u64<BITS>(h->count, h->skip));
                    return {cn, true};
                }
            }
        }

        // --- general split ---
        uint64_t* nn = alloc_node(alloc_, BO::template split_top_size_u64<BITS>(tc, h->skip));
        auto* nh = get_header(nn);
        nh->count = h->count + 1;
        nh->top_count = static_cast<uint16_t>(tc);
        nh->skip = h->skip;
        if (h->skip > 0) set_prefix(nn, get_prefix(node));
        nh->set_leaf(true); nh->set_split(true);

        BO::top_bitmap(nn) = tbm;
        if constexpr (BITS > 16) BO::bot_is_leaf_bitmap(nn) = tbm;

        uint64_t* nch = BO::template top_children<BITS>(nn);
        constexpr int sb = BITS - 8;
        using S = typename suffix_traits<sb>::type;
        constexpr uint64_t smask = (1ULL << sb) - 1;
        S nbs = static_cast<S>(ns & smask);

        int slot = 0;
        for (int ti = 0; ti < 256; ++ti) {
            if (!tbm.has_bit(ti)) continue;
            size_t cnt = bc[ti];
            uint64_t* bot = alloc_node(alloc_, BO::template bot_leaf_size_u64<BITS>(cnt));

            if constexpr (BITS == 16) {
                Bitmap256& bm = BO::template bot_leaf_bitmap<16>(bot);
                bm = Bitmap256{};
                VST* bv = BO::template bot_leaf_values<16>(bot, cnt);
                struct E { uint8_t s; VST v; };
                E es[256]; size_t ec = 0;
                for (uint32_t i = 0; i < h->count; ++i)
                    if ((ok[i] >> 8) == (uint64_t)ti) {
                        es[ec++] = {static_cast<uint8_t>(ok[i] & 0xFF), ov[i]};
                        bm.set_bit(static_cast<uint8_t>(ok[i] & 0xFF));
                    }
                if (nti == ti) {
                    uint8_t s8 = static_cast<uint8_t>(ns & 0xFF);
                    es[ec++] = {s8, value}; bm.set_bit(s8);
                }
                for (size_t i = 0; i < ec; ++i)
                    bv[bm.count_below(es[i].s)] = es[i].v;
            } else {
                BO::template set_bot_leaf_count<BITS>(bot, static_cast<uint32_t>(cnt));
                S* sd = BO::template bot_leaf_keys_data<BITS>(bot, cnt);
                VST* bv = BO::template bot_leaf_values<BITS>(bot, cnt);
                bool need_new = (nti == ti), done = false;
                size_t ci = 0;
                for (uint32_t i = 0; i < h->count; ++i) {
                    if ((ok[i] >> (BITS - 8)) != (uint64_t)ti) continue;
                    S os = static_cast<S>(ok[i] & smask);
                    if (need_new && !done && nbs < os) {
                        sd[ci] = nbs; bv[ci] = value; ci++; done = true;
                    }
                    sd[ci] = os; bv[ci] = ov[i]; ci++;
                }
                if (need_new && !done) { sd[ci] = nbs; bv[ci] = value; }
                KnSearch<S>::build(BO::template bot_leaf_search_start<BITS>(bot),
                                   sd, static_cast<int>(cnt));
            }
            nch[slot++] = reinterpret_cast<uint64_t>(bot);
        }
        dealloc_node(alloc_, node, CO::template size_u64<BITS>(h->count, h->skip));
        return {nn, true};
    }

    // ==================================================================
    // Create child (no prefix) – recursive, may produce compact or split
    // ==================================================================

    template<int CB>
    uint64_t create_child_no_prefix(uint64_t* suf, VST* vals, size_t count)
        requires (CB > 0)
    {
        if (count <= COMPACT_MAX) {
            uint64_t* c = alloc_node(alloc_, CO::template size_u64<CB>(count, 0));
            auto* ch = get_header(c);
            ch->count = static_cast<uint32_t>(count); ch->skip = 0; ch->set_leaf(true);
            using CK = typename suffix_traits<CB>::type;
            CK* ckd = CO::template keys_data<CB>(c, count);
            VST* cv = CO::template values<CB>(c, count);
            // insertion sort
            for (size_t i = 0; i < count; ++i) {
                CK k = static_cast<CK>(suf[i]); VST v = vals[i];
                size_t j = i;
                while (j > 0 && ckd[j-1] > k) {
                    ckd[j] = ckd[j-1]; cv[j] = cv[j-1]; j--;
                }
                ckd[j] = k; cv[j] = v;
            }
            KnSearch<CK>::build(CO::template search_start<CB>(c), ckd, static_cast<int>(count));
            return reinterpret_cast<uint64_t>(c);
        }

        // too many → build split
        Bitmap256 tbm{};
        uint16_t bc2[256] = {};
        for (size_t i = 0; i < count; ++i) {
            uint8_t ti = static_cast<uint8_t>(suf[i] >> (CB - 8));
            tbm.set_bit(ti); bc2[ti]++;
        }
        size_t tc = tbm.popcount();

        // prefix compression
        if constexpr (CB > 16) {
            if (tc == 1) {
                int st = tbm.find_next_set(0);
                Bitmap256 bb{}; constexpr int sb2 = CB - 8;
                for (size_t i = 0; i < count; ++i) {
                    uint8_t bi = static_cast<uint8_t>(suf[i] >> (sb2 - 8));
                    bb.set_bit(bi);
                }
                if (bb.popcount() == 1) {
                    int sbt = bb.find_next_set(0);
                    uint16_t sp = (static_cast<uint16_t>(st) << 8) | sbt;
                    constexpr int CB2 = CB - 16;
                    constexpr uint64_t cm2 = (1ULL << CB2) - 1;
                    for (size_t i = 0; i < count; ++i) suf[i] &= cm2;
                    uint64_t cp = create_child_no_prefix<CB2>(suf, vals, count);
                    uint64_t* cn = reinterpret_cast<uint64_t*>(cp);
                    auto* cnh = get_header(cn);
                    uint64_t ocp = cnh->skip > 0 ? get_prefix(cn) : 0;
                    uint8_t os = cnh->skip, nsk = os + 1;
                    uint64_t comb = (static_cast<uint64_t>(sp) << (16*os)) | ocp;
                    if (os == 0) {
                        size_t oldsz = CO::template size_u64<CB2>(count, 0);
                        size_t newsz = CO::template size_u64<CB2>(count, nsk);
                        uint64_t* nc = alloc_node(alloc_, newsz);
                        *get_header(nc) = *cnh;
                        get_header(nc)->skip = nsk; set_prefix(nc, comb);
                        using CK2 = typename suffix_traits<CB2>::type;
                        int ex2 = KnSearch<CK2>::extra(static_cast<int>(count));
                        std::memcpy(CO::template search_start<CB2>(nc),
                                    CO::template search_start<CB2>(cn),
                                    (ex2 + count) * sizeof(CK2));
                        std::memcpy(CO::template values<CB2>(nc, count),
                                    CO::template values<CB2>(cn, count),
                                    count * sizeof(VST));
                        dealloc_node(alloc_, cn, oldsz);
                        return reinterpret_cast<uint64_t>(nc);
                    }
                    cnh->skip = nsk; set_prefix(cn, comb);
                    return cp;
                }
            }
        }

        uint64_t* sn = alloc_node(alloc_, BO::template split_top_size_u64<CB>(tc, 0));
        auto* sh = get_header(sn);
        sh->count = static_cast<uint32_t>(count);
        sh->top_count = static_cast<uint16_t>(tc);
        sh->skip = 0; sh->set_split(true); sh->set_leaf(true);
        BO::top_bitmap(sn) = tbm;
        if constexpr (CB > 16) BO::bot_is_leaf_bitmap(sn) = tbm;

        uint64_t* tch = BO::template top_children<CB>(sn);
        constexpr int sb3 = CB - 8;
        constexpr uint64_t sm3 = (1ULL << sb3) - 1;

        int slot = 0;
        for (int bk = 0; bk < 256; ++bk) {
            if (!tbm.has_bit(bk)) continue;
            size_t bcount = bc2[bk];
            bool need_bi = false;
            if constexpr (CB > 16) need_bi = bcount > BOT_LEAF_MAX;

            if (need_bi) {
                if constexpr (CB > 16) {
                    Bitmap256 ibm{}; uint16_t ic[256] = {};
                    for (size_t i = 0; i < count; ++i)
                        if ((suf[i] >> (CB-8)) == (uint64_t)bk) {
                            uint8_t ii = static_cast<uint8_t>((suf[i] >> (sb3-8)) & 0xFF);
                            ibm.set_bit(ii); ic[ii]++;
                        }
                    size_t ibc = ibm.popcount();
                    uint64_t* bi2 = alloc_node(alloc_, BO::bot_internal_size_u64(ibc));
                    BO::bot_bitmap(bi2) = ibm;
                    uint64_t* bch = BO::bot_internal_children(bi2);
                    constexpr int CB3 = CB - 16;
                    constexpr uint64_t cm3 = (1ULL << CB3) - 1;
                    int is2 = 0;
                    for (int ib = 0; ib < 256; ++ib) {
                        if (!ibm.has_bit(ib)) continue;
                        size_t cc = ic[ib];
                        std::unique_ptr<uint64_t[]> cs2(new uint64_t[cc]);
                        std::unique_ptr<VST[]> cv2(new VST[cc]);
                        size_t ci2 = 0;
                        for (size_t i = 0; i < count; ++i)
                            if ((suf[i]>>(CB-8))==(uint64_t)bk &&
                                ((suf[i]>>(sb3-8))&0xFF)==(uint64_t)ib) {
                                cs2[ci2] = suf[i] & cm3; cv2[ci2] = vals[i]; ci2++;
                            }
                        bch[is2++] = create_child_no_prefix<CB3>(cs2.get(), cv2.get(), cc);
                    }
                    tch[slot++] = reinterpret_cast<uint64_t>(bi2);
                    BO::bot_is_leaf_bitmap(sn).clear_bit(bk);
                }
            } else {
                uint64_t* bot = alloc_node(alloc_, BO::template bot_leaf_size_u64<CB>(bcount));
                if constexpr (CB == 16) {
                    Bitmap256& bm = BO::template bot_leaf_bitmap<16>(bot);
                    bm = Bitmap256{};
                    VST* bv = BO::template bot_leaf_values<16>(bot, bcount);
                    struct E { uint8_t s; VST v; };
                    E es[256]; size_t ec = 0;
                    for (size_t i = 0; i < count; ++i)
                        if ((suf[i]>>8)==(uint64_t)bk) {
                            uint8_t s8 = static_cast<uint8_t>(suf[i]&0xFF);
                            es[ec++] = {s8, vals[i]}; bm.set_bit(s8);
                        }
                    for (size_t i = 0; i < ec; ++i)
                        bv[bm.count_below(es[i].s)] = es[i].v;
                } else {
                    using S3 = typename suffix_traits<sb3>::type;
                    BO::template set_bot_leaf_count<CB>(bot, static_cast<uint32_t>(bcount));
                    S3* sd = BO::template bot_leaf_keys_data<CB>(bot, bcount);
                    VST* bv = BO::template bot_leaf_values<CB>(bot, bcount);
                    size_t bi3 = 0;
                    for (size_t i = 0; i < count; ++i) {
                        if ((suf[i]>>(CB-8)) != (uint64_t)bk) continue;
                        S3 s3 = static_cast<S3>(suf[i] & sm3); VST v3 = vals[i];
                        size_t j = bi3;
                        while (j > 0 && sd[j-1] > s3) {
                            sd[j] = sd[j-1]; bv[j] = bv[j-1]; j--;
                        }
                        sd[j] = s3; bv[j] = v3; bi3++;
                    }
                    KnSearch<S3>::build(BO::template bot_leaf_search_start<CB>(bot),
                                        sd, static_cast<int>(bcount));
                }
                tch[slot++] = reinterpret_cast<uint64_t>(bot);
            }
        }

        if constexpr (CB > 16) {
            const Bitmap256& ilbm = BO::bot_is_leaf_bitmap(sn);
            bool any = false;
            for (int i = tbm.find_next_set(0); i >= 0; i = tbm.find_next_set(i+1))
                if (ilbm.has_bit(i)) { any = true; break; }
            if (!any) sh->set_leaf(false);
        }
        return reinterpret_cast<uint64_t>(sn);
    }

    // ==================================================================
    // Convert bot-leaf → bot-internal  (bridging)
    // ==================================================================

    template<int BITS>
    InsertResult convert_bot_leaf_to_internal(
            uint64_t* node, NodeHeader* h, uint8_t ti, int ts,
            uint64_t* bot, uint32_t count, uint64_t ik, VST value)
        requires (BITS > 16)
    {
        constexpr int sb = BITS - 8;
        using S = typename suffix_traits<sb>::type;
        S*   os2 = BO::template bot_leaf_keys_data<BITS>(bot, count);
        VST* ovl = BO::template bot_leaf_values<BITS>(bot, count);

        Bitmap256 bbm{}; uint16_t bc2[256] = {};
        for (uint32_t i = 0; i < count; ++i) {
            uint8_t bi = static_cast<uint8_t>(os2[i] >> (sb - 8));
            bbm.set_bit(bi); bc2[bi]++;
        }
        S ns2 = static_cast<S>(KO::template extract_suffix<sb>(ik));
        uint8_t nbi = static_cast<uint8_t>(ns2 >> (sb - 8));
        bbm.set_bit(nbi); bc2[nbi]++;
        int bcc = bbm.popcount();

        uint64_t* nb = alloc_node(alloc_, BO::bot_internal_size_u64(bcc));
        BO::bot_bitmap(nb) = bbm;
        uint64_t* children = BO::bot_internal_children(nb);

        constexpr int CB = BITS - 16;
        using CK = typename suffix_traits<CB>::type;
        constexpr uint64_t cmask = (1ULL << CB) - 1;
        CK ncs = static_cast<CK>(ns2 & cmask);

        int sl = 0;
        for (int bi = 0; bi < 256; ++bi) {
            if (!bbm.has_bit(bi)) continue;
            size_t cc = bc2[bi];
            uint64_t* child = alloc_node(alloc_, CO::template size_u64<CB>(cc, 0));
            auto* ch2 = get_header(child);
            ch2->count = static_cast<uint32_t>(cc); ch2->skip = 0; ch2->set_leaf(true);
            CK* ckd = CO::template keys_data<CB>(child, cc);
            VST* cv = CO::template values<CB>(child, cc);
            bool need = (nbi == bi), done = false; size_t ci = 0;
            for (uint32_t i = 0; i < count; ++i) {
                if ((os2[i] >> (sb - 8)) != (uint64_t)bi) continue;
                CK ocs = static_cast<CK>(os2[i] & cmask);
                if (need && !done && ncs < ocs) {
                    ckd[ci] = ncs; cv[ci] = value; ci++; done = true;
                }
                ckd[ci] = ocs; cv[ci] = ovl[i]; ci++;
            }
            if (need && !done) { ckd[ci] = ncs; cv[ci] = value; }
            KnSearch<CK>::build(CO::template search_start<CB>(child), ckd, static_cast<int>(cc));
            children[sl++] = reinterpret_cast<uint64_t>(child);
        }

        BO::template top_children<BITS>(node)[ts] = reinterpret_cast<uint64_t>(nb);
        BO::bot_is_leaf_bitmap(node).clear_bit(ti);
        h->count++;

        const Bitmap256& tbm2 = BO::top_bitmap(node);
        const Bitmap256& ilbm = BO::bot_is_leaf_bitmap(node);
        bool any = false;
        for (int i = tbm2.find_next_set(0); i >= 0; i = tbm2.find_next_set(i+1))
            if (ilbm.has_bit(i)) { any = true; break; }
        if (!any) h->set_leaf(false);

        dealloc_node(alloc_, bot, BO::template bot_leaf_size_u64<BITS>(count));
        return {node, true};
    }

    // ==================================================================
    // Split on prefix mismatch
    // ==================================================================

    template<int BITS>
    InsertResult split_on_prefix(uint64_t* node, NodeHeader* h,
                                 uint64_t ik, VST value, uint64_t expected)
        requires (BITS > 0)
    {
        uint64_t actual = get_prefix(node);
        int skip = h->skip;

        int common = 0;
        for (int i = skip - 1; i >= 0; --i) {
            uint16_t ec = (expected >> (i*16)) & 0xFFFF;
            uint16_t ac = (actual   >> (i*16)) & 0xFFFF;
            if (ec != ac) break;
            common++;
        }
        int di = skip - 1 - common;
        uint16_t nc2 = (expected >> (di*16)) & 0xFFFF;
        uint16_t oc = (actual   >> (di*16)) & 0xFFFF;
        uint8_t nt = (nc2 >> 8) & 0xFF, ot = (oc >> 8) & 0xFF;

        if (nt == ot) {
            uint8_t nb2 = nc2 & 0xFF, ob = oc & 0xFF;
            uint8_t ss = static_cast<uint8_t>(common);
            uint64_t* sn = alloc_node(alloc_, BO::template split_top_size_u64<BITS>(1, ss));
            auto* sh = get_header(sn);
            sh->count = h->count + 1; sh->top_count = 1; sh->skip = ss;
            if (common > 0) set_prefix(sn, expected >> ((skip - common) * 16));
            sh->set_split(true); sh->set_leaf(false);
            Bitmap256 tbm{}; tbm.set_bit(nt);
            BO::top_bitmap(sn) = tbm;
            if constexpr (BITS > 16) BO::bot_is_leaf_bitmap(sn) = Bitmap256{};

            uint64_t* bi = alloc_node(alloc_, BO::bot_internal_size_u64(2));
            Bitmap256 bbm{}; bbm.set_bit(nb2); bbm.set_bit(ob);
            BO::bot_bitmap(bi) = bbm;
            uint64_t* ch = BO::bot_internal_children(bi);

            int rem = di;
            h->skip = static_cast<uint8_t>(rem);
            if (rem > 0) set_prefix(node, actual & ((1ULL << (rem*16)) - 1));

            constexpr int CB = BITS - 16;
            uint8_t nls = static_cast<uint8_t>(rem);
            uint64_t* nl = alloc_node(alloc_, CO::template size_u64<CB>(1, nls));
            auto* nlh = get_header(nl);
            nlh->count = 1; nlh->skip = nls; nlh->set_leaf(true);
            if (rem > 0) set_prefix(nl, expected & ((1ULL << (rem*16)) - 1));
            using CK = typename suffix_traits<CB>::type;
            auto* nkd = CO::template keys_data<CB>(nl, 1);
            nkd[0] = static_cast<CK>(KO::template extract_suffix<CB>(ik));
            VT::write_slot(&CO::template values<CB>(nl, 1)[0], value);
            KnSearch<CK>::build(CO::template search_start<CB>(nl), nkd, 1);

            if (nb2 < ob) { ch[0] = reinterpret_cast<uint64_t>(nl); ch[1] = reinterpret_cast<uint64_t>(node); }
            else           { ch[0] = reinterpret_cast<uint64_t>(node); ch[1] = reinterpret_cast<uint64_t>(nl); }
            BO::template top_children<BITS>(sn)[0] = reinterpret_cast<uint64_t>(bi);
            return {sn, true};
        } else {
            uint8_t ss = static_cast<uint8_t>(common);
            uint64_t* sn = alloc_node(alloc_, BO::template split_top_size_u64<BITS>(2, ss));
            auto* sh = get_header(sn);
            sh->count = h->count + 1; sh->top_count = 2; sh->skip = ss;
            if (common > 0) set_prefix(sn, expected >> ((skip - common) * 16));
            sh->set_split(true); sh->set_leaf(false);
            Bitmap256 tbm{}; tbm.set_bit(nt); tbm.set_bit(ot);
            BO::top_bitmap(sn) = tbm;
            if constexpr (BITS > 16) BO::bot_is_leaf_bitmap(sn) = Bitmap256{};

            int rem = di;
            uint8_t ob = oc & 0xFF;
            uint64_t* obi = alloc_node(alloc_, BO::bot_internal_size_u64(1));
            Bitmap256 obm{}; obm.set_bit(ob);
            BO::bot_bitmap(obi) = obm;
            h->skip = static_cast<uint8_t>(rem);
            if (rem > 0) set_prefix(node, actual & ((1ULL << (rem*16)) - 1));
            BO::bot_internal_children(obi)[0] = reinterpret_cast<uint64_t>(node);

            uint8_t nb2 = nc2 & 0xFF;
            constexpr int CB = BITS - 16;
            uint8_t nls = static_cast<uint8_t>(rem);
            uint64_t* nl = alloc_node(alloc_, CO::template size_u64<CB>(1, nls));
            auto* nlh = get_header(nl);
            nlh->count = 1; nlh->skip = nls; nlh->set_leaf(true);
            if (rem > 0) set_prefix(nl, expected & ((1ULL << (rem*16)) - 1));
            using CK = typename suffix_traits<CB>::type;
            auto* nkd = CO::template keys_data<CB>(nl, 1);
            nkd[0] = static_cast<CK>(KO::template extract_suffix<CB>(ik));
            VT::write_slot(&CO::template values<CB>(nl, 1)[0], value);
            KnSearch<CK>::build(CO::template search_start<CB>(nl), nkd, 1);

            uint64_t* nbi = alloc_node(alloc_, BO::bot_internal_size_u64(1));
            Bitmap256 nbm{}; nbm.set_bit(nb2);
            BO::bot_bitmap(nbi) = nbm;
            BO::bot_internal_children(nbi)[0] = reinterpret_cast<uint64_t>(nl);

            uint64_t* tc2 = BO::template top_children<BITS>(sn);
            if (nt < ot) { tc2[0] = reinterpret_cast<uint64_t>(nbi); tc2[1] = reinterpret_cast<uint64_t>(obi); }
            else          { tc2[0] = reinterpret_cast<uint64_t>(obi); tc2[1] = reinterpret_cast<uint64_t>(nbi); }
            return {sn, true};
        }
    }

    // ==================================================================
    // Erase – recursive dispatch
    // ==================================================================

    template<int BITS>
    EraseResult erase_impl(uint64_t* node, uint64_t ik)
        requires (BITS <= 0)
    { return {node, false}; }

    template<int BITS>
    EraseResult erase_impl(uint64_t* node, uint64_t ik)
        requires (BITS > 0)
    {
        auto* h = get_header(node);
        if (h->skip > 0) [[unlikely]] {
            uint64_t exp = KO::template extract_prefix<BITS>(ik, h->skip);
            uint64_t act = get_prefix(node);
            if (exp != act) return {node, false};
            int ab = BITS - h->skip * 16;
            if (ab == 48) return erase_at_bits<48>(node, h, ik);
            if (ab == 32) return erase_at_bits<32>(node, h, ik);
            if (ab == 16) return erase_at_bits<16>(node, h, ik);
        }
        return erase_at_bits<BITS>(node, h, ik);
    }

    template<int BITS>
    EraseResult erase_at_bits(uint64_t* node, NodeHeader* h, uint64_t ik)
        requires (BITS <= 0)
    { return {node, false}; }

    template<int BITS>
    EraseResult erase_at_bits(uint64_t* node, NodeHeader* h, uint64_t ik)
        requires (BITS > 0)
    {
        if (h->is_leaf() && !h->is_split())
            return CO::template erase<BITS>(node, h, ik, alloc_);
        if (h->is_split())
            return erase_from_split<BITS>(node, h, ik);
        return {node, false};
    }

    // ==================================================================
    // Erase from split node
    // ==================================================================

    template<int BITS>
    EraseResult erase_from_split(uint64_t* node, NodeHeader* h, uint64_t ik)
        requires (BITS > 0)
    {
        uint8_t ti = KO::template extract_top8<BITS>(ik);
        Bitmap256& tbm = BO::top_bitmap(node);
        int ts;
        if (!tbm.find_slot(ti, ts)) return {node, false};

        uint64_t* bot = reinterpret_cast<uint64_t*>(
            BO::template top_children<BITS>(node)[ts]);

        bool is_leaf;
        if constexpr (BITS == 16) is_leaf = true;
        else is_leaf = BO::bot_is_leaf_bitmap(node).has_bit(ti);

        if (is_leaf) {
            auto [new_bot, erased] = BO::template erase_from_bot_leaf<BITS>(
                bot, ik, alloc_);
            if (!erased) return {node, false};
            if (new_bot) {
                // Bot shrunk but still exists — update pointer
                BO::template top_children<BITS>(node)[ts] =
                    reinterpret_cast<uint64_t>(new_bot);
                h->count--;
                return {node, true};
            }
            // Bot removed — remove top slot
            return remove_top_slot<BITS>(node, h, ts, ti);
        }

        if constexpr (BITS > 16) {
            return erase_from_bot_internal<BITS>(node, h, ti, ts, bot, ik);
        }
        return {node, false};
    }

    // ==================================================================
    // Remove a top slot from split node (reallocate with top_count-1)
    // ==================================================================

    template<int BITS>
    EraseResult remove_top_slot(uint64_t* node, NodeHeader* h,
                                int slot, uint8_t top_idx)
        requires (BITS > 0)
    {
        size_t otc = h->top_count;
        size_t ntc = otc - 1;

        if (ntc == 0) {
            // Split node empty — remove entirely
            dealloc_node(alloc_, node,
                BO::template split_top_size_u64<BITS>(otc, h->skip));
            return {nullptr, true};
        }

        uint64_t* nn = alloc_node(alloc_,
            BO::template split_top_size_u64<BITS>(ntc, h->skip));
        auto* nh = get_header(nn);
        *nh = *h;
        nh->count = h->count - 1;
        nh->top_count = static_cast<uint16_t>(ntc);
        if (h->skip > 0) set_prefix(nn, get_prefix(node));

        // Copy bitmaps with cleared bit
        BO::top_bitmap(nn) = BO::top_bitmap(node);
        BO::top_bitmap(nn).clear_bit(top_idx);
        if constexpr (BITS > 16) {
            BO::bot_is_leaf_bitmap(nn) = BO::bot_is_leaf_bitmap(node);
            BO::bot_is_leaf_bitmap(nn).clear_bit(top_idx);
        }

        // Copy child pointers, skipping removed slot
        const uint64_t* oc = BO::template top_children<BITS>(node);
        uint64_t* nc = BO::template top_children<BITS>(nn);
        for (int i = 0; i < slot; ++i)           nc[i] = oc[i];
        for (size_t i = slot; i < ntc; ++i)       nc[i] = oc[i + 1];

        dealloc_node(alloc_, node,
            BO::template split_top_size_u64<BITS>(otc, h->skip));
        return {nn, true};
    }

    // ==================================================================
    // Erase from bot_internal  (recurse into child)
    // ==================================================================

    template<int BITS>
    EraseResult erase_from_bot_internal(uint64_t* node, NodeHeader* h,
                                        uint8_t ti, int ts,
                                        uint64_t* bot, uint64_t ik)
        requires (BITS > 16)
    {
        uint8_t bi = KO::template extract_top8<BITS - 8>(ik);
        Bitmap256& bbm = BO::bot_bitmap(bot);
        int bs;
        if (!bbm.find_slot(bi, bs)) return {node, false};

        uint64_t* child = reinterpret_cast<uint64_t*>(
            BO::bot_internal_children(bot)[bs]);

        auto [new_child, erased] = erase_impl<BITS - 16>(child, ik);
        if (!erased) return {node, false};
        h->count--;

        if (new_child) {
            // Child shrunk — update pointer
            BO::bot_internal_children(bot)[bs] =
                reinterpret_cast<uint64_t>(new_child);
            return {node, true};
        }

        // Child removed — remove slot from bot_internal
        int bc = bbm.popcount();
        if (bc == 1) {
            // Bot_internal now empty — remove it and the top slot
            dealloc_node(alloc_, bot, BO::bot_internal_size_u64(bc));
            return remove_top_slot<BITS>(node, h, ts, ti);
        }

        // Reallocate bot_internal with one fewer child
        uint64_t* nb = alloc_node(alloc_, BO::bot_internal_size_u64(bc - 1));
        BO::bot_bitmap(nb) = bbm;
        BO::bot_bitmap(nb).clear_bit(bi);
        uint64_t* och = BO::bot_internal_children(bot);
        uint64_t* nch = BO::bot_internal_children(nb);
        for (int i = 0; i < bs; ++i)        nch[i] = och[i];
        for (int i = bs; i < bc - 1; ++i)   nch[i] = och[i + 1];

        BO::template top_children<BITS>(node)[ts] =
            reinterpret_cast<uint64_t>(nb);
        dealloc_node(alloc_, bot, BO::bot_internal_size_u64(bc));
        return {node, true};
    }

    // ==================================================================
    // Remove all  (recursive cleanup)
    // ==================================================================

    void remove_all() noexcept {
        if (root_) { remove_all_impl<KEY_BITS>(root_); root_ = nullptr; }
        size_ = 0;
    }

    template<int BITS>
    void remove_all_impl(uint64_t* node) noexcept {
        if constexpr (BITS <= 0) return;
        else {
            if (!node) return;
            auto* h = get_header(node);
            if (h->skip > 0) {
                int ab = BITS - h->skip * 16;
                if (ab == 48) { remove_at_bits<48>(node, h); return; }
                if (ab == 32) { remove_at_bits<32>(node, h); return; }
                if (ab == 16) { remove_at_bits<16>(node, h); return; }
                return;
            }
            remove_at_bits<BITS>(node, h);
        }
    }

    template<int BITS>
    void remove_at_bits(uint64_t* node, NodeHeader* h) noexcept {
        if constexpr (BITS <= 0) return;
        else {
            if (h->is_leaf() && !h->is_split()) {
                if constexpr (!VT::is_inline) {
                    auto* v = CO::template values<BITS>(node, h->count);
                    for (uint32_t i = 0; i < h->count; ++i) VT::destroy(v[i], alloc_);
                }
                dealloc_node(alloc_, node, CO::template size_u64<BITS>(h->count, h->skip));
            } else if (h->is_split()) {
                remove_split<BITS>(node, h);
            }
        }
    }

    template<int BITS>
    void remove_split(uint64_t* node, NodeHeader* h) noexcept {
        if constexpr (BITS <= 0) return;
        else {
            const Bitmap256& tbm = BO::top_bitmap(node);
            uint64_t* tch = BO::template top_children<BITS>(node);
            int sl = 0;
            for (int i = tbm.find_next_set(0); i >= 0; i = tbm.find_next_set(i+1)) {
                uint64_t* bot = reinterpret_cast<uint64_t*>(tch[sl]);
                bool il = (BITS == 16) || BO::bot_is_leaf_bitmap(node).has_bit(i);
                if (il) {
                    uint32_t bc2 = BO::template bot_leaf_count<BITS>(bot);
                    if constexpr (!VT::is_inline) {
                        auto* v = BO::template bot_leaf_values<BITS>(bot, bc2);
                        for (uint32_t j = 0; j < bc2; ++j) VT::destroy(v[j], alloc_);
                    }
                    dealloc_node(alloc_, bot, BO::template bot_leaf_size_u64<BITS>(bc2));
                } else if constexpr (BITS > 16) {
                    const Bitmap256& bbm = BO::bot_bitmap(bot);
                    int bc2 = bbm.popcount();
                    uint64_t* ch = BO::bot_internal_children(bot);
                    for (int j = 0; j < bc2; ++j)
                        remove_all_impl<BITS - 16>(reinterpret_cast<uint64_t*>(ch[j]));
                    dealloc_node(alloc_, bot, BO::bot_internal_size_u64(bc2));
                }
                ++sl;
            }
            dealloc_node(alloc_, node, BO::template split_top_size_u64<BITS>(h->top_count, h->skip));
        }
    }

    // ==================================================================
    // Stats collection
    // ==================================================================

    template<int BITS>
    void collect_stats(const uint64_t* node, DebugStats& s) const noexcept {
        if constexpr (BITS <= 0) return;
        else {
            if (!node) return;
            auto* h = get_header(node);
            if (h->skip > 0) {
                int ab = BITS - h->skip * 16;
                if (ab == 48) { stats_at_bits<48>(node, h, s, true); return; }
                if (ab == 32) { stats_at_bits<32>(node, h, s, true); return; }
                if (ab == 16) { stats_at_bits<16>(node, h, s, true); return; }
                return;
            }
            stats_at_bits<BITS>(node, h, s, false);
        }
    }

    template<int BITS>
    void stats_at_bits(const uint64_t* node, const NodeHeader* h,
                       DebugStats& s, bool compressed) const noexcept {
        if constexpr (BITS <= 0) return;
        else {
            constexpr int li = (static_cast<int>(KO::key_bits) - BITS) / 16;
            auto& L = s.levels[li < 4 ? li : 3];
            if (h->is_leaf() && !h->is_split()) {
                L.compact_leaf++;
                if (compressed) L.compact_leaf_compressed++;
                L.nodes++; L.entries += h->count;
                L.bytes += CO::template size_u64<BITS>(h->count, h->skip) * 8;
                L.compact_hist[h->count < 4097 ? h->count : 4097]++;
            } else if (h->is_split()) {
                L.split_nodes++;
                if (compressed) L.split_nodes_compressed++;
                L.nodes++;
                L.bytes += BO::template split_top_size_u64<BITS>(h->top_count, h->skip) * 8;
                const Bitmap256& tbm = BO::top_bitmap(node);
                const uint64_t* tch = BO::template top_children<BITS>(node);
                int sl = 0;
                for (int i = tbm.find_next_set(0); i >= 0; i = tbm.find_next_set(i+1)) {
                    const uint64_t* bot = reinterpret_cast<const uint64_t*>(tch[sl]);
                    bool il = (BITS == 16) || BO::bot_is_leaf_bitmap(node).has_bit(i);
                    if (il) {
                        L.bot_leaf++;
                        uint32_t bc2 = BO::template bot_leaf_count<BITS>(bot);
                        L.entries += bc2;
                        L.bytes += BO::template bot_leaf_size_u64<BITS>(bc2) * 8;
                        L.bot_leaf_hist[bc2 < 4097 ? bc2 : 4097]++;
                    } else if constexpr (BITS > 16) {
                        L.bot_internal++;
                        const Bitmap256& bbm = BO::bot_bitmap(bot);
                        int bc2 = bbm.popcount();
                        L.bytes += BO::bot_internal_size_u64(bc2) * 8;
                        const uint64_t* ch = BO::bot_internal_children(bot);
                        for (int j = 0; j < bc2; ++j)
                            collect_stats<BITS - 16>(reinterpret_cast<const uint64_t*>(ch[j]), s);
                    }
                    ++sl;
                }
            }
        }
    }
};

} // namespace kn3

#endif // KNTRIE_IMPL_HPP
