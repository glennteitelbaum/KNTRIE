#ifndef KNTRIE_ITER_OPS_HPP
#define KNTRIE_ITER_OPS_HPP

#include "kntrie_bitmask.hpp"
#include "kntrie_compact.hpp"
#include "kntrie_ops.hpp"

namespace gteitelbaum {

// Standalone stats accumulator
struct kntrie_stats_t {
    size_t total_bytes    = 0;
    size_t total_entries  = 0;
    size_t bitmap_leaves  = 0;
    size_t compact_leaves = 0;
    size_t bitmask_nodes  = 0;
    size_t bm_children    = 0;
};

// ======================================================================
// kntrie_iter_ops<VALUE, ALLOC> — destroy, stats.
//
// Iteration moved to fn-pointer dispatch in kntrie_ops.
// All functions take uint64_t ik. No NK narrowing.
// ======================================================================

template<typename VALUE, typename ALLOC, int KEY_BITS>
struct kntrie_iter_ops {
    using BO  = bitmask_ops<VALUE, ALLOC>;
    using VT  = value_traits<VALUE, ALLOC>;
    using VST = typename VT::slot_type;
    using BLD = builder<VALUE, VT::IS_TRIVIAL, ALLOC>;
    using OPS = kntrie_ops<VALUE, ALLOC, KEY_BITS>;

    // ==================================================================
    // Destroy leaf: compile-time NK dispatch via BITS
    // ==================================================================

    template<int BITS>
    static void destroy_leaf(uint64_t* node, BLD& bld) noexcept {
        using NK = nk_for_bits_t<BITS>;
        if constexpr (sizeof(NK) == 1)
            BO::bitmap_destroy_and_dealloc(node, bld);
        else {
            using CO = compact_ops<NK, VALUE, ALLOC>;
            CO::destroy_and_dealloc(node, bld);
        }
    }

    // ==================================================================
    // Remove subtree: recursive, no NK narrowing
    // ==================================================================

    template<int BITS> requires (BITS >= 8)
    static void remove_subtree(uint64_t tagged, BLD& bld) noexcept {
        if (tagged == BO::SENTINEL_TAGGED) return;

        if (tagged & LEAF_BIT) {
            uint64_t* node = untag_leaf_mut(tagged);
            auto* hdr = get_header(node);
            uint8_t skip = hdr->skip();
            if (skip)
                remove_leaf_skip<BITS>(node, skip, bld);
            else
                destroy_leaf<BITS>(node, bld);
            return;
        }

        uint64_t* node = bm_to_node(tagged);
        auto* hdr = get_header(node);
        uint8_t sc = hdr->skip();
        if (sc > 0)
            remove_chain_skip<BITS>(node, sc, 0, bld);
        else
            remove_bm_final<BITS>(node, sc, bld);
        BO::dealloc_bitmask(node, bld);
    }

    // Leaf skip: consume prefix bytes, then destroy
    template<int BITS> requires (BITS >= 8)
    static void remove_leaf_skip(uint64_t* node, uint8_t skip,
                                   BLD& bld) noexcept {
        if (skip == 0) {
            destroy_leaf<BITS>(node, bld);
            return;
        }
        if constexpr (BITS > 8) {
            remove_leaf_skip<BITS - 8>(node, skip - 1, bld);
        }
    }

    // Chain embed: consume skip bytes, then final bitmap
    template<int BITS> requires (BITS >= 8)
    static void remove_chain_skip(uint64_t* node, uint8_t sc, uint8_t pos,
                                    BLD& bld) noexcept {
        if (pos >= sc) {
            remove_bm_final<BITS>(node, sc, bld);
            return;
        }
        if constexpr (BITS > 8) {
            remove_chain_skip<BITS - 8>(node, sc, pos + 1, bld);
        }
    }

    // Final bitmap: recurse each child
    template<int BITS> requires (BITS >= 8)
    static void remove_bm_final(uint64_t* node, uint8_t sc,
                                  BLD& bld) noexcept {
        BO::chain_for_each_child(node, sc, [&](unsigned, uint64_t child) {
            if constexpr (BITS > 8) {
                remove_subtree<BITS - 8>(child, bld);
            }
        });
    }

    // ==================================================================
    // Stats collection: no NK narrowing
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
                stats_leaf_skip<BITS>(skip, s);
            else {
                using NK = nk_for_bits_t<BITS>;
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
    static void stats_leaf_skip(uint8_t skip, stats_t& s) noexcept {
        if (skip == 0) {
            using NK = nk_for_bits_t<BITS>;
            if constexpr (sizeof(NK) == 1) s.bitmap_leaves++;
            else                           s.compact_leaves++;
            return;
        }
        if constexpr (BITS > 8) {
            stats_leaf_skip<BITS - 8>(skip - 1, s);
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
            stats_chain_skip<BITS - 8>(node, sc, pos + 1, s);
        }
    }

    template<int BITS> requires (BITS >= 8)
    static void stats_bm_final(const uint64_t* node, uint8_t sc,
                                 stats_t& s) noexcept {
        BO::chain_for_each_child(node, sc, [&](unsigned, uint64_t child) {
            if constexpr (BITS > 8) {
                collect_stats<BITS - 8>(child, s);
            }
        });
    }
};

} // namespace gteitelbaum

#endif // KNTRIE_ITER_OPS_HPP
