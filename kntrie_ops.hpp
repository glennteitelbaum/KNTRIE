#ifndef KNTRIE_OPS_HPP
#define KNTRIE_OPS_HPP

#include "kntrie_bitmask.hpp"
#include "kntrie_compact.hpp"

namespace gteitelbaum {

// ======================================================================
// kntrie_ops<NK, VALUE, ALLOC> — stateless trie operations.
//
// NK = narrowed key type (u64/u32/u16/u8). As trie descent consumes
// bytes, NK narrows at half-width boundaries via the NARROW alias.
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
    using NARROW = kntrie_ops<NNK, VALUE, ALLOC>;

    // ==================================================================
    // Find — template<BITS> only, NK comes from struct
    // ==================================================================

    template<int BITS> requires (BITS > 8)
    static const VALUE* find_node(uint64_t ptr, NK ik) noexcept {
        if (ptr & LEAF_BIT) [[unlikely]] {
            const uint64_t* node = reinterpret_cast<const uint64_t*>(ptr ^ LEAF_BIT);
            node_header_t hdr = *get_header(node);
            return find_leaf<BITS>(node, hdr, ik,
                hdr.skip(), reinterpret_cast<const uint8_t*>(&node[1]));
        }

        const uint64_t* bm = reinterpret_cast<const uint64_t*>(ptr);
        uint8_t ti = static_cast<uint8_t>(ik >> (NK_BITS - 8));
        int slot = reinterpret_cast<const bitmap_256_t*>(bm)->
                       find_slot<slot_mode::BRANCHLESS>(ti);
        uint64_t child = bm[BITMAP_256_U64 + slot];

        if constexpr (BITS - 8 == NK_BITS / 2 && NK_BITS > 8) {
            return NARROW::template find_node<BITS - 8>(child,
                static_cast<NNK>(static_cast<NK>(ik << 8) >> (NK_BITS / 2)));
        } else {
            return find_node<BITS - 8>(child, static_cast<NK>(ik << 8));
        }
    }

    template<int BITS> requires (BITS == 8)
    static const VALUE* find_node(uint64_t ptr, NK ik) noexcept {
        const uint64_t* node = reinterpret_cast<const uint64_t*>(ptr ^ LEAF_BIT);
        node_header_t hdr = *get_header(node);
        return find_leaf<8>(node, hdr, ik, 0, nullptr);
    }

    template<int BITS> requires (BITS > 8)
    static const VALUE* find_leaf(const uint64_t* node, node_header_t hdr, NK ik,
                                   uint8_t skip, const uint8_t* prefix) noexcept {
        if (skip) [[unlikely]] {
            if (static_cast<uint8_t>(ik >> (NK_BITS - 8)) != *prefix) [[unlikely]]
                return nullptr;
            if constexpr (BITS - 8 == NK_BITS / 2 && NK_BITS > 8) {
                return NARROW::template find_leaf<BITS - 8>(node, hdr,
                    static_cast<NNK>(static_cast<NK>(ik << 8) >> (NK_BITS / 2)),
                    skip - 1, prefix + 1);
            } else {
                return find_leaf<BITS - 8>(node, hdr,
                    static_cast<NK>(ik << 8), skip - 1, prefix + 1);
            }
        }

        size_t hs = 1 + (hdr.skip() > 0);
        return CO::find(node, hdr, ik, hs);
    }

    template<int BITS> requires (BITS == 8)
    static const VALUE* find_leaf(const uint64_t* node, node_header_t hdr, NK ik,
                                   uint8_t, const uint8_t*) noexcept {
        return BO::bitmap_find(node, hdr,
            static_cast<uint8_t>(ik), 1 + (hdr.skip() > 0));
    }

    // ==================================================================
    // NK-dependent helpers (compile-time suffix_type dispatch)
    // ==================================================================

    // Create a 1-entry leaf. Returns raw (untagged) pointer.
    static uint64_t* make_single_leaf(NK suffix, VST value, ALLOC& alloc) {
        if constexpr (sizeof(NK) == 1) {
            return BO::make_single_bitmap(static_cast<uint8_t>(suffix), value, alloc);
        } else {
            return CO::make_leaf(&suffix, &value, 1, 0, nullptr, alloc);
        }
    }

    // Recursively descend `depth` bytes, narrowing NK at boundaries,
    // then create a single-entry leaf at the correct NK.
    // Returns raw (untagged) pointer. Caller tags.
    template<int BITS> requires (BITS >= 8)
    static uint64_t* make_leaf_descended(NK ik, VST value, uint8_t depth,
                                           ALLOC& alloc) {
        if (depth == 0) [[unlikely]]
            return make_single_leaf(ik, value, alloc);

        if constexpr (BITS > 8) {
            NK shifted = static_cast<NK>(ik << 8);
            if constexpr (BITS - 8 == NK_BITS / 2 && NK_BITS > 8) {
                return NARROW::template make_leaf_descended<BITS - 8>(
                    static_cast<NNK>(shifted >> (NK_BITS / 2)),
                    value, depth - 1, alloc);
            } else {
                return make_leaf_descended<BITS - 8>(
                    shifted, value, depth - 1, alloc);
            }
        }
        __builtin_unreachable();
    }

    // ==================================================================
    // split_on_prefix: leaf skip prefix diverges at position `common`.
    //
    // BITS: compile-time remaining key bits at this node's depth
    //       (BEFORE any prefix bytes were consumed).
    //       ik has already been shifted by `common` bytes by the caller's
    //       prefix-matching loop, so ik's top byte is the divergence byte.
    //
    // Returns: tagged uint64_t.
    // ==================================================================

    template<int BITS> requires (BITS >= 8)
    static uint64_t split_on_prefix(uint64_t* node, node_header_t* hdr,
                                      NK ik, VST value,
                                      const uint8_t* actual, uint8_t skip,
                                      uint8_t common, ALLOC& alloc) {
        uint8_t new_idx = static_cast<uint8_t>(ik >> (NK_BITS - 8));
        uint8_t old_idx = actual[common];
        uint8_t old_rem = skip - 1 - common;

        // Save common prefix before any reallocation invalidates `actual`
        uint8_t saved_prefix[6] = {};
        if (common > 0) [[unlikely]]
            std::memcpy(saved_prefix, actual, common);

        // Update old node: strip consumed prefix, keep remainder
        if (old_rem > 0) [[unlikely]] {
            uint8_t rem[6] = {};
            std::memcpy(rem, actual + common + 1, old_rem);
            hdr->set_skip(old_rem);
            hdr->set_prefix(rem, old_rem);
        } else {
            node = remove_skip(node, alloc);
            hdr = get_header(node);
        }

        // Extract prefix bytes from remaining ik (past divergence byte)
        uint8_t new_prefix[6] = {};
        {
            NK tmp = static_cast<NK>(ik << 8);
            for (uint8_t j = 0; j < old_rem; ++j) {
                new_prefix[j] = static_cast<uint8_t>(tmp >> (NK_BITS - 8));
                tmp = static_cast<NK>(tmp << 8);
            }
        }

        // Build new leaf at correct NK depth via recursive narrowing.
        // Consume old_rem bytes past the divergence byte.
        uint64_t* new_leaf;
        if constexpr (BITS > 8) {
            new_leaf = make_leaf_descended<BITS - 8>(
                static_cast<NK>(ik << 8), value, old_rem, alloc);
        } else {
            // BITS==8: no skip possible on 8-bit leaf, unreachable
            new_leaf = make_single_leaf(ik, value, alloc);
        }
        if (old_rem > 0) [[unlikely]]
            new_leaf = prepend_skip(new_leaf, old_rem, new_prefix, alloc);

        // Create parent bitmask with 2 children
        uint8_t   bi[2];
        uint64_t  cp[2];
        if (new_idx < old_idx) {
            bi[0] = new_idx; cp[0] = tag_leaf(new_leaf);
            bi[1] = old_idx; cp[1] = tag_leaf(node);
        } else {
            bi[0] = old_idx; cp[0] = tag_leaf(node);
            bi[1] = new_idx; cp[1] = tag_leaf(new_leaf);
        }

        uint64_t total = BO::exact_subtree_count(cp[0]) +
                         BO::exact_subtree_count(cp[1]);
        auto* bm_node = BO::make_bitmask(bi, cp, 2, alloc, total);
        if (common > 0) [[unlikely]]
            return BO::wrap_in_chain(bm_node, saved_prefix, common, alloc);
        return tag_bitmask(bm_node);
    }

    // ==================================================================
    // split_skip_at: key diverges at embed position `split_pos`
    //                 in a skip chain.
    //
    // BITS: compile-time remaining key bits (ik's top byte is the
    //       expected byte at embed split_pos).
    //
    // 1. Build new leaf for divergent key
    // 2. Build remainder from [split_pos+1..sc-1] + final bitmask
    // 3. Create 2-child bitmask at split point
    // 4. If split_pos > 0: wrap in skip chain for bytes [0..split_pos-1]
    // Returns: tagged bitmask pointer.
    // ==================================================================

    template<int BITS> requires (BITS >= 8)
    static uint64_t split_skip_at(uint64_t* node, node_header_t* hdr,
                                    uint8_t sc, uint8_t split_pos,
                                    NK ik, VST value, ALLOC& alloc) {
        uint8_t expected = static_cast<uint8_t>(ik >> (NK_BITS - 8));
        uint8_t actual_byte = BO::skip_byte(node, split_pos);

        // Build new leaf — one byte past the split point, with recursive narrowing
        uint64_t new_leaf_tagged;
        {
            NK shifted = static_cast<NK>(ik << 8);
            if constexpr (BITS - 8 == NK_BITS / 2 && NK_BITS > 8) {
                auto* leaf = NARROW::make_single_leaf(
                    static_cast<NNK>(shifted >> (NK_BITS / 2)), value, alloc);
                new_leaf_tagged = tag_leaf(leaf);
            } else {
                auto* leaf = make_single_leaf(shifted, value, alloc);
                new_leaf_tagged = tag_leaf(leaf);
            }
        }

        // Build remainder from [split_pos+1..sc-1] + final bitmask
        uint64_t remainder = BO::build_remainder(node, sc, split_pos + 1, alloc);

        // Create 2-child bitmask at split point
        uint8_t bi[2];
        uint64_t cp[2];
        if (expected < actual_byte) {
            bi[0] = expected;    cp[0] = new_leaf_tagged;
            bi[1] = actual_byte; cp[1] = remainder;
        } else {
            bi[0] = actual_byte; cp[0] = remainder;
            bi[1] = expected;    cp[1] = new_leaf_tagged;
        }
        uint64_t total = BO::exact_subtree_count(cp[0]) +
                         BO::exact_subtree_count(cp[1]);
        auto* split_node = BO::make_bitmask(bi, cp, 2, alloc, total);

        // Wrap in skip chain for prefix bytes [0..split_pos-1]
        uint64_t result;
        if (split_pos > 0) [[unlikely]] {
            uint8_t prefix_bytes[6];
            BO::skip_bytes(node, split_pos, prefix_bytes);
            result = BO::wrap_in_chain(split_node, prefix_bytes, split_pos, alloc);
        } else {
            result = tag_bitmask(split_node);
        }

        dealloc_node(alloc, node, hdr->alloc_u64());
        return result;
    }

    // Iterate leaf entries, callback receives (NK suffix, VST value)
    template<typename Fn>
    static void leaf_for_each(const uint64_t* node, const node_header_t* hdr,
                                Fn&& cb) {
        if constexpr (sizeof(NK) == 1) {
            BO::for_each_bitmap(node, [&](uint8_t s, VST v) {
                cb(static_cast<NK>(s), v);
            });
        } else {
            CO::for_each(node, hdr, std::forward<Fn>(cb));
        }
    }

    // Build leaf from NK-typed sorted arrays. Returns raw pointer.
    static uint64_t* build_leaf(NK* suf, VST* vals, size_t count, ALLOC& alloc) {
        if constexpr (sizeof(NK) == 1) {
            // BO expects uint8_t*
            return BO::make_bitmap_leaf(reinterpret_cast<uint8_t*>(suf), vals,
                static_cast<uint32_t>(count), alloc);
        } else {
            return CO::make_leaf(suf, vals,
                static_cast<uint32_t>(count), 0, nullptr, alloc);
        }
    }

    // Build from NK-typed sorted arrays. Returns tagged pointer.
    // bits: remaining key bits at this level.
    static uint64_t build_node_from_arrays_tagged(NK* suf, VST* vals,
                                                    size_t count, int bits,
                                                    ALLOC& alloc) {
        // Leaf case
        if (count <= COMPACT_MAX)
            return tag_leaf(build_leaf(suf, vals, count, alloc));

        // Skip compression: all entries share same top byte?
        if (bits > 8) {
            uint8_t first_top = static_cast<uint8_t>(suf[0] >> (NK_BITS - 8));
            bool all_same = true;
            for (size_t i = 1; i < count; ++i)
                if (static_cast<uint8_t>(suf[i] >> (NK_BITS - 8)) != first_top)
                    { all_same = false; break; }

            if (all_same) {
                // Strip top byte
                for (size_t i = 0; i < count; ++i)
                    suf[i] = static_cast<NK>(suf[i] << 8);
                uint64_t child_tagged;
                if constexpr (NK_BITS > 8) {
                    if (bits - 8 <= static_cast<int>(NK_BITS / 2)) {
                        // NARROW: extract NNK from bottom half
                        auto ns = std::make_unique<NNK[]>(count);
                        for (size_t i = 0; i < count; ++i)
                            ns[i] = static_cast<NNK>(suf[i] >> (NK_BITS / 2));
                        child_tagged = NARROW::build_node_from_arrays_tagged(
                            ns.get(), vals, count, bits - 8, alloc);
                    } else {
                        child_tagged = build_node_from_arrays_tagged(
                            suf, vals, count, bits - 8, alloc);
                    }
                } else {
                    child_tagged = build_node_from_arrays_tagged(
                        suf, vals, count, bits - 8, alloc);
                }
                uint8_t byte_arr[1] = {first_top};
                if (child_tagged & LEAF_BIT) {
                    uint64_t* leaf = untag_leaf_mut(child_tagged);
                    return tag_leaf(prepend_skip(leaf, 1, byte_arr, alloc));
                } else {
                    uint64_t* bm_node = bm_to_node(child_tagged);
                    return BO::wrap_in_chain(bm_node, byte_arr, 1, alloc);
                }
            }
        }

        return build_bitmask_from_arrays_tagged(suf, vals, count, bits, alloc);
    }

    // Groups by top byte, recurses, creates bitmask node.
    static uint64_t build_bitmask_from_arrays_tagged(NK* suf, VST* vals,
                                                       size_t count, int bits,
                                                       ALLOC& alloc) {
        uint8_t  indices[256];
        uint64_t child_tagged[256];
        int      n_children = 0;

        size_t i = 0;
        while (i < count) {
            uint8_t ti = static_cast<uint8_t>(suf[i] >> (NK_BITS - 8));
            size_t start = i;
            while (i < count && static_cast<uint8_t>(suf[i] >> (NK_BITS - 8)) == ti) ++i;
            size_t cc = i - start;

            if constexpr (NK_BITS > 8) {
                if (bits - 8 <= static_cast<int>(NK_BITS / 2)) {
                    // NARROW: extract NNK from shifted entries
                    auto cs = std::make_unique<NNK[]>(cc);
                    for (size_t j = 0; j < cc; ++j)
                        cs[j] = static_cast<NNK>(
                            static_cast<NK>(suf[start + j] << 8) >> (NK_BITS / 2));
                    child_tagged[n_children] = NARROW::build_node_from_arrays_tagged(
                        cs.get(), vals + start, cc, bits - 8, alloc);
                } else {
                    auto cs = std::make_unique<NK[]>(cc);
                    for (size_t j = 0; j < cc; ++j)
                        cs[j] = static_cast<NK>(suf[start + j] << 8);
                    child_tagged[n_children] = build_node_from_arrays_tagged(
                        cs.get(), vals + start, cc, bits - 8, alloc);
                }
            } else {
                auto cs = std::make_unique<NK[]>(cc);
                for (size_t j = 0; j < cc; ++j)
                    cs[j] = static_cast<NK>(suf[start + j] << 8);
                child_tagged[n_children] = build_node_from_arrays_tagged(
                    cs.get(), vals + start, cc, bits - 8, alloc);
            }
            indices[n_children] = ti;
            n_children++;
        }

        return tag_bitmask(
            BO::make_bitmask(indices, child_tagged, n_children, alloc, count));
    }

    // Convert overflowing compact leaf to bitmask tree.
    // suffix: NK-typed suffix, value: new value to insert.
    // bits: remaining key bits. Returns tagged pointer.
    static uint64_t convert_to_bitmask_tagged(const uint64_t* node,
                                                const node_header_t* hdr,
                                                NK suffix, VST value,
                                                int bits, ALLOC& alloc) {
        uint16_t old_count = hdr->entries();
        size_t total = old_count + 1;
        auto wk = std::make_unique<NK[]>(total);
        auto wv = std::make_unique<VST[]>(total);

        size_t wi = 0;
        bool ins = false;
        leaf_for_each(node, hdr, [&](NK s, VST v) {
            if (!ins && suffix < s) {
                wk[wi] = suffix; wv[wi] = value; wi++; ins = true;
            }
            wk[wi] = s; wv[wi] = v; wi++;
        });
        if (!ins) { wk[wi] = suffix; wv[wi] = value; }

        uint64_t child_tagged = build_node_from_arrays_tagged(
            wk.get(), wv.get(), total, bits, alloc);

        // Propagate old skip/prefix to new child
        uint8_t ps = hdr->skip();
        if (ps > 0) {
            const uint8_t* pfx = hdr->prefix_bytes();
            if (child_tagged & LEAF_BIT) {
                uint64_t* leaf = untag_leaf_mut(child_tagged);
                leaf = prepend_skip(leaf, ps, pfx, alloc);
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
    // Insert — template<BITS, INSERT, ASSIGN>, NK from struct
    //
    // BITS: compile-time remaining key bits at this node's depth.
    // ik: NK-typed key shifted so next byte is at (NK_BITS - 8).
    // ==================================================================

    template<int BITS, bool INSERT, bool ASSIGN> requires (BITS >= 8)
    static insert_result_t insert_node(uint64_t ptr, NK ik, VST value,
                                         ALLOC& alloc) {
        // SENTINEL
        if (ptr == SENTINEL_TAGGED) [[unlikely]] {
            if constexpr (!INSERT) return {ptr, false, false};
            return {tag_leaf(make_single_leaf(ik, value, alloc)), true, false};
        }

        // LEAF
        if (ptr & LEAF_BIT) [[unlikely]] {
            uint64_t* node = untag_leaf_mut(ptr);
            auto* hdr = get_header(node);

            uint8_t skip = hdr->skip();
            if (skip) [[unlikely]] {
                const uint8_t* actual = hdr->prefix_bytes();
                return insert_leaf_skip<BITS, INSERT, ASSIGN>(
                    node, hdr, ik, value, actual, skip, 0, alloc);
            }

            return leaf_insert<BITS, INSERT, ASSIGN>(node, hdr, ik, value, alloc);
        }

        // BITMASK
        uint64_t* node = bm_to_node(ptr);
        auto* hdr = get_header(node);
        uint8_t sc = hdr->skip();

        if (sc > 0) [[unlikely]]
            return insert_chain_skip<BITS, INSERT, ASSIGN>(
                node, hdr, sc, ik, value, 0, alloc);

        return insert_final_bitmask<BITS, INSERT, ASSIGN>(
            node, hdr, 0, ik, value, alloc);
    }

    // --- Leaf skip prefix: recursive byte-at-a-time with narrowing ---
    template<int BITS, bool INSERT, bool ASSIGN> requires (BITS >= 8)
    static insert_result_t insert_leaf_skip(
            uint64_t* node, node_header_t* hdr,
            NK ik, VST value,
            const uint8_t* actual, uint8_t skip, uint8_t pos,
            ALLOC& alloc) {
        if (pos >= skip) [[unlikely]]
            return leaf_insert<BITS, INSERT, ASSIGN>(node, hdr, ik, value, alloc);

        uint8_t expected = static_cast<uint8_t>(ik >> (NK_BITS - 8));
        if (expected != actual[pos]) [[unlikely]] {
            if constexpr (!INSERT) return {tag_leaf(node), false, false};
            return {split_on_prefix<BITS>(node, hdr, ik, value,
                                            actual, skip, pos, alloc), true, false};
        }

        // Match — consume byte, narrow at boundary
        if constexpr (BITS > 8) {
            NK shifted = static_cast<NK>(ik << 8);
            if constexpr (BITS - 8 == NK_BITS / 2 && NK_BITS > 8) {
                return NARROW::template insert_leaf_skip<BITS - 8, INSERT, ASSIGN>(
                    node, hdr,
                    static_cast<NNK>(shifted >> (NK_BITS / 2)),
                    value, actual, skip, pos + 1, alloc);
            } else {
                return insert_leaf_skip<BITS - 8, INSERT, ASSIGN>(
                    node, hdr, shifted, value, actual, skip, pos + 1, alloc);
            }
        }
        __builtin_unreachable();
    }

    // --- Leaf insert: compile-time NK dispatch ---
    template<int BITS, bool INSERT, bool ASSIGN>
    static insert_result_t leaf_insert(uint64_t* node, node_header_t* hdr,
                                         NK ik, VST value, ALLOC& alloc) {
        insert_result_t result;
        if constexpr (sizeof(NK) == 1) {
            result = BO::template bitmap_insert<INSERT, ASSIGN>(
                node, static_cast<uint8_t>(ik), value, alloc);
        } else {
            result = CO::template insert<INSERT, ASSIGN>(
                node, hdr, ik, value, alloc);
        }
        if (result.needs_split) [[unlikely]] {
            if constexpr (!INSERT) return {tag_leaf(node), false, false};
            return {convert_to_bitmask_tagged(node, hdr, ik, value, BITS, alloc),
                    true, false};
        }
        return result;
    }

    // --- Skip chain: recursive embed walk with narrowing ---
    template<int BITS, bool INSERT, bool ASSIGN> requires (BITS >= 8)
    static insert_result_t insert_chain_skip(
            uint64_t* node, node_header_t* hdr,
            uint8_t sc, NK ik, VST value, uint8_t pos,
            ALLOC& alloc) {
        if (pos >= sc) [[unlikely]]
            return insert_final_bitmask<BITS, INSERT, ASSIGN>(
                node, hdr, sc, ik, value, alloc);

        uint8_t actual_byte = BO::skip_byte(node, pos);
        uint8_t expected = static_cast<uint8_t>(ik >> (NK_BITS - 8));

        if (expected != actual_byte) [[unlikely]] {
            if constexpr (!INSERT) return {tag_bitmask(node), false, false};
            return {split_skip_at<BITS>(node, hdr, sc, pos, ik, value, alloc),
                    true, false};
        }

        // Match — consume byte, narrow at boundary
        if constexpr (BITS > 8) {
            NK shifted = static_cast<NK>(ik << 8);
            if constexpr (BITS - 8 == NK_BITS / 2 && NK_BITS > 8) {
                return NARROW::template insert_chain_skip<BITS - 8, INSERT, ASSIGN>(
                    node, hdr, sc,
                    static_cast<NNK>(shifted >> (NK_BITS / 2)),
                    value, pos + 1, alloc);
            } else {
                return insert_chain_skip<BITS - 8, INSERT, ASSIGN>(
                    node, hdr, sc, shifted, value, pos + 1, alloc);
            }
        }
        __builtin_unreachable();
    }

    // --- Final bitmask: lookup + recurse with narrowing ---
    template<int BITS, bool INSERT, bool ASSIGN> requires (BITS >= 8)
    static insert_result_t insert_final_bitmask(
            uint64_t* node, node_header_t* hdr,
            uint8_t sc, NK ik, VST value, ALLOC& alloc) {
        uint8_t ti = static_cast<uint8_t>(ik >> (NK_BITS - 8));

        typename BO::child_lookup cl;
        if (sc > 0) [[unlikely]]
            cl = BO::chain_lookup(node, sc, ti);
        else
            cl = BO::lookup(node, ti);

        if (!cl.found) [[unlikely]] {
            if constexpr (!INSERT) return {tag_bitmask(node), false, false};

            uint64_t* leaf;
            if constexpr (BITS > 8) {
                NK shifted = static_cast<NK>(ik << 8);
                if constexpr (BITS - 8 == NK_BITS / 2 && NK_BITS > 8)
                    leaf = NARROW::make_single_leaf(
                        static_cast<NNK>(shifted >> (NK_BITS / 2)), value, alloc);
                else
                    leaf = make_single_leaf(shifted, value, alloc);
            } else {
                // BITS==8: no child to create — shouldn't reach here
                // (bitmap leaf handles 8-bit case)
                __builtin_unreachable();
            }

            uint64_t* nn;
            if (sc > 0) [[unlikely]]
                nn = BO::chain_add_child(node, hdr, sc, ti, tag_leaf(leaf), alloc);
            else
                nn = BO::add_child(node, hdr, ti, tag_leaf(leaf), alloc);
            inc_descendants(nn, get_header(nn));
            return {tag_bitmask(nn), true, false};
        }

        // Found — recurse into child
        if constexpr (BITS > 8) {
            NK shifted = static_cast<NK>(ik << 8);
            insert_result_t cr;
            if constexpr (BITS - 8 == NK_BITS / 2 && NK_BITS > 8) {
                cr = NARROW::template insert_node<BITS - 8, INSERT, ASSIGN>(
                    cl.child,
                    static_cast<NNK>(shifted >> (NK_BITS / 2)),
                    value, alloc);
            } else {
                cr = insert_node<BITS - 8, INSERT, ASSIGN>(
                    cl.child, shifted, value, alloc);
            }

            if (cr.tagged_ptr != cl.child) {
                if (sc > 0) [[unlikely]]
                    BO::chain_set_child(node, sc, cl.slot, cr.tagged_ptr);
                else
                    BO::set_child(node, cl.slot, cr.tagged_ptr);
            }
            if (cr.inserted)
                inc_descendants(node, hdr);
            return {tag_bitmask(node), cr.inserted, false};
        }
        __builtin_unreachable();
    }

    // ==================================================================
    // Erase — template<BITS>, NK from struct
    //
    // Coalesce handled inline via do_coalesce<BITS>.
    // ==================================================================

    template<int BITS> requires (BITS >= 8)
    static erase_result_t erase_node(uint64_t ptr, NK ik, ALLOC& alloc) {
        if (ptr == SENTINEL_TAGGED) return {ptr, false, 0};

        if (ptr & LEAF_BIT) [[unlikely]] {
            uint64_t* node = untag_leaf_mut(ptr);
            auto* hdr = get_header(node);

            uint8_t skip = hdr->skip();
            if (skip) [[unlikely]] {
                const uint8_t* actual = hdr->prefix_bytes();
                return erase_leaf_skip<BITS>(
                    node, hdr, ik, actual, skip, 0, alloc);
            }

            return leaf_erase(node, hdr, ik, alloc);
        }

        uint64_t* node = bm_to_node(ptr);
        auto* hdr = get_header(node);
        uint8_t sc = hdr->skip();

        if (sc > 0) [[unlikely]]
            return erase_chain_skip<BITS>(
                node, hdr, sc, ik, 0, alloc);

        return erase_final_bitmask<BITS>(
            node, hdr, 0, ik, alloc);
    }

    // --- Leaf skip prefix: recursive byte-at-a-time with narrowing ---
    template<int BITS> requires (BITS >= 8)
    static erase_result_t erase_leaf_skip(
            uint64_t* node, node_header_t* hdr,
            NK ik, const uint8_t* actual, uint8_t skip, uint8_t pos,
            ALLOC& alloc) {
        if (pos >= skip) [[unlikely]]
            return leaf_erase(node, hdr, ik, alloc);

        uint8_t expected = static_cast<uint8_t>(ik >> (NK_BITS - 8));
        if (expected != actual[pos]) [[unlikely]]
            return {tag_leaf(node), false, 0};

        if constexpr (BITS > 8) {
            NK shifted = static_cast<NK>(ik << 8);
            if constexpr (BITS - 8 == NK_BITS / 2 && NK_BITS > 8) {
                return NARROW::template erase_leaf_skip<BITS - 8>(
                    node, hdr,
                    static_cast<NNK>(shifted >> (NK_BITS / 2)),
                    actual, skip, pos + 1, alloc);
            } else {
                return erase_leaf_skip<BITS - 8>(
                    node, hdr, shifted, actual, skip, pos + 1, alloc);
            }
        }
        __builtin_unreachable();
    }

    // --- Leaf erase: compile-time NK dispatch ---
    static erase_result_t leaf_erase(uint64_t* node, node_header_t* hdr,
                                       NK ik, ALLOC& alloc) {
        if constexpr (sizeof(NK) == 1) {
            return BO::bitmap_erase(node, static_cast<uint8_t>(ik), alloc);
        } else {
            return CO::erase(node, hdr, ik, alloc);
        }
    }

    // --- Skip chain: recursive embed walk with narrowing ---
    template<int BITS> requires (BITS >= 8)
    static erase_result_t erase_chain_skip(
            uint64_t* node, node_header_t* hdr,
            uint8_t sc, NK ik, uint8_t pos,
            ALLOC& alloc) {
        if (pos >= sc) [[unlikely]]
            return erase_final_bitmask<BITS>(
                node, hdr, sc, ik, alloc);

        uint8_t actual_byte = BO::skip_byte(node, pos);
        uint8_t expected = static_cast<uint8_t>(ik >> (NK_BITS - 8));

        if (expected != actual_byte) [[unlikely]]
            return {tag_bitmask(node), false, 0};

        if constexpr (BITS > 8) {
            NK shifted = static_cast<NK>(ik << 8);
            if constexpr (BITS - 8 == NK_BITS / 2 && NK_BITS > 8) {
                return NARROW::template erase_chain_skip<BITS - 8>(
                    node, hdr, sc,
                    static_cast<NNK>(shifted >> (NK_BITS / 2)),
                    pos + 1, alloc);
            } else {
                return erase_chain_skip<BITS - 8>(
                    node, hdr, sc, shifted, pos + 1, alloc);
            }
        }
        __builtin_unreachable();
    }

    // --- Final bitmask: lookup + recurse + coalesce check ---
    template<int BITS> requires (BITS >= 8)
    static erase_result_t erase_final_bitmask(
            uint64_t* node, node_header_t* hdr,
            uint8_t sc, NK ik, ALLOC& alloc) {
        uint8_t ti = static_cast<uint8_t>(ik >> (NK_BITS - 8));

        typename BO::child_lookup cl;
        if (sc > 0) [[unlikely]]
            cl = BO::chain_lookup(node, sc, ti);
        else
            cl = BO::lookup(node, ti);

        if (!cl.found) [[unlikely]] return {tag_bitmask(node), false, 0};

        // Recurse into child
        erase_result_t cr;
        if constexpr (BITS > 8) {
            NK shifted = static_cast<NK>(ik << 8);
            if constexpr (BITS - 8 == NK_BITS / 2 && NK_BITS > 8) {
                cr = NARROW::template erase_node<BITS - 8>(
                    cl.child,
                    static_cast<NNK>(shifted >> (NK_BITS / 2)),
                    alloc);
            } else {
                cr = erase_node<BITS - 8>(
                    cl.child, shifted, alloc);
            }
        } else {
            __builtin_unreachable();
        }

        if (!cr.erased) [[unlikely]] return {tag_bitmask(node), false, 0};

        if (cr.tagged_ptr) [[likely]] {
            // Child survived
            if (cr.tagged_ptr != cl.child) {
                if (sc > 0) [[unlikely]]
                    BO::chain_set_child(node, sc, cl.slot, cr.tagged_ptr);
                else
                    BO::set_child(node, cl.slot, cr.tagged_ptr);
            }
            // Decrement descendants and check coalesce
            uint64_t exact = dec_descendants(node, hdr);
            if (exact <= COMPACT_MAX) [[unlikely]]
                return do_coalesce<BITS>(node, hdr, alloc);
            return {tag_bitmask(node), true, exact};
        }

        // Child fully erased — remove from bitmask
        uint64_t* nn;
        if (sc > 0) [[unlikely]]
            nn = BO::chain_remove_child(node, hdr, sc, cl.slot, ti, alloc);
        else
            nn = BO::remove_child(node, hdr, cl.slot, ti, alloc);
        if (!nn) [[unlikely]] return {0, true, 0};

        hdr = get_header(nn);
        unsigned nc = hdr->entries();

        // Decrement the preserved descendants count
        uint64_t exact = dec_descendants(nn, hdr);

        // Collapse when final bitmask drops to 1 child
        if (nc == 1) [[unlikely]] {
            typename BO::collapse_info ci;
            if (sc > 0) [[unlikely]]
                ci = BO::chain_collapse_info(nn, sc);
            else
                ci = BO::standalone_collapse_info(nn);
            size_t nn_au64 = hdr->alloc_u64();

            if (ci.sole_child & LEAF_BIT) {
                uint64_t* leaf = untag_leaf_mut(ci.sole_child);
                leaf = prepend_skip(leaf, ci.total_skip, ci.bytes, alloc);
                dealloc_node(alloc, nn, nn_au64);
                return {tag_leaf(leaf), true, exact};
            }
            uint64_t* child_node = bm_to_node(ci.sole_child);
            dealloc_node(alloc, nn, nn_au64);
            return {BO::wrap_in_chain(child_node, ci.bytes, ci.total_skip, alloc),
                    true, exact};
        }

        // Multi-child: check coalesce
        if (exact <= COMPACT_MAX) [[unlikely]]
            return do_coalesce<BITS>(nn, get_header(nn), alloc);
        return {tag_bitmask(nn), true, exact};
    }


    // ==================================================================
    // Collect entries — each level returns NK-typed arrays
    //
    // Returns {keys, vals, count} where keys are NK-typed suffixes
    // covering exactly BITS bits from this node's perspective.
    // Parent widens child results at narrowing boundaries.
    // ==================================================================

    struct collected_t {
        std::unique_ptr<NK[]> keys;
        std::unique_ptr<VST[]> vals;
        size_t count;
    };

    template<int BITS> requires (BITS >= 8)
    static collected_t collect_entries(uint64_t tagged) {
        if (tagged & LEAF_BIT) [[unlikely]] {
            const uint64_t* node = untag_leaf(tagged);
            auto* hdr = get_header(node);
            uint8_t skip = hdr->skip();
            if (skip) [[unlikely]]
                return collect_leaf_skip<BITS>(node, hdr, hdr->prefix_bytes(), skip, 0);
            return collect_leaf<BITS>(node, hdr);
        }

        const uint64_t* node = bm_to_node_const(tagged);
        auto* hdr = get_header(node);
        uint8_t sc = hdr->skip();
        if (sc > 0) [[unlikely]]
            return collect_bm_skip<BITS>(node, sc, 0);
        return collect_bm_final<BITS>(node, 0);
    }

    // --- Leaf body: direct NK extraction ---
    template<int BITS> requires (BITS >= 8)
    static collected_t collect_leaf(const uint64_t* node, const node_header_t* hdr) {
        size_t n = hdr->entries();
        auto wk = std::make_unique<NK[]>(n);
        auto wv = std::make_unique<VST[]>(n);
        size_t wi = 0;
        leaf_for_each(node, hdr, [&](NK s, VST v) {
            wk[wi] = s; wv[wi] = v; wi++;
        });
        return {std::move(wk), std::move(wv), wi};
    }

    // --- Leaf skip: consume prefix bytes, narrow, widen on return ---
    template<int BITS> requires (BITS >= 8)
    static collected_t collect_leaf_skip(const uint64_t* node, const node_header_t* hdr,
                                           const uint8_t* pb, uint8_t skip, uint8_t pos) {
        if (pos >= skip) [[unlikely]]
            return collect_leaf<BITS>(node, hdr);

        if constexpr (BITS > 8) {
            uint8_t byte = pb[pos];
            if constexpr (BITS - 8 == NK_BITS / 2 && NK_BITS > 8) {
                auto child = NARROW::template collect_leaf_skip<BITS - 8>(
                    node, hdr, pb, skip, pos + 1);
                return widen_prepend(byte, BITS - 8, std::move(child));
            } else {
                auto child = collect_leaf_skip<BITS - 8>(
                    node, hdr, pb, skip, pos + 1);
                return prepend(byte, BITS - 8, std::move(child));
            }
        }
        __builtin_unreachable();
    }

    // --- BM skip: consume embed bytes, narrow, widen on return ---
    template<int BITS> requires (BITS >= 8)
    static collected_t collect_bm_skip(const uint64_t* node, uint8_t sc, uint8_t pos) {
        if (pos >= sc) [[unlikely]]
            return collect_bm_final<BITS>(node, sc);

        if constexpr (BITS > 8) {
            uint8_t byte = BO::skip_byte(node, pos);
            if constexpr (BITS - 8 == NK_BITS / 2 && NK_BITS > 8) {
                auto child = NARROW::template collect_bm_skip<BITS - 8>(
                    node, sc, pos + 1);
                return widen_prepend(byte, BITS - 8, std::move(child));
            } else {
                auto child = collect_bm_skip<BITS - 8>(
                    node, sc, pos + 1);
                return prepend(byte, BITS - 8, std::move(child));
            }
        }
        __builtin_unreachable();
    }

    // --- BM final: iterate children, collect each, merge ---
    template<int BITS> requires (BITS >= 8)
    static collected_t collect_bm_final(const uint64_t* node, uint8_t sc) {
        auto* hdr = get_header(node);
        uint64_t total = BO::chain_descendants(node, sc, hdr->entries());

        auto wk = std::make_unique<NK[]>(total);
        auto wv = std::make_unique<VST[]>(total);
        size_t wi = 0;

        const bitmap_256_t& fbm = BO::chain_bitmap(node, sc);
        const uint64_t* rch = BO::chain_children(node, sc);

        fbm.for_each_set([&](uint8_t idx, int slot) {
            if constexpr (BITS > 8) {
                if constexpr (BITS - 8 == NK_BITS / 2 && NK_BITS > 8) {
                    auto child = NARROW::template collect_entries<BITS - 8>(rch[slot]);
                    for (size_t i = 0; i < child.count; ++i) {
                        wk[wi] = (NK(idx) << (BITS - 8)) | NK(child.keys[i]);
                        wv[wi] = child.vals[i];
                        wi++;
                    }
                } else {
                    auto child = collect_entries<BITS - 8>(rch[slot]);
                    for (size_t i = 0; i < child.count; ++i) {
                        wk[wi] = (NK(idx) << (BITS - 8)) | child.keys[i];
                        wv[wi] = child.vals[i];
                        wi++;
                    }
                }
            }
        });
        return {std::move(wk), std::move(wv), wi};
    }

    // --- Prepend byte to same-NK results ---
    static collected_t prepend(uint8_t byte, int child_bits, collected_t child) {
        for (size_t i = 0; i < child.count; ++i)
            child.keys[i] = (NK(byte) << child_bits) | child.keys[i];
        return child;
    }

    // --- Widen NNK results to NK, prepending byte ---
    static collected_t widen_prepend(uint8_t byte, int child_bits,
                                       typename NARROW::collected_t child) {
        auto wk = std::make_unique<NK[]>(child.count);
        auto wv = std::make_unique<VST[]>(child.count);
        for (size_t i = 0; i < child.count; ++i) {
            wk[i] = (NK(byte) << child_bits) | NK(child.keys[i]);
            wv[i] = child.vals[i];
        }
        return {std::move(wk), std::move(wv), child.count};
    }

    // ==================================================================
    // do_coalesce — collect entries + build leaf at correct NK depth
    //
    // BITS: compile-time bits at the bitmask being coalesced.
    // Collects at BITS, strips skip bytes, narrows to find build depth.
    // ==================================================================

    template<int BITS> requires (BITS >= 8)
    static erase_result_t do_coalesce(uint64_t* node, node_header_t* hdr,
                                        ALLOC& alloc) {
        uint8_t sc = hdr->skip();
        uint64_t tagged = tag_bitmask(node);

        auto c = collect_entries<BITS>(tagged);

        // Strip skip bytes: shift left so suffixes are at (BITS - sc*8) level
        if (sc > 0) [[unlikely]] {
            unsigned shift = sc * 8;
            for (size_t i = 0; i < c.count; ++i)
                c.keys[i] = static_cast<NK>(c.keys[i] << shift);
        }

        // Build leaf: narrow through skip bytes to find correct NK
        uint64_t* leaf = coalesce_build_skip<BITS>(
            sc, 0, c.keys.get(), c.vals.get(), c.count, alloc);

        if (sc > 0) [[unlikely]] {
            uint8_t sb[6];
            BO::skip_bytes(node, sc, sb);
            leaf = prepend_skip(leaf, sc, sb, alloc);
        }

        dealloc_bitmask_subtree<BITS>(tagged, alloc);
        return {tag_leaf(leaf), true, c.count};
    }

    // --- NARROW through skip bytes to find correct NK for build_leaf ---
    template<int BITS> requires (BITS >= 8)
    static uint64_t* coalesce_build_skip(uint8_t sc, uint8_t pos,
                                            NK* wk, VST* wv,
                                            size_t count, ALLOC& alloc) {
        if (pos >= sc) [[unlikely]]
            return build_leaf(wk, wv, count, alloc);

        if constexpr (BITS > 8) {
            if constexpr (BITS - 8 == NK_BITS / 2 && NK_BITS > 8) {
                auto ns = std::make_unique<NNK[]>(count);
                for (size_t i = 0; i < count; ++i)
                    ns[i] = static_cast<NNK>(wk[i] >> (NK_BITS / 2));
                return NARROW::template coalesce_build_skip<BITS - 8>(
                    sc, pos + 1, ns.get(), wv, count, alloc);
            } else {
                return coalesce_build_skip<BITS - 8>(
                    sc, pos + 1, wk, wv, count, alloc);
            }
        }
        __builtin_unreachable();
    }



    // ==================================================================
    // NK-independent helpers (shared across all NK specializations)
    // ==================================================================

    // Increment descendants count (for insert path)
    static void inc_descendants(uint64_t* node, node_header_t* hdr) noexcept {
        BO::chain_descendants_mut(node, hdr->skip(), hdr->entries())++;
    }

    // Decrement descendants count, return new value (for erase path)
    static uint64_t dec_descendants(uint64_t* node, node_header_t* hdr) noexcept {
        uint64_t& d = BO::chain_descendants_mut(node, hdr->skip(), hdr->entries());
        return --d;
    }

    // ==================================================================
    // Skip prefix helpers
    // ==================================================================

    // Prepend skip bytes to an existing leaf. Returns raw (untagged) pointer.
    static uint64_t* prepend_skip(uint64_t* node, uint8_t new_len,
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
    static uint64_t* remove_skip(uint64_t* node, ALLOC& alloc) {
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
    // Subtree deallocation (for coalesce: values already collected)
    // B: destroy values (copies were made). C: do NOT destroy (pointers inherited).
    // ==================================================================

    template<int BITS> requires (BITS >= 8)
    static void dealloc_bitmask_subtree(uint64_t tagged, ALLOC& alloc) noexcept {
        if (tagged & LEAF_BIT) [[unlikely]] {
            uint64_t* node = untag_leaf_mut(tagged);
            auto* hdr = get_header(node);
            // B type: old leaf values are copies — must destroy before dealloc.
            // C type: new leaf inherited the pointers — do NOT destroy.
            // A type: noop.
            if constexpr (VT::IS_INLINE && VT::HAS_DESTRUCTOR) {
                uint8_t skip = hdr->skip();
                if (skip) [[unlikely]] {
                    dealloc_leaf_skip<BITS>(node, skip, alloc);
                } else {
                    if constexpr (sizeof(NK) == 1)
                        BO::bitmap_destroy_and_dealloc(node, alloc);
                    else
                        CO::destroy_and_dealloc(node, alloc);
                }
            } else {
                dealloc_node(alloc, node, hdr->alloc_u64());
            }
            return;
        }
        uint64_t* node = bm_to_node(tagged);
        auto* hdr = get_header(node);
        uint8_t sc = hdr->skip();

        // Recurse children with narrowing through skip chain
        if (sc > 0) [[unlikely]] {
            dealloc_bm_chain_skip<BITS>(node, sc, 0, alloc);
        } else {
            dealloc_bm_final<BITS>(node, sc, alloc);
        }
        dealloc_node(alloc, node, hdr->alloc_u64());
    }

    // Leaf skip: narrow NK through prefix bytes then destroy
    template<int BITS> requires (BITS >= 8)
    static void dealloc_leaf_skip(uint64_t* node, uint8_t skip,
                                    ALLOC& alloc) noexcept {
        if (skip == 0) [[unlikely]] {
            if constexpr (sizeof(NK) == 1)
                BO::bitmap_destroy_and_dealloc(node, alloc);
            else
                CO::destroy_and_dealloc(node, alloc);
            return;
        }
        if constexpr (BITS > 8) {
            if constexpr (BITS - 8 == NK_BITS / 2 && NK_BITS > 8)
                NARROW::template dealloc_leaf_skip<BITS - 8>(node, skip - 1, alloc);
            else
                dealloc_leaf_skip<BITS - 8>(node, skip - 1, alloc);
        }
    }

    // Chain skip: narrow through embed bytes
    template<int BITS> requires (BITS >= 8)
    static void dealloc_bm_chain_skip(uint64_t* node, uint8_t sc, uint8_t pos,
                                        ALLOC& alloc) noexcept {
        if (pos >= sc) [[unlikely]] {
            dealloc_bm_final<BITS>(node, sc, alloc);
            return;
        }
        if constexpr (BITS > 8) {
            if constexpr (BITS - 8 == NK_BITS / 2 && NK_BITS > 8)
                NARROW::template dealloc_bm_chain_skip<BITS - 8>(node, sc, pos + 1, alloc);
            else
                dealloc_bm_chain_skip<BITS - 8>(node, sc, pos + 1, alloc);
        }
    }

    // Final bitmap: recurse each child with narrowing
    template<int BITS> requires (BITS >= 8)
    static void dealloc_bm_final(uint64_t* node, uint8_t sc,
                                   ALLOC& alloc) noexcept {
        BO::chain_for_each_child(node, sc, [&](unsigned, uint64_t child) {
            if constexpr (BITS > 8) {
                if constexpr (BITS - 8 == NK_BITS / 2 && NK_BITS > 8)
                    NARROW::template dealloc_bitmask_subtree<BITS - 8>(child, alloc);
                else
                    dealloc_bitmask_subtree<BITS - 8>(child, alloc);
            }
        });
    }

};

} // namespace gteitelbaum

#endif // KNTRIE_OPS_HPP
