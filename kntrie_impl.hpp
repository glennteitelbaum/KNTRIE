#ifndef KNTRIE_IMPL_HPP
#define KNTRIE_IMPL_HPP

#include "kntrie_ops.hpp"
#include "kntrie_iter_ops.hpp"
#include <memory>

namespace gteitelbaum {

template<typename KEY, typename VALUE, typename ALLOC = std::allocator<uint64_t>>
class kntrie_impl {
    static_assert(std::is_integral_v<KEY>, "KEY must be integral");
    static_assert(sizeof(KEY) >= 2, "KEY must be at least 16 bits");

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
    using Ops     = kntrie_ops<NK0, VALUE, ALLOC>;
    using IterOps = kntrie_iter_ops<NK0, VALUE, ALLOC>;

    uint64_t  root_;      // tagged pointer (LEAF_BIT for leaf, raw for bitmask)
    size_t    size_;
    [[no_unique_address]] ALLOC alloc_;

public:
    // ==================================================================
    // Constructor / Destructor
    // ==================================================================

    kntrie_impl() : root_(SENTINEL_TAGGED), size_(0), alloc_() {}

    ~kntrie_impl() { remove_all_(); }

    kntrie_impl(const kntrie_impl&) = delete;
    kntrie_impl& operator=(const kntrie_impl&) = delete;

    [[nodiscard]] bool      empty() const noexcept { return root_ == SENTINEL_TAGGED; }
    [[nodiscard]] size_type size()  const noexcept { return size_; }

    void clear() noexcept {
        remove_all_();
        size_ = 0;
    }

    // ==================================================================
    // Find — delegates to kntrie_ops<NK0, VALUE, ALLOC>
    // ==================================================================

    const VALUE* find_value(const KEY& key) const noexcept {
        IK ik = KO::to_internal(key);
        return Ops::template find_node_<KEY_BITS>(root_,
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
        return insert_dispatch_<true, false>(key, value);
    }

    // ==================================================================
    // Insert-or-assign (overwrites existing values)
    // ==================================================================

    std::pair<bool, bool> insert_or_assign(const KEY& key, const VALUE& value) {
        return insert_dispatch_<true, true>(key, value);
    }

    // ==================================================================
    // Assign (overwrite only, no insert if missing)
    // ==================================================================

    std::pair<bool, bool> assign(const KEY& key, const VALUE& value) {
        return insert_dispatch_<false, true>(key, value);
    }

    // ==================================================================
    // Erase
    // ==================================================================

    bool erase(const KEY& key) {
        IK ik = KO::to_internal(key);

        if (root_ == SENTINEL_TAGGED) return false;

        NK0 nk = static_cast<NK0>(ik >> (IK_BITS - KEY_BITS));
        auto [new_tagged, erased, sub_ent] =
            Ops::template erase_node_<KEY_BITS>(root_, nk, alloc_);
        if (!erased) return false;

        root_ = new_tagged ? new_tagged : SENTINEL_TAGGED;
        --size_;
        return true;
    }

    // ==================================================================
    // Stats / Memory
    // ==================================================================

    struct debug_stats_t {
        size_t compact_leaves = 0;
        size_t bitmap_leaves  = 0;
        size_t bitmask_nodes  = 0;
        size_t total_entries  = 0;
        size_t total_bytes    = 0;
    };

    debug_stats_t debug_stats() const noexcept {
        debug_stats_t s{};
        s.total_bytes = sizeof(uint64_t);  // root_ pointer
        if (root_ != SENTINEL_TAGGED)
            collect_stats_(root_, s);
        return s;
    }

    size_t memory_usage() const noexcept { return debug_stats().total_bytes; }

    struct root_info_t {
        uint16_t entries; uint8_t skip;
        bool is_leaf;
    };

    root_info_t debug_root_info() const {
        if (root_ == SENTINEL_TAGGED) return {0, 0, false};
        const uint64_t* node;
        bool leaf;
        if (root_ & LEAF_BIT) {
            node = untag_leaf(root_);
            leaf = true;
        } else {
            node = bm_to_node_const(root_);
            leaf = false;
        }
        auto* hdr = get_header(node);
        return {hdr->entries(), hdr->skip(), leaf};
    }

    const uint64_t* debug_root() const noexcept {
        if (root_ & LEAF_BIT) return untag_leaf(root_);
        return bm_to_node_const(root_);
    }

    // ==================================================================
    // Iterator support: traversal functions
    // ==================================================================

    struct iter_result_t { KEY key; VALUE value; bool found; };

    iter_result_t iter_first_() const noexcept {
        if (root_ == SENTINEL_TAGGED) return {KEY{}, VALUE{}, false};
        auto r = IterOps::template descend_min_<KEY_BITS, IK>(root_, IK{0}, 0);
        if (!r.found) return {KEY{}, VALUE{}, false};
        return {KO::to_key(r.key), *VT::as_ptr(*r.value), true};
    }

    iter_result_t iter_last_() const noexcept {
        if (root_ == SENTINEL_TAGGED) return {KEY{}, VALUE{}, false};
        auto r = IterOps::template descend_max_<KEY_BITS, IK>(root_, IK{0}, 0);
        if (!r.found) return {KEY{}, VALUE{}, false};
        return {KO::to_key(r.key), *VT::as_ptr(*r.value), true};
    }

    iter_result_t iter_next_(KEY key) const noexcept {
        if (root_ == SENTINEL_TAGGED) return {KEY{}, VALUE{}, false};
        IK ik = KO::to_internal(key);
        NK0 nk = static_cast<NK0>(ik >> (IK_BITS - KEY_BITS));
        auto r = IterOps::template iter_next_node_<KEY_BITS, IK>(
            root_, nk, IK{0}, 0);
        if (!r.found) return {KEY{}, VALUE{}, false};
        return {KO::to_key(r.key), *VT::as_ptr(*r.value), true};
    }

    iter_result_t iter_prev_(KEY key) const noexcept {
        if (root_ == SENTINEL_TAGGED) return {KEY{}, VALUE{}, false};
        IK ik = KO::to_internal(key);
        NK0 nk = static_cast<NK0>(ik >> (IK_BITS - KEY_BITS));
        auto r = IterOps::template iter_prev_node_<KEY_BITS, IK>(
            root_, nk, IK{0}, 0);
        if (!r.found) return {KEY{}, VALUE{}, false};
        return {KO::to_key(r.key), *VT::as_ptr(*r.value), true};
    }

private:
    // ==================================================================
    // Insert dispatch (shared by insert / insert_or_assign / assign)
    // ==================================================================

    template<bool INSERT, bool ASSIGN>
    std::pair<bool, bool> insert_dispatch_(const KEY& key, const VALUE& value) {
        IK ik = KO::to_internal(key);
        VST sv = VT::store(value, alloc_);
        NK0 nk = static_cast<NK0>(ik >> (IK_BITS - KEY_BITS));

        // Empty trie: create single-entry leaf
        if (root_ == SENTINEL_TAGGED) {
            if constexpr (!INSERT) { VT::destroy(sv, alloc_); return {true, false}; }
            root_ = tag_leaf(Ops::make_single_leaf_(nk, sv, alloc_));
            ++size_;
            return {true, true};
        }

        auto r = Ops::template insert_node_<KEY_BITS, INSERT, ASSIGN>(
            root_, nk, sv, alloc_);
        if (r.tagged_ptr != root_) root_ = r.tagged_ptr;
        if (r.inserted) { ++size_; return {true, true}; }
        VT::destroy(sv, alloc_);
        return {true, false};
    }

    // ==================================================================
    // Remove all (tagged)
    // ==================================================================

    void remove_all_() noexcept {
        if (root_ != SENTINEL_TAGGED) {
            IterOps::template remove_subtree_<KEY_BITS>(root_, alloc_);
            root_ = SENTINEL_TAGGED;
        }
        size_ = 0;
    }

    // ==================================================================
    // Stats collection (tagged)
    // ==================================================================

    void collect_stats_(uint64_t tagged, debug_stats_t& s) const noexcept {
        typename IterOps::stats_t os{};
        IterOps::template collect_stats_<KEY_BITS>(tagged, os);
        s.total_bytes    += os.total_bytes;
        s.total_entries  += os.total_entries;
        s.bitmap_leaves  += os.bitmap_leaves;
        s.compact_leaves += os.compact_leaves;
        s.bitmask_nodes  += os.bitmask_nodes;
    }
};

} // namespace gteitelbaum

#endif // KNTRIE_IMPL_HPP
