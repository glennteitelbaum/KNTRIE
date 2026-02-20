#ifndef KNTRIE_IMPL_HPP
#define KNTRIE_IMPL_HPP

#include "kntrie_ops.hpp"
#include "kntrie_iter_ops.hpp"
#include <memory>
#include <cstring>
#include <algorithm>

namespace gteitelbaum {

template<typename KEY, typename VALUE, typename ALLOC = std::allocator<uint64_t>>
class kntrie_impl {
    static_assert(std::is_integral_v<KEY> && sizeof(KEY) >= 2,
                  "KEY must be integral and at least 16 bits");

public:
    using key_type       = KEY;
    using mapped_type    = VALUE;
    using size_type      = std::size_t;
    using allocator_type = ALLOC;

private:
    using KO   = key_ops<KEY>;
    using IK   = typename KO::IK;
    using VT   = value_traits<VALUE, ALLOC>;
    using VST  = typename VT::slot_type;
    using BO   = bitmask_ops<VALUE, ALLOC>;
    using BLD  = builder<VALUE, VT::IS_TRIVIAL, ALLOC>;

    static constexpr int IK_BITS  = KO::IK_BITS;
    static constexpr int KEY_BITS = KO::KEY_BITS;

    // NK0 = initial narrowed key type matching KEY width (>= 16 bits)
    using NK0 = std::conditional_t<KEY_BITS <= 16, uint16_t,
                std::conditional_t<KEY_BITS <= 32, uint32_t, uint64_t>>;
    using OPS      = kntrie_ops<NK0, VALUE, ALLOC, IK>;
    using ITER_OPS = kntrie_iter_ops<NK0, VALUE, ALLOC, IK>;

    static constexpr int NK0_BITS = static_cast<int>(sizeof(NK0) * 8);

    using NNK0         = next_narrow_t<NK0>;
    using NARROW_OPS   = kntrie_ops<NNK0, VALUE, ALLOC, IK>;
    using NARROW_ITER  = kntrie_iter_ops<NNK0, VALUE, ALLOC, IK>;

    using NNNK0  = next_narrow_t<NNK0>;
    using NNNNK0 = next_narrow_t<NNNK0>;   // uint8_t for u64 keys

    // MAX_ROOT_SKIP: leave 1 byte for root_v index + 1 byte for subtree
    // u16: 0  (no skip), u32: 2, u64: 6
    static constexpr int MAX_ROOT_SKIP = KEY_BITS / 8 - 2;

    // ======================================================================
    // root_dispatch<BITS>: compile-time trait for narrowing at a given depth
    //
    // KEY INVARIANT: We must choose NK_TYPE such that BITS > NK_TYPE_BITS/2
    // (or NK_TYPE_BITS == 8). This prevents find_node/insert_node from
    // narrowing internally, ensuring insert and find use the same NK type
    // for leaf creation and lookup. Use strict > not >= for boundaries.
    // ======================================================================

    template<int BITS>
    struct root_dispatch {
        static_assert(BITS >= 8);
        static constexpr int CONSUMED_BITS = KEY_BITS - BITS;
        static constexpr int NNK0_BITS  = static_cast<int>(sizeof(NNK0) * 8);
        static constexpr int NNNK0_BITS = static_cast<int>(sizeof(NNNK0) * 8);

        // Strict >: ensures find_node won't narrow past what insert used
        using NK_TYPE = std::conditional_t<(BITS > NK0_BITS / 2), NK0,
                        std::conditional_t<(BITS > NNK0_BITS / 2), NNK0,
                        std::conditional_t<(BITS > NNNK0_BITS / 2), NNNK0, NNNNK0>>>;

        using OPS_TYPE  = kntrie_ops<NK_TYPE, VALUE, ALLOC, IK>;
        using ITER_TYPE = kntrie_iter_ops<NK_TYPE, VALUE, ALLOC, IK>;

        // Shift and narrow nk to the correct type for this depth
        static NK_TYPE narrow(NK0 nk) noexcept {
            NK0 shifted = static_cast<NK0>(nk << CONSUMED_BITS);
            if constexpr (std::is_same_v<NK_TYPE, NK0>) {
                return shifted;
            } else {
                constexpr int NK_TYPE_BITS = static_cast<int>(sizeof(NK_TYPE) * 8);
                return static_cast<NK_TYPE>(shifted >> (NK0_BITS - NK_TYPE_BITS));
            }
        }
    };

    // ======================================================================
    // skip_switch: dispatch a templated lambda at the correct BITS depth
    // ======================================================================

    template<typename F>
    static decltype(auto) skip_switch(uint8_t skip, F&& fn_) {
        if constexpr (MAX_ROOT_SKIP <= 0) {
            return fn_.template operator()<KEY_BITS - 8>();
        } else if constexpr (MAX_ROOT_SKIP <= 2) {
            switch (skip) {
            case 0: return fn_.template operator()<KEY_BITS - 8>();
            case 1: return fn_.template operator()<KEY_BITS - 16>();
            case 2: return fn_.template operator()<KEY_BITS - 24>();
            default: __builtin_unreachable();
            }
        } else {
            switch (skip) {
            case 0: return fn_.template operator()<KEY_BITS - 8>();
            case 1: return fn_.template operator()<KEY_BITS - 16>();
            case 2: return fn_.template operator()<KEY_BITS - 24>();
            case 3: return fn_.template operator()<KEY_BITS - 32>();
            case 4: return fn_.template operator()<KEY_BITS - 40>();
            case 5: return fn_.template operator()<KEY_BITS - 48>();
            case 6: return fn_.template operator()<KEY_BITS - 56>();
            default: __builtin_unreachable();
            }
        }
    }

    // ======================================================================
    // nk_byte: extract byte i (0-based from MSB) from NK0
    // ======================================================================

    static uint8_t nk_byte(NK0 nk, uint8_t i) noexcept {
        return static_cast<uint8_t>(nk >> (NK0_BITS - 8 * (i + 1)));
    }

    // Left-align NK0 into uint64_t (byte 0 at bits 63..56)
    static uint64_t nk_to_u64(NK0 nk) noexcept {
        return static_cast<uint64_t>(nk) << (64 - NK0_BITS);
    }

    // Extract byte i from packed prefix (byte 0 in MSB)
    static uint8_t prefix_byte(uint64_t pfx, uint8_t i) noexcept {
        return static_cast<uint8_t>(pfx >> (56 - 8 * i));
    }

    // Build IK prefix from root_prefix_v bytes [0..skip-1] + top byte
    IK make_prefix(uint8_t top) const noexcept {
        IK prefix = IK(0);
        for (uint8_t j = 0; j < root_skip_v; ++j)
            prefix |= IK(prefix_byte(root_prefix_v, j)) << (IK_BITS - 8 * (j + 1));
        prefix |= IK(top) << (IK_BITS - 8 * (root_skip_v + 1));
        return prefix;
    }

    int prefix_bits() const noexcept {
        return 8 * (root_skip_v + 1);
    }

    // --- Data members ---
    uint64_t  root_v[256];
    uint8_t   root_skip_v;
    uint64_t  root_prefix_v;   // packed big-endian: byte 0 in bits 63..56
    size_t    size_v;
    BLD       bld_v;

public:
    // ==================================================================
    // Constructor / Destructor
    // ==================================================================

    kntrie_impl() : root_skip_v(0), root_prefix_v(0), size_v(0), bld_v() {
        std::fill(std::begin(root_v), std::end(root_v), SENTINEL_TAGGED);
    }

    ~kntrie_impl() { remove_all(); bld_v.drain(); }

    kntrie_impl(const kntrie_impl&) = delete;
    kntrie_impl& operator=(const kntrie_impl&) = delete;

    kntrie_impl(kntrie_impl&& o) noexcept
        : root_skip_v(o.root_skip_v), root_prefix_v(o.root_prefix_v),
          size_v(o.size_v), bld_v(std::move(o.bld_v)) {
        std::memcpy(root_v, o.root_v, sizeof(root_v));
        std::fill(std::begin(o.root_v), std::end(o.root_v), SENTINEL_TAGGED);
        o.root_skip_v = 0;
        o.root_prefix_v = 0;
        o.size_v = 0;
    }

    kntrie_impl& operator=(kntrie_impl&& o) noexcept {
        if (this != &o) {
            remove_all();
            bld_v.drain();
            std::memcpy(root_v, o.root_v, sizeof(root_v));
            root_prefix_v = o.root_prefix_v;
            root_skip_v = o.root_skip_v;
            size_v      = o.size_v;
            bld_v       = std::move(o.bld_v);
            std::fill(std::begin(o.root_v), std::end(o.root_v), SENTINEL_TAGGED);
            o.root_skip_v = 0;
            o.root_prefix_v = 0;
            o.size_v = 0;
        }
        return *this;
    }

    void swap(kntrie_impl& o) noexcept {
        uint64_t tmp[256];
        std::memcpy(tmp, root_v, sizeof(root_v));
        std::memcpy(root_v, o.root_v, sizeof(root_v));
        std::memcpy(o.root_v, tmp, sizeof(root_v));
        std::swap(root_skip_v, o.root_skip_v);
        std::swap(root_prefix_v, o.root_prefix_v);
        std::swap(size_v, o.size_v);
        bld_v.swap(o.bld_v);
    }

    [[nodiscard]] bool      empty() const noexcept { return size_v == 0; }
    [[nodiscard]] size_type size()  const noexcept { return size_v; }
    [[nodiscard]] const ALLOC& get_allocator() const noexcept { return bld_v.get_allocator(); }

    void clear() noexcept {
        remove_all();
        bld_v.drain();
        size_v = 0;
    }

    void shrink_to_fit() noexcept { bld_v.shrink_to_fit(); }
    size_t memory_in_use() const noexcept { return bld_v.memory_in_use(); }
    size_t memory_needed() const noexcept { return bld_v.memory_needed(); }

    // ==================================================================
    // root_skip_ops — combined functor table for root skip dispatch.
    // One entry per skip value with find/next/prev/min/max.
    // ==================================================================

    struct root_skip_ops_t {
        using find_fn_t   = const VALUE* (*)(const uint64_t*, uint64_t, NK0) noexcept;
        using minmax_fn_t = iter_ops_result_t<IK, VST> (*)(uint64_t, IK, int) noexcept;
        using iter_fn_t   = iter_ops_result_t<IK, VST> (*)(uint64_t, NK0, IK) noexcept;

        find_fn_t   find;
        iter_fn_t   next;
        iter_fn_t   prev;
        minmax_fn_t min;
        minmax_fn_t max;
    };

    template<int SKIP>
    static const VALUE* root_find_at(const uint64_t* root, uint64_t prefix,
                                      NK0 nk) noexcept {
        if constexpr (SKIP > 0) {
            constexpr uint64_t MASK = ~uint64_t(0) << (64 - 8 * SKIP);
            if ((nk_to_u64(nk) ^ prefix) & MASK) [[unlikely]] return nullptr;
        }
        uint8_t top = nk_byte(nk, SKIP);
        uint64_t child = root[top];
        constexpr int BITS = KEY_BITS - 8 * (SKIP + 1);
        using D = root_dispatch<BITS>;
        return D::OPS_TYPE::template find_node<BITS>(child, D::narrow(nk));
    }

    template<int SKIP>
    static iter_ops_result_t<IK, VST> root_min_at(uint64_t child,
                                                     IK prefix, int bits) noexcept {
        constexpr int BITS = KEY_BITS - 8 * (SKIP + 1);
        using D = root_dispatch<BITS>;
        return D::ITER_TYPE::template descend_min<BITS>(child, prefix, bits);
    }

    template<int SKIP>
    static iter_ops_result_t<IK, VST> root_max_at(uint64_t child,
                                                     IK prefix, int bits) noexcept {
        constexpr int BITS = KEY_BITS - 8 * (SKIP + 1);
        using D = root_dispatch<BITS>;
        return D::ITER_TYPE::template descend_max<BITS>(child, prefix, bits);
    }

    template<int SKIP>
    static iter_ops_result_t<IK, VST> root_next_at(uint64_t child,
                                                      NK0 nk, IK full_ik) noexcept {
        constexpr int BITS = KEY_BITS - 8 * (SKIP + 1);
        using D = root_dispatch<BITS>;
        return D::ITER_TYPE::template iter_next_node<BITS>(child, D::narrow(nk), full_ik);
    }

    template<int SKIP>
    static iter_ops_result_t<IK, VST> root_prev_at(uint64_t child,
                                                      NK0 nk, IK full_ik) noexcept {
        constexpr int BITS = KEY_BITS - 8 * (SKIP + 1);
        using D = root_dispatch<BITS>;
        return D::ITER_TYPE::template iter_prev_node<BITS>(child, D::narrow(nk), full_ik);
    }

    template<size_t... Is>
    static constexpr auto make_root_table(std::index_sequence<Is...>) {
        return std::array<root_skip_ops_t, sizeof...(Is)>{
            root_skip_ops_t{
                &root_find_at<static_cast<int>(Is)>,
                &root_next_at<static_cast<int>(Is)>,
                &root_prev_at<static_cast<int>(Is)>,
                &root_min_at<static_cast<int>(Is)>,
                &root_max_at<static_cast<int>(Is)>
            }...
        };
    }

    static constexpr auto ROOT_OPS = make_root_table(
        std::make_index_sequence<MAX_ROOT_SKIP + 1>{});

    const VALUE* find_value(const KEY& key) const noexcept {
        IK ik = KO::to_internal(key);
        NK0 nk = static_cast<NK0>(ik >> (IK_BITS - KEY_BITS));
        return ROOT_OPS[root_skip_v].find(root_v, root_prefix_v, nk);
    }

    bool contains(const KEY& key) const noexcept {
        return find_value(key) != nullptr;
    }

public:
    // ==================================================================
    // Insert / Insert-or-assign / Assign
    // ==================================================================

    std::pair<bool, bool> insert(const KEY& key, const VALUE& value) {
        return insert_dispatch<true, false>(key, value);
    }

    std::pair<bool, bool> insert_or_assign(const KEY& key, const VALUE& value) {
        return insert_dispatch<true, true>(key, value);
    }

    std::pair<bool, bool> assign(const KEY& key, const VALUE& value) {
        return insert_dispatch<false, true>(key, value);
    }

    // ==================================================================
    // Erase — sentinel check (write path)
    // ==================================================================

    bool erase(const KEY& key) {
        IK ik = KO::to_internal(key);
        NK0 nk = static_cast<NK0>(ik >> (IK_BITS - KEY_BITS));

        // Check prefix
        if (root_skip_v > 0) {
            uint64_t mask = ~uint64_t(0) << (64 - 8 * root_skip_v);
            if ((nk_to_u64(nk) ^ root_prefix_v) & mask) return false;
        }

        uint8_t top = nk_byte(nk, root_skip_v);
        uint64_t child = root_v[top];
        if (child == SENTINEL_TAGGED) return false;

        bool erased = skip_switch(root_skip_v, [&]<int BITS>() -> bool {
            using D = root_dispatch<BITS>;
            auto r = D::OPS_TYPE::template erase_node<BITS>(child, D::narrow(nk), bld_v);
            if (!r.erased) return false;
            root_v[top] = r.tagged_ptr ? r.tagged_ptr : SENTINEL_TAGGED;
            return true;
        });

        if (erased) --size_v;
        return erased;
    }

    // ==================================================================
    // Stats / Memory
    // ==================================================================

    struct debug_stats_t {
        size_t compact_leaves = 0;
        size_t bitmap_leaves  = 0;
        size_t bitmask_nodes  = 0;
        size_t bm_children    = 0;
        size_t total_entries  = 0;
        size_t total_bytes    = 0;
    };

    debug_stats_t debug_stats() const noexcept {
        debug_stats_t s{};
        s.total_bytes = sizeof(root_v);
        for (int i = 0; i < 256; ++i) {
            if (root_v[i] != SENTINEL_TAGGED)
                collect_stats_one(root_v[i], s);
        }
        return s;
    }

    size_t memory_usage() const noexcept { return debug_stats().total_bytes; }

    struct root_info_t {
        uint16_t entries; uint8_t skip;
        bool is_leaf;
    };

    root_info_t debug_root_info() const {
        uint16_t count = 0;
        for (int i = 0; i < 256; ++i)
            if (root_v[i] != SENTINEL_TAGGED) ++count;
        return {count, root_skip_v, false};
    }

    const uint64_t* debug_root() const noexcept { return root_v; }

    // ==================================================================
    // Iterator support — no sentinel checks, sentinel returns found=false
    // ==================================================================

    struct iter_result_t { KEY key; VALUE value; bool found; };

    iter_result_t iter_first() const noexcept {
        int pb = prefix_bits();
        auto& ops = ROOT_OPS[root_skip_v];
        for (int i = 0; i < 256; ++i) {
            if (root_v[i] == SENTINEL_TAGGED) continue;
            IK pfx = make_prefix(static_cast<uint8_t>(i));
            auto r = ops.min(root_v[i], pfx, pb);
            return {KO::to_key(r.key), *VT::as_ptr(*r.value), true};
        }
        return {KEY{}, VALUE{}, false};
    }

    iter_result_t iter_last() const noexcept {
        int pb = prefix_bits();
        auto& ops = ROOT_OPS[root_skip_v];
        for (int i = 255; i >= 0; --i) {
            if (root_v[i] == SENTINEL_TAGGED) continue;
            IK pfx = make_prefix(static_cast<uint8_t>(i));
            auto r = ops.max(root_v[i], pfx, pb);
            return {KO::to_key(r.key), *VT::as_ptr(*r.value), true};
        }
        return {KEY{}, VALUE{}, false};
    }

    iter_result_t iter_next(KEY key) const noexcept {
        IK ik = KO::to_internal(key);
        NK0 nk = static_cast<NK0>(ik >> (IK_BITS - KEY_BITS));
        uint8_t top = nk_byte(nk, root_skip_v);
        auto& ops = ROOT_OPS[root_skip_v];

        // Try next within same slot
        if (root_v[top] != SENTINEL_TAGGED) {
            auto r = ops.next(root_v[top], nk, ik);
            if (r.found) return {KO::to_key(r.key), *VT::as_ptr(*r.value), true};
        }

        // Scan forward — non-sentinel root always has entries
        int pb = prefix_bits();
        for (int i = top + 1; i < 256; ++i) {
            if (root_v[i] == SENTINEL_TAGGED) continue;
            IK pfx = make_prefix(static_cast<uint8_t>(i));
            auto r = ops.min(root_v[i], pfx, pb);
            return {KO::to_key(r.key), *VT::as_ptr(*r.value), true};
        }
        return {KEY{}, VALUE{}, false};
    }

    iter_result_t iter_prev(KEY key) const noexcept {
        IK ik = KO::to_internal(key);
        NK0 nk = static_cast<NK0>(ik >> (IK_BITS - KEY_BITS));
        uint8_t top = nk_byte(nk, root_skip_v);
        auto& ops = ROOT_OPS[root_skip_v];

        // Try prev within same slot
        if (root_v[top] != SENTINEL_TAGGED) {
            auto r = ops.prev(root_v[top], nk, ik);
            if (r.found) return {KO::to_key(r.key), *VT::as_ptr(*r.value), true};
        }

        // Scan backward — non-sentinel root always has entries
        int pb = prefix_bits();
        for (int i = top - 1; i >= 0; --i) {
            if (root_v[i] == SENTINEL_TAGGED) continue;
            IK pfx = make_prefix(static_cast<uint8_t>(i));
            auto r = ops.max(root_v[i], pfx, pb);
            return {KO::to_key(r.key), *VT::as_ptr(*r.value), true};
        }
        return {KEY{}, VALUE{}, false};
    }

private:
    // ==================================================================
    // Insert dispatch — prefix check + initial skip + reduce + switch
    // ==================================================================

    template<bool INSERT, bool ASSIGN>
    std::pair<bool, bool> insert_dispatch(const KEY& key, const VALUE& value) {
        IK ik = KO::to_internal(key);
        VST sv = bld_v.store_value(value);
        NK0 nk = static_cast<NK0>(ik >> (IK_BITS - KEY_BITS));

        // First insert: establish max skip prefix
        if constexpr (MAX_ROOT_SKIP > 0) {
            if (size_v == 0) [[unlikely]] {
                if constexpr (!INSERT) { bld_v.destroy_value(sv); return {true, false}; }
                root_skip_v = static_cast<uint8_t>(MAX_ROOT_SKIP);
                root_prefix_v = nk_to_u64(nk);
                // Fall through to normal insert (root_v[top] is SENTINEL_TAGGED)
            }
        }

        // Check prefix — find first divergence
        if (root_skip_v > 0) {
            uint64_t nk64 = nk_to_u64(nk);
            uint64_t diff = nk64 ^ root_prefix_v;
            uint64_t mask = ~uint64_t(0) << (64 - 8 * root_skip_v);
            if (diff & mask) [[unlikely]] {
                if constexpr (!INSERT) { bld_v.destroy_value(sv); return {true, false}; }
                // Find first differing byte position
                int clz = std::countl_zero(diff & mask);
                uint8_t div_pos = static_cast<uint8_t>(clz / 8);
                reduce_root_skip(div_pos);
            }
        }

        uint8_t top = nk_byte(nk, root_skip_v);
        uint64_t child = root_v[top];

        // Empty slot: create leaf
        if (child == SENTINEL_TAGGED) {
            if constexpr (!INSERT) { bld_v.destroy_value(sv); return {true, false}; }
            uint64_t* leaf = skip_switch(root_skip_v, [&]<int BITS>() -> uint64_t* {
                using D = root_dispatch<BITS>;
                return D::OPS_TYPE::make_single_leaf(D::narrow(nk), sv, bld_v);
            });
            root_v[top] = tag_leaf(leaf);
            ++size_v;
            return {true, true};
        }

        // Non-empty slot: recurse
        bool did_insert = skip_switch(root_skip_v, [&]<int BITS>() -> bool {
            using D = root_dispatch<BITS>;
            auto r = D::OPS_TYPE::template insert_node<BITS, INSERT, ASSIGN>(
                child, D::narrow(nk), sv, bld_v);
            if (r.tagged_ptr != child) root_v[top] = r.tagged_ptr;
            return r.inserted;
        });

        if (did_insert) { ++size_v; return {true, true}; }
        bld_v.destroy_value(sv);
        return {true, false};
    }

    // ==================================================================
    // reduce_root_skip: restructure root when prefix diverges
    // ==================================================================

    void reduce_root_skip(uint8_t div_pos) {
        uint8_t old_skip = root_skip_v;

        // Collect non-sentinel entries from old root_v
        uint8_t indices[256];
        uint64_t tagged_ptrs[256];
        unsigned count = 0;
        for (int i = 0; i < 256; ++i) {
            if (root_v[i] != SENTINEL_TAGGED) {
                indices[count] = static_cast<uint8_t>(i);
                tagged_ptrs[count] = root_v[i];
                ++count;
            }
        }

        // Create bitmask node (or skip chain) for old subtree
        uint64_t old_subtree;
        uint8_t chain_len = old_skip - div_pos - 1;  // intermediate skip bytes
        if (chain_len > 0) {
            uint8_t chain_bytes[6];
            for (uint8_t i = 0; i < chain_len; ++i)
                chain_bytes[i] = prefix_byte(root_prefix_v, div_pos + 1 + i);
            auto* node = BO::make_skip_chain(chain_bytes, chain_len,
                                              indices, tagged_ptrs, count, bld_v,
                                              size_v);
            old_subtree = tag_bitmask(node);
        } else {
            auto* node = BO::make_bitmask(indices, tagged_ptrs, count, bld_v,
                                           size_v);
            old_subtree = tag_bitmask(node);
        }

        // Clear root_v, set new skip
        std::fill(std::begin(root_v), std::end(root_v), SENTINEL_TAGGED);
        uint8_t old_byte = prefix_byte(root_prefix_v, div_pos);
        root_skip_v = div_pos;
        // root_prefix_v bits [0..div_pos-1] unchanged, rest don't matter

        // Place old subtree under old prefix byte at div_pos
        root_v[old_byte] = old_subtree;
    }

    // ==================================================================
    // Remove all — write path, switch outside loop
    // ==================================================================

    void remove_all() noexcept {
        skip_switch(root_skip_v, [&]<int BITS>() -> int {
            using D = root_dispatch<BITS>;
            for (int i = 0; i < 256; ++i) {
                if (root_v[i] != SENTINEL_TAGGED) {
                    D::ITER_TYPE::template remove_subtree<BITS>(root_v[i], bld_v);
                    root_v[i] = SENTINEL_TAGGED;
                }
            }
            return 0;
        });
        root_skip_v = 0;
        size_v = 0;
    }

    // ==================================================================
    // Stats collection — diagnostic path
    // ==================================================================

    void collect_stats_one(uint64_t tagged, debug_stats_t& s) const noexcept {
        skip_switch(root_skip_v, [&]<int BITS>() -> int {
            using D = root_dispatch<BITS>;
            typename D::ITER_TYPE::stats_t os{};
            D::ITER_TYPE::template collect_stats<BITS>(tagged, os);
            s.total_bytes    += os.total_bytes;
            s.total_entries  += os.total_entries;
            s.bitmap_leaves  += os.bitmap_leaves;
            s.compact_leaves += os.compact_leaves;
            s.bitmask_nodes  += os.bitmask_nodes;
            s.bm_children    += os.bm_children;
            return 0;
        });
    }
};

} // namespace gteitelbaum

#endif // KNTRIE_IMPL_HPP
