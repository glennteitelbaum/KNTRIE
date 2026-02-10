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
// Allocations are padded via round_up_u64 to enable in-place insert.
// alloc_u64 stores the actual (padded) allocation size.
// ==========================================================================

template<typename KEY, typename VALUE, typename ALLOC>
struct CompactOps {
    using KOps = KeyOps<KEY>;
    using VT   = ValueTraits<VALUE, ALLOC>;
    using VST  = typename VT::slot_type;

    // --- exact needed size (not padded) ---

    template<int BITS>
    static constexpr size_t size_u64(size_t count) noexcept {
        using K = typename suffix_traits<BITS>::type;
        size_t kb = count * sizeof(K);
        kb = (kb + 7) & ~size_t{7};
        size_t vb = count * sizeof(VST);
        vb = (vb + 7) & ~size_t{7};
        return HEADER_U64 + (kb + vb) / 8;
    }

    // ==================================================================
    // Factory: build from pre-sorted working arrays
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

        using K = typename suffix_traits<BITS>::type;
        K*   kd = keys_<BITS>(node);
        VST* vd = vals_<BITS>(node, count);
        if (count > 0) {
            std::memcpy(kd, sorted_keys, count * sizeof(K));
            std::memcpy(vd, values, count * sizeof(VST));
        }
        return node;
    }

    // ==================================================================
    // Iterate entries: cb(K suffix, VST value_slot)
    // ==================================================================

    template<int BITS, typename Fn>
    static void for_each(const uint64_t* node, const NodeHeader* h, Fn&& cb) {
        using K = typename suffix_traits<BITS>::type;
        const K*   kd = keys_<BITS>(node);
        const VST* vd = vals_<BITS>(node, h->entries);
        for (uint16_t i = 0; i < h->entries; ++i) cb(kd[i], vd[i]);
    }

    // ==================================================================
    // Destroy all values + deallocate node
    // ==================================================================

    template<int BITS>
    static void destroy_and_dealloc(uint64_t* node, ALLOC& alloc) {
        auto* h = get_header(node);
        if constexpr (!VT::is_inline) {
            VST* vd = vals_<BITS>(node, h->entries);
            for (uint16_t i = 0; i < h->entries; ++i) VT::destroy(vd[i], alloc);
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
        int idx = JumpSearch<K>::search(keys_<BITS>(node),
                                        static_cast<int>(h.entries), suffix);
        if (idx < 0) [[unlikely]] return nullptr;
        return VT::as_ptr(vals_<BITS>(node, h.entries)[idx]);
    }

    // ==================================================================
    // Insert  (returns needs_split=true when entries >= COMPACT_MAX)
    //
    // Template params:
    //   INSERT: allow inserting new keys
    //   ASSIGN: allow overwriting existing values
    //
    // In-place when padded allocation has room; otherwise alloc new.
    // ==================================================================

    struct CompactInsertResult { uint64_t* node; bool inserted; bool needs_split; };

    template<int BITS, bool INSERT = true, bool ASSIGN = true>
    requires (INSERT || ASSIGN)
    static CompactInsertResult insert(uint64_t* node, NodeHeader* h,
                                      uint64_t ik, VST value, ALLOC& alloc) {
        using K = typename suffix_traits<BITS>::type;
        K suffix = static_cast<K>(KOps::template extract_suffix<BITS>(ik));
        K*   kd = keys_<BITS>(node);
        VST* vd = vals_<BITS>(node, h->entries);

        int idx = JumpSearch<K>::search_insert(kd,
                                                static_cast<int>(h->entries), suffix);
        if (idx >= 0) {
            if constexpr (ASSIGN) {
                VT::destroy(vd[idx], alloc);
                VT::write_slot(&vd[idx], value);
            }
            return {node, false, false};
        }
        if constexpr (!INSERT) return {node, false, false};

        size_t ins = static_cast<size_t>(-(idx + 1));
        if (h->entries >= COMPACT_MAX) return {node, false, true};

        uint16_t count = h->entries;
        uint16_t nc = count + 1;
        size_t needed = size_u64<BITS>(nc);

        // --- In-place if we have capacity ---
        if (needed <= h->alloc_u64) {
            insert_in_place_<BITS>(node, h, suffix, value, ins);
            return {node, true, false};
        }

        // --- Allocate new with padding ---
        size_t au64 = round_up_u64(needed);
        uint64_t* nn = alloc_node(alloc, au64);
        auto* nh = get_header(nn); *nh = *h;
        nh->entries = nc;
        nh->descendants = nc;
        nh->alloc_u64 = static_cast<uint16_t>(au64);
        if (h->skip() > 0) set_prefix(nn, get_prefix(node));

        K*   nk = keys_<BITS>(nn);
        VST* nv = vals_<BITS>(nn, nc);
        std::memcpy(nk, kd, ins * sizeof(K));
        nk[ins] = suffix;
        std::memcpy(nk + ins + 1, kd + ins, (count - ins) * sizeof(K));
        std::memcpy(nv, vd, ins * sizeof(VST));
        VT::write_slot(&nv[ins], value);
        std::memcpy(nv + ins + 1, vd + ins, (count - ins) * sizeof(VST));

        dealloc_node(alloc, node, h->alloc_u64);
        return {nn, true, false};
    }

    // ==================================================================
    // Erase  ({nullptr,true} when last entry removed)
    //
    // In-place when allocation isn't oversized (>2 steps).
    // ==================================================================

    template<int BITS>
    static EraseResult erase(uint64_t* node, NodeHeader* h,
                             uint64_t ik, ALLOC& alloc) {
        using K = typename suffix_traits<BITS>::type;
        K suffix = static_cast<K>(KOps::template extract_suffix<BITS>(ik));
        uint16_t count = h->entries;
        K*   kd = keys_<BITS>(node);
        VST* vd = vals_<BITS>(node, count);

        int idx = JumpSearch<K>::search(kd, static_cast<int>(count), suffix);
        if (idx < 0) return {node, false};
        VT::destroy(vd[idx], alloc);

        uint16_t nc = count - 1;
        if (nc == 0) {
            dealloc_node(alloc, node, h->alloc_u64);
            return {nullptr, true};
        }

        size_t needed = size_u64<BITS>(nc);

        // --- In-place if not oversized ---
        if (!should_shrink_u64(h->alloc_u64, needed)) {
            erase_in_place_<BITS>(node, h, idx);
            return {node, true};
        }

        // --- Allocate smaller with padding ---
        size_t au64 = round_up_u64(needed);
        uint64_t* nn = alloc_node(alloc, au64);
        auto* nh = get_header(nn); *nh = *h;
        nh->entries = nc;
        nh->descendants = nc;
        nh->alloc_u64 = static_cast<uint16_t>(au64);
        if (h->skip() > 0) set_prefix(nn, get_prefix(node));

        K*   nk = keys_<BITS>(nn);
        VST* nv = vals_<BITS>(nn, nc);
        std::memcpy(nk, kd, idx * sizeof(K));
        std::memcpy(nk + idx, kd + idx + 1, (nc - idx) * sizeof(K));
        std::memcpy(nv, vd, idx * sizeof(VST));
        std::memcpy(nv + idx, vd + idx + 1, (nc - idx) * sizeof(VST));

        dealloc_node(alloc, node, h->alloc_u64);
        return {nn, true};
    }

    // ==================================================================
    // Dedup+seed placeholder (Phase 2 -- written, not called)
    //
    // Will redistribute duplicate tombstones evenly across the key array
    // after a fresh allocation or resize.
    // ==================================================================

    template<int BITS>
    static void seed_dups(uint64_t* /*node*/, NodeHeader* /*h*/) noexcept {
        // Phase 2: will redistribute dup entries evenly across sorted keys.
        // Algorithm:
        //   1. Calculate total_slots from SlotTable[alloc_u64]
        //   2. dups_available = total_slots - entries
        //   3. Spread entries apart, inserting dup copies at even intervals
        //   4. Update h->set_dups(dups_available)
    }

    template<int BITS>
    static void dedup(uint64_t* /*node*/, NodeHeader* /*h*/) noexcept {
        // Phase 2: will compact out duplicate tombstones before resize.
        // Algorithm:
        //   1. Scan keys, skip consecutive duplicates
        //   2. Compact keys[] and vals[] leftward
        //   3. Update h->entries, h->set_dups(0)
    }

private:
    // --- layout helpers (private) ---
    // Layout: [header (2 u64)][keys...][padding to 8-byte][values...]

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
    static VST* vals_(uint64_t* node, size_t count) noexcept {
        using K = typename suffix_traits<BITS>::type;
        size_t kb = count * sizeof(K);
        kb = (kb + 7) & ~size_t{7};
        return reinterpret_cast<VST*>(
            reinterpret_cast<char*>(node + HEADER_U64) + kb);
    }
    template<int BITS>
    static const VST* vals_(const uint64_t* node, size_t count) noexcept {
        using K = typename suffix_traits<BITS>::type;
        size_t kb = count * sizeof(K);
        kb = (kb + 7) & ~size_t{7};
        return reinterpret_cast<const VST*>(
            reinterpret_cast<const char*>(node + HEADER_U64) + kb);
    }

    // ==================================================================
    // In-place insert helper
    // ==================================================================

    template<int BITS>
    static void insert_in_place_(uint64_t* node, NodeHeader* h,
                                  typename suffix_traits<BITS>::type suffix,
                                  VST value, size_t ins) {
        using K = typename suffix_traits<BITS>::type;
        uint16_t count = h->entries;
        uint16_t nc = count + 1;

        char* base = reinterpret_cast<char*>(node + HEADER_U64);

        size_t old_kb = count * sizeof(K);
        old_kb = (old_kb + 7) & ~size_t{7};
        size_t new_kb = nc * sizeof(K);
        new_kb = (new_kb + 7) & ~size_t{7};

        VST* old_vd = reinterpret_cast<VST*>(base + old_kb);
        VST* new_vd = reinterpret_cast<VST*>(base + new_kb);
        K*   kd = reinterpret_cast<K*>(base);

        // 1. Move values: each element moves exactly once
        if (new_kb != old_kb) {
            std::memmove(new_vd, old_vd, ins * sizeof(VST));
            std::memmove(new_vd + ins + 1, old_vd + ins,
                         (count - ins) * sizeof(VST));
        } else {
            std::memmove(new_vd + ins + 1, new_vd + ins,
                         (count - ins) * sizeof(VST));
        }
        VT::write_slot(&new_vd[ins], value);

        // 2. Create gap at insertion point in keys
        std::memmove(kd + ins + 1, kd + ins,
                     (count - ins) * sizeof(K));
        kd[ins] = suffix;

        // 3. Update header
        h->entries = nc;
        h->descendants = nc;
    }

    // ==================================================================
    // In-place erase helper
    // ==================================================================

    template<int BITS>
    static void erase_in_place_(uint64_t* node, NodeHeader* h, int idx) {
        using K = typename suffix_traits<BITS>::type;
        uint16_t count = h->entries;
        uint16_t nc = count - 1;

        char* base = reinterpret_cast<char*>(node + HEADER_U64);

        size_t old_kb = count * sizeof(K);
        old_kb = (old_kb + 7) & ~size_t{7};
        size_t new_kb = nc * sizeof(K);
        new_kb = (new_kb + 7) & ~size_t{7};

        K*   kd = reinterpret_cast<K*>(base);
        VST* old_vd = reinterpret_cast<VST*>(base + old_kb);
        VST* new_vd = reinterpret_cast<VST*>(base + new_kb);

        // 1. Close gap in keys
        std::memmove(kd + idx, kd + idx + 1,
                     (count - idx - 1) * sizeof(K));

        // 2. Move values: each element moves exactly once
        if (new_kb != old_kb) {
            std::memmove(new_vd, old_vd, idx * sizeof(VST));
            std::memmove(new_vd + idx, old_vd + idx + 1,
                         (nc - idx) * sizeof(VST));
        } else {
            std::memmove(old_vd + idx, old_vd + idx + 1,
                         (nc - idx) * sizeof(VST));
        }

        // 3. Update header
        h->entries = nc;
        h->descendants = nc;
    }
};

} // namespace kn3

#endif // KNTRIE_COMPACT_HPP
