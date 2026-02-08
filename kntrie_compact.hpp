#ifndef KNTRIE_COMPACT_HPP
#define KNTRIE_COMPACT_HPP

#include "kntrie_support.hpp"

#include <cstdint>
#include <cstring>
#include <algorithm>

namespace kn3 {

// ==========================================================================
// Search Strategy: IdxSearch  (idx1 → idx2 → key)
// ==========================================================================

template<typename K>
struct IdxSearch {
    static int idx1_count(int c) noexcept { return c > 256 ? (c+255)/256 : 0; }
    static int idx2_count(int c) noexcept { return c > 16  ? (c+15)/16   : 0; }
    static int extra(int c) noexcept { return idx1_count(c) + idx2_count(c); }
    static void build(K* dest, const K* src, int count) noexcept {
        int i1 = idx1_count(count), i2 = idx2_count(count);
        for (int i = 0; i < i1; ++i) dest[i] = src[i * 256];
        K* d2 = dest + i1;
        for (int i = 0; i < i2; ++i) d2[i] = src[i * 16];
        std::memcpy(d2 + i2, src, count * sizeof(K));
    }
    static int subsearch(const K* s, int c, K key) noexcept {
        const K* p = s, *e = s + c;
        do { if (*p > key) break; p++; } while (p < e);
        return static_cast<int>(p - s) - 1;
    }
    static int search(const K* start, int count, K key) noexcept {
        if (count == 0) return -1;
        int i1 = idx1_count(count), i2 = idx2_count(count);
        const K* d2 = start + i1; const K* keys = d2 + i2;
        int ks = 0;
        if (i1 > 0) { int b = subsearch(start, i1, key); if (b < 0) return -1;
            d2 += b * 16; i2 = std::min(16, i2 - b * 16); ks = b * 256; }
        if (i2 > 0) { int b = subsearch(d2, i2, key); if (b < 0) return -1;
            ks += b * 16; }
        int kl = std::min(16, count - ks);
        int idx = subsearch(keys + ks, kl, key);
        if (idx >= 0 && keys[ks + idx] == key) return ks + idx;
        return -1;
    }
    static K*       keys_ptr(K* s, int c)       noexcept { return s + extra(c); }
    static const K* keys_ptr(const K* s, int c) noexcept { return s + extra(c); }
};

// ==========================================================================
// CompactOps  – builds/searches/mutates compact leaf nodes
//
// Layout: [header (1-2 u64)][search_overlay + sorted_keys][values]
//   flags=0 → is_leaf (compact)
// ==========================================================================

template<typename KEY, typename VALUE, typename ALLOC>
struct CompactOps {
    using KOps = KeyOps<KEY>;
    using VT   = ValueTraits<VALUE, ALLOC>;
    using VST  = typename VT::slot_type;

    // --- allocation size ---

    template<int BITS>
    static constexpr size_t size_u64(size_t count, uint8_t skip) noexcept {
        using K = typename suffix_traits<BITS>::type;
        int ex = IdxSearch<K>::extra(static_cast<int>(count));
        size_t sb = (static_cast<size_t>(ex) + count) * sizeof(K);
        sb = (sb + 7) & ~size_t{7};
        size_t vb = count * sizeof(VST);
        vb = (vb + 7) & ~size_t{7};
        return header_u64(skip) + (sb + vb) / 8;
    }

    // ==================================================================
    // Factory: build from pre-sorted working arrays
    // ==================================================================

    template<int BITS>
    static uint64_t* make_leaf(const typename suffix_traits<BITS>::type* sorted_keys,
                               const VST* values, uint32_t count,
                               uint8_t skip, uint64_t prefix, ALLOC& alloc) {
        size_t au64 = size_u64<BITS>(count, skip);
        uint64_t* node = alloc_node(alloc, au64);
        auto* h = get_header(node);
        h->entries = static_cast<uint16_t>(count);
        h->descendants = static_cast<uint16_t>(count);
        h->alloc_u64 = static_cast<uint16_t>(au64);
        h->skip = skip;
        // flags remains 0 → is_leaf (compact)
        if (skip > 0) set_prefix(node, prefix);

        using K = typename suffix_traits<BITS>::type;
        K*   kd = keys_data_<BITS>(node, count);
        VST* vd = vals_<BITS>(node, count);
        if (count > 0) {
            std::memcpy(kd, sorted_keys, count * sizeof(K));
            std::memcpy(vd, values, count * sizeof(VST));
            IdxSearch<K>::build(search_start_<BITS>(node), kd, static_cast<int>(count));
        }
        return node;
    }

    // ==================================================================
    // Iterate entries: cb(K suffix, VST value_slot)
    // ==================================================================

    template<int BITS, typename Fn>
    static void for_each(const uint64_t* node, const NodeHeader* h, Fn&& cb) {
        using K = typename suffix_traits<BITS>::type;
        const K*   kd = keys_data_<BITS>(node, h->entries);
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
    static const VALUE* find(const uint64_t* node, const NodeHeader* h,
                             uint64_t ik) noexcept {
        using K = typename suffix_traits<BITS>::type;
        K suffix = static_cast<K>(KOps::template extract_suffix<BITS>(ik));
        int idx = IdxSearch<K>::search(search_start_<BITS>(node),
                                       static_cast<int>(h->entries), suffix);
        if (idx < 0) return nullptr;
        return VT::as_ptr(vals_<BITS>(node, h->entries)[idx]);
    }

    // ==================================================================
    // Insert  (returns needs_split=true when entries >= COMPACT_MAX)
    // ==================================================================

    struct CompactInsertResult { uint64_t* node; bool inserted; bool needs_split; };

    template<int BITS>
    static CompactInsertResult insert(uint64_t* node, NodeHeader* h,
                                      uint64_t ik, VST value, ALLOC& alloc) {
        using K = typename suffix_traits<BITS>::type;
        K suffix = static_cast<K>(KOps::template extract_suffix<BITS>(ik));
        K*   kd = keys_data_<BITS>(node, h->entries);
        VST* vd = vals_<BITS>(node, h->entries);

        int idx = binary_search_for_insert(kd, static_cast<size_t>(h->entries), suffix);
        if (idx >= 0) {
            VT::destroy(vd[idx], alloc);
            VT::write_slot(&vd[idx], value);
            return {node, false, false};
        }
        size_t ins = static_cast<size_t>(-(idx + 1));
        if (h->entries >= COMPACT_MAX) return {node, false, true};

        uint16_t nc = h->entries + 1;
        size_t au64 = size_u64<BITS>(nc, h->skip);
        uint64_t* nn = alloc_node(alloc, au64);
        auto* nh = get_header(nn); *nh = *h;
        nh->entries = nc;
        nh->descendants = nc;
        nh->alloc_u64 = static_cast<uint16_t>(au64);
        if (h->skip > 0) set_prefix(nn, get_prefix(node));

        K*   nk = keys_data_<BITS>(nn, nc);
        VST* nv = vals_<BITS>(nn, nc);
        std::memcpy(nk, kd, ins * sizeof(K));
        std::memcpy(nv, vd, ins * sizeof(VST));
        nk[ins] = suffix;
        VT::write_slot(&nv[ins], value);
        std::memcpy(nk + ins + 1, kd + ins, (h->entries - ins) * sizeof(K));
        std::memcpy(nv + ins + 1, vd + ins, (h->entries - ins) * sizeof(VST));
        IdxSearch<K>::build(search_start_<BITS>(nn), nk, static_cast<int>(nc));

        dealloc_node(alloc, node, h->alloc_u64);
        return {nn, true, false};
    }

    // ==================================================================
    // Erase  ({nullptr,true} when last entry removed)
    // ==================================================================

    template<int BITS>
    static EraseResult erase(uint64_t* node, NodeHeader* h,
                             uint64_t ik, ALLOC& alloc) {
        using K = typename suffix_traits<BITS>::type;
        K suffix = static_cast<K>(KOps::template extract_suffix<BITS>(ik));
        uint16_t count = h->entries;
        K*   kd = keys_data_<BITS>(node, count);
        VST* vd = vals_<BITS>(node, count);

        int idx = binary_search_for_insert(kd, static_cast<size_t>(count), suffix);
        if (idx < 0) return {node, false};
        VT::destroy(vd[idx], alloc);

        uint16_t nc = count - 1;
        if (nc == 0) {
            dealloc_node(alloc, node, h->alloc_u64);
            return {nullptr, true};
        }
        size_t au64 = size_u64<BITS>(nc, h->skip);
        uint64_t* nn = alloc_node(alloc, au64);
        auto* nh = get_header(nn); *nh = *h;
        nh->entries = nc;
        nh->descendants = nc;
        nh->alloc_u64 = static_cast<uint16_t>(au64);
        if (h->skip > 0) set_prefix(nn, get_prefix(node));

        K*   nk = keys_data_<BITS>(nn, nc);
        VST* nv = vals_<BITS>(nn, nc);
        std::memcpy(nk, kd, idx * sizeof(K));
        std::memcpy(nv, vd, idx * sizeof(VST));
        std::memcpy(nk + idx, kd + idx + 1, (nc - idx) * sizeof(K));
        std::memcpy(nv + idx, vd + idx + 1, (nc - idx) * sizeof(VST));
        IdxSearch<K>::build(search_start_<BITS>(nn), nk, static_cast<int>(nc));

        dealloc_node(alloc, node, h->alloc_u64);
        return {nn, true};
    }

private:
    // --- layout helpers (private) ---

    template<int BITS>
    static auto search_start_(uint64_t* node) noexcept {
        using K = typename suffix_traits<BITS>::type;
        return reinterpret_cast<K*>(node + header_u64(get_header(node)->skip));
    }
    template<int BITS>
    static auto search_start_(const uint64_t* node) noexcept {
        using K = typename suffix_traits<BITS>::type;
        return reinterpret_cast<const K*>(node + header_u64(get_header(node)->skip));
    }

    template<int BITS>
    static auto keys_data_(uint64_t* node, size_t count) noexcept {
        using K = typename suffix_traits<BITS>::type;
        return IdxSearch<K>::keys_ptr(search_start_<BITS>(node), static_cast<int>(count));
    }
    template<int BITS>
    static auto keys_data_(const uint64_t* node, size_t count) noexcept {
        using K = typename suffix_traits<BITS>::type;
        return IdxSearch<K>::keys_ptr(search_start_<BITS>(node), static_cast<int>(count));
    }

    template<int BITS>
    static VST* vals_(uint64_t* node, size_t count) noexcept {
        using K = typename suffix_traits<BITS>::type;
        int ex = IdxSearch<K>::extra(static_cast<int>(count));
        size_t sb = (static_cast<size_t>(ex) + count) * sizeof(K);
        sb = (sb + 7) & ~size_t{7};
        return reinterpret_cast<VST*>(
            reinterpret_cast<char*>(node + header_u64(get_header(node)->skip)) + sb);
    }
    template<int BITS>
    static const VST* vals_(const uint64_t* node, size_t count) noexcept {
        using K = typename suffix_traits<BITS>::type;
        int ex = IdxSearch<K>::extra(static_cast<int>(count));
        size_t sb = (static_cast<size_t>(ex) + count) * sizeof(K);
        sb = (sb + 7) & ~size_t{7};
        return reinterpret_cast<const VST*>(
            reinterpret_cast<const char*>(node + header_u64(get_header(node)->skip)) + sb);
    }
};

} // namespace kn3

#endif // KNTRIE_COMPACT_HPP
