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
        do {
            count >>= 1;
            base += (base[count] <= key) ? count : 0;
        } while (count > 1);
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
    using BLD  = builder<VALUE, VT::IS_TRIVIAL, ALLOC>;

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
        size_t vb;
        if constexpr (VT::IS_BOOL)
            vb = bool_slots::bytes_for(slots);
        else {
            vb = slots * sizeof(VST);
            vb = (vb + 7) & ~size_t{7};
        }
        return hu + (kb + vb) / 8;
    }

    // ==================================================================
    // Find
    // ==================================================================

    static const VALUE* find(const uint64_t* node, node_header_t h,
                             K suffix, size_t header_size) noexcept {
        unsigned ts = h.total_slots();
        const K* kd = keys(node, header_size);
        const K* base = adaptive_search<K>::find_base(kd, ts, suffix);
        if (*base != suffix) [[unlikely]] return nullptr;
        if constexpr (VT::IS_BOOL)
            return bool_vals(node, ts, header_size).ptr_at(base - kd);
        else
            return VT::as_ptr(vals(node, ts, header_size)[base - kd]);
    }

    // ==================================================================
    // Factory: build from pre-sorted working arrays
    // ==================================================================

    static uint64_t* make_leaf(const K* sorted_keys, const VST* values,
                               unsigned count, BLD& bld) {
        uint16_t ts = slots_for(count);
        constexpr size_t hu = 1;  // no skip prefix on fresh leaf
        size_t au64 = size_u64(ts, hu);
        uint64_t* node = bld.alloc_node(au64);
        auto* h = get_header(node);
        h->set_entries(count);
        h->set_alloc_u64(au64);
        h->set_total_slots(ts);

        if (count > 0) {
            if constexpr (VT::IS_BOOL) {
                bool tmp_v[ts];
                seed_from_real(keys(node, hu), tmp_v,
                               sorted_keys, values, count, ts);
                bool_vals_mut(node, ts, hu).pack_from(tmp_v, ts);
            } else {
                seed_from_real(keys(node, hu), vals_mut(node, ts, hu),
                               sorted_keys, values, count, ts);
            }
        }
        return node;
    }

    // ==================================================================
    // Iterate entries: cb(K suffix, VST value_slot) -- skips dups
    // ==================================================================

    template<typename Fn>
    static void for_each(const uint64_t* node, const node_header_t* h, Fn&& cb) {
        unsigned ts = h->total_slots();
        size_t hs = hdr_u64(node);
        const K* kd = keys(node, hs);
        if constexpr (VT::IS_BOOL) {
            auto bv = bool_vals(node, ts, hs);
            cb(kd[0], bv.get(0));
            for (unsigned i = 1; i < ts; ++i) {
                if (kd[i] == kd[i - 1]) continue;
                cb(kd[i], bv.get(i));
            }
        } else {
            const VST* vd = vals(node, ts, hs);
            cb(kd[0], vd[0]);
            for (unsigned i = 1; i < ts; ++i) {
                if (kd[i] == kd[i - 1]) continue;
                cb(kd[i], vd[i]);
            }
        }
    }

    // ==================================================================
    // Iterator helpers: first, last, next, prev
    // ==================================================================

    struct iter_leaf_result { K suffix; const VST* value; bool found; };

    static iter_leaf_result iter_first(const uint64_t* node,
                                        const node_header_t* h) noexcept {
        unsigned ts = h->total_slots();
        size_t hs = hdr_u64(node);
        const K* kd = keys(node, hs);
        if constexpr (VT::IS_BOOL)
            return {kd[0], bool_vals(node, ts, hs).ptr_at(0), true};
        else {
            const VST* vd = vals(node, ts, hs);
            return {kd[0], &vd[0], true};
        }
    }

    static iter_leaf_result iter_last(const uint64_t* node,
                                       const node_header_t* h) noexcept {
        unsigned ts = h->total_slots();
        size_t hs = hdr_u64(node);
        const K* kd = keys(node, hs);
        if constexpr (VT::IS_BOOL)
            return {kd[ts - 1], bool_vals(node, ts, hs).ptr_at(ts - 1), true};
        else {
            const VST* vd = vals(node, ts, hs);
            return {kd[ts - 1], &vd[ts - 1], true};
        }
    }

    // Smallest suffix > key
    static iter_leaf_result iter_next(const uint64_t* node,
                                       const node_header_t* h,
                                       K suffix) noexcept {
        unsigned ts = h->total_slots();
        size_t hs = hdr_u64(node);
        const K* kd = keys(node, hs);
        const K* base = adaptive_search<K>::find_base(kd, ts, suffix);
        unsigned pos = static_cast<unsigned>(base - kd) + (*base <= suffix);
        if (pos >= ts) return {0, nullptr, false};
        if constexpr (VT::IS_BOOL)
            return {kd[pos], bool_vals(node, ts, hs).ptr_at(pos), true};
        else {
            const VST* vd = vals(node, ts, hs);
            return {kd[pos], &vd[pos], true};
        }
    }

    // Largest suffix < key (key is known to exist)
    static iter_leaf_result iter_prev(const uint64_t* node,
                                       const node_header_t* h,
                                       K suffix) noexcept {
        unsigned ts = h->total_slots();
        size_t hs = hdr_u64(node);
        const K* kd = keys(node, hs);
        const K* base = adaptive_search<K>::find_base(kd, ts, suffix);
        unsigned pos = static_cast<unsigned>(base - kd);
        while (pos > 0 && kd[pos - 1] == suffix) --pos;
        if (pos == 0) return {0, nullptr, false};
        --pos;
        if constexpr (VT::IS_BOOL)
            return {kd[pos], bool_vals(node, ts, hs).ptr_at(pos), true};
        else {
            const VST* vd = vals(node, ts, hs);
            return {kd[pos], &vd[pos], true};
        }
    }

    // ==================================================================
    // Destroy all values + deallocate node
    // ==================================================================

    static void destroy_and_dealloc(uint64_t* node, BLD& bld) {
        auto* h = get_header(node);
        if constexpr (VT::HAS_DESTRUCTOR) {
            // C-type (pointer): dups share pointers — destroy unique only
            unsigned ts = h->total_slots();
            size_t hs = hdr_u64(node);
            const K* kd = keys(node, hs);
            VST* vd = vals_mut(node, ts, hs);
            bld.destroy_value(vd[0]);
            for (unsigned i = 1; i < ts; ++i) {
                if (kd[i] == kd[i - 1]) continue;
                bld.destroy_value(vd[i]);
            }
        }
        bld.dealloc_node(node, h->alloc_u64());
    }

    // ==================================================================
    // Insert
    //
    // INSERT: allow inserting new keys
    // ASSIGN: allow overwriting existing values
    // ==================================================================

    template<bool INSERT = true, bool ASSIGN = true>
    requires (INSERT || ASSIGN)
    static insert_result_t insert(uint64_t* node, node_header_t* h,
                                  K suffix, VST value, BLD& bld) {
        unsigned entries = h->entries();
        unsigned ts = h->total_slots();
        size_t hs = hdr_u64(node);
        K*   kd = keys(node, hs);

        const K* base = adaptive_search<K>::find_base(
            kd, ts, suffix);

        // Key exists
        if (*base == suffix) [[unlikely]] {
            if constexpr (ASSIGN) {
                int idx = base - kd;
                if constexpr (VT::IS_BOOL) {
                    auto bv = bool_vals_mut(node, ts, hs);
                    bv.set(idx, value);
                    for (int i = idx - 1; i >= 0 && kd[i] == suffix; --i)
                        bv.set(i, value);
                } else {
                    VST* vd = vals_mut(node, ts, hs);
                    bld.destroy_value(vd[idx]);
                    VT::init_slot(&vd[idx], value);
                    for (int i = idx - 1; i >= 0 && kd[i] == suffix; --i)
                        VT::write_slot(&vd[i], value);
                }
            }
            return {tag_leaf(node), false, false};
        }
        if constexpr (!INSERT) return {tag_leaf(node), false, false};

        if (entries >= COMPACT_MAX) [[unlikely]]
            return {tag_leaf(node), false, true};  // needs_split

        int ins = (base - kd) + (*base < suffix);
        unsigned dups = ts - entries;

        // Dups available: consume one in-place
        if (dups > 0) [[likely]] {
            if constexpr (VT::IS_BOOL) {
                auto bv = bool_vals_mut(node, ts, hs);
                // Find dup and shift keys (keys are always memmove'd)
                int dup_pos = find_dup_pos(kd, ts, ins, entries);
                int write_pos;
                if (dup_pos < ins) {
                    int sc = ins - 1 - dup_pos;
                    if (sc > 0) {
                        std::memmove(kd + dup_pos, kd + dup_pos + 1, sc * sizeof(K));
                        bv.shift_left_1(dup_pos + 1, sc);
                    }
                    write_pos = ins - 1;
                } else {
                    int sc = dup_pos - ins;
                    if (sc > 0) {
                        std::memmove(kd + ins + 1, kd + ins, sc * sizeof(K));
                        bv.shift_right_1(ins, sc);
                    }
                    write_pos = ins;
                }
                kd[write_pos] = suffix;
                bv.set(write_pos, value);
            } else {
                VST* vd = vals_mut(node, ts, hs);
                insert_consume_dup(kd, vd, ts, ins, entries, suffix, value);
            }
            h->set_entries(entries + 1);
            return {tag_leaf(node), true, false};
        }

        // No dups: realloc to next slot count
        unsigned new_entries = entries + 1;
        uint16_t new_ts = slots_for(new_entries);
        size_t au64 = size_u64(new_ts, hs);
        uint64_t* nn = bld.alloc_node(au64);
        auto* nh = get_header(nn);
        *nh = *h;
        if (h->is_skip()) nn[1] = reinterpret_cast<const uint64_t*>(h)[1];
        nh->set_entries(new_entries);
        nh->set_alloc_u64(au64);
        nh->set_total_slots(new_ts);

        if constexpr (VT::IS_BOOL) {
            bool old_v[ts];
            bool_vals(node, ts, hs).unpack_to(old_v, ts);
            bool new_v[new_ts];
            seed_with_insert(keys(nn, hs), new_v, kd, old_v, ts, entries,
                              suffix, value, new_entries, new_ts);
            bool_vals_mut(nn, new_ts, hs).pack_from(new_v, new_ts);
        } else {
            VST* vd = vals_mut(node, ts, hs);
            seed_with_insert(keys(nn, hs), vals_mut(nn, new_ts, hs),
                              kd, vd, ts, entries,
                              suffix, value, new_entries, new_ts);
        }

        bld.dealloc_node(node, h->alloc_u64());
        return {tag_leaf(nn), true, false};
    }

    // ==================================================================
    // Erase
    // ==================================================================

    static erase_result_t erase(uint64_t* node, node_header_t* h,
                                K suffix, BLD& bld) {
        unsigned entries = h->entries();
        unsigned ts = h->total_slots();
        size_t hs = hdr_u64(node);
        K*   kd = keys(node, hs);

        const K* base = adaptive_search<K>::find_base(
            kd, ts, suffix);
        if (*base != suffix) [[unlikely]] return {tag_leaf(node), false, 0};
        unsigned idx = static_cast<unsigned>(base - kd);

        unsigned nc = entries - 1;

        // Last entry
        if (nc == 0) [[unlikely]] {
            destroy_and_dealloc(node, bld);
            return {0, true, 0};
        }

        // Shrink check: if entries-1 fits in half the slots, realloc
        uint16_t new_ts = slots_for(nc);
        if (new_ts < ts) [[unlikely]] {
            // Realloc + re-seed at smaller slot count
            size_t au64 = size_u64(new_ts, hs);
            uint64_t* nn = bld.alloc_node(au64);
            auto* nh = get_header(nn);
            *nh = *h;
            if (h->is_skip()) nn[1] = reinterpret_cast<const uint64_t*>(h)[1];
            nh->set_entries(nc);
            nh->set_alloc_u64(au64);
            nh->set_total_slots(new_ts);

            auto tmp_k = std::make_unique<K[]>(nc);
            auto tmp_v = std::make_unique<VST[]>(nc);

            if constexpr (VT::IS_BOOL) {
                bool old_v[ts];
                bool_vals(node, ts, hs).unpack_to(old_v, ts);
                dedup_skip_into(kd, old_v, ts, suffix, tmp_k.get(), tmp_v.get(), bld);
                bool new_v[new_ts];
                seed_from_real(keys(nn, hs), new_v,
                               tmp_k.get(), tmp_v.get(), nc, new_ts);
                bool_vals_mut(nn, new_ts, hs).pack_from(new_v, new_ts);
            } else {
                VST* vd = vals_mut(node, ts, hs);
                dedup_skip_into(kd, vd, ts, suffix, tmp_k.get(), tmp_v.get(), bld);
                seed_from_real(keys(nn, hs), vals_mut(nn, new_ts, hs),
                               tmp_k.get(), tmp_v.get(), nc, new_ts);
            }

            bld.dealloc_node(node, h->alloc_u64());
            return {tag_leaf(nn), true, nc};
        }

        // In-place: convert erased entry's run to neighbor dups
        if constexpr (VT::IS_BOOL) {
            auto bv = bool_vals_mut(node, ts, hs);
            int first = idx;
            while (first > 0 && kd[first - 1] == suffix) --first;

            bool neighbor_val;
            K neighbor_key;
            if (first > 0) {
                neighbor_key = kd[first - 1];
                neighbor_val = bv.get(first - 1);
            } else {
                neighbor_key = kd[idx + 1];
                neighbor_val = bv.get(idx + 1);
            }
            for (int i = first; i <= (int)idx; ++i) {
                kd[i] = neighbor_key;
                bv.set(i, neighbor_val);
            }
        } else {
            VST* vd = vals_mut(node, ts, hs);
            erase_create_dup(kd, vd, ts, idx, suffix, bld);
        }
        h->set_entries(nc);
        return {tag_leaf(node), true, nc};
    }

private:
    // ==================================================================
    // Layout helpers
    // ==================================================================

    static K* keys(uint64_t* node, size_t header_size) noexcept {
        return reinterpret_cast<K*>(node + header_size);
    }
    static const K* keys(const uint64_t* node, size_t header_size) noexcept {
        return reinterpret_cast<const K*>(node + header_size);
    }

    static VST* vals_mut(uint64_t* node, size_t total, size_t header_size) noexcept {
        size_t kb = total * sizeof(K);
        kb = (kb + 7) & ~size_t{7};
        return reinterpret_cast<VST*>(
            reinterpret_cast<char*>(node + header_size) + kb);
    }
    static const VST* vals(const uint64_t* node, size_t total, size_t header_size) noexcept {
        size_t kb = total * sizeof(K);
        kb = (kb + 7) & ~size_t{7};
        return reinterpret_cast<const VST*>(
            reinterpret_cast<const char*>(node + header_size) + kb);
    }

    static bool_slots bool_vals_mut(uint64_t* node, size_t total, size_t header_size) noexcept {
        size_t kb = total * sizeof(K);
        kb = (kb + 7) & ~size_t{7};
        return bool_slots{ reinterpret_cast<uint64_t*>(
            reinterpret_cast<char*>(node + header_size) + kb) };
    }
    static bool_slots bool_vals(const uint64_t* node, size_t total, size_t header_size) noexcept {
        size_t kb = total * sizeof(K);
        kb = (kb + 7) & ~size_t{7};
        return bool_slots{ const_cast<uint64_t*>(reinterpret_cast<const uint64_t*>(
            reinterpret_cast<const char*>(node + header_size) + kb)) };
    }

    // ==================================================================
    // Dedup + skip one key, writing into output arrays
    // ==================================================================

    static void dedup_skip_into(const K* kd, VST* vd, uint16_t ts,
                                  K skip_suffix,
                                  K* out_k, VST* out_v, BLD& bld) {
        bool skipped = false;
        int wi = 0;
        if (kd[0] == skip_suffix) {
            skipped = true;
            if constexpr (VT::HAS_DESTRUCTOR)
                bld.destroy_value(vd[0]);
        } else {
            out_k[wi] = kd[0];
            VT::write_slot(&out_v[wi], vd[0]);
            wi++;
        }
        for (int i = 1; i < ts; ++i) {
            if (kd[i] == kd[i - 1]) continue;
            if (!skipped && kd[i] == skip_suffix) {
                skipped = true;
                if constexpr (VT::HAS_DESTRUCTOR)
                    bld.destroy_value(vd[i]);
                continue;
            }
            out_k[wi] = kd[i];
            VT::write_slot(&out_v[wi], vd[i]);
            wi++;
        }
        // C-type: erased pointer freed above; dups share pointers with
        //         copied entries (transferred), so no further destroy needed.
    }

    // ==================================================================
    // Single-pass: dedup old array + inject new key + seed dups into dest
    // No temp arrays. One read pass, one write pass.
    // ==================================================================

    static void seed_with_insert(K* dk, VST* dv,
                                   const K* old_k, const VST* old_v,
                                   uint16_t old_ts, uint16_t old_entries,
                                   K new_suffix, VST new_val,
                                   uint16_t new_entries, uint16_t new_ts) {

        if (new_entries == new_ts) {
            // No dups needed — straight copy with insert
            bool inserted = false;
            int wi = 0;
            // Handle first element (no dup check needed)
            if (new_suffix < old_k[0]) {
                dk[wi] = new_suffix;
                VT::init_slot(&dv[wi], new_val);
                wi++;
                inserted = true;
            }
            dk[wi] = old_k[0];
            VT::init_slot(&dv[wi], old_v[0]);
            wi++;
            for (int i = 1; i < old_ts; ++i) {
                if (old_k[i] == old_k[i - 1]) continue;
                if (!inserted && new_suffix < old_k[i]) {
                    dk[wi] = new_suffix;
                    VT::init_slot(&dv[wi], new_val);
                    wi++;
                    inserted = true;
                }
                dk[wi] = old_k[i];
                VT::init_slot(&dv[wi], old_v[i]);
                wi++;
            }
            if (!inserted) {
                dk[wi] = new_suffix;
                VT::init_slot(&dv[wi], new_val);
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

        // Handle first old entry (no dup check needed for i=0)
        // Check if new key goes before first old key
        if (new_suffix < old_k[0]) {
            dk[wi] = new_suffix;
            VT::init_slot(&dv[wi], new_val);
            wi++;
            real_out++;
            in_group++;
            inserted = true;

            if (placed < n_dups && in_group >= group_size) {
                dk[wi] = dk[wi - 1];
                VT::init_slot(&dv[wi], dv[wi - 1]);
                wi++;
                placed++;
                in_group = 0;
                group_size = stride + (placed < remainder ? 1 : 0);
            }
        }

        // Emit first old entry
        dk[wi] = old_k[0];
        VT::init_slot(&dv[wi], old_v[0]);
        wi++;
        real_out++;
        in_group++;

        if (placed < n_dups && in_group >= group_size) {
            dk[wi] = dk[wi - 1];
            VT::init_slot(&dv[wi], dv[wi - 1]);
            wi++;
            placed++;
            in_group = 0;
            group_size = stride + (placed < remainder ? 1 : 0);
        }

        for (int i = 1; i < old_ts; ++i) {
            if (old_k[i] == old_k[i - 1]) continue;  // skip dup

            // Inject new key at sorted position
            if (!inserted && new_suffix < old_k[i]) {
                dk[wi] = new_suffix;
                VT::init_slot(&dv[wi], new_val);
                wi++;
                real_out++;
                in_group++;
                inserted = true;

                // Check if group full → emit dup
                if (placed < n_dups && in_group >= group_size) {
                    dk[wi] = dk[wi - 1];
                    VT::init_slot(&dv[wi], dv[wi - 1]);
                    wi++;
                    placed++;
                    in_group = 0;
                    group_size = stride + (placed < remainder ? 1 : 0);
                }
            }

            // Emit real entry from old array
            dk[wi] = old_k[i];
            VT::init_slot(&dv[wi], old_v[i]);
            wi++;
            real_out++;
            in_group++;

            // Check if group full → emit dup
            if (placed < n_dups && in_group >= group_size) {
                dk[wi] = dk[wi - 1];
                VT::init_slot(&dv[wi], dv[wi - 1]);
                wi++;
                placed++;
                in_group = 0;
                group_size = stride + (placed < remainder ? 1 : 0);
            }
        }

        // New key is largest — append at end
        if (!inserted) {
            dk[wi] = new_suffix;
            VT::init_slot(&dv[wi], new_val);
            wi++;
            real_out++;
            in_group++;

            if (placed < n_dups && in_group >= group_size) {
                dk[wi] = dk[wi - 1];
                VT::init_slot(&dv[wi], dv[wi - 1]);
                wi++;
                placed++;
            }
        }
    }

    // ==================================================================
    // Dup helpers
    // ==================================================================

    static int find_dup_pos(const K* kd, int total, int ins, unsigned entries) {
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
        return dup_pos;
    }

    static void insert_consume_dup(
            K* kd, VST* vd, int total, int ins, unsigned entries,
            K suffix, VST value) {
        int dup_pos = find_dup_pos(kd, total, ins, entries);

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

    static void erase_create_dup(
            K* kd, VST* vd, int total, int idx,
            K suffix, BLD& bld) {
        int first = idx;
        while (first > 0 && kd[first - 1] == suffix) --first;

        if constexpr (VT::HAS_DESTRUCTOR)
            bld.destroy_value(vd[first]);

        K   neighbor_key;
        VST neighbor_val;
        if (first > 0) {
            neighbor_key = kd[first - 1];
            neighbor_val = vd[first - 1];
        } else {
            neighbor_key = kd[idx + 1];
            neighbor_val = vd[idx + 1];
        }
        // vd[first] is destroyed (uninit for B), rest are live dups
        kd[first] = neighbor_key;
        VT::init_slot(&vd[first], neighbor_val);
        for (int i = first + 1; i <= idx; ++i) {
            kd[i] = neighbor_key;
            VT::write_slot(&vd[i], neighbor_val);
        }
    }

    // ==================================================================
    // Seed: distribute dups evenly among real entries
    // ==================================================================

    static void seed_from_real(K* kd, VST* vd,
                                const K* real_keys, const VST* real_vals,
                                uint16_t n_entries, uint16_t total) {

        if (n_entries == total) {
            std::memcpy(kd, real_keys, n_entries * sizeof(K));
            VT::copy_uninit(real_vals, n_entries, vd);
            return;
        }

        uint16_t n_dups = total - n_entries;
        uint16_t stride = n_entries / (n_dups + 1);
        uint16_t remainder = n_entries % (n_dups + 1);

        int write = 0, src = 0, placed = 0;
        while (placed < n_dups) {
            int chunk = stride + (placed < remainder ? 1 : 0);
            std::memcpy(kd + write, real_keys + src, chunk * sizeof(K));
            VT::copy_uninit(real_vals + src, chunk, vd + write);
            write += chunk;
            src += chunk;
            kd[write] = kd[write - 1];
            VT::init_slot(&vd[write], vd[write - 1]);
            write++;
            placed++;
        }

        int remaining = n_entries - src;
        if (remaining > 0) {
            std::memcpy(kd + write, real_keys + src, remaining * sizeof(K));
            VT::copy_uninit(real_vals + src, remaining, vd + write);
        }
    }
};

} // namespace gteitelbaum

#endif // KNTRIE_COMPACT_HPP
