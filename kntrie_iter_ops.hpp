#ifndef KNTRIE_ITER_OPS_HPP
#define KNTRIE_ITER_OPS_HPP

#include "kntrie_bitmask.hpp"
#include "kntrie_compact.hpp"

namespace gteitelbaum {

// Standalone stats accumulator — shared across all NK instantiations.
struct kntrie_stats_t {
    size_t total_bytes    = 0;
    size_t total_entries  = 0;
    size_t bitmap_leaves  = 0;
    size_t compact_leaves = 0;
    size_t bitmask_nodes  = 0;
    size_t bm_children    = 0;
};

// ======================================================================
// kntrie_iter_ops<NK, VALUE, ALLOC> — iteration, destroy, stats.
//
// Standalone stateless struct. Same NK narrowing pattern as kntrie_ops.
// ======================================================================

template<typename NK, typename VALUE, typename ALLOC>
struct kntrie_iter_ops {
    using BO  = bitmask_ops<VALUE, ALLOC>;
    using VT  = value_traits<VALUE, ALLOC>;
    using VST = typename VT::slot_type;
    using CO  = compact_ops<NK, VALUE, ALLOC>;

    static constexpr int NK_BITS = sizeof(NK) * 8;

    using NNK    = next_narrow_t<NK>;
    using NARROW = kntrie_iter_ops<NNK, VALUE, ALLOC>;

    // ==================================================================

    // ==================================================================

    // ==================================================================
    // Iteration — compile-time recursive, mirrors find/erase pattern.
    //
    // NK ik: narrowing key (byte-at-a-time, same as find/insert/erase).
    // IK prefix + int bits: accumulated result key from root.
    // BITS: compile-time remaining key bits for NK narrowing.
    // ==================================================================

    // --- leaf dispatch helpers (compile-time CO/BO) ---
    // Suffix is NK-typed: stored value with remaining key in top bits of NK.
    // To position in IK: shift left to top of IK, then right by consumed bits.
    // Formula: prefix | ((IK(suffix) << (IK_BITS - NK_BITS)) >> bits)

    template<typename IK>
    static iter_ops_result_t<IK, VST> leaf_first(const uint64_t* node,
                                                    const node_header_t* hdr,
                                                    IK prefix, int bits) noexcept {
        constexpr int IK_BITS = sizeof(IK) * 8;
        if constexpr (sizeof(NK) == 1) {
            size_t hs = 1 + (hdr->is_skip() ? 1 : 0);
            auto r = BO::bitmap_iter_first(node, hs);
            IK contrib = (IK(r.suffix) << (IK_BITS - NK_BITS)) >> bits;
            return {prefix | contrib, r.value, true};
        } else {
            auto r = CO::iter_first(node, hdr);
            IK contrib = (IK(r.suffix) << (IK_BITS - NK_BITS)) >> bits;
            return {prefix | contrib, r.value, true};
        }
    }

    template<typename IK>
    static iter_ops_result_t<IK, VST> leaf_last(const uint64_t* node,
                                                   const node_header_t* hdr,
                                                   IK prefix, int bits) noexcept {
        constexpr int IK_BITS = sizeof(IK) * 8;
        if constexpr (sizeof(NK) == 1) {
            size_t hs = 1 + (hdr->is_skip() ? 1 : 0);
            auto r = BO::bitmap_iter_last(node, *hdr, hs);
            IK contrib = (IK(r.suffix) << (IK_BITS - NK_BITS)) >> bits;
            return {prefix | contrib, r.value, true};
        } else {
            auto r = CO::iter_last(node, hdr);
            IK contrib = (IK(r.suffix) << (IK_BITS - NK_BITS)) >> bits;
            return {prefix | contrib, r.value, true};
        }
    }

    template<typename IK>
    static iter_ops_result_t<IK, VST> leaf_next(const uint64_t* node,
                                                   const node_header_t* hdr,
                                                   NK suf, IK prefix, int bits) noexcept {
        constexpr int IK_BITS = sizeof(IK) * 8;
        if constexpr (sizeof(NK) == 1) {
            size_t hs = 1 + (hdr->is_skip() ? 1 : 0);
            auto r = BO::bitmap_iter_next(node, static_cast<uint8_t>(suf), hs);
            if (!r.found) [[unlikely]] return {IK{}, nullptr, false};
            IK contrib = (IK(r.suffix) << (IK_BITS - NK_BITS)) >> bits;
            return {prefix | contrib, r.value, true};
        } else {
            auto r = CO::iter_next(node, hdr, suf);
            if (!r.found) [[unlikely]] return {IK{}, nullptr, false};
            IK contrib = (IK(r.suffix) << (IK_BITS - NK_BITS)) >> bits;
            return {prefix | contrib, r.value, true};
        }
    }

    template<typename IK>
    static iter_ops_result_t<IK, VST> leaf_prev(const uint64_t* node,
                                                   const node_header_t* hdr,
                                                   NK suf, IK prefix, int bits) noexcept {
        constexpr int IK_BITS = sizeof(IK) * 8;
        if constexpr (sizeof(NK) == 1) {
            size_t hs = 1 + (hdr->is_skip() ? 1 : 0);
            auto r = BO::bitmap_iter_prev(node, static_cast<uint8_t>(suf), hs);
            if (!r.found) [[unlikely]] return {IK{}, nullptr, false};
            IK contrib = (IK(r.suffix) << (IK_BITS - NK_BITS)) >> bits;
            return {prefix | contrib, r.value, true};
        } else {
            auto r = CO::iter_prev(node, hdr, suf);
            if (!r.found) [[unlikely]] return {IK{}, nullptr, false};
            IK contrib = (IK(r.suffix) << (IK_BITS - NK_BITS)) >> bits;
            return {prefix | contrib, r.value, true};
        }
    }

    // ==================================================================
    // descend_min: walk always-min path
    // Compile-time recursive through skips (leaf prefix + chain embed).
    // ==================================================================

    template<int BITS, typename IK> requires (BITS >= 8)
    static iter_ops_result_t<IK, VST> descend_min(uint64_t ptr,
                                                     IK prefix, int bits) noexcept {
        if (ptr & LEAF_BIT) {
            const uint64_t* node = untag_leaf(ptr);
            auto* hdr = get_header(node);
            if (hdr->entries() == 0) return {IK{}, nullptr, false};
            uint8_t skip = hdr->skip();
            if (skip)
                return descend_min_leaf_skip<BITS, IK>(
                    node, hdr, hdr->prefix_bytes(), skip, 0, prefix, bits);
            return leaf_first<IK>(node, hdr, prefix, bits);
        }

        const uint64_t* node = bm_to_node_const(ptr);
        uint8_t sc = get_header(node)->skip();
        if (sc > 0)
            return descend_min_chain_skip<BITS, IK>(node, sc, 0, prefix, bits);
        return descend_min_bm_final<BITS, IK>(node, sc, prefix, bits);
    }

    // Leaf prefix: accumulate bytes, compile-time narrow
    template<int BITS, typename IK> requires (BITS >= 8)
    static iter_ops_result_t<IK, VST> descend_min_leaf_skip(
            const uint64_t* node, const node_header_t* hdr,
            const uint8_t* pb, uint8_t skip, uint8_t pos,
            IK prefix, int bits) noexcept {
        constexpr int IK_BITS = sizeof(IK) * 8;
        if (pos >= skip)
            return leaf_first<IK>(node, hdr, prefix, bits);

        prefix |= IK(pb[pos]) << (IK_BITS - bits - 8);
        if constexpr (BITS > 8) {
            if constexpr (BITS - 8 == NK_BITS / 2 && NK_BITS > 8)
                return NARROW::template descend_min_leaf_skip<BITS - 8, IK>(
                    node, hdr, pb, skip, pos + 1, prefix, bits + 8);
            else
                return descend_min_leaf_skip<BITS - 8, IK>(
                    node, hdr, pb, skip, pos + 1, prefix, bits + 8);
        }
        __builtin_unreachable();
    }

    // Chain embed: accumulate bytes, compile-time narrow
    template<int BITS, typename IK> requires (BITS >= 8)
    static iter_ops_result_t<IK, VST> descend_min_chain_skip(
            const uint64_t* node, uint8_t sc, uint8_t pos,
            IK prefix, int bits) noexcept {
        constexpr int IK_BITS = sizeof(IK) * 8;
        if (pos >= sc)
            return descend_min_bm_final<BITS, IK>(node, sc, prefix, bits);

        prefix |= IK(BO::skip_byte(node, pos)) << (IK_BITS - bits - 8);
        if constexpr (BITS > 8) {
            if constexpr (BITS - 8 == NK_BITS / 2 && NK_BITS > 8)
                return NARROW::template descend_min_chain_skip<BITS - 8, IK>(
                    node, sc, pos + 1, prefix, bits + 8);
            else
                return descend_min_chain_skip<BITS - 8, IK>(
                    node, sc, pos + 1, prefix, bits + 8);
        }
        __builtin_unreachable();
    }

    // Final bitmap: first child, recurse
    template<int BITS, typename IK> requires (BITS >= 8)
    static iter_ops_result_t<IK, VST> descend_min_bm_final(
            const uint64_t* node, uint8_t sc,
            IK prefix, int bits) noexcept {
        constexpr int IK_BITS = sizeof(IK) * 8;
        const bitmap_256_t& fbm = BO::chain_bitmap(node, sc);
        uint8_t byte = fbm.first_set_bit();
        prefix |= IK(byte) << (IK_BITS - bits - 8);
        uint64_t child = BO::chain_children(node, sc)[0];

        if constexpr (BITS > 8) {
            if constexpr (BITS - 8 == NK_BITS / 2 && NK_BITS > 8)
                return NARROW::template descend_min<BITS - 8, IK>(
                    child, prefix, bits + 8);
            else
                return descend_min<BITS - 8, IK>(child, prefix, bits + 8);
        }
        __builtin_unreachable();
    }

    // ==================================================================
    // descend_max: walk always-max path (same structure as min)
    // ==================================================================

    template<int BITS, typename IK> requires (BITS >= 8)
    static iter_ops_result_t<IK, VST> descend_max(uint64_t ptr,
                                                     IK prefix, int bits) noexcept {
        if (ptr & LEAF_BIT) {
            const uint64_t* node = untag_leaf(ptr);
            auto* hdr = get_header(node);
            if (hdr->entries() == 0) return {IK{}, nullptr, false};
            uint8_t skip = hdr->skip();
            if (skip)
                return descend_max_leaf_skip<BITS, IK>(
                    node, hdr, hdr->prefix_bytes(), skip, 0, prefix, bits);
            return leaf_last<IK>(node, hdr, prefix, bits);
        }

        const uint64_t* node = bm_to_node_const(ptr);
        uint8_t sc = get_header(node)->skip();
        if (sc > 0)
            return descend_max_chain_skip<BITS, IK>(node, sc, 0, prefix, bits);
        return descend_max_bm_final<BITS, IK>(node, sc, prefix, bits);
    }

    template<int BITS, typename IK> requires (BITS >= 8)
    static iter_ops_result_t<IK, VST> descend_max_leaf_skip(
            const uint64_t* node, const node_header_t* hdr,
            const uint8_t* pb, uint8_t skip, uint8_t pos,
            IK prefix, int bits) noexcept {
        constexpr int IK_BITS = sizeof(IK) * 8;
        if (pos >= skip)
            return leaf_last<IK>(node, hdr, prefix, bits);
        prefix |= IK(pb[pos]) << (IK_BITS - bits - 8);
        if constexpr (BITS > 8) {
            if constexpr (BITS - 8 == NK_BITS / 2 && NK_BITS > 8)
                return NARROW::template descend_max_leaf_skip<BITS - 8, IK>(
                    node, hdr, pb, skip, pos + 1, prefix, bits + 8);
            else
                return descend_max_leaf_skip<BITS - 8, IK>(
                    node, hdr, pb, skip, pos + 1, prefix, bits + 8);
        }
        __builtin_unreachable();
    }

    template<int BITS, typename IK> requires (BITS >= 8)
    static iter_ops_result_t<IK, VST> descend_max_chain_skip(
            const uint64_t* node, uint8_t sc, uint8_t pos,
            IK prefix, int bits) noexcept {
        constexpr int IK_BITS = sizeof(IK) * 8;
        if (pos >= sc)
            return descend_max_bm_final<BITS, IK>(node, sc, prefix, bits);
        prefix |= IK(BO::skip_byte(node, pos)) << (IK_BITS - bits - 8);
        if constexpr (BITS > 8) {
            if constexpr (BITS - 8 == NK_BITS / 2 && NK_BITS > 8)
                return NARROW::template descend_max_chain_skip<BITS - 8, IK>(
                    node, sc, pos + 1, prefix, bits + 8);
            else
                return descend_max_chain_skip<BITS - 8, IK>(
                    node, sc, pos + 1, prefix, bits + 8);
        }
        __builtin_unreachable();
    }

    template<int BITS, typename IK> requires (BITS >= 8)
    static iter_ops_result_t<IK, VST> descend_max_bm_final(
            const uint64_t* node, uint8_t sc,
            IK prefix, int bits) noexcept {
        constexpr int IK_BITS = sizeof(IK) * 8;
        const bitmap_256_t& fbm = BO::chain_bitmap(node, sc);
        uint8_t byte = fbm.last_set_bit();
        int slot = get_header(node)->entries() - 1;
        prefix |= IK(byte) << (IK_BITS - bits - 8);
        uint64_t child = BO::chain_children(node, sc)[slot];
        if constexpr (BITS > 8) {
            if constexpr (BITS - 8 == NK_BITS / 2 && NK_BITS > 8)
                return NARROW::template descend_max<BITS - 8, IK>(
                    child, prefix, bits + 8);
            else
                return descend_max<BITS - 8, IK>(child, prefix, bits + 8);
        }
        __builtin_unreachable();
    }

    // ==================================================================
    // iter_next_node: find smallest key > ik
    // NK ik narrows byte-by-byte like find/erase.
    // ==================================================================

    template<int BITS, typename IK> requires (BITS >= 8)
    static iter_ops_result_t<IK, VST> iter_next_node(uint64_t ptr, NK ik,
                                                        IK prefix, int bits) noexcept {
        // --- LEAF ---
        if (ptr & LEAF_BIT) [[likely]] {
            const uint64_t* node = untag_leaf(ptr);
            auto* hdr = get_header(node);
            uint8_t skip = hdr->skip();
            if (skip) [[unlikely]]
                return iter_next_leaf_skip<BITS, IK>(
                    node, hdr, ik, hdr->prefix_bytes(), skip, 0, prefix, bits);
            return leaf_next<IK>(node, hdr, ik, prefix, bits);
        }

        // --- BITMASK ---
        const uint64_t* node = bm_to_node_const(ptr);
        uint8_t sc = get_header(node)->skip();
        if (sc > 0)
            return iter_next_chain_skip<BITS, IK>(node, sc, ik, 0, prefix, bits);
        return iter_next_bm_final<BITS, IK>(node, sc, ik, prefix, bits);
    }

    // Leaf prefix: compare byte-by-byte, narrow NK
    template<int BITS, typename IK> requires (BITS >= 8)
    static iter_ops_result_t<IK, VST> iter_next_leaf_skip(
            const uint64_t* node, const node_header_t* hdr,
            NK ik, const uint8_t* pb, uint8_t skip, uint8_t pos,
            IK prefix, int bits) noexcept {
        constexpr int IK_BITS = sizeof(IK) * 8;
        if (pos >= skip)
            return leaf_next<IK>(node, hdr, ik, prefix, bits);

        uint8_t kb = static_cast<uint8_t>(ik >> (NK_BITS - 8));
        if (kb < pb[pos]) [[unlikely]] {
            // Prefix > key: descend min through remaining prefix
            return descend_min_leaf_skip<BITS, IK>(
                node, hdr, pb, skip, pos, prefix, bits);
        }
        if (kb > pb[pos]) [[unlikely]] return {IK{}, nullptr, false};

        prefix |= IK(pb[pos]) << (IK_BITS - bits - 8);
        if constexpr (BITS > 8) {
            NK shifted = static_cast<NK>(ik << 8);
            if constexpr (BITS - 8 == NK_BITS / 2 && NK_BITS > 8)
                return NARROW::template iter_next_leaf_skip<BITS - 8, IK>(
                    node, hdr,
                    static_cast<NNK>(shifted >> (NK_BITS / 2)),
                    pb, skip, pos + 1, prefix, bits + 8);
            else
                return iter_next_leaf_skip<BITS - 8, IK>(
                    node, hdr, shifted, pb, skip, pos + 1, prefix, bits + 8);
        }
        __builtin_unreachable();
    }

    // Chain embed: compare byte-by-byte, narrow NK
    template<int BITS, typename IK> requires (BITS >= 8)
    static iter_ops_result_t<IK, VST> iter_next_chain_skip(
            const uint64_t* node, uint8_t sc,
            NK ik, uint8_t pos,
            IK prefix, int bits) noexcept {
        constexpr int IK_BITS = sizeof(IK) * 8;
        if (pos >= sc)
            return iter_next_bm_final<BITS, IK>(node, sc, ik, prefix, bits);

        uint8_t kb = static_cast<uint8_t>(ik >> (NK_BITS - 8));
        uint8_t sb = BO::skip_byte(node, pos);
        if (kb < sb) [[unlikely]] {
            // Chain byte > key: descend min from here
            return descend_min_chain_skip<BITS, IK>(node, sc, pos, prefix, bits);
        }
        if (kb > sb) [[unlikely]] return {IK{}, nullptr, false};

        prefix |= IK(sb) << (IK_BITS - bits - 8);
        if constexpr (BITS > 8) {
            NK shifted = static_cast<NK>(ik << 8);
            if constexpr (BITS - 8 == NK_BITS / 2 && NK_BITS > 8)
                return NARROW::template iter_next_chain_skip<BITS - 8, IK>(
                    node, sc,
                    static_cast<NNK>(shifted >> (NK_BITS / 2)),
                    pos + 1, prefix, bits + 8);
            else
                return iter_next_chain_skip<BITS - 8, IK>(
                    node, sc, shifted, pos + 1, prefix, bits + 8);
        }
        __builtin_unreachable();
    }

    // Final bitmap: lookup byte, recurse child or next sibling.
    // BRANCHLESS: single branchless scan, returns 1-based slot (0 = not found).
    template<int BITS, typename IK> requires (BITS >= 8)
    static iter_ops_result_t<IK, VST> iter_next_bm_final(
            const uint64_t* node, uint8_t sc,
            NK ik, IK prefix, int bits) noexcept {
        constexpr int IK_BITS = sizeof(IK) * 8;
        const bitmap_256_t& fbm = BO::chain_bitmap(node, sc);
        const uint64_t* children = BO::chain_children(node, sc);
        uint8_t byte = static_cast<uint8_t>(ik >> (NK_BITS - 8));

        // Single branchless scan: 1-based slot, 0 = not found
        int slot = fbm.template find_slot<slot_mode::BRANCHLESS>(byte);
        if (slot) [[likely]] {
            IK cp = prefix | (IK(byte) << (IK_BITS - bits - 8));

            if constexpr (BITS > 8) {
                NK shifted = static_cast<NK>(ik << 8);
                iter_ops_result_t<IK, VST> r;
                if constexpr (BITS - 8 == NK_BITS / 2 && NK_BITS > 8)
                    r = NARROW::template iter_next_node<BITS - 8, IK>(
                        children[slot - 1],
                        static_cast<NNK>(shifted >> (NK_BITS / 2)),
                        cp, bits + 8);
                else
                    r = iter_next_node<BITS - 8, IK>(
                        children[slot - 1], shifted, cp, bits + 8);
                if (r.found) [[likely]] return r;
            }
        }

        // Rare: byte not set or exhausted subtree, find next sibling
        auto adj = fbm.next_set_after(byte);
        if (adj.found) [[unlikely]] {
            IK np = prefix | (IK(adj.idx) << (IK_BITS - bits - 8));
            if constexpr (BITS > 8) {
                if constexpr (BITS - 8 == NK_BITS / 2 && NK_BITS > 8)
                    return NARROW::template descend_min<BITS - 8, IK>(
                        children[adj.slot], np, bits + 8);
                else
                    return descend_min<BITS - 8, IK>(
                        children[adj.slot], np, bits + 8);
            }
        }
        return {IK{}, nullptr, false};
    }

    // ==================================================================
    // iter_prev_node: find largest key < ik (mirrors iter_next)
    // ==================================================================

    template<int BITS, typename IK> requires (BITS >= 8)
    static iter_ops_result_t<IK, VST> iter_prev_node(uint64_t ptr, NK ik,
                                                        IK prefix, int bits) noexcept {
        if (ptr & LEAF_BIT) [[likely]] {
            const uint64_t* node = untag_leaf(ptr);
            auto* hdr = get_header(node);
            uint8_t skip = hdr->skip();
            if (skip) [[unlikely]]
                return iter_prev_leaf_skip<BITS, IK>(
                    node, hdr, ik, hdr->prefix_bytes(), skip, 0, prefix, bits);
            return leaf_prev<IK>(node, hdr, ik, prefix, bits);
        }

        const uint64_t* node = bm_to_node_const(ptr);
        uint8_t sc = get_header(node)->skip();
        if (sc > 0)
            return iter_prev_chain_skip<BITS, IK>(node, sc, ik, 0, prefix, bits);
        return iter_prev_bm_final<BITS, IK>(node, sc, ik, prefix, bits);
    }

    template<int BITS, typename IK> requires (BITS >= 8)
    static iter_ops_result_t<IK, VST> iter_prev_leaf_skip(
            const uint64_t* node, const node_header_t* hdr,
            NK ik, const uint8_t* pb, uint8_t skip, uint8_t pos,
            IK prefix, int bits) noexcept {
        constexpr int IK_BITS = sizeof(IK) * 8;
        if (pos >= skip)
            return leaf_prev<IK>(node, hdr, ik, prefix, bits);

        uint8_t kb = static_cast<uint8_t>(ik >> (NK_BITS - 8));
        if (kb > pb[pos]) [[unlikely]] {
            return descend_max_leaf_skip<BITS, IK>(
                node, hdr, pb, skip, pos, prefix, bits);
        }
        if (kb < pb[pos]) [[unlikely]] return {IK{}, nullptr, false};

        prefix |= IK(pb[pos]) << (IK_BITS - bits - 8);
        if constexpr (BITS > 8) {
            NK shifted = static_cast<NK>(ik << 8);
            if constexpr (BITS - 8 == NK_BITS / 2 && NK_BITS > 8)
                return NARROW::template iter_prev_leaf_skip<BITS - 8, IK>(
                    node, hdr,
                    static_cast<NNK>(shifted >> (NK_BITS / 2)),
                    pb, skip, pos + 1, prefix, bits + 8);
            else
                return iter_prev_leaf_skip<BITS - 8, IK>(
                    node, hdr, shifted, pb, skip, pos + 1, prefix, bits + 8);
        }
        __builtin_unreachable();
    }

    template<int BITS, typename IK> requires (BITS >= 8)
    static iter_ops_result_t<IK, VST> iter_prev_chain_skip(
            const uint64_t* node, uint8_t sc,
            NK ik, uint8_t pos,
            IK prefix, int bits) noexcept {
        constexpr int IK_BITS = sizeof(IK) * 8;
        if (pos >= sc)
            return iter_prev_bm_final<BITS, IK>(node, sc, ik, prefix, bits);

        uint8_t kb = static_cast<uint8_t>(ik >> (NK_BITS - 8));
        uint8_t sb = BO::skip_byte(node, pos);
        if (kb > sb) [[unlikely]] {
            return descend_max_chain_skip<BITS, IK>(node, sc, pos, prefix, bits);
        }
        if (kb < sb) [[unlikely]] return {IK{}, nullptr, false};

        prefix |= IK(sb) << (IK_BITS - bits - 8);
        if constexpr (BITS > 8) {
            NK shifted = static_cast<NK>(ik << 8);
            if constexpr (BITS - 8 == NK_BITS / 2 && NK_BITS > 8)
                return NARROW::template iter_prev_chain_skip<BITS - 8, IK>(
                    node, sc,
                    static_cast<NNK>(shifted >> (NK_BITS / 2)),
                    pos + 1, prefix, bits + 8);
            else
                return iter_prev_chain_skip<BITS - 8, IK>(
                    node, sc, shifted, pos + 1, prefix, bits + 8);
        }
        __builtin_unreachable();
    }

    template<int BITS, typename IK> requires (BITS >= 8)
    static iter_ops_result_t<IK, VST> iter_prev_bm_final(
            const uint64_t* node, uint8_t sc,
            NK ik, IK prefix, int bits) noexcept {
        constexpr int IK_BITS = sizeof(IK) * 8;
        const bitmap_256_t& fbm = BO::chain_bitmap(node, sc);
        const uint64_t* children = BO::chain_children(node, sc);
        uint8_t byte = static_cast<uint8_t>(ik >> (NK_BITS - 8));

        // Single branchless scan: 1-based slot, 0 = not found
        int slot = fbm.template find_slot<slot_mode::BRANCHLESS>(byte);
        if (slot) [[likely]] {
            IK cp = prefix | (IK(byte) << (IK_BITS - bits - 8));

            if constexpr (BITS > 8) {
                NK shifted = static_cast<NK>(ik << 8);
                iter_ops_result_t<IK, VST> r;
                if constexpr (BITS - 8 == NK_BITS / 2 && NK_BITS > 8)
                    r = NARROW::template iter_prev_node<BITS - 8, IK>(
                        children[slot - 1],
                        static_cast<NNK>(shifted >> (NK_BITS / 2)),
                        cp, bits + 8);
                else
                    r = iter_prev_node<BITS - 8, IK>(
                        children[slot - 1], shifted, cp, bits + 8);
                if (r.found) [[likely]] return r;
            }
        }

        // Rare: byte not set or exhausted subtree, find previous sibling
        auto adj = fbm.prev_set_before(byte);
        if (adj.found) [[unlikely]] {
            IK np = prefix | (IK(adj.idx) << (IK_BITS - bits - 8));
            if constexpr (BITS > 8) {
                if constexpr (BITS - 8 == NK_BITS / 2 && NK_BITS > 8)
                    return NARROW::template descend_max<BITS - 8, IK>(
                        children[adj.slot], np, bits + 8);
                else
                    return descend_max<BITS - 8, IK>(
                        children[adj.slot], np, bits + 8);
            }
        }
        return {IK{}, nullptr, false};
    }


    // ==================================================================
    // Destroy leaf: compile-time NK dispatch (replaces suffix_type switch)
    // ==================================================================

    static void destroy_leaf(uint64_t* node, ALLOC& alloc) noexcept {
        if constexpr (sizeof(NK) == 1)
            BO::bitmap_destroy_and_dealloc(node, alloc);
        else
            CO::destroy_and_dealloc(node, alloc);
    }

    // ==================================================================
    // Remove subtree: recursive with compile-time NK narrowing
    // Replaces impl::remove_node_ + destroy_leaf (eliminates suffix_type)
    // ==================================================================

    template<int BITS> requires (BITS >= 8)
    static void remove_subtree(uint64_t tagged, ALLOC& alloc) noexcept {
        if (tagged == SENTINEL_TAGGED) return;

        if (tagged & LEAF_BIT) {
            uint64_t* node = untag_leaf_mut(tagged);
            auto* hdr = get_header(node);
            uint8_t skip = hdr->skip();
            if (skip)
                remove_leaf_skip<BITS>(node, skip, alloc);
            else
                destroy_leaf(node, alloc);
            return;
        }

        uint64_t* node = bm_to_node(tagged);
        auto* hdr = get_header(node);
        uint8_t sc = hdr->skip();
        if (sc > 0)
            remove_chain_skip<BITS>(node, sc, 0, alloc);
        else
            remove_bm_final<BITS>(node, sc, alloc);
        BO::dealloc_bitmask(node, alloc);
    }

    // Leaf skip: consume prefix bytes, narrow NK, then destroy
    template<int BITS> requires (BITS >= 8)
    static void remove_leaf_skip(uint64_t* node, uint8_t skip,
                                   ALLOC& alloc) noexcept {
        if (skip == 0) {
            destroy_leaf(node, alloc);
            return;
        }
        if constexpr (BITS > 8) {
            if constexpr (BITS - 8 == NK_BITS / 2 && NK_BITS > 8)
                NARROW::template remove_leaf_skip<BITS - 8>(node, skip - 1, alloc);
            else
                remove_leaf_skip<BITS - 8>(node, skip - 1, alloc);
        }
    }

    // Chain embed: consume skip bytes, narrow, then final bitmap
    template<int BITS> requires (BITS >= 8)
    static void remove_chain_skip(uint64_t* node, uint8_t sc, uint8_t pos,
                                    ALLOC& alloc) noexcept {
        if (pos >= sc) {
            remove_bm_final<BITS>(node, sc, alloc);
            return;
        }
        if constexpr (BITS > 8) {
            if constexpr (BITS - 8 == NK_BITS / 2 && NK_BITS > 8)
                NARROW::template remove_chain_skip<BITS - 8>(node, sc, pos + 1, alloc);
            else
                remove_chain_skip<BITS - 8>(node, sc, pos + 1, alloc);
        }
    }

    // Final bitmap: recurse each child with narrowing
    template<int BITS> requires (BITS >= 8)
    static void remove_bm_final(uint64_t* node, uint8_t sc,
                                  ALLOC& alloc) noexcept {
        BO::chain_for_each_child(node, sc, [&](unsigned, uint64_t child) {
            if constexpr (BITS > 8) {
                if constexpr (BITS - 8 == NK_BITS / 2 && NK_BITS > 8)
                    NARROW::template remove_subtree<BITS - 8>(child, alloc);
                else
                    remove_subtree<BITS - 8>(child, alloc);
            }
        });
    }

    // ==================================================================
    // Stats collection: compile-time NK narrowing (replaces suffix_type)
    // ==================================================================

    using stats_t = kntrie_stats_t;

    template<int BITS> requires (BITS >= 8)
    static void collect_stats(uint64_t tagged, stats_t& s) noexcept {
        if (tagged & LEAF_BIT) {
            const uint64_t* node = untag_leaf(tagged);
            auto* hdr = get_header(node);
            s.total_bytes += static_cast<size_t>(hdr->alloc_u64()) * 8;
            s.total_entries += hdr->entries();
            uint8_t skip = hdr->skip();
            if (skip)
                stats_leaf_skip<BITS>(node, skip, s);
            else {
                if constexpr (sizeof(NK) == 1) s.bitmap_leaves++;
                else                           s.compact_leaves++;
            }
            return;
        }

        const uint64_t* node = bm_to_node_const(tagged);
        auto* hdr = get_header(node);
        s.total_bytes += static_cast<size_t>(hdr->alloc_u64()) * 8;
        s.bitmask_nodes++;
        s.bm_children += hdr->entries();
        uint8_t sc = hdr->skip();
        if (sc > 0)
            stats_chain_skip<BITS>(node, sc, 0, s);
        else
            stats_bm_final<BITS>(node, sc, s);
    }

    template<int BITS> requires (BITS >= 8)
    static void stats_leaf_skip(const uint64_t*, uint8_t skip,
                                  stats_t& s) noexcept {
        if (skip == 0) {
            if constexpr (sizeof(NK) == 1) s.bitmap_leaves++;
            else                           s.compact_leaves++;
            return;
        }
        if constexpr (BITS > 8) {
            if constexpr (BITS - 8 == NK_BITS / 2 && NK_BITS > 8)
                NARROW::template stats_leaf_skip<BITS - 8>(nullptr, skip - 1, s);
            else
                stats_leaf_skip<BITS - 8>(nullptr, skip - 1, s);
        }
    }

    template<int BITS> requires (BITS >= 8)
    static void stats_chain_skip(const uint64_t* node, uint8_t sc, uint8_t pos,
                                   stats_t& s) noexcept {
        if (pos >= sc) {
            stats_bm_final<BITS>(node, sc, s);
            return;
        }
        if constexpr (BITS > 8) {
            if constexpr (BITS - 8 == NK_BITS / 2 && NK_BITS > 8)
                NARROW::template stats_chain_skip<BITS - 8>(node, sc, pos + 1, s);
            else
                stats_chain_skip<BITS - 8>(node, sc, pos + 1, s);
        }
    }

    template<int BITS> requires (BITS >= 8)
    static void stats_bm_final(const uint64_t* node, uint8_t sc,
                                 stats_t& s) noexcept {
        BO::chain_for_each_child(node, sc, [&](unsigned, uint64_t child) {
            if constexpr (BITS > 8) {
                if constexpr (BITS - 8 == NK_BITS / 2 && NK_BITS > 8)
                    NARROW::template collect_stats<BITS - 8>(child, s);
                else
                    collect_stats<BITS - 8>(child, s);
            }
        });
    }

};

} // namespace gteitelbaum

#endif // KNTRIE_ITER_OPS_HPP
