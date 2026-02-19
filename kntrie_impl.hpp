#ifndef KNTRIE_IMPL_HPP
#define KNTRIE_IMPL_HPP

#include "kntrie_ops.hpp"
#include "kntrie_iter_ops.hpp"
#include <memory>

namespace gteitelbaum {

template<typename KEY, typename VALUE, typename ALLOC = std::allocator<uint64_t>>
class kntrie_impl {
    static_assert(std::is_integral_v<KEY>, "KEY must be integral");

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

    static constexpr int IK_BITS  = KO::IK_BITS;
    static constexpr int KEY_BITS = KO::KEY_BITS;

    // NK0 = initial narrowed key type matching KEY width
    using NK0 = std::conditional_t<KEY_BITS <= 8,  uint8_t,
                std::conditional_t<KEY_BITS <= 16, uint16_t,
                std::conditional_t<KEY_BITS <= 32, uint32_t, uint64_t>>>;
    using OPS     = kntrie_ops<NK0, VALUE, ALLOC>;
    using ITER_OPS = kntrie_iter_ops<NK0, VALUE, ALLOC>;

    uint64_t  root_v;      // tagged pointer (LEAF_BIT for leaf, raw for bitmask)
    size_t    size_v;
    [[no_unique_address]] ALLOC alloc_v;

public:
    // ==================================================================
    // Constructor / Destructor
    // ==================================================================

    kntrie_impl() : root_v(SENTINEL_TAGGED), size_v(0), alloc_v() {}

    ~kntrie_impl() { remove_all(); }

    kntrie_impl(const kntrie_impl&) = delete;
    kntrie_impl& operator=(const kntrie_impl&) = delete;

    kntrie_impl(kntrie_impl&& o) noexcept
        : root_v(o.root_v), size_v(o.size_v), alloc_v(std::move(o.alloc_v)) {
        o.root_v = SENTINEL_TAGGED;
        o.size_v = 0;
    }

    kntrie_impl& operator=(kntrie_impl&& o) noexcept {
        if (this != &o) {
            remove_all();
            root_v  = o.root_v;
            size_v  = o.size_v;
            alloc_v = std::move(o.alloc_v);
            o.root_v = SENTINEL_TAGGED;
            o.size_v = 0;
        }
        return *this;
    }

    void swap(kntrie_impl& o) noexcept {
        std::swap(root_v, o.root_v);
        std::swap(size_v, o.size_v);
        std::swap(alloc_v, o.alloc_v);
    }

    [[nodiscard]] bool      empty() const noexcept { return root_v == SENTINEL_TAGGED; }
    [[nodiscard]] size_type size()  const noexcept { return size_v; }
    [[nodiscard]] const ALLOC& get_allocator() const noexcept { return alloc_v; }

    void clear() noexcept {
        remove_all();
        size_v = 0;
    }

    // ==================================================================
    // Find — delegates to kntrie_ops<NK0, VALUE, ALLOC>
    // ==================================================================

    const VALUE* find_value(const KEY& key) const noexcept {
        IK ik = KO::to_internal(key);
        return OPS::template find_node<KEY_BITS>(root_v,
            static_cast<NK0>(ik >> (IK_BITS - KEY_BITS)));
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
    // Erase
    // ==================================================================

    bool erase(const KEY& key) {
        IK ik = KO::to_internal(key);

        if (root_v == SENTINEL_TAGGED) return false;

        NK0 nk = static_cast<NK0>(ik >> (IK_BITS - KEY_BITS));
        auto [new_tagged, erased, sub_ent] =
            OPS::template erase_node<KEY_BITS>(root_v, nk, alloc_v);
        if (!erased) return false;

        root_v = new_tagged ? new_tagged : SENTINEL_TAGGED;
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
        s.total_bytes = sizeof(uint64_t);  // root_v pointer
        if (root_v != SENTINEL_TAGGED)
            collect_stats(root_v, s);
        return s;
    }

    size_t memory_usage() const noexcept { return debug_stats().total_bytes; }

    struct root_info_t {
        uint16_t entries; uint8_t skip;
        bool is_leaf;
    };

    root_info_t debug_root_info() const {
        if (root_v == SENTINEL_TAGGED) return {0, 0, false};
        const uint64_t* node;
        bool leaf;
        if (root_v & LEAF_BIT) {
            node = untag_leaf(root_v);
            leaf = true;
        } else {
            node = bm_to_node_const(root_v);
            leaf = false;
        }
        auto* hdr = get_header(node);
        return {hdr->entries(), hdr->skip(), leaf};
    }

    const uint64_t* debug_root() const noexcept {
        if (root_v & LEAF_BIT) return untag_leaf(root_v);
        return bm_to_node_const(root_v);
    }

    // ==================================================================
    // Iterator support: traversal functions
    // ==================================================================

    struct iter_result_t { KEY key; VALUE value; bool found; };

    iter_result_t iter_first() const noexcept {
        if (root_v == SENTINEL_TAGGED) return {KEY{}, VALUE{}, false};
        auto r = ITER_OPS::template descend_min<KEY_BITS, IK>(root_v, IK{0}, 0);
        if (!r.found) return {KEY{}, VALUE{}, false};
        return {KO::to_key(r.key), *VT::as_ptr(*r.value), true};
    }

    iter_result_t iter_last() const noexcept {
        if (root_v == SENTINEL_TAGGED) return {KEY{}, VALUE{}, false};
        auto r = ITER_OPS::template descend_max<KEY_BITS, IK>(root_v, IK{0}, 0);
        if (!r.found) return {KEY{}, VALUE{}, false};
        return {KO::to_key(r.key), *VT::as_ptr(*r.value), true};
    }

    iter_result_t iter_next(KEY key) const noexcept {
        if (root_v == SENTINEL_TAGGED) return {KEY{}, VALUE{}, false};
        IK ik = KO::to_internal(key);
        NK0 nk = static_cast<NK0>(ik >> (IK_BITS - KEY_BITS));
        auto r = ITER_OPS::template iter_next_node<KEY_BITS, IK>(
            root_v, nk, IK{0}, 0);
        if (!r.found) return {KEY{}, VALUE{}, false};
        return {KO::to_key(r.key), *VT::as_ptr(*r.value), true};
    }

    iter_result_t iter_prev(KEY key) const noexcept {
        if (root_v == SENTINEL_TAGGED) return {KEY{}, VALUE{}, false};
        IK ik = KO::to_internal(key);
        NK0 nk = static_cast<NK0>(ik >> (IK_BITS - KEY_BITS));
        auto r = ITER_OPS::template iter_prev_node<KEY_BITS, IK>(
            root_v, nk, IK{0}, 0);
        if (!r.found) return {KEY{}, VALUE{}, false};
        return {KO::to_key(r.key), *VT::as_ptr(*r.value), true};
    }

private:
    // ==================================================================
    // Insert dispatch (shared by insert / insert_or_assign / assign)
    // ==================================================================

    template<bool INSERT, bool ASSIGN>
    std::pair<bool, bool> insert_dispatch(const KEY& key, const VALUE& value) {
        IK ik = KO::to_internal(key);
        VST sv = VT::store(value, alloc_v);
        NK0 nk = static_cast<NK0>(ik >> (IK_BITS - KEY_BITS));

        // Empty trie: create single-entry leaf
        if (root_v == SENTINEL_TAGGED) {
            if constexpr (!INSERT) { VT::destroy(sv, alloc_v); return {true, false}; }
            root_v = tag_leaf(OPS::make_single_leaf(nk, sv, alloc_v));
            ++size_v;
            return {true, true};
        }

        auto r = OPS::template insert_node<KEY_BITS, INSERT, ASSIGN>(
            root_v, nk, sv, alloc_v);
        if (r.tagged_ptr != root_v) root_v = r.tagged_ptr;
        if (r.inserted) { ++size_v; return {true, true}; }
        VT::destroy(sv, alloc_v);
        return {true, false};
    }

    // ==================================================================
    // Remove all (tagged)
    // ==================================================================

    void remove_all() noexcept {
        if (root_v != SENTINEL_TAGGED) {
            ITER_OPS::template remove_subtree<KEY_BITS>(root_v, alloc_v);
            root_v = SENTINEL_TAGGED;
        }
        size_v = 0;
    }

    // ==================================================================
    // Stats collection (tagged)
    // ==================================================================

    void collect_stats(uint64_t tagged, debug_stats_t& s) const noexcept {
        typename ITER_OPS::stats_t os{};
        ITER_OPS::template collect_stats<KEY_BITS>(tagged, os);
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
