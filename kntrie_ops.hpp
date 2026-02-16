#ifndef KNTRIE_OPS_HPP
#define KNTRIE_OPS_HPP

#include "kntrie_bitmask.hpp"
#include "kntrie_compact.hpp"

namespace gteitelbaum {

// ======================================================================
// kntrie_ops<NK, VALUE, ALLOC> — stateless trie operations.
//
// NK = narrowed key type (u64/u32/u16/u8). As trie descent consumes
// bytes, NK narrows at half-width boundaries via the Narrow alias.
// This eliminates all runtime suffix_type dispatch.
// ======================================================================

template<typename NK, typename VALUE, typename ALLOC>
struct kntrie_ops {
    using BO  = bitmask_ops<VALUE, ALLOC>;
    using VT  = value_traits<VALUE, ALLOC>;
    using VST = typename VT::slot_type;
    using CO  = compact_ops<NK, VALUE, ALLOC>;

    static constexpr int NK_BITS = sizeof(NK) * 8;

    using NNK    = next_narrow_t<NK>;
    using Narrow = kntrie_ops<NNK, VALUE, ALLOC>;

    // ==================================================================
    // Find — template<BITS> only, NK comes from struct
    // ==================================================================

    template<int BITS> requires (BITS > 8)
    static const VALUE* find_node_(uint64_t ptr, NK ik) noexcept {
        if (ptr & LEAF_BIT) [[unlikely]] {
            const uint64_t* node = reinterpret_cast<const uint64_t*>(ptr ^ LEAF_BIT);
            node_header hdr = *get_header(node);
            return find_leaf_<BITS>(node, hdr, ik,
                hdr.skip(), reinterpret_cast<const uint8_t*>(&node[1]));
        }

        const uint64_t* bm = reinterpret_cast<const uint64_t*>(ptr);
        uint8_t ti = static_cast<uint8_t>(ik >> (NK_BITS - 8));
        int slot = reinterpret_cast<const bitmap256*>(bm)->
                       find_slot<slot_mode::BRANCHLESS>(ti);
        uint64_t child = bm[BITMAP256_U64 + slot];

        if constexpr (BITS - 8 == NK_BITS / 2 && NK_BITS > 8) {
            return Narrow::template find_node_<BITS - 8>(child,
                static_cast<NNK>(static_cast<NK>(ik << 8) >> (NK_BITS / 2)));
        } else {
            return find_node_<BITS - 8>(child, static_cast<NK>(ik << 8));
        }
    }

    template<int BITS> requires (BITS == 8)
    static const VALUE* find_node_(uint64_t ptr, NK ik) noexcept {
        const uint64_t* node = reinterpret_cast<const uint64_t*>(ptr ^ LEAF_BIT);
        node_header hdr = *get_header(node);
        return find_leaf_<8>(node, hdr, ik, 0, nullptr);
    }

    template<int BITS> requires (BITS > 8)
    static const VALUE* find_leaf_(const uint64_t* node, node_header hdr, NK ik,
                                   uint8_t skip, const uint8_t* prefix) noexcept {
        if (skip) [[unlikely]] {
            if (static_cast<uint8_t>(ik >> (NK_BITS - 8)) != *prefix) [[unlikely]]
                return nullptr;
            if constexpr (BITS - 8 == NK_BITS / 2 && NK_BITS > 8) {
                return Narrow::template find_leaf_<BITS - 8>(node, hdr,
                    static_cast<NNK>(static_cast<NK>(ik << 8) >> (NK_BITS / 2)),
                    skip - 1, prefix + 1);
            } else {
                return find_leaf_<BITS - 8>(node, hdr,
                    static_cast<NK>(ik << 8), skip - 1, prefix + 1);
            }
        }

        size_t hs = 1 + (hdr.skip() > 0);
        return CO::find(node, hdr, ik, hs);
    }

    template<int BITS> requires (BITS == 8)
    static const VALUE* find_leaf_(const uint64_t* node, node_header hdr, NK ik,
                                   uint8_t, const uint8_t*) noexcept {
        return BO::bitmap_find(node, hdr,
            static_cast<uint8_t>(ik), 1 + (hdr.skip() > 0));
    }

    // ==================================================================
    // NK-dependent helpers (compile-time suffix_type dispatch)
    // ==================================================================

    // Create a 1-entry leaf. Returns raw (untagged) pointer.
    static uint64_t* make_single_leaf_(NK suffix, VST value, ALLOC& alloc) {
        if constexpr (sizeof(NK) == 1) {
            return BO::make_single_bitmap(static_cast<uint8_t>(suffix), value, alloc);
        } else {
            return CO::make_leaf(&suffix, &value, 1, 0, nullptr, alloc);
        }
    }

    // Iterate leaf entries, callback receives (uint64_t bit63_aligned, VST value)
    template<typename Fn>
    static void leaf_for_each_aligned_(const uint64_t* node, const node_header* hdr,
                                        Fn&& cb) {
        constexpr int SHIFT = 64 - NK_BITS;
        if constexpr (sizeof(NK) == 1) {
            BO::for_each_bitmap(node, [&](uint8_t s, VST v) {
                cb(uint64_t(s) << SHIFT, v);
            });
        } else {
            CO::for_each(node, hdr, [&](NK s, VST v) {
                if constexpr (SHIFT > 0)
                    cb(uint64_t(s) << SHIFT, v);
                else
                    cb(uint64_t(s), v);
            });
        }
    }

    // Build leaf from bit-63-aligned uint64_t arrays. Returns raw pointer.
    static uint64_t* build_leaf_(uint64_t* suf, VST* vals, size_t count, ALLOC& alloc) {
        if constexpr (sizeof(NK) == 1) {
            auto bk = std::make_unique<uint8_t[]>(count);
            for (size_t i = 0; i < count; ++i)
                bk[i] = static_cast<uint8_t>(suf[i] >> 56);
            return BO::make_bitmap_leaf(bk.get(), vals,
                static_cast<uint32_t>(count), alloc);
        } else {
            constexpr int SHIFT = 64 - NK_BITS;
            auto tk = std::make_unique<NK[]>(count);
            for (size_t i = 0; i < count; ++i)
                tk[i] = static_cast<NK>(suf[i] >> SHIFT);
            return CO::make_leaf(tk.get(), vals,
                static_cast<uint32_t>(count), 0, nullptr, alloc);
        }
    }

    // Build from bit-63-aligned arrays. Returns tagged pointer.
    // bits: remaining key bits at this level.
    static uint64_t build_node_from_arrays_tagged_(uint64_t* suf, VST* vals,
                                                    size_t count, int bits,
                                                    ALLOC& alloc) {
        // Leaf case
        if (count <= COMPACT_MAX)
            return tag_leaf(build_leaf_(suf, vals, count, alloc));

        // Skip compression: all entries share same top byte?
        if (bits > 8) {
            uint8_t first_top = static_cast<uint8_t>(suf[0] >> 56);
            bool all_same = true;
            for (size_t i = 1; i < count; ++i)
                if (static_cast<uint8_t>(suf[i] >> 56) != first_top)
                    { all_same = false; break; }

            if (all_same) {
                for (size_t i = 0; i < count; ++i) suf[i] <<= 8;
                uint64_t child_tagged;
                if constexpr (NK_BITS > 8) {
                    if (bits - 8 <= static_cast<int>(NK_BITS / 2))
                        child_tagged = Narrow::build_node_from_arrays_tagged_(
                            suf, vals, count, bits - 8, alloc);
                    else
                        child_tagged = build_node_from_arrays_tagged_(
                            suf, vals, count, bits - 8, alloc);
                } else {
                    child_tagged = build_node_from_arrays_tagged_(
                        suf, vals, count, bits - 8, alloc);
                }
                uint8_t byte_arr[1] = {first_top};
                if (child_tagged & LEAF_BIT) {
                    uint64_t* leaf = untag_leaf_mut(child_tagged);
                    return tag_leaf(prepend_skip_(leaf, 1, byte_arr, alloc));
                } else {
                    uint64_t* bm_node = bm_to_node(child_tagged);
                    return BO::wrap_in_chain(bm_node, byte_arr, 1, alloc);
                }
            }
        }

        return build_bitmask_from_arrays_tagged_(suf, vals, count, bits, alloc);
    }

    // Groups by top byte, recurses, creates bitmask node.
    static uint64_t build_bitmask_from_arrays_tagged_(uint64_t* suf, VST* vals,
                                                       size_t count, int bits,
                                                       ALLOC& alloc) {
        uint8_t  indices[256];
        uint64_t child_tagged[256];
        uint16_t descs[256];
        int      n_children = 0;

        size_t i = 0;
        while (i < count) {
            uint8_t ti = static_cast<uint8_t>(suf[i] >> 56);
            size_t start = i;
            while (i < count && static_cast<uint8_t>(suf[i] >> 56) == ti) ++i;
            size_t cc = i - start;

            auto cs = std::make_unique<uint64_t[]>(cc);
            for (size_t j = 0; j < cc; ++j)
                cs[j] = suf[start + j] << 8;

            if constexpr (NK_BITS > 8) {
                if (bits - 8 <= static_cast<int>(NK_BITS / 2))
                    child_tagged[n_children] = Narrow::build_node_from_arrays_tagged_(
                        cs.get(), vals + start, cc, bits - 8, alloc);
                else
                    child_tagged[n_children] = build_node_from_arrays_tagged_(
                        cs.get(), vals + start, cc, bits - 8, alloc);
            } else {
                child_tagged[n_children] = build_node_from_arrays_tagged_(
                    cs.get(), vals + start, cc, bits - 8, alloc);
            }
            indices[n_children] = ti;
            descs[n_children] = cc > COMPACT_MAX ? COALESCE_CAP
                                                   : static_cast<uint16_t>(cc);
            n_children++;
        }

        auto* node = BO::make_bitmask(indices, child_tagged, n_children, alloc, descs);
        set_desc_capped_(node, count);
        return tag_bitmask(node);
    }

    // Convert overflowing compact leaf to bitmask tree.
    // suffix: NK-typed suffix, value: new value to insert.
    // bits: remaining key bits. Returns tagged pointer.
    static uint64_t convert_to_bitmask_tagged_(const uint64_t* node,
                                                const node_header* hdr,
                                                NK suffix, VST value,
                                                int bits, ALLOC& alloc) {
        uint16_t old_count = hdr->entries();
        size_t total = old_count + 1;
        auto wk = std::make_unique<uint64_t[]>(total);
        auto wv = std::make_unique<VST[]>(total);

        uint64_t new_suf = uint64_t(suffix) << (64 - NK_BITS);
        size_t wi = 0;
        bool ins = false;
        leaf_for_each_aligned_(node, hdr, [&](uint64_t s, VST v) {
            if (!ins && new_suf < s) {
                wk[wi] = new_suf; wv[wi] = value; wi++; ins = true;
            }
            wk[wi] = s; wv[wi] = v; wi++;
        });
        if (!ins) { wk[wi] = new_suf; wv[wi] = value; }

        uint64_t child_tagged = build_node_from_arrays_tagged_(
            wk.get(), wv.get(), total, bits, alloc);

        // Propagate old skip/prefix to new child
        uint8_t ps = hdr->skip();
        if (ps > 0) {
            const uint8_t* pfx = hdr->prefix_bytes();
            if (child_tagged & LEAF_BIT) {
                uint64_t* leaf = untag_leaf_mut(child_tagged);
                leaf = prepend_skip_(leaf, ps, pfx, alloc);
                child_tagged = tag_leaf(leaf);
            } else {
                uint64_t* bm_node = bm_to_node(child_tagged);
                child_tagged = BO::wrap_in_chain(bm_node, pfx, ps, alloc);
            }
        }

        dealloc_node(alloc, const_cast<uint64_t*>(node), hdr->alloc_u64());
        return child_tagged;
    }

    // ==================================================================
    // NK-independent helpers (shared across all NK specializations)
    // ==================================================================

    // Sum child descriptors with early-exit if > COMPACT_MAX
    static uint16_t sum_children_desc_(const uint64_t* node, uint8_t sc) noexcept {
        unsigned nc = get_header(node)->entries();
        if (nc > COMPACT_MAX) return COALESCE_CAP;
        const uint16_t* desc = BO::chain_desc_array(node, sc, nc);
        uint32_t total = 0;
        unsigned remaining = nc;
        for (unsigned i = 0; i < nc; ++i) {
            total += desc[i];
            --remaining;
            if (total + remaining > COMPACT_MAX) return COALESCE_CAP;
        }
        return static_cast<uint16_t>(total);
    }

    // Set descendants from known count (capped)
    static void set_desc_capped_(uint64_t* node, size_t count) noexcept {
        get_header(node)->set_descendants(
            count > COMPACT_MAX ? COALESCE_CAP : static_cast<uint16_t>(count));
    }

    // Capping increment (for insert path)
    static void inc_descendants_(node_header* h) noexcept {
        uint16_t d = h->descendants();
        if (d < COALESCE_CAP) h->set_descendants(d + 1);
    }

    // Decrement or recompute if capped. Returns new count (capped).
    static uint16_t dec_or_recompute_desc_(uint64_t* node, uint8_t sc) noexcept {
        auto* h = get_header(node);
        uint16_t d = h->descendants();
        if (d <= COMPACT_MAX) {
            --d;
            h->set_descendants(d);
            return d;
        }
        d = sum_children_desc_(node, sc);
        h->set_descendants(d);
        return d;
    }

    // ==================================================================
    // Skip prefix helpers
    // ==================================================================

    // Prepend skip bytes to an existing leaf. Returns raw (untagged) pointer.
    static uint64_t* prepend_skip_(uint64_t* node, uint8_t new_len,
                                    const uint8_t* new_bytes, ALLOC& alloc) {
        auto* h = get_header(node);
        uint8_t os = h->skip();
        uint8_t ns = os + new_len;

        uint8_t combined[6] = {};
        std::memcpy(combined, new_bytes, new_len);
        if (os > 0) std::memcpy(combined + new_len, h->prefix_bytes(), os);

        if (os > 0) {
            h->set_skip(ns);
            h->set_prefix(combined, ns);
            return node;
        }

        // No skip u64 — realloc with extra u64 and shift data right
        size_t old_au64 = h->alloc_u64();
        size_t new_au64 = old_au64 + 1;
        uint64_t* nn = alloc_node(alloc, new_au64);
        nn[0] = node[0];
        std::memcpy(nn + 2, node + 1, (old_au64 - 1) * 8);
        auto* nh = get_header(nn);
        nh->set_alloc_u64(new_au64);
        nh->set_skip(ns);
        nh->set_prefix(combined, ns);
        dealloc_node(alloc, node, old_au64);
        return nn;
    }

    // Strip the skip u64 from a leaf. Returns raw (untagged) pointer.
    static uint64_t* remove_skip_(uint64_t* node, ALLOC& alloc) {
        auto* h = get_header(node);
        size_t old_au64 = h->alloc_u64();
        size_t new_au64 = old_au64 - 1;
        uint64_t* nn = alloc_node(alloc, new_au64);
        nn[0] = node[0];
        get_header(nn)->set_skip(0);
        std::memcpy(nn + 1, node + 2, (old_au64 - 2) * 8);
        get_header(nn)->set_alloc_u64(new_au64);
        dealloc_node(alloc, node, old_au64);
        return nn;
    }

    // ==================================================================
    // Subtree deallocation (no value destruction)
    // ==================================================================

    static void dealloc_bitmask_subtree_(uint64_t tagged, ALLOC& alloc) noexcept {
        if (tagged & LEAF_BIT) {
            uint64_t* node = untag_leaf_mut(tagged);
            dealloc_node(alloc, node, get_header(node)->alloc_u64());
            return;
        }
        uint64_t* node = bm_to_node(tagged);
        auto* hdr = get_header(node);
        uint8_t sc = hdr->skip();
        BO::chain_for_each_child(node, sc, [&](unsigned, uint64_t child) {
            dealloc_bitmask_subtree_(child, alloc);
        });
        dealloc_node(alloc, node, hdr->alloc_u64());
    }
};

} // namespace gteitelbaum

#endif // KNTRIE_OPS_HPP
