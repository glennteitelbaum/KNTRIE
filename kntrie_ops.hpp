#ifndef KNTRIE_OPS_HPP
#define KNTRIE_OPS_HPP

#include "kntrie_bitmask.hpp"
#include "kntrie_compact.hpp"

#include <array>
#include <bit>

namespace gteitelbaum {

// ======================================================================
// kntrie_ops<VALUE, ALLOC> — stateless trie operations.
//
// All functions take uint64_t ik (left-aligned, byte 0 at bits 63..56).
// BITS is the compile-time remaining key bits.
// NK narrowing eliminated — NK only at leaf storage boundary.
// ======================================================================

template<typename VALUE, typename ALLOC>
struct kntrie_ops {
    using BO  = bitmask_ops<VALUE, ALLOC>;
    using VT  = value_traits<VALUE, ALLOC>;
    using VST = typename VT::slot_type;
    using BLD = builder<VALUE, VT::IS_TRIVIAL, ALLOC>;

    using leaf_fn_t     = typename BO::leaf_fn_t;
    using leaf_result_t = typename BO::leaf_result_t;

    // ==================================================================
    // leaf_ops_t<BITS> — fn pointer array indexed by skip
    // ==================================================================

    template<int BITS>
    struct leaf_ops_t {
        static constexpr int MAX_LEAF_SKIP = (BITS - 8) / 8;

        // --- Narrow to storage NK at leaf boundary ---
        template<int REMAINING>
        static auto to_suffix(uint64_t ik) noexcept {
            using SNK = nk_for_bits_t<REMAINING>;
            constexpr int SNK_BITS = static_cast<int>(sizeof(SNK) * 8);
            return static_cast<SNK>(ik >> (64 - SNK_BITS));
        }

        // Place suffix back into u64 at correct position
        template<int REMAINING, typename SUF>
        static uint64_t suffix_to_u64(SUF suf) noexcept {
            constexpr int SUF_BITS = static_cast<int>(sizeof(SUF) * 8);
            return static_cast<uint64_t>(suf) << (64 - SUF_BITS);
        }

        // --- leaf_find_at<SKIP> ---
        template<int SKIP>
        static const VALUE* leaf_find_at(const uint64_t* node,
                                          uint64_t ik) noexcept {
            if constexpr (SKIP > 0) {
                constexpr uint64_t MASK = ~uint64_t(0) << (64 - 8 * SKIP);
                if ((ik ^ leaf_prefix(node)) & MASK) [[unlikely]] return nullptr;
            }
            constexpr int REMAINING = BITS - 8 * SKIP;
            uint64_t shifted = ik << (8 * SKIP);
            auto suf = to_suffix<REMAINING>(shifted);
            if constexpr (REMAINING <= 8)
                return BO::bitmap_find(node, *get_header(node), suf, LEAF_HEADER_U64);
            else {
                using RCO = compact_ops<nk_for_bits_t<REMAINING>, VALUE, ALLOC>;
                return RCO::find(node, *get_header(node), suf, LEAF_HEADER_U64);
            }
        }

        // --- leaf_first_at<SKIP> ---
        template<int SKIP>
        static leaf_result_t leaf_first_at(const uint64_t* node) noexcept {
            constexpr int REMAINING = BITS - 8 * SKIP;
            if constexpr (REMAINING <= 8) {
                auto r = BO::bitmap_iter_first(node, LEAF_HEADER_U64);
                return {leaf_prefix(node) | suffix_to_u64<REMAINING>(r.suffix),
                        r.value, true};
            } else {
                using RCO = compact_ops<nk_for_bits_t<REMAINING>, VALUE, ALLOC>;
                auto r = RCO::iter_first(node, get_header(node));
                return {leaf_prefix(node) | suffix_to_u64<REMAINING>(r.suffix),
                        r.value, true};
            }
        }

        // --- leaf_last_at<SKIP> ---
        template<int SKIP>
        static leaf_result_t leaf_last_at(const uint64_t* node) noexcept {
            constexpr int REMAINING = BITS - 8 * SKIP;
            if constexpr (REMAINING <= 8) {
                auto r = BO::bitmap_iter_last(node, *get_header(node), LEAF_HEADER_U64);
                return {leaf_prefix(node) | suffix_to_u64<REMAINING>(r.suffix),
                        r.value, true};
            } else {
                using RCO = compact_ops<nk_for_bits_t<REMAINING>, VALUE, ALLOC>;
                auto r = RCO::iter_last(node, get_header(node));
                return {leaf_prefix(node) | suffix_to_u64<REMAINING>(r.suffix),
                        r.value, true};
            }
        }

        // --- leaf_next_at<SKIP> ---
        template<int SKIP>
        static leaf_result_t leaf_next_at(const uint64_t* node,
                                           uint64_t ik) noexcept {
            constexpr int REMAINING = BITS - 8 * SKIP;
            if constexpr (SKIP > 0) {
                uint64_t pfx = leaf_prefix(node);
                constexpr uint64_t MASK = ~uint64_t(0) << (64 - 8 * SKIP);
                uint64_t diff = (ik ^ pfx) & MASK;
                if (diff) [[unlikely]] {
                    int shift = std::countl_zero(diff) & ~7;
                    uint8_t kb = static_cast<uint8_t>(ik >> (56 - shift));
                    uint8_t pb = static_cast<uint8_t>(pfx >> (56 - shift));
                    if (kb < pb) return leaf_first_at<SKIP>(node);
                    return {0, nullptr, false};
                }
            }
            uint64_t shifted = ik << (8 * SKIP);
            auto suf = to_suffix<REMAINING>(shifted);
            if constexpr (REMAINING <= 8) {
                auto r = BO::bitmap_iter_next(node, suf, LEAF_HEADER_U64);
                if (!r.found) [[unlikely]] return {0, nullptr, false};
                return {leaf_prefix(node) | suffix_to_u64<REMAINING>(r.suffix),
                        r.value, true};
            } else {
                using RCO = compact_ops<nk_for_bits_t<REMAINING>, VALUE, ALLOC>;
                auto r = RCO::iter_next(node, get_header(node), suf);
                if (!r.found) [[unlikely]] return {0, nullptr, false};
                return {leaf_prefix(node) | suffix_to_u64<REMAINING>(r.suffix),
                        r.value, true};
            }
        }

        // --- leaf_prev_at<SKIP> ---
        template<int SKIP>
        static leaf_result_t leaf_prev_at(const uint64_t* node,
                                           uint64_t ik) noexcept {
            constexpr int REMAINING = BITS - 8 * SKIP;
            if constexpr (SKIP > 0) {
                uint64_t pfx = leaf_prefix(node);
                constexpr uint64_t MASK = ~uint64_t(0) << (64 - 8 * SKIP);
                uint64_t diff = (ik ^ pfx) & MASK;
                if (diff) [[unlikely]] {
                    int shift = std::countl_zero(diff) & ~7;
                    uint8_t kb = static_cast<uint8_t>(ik >> (56 - shift));
                    uint8_t pb = static_cast<uint8_t>(pfx >> (56 - shift));
                    if (kb > pb) return leaf_last_at<SKIP>(node);
                    return {0, nullptr, false};
                }
            }
            uint64_t shifted = ik << (8 * SKIP);
            auto suf = to_suffix<REMAINING>(shifted);
            if constexpr (REMAINING <= 8) {
                auto r = BO::bitmap_iter_prev(node, suf, LEAF_HEADER_U64);
                if (!r.found) [[unlikely]] return {0, nullptr, false};
                return {leaf_prefix(node) | suffix_to_u64<REMAINING>(r.suffix),
                        r.value, true};
            } else {
                using RCO = compact_ops<nk_for_bits_t<REMAINING>, VALUE, ALLOC>;
                auto r = RCO::iter_prev(node, get_header(node), suf);
                if (!r.found) [[unlikely]] return {0, nullptr, false};
                return {leaf_prefix(node) | suffix_to_u64<REMAINING>(r.suffix),
                        r.value, true};
            }
        }

        // --- Build LEAF_FNS array ---
        template<size_t... Is>
        static constexpr auto make_leaf_fns(std::index_sequence<Is...>) {
            return std::array<leaf_fn_t, sizeof...(Is)>{
                leaf_fn_t{
                    static_cast<uint8_t>(Is),
                    &leaf_find_at<static_cast<int>(Is)>,
                    &leaf_next_at<static_cast<int>(Is)>,
                    &leaf_prev_at<static_cast<int>(Is)>,
                    &leaf_first_at<static_cast<int>(Is)>,
                    &leaf_last_at<static_cast<int>(Is)>,
                }...
            };
        }

        static constexpr auto LEAF_FNS = make_leaf_fns(
            std::make_index_sequence<MAX_LEAF_SKIP + 1>{});
    };

    // ==================================================================
    // find_node — branchless bitmask descent, fn dispatch at leaf
    // No sentinel checks — sentinel leaf's fn->find returns nullptr.
    // ==================================================================

    template<int BITS> requires (BITS > 8)
    static const VALUE* find_node(uint64_t ptr, uint64_t ik) noexcept {
        if (ptr & LEAF_BIT) [[unlikely]] {
            const uint64_t* node = untag_leaf(ptr);
            return BO::leaf_fn(node)->find(node, ik);
        }

        const uint64_t* bm = reinterpret_cast<const uint64_t*>(ptr);
        uint8_t ti = static_cast<uint8_t>(ik >> 56);
        int slot = reinterpret_cast<const bitmap_256_t*>(bm)->
                       find_slot<slot_mode::BRANCHLESS>(ti);
        uint64_t child = bm[BITMAP_256_U64 + slot];

        return find_node<BITS - 8>(child, ik << 8);
    }

    template<int BITS> requires (BITS == 8)
    static const VALUE* find_node(uint64_t ptr, uint64_t ik) noexcept {
        const uint64_t* node = untag_leaf(ptr);
        return BO::bitmap_find(node, *get_header(node),
                                static_cast<uint8_t>(ik >> 56), LEAF_HEADER_U64);
    }

    // ==================================================================
    // find_leaf_next / find_leaf_prev — descent to leaf for iteration
    // No sentinel checks — sentinel leaf returned, caller invokes fn.
    // ==================================================================

    template<int BITS> requires (BITS > 8)
    static const uint64_t* find_leaf_next(uint64_t ptr, uint64_t ik) noexcept {
        if (ptr & LEAF_BIT) [[unlikely]]
            return untag_leaf(ptr);

        const uint64_t* bm = reinterpret_cast<const uint64_t*>(ptr);
        const auto* bmp = reinterpret_cast<const bitmap_256_t*>(bm);
        uint8_t ti = static_cast<uint8_t>(ik >> 56);

        int slot = bmp->find_slot<slot_mode::FAST_EXIT>(ti);
        if (slot >= 0) [[likely]] {
            const uint64_t* r = find_leaf_next<BITS - 8>(bm[BITMAP_256_U64 + slot], ik << 8);
            if (r) [[likely]] return r;
        }

        auto adj = bmp->next_set_after(ti);
        if (!adj.found) [[unlikely]] return nullptr;
        return descend_min_leaf<BITS - 8>(bm[BITMAP_256_U64 + adj.slot]);
    }

    template<int BITS> requires (BITS == 8)
    static const uint64_t* find_leaf_next(uint64_t ptr, uint64_t) noexcept {
        return untag_leaf(ptr);
    }

    template<int BITS> requires (BITS > 8)
    static const uint64_t* find_leaf_prev(uint64_t ptr, uint64_t ik) noexcept {
        if (ptr & LEAF_BIT) [[unlikely]]
            return untag_leaf(ptr);

        const uint64_t* bm = reinterpret_cast<const uint64_t*>(ptr);
        const auto* bmp = reinterpret_cast<const bitmap_256_t*>(bm);
        uint8_t ti = static_cast<uint8_t>(ik >> 56);

        int slot = bmp->find_slot<slot_mode::FAST_EXIT>(ti);
        if (slot >= 0) [[likely]] {
            const uint64_t* r = find_leaf_prev<BITS - 8>(bm[BITMAP_256_U64 + slot], ik << 8);
            if (r) [[likely]] return r;
        }

        auto adj = bmp->prev_set_before(ti);
        if (!adj.found) [[unlikely]] return nullptr;
        return descend_max_leaf<BITS - 8>(bm[BITMAP_256_U64 + adj.slot]);
    }

    template<int BITS> requires (BITS == 8)
    static const uint64_t* find_leaf_prev(uint64_t ptr, uint64_t) noexcept {
        return untag_leaf(ptr);
    }

    template<int BITS> requires (BITS >= 8)
    static const uint64_t* descend_min_leaf(uint64_t ptr) noexcept {
        if (ptr & LEAF_BIT) [[unlikely]] return untag_leaf(ptr);
        const uint64_t* bm = reinterpret_cast<const uint64_t*>(ptr);
        // bm[BITMAP_256_U64] = sentinel, +1 = first real child
        if constexpr (BITS > 8)
            return descend_min_leaf<BITS - 8>(bm[BITMAP_256_U64 + 1]);
        else
            return untag_leaf(bm[BITMAP_256_U64 + 1]);
    }

    template<int BITS> requires (BITS >= 8)
    static const uint64_t* descend_max_leaf(uint64_t ptr) noexcept {
        if (ptr & LEAF_BIT) [[unlikely]] return untag_leaf(ptr);
        const uint64_t* bm = reinterpret_cast<const uint64_t*>(ptr);
        auto* hdr = get_header(bm_to_node_const(reinterpret_cast<uint64_t>(bm)));
        int last = hdr->entries() - 1;
        // bm[BITMAP_256_U64 + 1] = first real child, +last = last real child
        if constexpr (BITS > 8)
            return descend_max_leaf<BITS - 8>(bm[BITMAP_256_U64 + 1 + last]);
        else
            return untag_leaf(bm[BITMAP_256_U64 + 1 + last]);
    }

    // ==================================================================
    // Make single leaf — narrow to storage NK at boundary
    // ==================================================================

    template<int BITS> requires (BITS >= 8)
    static uint64_t* make_single_leaf(uint64_t ik, VST value, BLD& bld) {
        uint64_t* node;
        if constexpr (BITS <= 8) {
            node = BO::make_single_bitmap(static_cast<uint8_t>(ik >> 56), value, bld);
        } else {
            using SNK = nk_for_bits_t<BITS>;
            constexpr int SNK_BITS = static_cast<int>(sizeof(SNK) * 8);
            SNK suffix = static_cast<SNK>(ik >> (64 - SNK_BITS));
            using CO = compact_ops<SNK, VALUE, ALLOC>;
            node = CO::make_leaf(&suffix, &value, 1, bld);
        }
        init_leaf_fn<BITS>(node);
        return node;
    }

    // Recursively descend `depth` bytes, then create single-entry leaf.
    template<int BITS> requires (BITS >= 8)
    static uint64_t* make_leaf_descended(uint64_t ik, VST value, uint8_t depth,
                                           BLD& bld) {
        if (depth == 0) [[unlikely]]
            return make_single_leaf<BITS>(ik, value, bld);

        if constexpr (BITS > 8) {
            return make_leaf_descended<BITS - 8>(ik << 8, value, depth - 1, bld);
        }
        __builtin_unreachable();
    }

    // ==================================================================
    // Leaf iterate / build helpers
    // ==================================================================

    // Iterate leaf entries, callback receives (NK suffix, VST value)
    template<int BITS, typename Fn>
    static void leaf_for_each(const uint64_t* node, const node_header_t* hdr,
                                Fn&& cb) {
        using NK = nk_for_bits_t<BITS>;
        if constexpr (sizeof(NK) == 1) {
            BO::for_each_bitmap(node, [&](uint8_t s, VST v) {
                cb(static_cast<NK>(s), v);
            });
        } else {
            using CO = compact_ops<NK, VALUE, ALLOC>;
            CO::for_each(node, hdr, std::forward<Fn>(cb));
        }
    }

    // Build leaf from NK-typed sorted arrays. Returns raw pointer.
    template<int BITS>
    static uint64_t* build_leaf(nk_for_bits_t<BITS>* suf, VST* vals,
                                  size_t count, BLD& bld) {
        using NK = nk_for_bits_t<BITS>;
        uint64_t* node;
        if constexpr (sizeof(NK) == 1) {
            node = BO::make_bitmap_leaf(reinterpret_cast<uint8_t*>(suf), vals,
                static_cast<uint32_t>(count), bld);
        } else {
            using CO = compact_ops<NK, VALUE, ALLOC>;
            node = CO::make_leaf(suf, vals, static_cast<uint32_t>(count), bld);
        }
        init_leaf_fn<BITS>(node);
        return node;
    }

    // Build node from sorted arrays. Returns tagged pointer.
    template<int BITS>
    static uint64_t build_node_from_arrays_tagged(nk_for_bits_t<BITS>* suf,
                                                     VST* vals,
                                                     size_t count, BLD& bld) {
        using NK = nk_for_bits_t<BITS>;
        constexpr int NK_BITS = static_cast<int>(sizeof(NK) * 8);

        // Leaf case
        if (count <= COMPACT_MAX)
            return tag_leaf(build_leaf<BITS>(suf, vals, count, bld));

        // Skip compression: all entries share same top byte?
        uint8_t first_top = static_cast<uint8_t>(suf[0] >> (NK_BITS - 8));
        bool all_same = true;
        for (size_t i = 1; i < count; ++i)
            if (static_cast<uint8_t>(suf[i] >> (NK_BITS - 8)) != first_top)
                { all_same = false; break; }

        if (all_same && BITS > 8) {
            // Strip top byte, narrow to child NK
            using CNK = nk_for_bits_t<BITS - 8>;
            constexpr int CNK_BITS = static_cast<int>(sizeof(CNK) * 8);
            auto cs = std::make_unique<CNK[]>(count);
            for (size_t i = 0; i < count; ++i) {
                NK shifted = static_cast<NK>(suf[i] << 8);
                cs[i] = static_cast<CNK>(shifted >> (NK_BITS - CNK_BITS));
            }

            uint64_t child_tagged = build_node_from_arrays_tagged<BITS - 8>(
                cs.get(), vals, count, bld);

            uint8_t byte_arr[1] = {first_top};
            if (child_tagged & LEAF_BIT) {
                uint64_t* leaf = untag_leaf_mut(child_tagged);
                leaf = prepend_skip(leaf, 1, uint64_t(first_top) << 56, bld);
                return tag_leaf(leaf);
            }
            auto* bm_node = bm_to_node(child_tagged);
            return BO::wrap_in_chain(bm_node, byte_arr, 1, bld);
        }

        // Multi-child bitmask
        uint8_t indices[256];
        uint64_t child_tagged[256];
        int n_children = 0;

        size_t i = 0;
        while (i < count) {
            uint8_t ti = static_cast<uint8_t>(suf[i] >> (NK_BITS - 8));
            size_t start = i;
            while (i < count && static_cast<uint8_t>(suf[i] >> (NK_BITS - 8)) == ti) ++i;
            size_t cc = i - start;

            if constexpr (BITS > 8) {
                using CNK = nk_for_bits_t<BITS - 8>;
                constexpr int CNK_BITS = static_cast<int>(sizeof(CNK) * 8);
                auto cs = std::make_unique<CNK[]>(cc);
                for (size_t j = 0; j < cc; ++j) {
                    NK shifted = static_cast<NK>(suf[start + j] << 8);
                    cs[j] = static_cast<CNK>(shifted >> (NK_BITS - CNK_BITS));
                }
                child_tagged[n_children] = build_node_from_arrays_tagged<BITS - 8>(
                    cs.get(), vals + start, cc, bld);
            }
            indices[n_children] = ti;
            n_children++;
        }

        return tag_bitmask(
            BO::make_bitmask(indices, child_tagged, n_children, bld, count));
    }

    // ==================================================================
    // prepend_skip / remove_skip — no realloc, sets fn pointer + prefix
    // BITS: the tree level where this leaf sits after the operation.
    // ==================================================================

    template<int BITS>
    static void prepend_skip_fn(uint64_t* node, uint8_t new_len,
                                  uint64_t new_pfx) noexcept {
        uint8_t old_skip = get_header(node)->skip();
        uint8_t new_skip = old_skip + new_len;
        uint64_t combined = new_pfx;
        if (old_skip > 0)
            combined |= leaf_prefix(node) >> (8 * new_len);
        set_leaf_prefix(node, combined);
        get_header(node)->set_skip(new_skip);
        BO::set_leaf_fn(node, &leaf_ops_t<BITS>::LEAF_FNS[new_skip]);
    }

    // Write-path prepend_skip: returns node pointer for chaining.
    template<int BITS>
    static uint64_t* prepend_skip(uint64_t* node, uint8_t new_len,
                                    uint64_t new_pfx, BLD&) {
        prepend_skip_fn<BITS>(node, new_len, new_pfx);
        return node;
    }

    template<int BITS>
    static uint64_t* remove_skip(uint64_t* node, BLD&) {
        set_leaf_prefix(node, 0);
        get_header(node)->set_skip(0);
        BO::set_leaf_fn(node, &leaf_ops_t<BITS>::LEAF_FNS[0]);
        return node;
    }

    // Set fn pointer for freshly created leaf (skip=0)
    template<int BITS>
    static void init_leaf_fn(uint64_t* node) noexcept {
        BO::set_leaf_fn(node, &leaf_ops_t<BITS>::LEAF_FNS[0]);
        set_leaf_prefix(node, 0);
    }

    // ==================================================================
    // split_on_prefix: leaf skip prefix diverges
    // ==================================================================

    template<int BITS> requires (BITS >= 8)
    static uint64_t split_on_prefix(uint64_t* node, node_header_t* hdr,
                                      uint64_t ik, VST value,
                                      uint64_t pfx_u64, uint8_t skip,
                                      uint8_t common, BLD& bld) {
        uint8_t new_idx = static_cast<uint8_t>(ik >> 56);
        uint8_t old_idx = pfx_byte(pfx_u64, common);
        uint8_t old_rem = skip - 1 - common;

        // Save common prefix bytes for wrap_in_chain
        uint8_t saved_prefix[6] = {};
        pfx_to_bytes(pfx_u64, saved_prefix, common);

        // Update old node: strip consumed prefix, keep remainder
        if (old_rem > 0) [[unlikely]] {
            uint64_t rem_pfx = pfx_u64 << (8 * (common + 1));
            hdr->set_skip(old_rem);
            set_leaf_prefix(node, rem_pfx);
        } else {
            node = remove_skip(node, bld);
            hdr = get_header(node);
        }

        // Build new leaf
        uint64_t* new_leaf;
        if constexpr (BITS > 8) {
            new_leaf = make_leaf_descended<BITS - 8>(ik << 8, value, old_rem, bld);
        } else {
            new_leaf = make_single_leaf<BITS>(ik, value, bld);
        }
        if (old_rem > 0) [[unlikely]] {
            uint64_t new_pfx_u64 = (ik << 8) & (~uint64_t(0) << (64 - 8 * old_rem));
            new_leaf = prepend_skip(new_leaf, old_rem, new_pfx_u64, bld);
        }

        // Create parent bitmask with 2 children
        uint8_t bi[2];
        uint64_t cp[2];
        if (new_idx < old_idx) {
            bi[0] = new_idx; cp[0] = tag_leaf(new_leaf);
            bi[1] = old_idx; cp[1] = tag_leaf(node);
        } else {
            bi[0] = old_idx; cp[0] = tag_leaf(node);
            bi[1] = new_idx; cp[1] = tag_leaf(new_leaf);
        }

        uint64_t total = BO::exact_subtree_count(cp[0]) +
                         BO::exact_subtree_count(cp[1]);
        auto* bm_node = BO::make_bitmask(bi, cp, 2, bld, total);
        if (common > 0) [[unlikely]]
            return BO::wrap_in_chain(bm_node, saved_prefix, common, bld);
        return tag_bitmask(bm_node);
    }

    // ==================================================================
    // split_skip_at: key diverges in skip chain
    // ==================================================================

    template<int BITS> requires (BITS >= 8)
    static uint64_t split_skip_at(uint64_t* node, node_header_t* hdr,
                                    uint8_t sc, uint8_t split_pos,
                                    uint64_t ik, VST value, BLD& bld) {
        uint8_t expected = static_cast<uint8_t>(ik >> 56);
        uint8_t actual_byte = BO::skip_byte(node, split_pos);

        // Build new leaf — one byte past the split point
        uint64_t new_leaf_tagged;
        if constexpr (BITS > 8) {
            auto* leaf = make_single_leaf<BITS - 8>(ik << 8, value, bld);
            new_leaf_tagged = tag_leaf(leaf);
        } else {
            auto* leaf = make_single_leaf<BITS>(ik, value, bld);
            new_leaf_tagged = tag_leaf(leaf);
        }

        // Build remainder from [split_pos+1..sc-1] + final bitmask
        uint64_t remainder = BO::build_remainder(node, sc, split_pos + 1, bld);

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
        auto* split_node = BO::make_bitmask(bi, cp, 2, bld, total);

        // Wrap in skip chain for prefix bytes [0..split_pos-1]
        uint64_t result;
        if (split_pos > 0) [[unlikely]] {
            uint8_t prefix_bytes[6];
            BO::skip_bytes(node, split_pos, prefix_bytes);
            result = BO::wrap_in_chain(split_node, prefix_bytes, split_pos, bld);
        } else {
            result = tag_bitmask(split_node);
        }

        bld.dealloc_node(node, hdr->alloc_u64());
        return result;
    }

    // ==================================================================
    // convert_to_bitmask_tagged — compact leaf overflow
    // ==================================================================

    template<int BITS>
    static uint64_t convert_to_bitmask_tagged(const uint64_t* node,
                                                const node_header_t* hdr,
                                                uint64_t ik, VST value,
                                                BLD& bld) {
        using NK = nk_for_bits_t<BITS>;
        constexpr int NK_BITS = static_cast<int>(sizeof(NK) * 8);
        NK suffix = static_cast<NK>(ik >> (64 - NK_BITS));

        uint16_t old_count = hdr->entries();
        size_t total = old_count + 1;
        auto wk = std::make_unique<NK[]>(total);
        auto wv = std::make_unique<VST[]>(total);

        size_t wi = 0;
        bool ins = false;
        leaf_for_each<BITS>(node, hdr, [&](NK s, VST v) {
            if (!ins && suffix < s) {
                wk[wi] = suffix; wv[wi] = value; wi++; ins = true;
            }
            wk[wi] = s; wv[wi] = v; wi++;
        });
        if (!ins) { wk[wi] = suffix; wv[wi] = value; }

        uint64_t child_tagged = build_node_from_arrays_tagged<BITS>(
            wk.get(), wv.get(), total, bld);

        // Propagate old skip/prefix to new child
        uint8_t ps = hdr->skip();
        if (ps > 0) {
            uint64_t pfx_u64 = leaf_prefix(node);
            if (child_tagged & LEAF_BIT) {
                uint64_t* leaf = untag_leaf_mut(child_tagged);
                leaf = prepend_skip(leaf, ps, pfx_u64, bld);
                child_tagged = tag_leaf(leaf);
            } else {
                uint8_t pfx_bytes[6];
                pfx_to_bytes(pfx_u64, pfx_bytes, ps);
                uint64_t* bm_node = bm_to_node(child_tagged);
                child_tagged = BO::wrap_in_chain(bm_node, pfx_bytes, ps, bld);
            }
        }

        bld.dealloc_node(const_cast<uint64_t*>(node), hdr->alloc_u64());
        return child_tagged;
    }

    // ==================================================================
    // Insert — uint64_t ik, no narrowing
    // ==================================================================

    template<int BITS, bool INSERT, bool ASSIGN> requires (BITS >= 8)
    static insert_result_t insert_node(uint64_t ptr, uint64_t ik, VST value,
                                         BLD& bld) {
        // SENTINEL
        if (ptr == BO::SENTINEL_TAGGED) [[unlikely]] {
            if constexpr (!INSERT) return {ptr, false, false};
            return {tag_leaf(make_single_leaf<BITS>(ik, value, bld)), true, false};
        }

        // LEAF
        if (ptr & LEAF_BIT) [[unlikely]] {
            uint64_t* node = untag_leaf_mut(ptr);
            auto* hdr = get_header(node);

            uint8_t skip = hdr->skip();
            if (skip) [[unlikely]] {
                uint64_t pfx_u64 = leaf_prefix(node);
                return insert_leaf_skip<BITS, INSERT, ASSIGN>(
                    node, hdr, ik, value, pfx_u64, skip, 0, bld);
            }

            return leaf_insert<BITS, INSERT, ASSIGN>(node, hdr, ik, value, bld);
        }

        // BITMASK
        uint64_t* node = bm_to_node(ptr);
        auto* hdr = get_header(node);
        uint8_t sc = hdr->skip();

        if (sc > 0) [[unlikely]]
            return insert_chain_skip<BITS, INSERT, ASSIGN>(
                node, hdr, sc, ik, value, 0, bld);

        return insert_final_bitmask<BITS, INSERT, ASSIGN>(
            node, hdr, 0, ik, value, bld);
    }

    // --- Leaf skip prefix: recursive byte-at-a-time, no narrowing ---
    template<int BITS, bool INSERT, bool ASSIGN> requires (BITS >= 8)
    static insert_result_t insert_leaf_skip(
            uint64_t* node, node_header_t* hdr,
            uint64_t ik, VST value,
            uint64_t pfx_u64, uint8_t skip, uint8_t pos,
            BLD& bld) {
        if (pos >= skip) [[unlikely]]
            return leaf_insert<BITS, INSERT, ASSIGN>(node, hdr, ik, value, bld);

        uint8_t expected = static_cast<uint8_t>(ik >> 56);
        if (expected != pfx_byte(pfx_u64, pos)) [[unlikely]] {
            if constexpr (!INSERT) return {tag_leaf(node), false, false};
            return {split_on_prefix<BITS>(node, hdr, ik, value,
                                            pfx_u64, skip, pos, bld), true, false};
        }

        if constexpr (BITS > 8) {
            return insert_leaf_skip<BITS - 8, INSERT, ASSIGN>(
                node, hdr, ik << 8, value, pfx_u64, skip, pos + 1, bld);
        }
        __builtin_unreachable();
    }

    // --- Leaf insert: compile-time NK dispatch ---
    template<int BITS, bool INSERT, bool ASSIGN>
    static insert_result_t leaf_insert(uint64_t* node, node_header_t* hdr,
                                         uint64_t ik, VST value, BLD& bld) {
        using NK = nk_for_bits_t<BITS>;
        constexpr int NK_BITS = static_cast<int>(sizeof(NK) * 8);
        NK suffix = static_cast<NK>(ik >> (64 - NK_BITS));

        insert_result_t result;
        if constexpr (sizeof(NK) == 1) {
            result = BO::template bitmap_insert<INSERT, ASSIGN>(
                node, static_cast<uint8_t>(suffix), value, bld);
        } else {
            using CO = compact_ops<NK, VALUE, ALLOC>;
            result = CO::template insert<INSERT, ASSIGN>(
                node, hdr, suffix, value, bld);
        }
        if (result.needs_split) [[unlikely]] {
            if constexpr (!INSERT) return {tag_leaf(node), false, false};
            return {convert_to_bitmask_tagged<BITS>(node, hdr, ik, value, bld),
                    true, false};
        }
        return result;
    }

    // --- Skip chain: recursive embed walk, no narrowing ---
    template<int BITS, bool INSERT, bool ASSIGN> requires (BITS >= 8)
    static insert_result_t insert_chain_skip(
            uint64_t* node, node_header_t* hdr,
            uint8_t sc, uint64_t ik, VST value, uint8_t pos,
            BLD& bld) {
        if (pos >= sc) [[unlikely]]
            return insert_final_bitmask<BITS, INSERT, ASSIGN>(
                node, hdr, sc, ik, value, bld);

        uint8_t actual_byte = BO::skip_byte(node, pos);
        uint8_t expected = static_cast<uint8_t>(ik >> 56);

        if (expected != actual_byte) [[unlikely]] {
            if constexpr (!INSERT) return {tag_bitmask(node), false, false};
            return {split_skip_at<BITS>(node, hdr, sc, pos, ik, value, bld),
                    true, false};
        }

        if constexpr (BITS > 8) {
            return insert_chain_skip<BITS - 8, INSERT, ASSIGN>(
                node, hdr, sc, ik << 8, value, pos + 1, bld);
        }
        __builtin_unreachable();
    }

    // --- Final bitmask: lookup + recurse, no narrowing ---
    template<int BITS, bool INSERT, bool ASSIGN> requires (BITS >= 8)
    static insert_result_t insert_final_bitmask(
            uint64_t* node, node_header_t* hdr,
            uint8_t sc, uint64_t ik, VST value, BLD& bld) {
        uint8_t ti = static_cast<uint8_t>(ik >> 56);

        typename BO::child_lookup cl;
        if (sc > 0) [[unlikely]]
            cl = BO::chain_lookup(node, sc, ti);
        else
            cl = BO::lookup(node, ti);

        if (!cl.found) [[unlikely]] {
            if constexpr (!INSERT) return {tag_bitmask(node), false, false};

            uint64_t* leaf;
            if constexpr (BITS > 8) {
                leaf = make_single_leaf<BITS - 8>(ik << 8, value, bld);
            } else {
                __builtin_unreachable();
            }

            uint64_t* nn;
            if (sc > 0) [[unlikely]]
                nn = BO::chain_add_child(node, hdr, sc, ti, tag_leaf(leaf), bld);
            else
                nn = BO::add_child(node, hdr, ti, tag_leaf(leaf), bld);
            inc_descendants(nn, get_header(nn));
            return {tag_bitmask(nn), true, false};
        }

        // Found — recurse into child
        if constexpr (BITS > 8) {
            auto cr = insert_node<BITS - 8, INSERT, ASSIGN>(
                cl.child, ik << 8, value, bld);

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
    // Erase — uint64_t ik, no narrowing
    // ==================================================================

    template<int BITS> requires (BITS >= 8)
    static erase_result_t erase_node(uint64_t ptr, uint64_t ik, BLD& bld) {
        if (ptr == BO::SENTINEL_TAGGED) return {ptr, false, 0};

        if (ptr & LEAF_BIT) [[unlikely]] {
            uint64_t* node = untag_leaf_mut(ptr);
            auto* hdr = get_header(node);

            uint8_t skip = hdr->skip();
            if (skip) [[unlikely]] {
                uint64_t pfx_u64 = leaf_prefix(node);
                return erase_leaf_skip<BITS>(
                    node, hdr, ik, pfx_u64, skip, 0, bld);
            }

            return leaf_erase<BITS>(node, hdr, ik, bld);
        }

        uint64_t* node = bm_to_node(ptr);
        auto* hdr = get_header(node);
        uint8_t sc = hdr->skip();

        if (sc > 0) [[unlikely]]
            return erase_chain_skip<BITS>(
                node, hdr, sc, ik, 0, bld);

        return erase_final_bitmask<BITS>(
            node, hdr, 0, ik, bld);
    }

    // --- Leaf skip prefix: no narrowing ---
    template<int BITS> requires (BITS >= 8)
    static erase_result_t erase_leaf_skip(
            uint64_t* node, node_header_t* hdr,
            uint64_t ik, uint64_t pfx_u64, uint8_t skip, uint8_t pos,
            BLD& bld) {
        if (pos >= skip) [[unlikely]]
            return leaf_erase<BITS>(node, hdr, ik, bld);

        uint8_t expected = static_cast<uint8_t>(ik >> 56);
        if (expected != pfx_byte(pfx_u64, pos)) [[unlikely]]
            return {tag_leaf(node), false, 0};

        if constexpr (BITS > 8) {
            return erase_leaf_skip<BITS - 8>(
                node, hdr, ik << 8, pfx_u64, skip, pos + 1, bld);
        }
        __builtin_unreachable();
    }

    // --- Leaf erase: compile-time NK dispatch ---
    template<int BITS>
    static erase_result_t leaf_erase(uint64_t* node, node_header_t* hdr,
                                       uint64_t ik, BLD& bld) {
        using NK = nk_for_bits_t<BITS>;
        constexpr int NK_BITS = static_cast<int>(sizeof(NK) * 8);
        NK suffix = static_cast<NK>(ik >> (64 - NK_BITS));

        if constexpr (sizeof(NK) == 1) {
            return BO::bitmap_erase(node, static_cast<uint8_t>(suffix), bld);
        } else {
            using CO = compact_ops<NK, VALUE, ALLOC>;
            return CO::erase(node, hdr, suffix, bld);
        }
    }

    // --- Skip chain: no narrowing ---
    template<int BITS> requires (BITS >= 8)
    static erase_result_t erase_chain_skip(
            uint64_t* node, node_header_t* hdr,
            uint8_t sc, uint64_t ik, uint8_t pos,
            BLD& bld) {
        if (pos >= sc) [[unlikely]]
            return erase_final_bitmask<BITS>(
                node, hdr, sc, ik, bld);

        uint8_t actual_byte = BO::skip_byte(node, pos);
        uint8_t expected = static_cast<uint8_t>(ik >> 56);

        if (expected != actual_byte) [[unlikely]]
            return {tag_bitmask(node), false, 0};

        if constexpr (BITS > 8) {
            return erase_chain_skip<BITS - 8>(
                node, hdr, sc, ik << 8, pos + 1, bld);
        }
        __builtin_unreachable();
    }

    // --- Final bitmask: lookup + recurse + coalesce ---
    template<int BITS> requires (BITS >= 8)
    static erase_result_t erase_final_bitmask(
            uint64_t* node, node_header_t* hdr,
            uint8_t sc, uint64_t ik, BLD& bld) {
        uint8_t ti = static_cast<uint8_t>(ik >> 56);

        typename BO::child_lookup cl;
        if (sc > 0) [[unlikely]]
            cl = BO::chain_lookup(node, sc, ti);
        else
            cl = BO::lookup(node, ti);

        if (!cl.found) [[unlikely]] return {tag_bitmask(node), false, 0};

        // Recurse into child
        erase_result_t cr;
        if constexpr (BITS > 8) {
            cr = erase_node<BITS - 8>(cl.child, ik << 8, bld);
        } else {
            __builtin_unreachable();
        }

        if (!cr.erased) [[unlikely]] return {tag_bitmask(node), false, 0};

        if (cr.tagged_ptr) [[likely]] {
            if (cr.tagged_ptr != cl.child) {
                if (sc > 0) [[unlikely]]
                    BO::chain_set_child(node, sc, cl.slot, cr.tagged_ptr);
                else
                    BO::set_child(node, cl.slot, cr.tagged_ptr);
            }
            uint64_t exact = dec_descendants(node, hdr);
            if (exact <= COMPACT_MAX) [[unlikely]]
                return do_coalesce<BITS>(node, hdr, bld);
            return {tag_bitmask(node), true, exact};
        }

        // Child fully erased
        uint64_t* nn;
        if (sc > 0) [[unlikely]]
            nn = BO::chain_remove_child(node, hdr, sc, cl.slot, ti, bld);
        else
            nn = BO::remove_child(node, hdr, cl.slot, ti, bld);
        if (!nn) [[unlikely]] return {0, true, 0};

        hdr = get_header(nn);
        unsigned nc = hdr->entries();
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
                leaf = prepend_skip(leaf, ci.total_skip,
                           pack_prefix(ci.bytes, ci.total_skip), bld);
                bld.dealloc_node(nn, nn_au64);
                return {tag_leaf(leaf), true, exact};
            }
            uint64_t* child_node = bm_to_node(ci.sole_child);
            bld.dealloc_node(nn, nn_au64);
            return {BO::wrap_in_chain(child_node, ci.bytes, ci.total_skip, bld),
                    true, exact};
        }

        if (exact <= COMPACT_MAX) [[unlikely]]
            return do_coalesce<BITS>(nn, get_header(nn), bld);
        return {tag_bitmask(nn), true, exact};
    }

    // ==================================================================
    // Collect entries — all use u64 ik internally, narrow at leaf
    // ==================================================================

    struct collected_t {
        std::unique_ptr<nk_for_bits_t<8>[]> keys;   // placeholder, overridden per BITS
        std::unique_ptr<VST[]> vals;
        size_t count;
    };

    // Generic collected_t with typed keys
    template<int BITS>
    struct collected_typed_t {
        using NK = nk_for_bits_t<BITS>;
        std::unique_ptr<NK[]> keys;
        std::unique_ptr<VST[]> vals;
        size_t count;
    };

    template<int BITS> requires (BITS >= 8)
    static collected_typed_t<BITS> collect_entries(uint64_t tagged) {
        using NK = nk_for_bits_t<BITS>;
        constexpr int NK_BITS = static_cast<int>(sizeof(NK) * 8);

        if (tagged & LEAF_BIT) [[unlikely]] {
            const uint64_t* node = untag_leaf(tagged);
            auto* hdr = get_header(node);
            uint8_t skip = hdr->skip();
            if (skip) [[unlikely]]
                return collect_leaf_skip<BITS>(node, hdr, leaf_prefix(node), skip, 0);
            return collect_leaf<BITS>(node, hdr);
        }

        const uint64_t* node = bm_to_node_const(tagged);
        auto* hdr = get_header(node);
        uint8_t sc = hdr->skip();
        if (sc > 0) [[unlikely]]
            return collect_bm_skip<BITS>(node, sc, 0);
        return collect_bm_final<BITS>(node, 0);
    }

    // --- Leaf body: direct extraction ---
    template<int BITS> requires (BITS >= 8)
    static collected_typed_t<BITS> collect_leaf(const uint64_t* node,
                                                  const node_header_t* hdr) {
        using NK = nk_for_bits_t<BITS>;
        size_t n = hdr->entries();
        auto wk = std::make_unique<NK[]>(n);
        auto wv = std::make_unique<VST[]>(n);
        size_t wi = 0;
        leaf_for_each<BITS>(node, hdr, [&](NK s, VST v) {
            wk[wi] = s; wv[wi] = v; wi++;
        });
        return {std::move(wk), std::move(wv), wi};
    }

    // --- Leaf skip: consume prefix bytes, no narrowing ---
    template<int BITS> requires (BITS >= 8)
    static collected_typed_t<BITS> collect_leaf_skip(const uint64_t* node,
                                                       const node_header_t* hdr,
                                                       uint64_t pfx_u64, uint8_t skip,
                                                       uint8_t pos) {
        if (pos >= skip) [[unlikely]]
            return collect_leaf<BITS>(node, hdr);

        if constexpr (BITS > 8) {
            using NK = nk_for_bits_t<BITS>;
            constexpr int NK_BITS = static_cast<int>(sizeof(NK) * 8);
            uint8_t byte = pfx_byte(pfx_u64, pos);
            auto child = collect_leaf_skip<BITS - 8>(node, hdr, pfx_u64, skip, pos + 1);

            // Prepend byte: widen child NK to this level's NK
            using CNK = nk_for_bits_t<BITS - 8>;
            constexpr int CNK_BITS = static_cast<int>(sizeof(CNK) * 8);
            auto wk = std::make_unique<NK[]>(child.count);
            auto wv = std::make_unique<VST[]>(child.count);
            for (size_t i = 0; i < child.count; ++i) {
                wk[i] = (NK(byte) << (NK_BITS - 8))
                       | static_cast<NK>(static_cast<uint64_t>(child.keys[i]) << (64 - CNK_BITS) >> (64 - NK_BITS + 8));
                wv[i] = child.vals[i];
            }
            return {std::move(wk), std::move(wv), child.count};
        }
        __builtin_unreachable();
    }

    // --- BM skip: consume embed bytes, no narrowing ---
    template<int BITS> requires (BITS >= 8)
    static collected_typed_t<BITS> collect_bm_skip(const uint64_t* node,
                                                      uint8_t sc, uint8_t pos) {
        if (pos >= sc) [[unlikely]]
            return collect_bm_final<BITS>(node, sc);

        if constexpr (BITS > 8) {
            using NK = nk_for_bits_t<BITS>;
            constexpr int NK_BITS = static_cast<int>(sizeof(NK) * 8);
            uint8_t byte = BO::skip_byte(node, pos);
            auto child = collect_bm_skip<BITS - 8>(node, sc, pos + 1);

            using CNK = nk_for_bits_t<BITS - 8>;
            constexpr int CNK_BITS = static_cast<int>(sizeof(CNK) * 8);
            auto wk = std::make_unique<NK[]>(child.count);
            auto wv = std::make_unique<VST[]>(child.count);
            for (size_t i = 0; i < child.count; ++i) {
                wk[i] = (NK(byte) << (NK_BITS - 8))
                       | static_cast<NK>(static_cast<uint64_t>(child.keys[i]) << (64 - CNK_BITS) >> (64 - NK_BITS + 8));
                wv[i] = child.vals[i];
            }
            return {std::move(wk), std::move(wv), child.count};
        }
        __builtin_unreachable();
    }

    // --- BM final: iterate children, collect each, merge ---
    template<int BITS> requires (BITS >= 8)
    static collected_typed_t<BITS> collect_bm_final(const uint64_t* node, uint8_t sc) {
        using NK = nk_for_bits_t<BITS>;
        constexpr int NK_BITS = static_cast<int>(sizeof(NK) * 8);
        auto* hdr = get_header(node);
        uint64_t total = BO::chain_descendants(node, sc, hdr->entries());

        auto wk = std::make_unique<NK[]>(total);
        auto wv = std::make_unique<VST[]>(total);
        size_t wi = 0;

        const bitmap_256_t& fbm = BO::chain_bitmap(node, sc);
        const uint64_t* rch = BO::chain_children(node, sc);

        fbm.for_each_set([&](uint8_t idx, int slot) {
            if constexpr (BITS > 8) {
                using CNK = nk_for_bits_t<BITS - 8>;
                constexpr int CNK_BITS = static_cast<int>(sizeof(CNK) * 8);
                auto child = collect_entries<BITS - 8>(rch[slot]);
                for (size_t i = 0; i < child.count; ++i) {
                    wk[wi] = (NK(idx) << (NK_BITS - 8))
                           | static_cast<NK>(static_cast<uint64_t>(child.keys[i]) << (64 - CNK_BITS) >> (64 - NK_BITS + 8));
                    wv[wi] = child.vals[i];
                    wi++;
                }
            }
        });
        return {std::move(wk), std::move(wv), wi};
    }

    // ==================================================================
    // do_coalesce — collect entries + build leaf
    // ==================================================================

    template<int BITS> requires (BITS >= 8)
    static erase_result_t do_coalesce(uint64_t* node, node_header_t* hdr,
                                        BLD& bld) {
        uint8_t sc = hdr->skip();

        auto c = collect_bm_final<BITS>(node, sc);

        uint64_t* leaf = build_leaf<BITS>(c.keys.get(), c.vals.get(), c.count, bld);

        if (sc > 0) [[unlikely]] {
            uint8_t sb[6];
            BO::skip_bytes(node, sc, sb);
            leaf = prepend_skip(leaf, sc, pack_prefix(sb, sc), bld);
        }

        dealloc_coalesced_node<BITS>(node, sc, bld);
        return {tag_leaf(leaf), true, c.count};
    }

    // ==================================================================
    // NK-independent helpers
    // ==================================================================

    static void inc_descendants(uint64_t* node, node_header_t* hdr) noexcept {
        BO::chain_descendants_mut(node, hdr->skip(), hdr->entries())++;
    }

    static uint64_t dec_descendants(uint64_t* node, node_header_t* hdr) noexcept {
        uint64_t& d = BO::chain_descendants_mut(node, hdr->skip(), hdr->entries());
        return --d;
    }

    // ==================================================================
    // Subtree deallocation (values already collected)
    // ==================================================================

    template<int BITS> requires (BITS >= 8)
    static void dealloc_bitmask_subtree(uint64_t tagged, BLD& bld) noexcept {
        if (tagged & LEAF_BIT) [[unlikely]] {
            uint64_t* node = untag_leaf_mut(tagged);
            auto* hdr = get_header(node);
            bld.dealloc_node(node, hdr->alloc_u64());
            return;
        }
        uint64_t* node = bm_to_node(tagged);
        auto* hdr = get_header(node);
        uint8_t sc = hdr->skip();

        if (sc > 0) [[unlikely]]
            dealloc_bm_chain_skip<BITS>(node, sc, 0, bld);
        else
            dealloc_bm_final<BITS>(node, sc, bld);
        bld.dealloc_node(node, hdr->alloc_u64());
    }

    template<int BITS> requires (BITS >= 8)
    static void dealloc_coalesced_node(uint64_t* node, uint8_t sc,
                                         BLD& bld) noexcept {
        dealloc_bm_final<BITS>(node, sc, bld);
        bld.dealloc_node(node, get_header(node)->alloc_u64());
    }

    template<int BITS> requires (BITS >= 8)
    static void dealloc_bm_chain_skip(uint64_t* node, uint8_t sc, uint8_t pos,
                                        BLD& bld) noexcept {
        if (pos >= sc) [[unlikely]] {
            dealloc_bm_final<BITS>(node, sc, bld);
            return;
        }
        if constexpr (BITS > 8) {
            dealloc_bm_chain_skip<BITS - 8>(node, sc, pos + 1, bld);
        }
    }

    template<int BITS> requires (BITS >= 8)
    static void dealloc_bm_final(uint64_t* node, uint8_t sc,
                                   BLD& bld) noexcept {
        BO::chain_for_each_child(node, sc, [&](unsigned, uint64_t child) {
            if constexpr (BITS > 8) {
                dealloc_bitmask_subtree<BITS - 8>(child, bld);
            }
        });
    }

    template<int BITS> requires (BITS >= 8)
    static void dealloc_leaf_skip(uint64_t* node, uint8_t skip,
                                    BLD& bld) noexcept {
        using NK = nk_for_bits_t<BITS>;
        if (skip == 0) [[unlikely]] {
            if constexpr (sizeof(NK) == 1)
                BO::bitmap_destroy_and_dealloc(node, bld);
            else {
                using CO = compact_ops<NK, VALUE, ALLOC>;
                CO::destroy_and_dealloc(node, bld);
            }
            return;
        }
        if constexpr (BITS > 8) {
            dealloc_leaf_skip<BITS - 8>(node, skip - 1, bld);
        }
    }

    // ==================================================================
    // Iteration — tree-level next/prev + descend first/last
    //
    // These combine descent + leaf fn dispatch in one recursive pass.
    // No sentinel checks — sentinel leaf fn returns not_found.
    // ==================================================================

    // --- descend_first: walk min path to leaf, call fn->first ---
    template<int BITS> requires (BITS >= 8)
    static leaf_result_t descend_first(uint64_t ptr) noexcept {
        if (ptr & LEAF_BIT) [[unlikely]] {
            const uint64_t* node = untag_leaf(ptr);
            return BO::leaf_fn(node)->first(node);
        }

        const uint64_t* bm = reinterpret_cast<const uint64_t*>(ptr);
        // First child is at slot 0 (bitmap children are sorted)
        if constexpr (BITS > 8)
            return descend_first<BITS - 8>(bm[BITMAP_256_U64]);
        __builtin_unreachable();
    }

    // --- descend_last: walk max path to leaf, call fn->last ---
    template<int BITS> requires (BITS >= 8)
    static leaf_result_t descend_last(uint64_t ptr) noexcept {
        if (ptr & LEAF_BIT) [[unlikely]] {
            const uint64_t* node = untag_leaf(ptr);
            return BO::leaf_fn(node)->last(node);
        }

        const uint64_t* bm = reinterpret_cast<const uint64_t*>(ptr);
        auto* hdr = get_header(bm_to_node_const(reinterpret_cast<uint64_t>(bm)));
        int last = hdr->entries() - 1;
        if constexpr (BITS > 8)
            return descend_last<BITS - 8>(bm[BITMAP_256_U64 + last]);
        __builtin_unreachable();
    }

    // --- iter_next_tree: find smallest key > ik ---
    // No sentinel checks. Sentinel leaf fn->next returns not_found.
    template<int BITS> requires (BITS >= 8)
    static leaf_result_t iter_next_tree(uint64_t ptr, uint64_t ik) noexcept {
        // LEAF — dispatch via fn pointer
        if (ptr & LEAF_BIT) [[unlikely]] {
            const uint64_t* node = untag_leaf(ptr);
            return BO::leaf_fn(node)->next(node, ik);
        }

        // BITMASK — handle skip chain then final bitmap
        const uint64_t* node = bm_to_node_const(ptr);
        uint8_t sc = get_header(node)->skip();
        if (sc > 0) [[unlikely]]
            return iter_next_chain_skip<BITS>(node, sc, 0, ik);
        return iter_next_bm_final<BITS>(node, sc, ik);
    }

    template<int BITS> requires (BITS >= 8)
    static leaf_result_t iter_next_chain_skip(
            const uint64_t* node, uint8_t sc, uint8_t pos,
            uint64_t ik) noexcept {
        if (pos >= sc) [[unlikely]]
            return iter_next_bm_final<BITS>(node, sc, ik);

        uint8_t actual = BO::skip_byte(node, pos);
        uint8_t expected = static_cast<uint8_t>(ik >> 56);

        if (expected != actual) [[unlikely]] {
            if (expected < actual) {
                // ik is before this subtree — return first
                if constexpr (BITS > 8)
                    return iter_next_bm_final_first<BITS>(node, sc);
                __builtin_unreachable();
            }
            return {0, nullptr, false}; // ik past this subtree
        }

        if constexpr (BITS > 8) {
            return iter_next_chain_skip<BITS - 8>(node, sc, pos + 1, ik << 8);
        }
        __builtin_unreachable();
    }

    // Helper: return first entry in bitmask's final bitmap
    template<int BITS> requires (BITS >= 8)
    static leaf_result_t iter_next_bm_final_first(
            const uint64_t* node, uint8_t sc) noexcept {
        const uint64_t* rch = BO::chain_children(node, sc);
        if constexpr (BITS > 8)
            return descend_first<BITS - 8>(rch[0]);
        __builtin_unreachable();
    }

    template<int BITS> requires (BITS >= 8)
    static leaf_result_t iter_next_bm_final(
            const uint64_t* node, uint8_t sc,
            uint64_t ik) noexcept {
        const bitmap_256_t& fbm = BO::chain_bitmap(node, sc);
        const uint64_t* children = BO::chain_children(node, sc);
        uint8_t byte = static_cast<uint8_t>(ik >> 56);

        int slot = fbm.find_slot<slot_mode::FAST_EXIT>(byte);
        if (slot >= 0) [[likely]] {
            if constexpr (BITS > 8) {
                auto r = iter_next_tree<BITS - 8>(children[slot], ik << 8);
                if (r.found) [[likely]] return r;
            }
        }

        // Exhausted subtree or byte not set — find next sibling
        auto adj = fbm.next_set_after(byte);
        if (adj.found) [[unlikely]] {
            if constexpr (BITS > 8)
                return descend_first<BITS - 8>(children[adj.slot]);
        }
        return {0, nullptr, false};
    }

    // --- iter_prev_tree: find largest key < ik ---
    template<int BITS> requires (BITS >= 8)
    static leaf_result_t iter_prev_tree(uint64_t ptr, uint64_t ik) noexcept {
        if (ptr & LEAF_BIT) [[unlikely]] {
            const uint64_t* node = untag_leaf(ptr);
            return BO::leaf_fn(node)->prev(node, ik);
        }

        const uint64_t* node = bm_to_node_const(ptr);
        uint8_t sc = get_header(node)->skip();
        if (sc > 0) [[unlikely]]
            return iter_prev_chain_skip<BITS>(node, sc, 0, ik);
        return iter_prev_bm_final<BITS>(node, sc, ik);
    }

    template<int BITS> requires (BITS >= 8)
    static leaf_result_t iter_prev_chain_skip(
            const uint64_t* node, uint8_t sc, uint8_t pos,
            uint64_t ik) noexcept {
        if (pos >= sc) [[unlikely]]
            return iter_prev_bm_final<BITS>(node, sc, ik);

        uint8_t actual = BO::skip_byte(node, pos);
        uint8_t expected = static_cast<uint8_t>(ik >> 56);

        if (expected != actual) [[unlikely]] {
            if (expected > actual) {
                // ik is after this subtree — return last
                if constexpr (BITS > 8)
                    return iter_prev_bm_final_last<BITS>(node, sc);
                __builtin_unreachable();
            }
            return {0, nullptr, false}; // ik before this subtree
        }

        if constexpr (BITS > 8) {
            return iter_prev_chain_skip<BITS - 8>(node, sc, pos + 1, ik << 8);
        }
        __builtin_unreachable();
    }

    template<int BITS> requires (BITS >= 8)
    static leaf_result_t iter_prev_bm_final_last(
            const uint64_t* node, uint8_t sc) noexcept {
        const uint64_t* rch = BO::chain_children(node, sc);
        int last = get_header(node)->entries() - 1;
        if constexpr (BITS > 8)
            return descend_last<BITS - 8>(rch[last]);
        __builtin_unreachable();
    }

    template<int BITS> requires (BITS >= 8)
    static leaf_result_t iter_prev_bm_final(
            const uint64_t* node, uint8_t sc,
            uint64_t ik) noexcept {
        const bitmap_256_t& fbm = BO::chain_bitmap(node, sc);
        const uint64_t* children = BO::chain_children(node, sc);
        uint8_t byte = static_cast<uint8_t>(ik >> 56);

        int slot = fbm.find_slot<slot_mode::FAST_EXIT>(byte);
        if (slot >= 0) [[likely]] {
            if constexpr (BITS > 8) {
                auto r = iter_prev_tree<BITS - 8>(children[slot], ik << 8);
                if (r.found) [[likely]] return r;
            }
        }

        auto adj = fbm.prev_set_before(byte);
        if (adj.found) [[unlikely]] {
            if constexpr (BITS > 8)
                return descend_last<BITS - 8>(children[adj.slot]);
        }
        return {0, nullptr, false};
    }
};

} // namespace gteitelbaum

#endif // KNTRIE_OPS_HPP
