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

    using OPS  = kntrie_ops<VALUE, ALLOC, KEY_BITS>;
    using ITER_OPS = kntrie_iter_ops<VALUE, ALLOC, KEY_BITS>;

    // MAX_ROOT_SKIP: leave 1 byte for subtree root dispatch + 1 byte minimum
    // u16: 0, u32: 2, u64: 6
    static constexpr int MAX_ROOT_SKIP = KEY_BITS / 8 - 2;

    // ==================================================================
    // key_to_u64: left-align internal key in uint64_t
    // ==================================================================

    static uint64_t key_to_u64(const KEY& key) noexcept {
        IK internal = KO::to_internal(key);
        return static_cast<uint64_t>(internal) << (64 - IK_BITS);
    }

    // ==================================================================
    // root_fn_t — function pointer table for root dispatch
    // ==================================================================

    using root_find_fn_t     = const VALUE* (*)(uint64_t ptr, uint64_t prefix,
                                                 uint64_t ik) noexcept;
    using root_findleaf_fn_t = const uint64_t* (*)(uint64_t ptr, uint64_t prefix,
                                                    uint64_t ik) noexcept;

    struct root_fn_t {
        uint8_t             skip;
        root_find_fn_t      find;
        root_findleaf_fn_t  find_next;
        root_findleaf_fn_t  find_prev;
    };

    // --- Sentinel root fn (empty trie) ---
    static const VALUE* sentinel_root_find(uint64_t, uint64_t, uint64_t) noexcept {
        return nullptr;
    }
    static const uint64_t* sentinel_root_findleaf(uint64_t, uint64_t, uint64_t) noexcept {
        return nullptr;
    }

    static inline const root_fn_t SENTINEL_ROOT_FN = {
        0, &sentinel_root_find, &sentinel_root_findleaf, &sentinel_root_findleaf,
    };

    // --- Root find implementation ---
    template<int SKIP>
    static const VALUE* root_find_impl(uint64_t ptr, uint64_t prefix,
                                        uint64_t ik) noexcept {
        if constexpr (SKIP > 0) {
            constexpr uint64_t MASK = ~uint64_t(0) << (64 - 8 * SKIP);
            if ((ik ^ prefix) & MASK) [[unlikely]] return nullptr;
        }
        constexpr int BITS = KEY_BITS - 8 * SKIP;
        return OPS::template find_node<BITS>(ptr, ik);
    }

    // --- Root find_next_leaf: find leaf that may contain next key > ik ---
    template<int SKIP>
    static const uint64_t* root_find_next_impl(uint64_t ptr, uint64_t prefix,
                                                uint64_t ik) noexcept {
        constexpr int BITS = KEY_BITS - 8 * SKIP;
        if constexpr (SKIP > 0) {
            constexpr uint64_t MASK = ~uint64_t(0) << (64 - 8 * SKIP);
            uint64_t diff = (ik ^ prefix) & MASK;
            if (diff) [[unlikely]] {
                int shift = std::countl_zero(diff) & ~7;
                uint8_t kb = static_cast<uint8_t>(ik >> (56 - shift));
                uint8_t pb = static_cast<uint8_t>(prefix >> (56 - shift));
                if (kb < pb)
                    return OPS::template descend_min_leaf<BITS>(ptr);
                return nullptr;
            }
        }
        return OPS::template find_leaf_next<BITS>(ptr, ik);
    }

    // --- Root find_prev_leaf: find leaf that may contain prev key < ik ---
    template<int SKIP>
    static const uint64_t* root_find_prev_impl(uint64_t ptr, uint64_t prefix,
                                                uint64_t ik) noexcept {
        constexpr int BITS = KEY_BITS - 8 * SKIP;
        if constexpr (SKIP > 0) {
            constexpr uint64_t MASK = ~uint64_t(0) << (64 - 8 * SKIP);
            uint64_t diff = (ik ^ prefix) & MASK;
            if (diff) [[unlikely]] {
                int shift = std::countl_zero(diff) & ~7;
                uint8_t kb = static_cast<uint8_t>(ik >> (56 - shift));
                uint8_t pb = static_cast<uint8_t>(prefix >> (56 - shift));
                if (kb > pb)
                    return OPS::template descend_max_leaf<BITS>(ptr);
                return nullptr;
            }
        }
        return OPS::template find_leaf_prev<BITS>(ptr, ik);
    }

    // --- Build ROOT_FNS array ---
    template<size_t... Is>
    static constexpr auto make_root_fns(std::index_sequence<Is...>) {
        return std::array<root_fn_t, sizeof...(Is)>{
            root_fn_t{
                static_cast<uint8_t>(Is),
                &root_find_impl<static_cast<int>(Is)>,
                &root_find_next_impl<static_cast<int>(Is)>,
                &root_find_prev_impl<static_cast<int>(Is)>,
            }...
        };
    }

    static constexpr auto ROOT_FNS = make_root_fns(
        std::make_index_sequence<MAX_ROOT_SKIP + 1>{});

    // ==================================================================
    // skip_switch — still needed for write path (insert/erase)
    // ==================================================================

    template<typename F>
    decltype(auto) skip_switch(F&& fn_) const {
        if constexpr (MAX_ROOT_SKIP <= 0) {
            return fn_.template operator()<KEY_BITS>();
        } else if constexpr (MAX_ROOT_SKIP <= 2) {
            switch (root_fn_v->skip) {
            case 0: return fn_.template operator()<KEY_BITS>();
            case 1: return fn_.template operator()<KEY_BITS - 8>();
            case 2: return fn_.template operator()<KEY_BITS - 16>();
            default: __builtin_unreachable();
            }
        } else {
            switch (root_fn_v->skip) {
            case 0: return fn_.template operator()<KEY_BITS>();
            case 1: return fn_.template operator()<KEY_BITS - 8>();
            case 2: return fn_.template operator()<KEY_BITS - 16>();
            case 3: return fn_.template operator()<KEY_BITS - 24>();
            case 4: return fn_.template operator()<KEY_BITS - 32>();
            case 5: return fn_.template operator()<KEY_BITS - 40>();
            case 6: return fn_.template operator()<KEY_BITS - 48>();
            default: __builtin_unreachable();
            }
        }
    }

    // --- Data members ---
    const root_fn_t* root_fn_v;
    uint64_t  root_ptr_v;       // tagged child (SENTINEL, leaf, or bitmask)
    uint64_t  root_prefix_v;    // shared prefix bytes, left-aligned
    size_t    size_v;
    BLD       bld_v;

    void set_root_skip(uint8_t skip) noexcept {
        root_fn_v = &ROOT_FNS[skip];
    }

public:
    // ==================================================================
    // Constructor / Destructor
    // ==================================================================

    kntrie_impl()
        : root_fn_v(&SENTINEL_ROOT_FN),
          root_ptr_v(BO::SENTINEL_TAGGED),
          root_prefix_v(0),
          size_v(0),
          bld_v() {}

    ~kntrie_impl() { remove_all(); bld_v.drain(); }

    kntrie_impl(const kntrie_impl&) = delete;
    kntrie_impl& operator=(const kntrie_impl&) = delete;

    kntrie_impl(kntrie_impl&& o) noexcept
        : root_fn_v(o.root_fn_v),
          root_ptr_v(o.root_ptr_v),
          root_prefix_v(o.root_prefix_v),
          size_v(o.size_v),
          bld_v(std::move(o.bld_v)) {
        o.root_fn_v = &SENTINEL_ROOT_FN;
        o.root_ptr_v = BO::SENTINEL_TAGGED;
        o.root_prefix_v = 0;
        o.size_v = 0;
    }

    kntrie_impl& operator=(kntrie_impl&& o) noexcept {
        if (this != &o) {
            remove_all();
            bld_v.drain();
            root_fn_v = o.root_fn_v;
            root_ptr_v = o.root_ptr_v;
            root_prefix_v = o.root_prefix_v;
            size_v = o.size_v;
            bld_v = std::move(o.bld_v);
            o.root_fn_v = &SENTINEL_ROOT_FN;
            o.root_ptr_v = BO::SENTINEL_TAGGED;
            o.root_prefix_v = 0;
            o.size_v = 0;
        }
        return *this;
    }

    void swap(kntrie_impl& o) noexcept {
        std::swap(root_fn_v, o.root_fn_v);
        std::swap(root_ptr_v, o.root_ptr_v);
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

    // ==================================================================
    // Find — no sentinel checks, sentinel fn returns nullptr
    // ==================================================================

    const VALUE* find_value(const KEY& key) const noexcept {
        uint64_t ik = key_to_u64(key);
        return root_fn_v->find(root_ptr_v, root_prefix_v, ik);
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
    // Erase
    // ==================================================================

    bool erase(const KEY& key) {
        uint64_t ik = key_to_u64(key);

        // Check prefix
        uint8_t skip = root_fn_v->skip;
        if (skip > 0) {
            uint64_t mask = ~uint64_t(0) << (64 - 8 * skip);
            if ((ik ^ root_prefix_v) & mask) return false;
        }

        bool erased = skip_switch([&]<int BITS>() -> bool {
            auto r = OPS::template erase_node<BITS>(root_ptr_v, ik, bld_v);
            if (!r.erased) return false;
            root_ptr_v = r.tagged_ptr ? r.tagged_ptr : BO::SENTINEL_TAGGED;
            return true;
        });

        if (erased) {
            --size_v;
            if (size_v == 0) {
                root_fn_v = &SENTINEL_ROOT_FN;
                root_ptr_v = BO::SENTINEL_TAGGED;
                root_prefix_v = 0;
            }
        }
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
        s.total_bytes = sizeof(*this);
        if (root_ptr_v != BO::SENTINEL_TAGGED) {
            skip_switch([&]<int BITS>() -> int {
                typename ITER_OPS::stats_t os{};
                ITER_OPS::template collect_stats<BITS>(root_ptr_v, os);
                s.total_bytes    += os.total_bytes;
                s.total_entries  += os.total_entries;
                s.bitmap_leaves  += os.bitmap_leaves;
                s.compact_leaves += os.compact_leaves;
                s.bitmask_nodes  += os.bitmask_nodes;
                s.bm_children    += os.bm_children;
                return 0;
            });
        }
        return s;
    }

    size_t memory_usage() const noexcept { return debug_stats().total_bytes; }

    struct root_info_t {
        uint16_t entries; uint8_t skip;
        bool is_leaf;
    };

    root_info_t debug_root_info() const {
        bool is_leaf = (root_ptr_v & LEAF_BIT) != 0;
        uint16_t entries = 0;
        if (root_ptr_v != BO::SENTINEL_TAGGED) {
            if (is_leaf)
                entries = get_header(untag_leaf(root_ptr_v))->entries();
            else
                entries = get_header(bm_to_node_const(root_ptr_v))->entries();
        }
        return {entries, root_fn_v->skip, is_leaf};
    }

    const uint64_t* debug_root() const noexcept {
        if (root_ptr_v == BO::SENTINEL_TAGGED) return nullptr;
        if (root_ptr_v & LEAF_BIT) return untag_leaf(root_ptr_v);
        return bm_to_node_const(root_ptr_v);
    }

    // ==================================================================
    // Iterator support
    // ==================================================================

    struct iter_result_t { KEY key; VALUE value; bool found; };

    iter_result_t to_iter_result(const typename BO::leaf_result_t& r) const noexcept {
        IK internal = static_cast<IK>(r.key >> (64 - IK_BITS));
        return {KO::to_key(internal), *VT::as_ptr(*r.value), true};
    }

    iter_result_t iter_first() const noexcept {
        if (root_ptr_v == BO::SENTINEL_TAGGED) return {KEY{}, VALUE{}, false};
        const uint64_t* leaf = skip_switch([&]<int BITS>() -> const uint64_t* {
            return OPS::template descend_min_leaf<BITS>(root_ptr_v);
        });
        if (!leaf) return {KEY{}, VALUE{}, false};
        auto r = BO::leaf_fn(leaf)->first(leaf);
        if (!r.found) return {KEY{}, VALUE{}, false};
        return to_iter_result(r);
    }

    iter_result_t iter_last() const noexcept {
        if (root_ptr_v == BO::SENTINEL_TAGGED) return {KEY{}, VALUE{}, false};
        const uint64_t* leaf = skip_switch([&]<int BITS>() -> const uint64_t* {
            return OPS::template descend_max_leaf<BITS>(root_ptr_v);
        });
        if (!leaf) return {KEY{}, VALUE{}, false};
        auto r = BO::leaf_fn(leaf)->last(leaf);
        if (!r.found) return {KEY{}, VALUE{}, false};
        return to_iter_result(r);
    }

    iter_result_t iter_next(KEY key) const noexcept {
        uint64_t ik = key_to_u64(key);
        const uint64_t* leaf = root_fn_v->find_next(root_ptr_v, root_prefix_v, ik);
        if (!leaf) return {KEY{}, VALUE{}, false};

        auto r = BO::leaf_fn(leaf)->next(leaf, ik);
        if (r.found) return to_iter_result(r);

        // Leaf exhausted — step to next leaf
        auto last = BO::leaf_fn(leaf)->last(leaf);
        if (!last.found) return {KEY{}, VALUE{}, false};
        uint64_t next_ik = last.key + (uint64_t(1) << (64 - KEY_BITS));
        if (next_ik == 0) return {KEY{}, VALUE{}, false};  // wrapped
        const uint64_t* next_leaf = root_fn_v->find_next(root_ptr_v, root_prefix_v, next_ik);
        if (!next_leaf) return {KEY{}, VALUE{}, false};
        auto r2 = BO::leaf_fn(next_leaf)->first(next_leaf);
        if (!r2.found) return {KEY{}, VALUE{}, false};
        return to_iter_result(r2);
    }

    iter_result_t iter_prev(KEY key) const noexcept {
        uint64_t ik = key_to_u64(key);
        const uint64_t* leaf = root_fn_v->find_prev(root_ptr_v, root_prefix_v, ik);
        if (!leaf) return {KEY{}, VALUE{}, false};

        auto r = BO::leaf_fn(leaf)->prev(leaf, ik);
        if (r.found) return to_iter_result(r);

        // Leaf exhausted — step to prev leaf
        auto first = BO::leaf_fn(leaf)->first(leaf);
        if (!first.found) return {KEY{}, VALUE{}, false};
        uint64_t prev_ik = first.key - (uint64_t(1) << (64 - KEY_BITS));
        if (prev_ik > first.key) return {KEY{}, VALUE{}, false};  // underflow
        const uint64_t* prev_leaf = root_fn_v->find_prev(root_ptr_v, root_prefix_v, prev_ik);
        if (!prev_leaf) return {KEY{}, VALUE{}, false};
        auto r2 = BO::leaf_fn(prev_leaf)->last(prev_leaf);
        if (!r2.found) return {KEY{}, VALUE{}, false};
        return to_iter_result(r2);
    }

private:
    // ==================================================================
    // Insert dispatch
    // ==================================================================

    template<bool INSERT, bool ASSIGN>
    std::pair<bool, bool> insert_dispatch(const KEY& key, const VALUE& value) {
        uint64_t ik = key_to_u64(key);
        VST sv = bld_v.store_value(value);

        // First insert: establish max skip prefix
        if constexpr (MAX_ROOT_SKIP > 0) {
            if (size_v == 0) [[unlikely]] {
                if constexpr (!INSERT) { bld_v.destroy_value(sv); return {true, false}; }
                set_root_skip(MAX_ROOT_SKIP);
                root_prefix_v = ik;
                // Fall through to normal insert
            }
        }

        uint8_t skip = root_fn_v->skip;

        // Check prefix — find first divergence
        if (skip > 0) {
            uint64_t diff = ik ^ root_prefix_v;
            uint64_t mask = ~uint64_t(0) << (64 - 8 * skip);
            if (diff & mask) [[unlikely]] {
                if constexpr (!INSERT) { bld_v.destroy_value(sv); return {true, false}; }
                int clz = std::countl_zero(diff & mask);
                uint8_t div_pos = static_cast<uint8_t>(clz / 8);
                reduce_root_skip(div_pos);
                skip = root_fn_v->skip;
            }
        }

        // Insert into subtree
        bool did_insert = skip_switch([&]<int BITS>() -> bool {
            auto r = OPS::template insert_node<BITS, INSERT, ASSIGN>(
                root_ptr_v, ik, sv, bld_v);
            if (r.tagged_ptr != root_ptr_v) root_ptr_v = r.tagged_ptr;
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
        uint8_t old_skip = root_fn_v->skip;
        uint8_t remaining_skip = old_skip - div_pos - 1;

        // Build prefix bytes for intermediate skip chain
        uint64_t old_subtree;
        if (remaining_skip > 0) {
            uint8_t chain_bytes[6];
            for (uint8_t i = 0; i < remaining_skip; ++i)
                chain_bytes[i] = pfx_byte(root_prefix_v, div_pos + 1 + i);
            uint64_t pfx_packed = pack_prefix(chain_bytes, remaining_skip);

            if (root_ptr_v & LEAF_BIT) {
                // Leaf: prepend skip — need BITS = KEY_BITS - 8*(div_pos+1)
                uint64_t* leaf = untag_leaf_mut(root_ptr_v);
                // div_pos switch to get compile-time BITS for fn pointer
                auto do_prepend = [&]<int DIVP>() -> uint64_t* {
                    constexpr int BITS = KEY_BITS - 8 * (DIVP + 1);
                    return OPS::template prepend_skip<BITS>(
                        leaf, remaining_skip, pfx_packed, bld_v);
                };
                if constexpr (MAX_ROOT_SKIP >= 1) {
                    switch (div_pos) {
                    case 0: leaf = do_prepend.template operator()<0>(); break;
                    case 1: if constexpr (MAX_ROOT_SKIP >= 2) { leaf = do_prepend.template operator()<1>(); break; } [[fallthrough]];
                    case 2: if constexpr (MAX_ROOT_SKIP >= 3) { leaf = do_prepend.template operator()<2>(); break; } [[fallthrough]];
                    case 3: if constexpr (MAX_ROOT_SKIP >= 4) { leaf = do_prepend.template operator()<3>(); break; } [[fallthrough]];
                    case 4: if constexpr (MAX_ROOT_SKIP >= 5) { leaf = do_prepend.template operator()<4>(); break; } [[fallthrough]];
                    case 5: if constexpr (MAX_ROOT_SKIP >= 6) { leaf = do_prepend.template operator()<5>(); break; } [[fallthrough]];
                    default: __builtin_unreachable();
                    }
                }
                old_subtree = tag_leaf(leaf);
            } else {
                // Bitmask: wrap in chain (doesn't need BITS)
                uint64_t* bm_node = bm_to_node(root_ptr_v);
                old_subtree = BO::wrap_in_chain(bm_node, chain_bytes, remaining_skip, bld_v);
            }
        } else {
            old_subtree = root_ptr_v;
        }

        // Create a new bitmask with single child at the divergence byte
        uint8_t old_byte = pfx_byte(root_prefix_v, div_pos);
        uint8_t indices[1] = {old_byte};
        uint64_t children[1] = {old_subtree};
        auto* bm_node = BO::make_bitmask(indices, children, 1, bld_v, size_v);
        root_ptr_v = tag_bitmask(bm_node);

        // Update skip
        set_root_skip(div_pos);
    }

    // ==================================================================
    // Remove all
    // ==================================================================

    void remove_all() noexcept {
        if (root_ptr_v == BO::SENTINEL_TAGGED) return;
        skip_switch([&]<int BITS>() -> int {
            ITER_OPS::template remove_subtree<BITS>(root_ptr_v, bld_v);
            return 0;
        });
        root_fn_v = &SENTINEL_ROOT_FN;
        root_ptr_v = BO::SENTINEL_TAGGED;
        root_prefix_v = 0;
    }
};

} // namespace gteitelbaum

#endif // KNTRIE_IMPL_HPP
