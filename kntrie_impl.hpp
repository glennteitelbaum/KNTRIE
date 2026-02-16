#ifndef KNTRIE_IMPL_HPP
#define KNTRIE_IMPL_HPP

#include "kntrie_compact.hpp"
#include "kntrie_bitmask.hpp"
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
    // Find
    //
    // Hot loop: bitmask ptr is raw (no LEAF_BIT), use directly.
    // Exit: leaf ptr has LEAF_BIT, strip unconditionally.
    // No sentinel check — sentinel is a zeroed leaf, dispatch
    // returns nullptr naturally for entries=0.
    // ==================================================================

    const VALUE* find_value(const KEY& key) const noexcept {
        IK ik = KO::to_internal(key);
        uint64_t ptr = root_;

        // Bitmask descent — ptr is raw usable pointer (no LEAF_BIT)
        while (!(ptr & LEAF_BIT)) [[likely]] {
            const uint64_t* bm = reinterpret_cast<const uint64_t*>(ptr);
            uint8_t ti = static_cast<uint8_t>(ik >> (IK_BITS - 8));
            ik <<= 8;
            int slot = reinterpret_cast<const bitmap256*>(bm)->
                           find_slot<slot_mode::BRANCHLESS>(ti);
            ptr = bm[BITMAP256_U64 + slot];
        }

        // Leaf — strip LEAF_BIT unconditionally (we know it's set)
        const uint64_t* node = reinterpret_cast<const uint64_t*>(ptr ^ LEAF_BIT);
        node_header hdr = *get_header(node);
        // No sentinel check — sentinel is a valid zeroed leaf (entries=0,
        // suffix_type=0). Leaf dispatch naturally returns nullptr.

        // Leaf skip check
        size_t hs = 1;
        if (hdr.is_skip()) [[unlikely]] {
            hs = 2;
            const uint8_t* actual = reinterpret_cast<const uint8_t*>(&node[1]);
            uint8_t skip = hdr.skip();
            uint8_t i = 0;
            do {
                if (static_cast<uint8_t>(ik >> (IK_BITS - 8)) != actual[i]) [[unlikely]]
                    return nullptr;
                ik <<= 8;
            } while (++i < skip);
        }

        // Leaf dispatch by suffix_type
        uint8_t st = hdr.suffix_type();

        if (st <= 1) [[likely]] {
            if (st == 0)
                return BO::bitmap_find(node, hdr,
                    static_cast<uint8_t>(ik >> (IK_BITS - 8)), hs);
            return CO16::find(node, hdr,
                static_cast<uint16_t>(ik >> (IK_BITS - 16)), hs);
        }

        if constexpr (KEY_BITS > 16) {
            if constexpr (KEY_BITS > 32) {
                if (st & 0b01)
                    return CO64::find(node, hdr,
                        static_cast<uint64_t>(ik), hs);
            }
            return CO32::find(node, hdr,
                static_cast<uint32_t>(ik >> (IK_BITS - 32)), hs);
        }
    }

    bool contains(const KEY& key) const noexcept {
        return find_value(key) != nullptr;
    }

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

private:
    // ==================================================================
    // Insert dispatch (shared by insert / insert_or_assign / assign)
    // ==================================================================

    template<bool INSERT, bool ASSIGN>
    std::pair<bool, bool> insert_dispatch_(const KEY& key, const VALUE& value) {
        IK ik = KO::to_internal(key);
        VST sv = VT::store(value, alloc_);

        // Empty trie: create single-entry leaf
        if (root_ == SENTINEL_TAGGED) {
            if constexpr (!INSERT) { VT::destroy(sv, alloc_); return {true, false}; }
            root_ = tag_leaf(make_single_leaf_(ik, sv, KEY_BITS));
            ++size_;
            return {true, true};
        }

        auto r = insert_node_<INSERT, ASSIGN>(root_, ik, sv, KEY_BITS);
        if (r.tagged_ptr != root_) root_ = r.tagged_ptr;
        if (r.inserted) { ++size_; return {true, true}; }
        VT::destroy(sv, alloc_);
        return {true, false};
    }

    // ==================================================================
    // insert_node (recursive, tagged)
    //
    // ptr: tagged pointer (LEAF_BIT for leaf, raw for bitmask)
    // ik: shifted so next byte is at (IK_BITS - 8)
    // bits: remaining KEY bits at this node's level
    // Returns: insert_result_t with tagged_ptr
    // ==================================================================

    template<bool INSERT, bool ASSIGN>
    insert_result_t insert_node_(uint64_t ptr, IK ik, VST value, int bits) {

        // --- SENTINEL ---
        if (ptr == SENTINEL_TAGGED) {
            if constexpr (!INSERT) return {ptr, false, false};
            return {tag_leaf(make_single_leaf_(ik, value, bits)), true, false};
        }

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
                    if (expected != actual[i]) {
                        if constexpr (!INSERT) return {ptr, false, false};
                        return {split_on_prefix_tagged_(node, hdr, ik, value,
                                                         actual, skip, i, bits), true, false};
                    }
                    ik = static_cast<IK>(ik << 8);
                    bits -= 8;
                }
            }

            // leaf_insert_ returns tagged result (bitmap/compact ops tag internally)
            auto result = leaf_insert_<INSERT, ASSIGN>(node, hdr, ik, value, bits);
            if (result.needs_split) {
                if constexpr (!INSERT) return {ptr, false, false};
                return {convert_to_bitmask_tagged_(node, hdr, ik, value, bits), true, false};
            }
            return result;
        }

        // --- BITMASK ---
        uint64_t* node = bm_to_node(ptr);
        auto* hdr = get_header(node);
        uint8_t sc = hdr->skip();

        if (sc > 0)
            return insert_skip_chain_<INSERT, ASSIGN>(node, hdr, sc, ik, value, bits);

        // Standalone bitmask (skip=0)
        uint8_t ti = static_cast<uint8_t>(ik >> (IK_BITS - 8));
        auto lk = BO::lookup(node, ti);

        if (!lk.found) {
            if constexpr (!INSERT) return {tag_bitmask(node), false, false};
            auto* leaf = make_single_leaf_(static_cast<IK>(ik << 8), value, bits - 8);
            auto* nn = BO::add_child(node, hdr, ti, tag_leaf(leaf), 1, alloc_);
            inc_descendants_(get_header(nn));
            return {tag_bitmask(nn), true, false};
        }

        // Recurse into child (lk.child is already tagged)
        auto cr = insert_node_<INSERT, ASSIGN>(
            lk.child, static_cast<IK>(ik << 8), value, bits - 8);
        if (cr.tagged_ptr != lk.child)
            BO::set_child(node, lk.slot, cr.tagged_ptr);
        if (cr.inserted) {
            inc_descendants_(hdr);
            uint16_t* da = BO::child_desc_array(node);
            if (da[lk.slot] < COALESCE_CAP) da[lk.slot]++;
        }
        return {tag_bitmask(node), cr.inserted, false};
    }

    // ==================================================================
    // leaf_insert: dispatch by suffix_type
    // Returns tagged result (bitmap/compact ops tag internally now)
    // ==================================================================

    template<bool INSERT, bool ASSIGN>
    insert_result_t leaf_insert_(uint64_t* node, node_header* hdr,
                                 IK ik, VST value, int /*bits*/) {
        uint8_t st = hdr->suffix_type();

        if (st == 0) {
            return BO::template bitmap_insert<INSERT, ASSIGN>(
                node, static_cast<uint8_t>(ik >> (IK_BITS - 8)), value, alloc_);
        }

        if constexpr (KEY_BITS > 16) {
            if (st & 0b10) {
                if constexpr (KEY_BITS > 32) {
                    if (st & 0b01)
                        return CO64::template insert<INSERT, ASSIGN>(
                            node, hdr, static_cast<uint64_t>(ik), value, alloc_);
                }
                return CO32::template insert<INSERT, ASSIGN>(
                    node, hdr, static_cast<uint32_t>(ik >> (IK_BITS - 32)),
                    value, alloc_);
            }
        }

        return CO16::template insert<INSERT, ASSIGN>(
            node, hdr, static_cast<uint16_t>(ik >> (IK_BITS - 16)),
            value, alloc_);
    }

    // ==================================================================
    // insert_skip_chain_: walk embedded bo<1> nodes matching key bytes
    //
    // If all skip bytes match: operate on the final bitmask.
    // If mismatch at position e: split the chain.
    // ==================================================================

    template<bool INSERT, bool ASSIGN>
    insert_result_t insert_skip_chain_(uint64_t* node, node_header* hdr,
                                        uint8_t sc, IK ik, VST value, int bits) {
        for (uint8_t e = 0; e < sc; ++e) {
            uint64_t* embed = node + 1 + e * 6;
            const bitmap256& bm = *reinterpret_cast<const bitmap256*>(embed);
            uint8_t actual_byte = bm.single_bit_index();
            uint8_t expected = static_cast<uint8_t>(ik >> (IK_BITS - 8));

            if (expected != actual_byte) {
                if constexpr (!INSERT) return {tag_bitmask(node), false, false};
                return {split_skip_at_(node, hdr, sc, e, ik, value, bits), true, false};
            }
            ik = static_cast<IK>(ik << 8);
            bits -= 8;
        }

        // All skip matched — operate on final bitmask
        size_t final_offset = 1 + static_cast<size_t>(sc) * 6;
        const bitmap256& fbm = *reinterpret_cast<const bitmap256*>(node + final_offset);
        uint8_t ti = static_cast<uint8_t>(ik >> (IK_BITS - 8));
        int slot = fbm.find_slot<slot_mode::FAST_EXIT>(ti);

        if (slot < 0) {
            if constexpr (!INSERT) return {tag_bitmask(node), false, false};
            auto* leaf = make_single_leaf_(static_cast<IK>(ik << 8), value, bits - 8);
            auto* nn = add_child_to_chain_(node, hdr, sc, ti, tag_leaf(leaf), 1);
            inc_descendants_(get_header(nn));
            return {tag_bitmask(nn), true, false};
        }

        // Found — recurse into child
        uint64_t* real_ch = node + final_offset + 5;
        uint64_t old_child = real_ch[slot];
        auto cr = insert_node_<INSERT, ASSIGN>(
            old_child, static_cast<IK>(ik << 8), value, bits - 8);
        if (cr.tagged_ptr != old_child)
            real_ch[slot] = cr.tagged_ptr;
        if (cr.inserted) {
            inc_descendants_(hdr);
            unsigned nc = hdr->entries();
            uint16_t* da = reinterpret_cast<uint16_t*>(real_ch + nc);
            if (da[slot] < COALESCE_CAP) da[slot]++;
        }
        return {tag_bitmask(node), cr.inserted, false};
    }

    // ==================================================================
    // add_child_to_chain_: add child to final bitmask of a skip chain
    //
    // In-place if allocation has room, realloc whole chain otherwise.
    // Returns (possibly new) untagged node pointer.
    // ==================================================================

    uint64_t* add_child_to_chain_(uint64_t* node, node_header* hdr,
                                    uint8_t sc, uint8_t idx,
                                    uint64_t child_tagged,
                                    uint16_t child_desc) {
        unsigned oc = hdr->entries();
        unsigned nc = oc + 1;
        size_t final_offset = 1 + static_cast<size_t>(sc) * 6;
        size_t needed = final_offset + 5 + nc + desc_u64(nc);

        if (needed <= hdr->alloc_u64()) {
            // In-place: insert into final bitmask
            bitmap256& bm = *reinterpret_cast<bitmap256*>(node + final_offset);
            uint64_t* children = node + final_offset + 5;
            int isl = bm.find_slot<slot_mode::UNFILTERED>(idx);

            // Save desc (children shift will overwrite it)
            uint16_t saved_desc[256];
            uint16_t* od = reinterpret_cast<uint16_t*>(children + oc);
            std::memcpy(saved_desc, od, oc * sizeof(uint16_t));

            // Insert child
            std::memmove(children + isl + 1, children + isl, (oc - isl) * sizeof(uint64_t));
            children[isl] = child_tagged;
            bm.set_bit(idx);
            hdr->set_entries(nc);

            // Write desc with insertion
            uint16_t* nd = reinterpret_cast<uint16_t*>(children + nc);
            std::memcpy(nd, saved_desc, isl * sizeof(uint16_t));
            nd[isl] = child_desc;
            std::memcpy(nd + isl + 1, saved_desc + isl, (oc - isl) * sizeof(uint16_t));
            return node;
        }

        // Realloc whole chain
        size_t au64 = round_up_u64(needed);
        uint64_t* nn = alloc_node(alloc_, au64);

        // Copy everything up to and including final sentinel
        size_t prefix_u64 = final_offset + 5;
        std::memcpy(nn, node, prefix_u64 * 8);

        auto* nh = get_header(nn);
        nh->set_entries(nc);
        nh->set_alloc_u64(au64);

        // Fix embed internal pointers (they point into old allocation)
        for (uint8_t e = 0; e < sc; ++e) {
            uint64_t* embed_child = nn + 1 + e * 6 + 5;
            uint64_t* next_bm = nn + 1 + (e + 1) * 6;
            *embed_child = reinterpret_cast<uint64_t>(next_bm);
        }

        // Sentinel
        nn[final_offset + 4] = SENTINEL_TAGGED;

        // Copy-insert children
        bitmap256& old_bm = *reinterpret_cast<bitmap256*>(node + final_offset);
        bitmap256& new_bm = *reinterpret_cast<bitmap256*>(nn + final_offset);
        int isl = old_bm.find_slot<slot_mode::UNFILTERED>(idx);
        new_bm.set_bit(idx);
        bitmap256::arr_copy_insert(
            node + final_offset + 5, nn + final_offset + 5,
            oc, isl, child_tagged);

        // Copy-insert desc
        const uint16_t* od = reinterpret_cast<const uint16_t*>(node + final_offset + 5 + oc);
        uint16_t* nd = reinterpret_cast<uint16_t*>(nn + final_offset + 5 + nc);
        std::memcpy(nd, od, isl * sizeof(uint16_t));
        nd[isl] = child_desc;
        std::memcpy(nd + isl + 1, od + isl, (oc - isl) * sizeof(uint16_t));

        dealloc_node(alloc_, node, hdr->alloc_u64());
        return nn;
    }

    // ==================================================================
    // split_skip_at_: key diverges at embed position e
    //
    // 1. Build new leaf for divergent key
    // 2. Build remainder: embeds [e+1..sc-1] + final bitmask
    // 3. Create 2-child bitmask at split point
    // 4. If e > 0: wrap in skip chain for bytes [0..e-1]
    // Returns: tagged bitmask pointer.
    // ==================================================================

    uint64_t split_skip_at_(uint64_t* node, node_header* hdr,
                              uint8_t sc, uint8_t split_pos,
                              IK ik, VST value, int bits) {
        uint8_t expected = static_cast<uint8_t>(ik >> (IK_BITS - 8));
        uint64_t* embed = node + 1 + split_pos * 6;
        uint8_t actual_byte = reinterpret_cast<const bitmap256*>(embed)->single_bit_index();

        // Build new leaf for divergent key
        uint64_t new_leaf_tagged = tag_leaf(
            make_single_leaf_(static_cast<IK>(ik << 8), value, bits - 8));

        // Build remainder from [split_pos+1..sc-1] + final bitmask
        uint64_t remainder = build_remainder_tagged_(node, hdr, sc, split_pos + 1);

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
        uint16_t ds[2] = {tagged_count_(cp[0]), tagged_count_(cp[1])};
        auto* split_node = BO::make_bitmask(bi, cp, 2, alloc_, ds);
        get_header(split_node)->set_descendants(sum_tagged_array_(cp, 2));

        // Wrap in skip chain for prefix bytes [0..split_pos-1]
        uint64_t result;
        if (split_pos > 0) {
            uint8_t prefix_bytes[6];
            for (uint8_t i = 0; i < split_pos; ++i) {
                uint64_t* eb = node + 1 + i * 6;
                prefix_bytes[i] = reinterpret_cast<const bitmap256*>(eb)->single_bit_index();
            }
            result = wrap_bitmask_chain_(split_node, prefix_bytes, split_pos);
        } else {
            result = tag_bitmask(split_node);
        }

        dealloc_node(alloc_, node, hdr->alloc_u64());
        return result;
    }

    // ==================================================================
    // build_remainder_tagged_: extract embeds [from..sc-1] + final bitmask
    //                          into a new allocation
    //
    // Returns: tagged pointer (bitmask or skip chain).
    // ==================================================================

    uint64_t build_remainder_tagged_(uint64_t* old_node, node_header* old_hdr,
                                       uint8_t old_sc, uint8_t from_pos) {
        uint8_t rem_skip = old_sc - from_pos;
        unsigned final_nc = old_hdr->entries();
        size_t old_final_offset = 1 + static_cast<size_t>(old_sc) * 6;

        // Extract final bitmask indices + children
        const bitmap256& fbm = *reinterpret_cast<const bitmap256*>(
            old_node + old_final_offset);
        const uint64_t* old_ch = old_node + old_final_offset + 5;

        uint8_t indices[256];
        uint64_t children[256];
        uint16_t descs[256];
        const uint16_t* old_desc = reinterpret_cast<const uint16_t*>(old_ch + final_nc);
        fbm.for_each_set([&](uint8_t idx, int slot) {
            indices[slot] = idx;
            children[slot] = old_ch[slot];
            descs[slot] = old_desc[slot];
        });

        if (rem_skip == 0) {
            // Just the final bitmask — create standalone
            auto* bm_node = BO::make_bitmask(indices, children, final_nc, alloc_, descs);
            get_header(bm_node)->set_descendants(
                sum_tagged_array_(children, final_nc));
            return tag_bitmask(bm_node);
        }

        // rem_skip > 0: extract skip bytes + final
        uint8_t skip_bytes[6];
        for (uint8_t i = 0; i < rem_skip; ++i) {
            uint64_t* eb = old_node + 1 + (from_pos + i) * 6;
            skip_bytes[i] = reinterpret_cast<const bitmap256*>(eb)->single_bit_index();
        }

        auto* chain = BO::make_skip_chain(
            skip_bytes, rem_skip, indices, children, final_nc, alloc_, descs);
        get_header(chain)->set_descendants(
            sum_tagged_array_(children, final_nc));
        return tag_bitmask(chain);
    }

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
                d = sum_children_desc_(node, 0);
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
            uint64_t sole_child = 0;
            uint8_t sole_idx = 0;
            BO::for_each_child(nn, [&](uint8_t idx, int, uint64_t tagged) {
                sole_child = tagged;
                sole_idx = idx;
            });
            uint16_t sole_ent = tagged_count_(sole_child);
            if (sole_child & LEAF_BIT) {
                uint64_t* leaf = untag_leaf_mut(sole_child);
                uint8_t byte_arr[1] = {sole_idx};
                leaf = prepend_skip_(leaf, 1, byte_arr);
                size_t nn_au64 = get_header(nn)->alloc_u64();
                dealloc_node(alloc_, nn, nn_au64);
                return {tag_leaf(leaf), true, sole_ent};
            }
            uint64_t* child_node = bm_to_node(sole_child);
            uint8_t byte_arr[1] = {sole_idx};
            size_t nn_au64 = get_header(nn)->alloc_u64();
            dealloc_node(alloc_, nn, nn_au64);
            return {wrap_bitmask_chain_(child_node, byte_arr, 1), true, sole_ent};
        }

        // Multi-child: decrement descendants, check coalesce
        uint16_t desc = dec_or_recompute_desc_(nn, 0);
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
            uint64_t* embed = node + 1 + e * 6;
            uint8_t actual = reinterpret_cast<const bitmap256*>(embed)->single_bit_index();
            uint8_t expected = static_cast<uint8_t>(ik >> (IK_BITS - 8));
            if (expected != actual) return {tag_bitmask(node), false, 0};
            ik = static_cast<IK>(ik << 8);
            bits -= 8;
        }

        // Final bitmask
        size_t final_offset = 1 + static_cast<size_t>(sc) * 6;
        const bitmap256& fbm = *reinterpret_cast<const bitmap256*>(node + final_offset);
        uint8_t ti = static_cast<uint8_t>(ik >> (IK_BITS - 8));
        int slot = fbm.find_slot<slot_mode::FAST_EXIT>(ti);
        if (slot < 0) return {tag_bitmask(node), false, 0};

        uint64_t* real_ch = node + final_offset + 5;
        uint64_t old_child = real_ch[slot];

        auto cr = erase_node_(old_child, static_cast<IK>(ik << 8), bits - 8);
        if (!cr.erased) return {tag_bitmask(node), false, 0};

        if (cr.tagged_ptr) {
            // Child survived
            if (cr.tagged_ptr != old_child) real_ch[slot] = cr.tagged_ptr;
            // Update desc for this slot
            unsigned nc_cur = hdr->entries();
            uint16_t* da = reinterpret_cast<uint16_t*>(real_ch + nc_cur);
            da[slot] = cr.subtree_entries;
            if (cr.subtree_entries == COALESCE_CAP)
                return {tag_bitmask(node), true, COALESCE_CAP};
            uint16_t d = hdr->descendants();
            if (d == COALESCE_CAP) {
                d = sum_children_desc_(node, sc);
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
        unsigned nc = hdr->entries() - 1;

        if (nc == 0) {
            dealloc_node(alloc_, node, hdr->alloc_u64());
            return {0, true, 0};
        }

        // Remove from final bitmask
        // Check if we should shrink the allocation
        size_t needed = final_offset + 5 + nc + desc_u64(nc);
        if (should_shrink_u64(hdr->alloc_u64(), needed)) {
            // Realloc: rebuild chain with fewer children
            size_t au64 = round_up_u64(needed);
            uint64_t* nn = alloc_node(alloc_, au64);

            // Copy header + embeds + final bitmap + sentinel
            size_t prefix_u64 = final_offset + 5;
            std::memcpy(nn, node, prefix_u64 * 8);

            auto* nh = get_header(nn);
            nh->set_entries(nc);
            nh->set_alloc_u64(au64);

            // Fix embed internal pointers
            for (uint8_t e = 0; e < sc; ++e) {
                uint64_t* embed_child = nn + 1 + e * 6 + 5;
                uint64_t* next_bm = nn + 1 + (e + 1) * 6;
                *embed_child = reinterpret_cast<uint64_t>(next_bm);
            }
            nn[final_offset + 4] = SENTINEL_TAGGED;

            // Copy children excluding erased slot
            reinterpret_cast<bitmap256*>(nn + final_offset)->clear_bit(ti);
            uint64_t* nch = nn + final_offset + 5;
            bitmap256::arr_copy_remove(real_ch, nch, nc + 1, slot);

            // Copy desc excluding erased slot
            const uint16_t* od = reinterpret_cast<const uint16_t*>(real_ch + nc + 1);
            uint16_t* nd = reinterpret_cast<uint16_t*>(nch + nc);
            std::memcpy(nd, od, slot * sizeof(uint16_t));
            std::memcpy(nd + slot, od + slot + 1, (nc - slot) * sizeof(uint16_t));

            dealloc_node(alloc_, node, hdr->alloc_u64());
            node = nn;
            hdr = nh;
            real_ch = nch;
        } else {
            // In-place removal — save desc first (children shift overwrites it)
            uint16_t saved_desc[256];
            const uint16_t* od = reinterpret_cast<const uint16_t*>(real_ch + nc + 1);
            std::memcpy(saved_desc, od, slot * sizeof(uint16_t));
            std::memcpy(saved_desc + slot, od + slot + 1, (nc - slot) * sizeof(uint16_t));

            bitmap256& bm = *reinterpret_cast<bitmap256*>(node + final_offset);
            bitmap256::arr_remove(bm, real_ch, nc + 1, slot, ti);
            hdr->set_entries(nc);

            uint16_t* nd = reinterpret_cast<uint16_t*>(real_ch + nc);
            std::memcpy(nd, saved_desc, nc * sizeof(uint16_t));
        }

        // Collapse when final drops to 1 child
        if (nc == 1) {
            const bitmap256& fbm_after = *reinterpret_cast<const bitmap256*>(node + final_offset);
            uint8_t sole_idx = fbm_after.first_set_bit();
            uint64_t sole_child = real_ch[0];

            // Collect all skip bytes + sole_idx
            uint8_t all_bytes[7];
            for (uint8_t i = 0; i < sc; ++i) {
                uint64_t* eb = node + 1 + i * 6;
                all_bytes[i] = reinterpret_cast<const bitmap256*>(eb)->single_bit_index();
            }
            all_bytes[sc] = sole_idx;
            uint8_t total_skip = sc + 1;

            size_t node_au64 = hdr->alloc_u64();
            uint16_t sole_ent = tagged_count_(sole_child);

            if (sole_child & LEAF_BIT) {
                uint64_t* leaf = untag_leaf_mut(sole_child);
                leaf = prepend_skip_(leaf, total_skip, all_bytes);
                dealloc_node(alloc_, node, node_au64);
                return {tag_leaf(leaf), true, sole_ent};
            }

            uint64_t* child_node = bm_to_node(sole_child);
            dealloc_node(alloc_, node, node_au64);
            return {wrap_bitmask_chain_(child_node, all_bytes, total_skip), true, sole_ent};
        }

        // Multi-child: decrement descendants, check coalesce
        uint16_t desc = dec_or_recompute_desc_(node, sc);
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

    static constexpr uint16_t COALESCE_CAP = static_cast<uint16_t>(COMPACT_MAX + 1);

    // ------------------------------------------------------------------
    // Descendant tracking helpers
    //
    // Bitmask nodes store total descendant count in total_slots_ field
    // (unused by bitmask). Capped at COALESCE_CAP (COMPACT_MAX + 1).
    // Leaf nodes: use entries() instead.
    // ------------------------------------------------------------------

    // Get descendant count from any tagged pointer (capped)
    static uint16_t tagged_count_(uint64_t tagged) noexcept {
        if (tagged & LEAF_BIT)
            return get_header(untag_leaf(tagged))->entries();
        return get_header(bm_to_node_const(tagged))->descendants();
    }

    // Sum immediate children's counts from local desc array. No pointer chasing.
    // Early exit when > COMPACT_MAX. Unchecked children assumed to have ≥ 1.
    static uint16_t sum_children_desc_(const uint64_t* node, uint8_t sc) noexcept {
        unsigned nc = get_header(node)->entries();
        if (nc > COMPACT_MAX) return COALESCE_CAP;
        size_t fo = 1 + static_cast<size_t>(sc) * 6;
        const uint16_t* desc = reinterpret_cast<const uint16_t*>(node + fo + 5 + nc);
        uint32_t total = 0;
        unsigned remaining = nc;
        for (unsigned i = 0; i < nc; ++i) {
            total += desc[i];
            --remaining;
            if (total + remaining > COMPACT_MAX) return COALESCE_CAP;
        }
        return static_cast<uint16_t>(total);
    }

    // Set descendants from known count (capped)
    static void set_desc_capped_(uint64_t* node, size_t count) noexcept {
        get_header(node)->set_descendants(
            count > COMPACT_MAX ? COALESCE_CAP : static_cast<uint16_t>(count));
    }

    // Capping increment (for insert path)
    static void inc_descendants_(node_header* h) noexcept {
        uint16_t d = h->descendants();
        if (d < COALESCE_CAP) h->set_descendants(d + 1);
    }

    // Decrement or recompute if capped. Returns new count (capped).
    // Node's children must reflect post-operation state before calling.
    static uint16_t dec_or_recompute_desc_(uint64_t* node, uint8_t sc) noexcept {
        auto* h = get_header(node);
        uint16_t d = h->descendants();
        if (d <= COMPACT_MAX) {
            --d;
            h->set_descendants(d);
            return d;
        }
        // Capped: recompute from immediate children (early exit > COMPACT_MAX)
        d = sum_children_desc_(node, sc);
        h->set_descendants(d);
        return d;
    }

    // Sum tagged children array (for build paths)
    static uint16_t sum_tagged_array_(const uint64_t* children, unsigned nc) noexcept {
        uint32_t total = 0;
        for (unsigned i = 0; i < nc; ++i) {
            total += tagged_count_(children[i]);
            if (total > COMPACT_MAX) return COALESCE_CAP;
        }
        return static_cast<uint16_t>(total);
    }

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
            leaf = prepend_skip_(leaf, sc, skip_bytes);
        }

        dealloc_bitmask_subtree_(tagged);
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
            const uint64_t* eb = node + 1 + i * 6;
            uint8_t byte = reinterpret_cast<const bitmap256*>(eb)->single_bit_index();
            cur_prefix |= uint64_t(byte) << (56 - cur_bits);
            cur_bits += 8;
        }

        size_t final_offset = 1 + static_cast<size_t>(sc) * 6;
        const bitmap256& fbm = *reinterpret_cast<const bitmap256*>(node + final_offset);
        const uint64_t* rch = node + final_offset + 5;
        fbm.for_each_set([&](uint8_t idx, int slot) {
            uint64_t child_prefix = cur_prefix | (uint64_t(idx) << (56 - cur_bits));
            collect_entries_tagged_(rch[slot], child_prefix, cur_bits + 8,
                                     keys, vals, wi);
        });
    }

    // Free all bitmask nodes in subtree without destroying leaf values
    // (values are being moved, not destroyed)
    void dealloc_bitmask_subtree_(uint64_t tagged) noexcept {
        if (tagged & LEAF_BIT) {
            // Just dealloc the leaf node, DON'T destroy values
            uint64_t* node = untag_leaf_mut(tagged);
            dealloc_node(alloc_, node, get_header(node)->alloc_u64());
            return;
        }
        uint64_t* node = bm_to_node(tagged);
        auto* hdr = get_header(node);
        uint8_t sc = hdr->skip();
        size_t final_offset = 1 + static_cast<size_t>(sc) * 6;
        const bitmap256& fbm = *reinterpret_cast<const bitmap256*>(node + final_offset);
        const uint64_t* rch = node + final_offset + 5;
        fbm.for_each_set([&](uint8_t, int slot) {
            dealloc_bitmask_subtree_(rch[slot]);
        });
        dealloc_node(alloc_, node, hdr->alloc_u64());
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
    // prepend_skip_: add or extend skip prefix on an existing leaf
    //
    // Receives/returns RAW (untagged) node pointer. Caller tags.
    // ==================================================================

    uint64_t* prepend_skip_(uint64_t* node, uint8_t new_len,
                             const uint8_t* new_bytes) {
        auto* h = get_header(node);
        uint8_t os = h->skip();
        uint8_t ns = os + new_len;

        uint8_t combined[6] = {};
        std::memcpy(combined, new_bytes, new_len);
        if (os > 0) std::memcpy(combined + new_len, h->prefix_bytes(), os);

        if (os > 0) {
            // Already has skip u64 -- update in place
            h->set_skip(ns);
            h->set_prefix(combined, ns);
            return node;
        }

        // No skip u64 -- realloc with extra u64 and shift data right
        size_t old_au64 = h->alloc_u64();
        size_t new_au64 = old_au64 + 1;
        uint64_t* nn = alloc_node(alloc_, new_au64);
        nn[0] = node[0];  // copy header
        std::memcpy(nn + 2, node + 1, (old_au64 - 1) * 8);  // shift data
        auto* nh = get_header(nn);
        nh->set_alloc_u64(new_au64);
        nh->set_skip(ns);
        nh->set_prefix(combined, ns);
        dealloc_node(alloc_, node, old_au64);
        return nn;
    }

    // ==================================================================
    // remove_skip_: strip the skip u64 from a leaf
    //
    // Receives/returns RAW (untagged) node pointer. Caller tags.
    // ==================================================================

    uint64_t* remove_skip_(uint64_t* node) {
        auto* h = get_header(node);
        size_t old_au64 = h->alloc_u64();
        size_t new_au64 = old_au64 - 1;
        uint64_t* nn = alloc_node(alloc_, new_au64);
        nn[0] = node[0];  // copy header
        get_header(nn)->set_skip(0);  // clear skip
        std::memcpy(nn + 1, node + 2, (old_au64 - 2) * 8);  // shift data left
        get_header(nn)->set_alloc_u64(new_au64);
        dealloc_node(alloc_, node, old_au64);
        return nn;
    }

    // ==================================================================
    // wrap_bitmask_chain_: wrap child bitmask in a skip chain
    //
    // child: RAW (untagged) bitmask node pointer (standalone or chain).
    // Returns: tagged bitmask pointer to the new chain.
    //
    // If child is already a skip chain, merges skip bytes.
    // Extracts child's final bitmap + children, creates a new combined
    // skip chain allocation, deallocates the old child.
    // ==================================================================

    uint64_t wrap_bitmask_chain_(uint64_t* child, const uint8_t* bytes, uint8_t count) {
        auto* ch = get_header(child);
        uint8_t child_sc = ch->skip();
        unsigned nc = ch->entries();

        // Collect all skip bytes: new prefix + child's existing skip
        uint8_t all_bytes[12];
        std::memcpy(all_bytes, bytes, count);
        for (uint8_t i = 0; i < child_sc; ++i) {
            uint64_t* eb = child + 1 + i * 6;
            all_bytes[count + i] = reinterpret_cast<const bitmap256*>(eb)->single_bit_index();
        }
        uint8_t total_skip = count + child_sc;

        // Extract final bitmask indices + children
        size_t final_offset = 1 + static_cast<size_t>(child_sc) * 6;
        const bitmap256& fbm = *reinterpret_cast<const bitmap256*>(child + final_offset);
        const uint64_t* cch = child + final_offset + 5;  // real children

        uint8_t indices[256];
        uint64_t children[256];
        uint16_t descs[256];
        const uint16_t* old_desc = reinterpret_cast<const uint16_t*>(cch + nc);
        fbm.for_each_set([&](uint8_t idx, int slot) {
            indices[slot] = idx;
            children[slot] = cch[slot];
            descs[slot] = old_desc[slot];
        });

        auto* chain = BO::make_skip_chain(all_bytes, total_skip, indices, children, nc, alloc_, descs);
        get_header(chain)->set_descendants(ch->descendants());
        dealloc_node(alloc_, child, ch->alloc_u64());
        return tag_bitmask(chain);
    }

    // ==================================================================
    // make_single_leaf: create 1-entry leaf at given bits
    //
    // Returns RAW (untagged) node pointer. Caller tags.
    // ==================================================================

    uint64_t* make_single_leaf_(IK ik, VST value, int bits) {
        uint8_t st = suffix_type_for(bits);
        if (st == 0) {
            uint8_t s = static_cast<uint8_t>(ik >> (IK_BITS - 8));
            return BO::make_single_bitmap(s, value, alloc_);
        }
        if (st == 1) {
            uint16_t s = static_cast<uint16_t>(ik >> (IK_BITS - 16));
            return CO16::make_leaf(&s, &value, 1, 0, nullptr, alloc_);
        }
        if constexpr (KEY_BITS > 16) {
            if (st == 2) {
                uint32_t s = static_cast<uint32_t>(ik >> (IK_BITS - 32));
                return CO32::make_leaf(&s, &value, 1, 0, nullptr, alloc_);
            }
        }
        if constexpr (KEY_BITS > 32) {
            uint64_t s = static_cast<uint64_t>(ik);
            return CO64::make_leaf(&s, &value, 1, 0, nullptr, alloc_);
        }
        __builtin_unreachable();
    }

    // ==================================================================
    // convert_to_bitmask_tagged_: compact leaf overflow -> new tree
    //
    // Returns tagged uint64_t.
    // ==================================================================

    uint64_t convert_to_bitmask_tagged_(uint64_t* node, node_header* hdr,
                                         IK ik, VST value, int bits) {
        uint16_t old_count = hdr->entries();
        size_t total = old_count + 1;
        auto wk = std::make_unique<uint64_t[]>(total);
        auto wv = std::make_unique<VST[]>(total);

        // Promote ik to bit-63-aligned uint64_t
        uint64_t new_suf = uint64_t(ik) << (64 - IK_BITS);
        size_t wi = 0;
        bool ins = false;
        leaf_for_each_u64_(node, hdr, [&](uint64_t s, VST v) {
            if (!ins && new_suf < s) {
                wk[wi] = new_suf; wv[wi] = value; wi++; ins = true;
            }
            wk[wi] = s; wv[wi] = v; wi++;
        });
        if (!ins) { wk[wi] = new_suf; wv[wi] = value; }

        uint64_t child_tagged = build_node_from_arrays_tagged_(
            wk.get(), wv.get(), total, bits);

        // Propagate old skip/prefix to new child
        uint8_t ps = hdr->skip();
        if (ps > 0) {
            const uint8_t* pfx = hdr->prefix_bytes();
            if (child_tagged & LEAF_BIT) {
                uint64_t* leaf = untag_leaf_mut(child_tagged);
                leaf = prepend_skip_(leaf, ps, pfx);
                child_tagged = tag_leaf(leaf);
            } else {
                // Wrap bitmask in chain
                uint64_t* bm_node = bm_to_node(child_tagged);
                child_tagged = wrap_bitmask_chain_(bm_node, pfx, ps);
            }
        }

        dealloc_node(alloc_, node, hdr->alloc_u64());
        return child_tagged;
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
    // build_node_from_arrays_tagged_
    //
    // suf[]: bit-63-aligned uint64_t, sorted ascending.
    // bits: remaining KEY bits.
    // Returns: tagged uint64_t.
    // ==================================================================

    uint64_t build_node_from_arrays_tagged_(uint64_t* suf, VST* vals,
                                             size_t count, int bits) {
        // Leaf case
        if (count <= COMPACT_MAX) {
            uint8_t st = suffix_type_for(bits);
            uint64_t* leaf;
            if (st == 0) {
                auto bk = std::make_unique<uint8_t[]>(count);
                for (size_t i = 0; i < count; ++i)
                    bk[i] = static_cast<uint8_t>(suf[i] >> 56);
                leaf = BO::make_bitmap_leaf(bk.get(), vals,
                    static_cast<uint32_t>(count), alloc_);
            } else if (st == 1) {
                auto tk = std::make_unique<uint16_t[]>(count);
                for (size_t i = 0; i < count; ++i)
                    tk[i] = static_cast<uint16_t>(suf[i] >> 48);
                leaf = CO16::make_leaf(tk.get(), vals,
                    static_cast<uint32_t>(count), 0, nullptr, alloc_);
            } else if constexpr (KEY_BITS > 16) {
                if (st == 2) {
                    auto tk = std::make_unique<uint32_t[]>(count);
                    for (size_t i = 0; i < count; ++i)
                        tk[i] = static_cast<uint32_t>(suf[i] >> 32);
                    leaf = CO32::make_leaf(tk.get(), vals,
                        static_cast<uint32_t>(count), 0, nullptr, alloc_);
                } else if constexpr (KEY_BITS > 32) {
                    // st == 3
                    leaf = CO64::make_leaf(suf, vals,
                        static_cast<uint32_t>(count), 0, nullptr, alloc_);
                } else {
                    __builtin_unreachable();
                }
            } else {
                __builtin_unreachable();
            }
            return tag_leaf(leaf);
        }

        // Skip compression: all entries share same top byte?
        if (bits > 8) {
            uint8_t first_top = static_cast<uint8_t>(suf[0] >> 56);
            bool all_same = true;
            for (size_t i = 1; i < count; ++i)
                if (static_cast<uint8_t>(suf[i] >> 56) != first_top)
                    { all_same = false; break; }

            if (all_same) {
                // Strip top byte: shift left by 8
                for (size_t i = 0; i < count; ++i) suf[i] <<= 8;

                uint64_t child_tagged = build_node_from_arrays_tagged_(
                    suf, vals, count, bits - 8);

                // Leaf gets skip prefix, bitmask gets chain wrapper
                uint8_t byte_arr[1] = {first_top};
                if (child_tagged & LEAF_BIT) {
                    uint64_t* leaf = untag_leaf_mut(child_tagged);
                    return tag_leaf(prepend_skip_(leaf, 1, byte_arr));
                } else {
                    uint64_t* bm_node = bm_to_node(child_tagged);
                    return wrap_bitmask_chain_(bm_node, byte_arr, 1);
                }
            }
        }

        return build_bitmask_from_arrays_tagged_(suf, vals, count, bits);
    }

    // ==================================================================
    // build_bitmask_from_arrays_tagged_
    //
    // Groups by top byte, recurses, creates bitmask node.
    // Returns: tagged bitmask uint64_t.
    // ==================================================================

    uint64_t build_bitmask_from_arrays_tagged_(uint64_t* suf, VST* vals,
                                                size_t count, int bits) {
        uint8_t  indices[256];
        uint64_t child_tagged[256];
        uint16_t descs[256];
        int      n_children = 0;

        size_t i = 0;
        while (i < count) {
            uint8_t ti = static_cast<uint8_t>(suf[i] >> 56);
            size_t start = i;
            while (i < count && static_cast<uint8_t>(suf[i] >> 56) == ti) ++i;
            size_t cc = i - start;

            // Strip top byte for children
            auto cs = std::make_unique<uint64_t[]>(cc);
            for (size_t j = 0; j < cc; ++j)
                cs[j] = suf[start + j] << 8;

            indices[n_children]      = ti;
            child_tagged[n_children] = build_node_from_arrays_tagged_(
                cs.get(), vals + start, cc, bits - 8);
            descs[n_children] = cc > COMPACT_MAX ? COALESCE_CAP
                                                  : static_cast<uint16_t>(cc);
            n_children++;
        }

        auto* node = BO::make_bitmask(indices, child_tagged, n_children, alloc_, descs);
        set_desc_capped_(node, count);
        return tag_bitmask(node);
    }

    // ==================================================================
    // split_on_prefix_tagged_
    //
    // Returns: tagged uint64_t.
    // ==================================================================

    uint64_t split_on_prefix_tagged_(uint64_t* node, node_header* hdr,
                                      IK ik, VST value,
                                      const uint8_t* actual, uint8_t skip,
                                      uint8_t common, int bits) {
        uint8_t new_idx = static_cast<uint8_t>(ik >> (IK_BITS - 8));
        uint8_t old_idx = actual[common];
        uint8_t old_rem = skip - 1 - common;

        // Save common prefix before any reallocation invalidates `actual`
        uint8_t saved_prefix[6] = {};
        if (common > 0)
            std::memcpy(saved_prefix, actual, common);

        // Update old node: strip consumed prefix, keep remainder
        if (old_rem > 0) {
            uint8_t rem[6] = {};
            std::memcpy(rem, actual + common + 1, old_rem);
            hdr->set_skip(old_rem);
            hdr->set_prefix(rem, old_rem);
        } else {
            node = remove_skip_(node);
            hdr = get_header(node);
        }

        // Advance ik and bits past divergence byte + remaining prefix
        IK leaf_ik = static_cast<IK>(ik << 8);
        int leaf_bits = bits - 8;
        uint8_t new_prefix[6] = {};
        for (uint8_t j = 0; j < old_rem; ++j) {
            new_prefix[j] = static_cast<uint8_t>(leaf_ik >> (IK_BITS - 8));
            leaf_ik = static_cast<IK>(leaf_ik << 8);
            leaf_bits -= 8;
        }

        // Build new leaf at same depth as old node
        uint64_t* new_leaf = make_single_leaf_(leaf_ik, value, leaf_bits);
        if (old_rem > 0)
            new_leaf = prepend_skip_(new_leaf, old_rem, new_prefix);

        // Create parent bitmask with 2 children (both leaves -> tagged)
        uint8_t   bi[2];
        uint64_t  cp[2];
        if (new_idx < old_idx) {
            bi[0] = new_idx; cp[0] = tag_leaf(new_leaf);
            bi[1] = old_idx; cp[1] = tag_leaf(node);
        } else {
            bi[0] = old_idx; cp[0] = tag_leaf(node);
            bi[1] = new_idx; cp[1] = tag_leaf(new_leaf);
        }

        uint16_t ds[2] = {tagged_count_(cp[0]), tagged_count_(cp[1])};
        auto* bm_node = BO::make_bitmask(bi, cp, 2, alloc_, ds);
        get_header(bm_node)->set_descendants(
            sum_tagged_array_(cp, 2));
        if (common > 0)
            return wrap_bitmask_chain_(bm_node, saved_prefix, common);
        return tag_bitmask(bm_node);
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
            size_t final_offset = 1 + static_cast<size_t>(sc) * 6;
            const bitmap256& fbm = *reinterpret_cast<const bitmap256*>(node + final_offset);
            const uint64_t* real_ch = node + final_offset + 5;
            fbm.for_each_set([&](uint8_t, int slot) {
                remove_node_(real_ch[slot]);
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
            size_t final_offset = 1 + static_cast<size_t>(sc) * 6;
            const bitmap256& fbm = *reinterpret_cast<const bitmap256*>(node + final_offset);
            const uint64_t* real_ch = node + final_offset + 5;
            fbm.for_each_set([&](uint8_t, int slot) {
                collect_stats_(real_ch[slot], s);
            });
        }
    }
};

} // namespace gteitelbaum

#endif // KNTRIE_IMPL_HPP
