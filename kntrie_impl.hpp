#ifndef KNTRIE_IMPL_HPP
#define KNTRIE_IMPL_HPP

#include "kntrie_compact.hpp"
#include "kntrie_bitmask.hpp"
#include <memory>

namespace gteitelbaum {

template<typename KEY, typename VALUE, typename ALLOC = std::allocator<uint64_t>>
class kntrie_impl {
    static_assert(std::is_integral_v<KEY>);

public:
    using key_type       = KEY;
    using mapped_type    = VALUE;
    using size_type      = std::size_t;
    using allocator_type = ALLOC;

private:
    using KO  = key_ops<KEY>;
    using IK  = typename KO::internal_key_t;
    using VT  = value_traits<VALUE, ALLOC>;
    using VST = typename VT::slot_type;
    using CO  = compact_ops<KEY, VALUE, ALLOC>;
    using SO  = split_ops<KEY, VALUE, ALLOC>;
    using FO  = fan_ops<KEY, VALUE, ALLOC>;
    using BL  = bitmap_leaf_ops<KEY, VALUE, ALLOC>;

    static constexpr int KEY_BITS = static_cast<int>(KO::KEY_BITS);
    static constexpr int IK_BITS  = static_cast<int>(KO::IK_BITS);

    // Descent stack for insert/erase
    enum class parent_type : uint8_t { ROOT, SPLIT, FAN };

    struct descent_entry_t {
        uint64_t*   node;
        parent_type type;
        uint8_t     index;
        int16_t     slot;
    };

    static constexpr int MAX_DEPTH = 10;

    uint64_t* root_[256];
    size_t    size_;
    [[no_unique_address]] ALLOC alloc_;

public:
    // ==================================================================
    // Constructor / Destructor
    // ==================================================================

    kntrie_impl() : size_(0), alloc_() {
        for (int i = 0; i < 256; ++i) root_[i] = SENTINEL_NODE;
    }

    ~kntrie_impl() { remove_all(); }

    kntrie_impl(const kntrie_impl&) = delete;
    kntrie_impl& operator=(const kntrie_impl&) = delete;

    [[nodiscard]] bool      empty() const noexcept { return size_ == 0; }
    [[nodiscard]] size_type size()  const noexcept { return size_; }

    void clear() noexcept {
        remove_all();
        size_ = 0;
    }

    // ==================================================================
    // Find
    // ==================================================================

    const VALUE* find_value(const KEY& key) const noexcept {
        IK ik = KO::to_internal(key);
        uint8_t ri = static_cast<uint8_t>(ik >> (IK_BITS - 8));
        ik <<= 8;

        const uint64_t* node = root_[ri];
        node_header hdr = *get_header(node);

        // Root: compact leaf
        if (hdr.is_leaf()) [[unlikely]]
            return compact_find(node, hdr, ik);

        // Root is fan: descend one level
        node = FO::branchless_child(node, static_cast<uint8_t>(ik >> (IK_BITS - 8)));
        ik <<= 8;
        hdr = *get_header(node);

        // Main descent loop
        bool child_leaf = false;
        while (true) {
            // Handle skip/prefix
            if (hdr.skip()) [[unlikely]] {
                prefix_t actual = hdr.prefix();
                int skip = hdr.skip();
                for (int i = 0; i < skip; ++i) {
                    uint16_t expected = static_cast<uint16_t>(ik >> (IK_BITS - 16));
                    if (expected != actual[i]) [[unlikely]] return nullptr;
                    ik <<= 16;
                }
            }

            // Compact leaf: exit
            if (hdr.is_leaf()) [[unlikely]] break;

            // Split node: top child + leaf check
            uint8_t ti = static_cast<uint8_t>(ik >> (IK_BITS - 8));
            ik <<= 8;
            auto [child, is_leaf] = SO::branchless_top_child(node, ti);
            node = child;
            hdr = *get_header(node);

            if (is_leaf) [[unlikely]] {
                child_leaf = true;
                break;
            }

            // Child is fan: consume 8 more bits
            node = FO::branchless_child(node, static_cast<uint8_t>(ik >> (IK_BITS - 8)));
            ik <<= 8;
            hdr = *get_header(node);
        }

        // Split's leaf children are always bitmap256
        if (child_leaf)
            return BL::find(node, static_cast<uint8_t>(ik >> (IK_BITS - 8)));

        // Exited via is_leaf() at top of loop: always compact
        return compact_find(node, hdr, ik);
    }

    bool contains(const KEY& key) const noexcept {
        return find_value(key) != nullptr;
    }

    // ==================================================================
    // Insert (insert-only: does NOT overwrite existing values)
    // ==================================================================

    std::pair<bool, bool> insert(const KEY& key, const VALUE& value) {
        return insert_impl_<true, false>(key, value);
    }

    // ==================================================================
    // Insert-or-assign (overwrites existing values)
    // ==================================================================

    std::pair<bool, bool> insert_or_assign(const KEY& key, const VALUE& value) {
        return insert_impl_<true, true>(key, value);
    }

    // ==================================================================
    // Assign (overwrite only, no insert if missing)
    // ==================================================================

    std::pair<bool, bool> assign(const KEY& key, const VALUE& value) {
        return insert_impl_<false, true>(key, value);
    }

    // ==================================================================
    // Erase
    // ==================================================================

    bool erase(const KEY& key) {
        IK ik = KO::to_internal(key);
        uint8_t ri = static_cast<uint8_t>(ik >> (IK_BITS - 8));
        ik <<= 8;
        int bits = KEY_BITS - 8;

        uint64_t* node = root_[ri];
        if (node == SENTINEL_NODE) return false;

        descent_entry_t stack[MAX_DEPTH];
        int depth = 0;
        node_header* hdr = get_header(node);

        // Root compact leaf
        if (hdr->is_leaf()) {
            auto r = compact_erase(node, hdr, ik);
            if (!r.erased) return false;
            root_[ri] = r.node ? r.node : SENTINEL_NODE;
            --size_;
            return true;
        }

        // Root is fan: descend
        uint8_t bi = static_cast<uint8_t>(ik >> (IK_BITS - 8));
        auto blk = FO::lookup_child(node, bi);
        if (!blk.found) return false;
        stack[depth++] = {node, parent_type::FAN, bi, static_cast<int16_t>(blk.slot)};
        ik <<= 8; bits -= 8;
        node = blk.child;
        hdr = get_header(node);

        // Main descent loop
        while (true) {
            int skip = hdr->skip();
            if (skip > 0) {
                prefix_t actual = hdr->prefix();
                for (int i = 0; i < skip; ++i) {
                    uint16_t expected = static_cast<uint16_t>(ik >> (IK_BITS - 16));
                    if (expected != actual[i]) return false;
                    ik <<= 16; bits -= 16;
                }
            }

            if (hdr->is_leaf()) {
                auto r = compact_erase(node, hdr, ik);
                if (!r.erased) return false;
                if (r.node) {
                    if (r.node != node) propagate(stack, depth, r.node, node);
                } else {
                    remove_from_parent(stack, depth);
                }
                --size_;
                return true;
            }

            // Split node
            uint8_t ti = static_cast<uint8_t>(ik >> (IK_BITS - 8));
            auto lk = SO::lookup_child(node, ti);
            if (!lk.found) return false;

            bool child_is_leaf = !SO::is_internal(node, ti);

            if (child_is_leaf) {
                ik <<= 8; bits -= 8;
                auto r = bot_leaf_erase(lk.child, ik);
                if (!r.erased) return false;
                if (r.node) {
                    if (r.node != lk.child)
                        SO::set_child(node, lk.slot, r.node);
                } else {
                    auto* nn = SO::remove_child(node, lk.slot, ti, alloc_);
                    if (!nn) {
                        remove_from_parent(stack, depth);
                    } else if (nn != node) {
                        propagate(stack, depth, nn, node);
                    }
                }
                --size_;
                return true;
            }

            // Fan child: descend
            ik <<= 8; bits -= 8;
            stack[depth++] = {node, parent_type::SPLIT, ti, static_cast<int16_t>(lk.slot)};
            uint64_t* fan = lk.child;

            bi = static_cast<uint8_t>(ik >> (IK_BITS - 8));
            auto blk2 = FO::lookup_child(fan, bi);
            if (!blk2.found) return false;

            stack[depth++] = {fan, parent_type::FAN, bi, static_cast<int16_t>(blk2.slot)};
            ik <<= 8; bits -= 8;
            node = blk2.child;
            hdr = get_header(node);
        }
    }

    // ==================================================================
    // Stats / Memory
    // ==================================================================

    struct debug_stats_t {
        size_t compact_leaves = 0;
        size_t bitmap_leaves = 0;
        size_t split_nodes = 0;
        size_t fan_nodes = 0;
        size_t total_entries = 0;
        size_t total_bytes = 0;
    };

    debug_stats_t debug_stats() const noexcept {
        debug_stats_t s{};
        for (int i = 0; i < 256; ++i) {
            const uint64_t* child = root_[i];
            if (child == SENTINEL_NODE) continue;
            auto* h = get_header(child);
            if (h->is_leaf()) {
                stats_compact(child, s);
            } else {
                stats_fan(child, s);
            }
        }
        return s;
    }

    size_t memory_usage() const noexcept { return debug_stats().total_bytes; }

    struct RootInfo {
        uint16_t entries; uint8_t skip;
        bool is_leaf; prefix_t prefix;
    };
    RootInfo debug_root_info() const {
        int occupied = 0;
        for (int i = 0; i < 256; ++i)
            if (root_[i] != SENTINEL_NODE) ++occupied;
        return {static_cast<uint16_t>(occupied), 0, false, prefix_t{0,0}};
    }

private:

    // ==================================================================
    // Safe suffix extraction (avoids negative shift when K wider than IK)
    // ==================================================================

    template<typename K>
    static K extract_suffix(IK ik) noexcept {
        constexpr int K_BITS = static_cast<int>(sizeof(K)) * 8;
        if constexpr (K_BITS >= IK_BITS) return static_cast<K>(ik);
        else return static_cast<K>(ik >> (IK_BITS - K_BITS));
    }

    // ==================================================================
    // compact_find dispatch (nested bit tests, u16 fallthrough)
    // ==================================================================

    const VALUE* compact_find(const uint64_t* node, node_header hdr,
                              IK ik) const noexcept {
        uint8_t st = hdr.suffix_type();
        if (st & 0b10) {
            if (st & 0b01)
                return CO::template find<uint64_t>(node, hdr, static_cast<uint64_t>(ik));
            else
                return CO::template find<uint32_t>(node, hdr,
                    static_cast<uint32_t>(ik >> (IK_BITS - 32)));
        }
        return CO::template find<uint16_t>(node, hdr,
            static_cast<uint16_t>(ik >> (IK_BITS - 16)));
    }

    // ==================================================================
    // compact_insert dispatch
    // ==================================================================

    using CIR = typename CO::compact_insert_result_t;

    template<bool INSERT, bool ASSIGN>
    CIR compact_insert(uint64_t* node, node_header* hdr,
                       IK ik, VST value) {
        uint8_t st = hdr->suffix_type();
        if (st & 0b10) {
            if (st & 0b01)
                return CO::template insert<uint64_t, INSERT, ASSIGN>(
                    node, hdr, static_cast<uint64_t>(ik), value, alloc_);
            else
                return CO::template insert<uint32_t, INSERT, ASSIGN>(
                    node, hdr, static_cast<uint32_t>(ik >> (IK_BITS - 32)), value, alloc_);
        }
        return CO::template insert<uint16_t, INSERT, ASSIGN>(
            node, hdr, static_cast<uint16_t>(ik >> (IK_BITS - 16)), value, alloc_);
    }

    // ==================================================================
    // compact_erase dispatch
    // ==================================================================

    erase_result_t compact_erase(uint64_t* node, node_header* hdr, IK ik) {
        uint8_t st = hdr->suffix_type();
        if (st & 0b10) {
            if (st & 0b01)
                return CO::template erase<uint64_t>(node, hdr,
                    static_cast<uint64_t>(ik), alloc_);
            else
                return CO::template erase<uint32_t>(node, hdr,
                    static_cast<uint32_t>(ik >> (IK_BITS - 32)), alloc_);
        }
        return CO::template erase<uint16_t>(node, hdr,
            static_cast<uint16_t>(ik >> (IK_BITS - 16)), alloc_);
    }

    // ==================================================================
    // destroy_compact dispatch
    // ==================================================================

    void destroy_compact(uint64_t* node) noexcept {
        auto* h = get_header(node);
        uint8_t st = h->suffix_type();
        if (st & 0b10) {
            if (st & 0b01) CO::template destroy_and_dealloc<uint64_t>(node, alloc_);
            else           CO::template destroy_and_dealloc<uint32_t>(node, alloc_);
        } else {
            CO::template destroy_and_dealloc<uint16_t>(node, alloc_);
        }
    }

    // ==================================================================
    // bot_leaf_insert dispatch (bitmap256 or compact under split)
    // ==================================================================

    template<bool INSERT, bool ASSIGN>
    CIR bot_leaf_insert(uint64_t* bot, IK ik, VST value) {
        auto* bh = get_header(bot);
        if (!bh->is_leaf()) {
            // Bitmap256 leaf — never overflows
            auto r = BL::template insert<INSERT, ASSIGN>(
                bot, static_cast<uint8_t>(ik >> (IK_BITS - 8)), value, alloc_);
            return {r.node, r.inserted, false};
        }
        // Compact leaf — can trigger needs_split at COMPACT_MAX
        return compact_insert<INSERT, ASSIGN>(bot, bh, ik, value);
    }

    // ==================================================================
    // bot_leaf_erase dispatch
    // ==================================================================

    erase_result_t bot_leaf_erase(uint64_t* bot, IK ik) {
        auto* bh = get_header(bot);
        if (!bh->is_leaf()) {
            return BL::erase(bot, static_cast<uint8_t>(ik >> (IK_BITS - 8)), alloc_);
        }
        return compact_erase(bot, bh, ik);
    }

    // ==================================================================
    // make_single_leaf: compact leaf with 1 entry
    // ==================================================================

    uint64_t* make_single_leaf(IK ik, VST value, int bits) {
        uint8_t stype = suffix_type_for(bits);
        return dispatch_suffix(stype, [&](auto k_tag) -> uint64_t* {
            using K = decltype(k_tag);
            K suffix = extract_suffix<K>(ik);
            return CO::template make_leaf<K>(&suffix, &value, 1, 0, prefix_t{0,0}, stype, alloc_);
        });
    }

    // ==================================================================
    // make_single_bot_leaf: bitmap256 for bits<=8, else compact
    // ==================================================================

    uint64_t* make_single_bot_leaf(IK ik, VST value, int bits) {
        if (bits <= 8) {
            return BL::make_single(
                static_cast<uint8_t>(ik >> (IK_BITS - 8)), value, alloc_);
        }
        return make_single_leaf(ik, value, bits);
    }

    // ==================================================================
    // Propagate pointer change through descent stack
    // ==================================================================

    void propagate(descent_entry_t* stack, int depth,
                   uint64_t* new_node, uint64_t* old_node) {
        if (new_node == old_node) return;
        if (depth == 0) {
            // Shouldn't happen — root changes are handled directly
            return;
        }
        auto& parent = stack[depth - 1];
        switch (parent.type) {
            case parent_type::ROOT:
                root_[parent.index] = new_node; break;
            case parent_type::SPLIT:
                SO::set_child(parent.node, parent.slot, new_node); break;
            case parent_type::FAN:
                FO::set_child(parent.node, parent.slot, new_node); break;
        }
    }

    // ==================================================================
    // Remove from parent: cascade upward when child fully erased
    // ==================================================================

    void remove_from_parent(descent_entry_t* stack, int depth) {
        while (depth > 0) {
            auto& entry = stack[depth - 1];
            uint64_t* parent = entry.node;
            int cc;
            uint64_t* nn;

            if (entry.type == parent_type::SPLIT) {
                cc = SO::child_count(parent);
                if (cc > 1) {
                    nn = SO::remove_child(parent, entry.slot, entry.index, alloc_);
                    if (nn != parent) propagate(stack, depth - 1, nn, parent);
                    return;
                }
                SO::dealloc(parent, alloc_);
            } else if (entry.type == parent_type::FAN) {
                cc = FO::child_count(parent);
                if (cc > 1) {
                    nn = FO::remove_child(parent, entry.slot, entry.index, alloc_);
                    if (nn != parent) propagate(stack, depth - 1, nn, parent);
                    return;
                }
                FO::dealloc(parent, alloc_);
            } else {
                // ROOT — shouldn't cascade here
                root_[entry.index] = SENTINEL_NODE;
                return;
            }
            depth--;
        }
        // Fell through all stack entries — root is now empty
        // The root index was consumed before the loop started;
        // the caller's ri is not in the stack. This path means
        // the root fan itself was deallocated. But wait — depth==0
        // means we popped everything. The root_[ri] update should
        // happen in the caller if the root fan/compact was the target.
        // This case shouldn't arise in practice since the root fan's
        // removal is handled inline in erase().
    }

    // ==================================================================
    // Insert dispatch
    // ==================================================================

    template<bool INSERT, bool ASSIGN>
    std::pair<bool, bool> insert_impl_(const KEY& key, const VALUE& value) {
        IK ik = KO::to_internal(key);
        VST sv = VT::store(value, alloc_);
        uint8_t ri = static_cast<uint8_t>(ik >> (IK_BITS - 8));
        ik <<= 8;
        int bits = KEY_BITS - 8;

        uint64_t* node = root_[ri];

        // Empty root slot
        if (node == SENTINEL_NODE) {
            if constexpr (!INSERT) { VT::destroy(sv, alloc_); return {true, false}; }
            root_[ri] = make_single_leaf(ik, sv, bits);
            ++size_;
            return {true, true};
        }

        descent_entry_t stack[MAX_DEPTH];
        int depth = 0;
        node_header* hdr = get_header(node);

        // Root compact leaf
        if (hdr->is_leaf()) {
            auto r = compact_insert<INSERT, ASSIGN>(node, hdr, ik, sv);
            if (r.needs_split) {
                if constexpr (!INSERT) { VT::destroy(sv, alloc_); return {true, false}; }
                root_[ri] = convert_root_to_fan(node, hdr, ik, sv, bits);
                ++size_;
                return {true, true};
            }
            root_[ri] = r.node;
            if (r.inserted) { ++size_; return {true, true}; }
            VT::destroy(sv, alloc_);
            return {true, false};
        }

        // Root is fan: descend
        uint8_t bi = static_cast<uint8_t>(ik >> (IK_BITS - 8));
        auto blk = FO::lookup_child(node, bi);
        if (!blk.found) {
            if constexpr (!INSERT) { VT::destroy(sv, alloc_); return {true, false}; }
            ik <<= 8; bits -= 8;
            auto* leaf = make_single_leaf(ik, sv, bits);
            auto* new_fan = FO::add_child(node, bi, leaf, alloc_);
            if (new_fan != node) root_[ri] = new_fan;
            ++size_;
            return {true, true};
        }
        stack[depth++] = {node, parent_type::FAN, bi, static_cast<int16_t>(blk.slot)};
        ik <<= 8; bits -= 8;
        node = blk.child;
        hdr = get_header(node);

        // Main descent loop
        while (true) {
            // Handle skip/prefix
            int skip = hdr->skip();
            if (skip > 0) {
                prefix_t actual = hdr->prefix();
                for (int i = 0; i < skip; ++i) {
                    uint16_t expected = static_cast<uint16_t>(ik >> (IK_BITS - 16));
                    if (expected != actual[i]) {
                        if constexpr (!INSERT) { VT::destroy(sv, alloc_); return {true, false}; }
                        auto* nn = split_on_prefix(node, hdr, ik, sv, bits, i, expected, actual);
                        propagate(stack, depth, nn, node);
                        ++size_;
                        return {true, true};
                    }
                    ik <<= 16; bits -= 16;
                }
            }

            // Compact leaf: insert here
            if (hdr->is_leaf()) {
                auto r = compact_insert<INSERT, ASSIGN>(node, hdr, ik, sv);
                if (r.needs_split) {
                    if constexpr (!INSERT) { VT::destroy(sv, alloc_); return {true, false}; }
                    auto* nn = convert_to_split(node, hdr, ik, sv, bits);
                    propagate(stack, depth, nn, node);
                    ++size_;
                    return {true, true};
                }
                if (r.node != node) propagate(stack, depth, r.node, node);
                if (r.inserted) { ++size_; return {true, true}; }
                VT::destroy(sv, alloc_);
                return {true, false};
            }

            // Split node: lookup top child
            uint8_t ti = static_cast<uint8_t>(ik >> (IK_BITS - 8));
            auto lk = SO::lookup_child(node, ti);

            if (!lk.found) {
                if constexpr (!INSERT) { VT::destroy(sv, alloc_); return {true, false}; }
                ik <<= 8; bits -= 8;
                auto* leaf = make_single_bot_leaf(ik, sv, bits);
                auto* nn = SO::add_child_as_leaf(node, ti, leaf, alloc_);
                if (nn != node) propagate(stack, depth, nn, node);
                ++size_;
                return {true, true};
            }

            bool child_is_leaf = !SO::is_internal(node, ti);

            if (child_is_leaf) {
                ik <<= 8; bits -= 8;
                auto r = bot_leaf_insert<INSERT, ASSIGN>(lk.child, ik, sv);
                if (r.needs_split) {
                    if constexpr (!INSERT) { VT::destroy(sv, alloc_); return {true, false}; }
                    convert_bot_leaf_to_fan(node, ti, lk.slot, lk.child, ik, sv, bits);
                    ++size_;
                    return {true, true};
                }
                if (r.node != lk.child)
                    SO::set_child(node, lk.slot, r.node);
                if (r.inserted) { ++size_; return {true, true}; }
                VT::destroy(sv, alloc_);
                return {true, false};
            }

            // Child is fan: descend
            ik <<= 8; bits -= 8;
            stack[depth++] = {node, parent_type::SPLIT, ti, static_cast<int16_t>(lk.slot)};
            uint64_t* fan = lk.child;

            bi = static_cast<uint8_t>(ik >> (IK_BITS - 8));
            auto blk2 = FO::lookup_child(fan, bi);

            if (!blk2.found) {
                if constexpr (!INSERT) { VT::destroy(sv, alloc_); return {true, false}; }
                ik <<= 8; bits -= 8;
                auto* leaf = make_single_leaf(ik, sv, bits);
                auto* new_fan = FO::add_child(fan, bi, leaf, alloc_);
                if (new_fan != fan) SO::set_child(node, lk.slot, new_fan);
                ++size_;
                return {true, true};
            }

            stack[depth++] = {fan, parent_type::FAN, bi, static_cast<int16_t>(blk2.slot)};
            ik <<= 8; bits -= 8;
            node = blk2.child;
            hdr = get_header(node);
        }
    }

    // ==================================================================
    // Build / Conversion helpers
    // ==================================================================

    // Build node from working arrays of uint64_t suffixes (top-aligned in
    // IK_BITS or fewer bits), plus values. `bits` = number of remaining key
    // bits. Suffixes are in the top `bits` bits of each uint64_t.
    // Returns a node pointer (compact leaf, or split node).

    uint64_t* build_node_from_arrays(uint64_t* suf, VST* vals,
                                      size_t count, int bits) {
        // Small enough for compact leaf
        if (count <= COMPACT_MAX) {
            uint8_t stype = suffix_type_for(bits);
            return dispatch_suffix(stype, [&](auto k_tag) -> uint64_t* {
                using K = decltype(k_tag);
                constexpr int K_BITS = static_cast<int>(sizeof(K)) * 8;
                auto tk = std::make_unique<K[]>(count);
                auto tv = std::make_unique<VST[]>(count);
                // Extract K suffixes and insertion-sort
                for (size_t i = 0; i < count; ++i) {
                    K k = static_cast<K>(suf[i] >> (64 - K_BITS));
                    VST v = vals[i];
                    size_t j = i;
                    while (j > 0 && tk[j-1] > k) {
                        tk[j] = tk[j-1]; tv[j] = tv[j-1]; j--;
                    }
                    tk[j] = k; tv[j] = v;
                }
                return CO::template make_leaf<K>(
                    tk.get(), tv.get(), static_cast<uint32_t>(count),
                    0, prefix_t{0,0}, stype, alloc_);
            });
        }

        // Check skip compression: all share top 16 bits
        if (bits > 16) {
            uint16_t first16 = static_cast<uint16_t>(suf[0] >> 48);
            bool all_same = true;
            for (size_t i = 1; i < count; ++i) {
                if (static_cast<uint16_t>(suf[i] >> 48) != first16) {
                    all_same = false; break;
                }
            }
            if (all_same) {
                // Strip top 16 bits, recurse
                for (size_t i = 0; i < count; ++i) suf[i] <<= 16;
                uint64_t* child = build_node_from_arrays(suf, vals, count, bits - 16);

                // Set skip/prefix on result
                auto* ch = get_header(child);
                uint8_t os = ch->skip();
                prefix_t child_prefix = ch->prefix();
                prefix_t combined = {0, 0};
                combined[0] = first16;
                for (int i = 0; i < os; ++i) combined[1 + i] = child_prefix[i];
                ch->set_skip(os + 1);
                ch->set_prefix(combined);
                return child;
            }
        }

        return build_split_from_arrays(suf, vals, count, bits);
    }

    // Build split node from working arrays.

    uint64_t* build_split_from_arrays(uint64_t* suf, VST* vals,
                                       size_t count, int bits) {
        uint8_t   top_indices[256];
        uint64_t* bot_ptrs[256];
        bool      is_leaf_flags[256];
        int       n_tops = 0;

        int child_bits = bits - 8;

        size_t i = 0;
        while (i < count) {
            uint8_t ti = static_cast<uint8_t>(suf[i] >> 56);
            size_t start = i;
            while (i < count && static_cast<uint8_t>(suf[i] >> 56) == ti) ++i;
            size_t bcount = i - start;

            // Shift suffixes left 8 to remove top byte
            for (size_t j = start; j < i; ++j) suf[j] <<= 8;

            bool need_fan = (bcount > BOT_LEAF_MAX);

            if (need_fan) {
                // Build fan node
                bot_ptrs[n_tops] = build_fan_from_range(
                    suf + start, vals + start, bcount, child_bits);
                is_leaf_flags[n_tops] = false;
            } else if (child_bits <= 8) {
                // Bitmap256 leaf
                auto bk = std::make_unique<uint8_t[]>(bcount);
                for (size_t j = 0; j < bcount; ++j)
                    bk[j] = static_cast<uint8_t>(suf[start + j] >> 56);
                bot_ptrs[n_tops] = BL::make_from_sorted(
                    bk.get(), vals + start, static_cast<uint32_t>(bcount), alloc_);
                is_leaf_flags[n_tops] = true;
            } else {
                // Compact leaf at child_bits
                bot_ptrs[n_tops] = build_compact_from_range(
                    suf + start, vals + start, bcount, child_bits);
                is_leaf_flags[n_tops] = true;
            }
            top_indices[n_tops] = ti;
            n_tops++;
        }

        return SO::make_split(
            top_indices, bot_ptrs, is_leaf_flags, n_tops,
            0, prefix_t{0,0}, alloc_);
    }

    // Build fan node from a range of suffixes.

    uint64_t* build_fan_from_range(uint64_t* suf, VST* vals,
                                    size_t count, int bits) {
        uint8_t   indices[256];
        uint64_t* child_ptrs[256];
        int       n_children = 0;

        int child_bits = bits - 8;

        size_t i = 0;
        while (i < count) {
            uint8_t bi = static_cast<uint8_t>(suf[i] >> 56);
            size_t start = i;
            while (i < count && static_cast<uint8_t>(suf[i] >> 56) == bi) ++i;
            size_t cc = i - start;

            // Shift suffixes left 8
            for (size_t j = start; j < i; ++j) suf[j] <<= 8;

            indices[n_children] = bi;
            child_ptrs[n_children] = build_node_from_arrays(
                suf + start, vals + start, cc, child_bits);
            n_children++;
        }

        return FO::make_fan(indices, child_ptrs, n_children, alloc_);
    }

    // Build compact leaf from a range (helper for build_split).

    uint64_t* build_compact_from_range(uint64_t* suf, VST* vals,
                                        size_t count, int bits) {
        uint8_t stype = suffix_type_for(bits);
        return dispatch_suffix(stype, [&](auto k_tag) -> uint64_t* {
            using K = decltype(k_tag);
            constexpr int K_BITS = static_cast<int>(sizeof(K)) * 8;
            auto tk = std::make_unique<K[]>(count);
            for (size_t i = 0; i < count; ++i)
                tk[i] = static_cast<K>(suf[i] >> (64 - K_BITS));
            // Already sorted (entries grouped from sorted parent)
            return CO::template make_leaf<K>(
                tk.get(), vals, static_cast<uint32_t>(count),
                0, prefix_t{0,0}, stype, alloc_);
        });
    }

    // ==================================================================
    // convert_root_to_fan: root compact leaf overflow → fan node
    //
    // Root must always be compact leaf or fan (never split), because
    // find/insert/erase root paths use FO::branchless_child which
    // expects fan layout. Split nodes have internal_bm and different
    // offsets.
    // ==================================================================

    uint64_t* convert_root_to_fan(uint64_t* node, node_header* hdr,
                                   IK ik, VST value, int bits) {
        uint16_t old_count = hdr->entries();
        size_t total = old_count + 1;
        auto wk = std::make_unique<uint64_t[]>(total);
        auto wv = std::make_unique<VST[]>(total);

        // Collect existing entries + new entry, sorted, left-aligned in u64
        uint8_t st = hdr->suffix_type();
        size_t wi = 0;

        dispatch_suffix(st, [&](auto k_tag) {
            using K = decltype(k_tag);
            constexpr int K_BITS = static_cast<int>(sizeof(K)) * 8;
            K new_suffix = extract_suffix<K>(ik);
            uint64_t new_suf64 = static_cast<uint64_t>(new_suffix) << (64 - K_BITS);

            bool ins = false;
            CO::template for_each<K>(node, hdr, [&](K s, VST v) {
                uint64_t s64 = static_cast<uint64_t>(s) << (64 - K_BITS);
                if (!ins && new_suf64 < s64) {
                    wk[wi] = new_suf64; wv[wi] = value; wi++; ins = true;
                }
                wk[wi] = s64; wv[wi] = v; wi++;
            });
            if (!ins) { wk[wi] = new_suf64; wv[wi] = value; }
        });

        // Build fan node by grouping by top 8 bits
        uint64_t* fan = build_fan_from_range(wk.get(), wv.get(), total, bits);

        dealloc_node(alloc_, node, hdr->alloc_u64());
        return fan;
    }

    // ==================================================================
    // convert_to_split: compact leaf overflow → build split
    // ==================================================================

    uint64_t* convert_to_split(uint64_t* node, node_header* hdr,
                                IK ik, VST value, int bits) {
        uint16_t old_count = hdr->entries();
        size_t total = old_count + 1;
        auto wk = std::make_unique<uint64_t[]>(total);
        auto wv = std::make_unique<VST[]>(total);

        // Collect existing entries + new entry, in sorted order
        // Extract suffix from compact leaf, convert back to uint64_t left-aligned
        uint8_t st = hdr->suffix_type();
        size_t wi = 0;

        dispatch_suffix(st, [&](auto k_tag) {
            using K = decltype(k_tag);
            constexpr int K_BITS = static_cast<int>(sizeof(K)) * 8;
            K new_suffix = extract_suffix<K>(ik);
            uint64_t new_suf64 = static_cast<uint64_t>(new_suffix) << (64 - K_BITS);

            bool ins = false;
            CO::template for_each<K>(node, hdr, [&](K s, VST v) {
                uint64_t s64 = static_cast<uint64_t>(s) << (64 - K_BITS);
                if (!ins && new_suf64 < s64) {
                    wk[wi] = new_suf64; wv[wi] = value; wi++; ins = true;
                }
                wk[wi] = s64; wv[wi] = v; wi++;
            });
            if (!ins) { wk[wi] = new_suf64; wv[wi] = value; }
        });

        // Account for skip/prefix on original node
        int skip = hdr->skip();
        prefix_t orig_prefix = hdr->prefix();

        // Build the new node
        uint64_t* child = build_node_from_arrays(wk.get(), wv.get(), total, bits);

        // Prepend original skip/prefix
        if (skip > 0) {
            auto* ch = get_header(child);
            uint8_t os = ch->skip();
            prefix_t child_prefix = ch->prefix();
            prefix_t combined = {0, 0};
            for (int i = 0; i < skip; ++i) combined[i] = orig_prefix[i];
            for (int i = 0; i < os; ++i) combined[skip + i] = child_prefix[i];
            ch->set_skip(skip + os);
            ch->set_prefix(combined);
        }

        // Dealloc old compact leaf (values already moved, just free node)
        dealloc_node(alloc_, node, hdr->alloc_u64());
        return child;
    }

    // ==================================================================
    // convert_bot_leaf_to_fan: bot-leaf overflow → fan node
    // ==================================================================

    void convert_bot_leaf_to_fan(uint64_t* split_node, uint8_t ti, int ts,
                                  uint64_t* bot, IK ik, VST value, int bits) {
        // Collect entries from bot-leaf + new entry
        auto* bh = get_header(bot);
        uint32_t old_count;
        bool is_bitmap;
        if (!bh->is_leaf()) {
            // Bitmap256 leaf
            old_count = bh->entries();
            is_bitmap = true;
        } else {
            old_count = bh->entries();
            is_bitmap = false;
        }

        size_t total = old_count + 1;
        auto wk = std::make_unique<uint64_t[]>(total);
        auto wv = std::make_unique<VST[]>(total);
        size_t wi = 0;

        if (is_bitmap) {
            // Collect from bitmap256 leaf: suffix is 8-bit
            uint8_t new_suffix = static_cast<uint8_t>(ik >> (IK_BITS - 8));
            uint64_t new_suf64 = static_cast<uint64_t>(new_suffix) << 56;

            bool ins = false;
            BL::for_each(bot, [&](uint8_t s, VST v) {
                uint64_t s64 = static_cast<uint64_t>(s) << 56;
                if (!ins && new_suf64 < s64) {
                    wk[wi] = new_suf64; wv[wi] = value; wi++; ins = true;
                }
                wk[wi] = s64; wv[wi] = v; wi++;
            });
            if (!ins) { wk[wi] = new_suf64; wv[wi] = value; }
        } else {
            // Collect from compact leaf
            uint8_t st = bh->suffix_type();
            dispatch_suffix(st, [&](auto k_tag) {
                using K = decltype(k_tag);
                constexpr int K_BITS = static_cast<int>(sizeof(K)) * 8;
                K new_suffix = extract_suffix<K>(ik);
                uint64_t new_suf64 = static_cast<uint64_t>(new_suffix) << (64 - K_BITS);

                bool ins = false;
                CO::template for_each<K>(bot, bh, [&](K s, VST v) {
                    uint64_t s64 = static_cast<uint64_t>(s) << (64 - K_BITS);
                    if (!ins && new_suf64 < s64) {
                        wk[wi] = new_suf64; wv[wi] = value; wi++; ins = true;
                    }
                    wk[wi] = s64; wv[wi] = v; wi++;
                });
                if (!ins) { wk[wi] = new_suf64; wv[wi] = value; }
            });
        }

        // Group by top 8 bits → build fan
        uint8_t   indices[256];
        uint64_t* child_ptrs[256];
        int       n_children = 0;
        int child_bits = bits - 8;

        size_t ii = 0;
        while (ii < total) {
            uint8_t bi = static_cast<uint8_t>(wk[ii] >> 56);
            size_t start = ii;
            while (ii < total && static_cast<uint8_t>(wk[ii] >> 56) == bi) ++ii;
            size_t cc = ii - start;

            // Shift left 8
            for (size_t j = start; j < ii; ++j) wk[j] <<= 8;

            indices[n_children] = bi;
            child_ptrs[n_children] = build_node_from_arrays(
                wk.get() + start, wv.get() + start, cc, child_bits);
            n_children++;
        }

        auto* new_fan = FO::make_fan(indices, child_ptrs, n_children, alloc_);

        // Update parent split
        SO::set_child(split_node, ts, new_fan);
        SO::mark_internal(split_node, ti);

        // Dealloc old bot-leaf
        if (is_bitmap)
            BL::dealloc_only(bot, alloc_);
        else
            dealloc_node(alloc_, bot, bh->alloc_u64());
    }

    // ==================================================================
    // split_on_prefix: prefix mismatch → new split
    //
    // Called when prefix chunk at index `div_idx` doesn't match.
    // `ik` and `bits` are at the state BEFORE consuming chunk div_idx.
    // ==================================================================

    uint64_t* split_on_prefix(uint64_t* node, node_header* hdr,
                               IK ik, VST value, int bits,
                               int div_idx, uint16_t expected_chunk,
                               prefix_t actual) {
        int skip = hdr->skip();

        // Shared prefix for new parent split node
        uint8_t ss = static_cast<uint8_t>(div_idx);
        prefix_t split_prefix = {0, 0};
        for (int i = 0; i < div_idx; ++i) split_prefix[i] = actual[i];

        // Decompose divergent chunk
        uint16_t nc = expected_chunk;
        uint16_t oc = actual[div_idx];
        uint8_t nt = (nc >> 8) & 0xFF, ot = (oc >> 8) & 0xFF;
        uint8_t nb = nc & 0xFF,        ob = oc & 0xFF;

        // Remaining prefix chunks after divergence
        int rem = skip - 1 - div_idx;

        // Update old node's skip/prefix to remainder
        hdr->set_skip(static_cast<uint8_t>(rem));
        if (rem > 0) {
            prefix_t rem_actual = {0, 0};
            for (int i = 0; i < rem; ++i) rem_actual[i] = actual[div_idx + 1 + i];
            hdr->set_prefix(rem_actual);
        }

        // Build new leaf: consume the divergent chunk + remaining prefix from ik
        IK leaf_ik = ik;
        leaf_ik <<= 16; // consume divergent chunk
        int leaf_bits = bits - 16;

        // New leaf needs remaining prefix from the key
        prefix_t nl_prefix = {0, 0};
        IK tmp_ik = leaf_ik;
        for (int i = 0; i < rem; ++i) {
            nl_prefix[i] = static_cast<uint16_t>(tmp_ik >> (IK_BITS - 16));
            tmp_ik <<= 16;
        }
        leaf_bits -= rem * 16;

        uint64_t* nl = make_single_leaf(tmp_ik, value, leaf_bits);
        if (rem > 0) {
            auto* nlh = get_header(nl);
            nlh->set_skip(static_cast<uint8_t>(rem));
            nlh->set_prefix(nl_prefix);
        }

        // Create the new split structure
        if (nt == ot) {
            // Same top byte, different bottom byte → single split entry with fan of 2
            uint8_t bi[2]; uint64_t* cp[2];
            if (nb < ob) { bi[0]=nb; cp[0]=nl; bi[1]=ob; cp[1]=node; }
            else         { bi[0]=ob; cp[0]=node; bi[1]=nb; cp[1]=nl; }
            auto* fan = FO::make_fan(bi, cp, 2, alloc_);

            uint8_t   ti_arr[1] = {nt};
            uint64_t* bp_arr[1] = {fan};
            bool      il_arr[1] = {false};
            return SO::make_split(ti_arr, bp_arr, il_arr, 1, ss, split_prefix, alloc_);
        } else {
            // Different top bytes → two split entries, each with a fan of 1
            uint8_t obi[1] = {ob}; uint64_t* ocp[1] = {node};
            auto* old_fan = FO::make_fan(obi, ocp, 1, alloc_);

            uint8_t nbi[1] = {nb}; uint64_t* ncp[1] = {nl};
            auto* new_fan = FO::make_fan(nbi, ncp, 1, alloc_);

            uint8_t   ti_arr[2]; uint64_t* bp_arr[2]; bool il_arr[2] = {false, false};
            if (nt < ot) { ti_arr[0]=nt; bp_arr[0]=new_fan; ti_arr[1]=ot; bp_arr[1]=old_fan; }
            else         { ti_arr[0]=ot; bp_arr[0]=old_fan; ti_arr[1]=nt; bp_arr[1]=new_fan; }

            return SO::make_split(ti_arr, bp_arr, il_arr, 2, ss, split_prefix, alloc_);
        }
    }

    // ==================================================================
    // Remove all
    // ==================================================================

    void remove_all() noexcept {
        for (int i = 0; i < 256; ++i) {
            uint64_t* child = root_[i];
            if (child == SENTINEL_NODE) continue;
            auto* h = get_header(child);
            if (h->is_leaf()) {
                destroy_compact(child);
            } else {
                remove_fan_children(child);
                FO::dealloc(child, alloc_);
            }
            root_[i] = SENTINEL_NODE;
        }
        size_ = 0;
    }

    void remove_fan_children(uint64_t* fan) noexcept {
        FO::for_each_child(fan, [&](uint8_t, int, uint64_t* child) {
            auto* h = get_header(child);
            if (h->is_leaf()) {
                destroy_compact(child);
            } else {
                // Split node
                remove_split_children(child);
                SO::dealloc(child, alloc_);
            }
        });
    }

    void remove_split_children(uint64_t* split) noexcept {
        SO::for_each_child(split, [&](uint8_t, int, uint64_t* child, bool is_leaf) {
            if (is_leaf) {
                auto* h = get_header(child);
                if (h->is_leaf()) {
                    destroy_compact(child);
                } else {
                    BL::destroy_and_dealloc(child, alloc_);
                }
            } else {
                // Fan node
                remove_fan_children(child);
                FO::dealloc(child, alloc_);
            }
        });
    }

    // ==================================================================
    // Stats collection
    // ==================================================================

    void stats_compact(const uint64_t* node, debug_stats_t& s) const noexcept {
        auto* h = get_header(node);
        s.compact_leaves++;
        s.total_entries += h->entries();
        s.total_bytes += static_cast<size_t>(h->alloc_u64()) * 8;
    }

    void stats_bitmap_leaf(const uint64_t* node, debug_stats_t& s) const noexcept {
        auto* h = get_header(node);
        s.bitmap_leaves++;
        s.total_entries += h->entries();
        s.total_bytes += static_cast<size_t>(h->alloc_u64()) * 8;
    }

    void stats_fan(const uint64_t* fan, debug_stats_t& s) const noexcept {
        s.fan_nodes++;
        s.total_bytes += static_cast<size_t>(get_header(fan)->alloc_u64()) * 8;
        FO::for_each_child(fan, [&](uint8_t, int, uint64_t* child) {
            auto* h = get_header(child);
            if (h->is_leaf()) {
                stats_compact(child, s);
            } else {
                stats_split(child, s);
            }
        });
    }

    void stats_split(const uint64_t* split, debug_stats_t& s) const noexcept {
        s.split_nodes++;
        s.total_bytes += static_cast<size_t>(get_header(split)->alloc_u64()) * 8;
        SO::for_each_child(split, [&](uint8_t, int, uint64_t* child, bool is_leaf) {
            if (is_leaf) {
                auto* h = get_header(child);
                if (h->is_leaf()) {
                    stats_compact(child, s);
                } else {
                    stats_bitmap_leaf(child, s);
                }
            } else {
                stats_fan(child, s);
            }
        });
    }
};

} // namespace gteitelbaum

#endif // KNTRIE_IMPL_HPP
