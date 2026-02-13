#ifndef KNTRIE_COMPACT_HPP
#define KNTRIE_COMPACT_HPP

#include "kntrie_support.hpp"

#include <cstdint>
#include <cstring>
#include <algorithm>
#include <memory>

namespace gteitelbaum {

// ==========================================================================
// Search Strategy: jump_search (stride 256 -> 16 -> 1, no index overlay)
// ==========================================================================

template<typename K>
struct jump_search {
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
// compact_ops -- builds/searches/mutates compact leaf nodes
//
// Layout: [header (1 u64)][sorted_keys (aligned)][values (aligned)]
//   is_leaf() == true (bit 15 = 0)
//   suffix_type() gives K type
//
// Keys and values arrays have `total` slots where
//   total = slot_table<K, VST>::max_slots(alloc_u64)
// Real entries = header.entries(). Dups = total - entries.
// Dups are copies of adjacent entries, evenly distributed.
//
// Methods are templated on K (uint8_t/uint16_t/uint32_t/uint64_t).
// ==========================================================================

template<typename KEY, typename VALUE, typename ALLOC>
struct compact_ops {
    using VT   = value_traits<VALUE, ALLOC>;
    using VST  = typename VT::slot_type;

    // --- exact needed size for `count` entries (not padded, no dups) ---

    template<typename K>
    static constexpr size_t size_u64(size_t count) noexcept {
        size_t kb = count * sizeof(K);
        kb = (kb + 7) & ~size_t{7};
        size_t vb = count * sizeof(VST);
        vb = (vb + 7) & ~size_t{7};
        return HEADER_U64 + (kb + vb) / 8;
    }

    // --- total physical slots for a given alloc ---

    template<typename K>
    static uint16_t total_slots(const node_header* h) noexcept {
        return slot_table<K, VST>::max_slots(h->alloc_u64());
    }

    template<typename K>
    static uint16_t total_slots(uint16_t alloc_u64) noexcept {
        return slot_table<K, VST>::max_slots(alloc_u64);
    }

    // ==================================================================
    // Factory: build from pre-sorted working arrays, with dup seeding
    // ==================================================================

    template<typename K>
    static uint64_t* make_leaf(const K* sorted_keys,
                               const VST* values, uint32_t count,
                               uint8_t skip, prefix_t prefix,
                               uint8_t stype, ALLOC& alloc) {
        size_t needed = size_u64<K>(count);
        size_t au64 = round_up_u64(needed);
        uint64_t* node = alloc_node(alloc, au64);
        auto* h = get_header(node);
        h->set_entries(static_cast<uint16_t>(count));
        h->set_alloc_u64(static_cast<uint16_t>(au64));
        h->set_suffix_type(stype);
        h->set_skip(skip);
        if (skip > 0) h->set_prefix(prefix);

        uint16_t total = total_slots<K>(h);
        if (count > 0) {
            if (total == count) {
                std::memcpy(keys<K>(node), sorted_keys, count * sizeof(K));
                std::memcpy(vals<K>(node, total), values, count * sizeof(VST));
            } else {
                seed_from_real<K>(node, sorted_keys, values, count, total);
            }
        }
        return node;
    }

    // ==================================================================
    // Iterate entries: cb(K suffix, VST value_slot) — skips dups
    // ==================================================================

    template<typename K, typename Fn>
    static void for_each(const uint64_t* node, const node_header* h, Fn&& cb) {
        uint16_t total = total_slots<K>(h);
        const K*   kd = keys<K>(node);
        const VST* vd = vals<K>(node, total);
        for (uint16_t i = 0; i < total; ++i) {
            if (i > 0 && kd[i] == kd[i - 1]) continue;
            cb(kd[i], vd[i]);
        }
    }

    // ==================================================================
    // Destroy all values + deallocate node (skip dups to avoid double-free)
    // ==================================================================

    template<typename K>
    static void destroy_and_dealloc(uint64_t* node, ALLOC& alloc) {
        auto* h = get_header(node);
        if constexpr (!VT::IS_INLINE) {
            uint16_t total = total_slots<K>(h);
            const K* kd = keys<K>(node);
            VST* vd = vals<K>(node, total);
            for (uint16_t i = 0; i < total; ++i) {
                if (i > 0 && kd[i] == kd[i - 1]) continue;
                VT::destroy(vd[i], alloc);
            }
        }
        dealloc_node(alloc, node, h->alloc_u64());
    }

    // ==================================================================
    // Find
    // ==================================================================

    template<typename K>
    static const VALUE* find(const uint64_t* node, node_header h,
                             K suffix) noexcept {
        uint16_t total = total_slots<K>(&h);
        int idx = jump_search<K>::search(keys<K>(node),
                                         static_cast<int>(total), suffix);
        if (idx < 0) [[unlikely]] return nullptr;
        return VT::as_ptr(vals<K>(node, total)[idx]);
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

    struct compact_insert_result_t {
        uint64_t* node;
        bool      inserted;
        bool      needs_split;
    };

    template<typename K, bool INSERT = true, bool ASSIGN = true>
    requires (INSERT || ASSIGN)
    static compact_insert_result_t insert(uint64_t* node, node_header* h,
                                          K suffix, VST value, ALLOC& alloc) {
        uint16_t total = total_slots<K>(h);
        K*   kd = keys<K>(node);
        VST* vd = vals<K>(node, total);

        int idx = jump_search<K>::search_insert(kd, static_cast<int>(total), suffix);

        // --- Key exists: update path ---
        if (idx >= 0) {
            if constexpr (ASSIGN) {
                VT::destroy(vd[idx], alloc);
                VT::write_slot(&vd[idx], value);
                for (int i = idx - 1; i >= 0 && kd[i] == suffix; --i)
                    VT::write_slot(&vd[i], value);
            }
            return {node, false, false};
        }
        if constexpr (!INSERT) return {node, false, false};

        int ins = -(idx + 1);
        uint16_t entries = h->entries();
        uint16_t dups = total - entries;

        // --- Dups available: consume one in-place ---
        if (dups > 0) {
            insert_consume_dup<K>(kd, vd, total, ins, entries, suffix, value);
            h->set_entries(entries + 1);
            return {node, true, false};
        }

        // --- No dups: need realloc ---
        if (entries >= COMPACT_MAX) return {node, false, true};

        uint16_t old_entries = entries;
        size_t needed = size_u64<K>(old_entries + 1);
        size_t au64 = round_up_u64(needed);
        uint64_t* nn = alloc_node(alloc, au64);
        auto* nh = get_header(nn);
        *nh = *h;
        nh->set_entries(old_entries + 1);
        nh->set_alloc_u64(static_cast<uint16_t>(au64));

        uint16_t new_total = total_slots<K>(nh);

        seed_with_insert<K>(nn, kd, vd, old_entries, suffix, value,
                            old_entries + 1, new_total);

        dealloc_node(alloc, node, h->alloc_u64());
        return {nn, true, false};
    }

    // ==================================================================
    // Erase
    //
    // In-place when not oversized: O(1) dup creation.
    // Realloc when should_shrink: dedup + skip erased + seed.
    // Returns {nullptr, true} when last entry removed.
    // ==================================================================

    template<typename K>
    static erase_result_t erase(uint64_t* node, node_header* h,
                                K suffix, ALLOC& alloc) {
        uint16_t total = total_slots<K>(h);
        K*   kd = keys<K>(node);
        VST* vd = vals<K>(node, total);

        int idx = jump_search<K>::search(kd, static_cast<int>(total), suffix);
        if (idx < 0) return {node, false};

        uint16_t nc = h->entries() - 1;

        // --- Last real entry: destroy and dealloc ---
        if (nc == 0) {
            if constexpr (!VT::IS_INLINE)
                VT::destroy(vd[idx], alloc);
            dealloc_node(alloc, node, h->alloc_u64());
            return {nullptr, true};
        }

        size_t needed = size_u64<K>(nc);

        // --- Should shrink: realloc with dedup + skip + seed ---
        if (should_shrink_u64(h->alloc_u64(), needed)) {
            size_t au64 = round_up_u64(needed);
            uint64_t* nn = alloc_node(alloc, au64);
            auto* nh = get_header(nn);
            *nh = *h;
            nh->set_entries(nc);
            nh->set_alloc_u64(static_cast<uint16_t>(au64));

            uint16_t new_total = total_slots<K>(nh);

            seed_with_skip<K>(nn, kd, vd, total, suffix, nc, new_total, alloc);

            dealloc_node(alloc, node, h->alloc_u64());
            return {nn, true};
        }

        // --- In-place O(1) erase: convert run to neighbor dups ---
        erase_create_dup<K>(kd, vd, total, idx, suffix, alloc);
        h->set_entries(nc);
        return {node, true};
    }

    // ==================================================================
    // Layout helpers
    // ==================================================================

    template<typename K>
    static K* keys(uint64_t* node) noexcept {
        return reinterpret_cast<K*>(node + HEADER_U64);
    }
    template<typename K>
    static const K* keys(const uint64_t* node) noexcept {
        return reinterpret_cast<const K*>(node + HEADER_U64);
    }

    template<typename K>
    static VST* vals(uint64_t* node, size_t total) noexcept {
        size_t kb = total * sizeof(K);
        kb = (kb + 7) & ~size_t{7};
        return reinterpret_cast<VST*>(
            reinterpret_cast<char*>(node + HEADER_U64) + kb);
    }
    template<typename K>
    static const VST* vals(const uint64_t* node, size_t total) noexcept {
        size_t kb = total * sizeof(K);
        kb = (kb + 7) & ~size_t{7};
        return reinterpret_cast<const VST*>(
            reinterpret_cast<const char*>(node + HEADER_U64) + kb);
    }

private:

    // ==================================================================
    // Insert helper: consume nearest dup, shift, write new entry
    // ==================================================================

    template<typename K>
    static void insert_consume_dup(
            K* kd, VST* vd, int total, int ins, uint16_t entries,
            K suffix, VST value) {

        int dup_pos = -1;

        if (total <= 64) {
            for (int i = ins; i < total - 1; ++i) {
                if (kd[i] == kd[i + 1]) { dup_pos = i; break; }
            }
            if (dup_pos < 0) {
                for (int i = ins - 1; i >= 1; --i) {
                    if (kd[i] == kd[i - 1]) { dup_pos = i; break; }
                }
            }
        } else {
            uint16_t dups = static_cast<uint16_t>(total - entries);
            int band = static_cast<int>(entries / (dups + 1)) + 1;

            int r_lo = ins, r_hi = ins;
            int l_hi = ins - 1, l_lo = ins - 1;

            while (dup_pos < 0) {
                r_hi = std::min(r_lo + band, total - 1);
                for (int i = r_lo; i < r_hi; ++i) {
                    if (kd[i] == kd[i + 1]) { dup_pos = i; break; }
                }
                if (dup_pos >= 0) break;
                r_lo = r_hi;

                l_lo = std::max(1, l_hi - band + 1);
                for (int i = l_lo; i <= l_hi; ++i) {
                    if (kd[i] == kd[i - 1]) { dup_pos = i; break; }
                }
                if (dup_pos >= 0) break;
                l_hi = l_lo - 1;
            }
        }

        int write_pos;
        if (dup_pos < ins) {
            int shift_count = ins - 1 - dup_pos;
            if (shift_count > 0) {
                std::memmove(kd + dup_pos, kd + dup_pos + 1, shift_count * sizeof(K));
                std::memmove(vd + dup_pos, vd + dup_pos + 1, shift_count * sizeof(VST));
            }
            write_pos = ins - 1;
        } else {
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

    template<typename K>
    static void erase_create_dup(
            K* kd, VST* vd, int total, int idx,
            K suffix, ALLOC& alloc) {

        int first = idx;
        while (first > 0 && kd[first - 1] == suffix) --first;

        if constexpr (!VT::IS_INLINE)
            VT::destroy(vd[first], alloc);

        K   neighbor_key;
        VST neighbor_val;
        if (first > 0) {
            neighbor_key = kd[first - 1];
            neighbor_val = vd[first - 1];
        } else {
            neighbor_key = kd[idx + 1];
            neighbor_val = vd[idx + 1];
        }

        for (int i = first; i <= idx; ++i) {
            kd[i] = neighbor_key;
            vd[i] = neighbor_val;
        }
    }

    // ==================================================================
    // Seed: distribute dups evenly among real entries
    // ==================================================================

    template<typename K>
    static void seed_from_real(uint64_t* node,
                               const K* real_keys,
                               const VST* real_vals,
                               uint16_t n_entries, uint16_t total) {
        K*   kd = keys<K>(node);
        VST* vd = vals<K>(node, total);

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
            kd[write] = kd[write - 1];
            vd[write] = vd[write - 1];
            write++;
            placed++;
        }

        int remaining = n_entries - src;
        if (remaining > 0) {
            std::memcpy(kd + write, real_keys + src, remaining * sizeof(K));
            std::memcpy(vd + write, real_vals + src, remaining * sizeof(VST));
        }
    }

    // ==================================================================
    // Seed with insert: dedup source, merge new entry, seed dups
    // ==================================================================

    template<typename K>
    static void seed_with_insert(uint64_t* node,
                                  const K* old_keys,
                                  const VST* old_vals,
                                  uint16_t old_entries,
                                  K new_suffix, VST new_val,
                                  uint16_t n_entries,
                                  uint16_t new_total) {
        auto tmp_k = std::make_unique<K[]>(n_entries);
        auto tmp_v = std::make_unique<VST[]>(n_entries);

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

        seed_from_real<K>(node, tmp_k.get(), tmp_v.get(), n_entries, new_total);
    }

    // ==================================================================
    // Seed with skip: dedup source, skip one erased key, seed dups
    // ==================================================================

    template<typename K>
    static void seed_with_skip(uint64_t* node,
                                const K* src_keys,
                                const VST* src_vals,
                                uint16_t src_total,
                                K skip_suffix,
                                uint16_t n_entries,
                                uint16_t new_total,
                                ALLOC& alloc) {
        auto tmp_k = std::make_unique<K[]>(n_entries);
        auto tmp_v = std::make_unique<VST[]>(n_entries);

        bool skipped = false;
        int ri = 0;
        for (int i = 0; i < src_total; ++i) {
            if (i > 0 && src_keys[i] == src_keys[i - 1]) continue;

            if (!skipped && src_keys[i] == skip_suffix) {
                skipped = true;
                if constexpr (!VT::IS_INLINE)
                    VT::destroy(src_vals[i], alloc);
                continue;
            }

            tmp_k[ri] = src_keys[i];
            tmp_v[ri] = src_vals[i];
            ri++;
        }

        seed_from_real<K>(node, tmp_k.get(), tmp_v.get(), n_entries, new_total);
    }
};

} // namespace gteitelbaum

#endif // KNTRIE_COMPACT_HPP
