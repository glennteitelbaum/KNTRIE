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
};

} // namespace gteitelbaum

#endif // KNTRIE_OPS_HPP
