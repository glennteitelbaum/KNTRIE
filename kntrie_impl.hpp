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

    // NK0 = initial narrowed key type matching KEY width (>= 16 bits now)
    using NK0 = std::conditional_t<KEY_BITS <= 16, uint16_t,
                std::conditional_t<KEY_BITS <= 32, uint32_t, uint64_t>>;
    using OPS      = kntrie_ops<NK0, VALUE, ALLOC>;
    using ITER_OPS = kntrie_iter_ops<NK0, VALUE, ALLOC>;

    // Root-level narrowing support
    static constexpr int NK0_BITS = static_cast<int>(sizeof(NK0) * 8);

    using NNK0        = next_narrow_t<NK0>;
    using NARROW_OPS  = kntrie_ops<NNK0, VALUE, ALLOC>;
    using NARROW_ITER = kntrie_iter_ops<NNK0, VALUE, ALLOC>;

    // True only for u16/i16: after peeling top byte, 8 bits remain = uint8_t
    static constexpr bool NARROWS_AT_ROOT =
        (KEY_BITS - 8 == NK0_BITS / 2 && NK0_BITS > 8);

    // --- Data members ---
    uint64_t  root_v[256];   // flat array: root_v[top_byte] = tagged child
    size_t    size_v;
    BLD       bld_v;

public:
    // ==================================================================
    // Constructor / Destructor
    // ==================================================================

    kntrie_impl() : size_v(0), bld_v() {
        std::fill(std::begin(root_v), std::end(root_v), SENTINEL_TAGGED);
    }

    ~kntrie_impl() { remove_all(); bld_v.drain(); }

    kntrie_impl(const kntrie_impl&) = delete;
    kntrie_impl& operator=(const kntrie_impl&) = delete;

    kntrie_impl(kntrie_impl&& o) noexcept
        : size_v(o.size_v), bld_v(std::move(o.bld_v)) {
        std::memcpy(root_v, o.root_v, sizeof(root_v));
        std::fill(std::begin(o.root_v), std::end(o.root_v), SENTINEL_TAGGED);
        o.size_v = 0;
    }

    kntrie_impl& operator=(kntrie_impl&& o) noexcept {
        if (this != &o) {
            remove_all();
            bld_v.drain();
            std::memcpy(root_v, o.root_v, sizeof(root_v));
            size_v  = o.size_v;
            bld_v   = std::move(o.bld_v);
            std::fill(std::begin(o.root_v), std::end(o.root_v), SENTINEL_TAGGED);
            o.size_v = 0;
        }
        return *this;
    }

    void swap(kntrie_impl& o) noexcept {
        uint64_t tmp[256];
        std::memcpy(tmp, root_v, sizeof(root_v));
        std::memcpy(root_v, o.root_v, sizeof(root_v));
        std::memcpy(o.root_v, tmp, sizeof(root_v));
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

    void shrink_to_fit() noexcept {
        bld_v.shrink_to_fit();
    }

    size_t memory_in_use() const noexcept {
        return bld_v.memory_in_use();
    }

    size_t memory_needed() const noexcept {
        return bld_v.memory_needed();
    }

    // ==================================================================
    // Find — peel top byte, index root_v, recurse at KEY_BITS-8
    // No sentinel check: sentinel handles misses branchlessly.
    // ==================================================================

    const VALUE* find_value(const KEY& key) const noexcept {
        IK ik = KO::to_internal(key);
        NK0 nk = static_cast<NK0>(ik >> (IK_BITS - KEY_BITS));
        uint8_t top = static_cast<uint8_t>(nk >> (NK0_BITS - 8));
        NK0 shifted = static_cast<NK0>(nk << 8);
        if constexpr (NARROWS_AT_ROOT) {
            return NARROW_OPS::template find_node<KEY_BITS - 8>(root_v[top],
                static_cast<NNK0>(shifted >> (NK0_BITS / 2)));
        } else {
            return OPS::template find_node<KEY_BITS - 8>(root_v[top], shifted);
        }
    }

    bool contains(const KEY& key) const noexcept {
        return find_value(key) != nullptr;
    }

public:
    // ==================================================================
    // Insert (insert-only: does NOT overwrite existing values)
    // ==================================================================

    std::pair<bool, bool> insert(const KEY& key, const VALUE& value) {
        return insert_dispatch<true, false>(key, value);
    }

    // ==================================================================
    // Insert-or-assign (overwrites existing values)
    // ==================================================================

    std::pair<bool, bool> insert_or_assign(const KEY& key, const VALUE& value) {
        return insert_dispatch<true, true>(key, value);
    }

    // ==================================================================
    // Assign (overwrite only, no insert if missing)
    // ==================================================================

    std::pair<bool, bool> assign(const KEY& key, const VALUE& value) {
        return insert_dispatch<false, true>(key, value);
    }

    // ==================================================================
    // Erase — sentinel check (write path: structural decision)
    // ==================================================================

    bool erase(const KEY& key) {
        IK ik = KO::to_internal(key);
        NK0 nk = static_cast<NK0>(ik >> (IK_BITS - KEY_BITS));
        uint8_t top = static_cast<uint8_t>(nk >> (NK0_BITS - 8));
        uint64_t child = root_v[top];
        if (child == SENTINEL_TAGGED) return false;

        NK0 shifted = static_cast<NK0>(nk << 8);
        erase_result_t r;
        if constexpr (NARROWS_AT_ROOT) {
            r = NARROW_OPS::template erase_node<KEY_BITS - 8>(child,
                static_cast<NNK0>(shifted >> (NK0_BITS / 2)), bld_v);
        } else {
            r = OPS::template erase_node<KEY_BITS - 8>(child, shifted, bld_v);
        }

        if (!r.erased) return false;
        root_v[top] = r.tagged_ptr ? r.tagged_ptr : SENTINEL_TAGGED;
        --size_v;
        return true;
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
        s.total_bytes = sizeof(root_v);  // 2048 bytes for root array
        for (int i = 0; i < 256; ++i) {
            if (root_v[i] != SENTINEL_TAGGED)
                collect_stats(root_v[i], s);
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
        return {count, 0, false};
    }

    const uint64_t* debug_root() const noexcept {
        return root_v;
    }

    // ==================================================================
    // Iterator support — no sentinel checks, sentinel returns found=false
    // ==================================================================

    struct iter_result_t { KEY key; VALUE value; bool found; };

    iter_result_t iter_first() const noexcept {
        for (int i = 0; i < 256; ++i) {
            IK prefix = IK(i) << (IK_BITS - 8);
            if constexpr (NARROWS_AT_ROOT) {
                auto r = NARROW_ITER::template descend_min<KEY_BITS - 8, IK>(
                    root_v[i], prefix, 8);
                if (r.found) return {KO::to_key(r.key), *VT::as_ptr(*r.value), true};
            } else {
                auto r = ITER_OPS::template descend_min<KEY_BITS - 8, IK>(
                    root_v[i], prefix, 8);
                if (r.found) return {KO::to_key(r.key), *VT::as_ptr(*r.value), true};
            }
        }
        return {KEY{}, VALUE{}, false};
    }

    iter_result_t iter_last() const noexcept {
        for (int i = 255; i >= 0; --i) {
            IK prefix = IK(i) << (IK_BITS - 8);
            if constexpr (NARROWS_AT_ROOT) {
                auto r = NARROW_ITER::template descend_max<KEY_BITS - 8, IK>(
                    root_v[i], prefix, 8);
                if (r.found) return {KO::to_key(r.key), *VT::as_ptr(*r.value), true};
            } else {
                auto r = ITER_OPS::template descend_max<KEY_BITS - 8, IK>(
                    root_v[i], prefix, 8);
                if (r.found) return {KO::to_key(r.key), *VT::as_ptr(*r.value), true};
            }
        }
        return {KEY{}, VALUE{}, false};
    }

    iter_result_t iter_next(KEY key) const noexcept {
        IK ik = KO::to_internal(key);
        NK0 nk = static_cast<NK0>(ik >> (IK_BITS - KEY_BITS));
        uint8_t top = static_cast<uint8_t>(nk >> (NK0_BITS - 8));
        NK0 shifted = static_cast<NK0>(nk << 8);

        // Try to find next within same slot
        {
            IK prefix = IK(top) << (IK_BITS - 8);
            if constexpr (NARROWS_AT_ROOT) {
                auto r = NARROW_ITER::template iter_next_node<KEY_BITS - 8, IK>(
                    root_v[top],
                    static_cast<NNK0>(shifted >> (NK0_BITS / 2)),
                    prefix, 8);
                if (r.found) return {KO::to_key(r.key), *VT::as_ptr(*r.value), true};
            } else {
                auto r = ITER_OPS::template iter_next_node<KEY_BITS - 8, IK>(
                    root_v[top], shifted, prefix, 8);
                if (r.found) return {KO::to_key(r.key), *VT::as_ptr(*r.value), true};
            }
        }

        // Scan forward — sentinel returns found=false naturally
        for (int i = top + 1; i < 256; ++i) {
            IK prefix = IK(i) << (IK_BITS - 8);
            if constexpr (NARROWS_AT_ROOT) {
                auto r = NARROW_ITER::template descend_min<KEY_BITS - 8, IK>(
                    root_v[i], prefix, 8);
                if (r.found) return {KO::to_key(r.key), *VT::as_ptr(*r.value), true};
            } else {
                auto r = ITER_OPS::template descend_min<KEY_BITS - 8, IK>(
                    root_v[i], prefix, 8);
                if (r.found) return {KO::to_key(r.key), *VT::as_ptr(*r.value), true};
            }
        }
        return {KEY{}, VALUE{}, false};
    }

    iter_result_t iter_prev(KEY key) const noexcept {
        IK ik = KO::to_internal(key);
        NK0 nk = static_cast<NK0>(ik >> (IK_BITS - KEY_BITS));
        uint8_t top = static_cast<uint8_t>(nk >> (NK0_BITS - 8));
        NK0 shifted = static_cast<NK0>(nk << 8);

        // Try to find prev within same slot
        {
            IK prefix = IK(top) << (IK_BITS - 8);
            if constexpr (NARROWS_AT_ROOT) {
                auto r = NARROW_ITER::template iter_prev_node<KEY_BITS - 8, IK>(
                    root_v[top],
                    static_cast<NNK0>(shifted >> (NK0_BITS / 2)),
                    prefix, 8);
                if (r.found) return {KO::to_key(r.key), *VT::as_ptr(*r.value), true};
            } else {
                auto r = ITER_OPS::template iter_prev_node<KEY_BITS - 8, IK>(
                    root_v[top], shifted, prefix, 8);
                if (r.found) return {KO::to_key(r.key), *VT::as_ptr(*r.value), true};
            }
        }

        // Scan backward — sentinel returns found=false naturally
        for (int i = top - 1; i >= 0; --i) {
            IK prefix = IK(i) << (IK_BITS - 8);
            if constexpr (NARROWS_AT_ROOT) {
                auto r = NARROW_ITER::template descend_max<KEY_BITS - 8, IK>(
                    root_v[i], prefix, 8);
                if (r.found) return {KO::to_key(r.key), *VT::as_ptr(*r.value), true};
            } else {
                auto r = ITER_OPS::template descend_max<KEY_BITS - 8, IK>(
                    root_v[i], prefix, 8);
                if (r.found) return {KO::to_key(r.key), *VT::as_ptr(*r.value), true};
            }
        }
        return {KEY{}, VALUE{}, false};
    }

private:
    // ==================================================================
    // Insert dispatch (shared by insert / insert_or_assign / assign)
    // Sentinel check: write path structural decision
    // ==================================================================

    template<bool INSERT, bool ASSIGN>
    std::pair<bool, bool> insert_dispatch(const KEY& key, const VALUE& value) {
        IK ik = KO::to_internal(key);
        VST sv = bld_v.store_value(value);
        NK0 nk = static_cast<NK0>(ik >> (IK_BITS - KEY_BITS));
        uint8_t top = static_cast<uint8_t>(nk >> (NK0_BITS - 8));
        NK0 shifted = static_cast<NK0>(nk << 8);

        uint64_t child = root_v[top];

        // Empty slot: create single-entry leaf for remaining key
        if (child == SENTINEL_TAGGED) {
            if constexpr (!INSERT) { bld_v.destroy_value(sv); return {true, false}; }
            uint64_t* leaf;
            if constexpr (NARROWS_AT_ROOT) {
                leaf = NARROW_OPS::make_single_leaf(
                    static_cast<NNK0>(shifted >> (NK0_BITS / 2)), sv, bld_v);
            } else {
                leaf = OPS::make_single_leaf(shifted, sv, bld_v);
            }
            root_v[top] = tag_leaf(leaf);
            ++size_v;
            return {true, true};
        }

        // Non-empty slot: recurse into child with KEY_BITS-8
        insert_result_t r;
        if constexpr (NARROWS_AT_ROOT) {
            r = NARROW_OPS::template insert_node<KEY_BITS - 8, INSERT, ASSIGN>(
                child, static_cast<NNK0>(shifted >> (NK0_BITS / 2)), sv, bld_v);
        } else {
            r = OPS::template insert_node<KEY_BITS - 8, INSERT, ASSIGN>(
                child, shifted, sv, bld_v);
        }

        if (r.tagged_ptr != child) root_v[top] = r.tagged_ptr;
        if (r.inserted) { ++size_v; return {true, true}; }
        bld_v.destroy_value(sv);
        return {true, false};
    }

    // ==================================================================
    // Remove all — write path, checks sentinel to skip empty slots
    // ==================================================================

    void remove_all() noexcept {
        for (int i = 0; i < 256; ++i) {
            if (root_v[i] != SENTINEL_TAGGED) {
                if constexpr (NARROWS_AT_ROOT) {
                    NARROW_ITER::template remove_subtree<KEY_BITS - 8>(root_v[i], bld_v);
                } else {
                    ITER_OPS::template remove_subtree<KEY_BITS - 8>(root_v[i], bld_v);
                }
                root_v[i] = SENTINEL_TAGGED;
            }
        }
        size_v = 0;
    }

    // ==================================================================
    // Stats collection — diagnostic path, checks sentinel
    // ==================================================================

    void collect_stats(uint64_t tagged, debug_stats_t& s) const noexcept {
        typename ITER_OPS::stats_t os{};
        if constexpr (NARROWS_AT_ROOT) {
            NARROW_ITER::template collect_stats<KEY_BITS - 8>(tagged, os);
        } else {
            ITER_OPS::template collect_stats<KEY_BITS - 8>(tagged, os);
        }
        s.total_bytes    += os.total_bytes;
        s.total_entries  += os.total_entries;
        s.bitmap_leaves  += os.bitmap_leaves;
        s.compact_leaves += os.compact_leaves;
        s.bitmask_nodes  += os.bitmask_nodes;
        s.bm_children    += os.bm_children;
    }
};

} // namespace gteitelbaum

#endif // KNTRIE_IMPL_HPP
