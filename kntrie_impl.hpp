#ifndef KNTRIE_IMPL_HPP
#define KNTRIE_IMPL_HPP

#include "kntrie_compact.hpp"
#include "kntrie_bitmask.hpp"
#include <memory>

namespace kn3 {

template<typename KEY, typename VALUE, typename ALLOC = std::allocator<uint64_t>>
class kntrie3 {
    static_assert(std::is_integral_v<KEY>);

public:
    using key_type       = KEY;
    using mapped_type    = VALUE;
    using size_type      = std::size_t;
    using allocator_type = ALLOC;

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
    // Constructor / Destructor
    // ==================================================================

    kntrie3() : size_(0), alloc_() {
        root_ = CO::template make_leaf<KEY_BITS>(nullptr, nullptr, 0, 0, 0, alloc_);
    }

    ~kntrie3() { remove_all(); }

    kntrie3(const kntrie3&) = delete;
    kntrie3& operator=(const kntrie3&) = delete;

    [[nodiscard]] bool      empty() const noexcept { return size_ == 0; }
    [[nodiscard]] size_type size()  const noexcept { return size_; }

    void clear() noexcept {
        remove_all();
        root_ = CO::template make_leaf<KEY_BITS>(nullptr, nullptr, 0, 0, 0, alloc_);
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

    bool contains(const KEY& key) const noexcept {
        return find_value(key) != nullptr;
    }

    // ==================================================================
    // Insert
    // ==================================================================

    std::pair<bool, bool> insert(const KEY& key, const VALUE& value) {
        uint64_t ik = KO::to_internal(key);
        VST sv = VT::store(value, alloc_);
        auto [new_root, inserted] = insert_impl<KEY_BITS>(root_, ik, sv);
        root_ = new_root;
        if (inserted) { ++size_; return {true, true}; }
        VT::destroy(sv, alloc_);
        return {true, false};
    }

    // ==================================================================
    // Erase
    // ==================================================================

    bool erase(const KEY& key) {
        uint64_t ik = KO::to_internal(key);
        auto [nn, erased] = erase_impl<KEY_BITS>(root_, ik);
        if (erased) {
            root_ = nn ? nn : CO::template make_leaf<KEY_BITS>(
                nullptr, nullptr, 0, 0, 0, alloc_);
            --size_;
        }
        return erased;
    }

    // ==================================================================
    // Stats / Memory
    // ==================================================================

    struct DebugStats {
        struct Level {
            size_t compact_leaf = 0, compact_leaf_compressed = 0;
            size_t split_nodes = 0,  split_nodes_compressed = 0;
            size_t bot_leaf = 0, bot_internal = 0;
            size_t entries = 0, nodes = 0, bytes = 0;
        };
        Level  levels[4];
        size_t total_nodes = 0, total_bytes = 0, total_entries = 0;
    };

    DebugStats debug_stats() const noexcept {
        DebugStats s{};
        collect_stats<KEY_BITS>(root_, s);
        for (int i = 0; i < 4; ++i) {
            s.total_nodes   += s.levels[i].nodes;
            s.total_bytes   += s.levels[i].bytes;
            s.total_entries += s.levels[i].entries;
        }
        return s;
    }

    size_t memory_usage() const noexcept { return debug_stats().total_bytes; }

    // ==================================================================
    // Debug helpers
    // ==================================================================

    struct RootInfo {
        uint16_t entries; uint16_t descendants; uint8_t skip;
        bool is_leaf; uint64_t prefix;
    };
    RootInfo debug_root_info() const {
        auto* h = get_header(root_);
        return {h->entries, h->descendants, h->skip,
                h->is_leaf(),
                h->skip > 0 ? get_prefix(root_) : 0};
    }
    uint64_t debug_key_to_internal(KEY k) const { return KO::to_internal(k); }

private:

    // ==================================================================
    // Find — recursive dispatch
    // ==================================================================

    template<int BITS>
    const VALUE* find_impl(const uint64_t* node, uint64_t ik, NodeHeader h,
                           uint64_t skip_prefix, int skip_left) const noexcept
        requires (BITS == 16)
    {
        if (h.is_leaf()) [[unlikely]]
            return CO::template find<16>(node, &h, ik);
        return find_in_split<16>(node, ik);
    }

    template<int BITS>
    const VALUE* find_impl(const uint64_t* node, uint64_t ik, NodeHeader h,
                           uint64_t skip_prefix, int skip_left) const noexcept
        requires (BITS > 16)
    {
        uint16_t key_chunk = KO::template extract_top16<BITS>(ik);

        if (skip_left > 0) [[unlikely]] {
            uint16_t sc = KO::get_skip_chunk(skip_prefix, h.skip, skip_left);
            if (key_chunk != sc) [[unlikely]] return nullptr;
            skip_left--;
        } else if (h.skip >= 1) [[unlikely]] {
            uint64_t np = get_prefix(node);
            uint16_t sc = KO::get_skip_chunk(np, h.skip, h.skip);
            if (key_chunk != sc) [[unlikely]] return nullptr;
            if (h.skip > 1) [[unlikely]] { skip_prefix = np; skip_left = h.skip - 1; }
            h.skip = 0;
        } else if (h.is_leaf()) [[unlikely]] {
            return CO::template find<BITS>(node, &h, ik);
        } else {
            return find_in_split<BITS>(node, ik);
        }

        return find_impl<BITS - 16>(node, ik, h, skip_prefix, skip_left);
    }

    // ==================================================================
    // Find within a split node
    // ==================================================================

    template<int BITS>
    const VALUE* find_in_split(const uint64_t* node, uint64_t ik) const noexcept
        requires (BITS > 0)
    {
        uint8_t ti = KO::template extract_top8<BITS>(ik);

        if constexpr (BITS > 16) {
            const uint64_t* bot = BO::template branchless_top_child<BITS>(node, ti);

            if (BO::template is_top_entry_leaf<BITS>(node, ti)) [[unlikely]] {
                return BO::template find_in_bot_leaf<BITS>(bot, ik);
            }

            uint8_t bi = KO::template extract_top8<BITS - 8>(ik);
            const uint64_t* child = BO::branchless_bot_child(bot, bi);
            NodeHeader ch = *get_header(child);
            return find_impl<BITS - 16>(child, ik, ch, 0, 0);
        } else {
            auto lk = BO::template lookup_top<BITS>(node, ti);
            if (!lk.found) [[unlikely]] return nullptr;
            return BO::template find_in_bot_leaf<BITS>(lk.bot, ik);
        }
    }

    // ==================================================================
    // Insert — recursive dispatch
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
            uint64_t expected = KO::template extract_prefix<BITS>(ik, h->skip);
            uint64_t actual   = get_prefix(node);
            if (expected != actual)
                return split_on_prefix<BITS>(node, h, ik, value, expected);
            int ab = BITS - h->skip * 16;
            if (ab == 48) return insert_at_bits<48>(node, h, ik, value);
            if (ab == 32) return insert_at_bits<32>(node, h, ik, value);
            if (ab == 16) return insert_at_bits<16>(node, h, ik, value);
        }
        return insert_at_bits<BITS>(node, h, ik, value);
    }

    template<int BITS>
    InsertResult insert_at_bits(uint64_t* node, NodeHeader* h,
                                uint64_t ik, VST value)
        requires (BITS <= 0)
    { return {node, false}; }

    template<int BITS>
    InsertResult insert_at_bits(uint64_t* node, NodeHeader* h,
                                uint64_t ik, VST value)
        requires (BITS > 0)
    {
        if (h->is_leaf()) {
            auto r = CO::template insert<BITS>(node, h, ik, value, alloc_);
            if (r.needs_split)
                return convert_to_split<BITS>(node, h, ik, value);
            return {r.node, r.inserted};
        }
        return insert_into_split<BITS>(node, h, ik, value);
    }

    // ------------------------------------------------------------------
    // Insert into split node
    // ------------------------------------------------------------------

    template<int BITS>
    InsertResult insert_into_split(uint64_t* node, NodeHeader* h,
                                   uint64_t ik, VST value)
        requires (BITS > 0)
    {
        uint8_t ti = KO::template extract_top8<BITS>(ik);
        auto lk = BO::template lookup_top<BITS>(node, ti);

        if (!lk.found) {
            auto* bot = BO::template make_single_bot_leaf<BITS>(ik, value, alloc_);
            auto* nn = BO::template add_top_slot<BITS>(
                node, h, ti, bot, /*is_leaf=*/true, alloc_);
            return {nn, true};
        }

        if (lk.is_leaf) {
            auto r = BO::template insert_into_bot_leaf<BITS>(
                lk.bot, ik, value, alloc_);

            if (r.overflow) {
                if constexpr (BITS > 16) {
                    uint32_t bc = BO::template bot_leaf_count<BITS>(lk.bot);
                    return convert_bot_leaf_to_internal<BITS>(
                        node, h, ti, lk.slot, lk.bot, bc, ik, value);
                }
            }
            BO::template set_top_child<BITS>(node, lk.slot, r.new_bot);
            if (r.inserted) h->add_descendants(1);
            return {node, r.inserted};
        }

        if constexpr (BITS > 16)
            return insert_into_bot_internal<BITS>(
                node, h, ti, lk.slot, lk.bot, ik, value);
        return {node, false};
    }

    // ------------------------------------------------------------------
    // Insert into bot_internal (recurse into child)
    // ------------------------------------------------------------------

    template<int BITS>
    InsertResult insert_into_bot_internal(uint64_t* node, NodeHeader* h,
                                          uint8_t ti, int ts,
                                          uint64_t* bot, uint64_t ik, VST value)
        requires (BITS > 16)
    {
        uint8_t bi = KO::template extract_top8<BITS - 8>(ik);
        auto blk = BO::lookup_bot_child(bot, bi);

        if (blk.found) {
            auto [nc, ins] = insert_impl<BITS - 16>(blk.child, ik, value);
            BO::set_bot_child(bot, blk.slot, nc);
            if (ins) h->add_descendants(1);
            return {node, ins};
        }

        constexpr int CB = BITS - 16;
        using CK = typename suffix_traits<CB>::type;
        CK ck = static_cast<CK>(KO::template extract_suffix<CB>(ik));
        auto* child = CO::template make_leaf<CB>(&ck, &value, 1, 0, 0, alloc_);

        auto* new_bot = BO::add_bot_child(bot, bi, child, alloc_);
        BO::template set_top_child<BITS>(node, ts, new_bot);
        h->add_descendants(1);
        return {node, true};
    }

    // ==================================================================
    // Conversion: compact leaf → split
    // ==================================================================

    template<int BITS>
    InsertResult convert_to_split(uint64_t* node, NodeHeader* h,
                                  uint64_t ik, VST value)
        requires (BITS > 0)
    {
        using K = typename suffix_traits<BITS>::type;

        uint16_t old_count = h->entries;
        size_t total = old_count + 1;
        auto wk = std::make_unique<uint64_t[]>(total);
        auto wv = std::make_unique<VST[]>(total);

        K new_suffix = static_cast<K>(KO::template extract_suffix<BITS>(ik));
        size_t wi = 0;
        bool ins = false;
        CO::template for_each<BITS>(node, h, [&](K s, VST v) {
            if (!ins && new_suffix < s) {
                wk[wi] = static_cast<uint64_t>(new_suffix);
                wv[wi] = value; wi++; ins = true;
            }
            wk[wi] = static_cast<uint64_t>(s);
            wv[wi] = v; wi++;
        });
        if (!ins) { wk[wi] = static_cast<uint64_t>(new_suffix); wv[wi] = value; }

        uint64_t* child = build_node_from_arrays<BITS>(wk.get(), wv.get(), total);

        if (h->skip > 0) {
            auto* ch2 = get_header(child);
            uint64_t old_cp = ch2->skip > 0 ? get_prefix(child) : 0;
            uint8_t  os     = ch2->skip;
            uint8_t  ns     = h->skip + os;
            uint64_t parent_prefix = get_prefix(node);
            uint64_t combined = (parent_prefix << (16 * os)) | old_cp;
            if (os == 0)
                child = prepend_skip<BITS>(child, ns, combined);
            else { ch2->skip = ns; set_prefix(child, combined); }
        }

        dealloc_node(alloc_, node, h->alloc_u64);
        return {child, true};
    }

    // ==================================================================
    // Conversion: bot_leaf → bot_internal
    // ==================================================================

    template<int BITS>
    InsertResult convert_bot_leaf_to_internal(
            uint64_t* node, NodeHeader* h, uint8_t ti, int ts,
            uint64_t* bot, uint32_t count, uint64_t ik, VST value)
        requires (BITS > 16)
    {
        constexpr int sb = BITS - 8;
        using S = typename suffix_traits<sb>::type;

        size_t total = count + 1;
        auto wk = std::make_unique<S[]>(total);
        auto wv = std::make_unique<VST[]>(total);

        S new_suffix = static_cast<S>(KO::template extract_suffix<sb>(ik));
        size_t wi = 0;
        bool ins = false;
        BO::template for_each_bot_leaf<BITS>(bot, [&](S s, VST v) {
            if (!ins && new_suffix < s) {
                wk[wi] = new_suffix; wv[wi] = value; wi++; ins = true;
            }
            wk[wi] = s; wv[wi] = v; wi++;
        });
        if (!ins) { wk[wi] = new_suffix; wv[wi] = value; }

        constexpr int CB = BITS - 16;
        using CK = typename suffix_traits<CB>::type;
        constexpr uint64_t cmask = (CB >= 64) ? ~uint64_t(0) : ((1ULL << CB) - 1);

        uint8_t  indices[256];
        uint64_t* child_ptrs[256];
        int n_children = 0;

        size_t i = 0;
        while (i < total) {
            uint8_t bi = static_cast<uint8_t>(wk[i] >> (sb - 8));
            size_t start = i;
            while (i < total && static_cast<uint8_t>(wk[i] >> (sb - 8)) == bi) ++i;
            size_t cc = i - start;

            auto ck = std::make_unique<CK[]>(cc);
            for (size_t j = 0; j < cc; ++j)
                ck[j] = static_cast<CK>(wk[start + j] & cmask);

            auto* child = CO::template make_leaf<CB>(
                ck.get(), wv.get() + start, static_cast<uint32_t>(cc), 0, 0, alloc_);

            indices[n_children]    = bi;
            child_ptrs[n_children] = child;
            n_children++;
        }

        auto* new_bot = BO::make_bot_internal(indices, child_ptrs, n_children, alloc_);

        BO::template set_top_child<BITS>(node, ts, new_bot);
        BO::template mark_bot_internal<BITS>(node, ti);
        h->add_descendants(1);

        BO::template dealloc_bot_leaf<BITS>(bot, count, alloc_);
        return {node, true};
    }

    // ==================================================================
    // Build node from working arrays
    // ==================================================================

    template<int BITS>
    uint64_t* build_node_from_arrays(uint64_t* suf, VST* vals, size_t count)
        requires (BITS > 0)
    {
        if (count <= COMPACT_MAX) {
            using K = typename suffix_traits<BITS>::type;
            auto tk = std::make_unique<K[]>(count);
            auto tv = std::make_unique<VST[]>(count);
            for (size_t i = 0; i < count; ++i) {
                K k = static_cast<K>(suf[i]); VST v = vals[i];
                size_t j = i;
                while (j > 0 && tk[j-1] > k) {
                    tk[j] = tk[j-1]; tv[j] = tv[j-1]; j--;
                }
                tk[j] = k; tv[j] = v;
            }
            return CO::template make_leaf<BITS>(
                tk.get(), tv.get(), static_cast<uint32_t>(count), 0, 0, alloc_);
        }

        if constexpr (BITS > 16) {
            uint8_t first_top = static_cast<uint8_t>(suf[0] >> (BITS - 8));
            bool all_same_top = true;
            for (size_t i = 1; i < count; ++i)
                if (static_cast<uint8_t>(suf[i] >> (BITS - 8)) != first_top)
                    { all_same_top = false; break; }

            if (all_same_top) {
                constexpr int sb = BITS - 8;
                uint8_t first_bot = static_cast<uint8_t>(suf[0] >> (sb - 8));
                bool all_same_bot = true;
                for (size_t i = 1; i < count; ++i)
                    if (static_cast<uint8_t>(suf[i] >> (sb - 8)) != first_bot)
                        { all_same_bot = false; break; }

                if (all_same_bot) {
                    uint16_t sp = (static_cast<uint16_t>(first_top) << 8) | first_bot;
                    constexpr int CB = BITS - 16;
                    constexpr uint64_t cm = (CB >= 64) ? ~uint64_t(0) : ((1ULL << CB) - 1);
                    for (size_t i = 0; i < count; ++i) suf[i] &= cm;

                    uint64_t* child = build_node_from_arrays<CB>(suf, vals, count);

                    auto* ch = get_header(child);
                    uint64_t ocp = ch->skip > 0 ? get_prefix(child) : 0;
                    uint8_t  os  = ch->skip;
                    uint8_t  ns  = os + 1;
                    uint64_t combined = (static_cast<uint64_t>(sp) << (16 * os)) | ocp;

                    if (os == 0)
                        return prepend_skip<CB>(child, ns, combined);
                    ch->skip = ns; set_prefix(child, combined);
                    return child;
                }
            }
        }

        return build_split_from_arrays<BITS>(suf, vals, count);
    }

    template<int BITS>
    uint64_t* build_split_from_arrays(uint64_t* suf, VST* vals, size_t count)
        requires (BITS > 0)
    {
        uint8_t   top_indices[256];
        uint64_t* bot_ptrs[256];
        bool      is_leaf_flags[256];
        int       n_tops = 0;

        constexpr int sb = BITS - 8;
        constexpr uint64_t smask = (sb >= 64) ? ~uint64_t(0) : ((1ULL << sb) - 1);

        size_t i = 0;
        while (i < count) {
            uint8_t ti = static_cast<uint8_t>(suf[i] >> (BITS - 8));
            size_t start = i;
            while (i < count && static_cast<uint8_t>(suf[i] >> (BITS - 8)) == ti) ++i;
            size_t bcount = i - start;

            bool need_internal = false;
            if constexpr (BITS > 16) need_internal = (bcount > BOT_LEAF_MAX);

            if (need_internal) {
                if constexpr (BITS > 16) {
                    bot_ptrs[n_tops] = build_bot_internal_from_range<BITS>(
                        suf + start, vals + start, bcount);
                    is_leaf_flags[n_tops] = false;
                }
            } else {
                using S = typename suffix_traits<sb>::type;
                auto bk = std::make_unique<S[]>(bcount);
                for (size_t j = 0; j < bcount; ++j)
                    bk[j] = static_cast<S>(suf[start + j] & smask);

                bot_ptrs[n_tops] = BO::template make_bot_leaf<BITS>(
                    bk.get(), vals + start, static_cast<uint32_t>(bcount), alloc_);
                is_leaf_flags[n_tops] = true;
            }
            top_indices[n_tops] = ti;
            n_tops++;
        }

        return BO::template make_split_top<BITS>(
            top_indices, bot_ptrs, is_leaf_flags, n_tops,
            /*skip=*/0, /*prefix=*/0,
            static_cast<uint32_t>(count), alloc_);
    }

    template<int BITS>
    uint64_t* build_bot_internal_from_range(uint64_t* suf, VST* vals, size_t count)
        requires (BITS > 16)
    {
        constexpr int sb = BITS - 8;
        constexpr int CB = BITS - 16;
        constexpr uint64_t cmask = (CB >= 64) ? ~uint64_t(0) : ((1ULL << CB) - 1);

        uint8_t   indices[256];
        uint64_t* child_ptrs[256];
        int       n_children = 0;

        size_t i = 0;
        while (i < count) {
            uint8_t bi = static_cast<uint8_t>((suf[i] >> (sb - 8)) & 0xFF);
            size_t start = i;
            while (i < count &&
                   static_cast<uint8_t>((suf[i] >> (sb - 8)) & 0xFF) == bi) ++i;
            size_t cc = i - start;

            auto cs = std::make_unique<uint64_t[]>(cc);
            for (size_t j = 0; j < cc; ++j) cs[j] = suf[start + j] & cmask;

            indices[n_children]    = bi;
            child_ptrs[n_children] = build_node_from_arrays<CB>(
                cs.get(), vals + start, cc);
            n_children++;
        }

        return BO::make_bot_internal(indices, child_ptrs, n_children, alloc_);
    }

    // ==================================================================
    // Helper: prepend skip/prefix to a node with skip==0
    // ==================================================================

    template<int BITS>
    uint64_t* prepend_skip(uint64_t* node, uint8_t new_skip, uint64_t prefix) {
        auto* h = get_header(node);
        assert(h->skip == 0);

        size_t old_sz = h->alloc_u64;
        size_t new_needed = old_sz + 1;  // +1 u64 for prefix slot
        size_t new_sz = round_up_u64(new_needed);

        size_t data_u64 = old_sz - 1;  // everything after 1-u64 header
        uint64_t* nn = alloc_node(alloc_, new_sz);
        *get_header(nn) = *h;
        get_header(nn)->skip = new_skip;
        get_header(nn)->alloc_u64 = static_cast<uint16_t>(new_sz);
        set_prefix(nn, prefix);
        std::memcpy(nn + 2, node + 1, data_u64 * 8);
        dealloc_node(alloc_, node, old_sz);
        return nn;
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
            uint16_t ec = (expected >> (i * 16)) & 0xFFFF;
            uint16_t ac = (actual   >> (i * 16)) & 0xFFFF;
            if (ec != ac) break;
            common++;
        }
        int di = skip - 1 - common;
        uint16_t nc = (expected >> (di * 16)) & 0xFFFF;
        uint16_t oc = (actual   >> (di * 16)) & 0xFFFF;
        uint8_t nt = (nc >> 8) & 0xFF, ot = (oc >> 8) & 0xFF;

        uint8_t ss = static_cast<uint8_t>(common);
        uint64_t split_prefix = common > 0
            ? (expected >> ((skip - common) * 16)) : 0;

        int rem = di;
        h->skip = static_cast<uint8_t>(rem);
        if (rem > 0)
            set_prefix(node, actual & ((1ULL << (rem * 16)) - 1));

        constexpr int CB = BITS - 16;
        using CK = typename suffix_traits<CB>::type;
        CK ck = static_cast<CK>(KO::template extract_suffix<CB>(ik));
        uint8_t nls = static_cast<uint8_t>(rem);
        uint64_t nl_prefix = rem > 0
            ? (expected & ((1ULL << (rem * 16)) - 1)) : 0;
        auto* nl = CO::template make_leaf<CB>(&ck, &value, 1, nls, nl_prefix, alloc_);

        uint32_t total_desc = static_cast<uint32_t>(h->descendants) + 1;

        if (nt == ot) {
            uint8_t nb = nc & 0xFF, ob = oc & 0xFF;

            uint8_t bi[2]; uint64_t* cp[2];
            if (nb < ob) { bi[0]=nb; cp[0]=nl; bi[1]=ob; cp[1]=node; }
            else         { bi[0]=ob; cp[0]=node; bi[1]=nb; cp[1]=nl; }
            auto* bot_int = BO::make_bot_internal(bi, cp, 2, alloc_);

            uint8_t   ti_arr[1] = {nt};
            uint64_t* bp_arr[1] = {bot_int};
            bool      il_arr[1] = {false};
            auto* sn = BO::template make_split_top<BITS>(
                ti_arr, bp_arr, il_arr, 1, ss, split_prefix,
                total_desc, alloc_);
            return {sn, true};
        } else {
            uint8_t ob = oc & 0xFF, nb = nc & 0xFF;

            uint8_t obi[1] = {ob}; uint64_t* ocp[1] = {node};
            auto* old_bot = BO::make_bot_internal(obi, ocp, 1, alloc_);

            uint8_t nbi[1] = {nb}; uint64_t* ncp[1] = {nl};
            auto* new_bot = BO::make_bot_internal(nbi, ncp, 1, alloc_);

            uint8_t   ti_arr[2]; uint64_t* bp_arr[2]; bool il_arr[2] = {false, false};
            if (nt < ot) { ti_arr[0]=nt; bp_arr[0]=new_bot; ti_arr[1]=ot; bp_arr[1]=old_bot; }
            else         { ti_arr[0]=ot; bp_arr[0]=old_bot; ti_arr[1]=nt; bp_arr[1]=new_bot; }

            auto* sn = BO::template make_split_top<BITS>(
                ti_arr, bp_arr, il_arr, 2, ss, split_prefix,
                total_desc, alloc_);
            return {sn, true};
        }
    }

    // ==================================================================
    // Erase — recursive dispatch
    // ==================================================================

    template<int BITS>
    EraseResult erase_impl(uint64_t* node, uint64_t ik, int skip_remaining = -1)
        requires (BITS <= 0)
    { return {node, false}; }

    template<int BITS>
    EraseResult erase_impl(uint64_t* node, uint64_t ik, int skip_remaining = -1)
        requires (BITS > 0)
    {
        if (skip_remaining < 0) {
            auto* h = get_header(node);
            if (h->skip > 0) [[unlikely]] {
                uint64_t exp = KO::template extract_prefix<BITS>(ik, h->skip);
                if (exp != get_prefix(node)) return {node, false};
                return erase_impl<BITS - 16>(node, ik, h->skip - 1);
            }
            return erase_at_bits<BITS>(node, h, ik);
        }
        if (skip_remaining > 0)
            return erase_impl<BITS - 16>(node, ik, skip_remaining - 1);
        return erase_at_bits<BITS>(node, get_header(node), ik);
    }

    template<int BITS>
    EraseResult erase_at_bits(uint64_t* node, NodeHeader* h, uint64_t ik)
        requires (BITS <= 0)
    { return {node, false}; }

    template<int BITS>
    EraseResult erase_at_bits(uint64_t* node, NodeHeader* h, uint64_t ik)
        requires (BITS > 0)
    {
        if (h->is_leaf())
            return CO::template erase<BITS>(node, h, ik, alloc_);
        return erase_from_split<BITS>(node, h, ik);
    }

    // ------------------------------------------------------------------
    // Erase from split node
    // ------------------------------------------------------------------

    template<int BITS>
    EraseResult erase_from_split(uint64_t* node, NodeHeader* h, uint64_t ik)
        requires (BITS > 0)
    {
        uint8_t ti = KO::template extract_top8<BITS>(ik);
        auto lk = BO::template lookup_top<BITS>(node, ti);
        if (!lk.found) return {node, false};

        if (lk.is_leaf) {
            auto [new_bot, erased] = BO::template erase_from_bot_leaf<BITS>(
                lk.bot, ik, alloc_);
            if (!erased) return {node, false};
            if (new_bot) {
                BO::template set_top_child<BITS>(node, lk.slot, new_bot);
                h->sub_descendants(1);
                return {node, true};
            }
            auto* nn = BO::template remove_top_slot<BITS>(
                node, h, lk.slot, ti, alloc_);
            return {nn, true};
        }

        if constexpr (BITS > 16)
            return erase_from_bot_internal<BITS>(
                node, h, ti, lk.slot, lk.bot, ik);
        return {node, false};
    }

    // ------------------------------------------------------------------
    // Erase from bot_internal
    // ------------------------------------------------------------------

    template<int BITS>
    EraseResult erase_from_bot_internal(uint64_t* node, NodeHeader* h,
                                         uint8_t ti, int ts,
                                         uint64_t* bot, uint64_t ik)
        requires (BITS > 16)
    {
        uint8_t bi = KO::template extract_top8<BITS - 8>(ik);
        auto blk = BO::lookup_bot_child(bot, bi);
        if (!blk.found) return {node, false};

        auto [nc, erased] = erase_impl<BITS - 16>(blk.child, ik);
        if (!erased) return {node, false};
        h->sub_descendants(1);

        if (nc) {
            BO::set_bot_child(bot, blk.slot, nc);
            return {node, true};
        }

        int bc = BO::bot_internal_child_count(bot);
        if (bc == 1) {
            BO::dealloc_bot_internal(bot, alloc_);
            auto* nn = BO::template remove_top_slot<BITS>(
                node, h, ts, ti, alloc_);
            return {nn, true};
        }

        auto* nb = BO::remove_bot_child(bot, blk.slot, bi, alloc_);
        BO::template set_top_child<BITS>(node, ts, nb);
        return {node, true};
    }

    // ==================================================================
    // Remove all
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
                if (ab == 48) { remove_all_at_bits<48>(node); return; }
                if (ab == 32) { remove_all_at_bits<32>(node); return; }
                if (ab == 16) { remove_all_at_bits<16>(node); return; }
                return;
            }
            remove_all_at_bits<BITS>(node);
        }
    }

    template<int BITS>
    void remove_all_at_bits(uint64_t* node) noexcept {
        if constexpr (BITS <= 0) return;
        else {
            auto* h = get_header(node);
            if (h->is_leaf()) {
                CO::template destroy_and_dealloc<BITS>(node, alloc_);
                return;
            }
            BO::template for_each_top<BITS>(node,
                [&](uint8_t /*ti*/, int /*slot*/, uint64_t* bot, bool is_leaf) {
                    if (is_leaf) {
                        BO::template destroy_bot_leaf_and_dealloc<BITS>(bot, alloc_);
                    } else {
                        if constexpr (BITS > 16) {
                            BO::for_each_bot_child(bot,
                                [&](uint8_t /*bi*/, uint64_t* child) {
                                    remove_all_impl<BITS - 16>(child);
                                });
                            BO::dealloc_bot_internal(bot, alloc_);
                        }
                    }
                });
            BO::template dealloc_split_top<BITS>(node, alloc_);
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
                if (ab == 48) { collect_stats_at_bits<48>(node, s, true); return; }
                if (ab == 32) { collect_stats_at_bits<32>(node, s, true); return; }
                if (ab == 16) { collect_stats_at_bits<16>(node, s, true); return; }
                return;
            }
            collect_stats_at_bits<BITS>(node, s, false);
        }
    }

    template<int BITS>
    void collect_stats_at_bits(const uint64_t* node, DebugStats& s,
                               bool compressed) const noexcept {
        if constexpr (BITS <= 0) return;
        else {
            constexpr int li = (KEY_BITS - BITS) / 16;
            auto& L = s.levels[li < 4 ? li : 3];
            auto* h = get_header(node);

            if (h->is_leaf()) {
                L.compact_leaf++;
                if (compressed) L.compact_leaf_compressed++;
                L.nodes++;
                L.entries += h->entries;
                L.bytes += static_cast<size_t>(h->alloc_u64) * 8;
            } else {
                L.split_nodes++;
                if (compressed) L.split_nodes_compressed++;
                L.nodes++;
                L.bytes += static_cast<size_t>(h->alloc_u64) * 8;

                BO::template for_each_top<BITS>(node,
                    [&](uint8_t /*ti*/, int /*slot*/, uint64_t* bot, bool is_leaf) {
                        if (is_leaf) {
                            L.bot_leaf++;
                            uint32_t bc = BO::template bot_leaf_count<BITS>(bot);
                            L.entries += bc;
                            L.bytes += BO::template bot_leaf_size_u64<BITS>(bc) * 8;
                        } else {
                            if constexpr (BITS > 16) {
                                L.bot_internal++;
                                int bc = BO::bot_internal_child_count(bot);
                                L.bytes += BO::bot_internal_alloc_u64(bot) * 8;
                                BO::for_each_bot_child(bot,
                                    [&](uint8_t /*bi*/, uint64_t* child) {
                                        collect_stats<BITS - 16>(child, s);
                                    });
                            }
                        }
                    });
            }
        }
    }
};

} // namespace kn3

#endif // KNTRIE_IMPL_HPP
