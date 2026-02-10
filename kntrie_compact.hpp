#ifndef KNTRIE_COMPACT_HPP
#define KNTRIE_COMPACT_HPP

#include "kntrie_support.hpp"

#include <cstdint>
#include <cstring>
#include <algorithm>

namespace kn3 {

// ==========================================================================
// Search Strategy: JumpSearch  (stride 256 -> 16 -> 1, no index overlay)
// ==========================================================================

template<typename K>
struct JumpSearch {
    // Returns index if found (>=0), or -1 if not found.
    static int search(const K* keys, int count, K key) noexcept {
        const K* end = keys + count;
        const K* p = keys;
        for (const K* q = p + 256; q < end; q += 256) { if (*q > key) break; p = q; }
        for (const K* q = p + 16;  q < end; q += 16)  { if (*q > key) break; p = q; }
        for (const K* q = p + 1;   q < end; ++q)      { if (*q > key) break; p = q; }
        return (p < end && *p == key) ? static_cast<int>(p - keys) : -1;
    }

    // Returns index if found (>=0), or -(insertion_point + 1) if not found.
    static int search_insert(const K* keys, int count, K key) noexcept {
        if (count == 0) [[unlikely]] return -(0 + 1);
        const K* end = keys + count;
        const K* p = keys;
        for (const K* q = p + 256; q < end; q += 256) { if (*q > key) break; p = q; }
        for (const K* q = p + 16;  q < end; q += 16)  { if (*q > key) break; p = q; }
        for (const K* q = p + 1;   q < end; ++q)      { if (*q > key) break; p = q; }
        if (*p == key) return static_cast<int>(p - keys);
        int pos = static_cast<int>(p - keys);
        if (*p < key) pos++;
        return -(pos + 1);
    }
};

// ==========================================================================
// CompactOps  -- builds/searches/mutates compact leaf nodes
//
// Layout: [header (2 u64)][sorted_keys (aligned)][values (aligned)]
//   flags_ bit 0 = 0 -> is_leaf (compact)
//   node[0] = NodeHeader, node[1] = prefix (0 when skip==0)
//
// Keys and values arrays have `total` slots where
//   total = SlotTable<BITS, VST>::max_slots(alloc_u64)
// Real entries = header.entries.  Dups = total - entries.
// Dups are copies of adjacent entries, evenly distributed.
//
// Allocations are padded via round_up_u64 to enable in-place insert/erase
// through dup consumption/creation.
// ==========================================================================

template<typename KEY, typename VALUE, typename ALLOC>
struct CompactOps {
    using KOps = KeyOps<KEY>;
    using VT   = ValueTraits<VALUE, ALLOC>;
    using VST  = typename VT::slot_type;

    // --- exact needed size for `count` entries (not padded, no dups) ---

    template<int BITS>
    static constexpr size_t size_u64(size_t count) noexcept {
        using K = typename suffix_traits<BITS>::type;
        size_t kb = count * sizeof(K);
        kb = (kb + 7) & ~size_t{7};
        size_t vb = count * sizeof(VST);
        vb = (vb + 7) & ~size_t{7};
        return HEADER_U64 + (kb + vb) / 8;
    }

    // --- total physical slots for a given alloc ---

    template<int BITS>
    static uint16_t total_slots(const NodeHeader* h) noexcept {
        return SlotTable<BITS, VST>::max_slots(h->alloc_u64);
    }

    template<int BITS>
    static uint16_t total_slots(uint16_t alloc_u64) noexcept {
        return SlotTable<BITS, VST>::max_slots(alloc_u64);
    }

    // ==================================================================
    // Factory: build from pre-sorted working arrays, with dup seeding
    // ==================================================================

    template<int BITS>
    static uint64_t* make_leaf(const typename suffix_traits<BITS>::type* sorted_keys,
                               const VST* values, uint32_t count,
                               uint8_t skip, uint64_t prefix, ALLOC& alloc) {
        size_t needed = size_u64<BITS>(count);
        size_t au64 = round_up_u64(needed);
        uint64_t* node = alloc_node(alloc, au64);
        auto* h = get_header(node);
        h->entries = static_cast<uint16_t>(count);
        h->descendants = static_cast<uint16_t>(count);
        h->alloc_u64 = static_cast<uint16_t>(au64);
        h->set_skip(skip);
        if (skip > 0) set_prefix(node, prefix);

        uint16_t total = total_slots<BITS>(h);
        if (count > 0) {
            if (total == count) {
                // No room for dups — copy directly
                using K = typename suffix_traits<BITS>::type;
                std::memcpy(keys_<BITS>(node), sorted_keys, count * sizeof(K));
                std::memcpy(vals_<BITS>(node, total), values, count * sizeof(VST));
            } else {
                // Seed dups into the extra space
                seed_from_real_<BITS>(node, sorted_keys, values, count, total);
            }
        }
        return node;
    }

    // ==================================================================
    // Iterate entries: cb(K suffix, VST value_slot) — skips dups
    // ==================================================================

    template<int BITS, typename Fn>
    static void for_each(const uint64_t* node, const NodeHeader* h, Fn&& cb) {
        using K = typename suffix_traits<BITS>::type;
        uint16_t total = total_slots<BITS>(h);
        const K*   kd = keys_<BITS>(node);
        const VST* vd = vals_<BITS>(node, total);
        for (uint16_t i = 0; i < total; ++i) {
            if (i > 0 && kd[i] == kd[i - 1]) continue;  // skip dup
            cb(kd[i], vd[i]);
        }
    }

    // ==================================================================
    // Destroy all values + deallocate node (skip dups to avoid double-free)
    // ==================================================================

    template<int BITS>
    static void destroy_and_dealloc(uint64_t* node, ALLOC& alloc) {
        auto* h = get_header(node);
        if constexpr (!VT::is_inline) {
            uint16_t total = total_slots<BITS>(h);
            const auto* kd = keys_<BITS>(node);
            VST* vd = vals_<BITS>(node, total);
            for (uint16_t i = 0; i < total; ++i) {
                if (i > 0 && kd[i] == kd[i - 1]) continue;  // same T*
                VT::destroy(vd[i], alloc);
            }
        }
        dealloc_node(alloc, node, h->alloc_u64);
    }

    // ==================================================================
    // Find
    // ==================================================================

    template<int BITS>
    static const VALUE* find(const uint64_t* node, NodeHeader h,
                             uint64_t ik) noexcept {
        using K = typename suffix_traits<BITS>::type;
        K suffix = static_cast<K>(KOps::template extract_suffix<BITS>(ik));
        uint16_t total = total_slots<BITS>(&h);
        int idx = JumpSearch<K>::search(keys_<BITS>(node),
                                        static_cast<int>(total), suffix);
        if (idx < 0) [[unlikely]] return nullptr;
        return VT::as_ptr(vals_<BITS>(node, total)[idx]);
    }

    // ==================================================================
    // Insert
    //
    // Template params:
    //   INSERT: allow inserting new keys
    //   ASSIGN: allow overwriting existing values
    //
    // When dups > 0: consume a dup (in-place, no realloc).
    // When dups == 0: realloc to next size class, seed dups.
    // ==================================================================

    struct CompactInsertResult { uint64_t* node; bool inserted; bool needs_split; };

    template<int BITS, bool INSERT = true, bool ASSIGN = true>
    requires (INSERT || ASSIGN)
    static CompactInsertResult insert(uint64_t* node, NodeHeader* h,
                                      uint64_t ik, VST value, ALLOC& alloc) {
        using K = typename suffix_traits<BITS>::type;
        K suffix = static_cast<K>(KOps::template extract_suffix<BITS>(ik));
        uint16_t total = total_slots<BITS>(h);
        K*   kd = keys_<BITS>(node);
        VST* vd = vals_<BITS>(node, total);

        int idx = JumpSearch<K>::search_insert(kd, static_cast<int>(total), suffix);

        // --- Key exists: update path ---
        if (idx >= 0) {
            if constexpr (ASSIGN) {
                VT::destroy(vd[idx], alloc);
                VT::write_slot(&vd[idx], value);
                // Walk left: update dups of this key to share new value
                for (int i = idx - 1; i >= 0 && kd[i] == suffix; --i)
                    VT::write_slot(&vd[i], value);
            }
            return {node, false, false};
        }
        if constexpr (!INSERT) return {node, false, false};

        int ins = -(idx + 1);  // insertion point in total-sized array
        uint16_t dups = total - h->entries;

        // --- Dups available: consume one in-place ---
        if (dups > 0) {
            insert_consume_dup_<BITS>(kd, vd, total, ins, suffix, value);
            h->entries++;
            h->descendants++;
            return {node, true, false};
        }

        // --- No dups: need realloc ---
        if (h->entries >= COMPACT_MAX) return {node, false, true};  // needs_split

        uint16_t old_entries = h->entries;
        size_t needed = size_u64<BITS>(old_entries + 1);
        size_t au64 = round_up_u64(needed);
        uint64_t* nn = alloc_node(alloc, au64);
        auto* nh = get_header(nn);
        *nh = *h;
        nh->entries = old_entries + 1;
        nh->descendants = old_entries + 1;
        nh->alloc_u64 = static_cast<uint16_t>(au64);
        if (h->skip() > 0) set_prefix(nn, get_prefix(node));

        uint16_t new_total = total_slots<BITS>(nh);

        // Source has 0 dups (total == entries), so kd/vd are clean sorted arrays.
        // Merge new entry and seed dups.
        seed_with_insert_<BITS>(nn, kd, vd, old_entries, suffix, value,
                                old_entries + 1, new_total);

        dealloc_node(alloc, node, h->alloc_u64);
        return {nn, true, false};
    }

    // ==================================================================
    // Erase
    //
    // In-place when not oversized: O(1) dup creation.
    // Realloc when should_shrink: dedup + skip erased + seed.
    // Returns {nullptr, true} when last entry removed.
    // ==================================================================

    template<int BITS>
    static EraseResult erase(uint64_t* node, NodeHeader* h,
                             uint64_t ik, ALLOC& alloc) {
        using K = typename suffix_traits<BITS>::type;
        K suffix = static_cast<K>(KOps::template extract_suffix<BITS>(ik));
        uint16_t total = total_slots<BITS>(h);
        K*   kd = keys_<BITS>(node);
        VST* vd = vals_<BITS>(node, total);

        int idx = JumpSearch<K>::search(kd, static_cast<int>(total), suffix);
        if (idx < 0) return {node, false};

        uint16_t nc = h->entries - 1;

        // --- Last real entry: destroy and dealloc ---
        if (nc == 0) {
            if constexpr (!VT::is_inline)
                VT::destroy(vd[idx], alloc);
            dealloc_node(alloc, node, h->alloc_u64);
            return {nullptr, true};
        }

        size_t needed = size_u64<BITS>(nc);

        // --- Should shrink: realloc with dedup + skip + seed ---
        if (should_shrink_u64(h->alloc_u64, needed)) {
            size_t au64 = round_up_u64(needed);
            uint64_t* nn = alloc_node(alloc, au64);
            auto* nh = get_header(nn);
            *nh = *h;
            nh->entries = nc;
            nh->descendants = nc;
            nh->alloc_u64 = static_cast<uint16_t>(au64);
            if (h->skip() > 0) set_prefix(nn, get_prefix(node));

            uint16_t new_total = total_slots<BITS>(nh);

            // Dedup source, skip erased key, seed into new node.
            // Handles T* destroy for the erased key.
            seed_with_skip_<BITS>(nn, kd, vd, total, suffix, nc, new_total, alloc);

            dealloc_node(alloc, node, h->alloc_u64);
            return {nn, true};
        }

        // --- In-place O(1) erase: convert run to neighbor dups ---
        erase_create_dup_<BITS>(kd, vd, total, idx, suffix, alloc);
        h->entries = nc;
        h->descendants = nc;
        return {node, true};
    }

private:
    // --- layout helpers (private) ---
    // Layout: [header (2 u64)][keys...][padding to 8-byte][values...]
    // Keys and values arrays have `total` physical slots.

    template<int BITS>
    static auto keys_(uint64_t* node) noexcept {
        using K = typename suffix_traits<BITS>::type;
        return reinterpret_cast<K*>(node + HEADER_U64);
    }
    template<int BITS>
    static auto keys_(const uint64_t* node) noexcept {
        using K = typename suffix_traits<BITS>::type;
        return reinterpret_cast<const K*>(node + HEADER_U64);
    }

    template<int BITS>
    static VST* vals_(uint64_t* node, size_t total) noexcept {
        using K = typename suffix_traits<BITS>::type;
        size_t kb = total * sizeof(K);
        kb = (kb + 7) & ~size_t{7};
        return reinterpret_cast<VST*>(
            reinterpret_cast<char*>(node + HEADER_U64) + kb);
    }
    template<int BITS>
    static const VST* vals_(const uint64_t* node, size_t total) noexcept {
        using K = typename suffix_traits<BITS>::type;
        size_t kb = total * sizeof(K);
        kb = (kb + 7) & ~size_t{7};
        return reinterpret_cast<const VST*>(
            reinterpret_cast<const char*>(node + HEADER_U64) + kb);
    }

    // ==================================================================
    // Insert helper: consume nearest dup, shift, write new entry
    // ==================================================================

    template<int BITS>
    static void insert_consume_dup_(
            typename suffix_traits<BITS>::type* kd, VST* vd,
            int total, int ins,
            typename suffix_traits<BITS>::type suffix, VST value) {
        using K = typename suffix_traits<BITS>::type;

        // Find nearest dup by scanning left and right from insertion point
        int left_dup = -1;
        for (int i = ins - 1; i >= 1; --i) {
            if (kd[i] == kd[i - 1]) { left_dup = i; break; }
        }
        int right_dup = -1;
        for (int i = ins; i < total - 1; ++i) {
            if (kd[i] == kd[i + 1]) { right_dup = i; break; }
        }

        // Pick closer dup
        int dup_pos;
        if (left_dup < 0) dup_pos = right_dup;
        else if (right_dup < 0) dup_pos = left_dup;
        else dup_pos = (ins - left_dup <= right_dup - ins) ? left_dup : right_dup;

        int write_pos;
        if (dup_pos < ins) {
            // Dup is left: shift [dup_pos+1 .. ins-1] left by 1
            int shift_count = ins - 1 - dup_pos;
            if (shift_count > 0) {
                std::memmove(kd + dup_pos, kd + dup_pos + 1, shift_count * sizeof(K));
                std::memmove(vd + dup_pos, vd + dup_pos + 1, shift_count * sizeof(VST));
            }
            write_pos = ins - 1;
        } else {
            // Dup is right: shift [ins .. dup_pos-1] right by 1
            int shift_count = dup_pos - ins;
            if (shift_count > 0) {
                std::memmove(kd + ins + 1, kd + ins, shift_count * sizeof(K));
                std::memmove(vd + ins + 1, vd + ins, shift_count * sizeof(VST));
            }
            write_pos = ins;
        }

        kd[write_pos] = suffix;
        VT::write_slot(&vd[write_pos], value);
    }

    // ==================================================================
    // Erase helper: convert run of erased key to neighbor dups (O(1))
    // ==================================================================

    template<int BITS>
    static void erase_create_dup_(
            typename suffix_traits<BITS>::type* kd, VST* vd,
            int total, int idx,
            typename suffix_traits<BITS>::type suffix, ALLOC& alloc) {
        using K = typename suffix_traits<BITS>::type;

        // Find full run of this key: [first .. idx]
        // (JumpSearch finds last match, so run extends leftward)
        int first = idx;
        while (first > 0 && kd[first - 1] == suffix) --first;

        // Destroy value ONCE (all slots in run share same T*)
        if constexpr (!VT::is_inline)
            VT::destroy(vd[first], alloc);

        // Overwrite entire run with neighbor's key+value
        K   neighbor_key;
        VST neighbor_val;
        if (first > 0) {
            neighbor_key = kd[first - 1];
            neighbor_val = vd[first - 1];
        } else {
            // Must have a right neighbor since nc > 0
            neighbor_key = kd[idx + 1];
            neighbor_val = vd[idx + 1];
        }

        for (int i = first; i <= idx; ++i) {
            kd[i] = neighbor_key;
            vd[i] = neighbor_val;
        }
    }

    // ==================================================================
    // Seed: distribute dups evenly among real entries into node arrays
    //
    // Input: sorted real_keys/real_vals of n_entries entries.
    // Output: total slots written to node's key/val arrays.
    // ==================================================================

    template<int BITS>
    static void seed_from_real_(uint64_t* node,
                                const typename suffix_traits<BITS>::type* real_keys,
                                const VST* real_vals,
                                uint16_t n_entries, uint16_t total) {
        using K = typename suffix_traits<BITS>::type;
        K*   kd = keys_<BITS>(node);
        VST* vd = vals_<BITS>(node, total);

        if (n_entries == total) {
            std::memcpy(kd, real_keys, n_entries * sizeof(K));
            std::memcpy(vd, real_vals, n_entries * sizeof(VST));
            return;
        }

        uint16_t n_dups = total - n_entries;
        uint16_t stride = n_entries / (n_dups + 1);
        uint16_t remainder = n_entries % (n_dups + 1);

        int write = 0, src = 0, placed = 0;
        while (placed < n_dups) {
            int chunk = stride + (placed < remainder ? 1 : 0);
            std::memcpy(kd + write, real_keys + src, chunk * sizeof(K));
            std::memcpy(vd + write, real_vals + src, chunk * sizeof(VST));
            write += chunk;
            src += chunk;
            // Place dup: copy of entry just written
            kd[write] = kd[write - 1];
            vd[write] = vd[write - 1];
            write++;
            placed++;
        }

        // Copy remaining real entries
        int remaining = n_entries - src;
        if (remaining > 0) {
            std::memcpy(kd + write, real_keys + src, remaining * sizeof(K));
            std::memcpy(vd + write, real_vals + src, remaining * sizeof(VST));
        }
    }

    // ==================================================================
    // Seed with insert: dedup source, merge new entry, seed dups
    //
    // Source may have dups (old_total > old_entries when called from
    // a node that had dups == 0 this won't happen, but handle generally).
    // old_keys/old_vals have old_entries real entries (no dups in this path).
    // ==================================================================

    template<int BITS>
    static void seed_with_insert_(uint64_t* node,
                                   const typename suffix_traits<BITS>::type* old_keys,
                                   const VST* old_vals,
                                   uint16_t old_entries,
                                   typename suffix_traits<BITS>::type new_suffix,
                                   VST new_val,
                                   uint16_t n_entries,
                                   uint16_t new_total) {
        using K = typename suffix_traits<BITS>::type;

        // Build merged sorted array: old entries + new entry
        // old_keys is already sorted, insert new_suffix at correct position
        auto tmp_k = std::make_unique<K[]>(n_entries);
        auto tmp_v = std::make_unique<VST[]>(n_entries);

        // Find insertion point in old_keys
        int ins = 0;
        while (ins < old_entries && old_keys[ins] < new_suffix) ++ins;

        std::memcpy(tmp_k.get(), old_keys, ins * sizeof(K));
        std::memcpy(tmp_v.get(), old_vals, ins * sizeof(VST));
        tmp_k[ins] = new_suffix;
        VT::write_slot(&tmp_v[ins], new_val);
        std::memcpy(tmp_k.get() + ins + 1, old_keys + ins,
                     (old_entries - ins) * sizeof(K));
        std::memcpy(tmp_v.get() + ins + 1, old_vals + ins,
                     (old_entries - ins) * sizeof(VST));

        seed_from_real_<BITS>(node, tmp_k.get(), tmp_v.get(), n_entries, new_total);
    }

    // ==================================================================
    // Seed with skip: dedup source, skip one erased key, seed dups
    //
    // Source has `src_total` physical slots (entries + dups).
    // Collects unique entries, skips first match of skip_suffix,
    // destroys the skipped entry's T*, seeds into new node.
    // ==================================================================

    template<int BITS>
    static void seed_with_skip_(uint64_t* node,
                                 const typename suffix_traits<BITS>::type* src_keys,
                                 const VST* src_vals,
                                 uint16_t src_total,
                                 typename suffix_traits<BITS>::type skip_suffix,
                                 uint16_t n_entries,
                                 uint16_t new_total,
                                 ALLOC& alloc) {
        using K = typename suffix_traits<BITS>::type;

        auto tmp_k = std::make_unique<K[]>(n_entries);
        auto tmp_v = std::make_unique<VST[]>(n_entries);

        bool skipped = false;
        int ri = 0;
        for (int i = 0; i < src_total; ++i) {
            // Skip dups
            if (i > 0 && src_keys[i] == src_keys[i - 1]) continue;

            // Skip erased key (first match only)
            if (!skipped && src_keys[i] == skip_suffix) {
                skipped = true;
                if constexpr (!VT::is_inline)
                    VT::destroy(src_vals[i], alloc);
                continue;
            }

            tmp_k[ri] = src_keys[i];
            tmp_v[ri] = src_vals[i];
            ri++;
        }

        seed_from_real_<BITS>(node, tmp_k.get(), tmp_v.get(), n_entries, new_total);
    }
};

} // namespace kn3

#endif // KNTRIE_COMPACT_HPP
