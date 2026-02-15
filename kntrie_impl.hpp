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

    uint64_t* root_;
    size_t    size_;
    [[no_unique_address]] ALLOC alloc_;

public:
    // ==================================================================
    // Constructor / Destructor
    // ==================================================================

    kntrie_impl() : root_(SENTINEL_NODE), size_(0), alloc_() {}

    ~kntrie_impl() { remove_all_(); }

    kntrie_impl(const kntrie_impl&) = delete;
    kntrie_impl& operator=(const kntrie_impl&) = delete;

    [[nodiscard]] bool      empty() const noexcept { return size_ == 0; }
    [[nodiscard]] size_type size()  const noexcept { return size_; }

    void clear() noexcept {
        remove_all_();
        size_ = 0;
    }

    // ==================================================================
    // Find
    // ==================================================================

    const VALUE* find_value(const KEY& key) const noexcept {
        IK ik = KO::to_internal(key);

        const uint64_t* node = root_;
        node_header hdr = *get_header(node);

        while (!hdr.is_leaf()) [[likely]] {
            uint8_t ti = static_cast<uint8_t>(ik >> (IK_BITS - 8));
            ik <<= 8;
            node = BO::branchless_child(node, ti);
            hdr = *get_header(node);
        }

        // Leaf skip check (leaves still have skip)
        size_t hs = 1;
        if (hdr.is_skip()) [[unlikely]] {
            hs = 2;
            const uint8_t* actual = reinterpret_cast<const uint8_t*>(&node[1]);
            uint8_t skip = actual[7];
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

        if (root_ == SENTINEL_NODE) return false;

        auto [new_node, erased] = erase_node_(root_, ik, KEY_BITS);
        if (!erased) return false;

        root_ = new_node ? new_node : SENTINEL_NODE;
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
        s.total_bytes = sizeof(uint64_t*);
        if (root_ != SENTINEL_NODE)
            collect_stats_(root_, s);
        return s;
    }

    size_t memory_usage() const noexcept { return debug_stats().total_bytes; }

    struct root_info_t {
        uint16_t entries; uint8_t skip;
        bool is_leaf;
    };

    root_info_t debug_root_info() const {
        if (root_ == SENTINEL_NODE) return {0, 0, false};
        auto* hdr = get_header(root_);
        return {hdr->entries(), hdr->skip(), hdr->is_leaf()};
    }

    const uint64_t* debug_root() const noexcept { return root_; }

private:
    // ==================================================================
    // Insert dispatch (shared by insert / insert_or_assign / assign)
    // ==================================================================

    template<bool INSERT, bool ASSIGN>
    std::pair<bool, bool> insert_dispatch_(const KEY& key, const VALUE& value) {
        IK ik = KO::to_internal(key);
        VST sv = VT::store(value, alloc_);

        // Empty trie: create single-entry leaf
        if (root_ == SENTINEL_NODE) {
            if constexpr (!INSERT) { VT::destroy(sv, alloc_); return {true, false}; }
            root_ = make_single_leaf_(ik, sv, KEY_BITS);
            ++size_;
            return {true, true};
        }

        auto r = insert_node_<INSERT, ASSIGN>(root_, ik, sv, KEY_BITS);
        if (r.node != root_) root_ = r.node;
        if (r.inserted) { ++size_; return {true, true}; }
        VT::destroy(sv, alloc_);
        return {true, false};
    }

    // ==================================================================
    // insert_node (recursive)
    //
    // ik: shifted so next byte is at (IK_BITS - 8)
    // bits: remaining KEY bits at this node's level
    // ==================================================================

    template<bool INSERT, bool ASSIGN>
    insert_result_t insert_node_(uint64_t* node, IK ik, VST value, int bits) {
        auto* hdr = get_header(node);

        if (hdr->is_leaf()) {
            // Leaf skip check
            uint8_t skip = hdr->skip();
            if (skip) [[unlikely]] {
                const uint8_t* actual = hdr->prefix_bytes();
                for (uint8_t i = 0; i < skip; ++i) {
                    uint8_t expected = static_cast<uint8_t>(ik >> (IK_BITS - 8));
                    if (expected != actual[i]) {
                        if constexpr (!INSERT) return {node, false, false};
                        return {split_on_prefix_(node, hdr, ik, value,
                                                  actual, skip, i, bits), true, false};
                    }
                    ik = static_cast<IK>(ik << 8);
                    bits -= 8;
                }
            }
            auto result = leaf_insert_<INSERT, ASSIGN>(node, hdr, ik, value, bits);
            if (result.needs_split) {
                if constexpr (!INSERT) return {node, false, false};
                return {convert_to_bitmask_(node, hdr, ik, value, bits), true, false};
            }
            return result;
        }

        // Bitmask node: no skip, just descend
        uint8_t ti = static_cast<uint8_t>(ik >> (IK_BITS - 8));
        auto lk = BO::lookup(node, ti);

        if (!lk.found) {
            if constexpr (!INSERT) return {node, false, false};
            auto* leaf = make_single_leaf_(static_cast<IK>(ik << 8), value, bits - 8);
            auto* nn = BO::add_child(node, hdr, ti, leaf, alloc_);
            return {nn, true, false};
        }

        // Recurse into child
        auto cr = insert_node_<INSERT, ASSIGN>(
            lk.child, static_cast<IK>(ik << 8), value, bits - 8);
        if (cr.node != lk.child)
            BO::set_child(node, lk.slot, cr.node);
        return {node, cr.inserted, false};
    }

    // ==================================================================
    // leaf_insert: dispatch by suffix_type
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
    // erase_node (recursive)
    // ==================================================================

    erase_result_t erase_node_(uint64_t* node, IK ik, int bits) {
        auto* hdr = get_header(node);

        if (hdr->is_leaf()) {
            // Leaf skip check
            uint8_t skip = hdr->skip();
            if (skip) [[unlikely]] {
                const uint8_t* actual = hdr->prefix_bytes();
                for (uint8_t i = 0; i < skip; ++i) {
                    uint8_t expected = static_cast<uint8_t>(ik >> (IK_BITS - 8));
                    if (expected != actual[i]) return {node, false};
                    ik = static_cast<IK>(ik << 8);
                    bits -= 8;
                }
            }
            return leaf_erase_(node, hdr, ik);
        }

        // Bitmask node: no skip, just descend
        uint8_t ti = static_cast<uint8_t>(ik >> (IK_BITS - 8));
        auto lk = BO::lookup(node, ti);
        if (!lk.found) return {node, false};

        // Recurse into child
        auto [new_child, erased] = erase_node_(
            lk.child, static_cast<IK>(ik << 8), bits - 8);
        if (!erased) return {node, false};

        if (new_child) {
            if (new_child != lk.child)
                BO::set_child(node, lk.slot, new_child);
            return {node, true};
        }

        // Child fully erased — remove from bitmask
        auto* nn = BO::remove_child(node, hdr, lk.slot, ti, alloc_);

        // Collapse: single-child bitmask whose child is a leaf → absorb byte
        if (nn && get_header(nn)->entries() == 1) {
            uint64_t* sole_child = nullptr;
            uint8_t sole_idx = 0;
            BO::for_each_child(nn, [&](uint8_t idx, int, uint64_t* child) {
                sole_child = child;
                sole_idx = idx;
            });
            if (get_header(sole_child)->is_leaf()) {
                uint8_t byte_arr[1] = {sole_idx};
                sole_child = prepend_skip_(sole_child, 1, byte_arr);
                size_t nn_au64 = get_header(nn)->alloc_u64();
                dealloc_node(alloc_, nn, nn_au64);
                nn = sole_child;
            }
        }
        return {nn, true};
    }

    // ==================================================================
    // leaf_erase: dispatch by suffix_type
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
    // prepend_skip_: add or extend skip prefix on an existing node
    //
    // If node has no skip: realloc with +1 u64, shift data right.
    // If node already has skip: update prefix bytes in place.
    // Returns (possibly new) node pointer.
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
    // remove_skip_: strip the skip u64 from a node that no longer needs it
    //
    // Realloc with -1 u64, shift data left, clear SKIP_BIT.
    // Returns new node pointer.
    // ==================================================================

    uint64_t* remove_skip_(uint64_t* node) {
        auto* h = get_header(node);
        size_t old_au64 = h->alloc_u64();
        size_t new_au64 = old_au64 - 1;
        uint64_t* nn = alloc_node(alloc_, new_au64);
        nn[0] = node[0];  // copy header
        get_header(nn)->set_skip(0);  // clear skip bit
        std::memcpy(nn + 1, node + 2, (old_au64 - 2) * 8);  // shift data left
        get_header(nn)->set_alloc_u64(new_au64);
        dealloc_node(alloc_, node, old_au64);
        return nn;
    }

    // ==================================================================
    // wrap_bitmask_chain_: wrap child in single-child bitmask nodes
    //
    // Each prefix byte becomes a bitmask node with one child.
    // Built inside-out so bytes[0] is outermost.
    // ==================================================================

    uint64_t* wrap_bitmask_chain_(uint64_t* child, const uint8_t* bytes, uint8_t count) {
        for (int i = count - 1; i >= 0; --i) {
            uint8_t idx = bytes[i];
            child = BO::make_bitmask(&idx, &child, 1, alloc_);
        }
        return child;
    }

    // ==================================================================
    // make_single_leaf: create 1-entry leaf at given bits
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
    // convert_to_bitmask: compact leaf overflow → bitmask node
    //
    // Collects all entries + new entry into bit-63-aligned working arrays,
    // builds node tree via build_node_from_arrays.
    // ==================================================================

    uint64_t* convert_to_bitmask_(uint64_t* node, node_header* hdr,
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

        auto* child = build_node_from_arrays_(wk.get(), wv.get(), total, bits);

        // Propagate old skip/prefix to new child
        uint8_t ps = hdr->skip();
        if (ps > 0) {
            if (get_header(child)->is_leaf())
                child = prepend_skip_(child, ps, hdr->prefix_bytes());
            else
                child = wrap_bitmask_chain_(child, hdr->prefix_bytes(), ps);
        }

        dealloc_node(alloc_, node, hdr->alloc_u64());
        return child;
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
    // build_node_from_arrays
    //
    // suf[]: bit-63-aligned uint64_t, sorted ascending.
    // bits: remaining KEY bits.
    // ==================================================================

    uint64_t* build_node_from_arrays_(uint64_t* suf, VST* vals,
                                       size_t count, int bits) {
        // Leaf case
        if (count <= COMPACT_MAX) {
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
                // st == 3
                return CO64::make_leaf(suf, vals,
                    static_cast<uint32_t>(count), 0, nullptr, alloc_);
            }
            __builtin_unreachable();
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

                uint64_t* child = build_node_from_arrays_(suf, vals, count, bits - 8);

                // Leaf gets skip prefix, bitmask gets chain wrapper
                uint8_t byte_arr[1] = {first_top};
                if (get_header(child)->is_leaf())
                    return prepend_skip_(child, 1, byte_arr);
                else
                    return wrap_bitmask_chain_(child, byte_arr, 1);
            }
        }

        return build_bitmask_from_arrays_(suf, vals, count, bits);
    }

    // ==================================================================
    // build_bitmask_from_arrays
    //
    // Groups by top byte, recurses, creates bitmask node.
    // ==================================================================

    uint64_t* build_bitmask_from_arrays_(uint64_t* suf, VST* vals,
                                          size_t count, int bits) {
        uint8_t   indices[256];
        uint64_t* child_ptrs[256];
        int       n_children = 0;

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

            indices[n_children]    = ti;
            child_ptrs[n_children] = build_node_from_arrays_(
                cs.get(), vals + start, cc, bits - 8);
            n_children++;
        }

        return BO::make_bitmask(indices, child_ptrs, n_children, alloc_);
    }

    // ==================================================================
    // split_on_prefix
    //
    // At entry: ik has been shifted past `common` matching prefix bytes.
    // ik >> (IK_BITS-8) = diverging expected byte.
    // actual[common] = diverging actual byte.
    // bits = original_bits_at_node - 8*common.
    // ==================================================================

    uint64_t* split_on_prefix_(uint64_t* node, node_header* hdr,
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

        // Create parent bitmask with 2 children
        uint8_t   bi[2];
        uint64_t* cp[2];
        if (new_idx < old_idx) {
            bi[0] = new_idx; cp[0] = new_leaf;
            bi[1] = old_idx; cp[1] = node;
        } else {
            bi[0] = old_idx; cp[0] = node;
            bi[1] = new_idx; cp[1] = new_leaf;
        }

        auto* bm_node = BO::make_bitmask(bi, cp, 2, alloc_);
        if (common > 0)
            bm_node = wrap_bitmask_chain_(bm_node, saved_prefix, common);
        return bm_node;
    }

    // ==================================================================
    // Remove all
    // ==================================================================

    void remove_all_() noexcept {
        if (root_ != SENTINEL_NODE) {
            remove_node_(root_);
            root_ = SENTINEL_NODE;
        }
        size_ = 0;
    }

    void remove_node_(uint64_t* node) noexcept {
        auto* hdr = get_header(node);
        if (hdr->is_leaf()) {
            destroy_leaf_(node, hdr);
        } else {
            BO::for_each_child(node, [&](uint8_t, int, uint64_t* child) {
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
    // Stats collection
    // ==================================================================

    void collect_stats_(const uint64_t* node, debug_stats_t& s) const noexcept {
        auto* hdr = get_header(node);
        s.total_bytes += static_cast<size_t>(hdr->alloc_u64()) * 8;

        if (hdr->is_leaf()) {
            s.total_entries += hdr->entries();
            if (hdr->suffix_type() == 0)
                s.bitmap_leaves++;
            else
                s.compact_leaves++;
        } else {
            s.bitmask_nodes++;
            BO::for_each_child(node, [&](uint8_t, int, uint64_t* child) {
                collect_stats_(child, s);
            });
        }
    }
};

} // namespace gteitelbaum

#endif // KNTRIE_IMPL_HPP
