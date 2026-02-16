#ifndef KNTRIE_IMPL_HPP
#define KNTRIE_IMPL_HPP

#include "kntrie_ops.hpp"
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
    using CO16 = compact_ops<uint16_t, VALUE, ALLOC>;
    using CO32 = compact_ops<uint32_t, VALUE, ALLOC>;
    using CO64 = compact_ops<uint64_t, VALUE, ALLOC>;

    static constexpr int IK_BITS  = KO::IK_BITS;
    static constexpr int KEY_BITS = KO::KEY_BITS;

    // NK0 = initial narrowed key type matching KEY width
    using NK0 = std::conditional_t<KEY_BITS <= 8,  uint8_t,
                std::conditional_t<KEY_BITS <= 16, uint16_t,
                std::conditional_t<KEY_BITS <= 32, uint32_t, uint64_t>>>;
    using Ops = kntrie_ops<NK0, VALUE, ALLOC>;

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

        auto [new_tagged, erased, sub_ent] = erase_node_(root_, ik, KEY_BITS);
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
        return descend_min_(root_, IK{0}, 0);
    }

    iter_result_t iter_last_() const noexcept {
        if (root_ == SENTINEL_TAGGED) return {KEY{}, VALUE{}, false};
        return descend_max_(root_, IK{0}, 0);
    }

    iter_result_t iter_next_(KEY key) const noexcept {
        if (root_ == SENTINEL_TAGGED) return {KEY{}, VALUE{}, false};
        return iter_next_node_(root_, KO::to_internal(key), IK{0}, 0);
    }

    iter_result_t iter_prev_(KEY key) const noexcept {
        if (root_ == SENTINEL_TAGGED) return {KEY{}, VALUE{}, false};
        return iter_prev_node_(root_, KO::to_internal(key), IK{0}, 0);
    }

private:
    // ==================================================================
    // Iterator helpers (private)
    // ==================================================================

    // Recursive: find smallest key > ik in subtree at ptr
    iter_result_t iter_next_node_(uint64_t ptr, IK ik, IK prefix, int bits) const noexcept {
        // --- LEAF ---
        if (ptr & LEAF_BIT) {
            const uint64_t* node = untag_leaf(ptr);
            node_header hdr = *get_header(node);
            if (hdr.entries() == 0) return {KEY{}, VALUE{}, false};

            size_t hs = 1;
            if (hdr.is_skip()) {
                hs = 2;
                const uint8_t* sb = reinterpret_cast<const uint8_t*>(&node[1]);
                uint8_t skip = hdr.skip();
                for (uint8_t i = 0; i < skip; ++i) {
                    uint8_t kb = static_cast<uint8_t>(ik >> (IK_BITS - 8));
                    if (kb < sb[i]) {
                        for (uint8_t j = i; j < skip; ++j) {
                            prefix |= IK(sb[j]) << (IK_BITS - bits - 8);
                            bits += 8;
                        }
                        return leaf_first_(node, hdr, prefix, bits, hs);
                    }
                    if (kb > sb[i]) return {KEY{}, VALUE{}, false};
                    prefix |= IK(sb[i]) << (IK_BITS - bits - 8);
                    bits += 8;
                    ik <<= 8;
                }
            }
            return leaf_next_dispatch_(node, hdr, ik, prefix, bits, hs);
        }

        // --- BITMASK ---
        const auto& bitmap = BO::bitmap_ref(ptr);
        uint8_t byte = static_cast<uint8_t>(ik >> (IK_BITS - 8));

        if (bitmap.has_bit(byte)) {
            int slot = bitmap.template find_slot<slot_mode::UNFILTERED>(byte);
            IK cp = prefix | (IK(byte) << (IK_BITS - bits - 8));
            auto r = iter_next_node_(BO::child_at(ptr, slot),
                                     static_cast<IK>(ik << 8), cp, bits + 8);
            if (r.found) return r;
            // Child exhausted — fall through to next sibling
        }

        auto adj = bitmap.next_set_after(byte);
        if (adj.found) {
            IK np = prefix | (IK(adj.idx) << (IK_BITS - bits - 8));
            return descend_min_(BO::child_at(ptr, adj.slot), np, bits + 8);
        }
        return {KEY{}, VALUE{}, false};
    }

    // Recursive: find largest key < ik in subtree at ptr
    iter_result_t iter_prev_node_(uint64_t ptr, IK ik, IK prefix, int bits) const noexcept {
        // --- LEAF ---
        if (ptr & LEAF_BIT) {
            const uint64_t* node = untag_leaf(ptr);
            node_header hdr = *get_header(node);
            if (hdr.entries() == 0) return {KEY{}, VALUE{}, false};

            size_t hs = 1;
            if (hdr.is_skip()) {
                hs = 2;
                const uint8_t* sb = reinterpret_cast<const uint8_t*>(&node[1]);
                uint8_t skip = hdr.skip();
                for (uint8_t i = 0; i < skip; ++i) {
                    uint8_t kb = static_cast<uint8_t>(ik >> (IK_BITS - 8));
                    if (kb > sb[i]) {
                        for (uint8_t j = i; j < skip; ++j) {
                            prefix |= IK(sb[j]) << (IK_BITS - bits - 8);
                            bits += 8;
                        }
                        return leaf_last_(node, hdr, prefix, bits, hs);
                    }
                    if (kb < sb[i]) return {KEY{}, VALUE{}, false};
                    prefix |= IK(sb[i]) << (IK_BITS - bits - 8);
                    bits += 8;
                    ik <<= 8;
                }
            }
            return leaf_prev_dispatch_(node, hdr, ik, prefix, bits, hs);
        }

        // --- BITMASK ---
        const auto& bitmap = BO::bitmap_ref(ptr);
        uint8_t byte = static_cast<uint8_t>(ik >> (IK_BITS - 8));

        if (bitmap.has_bit(byte)) {
            int slot = bitmap.template find_slot<slot_mode::UNFILTERED>(byte);
            IK cp = prefix | (IK(byte) << (IK_BITS - bits - 8));
            auto r = iter_prev_node_(BO::child_at(ptr, slot),
                                     static_cast<IK>(ik << 8), cp, bits + 8);
            if (r.found) return r;
            // Child exhausted — fall through to prev sibling
        }

        auto adj = bitmap.prev_set_before(byte);
        if (adj.found) {
            IK np = prefix | (IK(adj.idx) << (IK_BITS - bits - 8));
            return descend_max_(BO::child_at(ptr, adj.slot), np, bits + 8);
        }
        return {KEY{}, VALUE{}, false};
    }
    // ==================================================================

    // Reconstruct full IK from prefix + suffix
    static IK combine_suffix_(IK prefix, int bits, uint8_t st, uint64_t suffix_val) noexcept {
        IK suffix_ik;
        switch (st) {
            case 0: suffix_ik = IK(suffix_val) << (IK_BITS - 8); break;
            case 1: suffix_ik = IK(suffix_val) << (IK_BITS - 16); break;
            case 2: suffix_ik = IK(suffix_val) << (IK_BITS - 32); break;
            default: suffix_ik = IK(suffix_val); break;
        }
        return prefix | (suffix_ik >> bits);
    }

    // Descend always-min from tagged ptr
    iter_result_t descend_min_(uint64_t ptr, IK prefix, int bits) const noexcept {
        while (!(ptr & LEAF_BIT)) {
            const auto& bitmap = BO::bitmap_ref(ptr);
            uint8_t byte = bitmap.first_set_bit();
            prefix |= IK(byte) << (IK_BITS - bits - 8);
            bits += 8;
            ptr = BO::first_child(ptr);
        }
        const uint64_t* node = untag_leaf(ptr);
        node_header hdr = *get_header(node);
        if (hdr.entries() == 0) return {KEY{}, VALUE{}, false};
        size_t hs = 1;
        if (hdr.is_skip()) {
            hs = 2;
            const uint8_t* sb = reinterpret_cast<const uint8_t*>(&node[1]);
            for (uint8_t i = 0; i < hdr.skip(); ++i) {
                prefix |= IK(sb[i]) << (IK_BITS - bits - 8);
                bits += 8;
            }
        }
        return leaf_first_(node, hdr, prefix, bits, hs);
    }

    // Descend always-max from tagged ptr
    iter_result_t descend_max_(uint64_t ptr, IK prefix, int bits) const noexcept {
        while (!(ptr & LEAF_BIT)) {
            const auto& bitmap = BO::bitmap_ref(ptr);
            uint8_t byte = bitmap.last_set_bit();
            int slot = bitmap.template find_slot<slot_mode::UNFILTERED>(byte);
            prefix |= IK(byte) << (IK_BITS - bits - 8);
            bits += 8;
            ptr = BO::child_at(ptr, slot);
        }
        const uint64_t* node = untag_leaf(ptr);
        node_header hdr = *get_header(node);
        if (hdr.entries() == 0) return {KEY{}, VALUE{}, false};
        size_t hs = 1;
        if (hdr.is_skip()) {
            hs = 2;
            const uint8_t* sb = reinterpret_cast<const uint8_t*>(&node[1]);
            for (uint8_t i = 0; i < hdr.skip(); ++i) {
                prefix |= IK(sb[i]) << (IK_BITS - bits - 8);
                bits += 8;
            }
        }
        return leaf_last_(node, hdr, prefix, bits, hs);
    }

    // Leaf first/last/next/prev dispatch by suffix type
    iter_result_t leaf_first_(const uint64_t* node, node_header hdr,
                               IK prefix, int bits, size_t hs) const noexcept {
        uint8_t st = hdr.suffix_type();
        if (st == 0) {
            auto r = BO::bitmap_iter_first(node, hs);
            return {KO::to_key(combine_suffix_(prefix, bits, 0, r.suffix)),
                    *VT::as_ptr(*r.value), true};
        }
        if (st == 1) {
            auto r = CO16::iter_first(node, &hdr);
            if (!r.found) return {KEY{}, VALUE{}, false};
            return {KO::to_key(combine_suffix_(prefix, bits, 1, r.suffix)),
                    *VT::as_ptr(*r.value), true};
        }
        if constexpr (KEY_BITS > 16) {
            if (st == 2) {
                auto r = CO32::iter_first(node, &hdr);
                if (!r.found) return {KEY{}, VALUE{}, false};
                return {KO::to_key(combine_suffix_(prefix, bits, 2, r.suffix)),
                        *VT::as_ptr(*r.value), true};
            }
            if constexpr (KEY_BITS > 32) {
                auto r = CO64::iter_first(node, &hdr);
                if (!r.found) return {KEY{}, VALUE{}, false};
                return {KO::to_key(combine_suffix_(prefix, bits, 3, r.suffix)),
                        *VT::as_ptr(*r.value), true};
            }
        }
        __builtin_unreachable();
    }

    iter_result_t leaf_last_(const uint64_t* node, node_header hdr,
                              IK prefix, int bits, size_t hs) const noexcept {
        uint8_t st = hdr.suffix_type();
        if (st == 0) {
            auto r = BO::bitmap_iter_last(node, hdr, hs);
            return {KO::to_key(combine_suffix_(prefix, bits, 0, r.suffix)),
                    *VT::as_ptr(*r.value), true};
        }
        if (st == 1) {
            auto r = CO16::iter_last(node, &hdr);
            if (!r.found) return {KEY{}, VALUE{}, false};
            return {KO::to_key(combine_suffix_(prefix, bits, 1, r.suffix)),
                    *VT::as_ptr(*r.value), true};
        }
        if constexpr (KEY_BITS > 16) {
            if (st == 2) {
                auto r = CO32::iter_last(node, &hdr);
                if (!r.found) return {KEY{}, VALUE{}, false};
                return {KO::to_key(combine_suffix_(prefix, bits, 2, r.suffix)),
                        *VT::as_ptr(*r.value), true};
            }
            if constexpr (KEY_BITS > 32) {
                auto r = CO64::iter_last(node, &hdr);
                if (!r.found) return {KEY{}, VALUE{}, false};
                return {KO::to_key(combine_suffix_(prefix, bits, 3, r.suffix)),
                        *VT::as_ptr(*r.value), true};
            }
        }
        __builtin_unreachable();
    }

    iter_result_t leaf_next_dispatch_(const uint64_t* node, node_header hdr,
                                       IK ik, IK prefix, int bits, size_t hs) const noexcept {
        uint8_t st = hdr.suffix_type();
        if (st == 0) {
            uint8_t suf = static_cast<uint8_t>(ik >> (IK_BITS - 8));
            auto r = BO::bitmap_iter_next(node, suf, hs);
            if (!r.found) return {KEY{}, VALUE{}, false};
            return {KO::to_key(combine_suffix_(prefix, bits, 0, r.suffix)),
                    *VT::as_ptr(*r.value), true};
        }
        if (st == 1) {
            auto suf = static_cast<uint16_t>(ik >> (IK_BITS - 16));
            auto r = CO16::iter_next(node, &hdr, suf);
            if (!r.found) return {KEY{}, VALUE{}, false};
            return {KO::to_key(combine_suffix_(prefix, bits, 1, r.suffix)),
                    *VT::as_ptr(*r.value), true};
        }
        if constexpr (KEY_BITS > 16) {
            if (st == 2) {
                auto suf = static_cast<uint32_t>(ik >> (IK_BITS - 32));
                auto r = CO32::iter_next(node, &hdr, suf);
                if (!r.found) return {KEY{}, VALUE{}, false};
                return {KO::to_key(combine_suffix_(prefix, bits, 2, r.suffix)),
                        *VT::as_ptr(*r.value), true};
            }
            if constexpr (KEY_BITS > 32) {
                auto suf = static_cast<uint64_t>(ik);
                auto r = CO64::iter_next(node, &hdr, suf);
                if (!r.found) return {KEY{}, VALUE{}, false};
                return {KO::to_key(combine_suffix_(prefix, bits, 3, r.suffix)),
                        *VT::as_ptr(*r.value), true};
            }
        }
        __builtin_unreachable();
    }

    iter_result_t leaf_prev_dispatch_(const uint64_t* node, node_header hdr,
                                       IK ik, IK prefix, int bits, size_t hs) const noexcept {
        uint8_t st = hdr.suffix_type();
        if (st == 0) {
            uint8_t suf = static_cast<uint8_t>(ik >> (IK_BITS - 8));
            auto r = BO::bitmap_iter_prev(node, suf, hs);
            if (!r.found) return {KEY{}, VALUE{}, false};
            return {KO::to_key(combine_suffix_(prefix, bits, 0, r.suffix)),
                    *VT::as_ptr(*r.value), true};
        }
        if (st == 1) {
            auto suf = static_cast<uint16_t>(ik >> (IK_BITS - 16));
            auto r = CO16::iter_prev(node, &hdr, suf);
            if (!r.found) return {KEY{}, VALUE{}, false};
            return {KO::to_key(combine_suffix_(prefix, bits, 1, r.suffix)),
                    *VT::as_ptr(*r.value), true};
        }
        if constexpr (KEY_BITS > 16) {
            if (st == 2) {
                auto suf = static_cast<uint32_t>(ik >> (IK_BITS - 32));
                auto r = CO32::iter_prev(node, &hdr, suf);
                if (!r.found) return {KEY{}, VALUE{}, false};
                return {KO::to_key(combine_suffix_(prefix, bits, 2, r.suffix)),
                        *VT::as_ptr(*r.value), true};
            }
            if constexpr (KEY_BITS > 32) {
                auto suf = static_cast<uint64_t>(ik);
                auto r = CO64::iter_prev(node, &hdr, suf);
                if (!r.found) return {KEY{}, VALUE{}, false};
                return {KO::to_key(combine_suffix_(prefix, bits, 3, r.suffix)),
                        *VT::as_ptr(*r.value), true};
            }
        }
        __builtin_unreachable();
    }
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
    // ==================================================================
    // erase_node (recursive, tagged)
    //
    // ptr: tagged pointer
    // Returns: erase_result_t with tagged_ptr (0 if fully erased)
    //          and subtree_entries for coalesce walk-up
    // ==================================================================

    erase_result_t erase_node_(uint64_t ptr, IK ik, int bits) {

        // --- SENTINEL ---
        if (ptr == SENTINEL_TAGGED) return {ptr, false, 0};

        // --- LEAF ---
        if (ptr & LEAF_BIT) {
            uint64_t* node = untag_leaf_mut(ptr);
            auto* hdr = get_header(node);

            // Leaf skip check
            uint8_t skip = hdr->skip();
            if (skip) [[unlikely]] {
                const uint8_t* actual = hdr->prefix_bytes();
                for (uint8_t i = 0; i < skip; ++i) {
                    uint8_t expected = static_cast<uint8_t>(ik >> (IK_BITS - 8));
                    if (expected != actual[i]) return {ptr, false, 0};
                    ik = static_cast<IK>(ik << 8);
                    bits -= 8;
                }
            }

            // leaf_erase_ returns tagged result with subtree_entries
            return leaf_erase_(node, hdr, ik);
        }

        // --- BITMASK ---
        uint64_t* node = bm_to_node(ptr);
        auto* hdr = get_header(node);
        uint8_t sc = hdr->skip();

        if (sc > 0)
            return erase_skip_chain_(node, hdr, sc, ik, bits);

        // Standalone bitmask (skip=0)
        uint8_t ti = static_cast<uint8_t>(ik >> (IK_BITS - 8));
        auto lk = BO::lookup(node, ti);
        if (!lk.found) return {tag_bitmask(node), false, 0};

        // Recurse into child
        auto cr = erase_node_(lk.child, static_cast<IK>(ik << 8), bits - 8);
        if (!cr.erased) return {tag_bitmask(node), false, 0};

        if (cr.tagged_ptr) {
            // Child survived
            if (cr.tagged_ptr != lk.child)
                BO::set_child(node, lk.slot, cr.tagged_ptr);
            // Update desc for this slot
            BO::child_desc_array(node)[lk.slot] = cr.subtree_entries;
            // If child is still above COMPACT_MAX, so is parent — bail
            if (cr.subtree_entries == COALESCE_CAP)
                return {tag_bitmask(node), true, COALESCE_CAP};
            // Child returned exact count — decrement if exact, recompute if capped
            uint16_t d = hdr->descendants();
            if (d == COALESCE_CAP) {
                d = Ops::sum_children_desc_(node, 0);
                hdr->set_descendants(d);
            } else {
                --d;
                hdr->set_descendants(d);
            }
            if (d <= COMPACT_MAX)
                return do_coalesce_(node, hdr, bits, d);
            return {tag_bitmask(node), true, d};
        }

        // Child fully erased — remove from bitmask
        auto* nn = BO::remove_child(node, hdr, lk.slot, ti, alloc_);
        if (!nn) return {0, true, 0};

        // Collapse: single-child bitmask
        if (get_header(nn)->entries() == 1) {
            auto ci = BO::standalone_collapse_info(nn);
            size_t nn_au64 = get_header(nn)->alloc_u64();

            if (ci.sole_child & LEAF_BIT) {
                uint64_t* leaf = untag_leaf_mut(ci.sole_child);
                leaf = Ops::prepend_skip_(leaf, ci.total_skip, ci.bytes, alloc_);
                dealloc_node(alloc_, nn, nn_au64);
                return {tag_leaf(leaf), true, ci.sole_entries};
            }
            uint64_t* child_node = bm_to_node(ci.sole_child);
            dealloc_node(alloc_, nn, nn_au64);
            return {BO::wrap_in_chain(child_node, ci.bytes, ci.total_skip, alloc_),
                    true, ci.sole_entries};
        }

        // Multi-child: decrement descendants, check coalesce
        uint16_t desc = Ops::dec_or_recompute_desc_(nn, 0);
        if (desc <= COMPACT_MAX)
            return do_coalesce_(nn, get_header(nn), bits, desc);
        return {tag_bitmask(nn), true, desc};
    }

    // ==================================================================
    // erase_skip_chain_: walk embedded bo<1> nodes, erase from final
    //
    // Uses stored descendants for O(1) coalesce check.
    // ==================================================================

    erase_result_t erase_skip_chain_(uint64_t* node, node_header* hdr,
                                       uint8_t sc, IK ik, int bits) {
        int orig_bits = bits;  // save for coalesce (includes skip)

        for (uint8_t e = 0; e < sc; ++e) {
            uint8_t actual = BO::skip_byte(node, e);
            uint8_t expected = static_cast<uint8_t>(ik >> (IK_BITS - 8));
            if (expected != actual) return {tag_bitmask(node), false, 0};
            ik = static_cast<IK>(ik << 8);
            bits -= 8;
        }

        // Final bitmask
        uint8_t ti = static_cast<uint8_t>(ik >> (IK_BITS - 8));
        auto cl = BO::chain_lookup(node, sc, ti);
        if (!cl.found) return {tag_bitmask(node), false, 0};

        uint64_t old_child = cl.child;

        auto cr = erase_node_(old_child, static_cast<IK>(ik << 8), bits - 8);
        if (!cr.erased) return {tag_bitmask(node), false, 0};

        if (cr.tagged_ptr) {
            // Child survived
            if (cr.tagged_ptr != old_child)
                BO::chain_set_child(node, sc, cl.slot, cr.tagged_ptr);
            // Update desc for this slot
            unsigned nc_cur = hdr->entries();
            uint16_t* da = BO::chain_desc_array_mut(node, sc, nc_cur);
            da[cl.slot] = cr.subtree_entries;
            if (cr.subtree_entries == COALESCE_CAP)
                return {tag_bitmask(node), true, COALESCE_CAP};
            uint16_t d = hdr->descendants();
            if (d == COALESCE_CAP) {
                d = Ops::sum_children_desc_(node, sc);
                hdr->set_descendants(d);
            } else {
                --d;
                hdr->set_descendants(d);
            }
            if (d <= COMPACT_MAX)
                return do_coalesce_(node, hdr, orig_bits, d);
            return {tag_bitmask(node), true, d};
        }

        // Child erased — remove from final bitmask
        node = BO::chain_remove_child(node, hdr, sc, cl.slot, ti, alloc_);
        if (!node) return {0, true, 0};

        hdr = get_header(node);
        unsigned nc = hdr->entries();

        // Collapse when final drops to 1 child
        if (nc == 1) {
            auto ci = BO::chain_collapse_info(node, sc);
            size_t node_au64 = hdr->alloc_u64();

            if (ci.sole_child & LEAF_BIT) {
                uint64_t* leaf = untag_leaf_mut(ci.sole_child);
                leaf = Ops::prepend_skip_(leaf, ci.total_skip, ci.bytes, alloc_);
                dealloc_node(alloc_, node, node_au64);
                return {tag_leaf(leaf), true, ci.sole_entries};
            }

            uint64_t* child_node = bm_to_node(ci.sole_child);
            dealloc_node(alloc_, node, node_au64);
            return {BO::wrap_in_chain(child_node, ci.bytes, ci.total_skip, alloc_),
                    true, ci.sole_entries};
        }

        // Multi-child: decrement descendants, check coalesce
        uint16_t desc = Ops::dec_or_recompute_desc_(node, sc);
        if (desc <= COMPACT_MAX)
            return do_coalesce_(node, hdr, orig_bits, desc);
        return {tag_bitmask(node), true, desc};
    }

    // ==================================================================
    // leaf_erase: dispatch by suffix_type
    // Returns tagged result
    // ==================================================================

    erase_result_t leaf_erase_(uint64_t* node, node_header* hdr, IK ik) {
        uint8_t st = hdr->suffix_type();

        if (st == 0)
            return BO::bitmap_erase(node,
                static_cast<uint8_t>(ik >> (IK_BITS - 8)), alloc_);

        if constexpr (KEY_BITS > 16) {
            if (st & 0b10) {
                if constexpr (KEY_BITS > 32) {
                    if (st & 0b01)
                        return CO64::erase(node, hdr,
                            static_cast<uint64_t>(ik), alloc_);
                }
                return CO32::erase(node, hdr,
                    static_cast<uint32_t>(ik >> (IK_BITS - 32)), alloc_);
            }
        }

        return CO16::erase(node, hdr,
            static_cast<uint16_t>(ik >> (IK_BITS - 16)), alloc_);
    }

    // ==================================================================
    // Coalesce: collapse bitmask subtree back into compact leaf
    //
    // Descendant tracking via stored counts makes coalesce O(1) check.
    // do_coalesce_: rebuild as leaf (caller already verified total <= COMPACT_MAX)
    // collect_entries_tagged_: gather (suffix_u64, value_slot) pairs
    // dealloc_bitmask_subtree_: free bitmask nodes (NOT leaf values)
    // ==================================================================

    // ------------------------------------------------------------------
    // Descendant tracking helpers
    //
    // Bitmask nodes store total descendant count in total_slots_ field
    // (unused by bitmask). Capped at COALESCE_CAP (COMPACT_MAX + 1).
    // Leaf nodes: use entries() instead.
    // ------------------------------------------------------------------

    // Sum immediate children's counts from local desc array. No pointer chasing.
    // Early exit when > COMPACT_MAX. Unchecked children assumed to have ≥ 1.
    // Coalesce: caller already verified total <= COMPACT_MAX.
    erase_result_t do_coalesce_(uint64_t* node, node_header* hdr,
                                  int bits, uint16_t total_entries) {
        uint8_t sc = hdr->skip();
        uint64_t tagged = tag_bitmask(node);

        // Collect all entries
        auto wk = std::make_unique<uint64_t[]>(total_entries);
        auto wv = std::make_unique<VST[]>(total_entries);
        size_t wi = 0;
        collect_entries_tagged_(tagged, 0, 0, wk.get(), wv.get(), wi);

        // Strip skip bytes from collected keys
        int leaf_bits = bits - sc * 8;
        if (sc > 0) {
            unsigned shift = sc * 8;
            for (size_t i = 0; i < total_entries; ++i)
                wk[i] <<= shift;
        }

        uint64_t* leaf = build_leaf_from_arrays_(wk.get(), wv.get(), total_entries, leaf_bits);

        if (sc > 0) {
            uint8_t skip_bytes[6];
            for (uint8_t i = 0; i < sc; ++i) {
                uint64_t* eb = node + 1 + i * 6;
                skip_bytes[i] = reinterpret_cast<const bitmap256*>(eb)->single_bit_index();
            }
            leaf = Ops::prepend_skip_(leaf, sc, skip_bytes, alloc_);
        }

        Ops::dealloc_bitmask_subtree_(tagged, alloc_);
        return {tag_leaf(leaf), true, COALESCE_CAP};
    }

    // prefix: accumulated bits so far, shifted into top of uint64_t
    void collect_entries_tagged_(uint64_t tagged, uint64_t prefix, int prefix_bits,
                                  uint64_t* keys, VST* vals, size_t& wi) const {
        if (tagged & LEAF_BIT) {
            const uint64_t* node = untag_leaf(tagged);
            auto* hdr = get_header(node);

            // Account for leaf skip
            uint8_t skip = hdr->skip();
            if (skip) {
                const uint8_t* pb = hdr->prefix_bytes();
                for (uint8_t i = 0; i < skip; ++i) {
                    prefix |= uint64_t(pb[i]) << (56 - prefix_bits);
                    prefix_bits += 8;
                }
            }

            leaf_for_each_u64_(node, hdr, [&](uint64_t suf, VST v) {
                // suf is bit-63-aligned within its suffix_type width
                // Combine prefix + suf shifted right
                uint64_t combined = prefix | (suf >> prefix_bits);
                keys[wi] = combined;
                vals[wi] = v;
                wi++;
            });
            return;
        }

        const uint64_t* node = bm_to_node_const(tagged);
        auto* hdr = get_header(node);
        uint8_t sc = hdr->skip();

        // Accumulate skip bytes into prefix
        uint64_t cur_prefix = prefix;
        int cur_bits = prefix_bits;
        for (uint8_t i = 0; i < sc; ++i) {
            uint8_t byte = BO::skip_byte(node, i);
            cur_prefix |= uint64_t(byte) << (56 - cur_bits);
            cur_bits += 8;
        }

        const bitmap256& fbm = BO::chain_bitmap(node, sc);
        const uint64_t* rch = BO::chain_children(node, sc);
        fbm.for_each_set([&](uint8_t idx, int slot) {
            uint64_t child_prefix = cur_prefix | (uint64_t(idx) << (56 - cur_bits));
            collect_entries_tagged_(rch[slot], child_prefix, cur_bits + 8,
                                     keys, vals, wi);
        });
    }

    // Build leaf from bit-63-aligned sorted arrays (leaf-only path)
    uint64_t* build_leaf_from_arrays_(uint64_t* suf, VST* vals,
                                       size_t count, int bits) {
        uint8_t st = suffix_type_for(bits);
        if (st == 0) {
            auto bk = std::make_unique<uint8_t[]>(count);
            for (size_t i = 0; i < count; ++i)
                bk[i] = static_cast<uint8_t>(suf[i] >> 56);
            return BO::make_bitmap_leaf(bk.get(), vals,
                static_cast<uint32_t>(count), alloc_);
        }
        if (st == 1) {
            auto tk = std::make_unique<uint16_t[]>(count);
            for (size_t i = 0; i < count; ++i)
                tk[i] = static_cast<uint16_t>(suf[i] >> 48);
            return CO16::make_leaf(tk.get(), vals,
                static_cast<uint32_t>(count), 0, nullptr, alloc_);
        }
        if constexpr (KEY_BITS > 16) {
            if (st == 2) {
                auto tk = std::make_unique<uint32_t[]>(count);
                for (size_t i = 0; i < count; ++i)
                    tk[i] = static_cast<uint32_t>(suf[i] >> 32);
                return CO32::make_leaf(tk.get(), vals,
                    static_cast<uint32_t>(count), 0, nullptr, alloc_);
            }
        }
        if constexpr (KEY_BITS > 32) {
            return CO64::make_leaf(suf, vals,
                static_cast<uint32_t>(count), 0, nullptr, alloc_);
        }
        __builtin_unreachable();
    }

    // ==================================================================
    // leaf_for_each_u64: iterate leaf entries as bit-63-aligned uint64_t
    // ==================================================================

    template<typename Fn>
    static void leaf_for_each_u64_(const uint64_t* node, const node_header* hdr,
                                    Fn&& cb) {
        uint8_t st = hdr->suffix_type();
        if (st == 0) {
            BO::for_each_bitmap(node, [&](uint8_t s, VST v) {
                cb(uint64_t(s) << 56, v);
            });
        } else if (st == 1) {
            CO16::for_each(node, hdr, [&](uint16_t s, VST v) {
                cb(uint64_t(s) << 48, v);
            });
        } else if constexpr (KEY_BITS > 16) {
            if (st == 2) {
                CO32::for_each(node, hdr, [&](uint32_t s, VST v) {
                    cb(uint64_t(s) << 32, v);
                });
            } else if constexpr (KEY_BITS > 32) {
                CO64::for_each(node, hdr, [&](uint64_t s, VST v) {
                    cb(s, v);
                });
            }
        }
    }

    // ==================================================================
    // Remove all (tagged)
    // ==================================================================

    void remove_all_() noexcept {
        if (root_ != SENTINEL_TAGGED) {
            remove_node_(root_);
            root_ = SENTINEL_TAGGED;
        }
        size_ = 0;
    }

    void remove_node_(uint64_t tagged) noexcept {
        if (tagged == SENTINEL_TAGGED) return;

        if (tagged & LEAF_BIT) {
            uint64_t* node = untag_leaf_mut(tagged);
            auto* hdr = get_header(node);
            destroy_leaf_(node, hdr);
        } else {
            uint64_t* node = bm_to_node(tagged);
            auto* hdr = get_header(node);
            uint8_t sc = hdr->skip();

            // For skip chains: only recurse into final bitmask's children
            // (embeds are internal pointers within the same allocation)
            BO::chain_for_each_child(node, sc, [&](unsigned, uint64_t child) {
                remove_node_(child);
            });

            BO::dealloc_bitmask(node, alloc_);
        }
    }

    void destroy_leaf_(uint64_t* node, node_header* hdr) noexcept {
        switch (hdr->suffix_type()) {
            case 0: BO::bitmap_destroy_and_dealloc(node, alloc_); break;
            case 1: CO16::destroy_and_dealloc(node, alloc_); break;
            case 2:
                if constexpr (KEY_BITS > 16)
                    CO32::destroy_and_dealloc(node, alloc_);
                break;
            case 3:
                if constexpr (KEY_BITS > 32)
                    CO64::destroy_and_dealloc(node, alloc_);
                break;
        }
    }

    // ==================================================================
    // Stats collection (tagged)
    // ==================================================================

    void collect_stats_(uint64_t tagged, debug_stats_t& s) const noexcept {
        if (tagged & LEAF_BIT) {
            const uint64_t* node = untag_leaf(tagged);
            auto* hdr = get_header(node);
            s.total_bytes += static_cast<size_t>(hdr->alloc_u64()) * 8;
            s.total_entries += hdr->entries();
            if (hdr->suffix_type() == 0)
                s.bitmap_leaves++;
            else
                s.compact_leaves++;
        } else {
            const uint64_t* node = bm_to_node_const(tagged);
            auto* hdr = get_header(node);
            s.total_bytes += static_cast<size_t>(hdr->alloc_u64()) * 8;
            s.bitmask_nodes++;

            uint8_t sc = hdr->skip();
            BO::chain_for_each_child(node, sc, [&](unsigned, uint64_t child) {
                collect_stats_(child, s);
            });
        }
    }
};

} // namespace gteitelbaum

#endif // KNTRIE_IMPL_HPP
