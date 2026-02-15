#ifndef KNTRIE_COMPACT_HPP
#define KNTRIE_COMPACT_HPP

#include "kntrie_support.hpp"

#include <cstdint>
#include <cstring>
#include <algorithm>

namespace gteitelbaum {

// ==========================================================================
// AdaptiveSearch  (branchless binary search for pow2 and 3/4 midpoint counts)
//
// Every compact leaf has power-of-2 or 3/4 midpoint total slots, so the search is a
// pure halving loop — no alignment preamble, no branches.
// ==========================================================================

template<typename K>
struct adaptive_search {
    // Pure cmov loop — returns pointer to candidate.
    // Caller checks *result == key.
    // count must be power of 2.
    static const K* find_base(const K* base, unsigned count, K key) noexcept {
        count >>= 1;
        do {
            base += (base[count] <= key) ? count : 0;
            count >>= 1;
        } while (count > 0);
        return base;
    }
};

// ==========================================================================
// compact_ops  -- compact leaf operations templated on K type
//
// Layout: [header (2 u64)][sorted_keys (aligned)][values (aligned)]
//
// Slot count is always power-of-2. Extra slots are
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

    // --- slot count: next power of 2 ---

    static constexpr uint16_t slots_for(unsigned entries) noexcept {
        unsigned p = std::bit_ceil(entries);
        return static_cast<uint16_t>(std::min(p, unsigned(COMPACT_MAX)));
    }

    // --- exact u64 size for a given slot count ---

    static constexpr size_t size_u64(size_t slots, size_t hu = HEADER_U64) noexcept {
        size_t kb = slots * sizeof(K);
        kb = (kb + 7) & ~size_t{7};
        size_t vb = slots * sizeof(VST);
        vb = (vb + 7) & ~size_t{7};
        return hu + (kb + vb) / 8;
    }

    // ==================================================================
    // Find
    // ==================================================================

    static const VALUE* find(const uint64_t* node, node_header h,
                             K suffix) noexcept {
        uint16_t entries = h.entries();
        uint16_t ts = slots_for(entries);
        const K* keys = keys_(node);
        const K* base = adaptive_search<K>::find_base(keys, ts, suffix);
        if (*base != suffix) [[unlikely]] return nullptr;
        return VT::as_ptr(vals_(node, ts)[base - keys]);
    }

    // ==================================================================
    // Factory: build from pre-sorted working arrays
    // ==================================================================

    static uint64_t* make_leaf(const K* sorted_keys, const VST* values,
                               unsigned count, uint8_t skip,
                               const uint8_t* prefix, ALLOC& alloc) {
        uint16_t ts = slots_for(count);
        size_t hu = 1 + (skip > 0);
        size_t au64 = size_u64(ts, hu);
        uint64_t* node = alloc_node(alloc, au64);
        auto* h = get_header(node);
        h->set_entries(count);
        h->set_alloc_u64(au64);
        h->set_suffix_type(STYPE);
        h->set_skip(skip);
        if (skip > 0) h->set_prefix(prefix, skip);

        if (count > 0)
            seed_from_real_(node, sorted_keys, values, count, ts);
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
        unsigned entries = h->entries();
        uint16_t ts = slots_for(entries);
        K*   kd = keys_(node);
        VST* vd = vals_mut_(node, ts);

        const K* base = adaptive_search<K>::find_base(
            kd, ts, suffix);

        // Key exists
        if (*base == suffix) {
            if constexpr (ASSIGN) {
                int idx = base - kd;
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

        int ins = (base - kd) + (*base < suffix);
        unsigned dups = ts - entries;

        // Dups available: consume one in-place
        if (dups > 0) {
            insert_consume_dup_(kd, vd, ts,
                                ins, entries, suffix, value);
            h->set_entries(entries + 1);
            return {node, true, false};
        }

        // No dups: realloc to next slot count
        unsigned new_entries = entries + 1;
        uint16_t new_ts = slots_for(new_entries);
        size_t hu = hdr_u64(node);
        size_t au64 = size_u64(new_ts, hu);
        uint64_t* nn = alloc_node(alloc, au64);
        auto* nh = get_header(nn);
        *nh = *h;
        if (h->is_skip()) nn[1] = reinterpret_cast<const uint64_t*>(h)[1];
        nh->set_entries(new_entries);
        nh->set_alloc_u64(au64);

        // Single-pass: dedup old + inject new key + seed dups
        seed_with_insert_(nn, kd, vd, ts, entries,
                          suffix, value, new_entries, new_ts);

        dealloc_node(alloc, node, h->alloc_u64());
        return {nn, true, false};
    }

    // ==================================================================
    // Erase
    // ==================================================================

    static erase_result_t erase(uint64_t* node, node_header* h,
                                K suffix, ALLOC& alloc) {
        unsigned entries = h->entries();
        uint16_t ts = slots_for(entries);
        K*   kd = keys_(node);
        VST* vd = vals_mut_(node, ts);

        const K* base = adaptive_search<K>::find_base(
            kd, ts, suffix);
        if (*base != suffix) return {node, false};
        unsigned idx = static_cast<unsigned>(base - kd);

        unsigned nc = entries - 1;

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
            // Realloc + re-seed at smaller slot count
            size_t hu = hdr_u64(node);
            size_t au64 = size_u64(new_ts, hu);
            uint64_t* nn = alloc_node(alloc, au64);
            auto* nh = get_header(nn);
            *nh = *h;
            if (h->is_skip()) nn[1] = reinterpret_cast<const uint64_t*>(h)[1];
            nh->set_entries(nc);
            nh->set_alloc_u64(au64);

            // Dedup old, skip erased key, seed into new
            auto tmp_k = std::make_unique<K[]>(nc);
            auto tmp_v = std::make_unique<VST[]>(nc);
            dedup_skip_into_(kd, vd, ts, suffix, tmp_k.get(), tmp_v.get(), alloc);
            seed_from_real_(nn, tmp_k.get(), tmp_v.get(), nc, new_ts);

            dealloc_node(alloc, node, h->alloc_u64());
            return {nn, true};
        }

        // In-place: convert erased entry's run to neighbor dups
        erase_create_dup_(kd, vd, ts, idx, suffix, alloc);
        h->set_entries(nc);
        return {node, true};
    }

private:
    // ==================================================================
    // Layout helpers
    // ==================================================================

    static K* keys_(uint64_t* node) noexcept {
        return reinterpret_cast<K*>(node + hdr_u64(node));
    }
    static const K* keys_(const uint64_t* node) noexcept {
        return reinterpret_cast<const K*>(node + hdr_u64(node));
    }

    static VST* vals_mut_(uint64_t* node, size_t total) noexcept {
        size_t kb = total * sizeof(K);
        kb = (kb + 7) & ~size_t{7};
        return reinterpret_cast<VST*>(
            reinterpret_cast<char*>(node + hdr_u64(node)) + kb);
    }
    static const VST* vals_(const uint64_t* node, size_t total) noexcept {
        size_t kb = total * sizeof(K);
        kb = (kb + 7) & ~size_t{7};
        return reinterpret_cast<const VST*>(
            reinterpret_cast<const char*>(node + hdr_u64(node)) + kb);
    }

    // ==================================================================
    // Dedup + skip one key, writing into output arrays
    // ==================================================================

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
    // Single-pass: dedup old array + inject new key + seed dups into dest
    // No temp arrays. One read pass, one write pass.
    // ==================================================================

    static void seed_with_insert_(uint64_t* dst,
                                   const K* old_k, const VST* old_v,
                                   uint16_t old_ts, uint16_t old_entries,
                                   K new_suffix, VST new_val,
                                   uint16_t new_entries, uint16_t new_ts) {
        K*   dk = keys_(dst);
        VST* dv = vals_mut_(dst, new_ts);

        if (new_entries == new_ts) {
            // No dups needed — straight copy with insert
            bool inserted = false;
            int wi = 0;
            for (int i = 0; i < old_ts; ++i) {
                if (i > 0 && old_k[i] == old_k[i - 1]) continue;
                if (!inserted && new_suffix < old_k[i]) {
                    dk[wi] = new_suffix;
                    VT::write_slot(&dv[wi], new_val);
                    wi++;
                    inserted = true;
                }
                dk[wi] = old_k[i];
                dv[wi] = old_v[i];
                wi++;
            }
            if (!inserted) {
                dk[wi] = new_suffix;
                VT::write_slot(&dv[wi], new_val);
            }
            return;
        }

        // Dup seeding: distribute n_dups evenly among new_entries real entries
        uint16_t n_dups = new_ts - new_entries;
        uint16_t stride = new_entries / (n_dups + 1);
        uint16_t remainder = new_entries % (n_dups + 1);

        int wi = 0;         // write index into destination
        int real_out = 0;   // count of real entries emitted
        int placed = 0;     // dups placed so far
        int group_size = stride + (0 < remainder ? 1 : 0);
        int in_group = 0;   // entries written in current group

        bool inserted = false;
        for (int i = 0; i < old_ts; ++i) {
            if (i > 0 && old_k[i] == old_k[i - 1]) continue;  // skip dup

            // Inject new key at sorted position
            if (!inserted && new_suffix < old_k[i]) {
                dk[wi] = new_suffix;
                VT::write_slot(&dv[wi], new_val);
                wi++;
                real_out++;
                in_group++;
                inserted = true;

                // Check if group full → emit dup
                if (placed < n_dups && in_group >= group_size) {
                    dk[wi] = dk[wi - 1];
                    dv[wi] = dv[wi - 1];
                    wi++;
                    placed++;
                    in_group = 0;
                    group_size = stride + (placed < remainder ? 1 : 0);
                }
            }

            // Emit real entry from old array
            dk[wi] = old_k[i];
            dv[wi] = old_v[i];
            wi++;
            real_out++;
            in_group++;

            // Check if group full → emit dup
            if (placed < n_dups && in_group >= group_size) {
                dk[wi] = dk[wi - 1];
                dv[wi] = dv[wi - 1];
                wi++;
                placed++;
                in_group = 0;
                group_size = stride + (placed < remainder ? 1 : 0);
            }
        }

        // New key is largest — append at end
        if (!inserted) {
            dk[wi] = new_suffix;
            VT::write_slot(&dv[wi], new_val);
            wi++;
            real_out++;
            in_group++;

            if (placed < n_dups && in_group >= group_size) {
                dk[wi] = dk[wi - 1];
                dv[wi] = dv[wi - 1];
                wi++;
                placed++;
            }
        }
    }

    // ==================================================================
    // Dup helpers
    // ==================================================================

    static void insert_consume_dup_(
            K* kd, VST* vd, int total, int ins, unsigned entries,
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
            unsigned dups = total - entries;
            int band = entries / (dups + 1) + 1;
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
