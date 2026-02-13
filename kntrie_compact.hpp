#ifndef KNTRIE_COMPACT_HPP
#define KNTRIE_COMPACT_HPP

#include "kntrie_support.hpp"

#include <cstdint>
#include <cstring>
#include <algorithm>

namespace gteitelbaum {

// ==========================================================================
// JumpSearch  (stride 256 -> 16 -> 1)
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
// compact_ops  -- compact leaf operations templated on K type
//
// Layout: [header (2 u64)][sorted_keys (aligned)][values (aligned)]
//
// Slot strategy:
//   Below 256 entries: unused slots are empty (no dups).
//     Insert: memmove right into unused tail.
//     Erase: memmove left.
//   Above 256 entries: spread dups evenly.
//     Insert: consume nearest dup.
//     Erase: convert to neighbor dup.
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

    // DUP_THRESHOLD: below this, use simple unused-slot strategy.
    static constexpr uint16_t DUP_THRESHOLD = 256;

    // --- exact needed size for `count` entries (no padding, no dups) ---

    static constexpr size_t size_u64(size_t count) noexcept {
        size_t kb = count * sizeof(K);
        kb = (kb + 7) & ~size_t{7};
        size_t vb = count * sizeof(VST);
        vb = (vb + 7) & ~size_t{7};
        return HEADER_U64 + (kb + vb) / 8;
    }

    // --- total physical slots for a given alloc ---

    static uint16_t total_slots(uint16_t alloc_u64) noexcept {
        return slot_table<K, VST>::max_slots(alloc_u64);
    }

    static uint16_t total_slots(const node_header* h) noexcept {
        return total_slots(h->alloc_u64());
    }

    // ==================================================================
    // Find
    // ==================================================================

    static const VALUE* find(const uint64_t* node, node_header h,
                             K suffix) noexcept {
        uint16_t entries = h.entries();
        if (entries == 0) [[unlikely]] return nullptr;
        uint16_t ts = total_slots(&h);
        int search_count = (entries < DUP_THRESHOLD) ?
            static_cast<int>(entries) : static_cast<int>(ts);
        int idx = jump_search<K>::search(keys_(node), search_count, suffix);
        if (idx < 0) [[unlikely]] return nullptr;
        return VT::as_ptr(vals_(node, ts)[idx]);
    }

    // ==================================================================
    // Factory: build from pre-sorted working arrays
    // ==================================================================

    static uint64_t* make_leaf(const K* sorted_keys, const VST* values,
                               uint32_t count, uint8_t skip,
                               const uint8_t* prefix, ALLOC& alloc) {
        size_t needed = size_u64(count);
        size_t au64 = round_up_u64(needed);
        uint64_t* node = alloc_node(alloc, au64);
        auto* h = get_header(node);
        h->set_entries(static_cast<uint16_t>(count));
        h->set_alloc_u64(static_cast<uint16_t>(au64));
        h->set_suffix_type(STYPE);
        h->set_skip(skip);
        if (skip > 0) h->set_prefix(prefix, skip);

        uint16_t ts = total_slots(h);
        if (count > 0) {
            if (count >= DUP_THRESHOLD && ts > count) {
                // Seed dups into the extra space
                seed_from_real_(node, sorted_keys, values, count, ts);
            } else {
                // No dups: copy directly (entries contiguous at front)
                std::memcpy(keys_(node), sorted_keys, count * sizeof(K));
                std::memcpy(vals_mut_(node, ts), values, count * sizeof(VST));
            }
        }
        return node;
    }

    // ==================================================================
    // Iterate entries: cb(K suffix, VST value_slot) -- skips dups
    // ==================================================================

    template<typename Fn>
    static void for_each(const uint64_t* node, const node_header* h, Fn&& cb) {
        uint16_t entries = h->entries();
        uint16_t ts = total_slots(h);

        if (entries < DUP_THRESHOLD) {
            // No dups: entries are contiguous at front
            const K*   kd = keys_(node);
            const VST* vd = vals_(node, ts);
            for (uint16_t i = 0; i < entries; ++i)
                cb(kd[i], vd[i]);
        } else {
            // May have dups: iterate total, skip adjacent duplicates
            const K*   kd = keys_(node);
            const VST* vd = vals_(node, ts);
            for (uint16_t i = 0; i < ts; ++i) {
                if (i > 0 && kd[i] == kd[i - 1]) continue;
                cb(kd[i], vd[i]);
            }
        }
    }

    // ==================================================================
    // Destroy all values + deallocate node
    // ==================================================================

    static void destroy_and_dealloc(uint64_t* node, ALLOC& alloc) {
        auto* h = get_header(node);
        if constexpr (!VT::IS_INLINE) {
            uint16_t entries = h->entries();
            uint16_t ts = total_slots(h);
            const K* kd = keys_(node);
            VST* vd = vals_mut_(node, ts);
            if (entries < DUP_THRESHOLD) {
                for (uint16_t i = 0; i < entries; ++i)
                    VT::destroy(vd[i], alloc);
            } else {
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
        uint16_t ts = total_slots(h);

        if (entries < DUP_THRESHOLD)
            return insert_simple_<INSERT, ASSIGN>(node, h, entries, ts, suffix, value, alloc);
        else
            return insert_dup_<INSERT, ASSIGN>(node, h, entries, ts, suffix, value, alloc);
    }

    // ==================================================================
    // Erase
    // ==================================================================

    static erase_result_t erase(uint64_t* node, node_header* h,
                                K suffix, ALLOC& alloc) {
        uint16_t entries = h->entries();
        uint16_t ts = total_slots(h);

        if (entries < DUP_THRESHOLD)
            return erase_simple_(node, h, entries, ts, suffix, alloc);
        else
            return erase_dup_(node, h, entries, ts, suffix, alloc);
    }

private:
    // --- layout helpers ---

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
    // Simple strategy (below DUP_THRESHOLD): entries contiguous at front
    // ==================================================================

    template<bool INSERT, bool ASSIGN>
    requires (INSERT || ASSIGN)
    static insert_result_t insert_simple_(
            uint64_t* node, node_header* h,
            uint16_t entries, uint16_t ts,
            K suffix, VST value, ALLOC& alloc) {
        K*   kd = keys_(node);
        VST* vd = vals_mut_(node, ts);

        int idx = jump_search<K>::search_insert(kd, static_cast<int>(entries), suffix);

        // Key exists
        if (idx >= 0) {
            if constexpr (ASSIGN) {
                VT::destroy(vd[idx], alloc);
                VT::write_slot(&vd[idx], value);
            }
            return {node, false, false};
        }
        if constexpr (!INSERT) return {node, false, false};

        int ins = -(idx + 1);

        // Check if we have unused slots
        if (entries < ts) {
            if (entries + 1 >= DUP_THRESHOLD) {
                // Crossing into dup mode: insert then seed dups in-place
                int tail = static_cast<int>(entries) - ins;
                if (tail > 0) {
                    std::memmove(kd + ins + 1, kd + ins, tail * sizeof(K));
                    std::memmove(vd + ins + 1, vd + ins, tail * sizeof(VST));
                }
                kd[ins] = suffix;
                VT::write_slot(&vd[ins], value);
                uint16_t ne = entries + 1;
                h->set_entries(ne);
                // Now ne contiguous entries at front, ts-ne empty slots at end.
                // Spread dups backwards so we don't overwrite unread data.
                spread_dups_backward_(kd, vd, ne, ts);
                return {node, true, false};
            }
            // memmove right to make room
            // memmove right to make room
            int tail = static_cast<int>(entries) - ins;
            if (tail > 0) {
                std::memmove(kd + ins + 1, kd + ins, tail * sizeof(K));
                std::memmove(vd + ins + 1, vd + ins, tail * sizeof(VST));
            }
            kd[ins] = suffix;
            VT::write_slot(&vd[ins], value);
            h->set_entries(entries + 1);
            return {node, true, false};
        }

        // No unused slots: need realloc
        if (entries >= COMPACT_MAX)
            return {node, false, true};  // needs_split

        uint16_t new_entries = entries + 1;
        size_t needed = size_u64(new_entries);
        size_t au64 = round_up_u64(needed);
        uint64_t* nn = alloc_node(alloc, au64);
        auto* nh = get_header(nn);
        *nh = *h;
        nh->set_entries(new_entries);
        nh->set_alloc_u64(static_cast<uint16_t>(au64));

        uint16_t new_ts = total_slots(nh);
        K*   nk = keys_(nn);
        VST* nv = vals_mut_(nn, new_ts);

        // If crossing into dup territory, seed with dups
        if (new_entries >= DUP_THRESHOLD && new_ts > new_entries) {
            auto tmp_k = std::make_unique<K[]>(new_entries);
            auto tmp_v = std::make_unique<VST[]>(new_entries);
            std::memcpy(tmp_k.get(), kd, ins * sizeof(K));
            std::memcpy(tmp_v.get(), vd, ins * sizeof(VST));
            tmp_k[ins] = suffix;
            VT::write_slot(&tmp_v[ins], value);
            std::memcpy(tmp_k.get() + ins + 1, kd + ins, (entries - ins) * sizeof(K));
            std::memcpy(tmp_v.get() + ins + 1, vd + ins, (entries - ins) * sizeof(VST));
            seed_from_real_(nn, tmp_k.get(), tmp_v.get(), new_entries, new_ts);
        } else {
            std::memcpy(nk, kd, ins * sizeof(K));
            nk[ins] = suffix;
            std::memcpy(nk + ins + 1, kd + ins, (entries - ins) * sizeof(K));

            std::memcpy(nv, vd, ins * sizeof(VST));
            VT::write_slot(&nv[ins], value);
            std::memcpy(nv + ins + 1, vd + ins, (entries - ins) * sizeof(VST));
        }

        dealloc_node(alloc, node, h->alloc_u64());
        return {nn, true, false};
    }

    static erase_result_t erase_simple_(
            uint64_t* node, node_header* h,
            uint16_t entries, uint16_t ts,
            K suffix, ALLOC& alloc) {
        K*   kd = keys_(node);
        VST* vd = vals_mut_(node, ts);

        int idx = jump_search<K>::search(kd, static_cast<int>(entries), suffix);
        if (idx < 0) return {node, false};

        uint16_t nc = entries - 1;

        // Last entry
        if (nc == 0) {
            if constexpr (!VT::IS_INLINE)
                VT::destroy(vd[idx], alloc);
            dealloc_node(alloc, node, h->alloc_u64());
            return {nullptr, true};
        }

        size_t needed = size_u64(nc);

        // Should shrink?
        if (should_shrink_u64(h->alloc_u64(), needed)) {
            size_t au64 = round_up_u64(needed);
            uint64_t* nn = alloc_node(alloc, au64);
            auto* nh = get_header(nn);
            *nh = *h;
            nh->set_entries(nc);
            nh->set_alloc_u64(static_cast<uint16_t>(au64));

            uint16_t new_ts = total_slots(nh);
            K*   nk = keys_(nn);
            VST* nv = vals_mut_(nn, new_ts);

            std::memcpy(nk, kd, idx * sizeof(K));
            std::memcpy(nk + idx, kd + idx + 1, (nc - idx) * sizeof(K));
            std::memcpy(nv, vd, idx * sizeof(VST));
            std::memcpy(nv + idx, vd + idx + 1, (nc - idx) * sizeof(VST));

            if constexpr (!VT::IS_INLINE)
                VT::destroy(vd[idx], alloc);
            dealloc_node(alloc, node, h->alloc_u64());
            return {nn, true};
        }

        // In-place: memmove left
        if constexpr (!VT::IS_INLINE)
            VT::destroy(vd[idx], alloc);
        int tail = static_cast<int>(entries) - idx - 1;
        if (tail > 0) {
            std::memmove(kd + idx, kd + idx + 1, tail * sizeof(K));
            std::memmove(vd + idx, vd + idx + 1, tail * sizeof(VST));
        }
        h->set_entries(nc);
        return {node, true};
    }

    // ==================================================================
    // Dup strategy (at or above DUP_THRESHOLD): dups spread evenly
    // ==================================================================

    template<bool INSERT, bool ASSIGN>
    requires (INSERT || ASSIGN)
    static insert_result_t insert_dup_(
            uint64_t* node, node_header* h,
            uint16_t entries, uint16_t ts,
            K suffix, VST value, ALLOC& alloc) {
        K*   kd = keys_(node);
        VST* vd = vals_mut_(node, ts);

        int idx = jump_search<K>::search_insert(kd, static_cast<int>(ts), suffix);

        // Key exists
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
        uint16_t dups = ts - entries;

        // Dups available: consume one in-place
        if (dups > 0) {
            insert_consume_dup_(kd, vd, ts, ins, entries, suffix, value);
            h->set_entries(entries + 1);
            return {node, true, false};
        }

        // No dups: need realloc
        if (entries >= COMPACT_MAX)
            return {node, false, true};  // needs_split

        uint16_t new_entries = entries + 1;
        size_t needed = size_u64(new_entries);
        size_t au64 = round_up_u64(needed);
        uint64_t* nn = alloc_node(alloc, au64);
        auto* nh = get_header(nn);
        *nh = *h;
        nh->set_entries(new_entries);
        nh->set_alloc_u64(static_cast<uint16_t>(au64));
        uint16_t new_ts = total_slots(nh);

        seed_with_insert_(nn, kd, vd, entries, suffix, value,
                          new_entries, new_ts);

        dealloc_node(alloc, node, h->alloc_u64());
        return {nn, true, false};
    }

    static erase_result_t erase_dup_(
            uint64_t* node, node_header* h,
            uint16_t entries, uint16_t ts,
            K suffix, ALLOC& alloc) {
        K*   kd = keys_(node);
        VST* vd = vals_mut_(node, ts);

        int idx = jump_search<K>::search(kd, static_cast<int>(ts), suffix);
        if (idx < 0) return {node, false};

        uint16_t nc = entries - 1;

        // Last entry
        if (nc == 0) {
            if constexpr (!VT::IS_INLINE)
                VT::destroy(vd[idx], alloc);
            dealloc_node(alloc, node, h->alloc_u64());
            return {nullptr, true};
        }

        size_t needed = size_u64(nc);

        // Should shrink
        if (should_shrink_u64(h->alloc_u64(), needed)) {
            size_t au64 = round_up_u64(needed);
            uint64_t* nn = alloc_node(alloc, au64);
            auto* nh = get_header(nn);
            *nh = *h;
            nh->set_entries(nc);
            nh->set_alloc_u64(static_cast<uint16_t>(au64));
            uint16_t new_ts = total_slots(nh);

            if (nc < DUP_THRESHOLD) {
                dedup_skip_contiguous_(nn, nc, kd, vd, ts, suffix, alloc);
            } else {
                seed_with_skip_(nn, kd, vd, ts, suffix, nc, new_ts, alloc);
            }
            dealloc_node(alloc, node, h->alloc_u64());
            return {nn, true};
        }

        // Crossing threshold from dup→simple: dedup in-place
        if (nc < DUP_THRESHOLD) {
            dedup_skip_inplace_(kd, vd, ts, suffix, alloc);
            h->set_entries(nc);
            return {node, true};
        }

        // In-place O(1): convert run to neighbor dups
        erase_create_dup_(kd, vd, ts, idx, suffix, alloc);
        h->set_entries(nc);
        return {node, true};
    }

    // ==================================================================
    // Spread dups backward: ne entries at front, total slots available.
    // Works right-to-left so we never overwrite unread source data.
    // ==================================================================

    static void spread_dups_backward_(K* kd, VST* vd,
                                       uint16_t ne, uint16_t total) {
        uint16_t n_dups = total - ne;
        if (n_dups == 0) return;
        uint16_t stride = ne / (n_dups + 1);
        uint16_t remainder = ne % (n_dups + 1);
        // Last (n_dups+1-remainder) groups get `stride` entries,
        // first `remainder` groups get `stride+1`.
        // Work backward: place last group first.
        int src = ne - 1;
        int dst = total - 1;
        int placed = 0;  // dups placed so far (from the right)
        int groups_from_right = 0;
        while (placed < n_dups) {
            // Group size: last groups get `stride`, first get `stride+1`
            // Groups from right: groups_from_right counts 0,1,...
            // Total groups = n_dups+1. Group index from left = n_dups - groups_from_right
            int group_idx_from_left = n_dups - groups_from_right;
            int chunk = stride + (group_idx_from_left < remainder ? 1 : 0);
            // Copy chunk entries right-to-left
            for (int j = 0; j < chunk; ++j) {
                kd[dst] = kd[src];
                vd[dst] = vd[src];
                dst--; src--;
            }
            // Place dup (copy of entry just written, which is at dst+1)
            kd[dst] = kd[dst + 1];
            vd[dst] = vd[dst + 1];
            dst--;
            placed++;
            groups_from_right++;
        }
        // Remaining entries at front are already in place (src == dst)
    }

    // ==================================================================
    // Dedup + skip in-place: compact entries to front (for dup→simple crossing)
    // ==================================================================

    static void dedup_skip_inplace_(K* kd, VST* vd, uint16_t total,
                                     K skip_suffix, ALLOC& alloc) {
        bool skipped = false;
        int wi = 0;
        for (int i = 0; i < total; ++i) {
            if (i > 0 && kd[i] == kd[i - 1]) continue;  // skip dup
            if (!skipped && kd[i] == skip_suffix) {
                skipped = true;
                if constexpr (!VT::IS_INLINE)
                    VT::destroy(vd[i], alloc);
                continue;
            }
            if (wi != i) {
                kd[wi] = kd[i];
                vd[wi] = vd[i];
            }
            wi++;
        }
    }

    // ==================================================================
    // Dedup + skip, write contiguously (for shrink realloc at threshold)
    // ==================================================================

    static void dedup_skip_contiguous_(uint64_t* node, uint16_t nc,
                                        const K* src_keys, const VST* src_vals,
                                        uint16_t src_total, K skip_suffix,
                                        ALLOC& alloc) {
        uint16_t new_ts = total_slots(get_header(node));
        K*   dk = keys_(node);
        VST* dv = vals_mut_(node, new_ts);

        bool skipped = false;
        int wi = 0;
        for (int i = 0; i < src_total; ++i) {
            if (i > 0 && src_keys[i] == src_keys[i - 1]) continue;  // skip dup
            if (!skipped && src_keys[i] == skip_suffix) {
                skipped = true;
                if constexpr (!VT::IS_INLINE)
                    VT::destroy(src_vals[i], alloc);
                continue;
            }
            dk[wi] = src_keys[i];
            dv[wi] = src_vals[i];
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

    static void seed_with_insert_(uint64_t* node,
                                   const K* old_keys, const VST* old_vals,
                                   uint16_t old_entries,
                                   K new_suffix, VST new_val,
                                   uint16_t n_entries, uint16_t new_total) {
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

        seed_from_real_(node, tmp_k.get(), tmp_v.get(), n_entries, new_total);
    }

    static void seed_with_skip_(uint64_t* node,
                                 const K* src_keys, const VST* src_vals,
                                 uint16_t src_total, K skip_suffix,
                                 uint16_t n_entries, uint16_t new_total,
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

        seed_from_real_(node, tmp_k.get(), tmp_v.get(), n_entries, new_total);
    }
};

} // namespace gteitelbaum

#endif // KNTRIE_COMPACT_HPP