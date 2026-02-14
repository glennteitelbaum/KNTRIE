#ifndef KNTRIE_COMPACT_HPP
#define KNTRIE_COMPACT_HPP

#include "kntrie_support.hpp"

#include <cstdint>
#include <cstring>
#include <algorithm>

namespace gteitelbaum {

// ==========================================================================
// AdaptiveSearch  (branchless binary search for power-of-2 counts)
//
// Every compact leaf has power-of-2 total slots, so the search is a
// pure halving loop — no alignment preamble, no branches.
// ==========================================================================

template<typename K>
struct adaptive_search {
    // Returns index if found (>=0), or -1 if not found.
    // count must be a power of 2.
    static int search(const K* keys, int count, K key) noexcept {
        const K* base = keys;
        for (int step = count >> 1; step > 0; step >>= 1)
            base = (base[step] <= key) ? base + step : base;
        return (*base == key) ? static_cast<int>(base - keys) : -1;
    }

    // Returns index if found (>=0), or -(insertion_point + 1) if not found.
    // count must be a power of 2.
    static int search_insert(const K* keys, int count, K key) noexcept {
        const K* base = keys;
        for (int step = count >> 1; step > 0; step >>= 1)
            base = (base[step] <= key) ? base + step : base;
        if (*base == key) return static_cast<int>(base - keys);
        int pos = static_cast<int>(base - keys);
        if (*base < key) pos++;
        return -(pos + 1);
    }
};

// ==========================================================================
// compact_ops  -- compact leaf operations templated on K type
//
// Layout: [header (2 u64)][sorted_keys (aligned)][values (aligned)]
//
// Slot count is always power-of-2 (via bit_ceil). Extra slots are
// filled with evenly-spaced duplicates of neighboring keys.
// Insert consumes the nearest dup; erase creates a new dup.
//
// K = suffix type (uint16_t, uint32_t, uint64_t)
// ==========================================================================

template<typename K, typename VALUE, typename ALLOC>
struct compact_ops {
    using VT   = value_traits<VALUE, ALLOC>;
    using VST  = typename VT::slot_type;

    // Suffix type constant for this K
    static constexpr uint8_t STYPE =
        (sizeof(K) == 2) ? 1 :
        (sizeof(K) == 4) ? 2 : 3;

    // --- power-of-2 slot count for a given entry count ---

    static constexpr uint16_t slots_for(uint16_t entries) noexcept {
        if (entries <= 1) return 1;
        return static_cast<uint16_t>(std::min<uint32_t>(
            std::bit_ceil(static_cast<unsigned>(entries)), COMPACT_MAX));
    }

    // --- exact u64 size for a given slot count ---

    static constexpr size_t size_u64(size_t slots) noexcept {
        size_t kb = slots * sizeof(K);
        kb = (kb + 7) & ~size_t{7};
        size_t vb = slots * sizeof(VST);
        vb = (vb + 7) & ~size_t{7};
        return HEADER_U64 + (kb + vb) / 8;
    }

    // ==================================================================
    // Find
    // ==================================================================

    static const VALUE* find(const uint64_t* node, node_header h,
                             K suffix) noexcept {
        uint16_t entries = h.entries();
        if (entries == 0) [[unlikely]] return nullptr;
        uint16_t ts = slots_for(entries);
        int idx = adaptive_search<K>::search(keys_(node), static_cast<int>(ts), suffix);
        if (idx < 0) [[unlikely]] return nullptr;
        return VT::as_ptr(vals_(node, ts)[idx]);
    }

    // ==================================================================
    // Factory: build from pre-sorted working arrays
    // ==================================================================

    static uint64_t* make_leaf(const K* sorted_keys, const VST* values,
                               uint32_t count, uint8_t skip,
                               const uint8_t* prefix, ALLOC& alloc) {
        uint16_t ts = slots_for(static_cast<uint16_t>(count));
        size_t au64 = size_u64(ts);
        uint64_t* node = alloc_node(alloc, au64);
        auto* h = get_header(node);
        h->set_entries(static_cast<uint16_t>(count));
        h->set_alloc_u64(static_cast<uint16_t>(au64));
        h->set_suffix_type(STYPE);
        h->set_skip(skip);
        if (skip > 0) h->set_prefix(prefix, skip);

        if (count > 0)
            seed_from_real_(node, sorted_keys, values,
                            static_cast<uint16_t>(count), ts);
        return node;
    }

    // ==================================================================
    // Iterate entries: cb(K suffix, VST value_slot) -- skips dups
    // ==================================================================

    template<typename Fn>
    static void for_each(const uint64_t* node, const node_header* h, Fn&& cb) {
        uint16_t entries = h->entries();
        if (entries == 0) return;
        uint16_t ts = slots_for(entries);
        const K*   kd = keys_(node);
        const VST* vd = vals_(node, ts);
        for (uint16_t i = 0; i < ts; ++i) {
            if (i > 0 && kd[i] == kd[i - 1]) continue;
            cb(kd[i], vd[i]);
        }
    }

    // ==================================================================
    // Destroy all values + deallocate node
    // ==================================================================

    static void destroy_and_dealloc(uint64_t* node, ALLOC& alloc) {
        auto* h = get_header(node);
        if constexpr (!VT::IS_INLINE) {
            uint16_t entries = h->entries();
            if (entries > 0) {
                uint16_t ts = slots_for(entries);
                const K* kd = keys_(node);
                VST* vd = vals_mut_(node, ts);
                for (uint16_t i = 0; i < ts; ++i) {
                    if (i > 0 && kd[i] == kd[i - 1]) continue;
                    VT::destroy(vd[i], alloc);
                }
            }
        }
        dealloc_node(alloc, node, h->alloc_u64());
    }

    // ==================================================================
    // Insert
    //
    // INSERT: allow inserting new keys
    // ASSIGN: allow overwriting existing values
    // ==================================================================

    template<bool INSERT = true, bool ASSIGN = true>
    requires (INSERT || ASSIGN)
    static insert_result_t insert(uint64_t* node, node_header* h,
                                  K suffix, VST value, ALLOC& alloc) {
        uint16_t entries = h->entries();
        uint16_t ts = slots_for(entries);
        K*   kd = keys_(node);
        VST* vd = vals_mut_(node, ts);

        int idx = adaptive_search<K>::search_insert(
            kd, static_cast<int>(ts), suffix);

        // Key exists
        if (idx >= 0) {
            if constexpr (ASSIGN) {
                VT::destroy(vd[idx], alloc);
                VT::write_slot(&vd[idx], value);
                // Update all dup copies too
                for (int i = idx - 1; i >= 0 && kd[i] == suffix; --i)
                    VT::write_slot(&vd[i], value);
            }
            return {node, false, false};
        }
        if constexpr (!INSERT) return {node, false, false};

        if (entries >= COMPACT_MAX)
            return {node, false, true};  // needs_split

        int ins = -(idx + 1);
        uint16_t dups = ts - entries;

        // Dups available: consume one in-place
        if (dups > 0) {
            insert_consume_dup_(kd, vd, static_cast<int>(ts),
                                ins, entries, suffix, value);
            h->set_entries(entries + 1);
            return {node, true, false};
        }

        // No dups: realloc to next power-of-2 slot count
        uint16_t new_entries = entries + 1;
        uint16_t new_ts = slots_for(new_entries);
        size_t au64 = size_u64(new_ts);
        uint64_t* nn = alloc_node(alloc, au64);
        auto* nh = get_header(nn);
        *nh = *h;
        nh->set_entries(new_entries);
        nh->set_alloc_u64(static_cast<uint16_t>(au64));

        // Build sorted real entries with new key, then seed
        auto tmp_k = std::make_unique<K[]>(new_entries);
        auto tmp_v = std::make_unique<VST[]>(new_entries);
        dedup_into_(kd, vd, ts, entries, tmp_k.get(), tmp_v.get());
        // Insert new key at sorted position
        int real_ins = 0;
        while (real_ins < entries && tmp_k[real_ins] < suffix) ++real_ins;
        std::memmove(tmp_k.get() + real_ins + 1, tmp_k.get() + real_ins,
                     (entries - real_ins) * sizeof(K));
        std::memmove(tmp_v.get() + real_ins + 1, tmp_v.get() + real_ins,
                     (entries - real_ins) * sizeof(VST));
        tmp_k[real_ins] = suffix;
        VT::write_slot(&tmp_v[real_ins], value);

        seed_from_real_(nn, tmp_k.get(), tmp_v.get(), new_entries, new_ts);

        dealloc_node(alloc, node, h->alloc_u64());
        return {nn, true, false};
    }

    // ==================================================================
    // Erase
    // ==================================================================

    static erase_result_t erase(uint64_t* node, node_header* h,
                                K suffix, ALLOC& alloc) {
        uint16_t entries = h->entries();
        uint16_t ts = slots_for(entries);
        K*   kd = keys_(node);
        VST* vd = vals_mut_(node, ts);

        int idx = adaptive_search<K>::search(
            kd, static_cast<int>(ts), suffix);
        if (idx < 0) return {node, false};

        uint16_t nc = entries - 1;

        // Last entry
        if (nc == 0) {
            if constexpr (!VT::IS_INLINE)
                VT::destroy(vd[idx], alloc);
            dealloc_node(alloc, node, h->alloc_u64());
            return {nullptr, true};
        }

        // Shrink check: if entries-1 fits in half the slots, realloc
        uint16_t new_ts = slots_for(nc);
        if (new_ts < ts) {
            // Realloc + re-seed at smaller power-of-2
            size_t au64 = size_u64(new_ts);
            uint64_t* nn = alloc_node(alloc, au64);
            auto* nh = get_header(nn);
            *nh = *h;
            nh->set_entries(nc);
            nh->set_alloc_u64(static_cast<uint16_t>(au64));

            // Dedup old, skip erased key, seed into new
            auto tmp_k = std::make_unique<K[]>(nc);
            auto tmp_v = std::make_unique<VST[]>(nc);
            dedup_skip_into_(kd, vd, ts, suffix, tmp_k.get(), tmp_v.get(), alloc);
            seed_from_real_(nn, tmp_k.get(), tmp_v.get(), nc, new_ts);

            dealloc_node(alloc, node, h->alloc_u64());
            return {nn, true};
        }

        // In-place: convert erased entry's run to neighbor dups
        erase_create_dup_(kd, vd, static_cast<int>(ts), idx, suffix, alloc);
        h->set_entries(nc);
        return {node, true};
    }

private:
    // ==================================================================
    // Layout helpers
    // ==================================================================

    static K* keys_(uint64_t* node) noexcept {
        return reinterpret_cast<K*>(node + HEADER_U64);
    }
    static const K* keys_(const uint64_t* node) noexcept {
        return reinterpret_cast<const K*>(node + HEADER_U64);
    }

    static VST* vals_mut_(uint64_t* node, size_t total) noexcept {
        size_t kb = total * sizeof(K);
        kb = (kb + 7) & ~size_t{7};
        return reinterpret_cast<VST*>(
            reinterpret_cast<char*>(node + HEADER_U64) + kb);
    }
    static const VST* vals_(const uint64_t* node, size_t total) noexcept {
        size_t kb = total * sizeof(K);
        kb = (kb + 7) & ~size_t{7};
        return reinterpret_cast<const VST*>(
            reinterpret_cast<const char*>(node + HEADER_U64) + kb);
    }

    // ==================================================================
    // Dedup: extract real entries from dup-seeded array
    // ==================================================================

    static void dedup_into_(const K* kd, const VST* vd, uint16_t ts,
                             uint16_t entries,
                             K* out_k, VST* out_v) {
        int wi = 0;
        for (int i = 0; i < ts; ++i) {
            if (i > 0 && kd[i] == kd[i - 1]) continue;
            out_k[wi] = kd[i];
            out_v[wi] = vd[i];
            wi++;
        }
    }

    // Dedup + skip one key, writing into output arrays
    static void dedup_skip_into_(const K* kd, const VST* vd, uint16_t ts,
                                  K skip_suffix,
                                  K* out_k, VST* out_v, ALLOC& alloc) {
        bool skipped = false;
        int wi = 0;
        for (int i = 0; i < ts; ++i) {
            if (i > 0 && kd[i] == kd[i - 1]) continue;
            if (!skipped && kd[i] == skip_suffix) {
                skipped = true;
                if constexpr (!VT::IS_INLINE)
                    VT::destroy(vd[i], alloc);
                continue;
            }
            out_k[wi] = kd[i];
            out_v[wi] = vd[i];
            wi++;
        }
    }

    // ==================================================================
    // Dup helpers
    // ==================================================================

    static void insert_consume_dup_(
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

    static void erase_create_dup_(
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

    static void seed_from_real_(uint64_t* node,
                                const K* real_keys, const VST* real_vals,
                                uint16_t n_entries, uint16_t total) {
        K*   kd = keys_(node);
        VST* vd = vals_mut_(node, total);

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
};

} // namespace gteitelbaum

#endif // KNTRIE_COMPACT_HPP
