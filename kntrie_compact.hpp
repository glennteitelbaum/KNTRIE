#ifndef KNTRIE_COMPACT_HPP
#define KNTRIE_COMPACT_HPP

#include "kntrie_support.hpp"

#include <cstdint>
#include <cstring>
#include <bit>
#include <limits>
#include <array>
#include <algorithm>

namespace kn3 {

// ==========================================================================
// Static Eytzinger Block Tables (compiled into .rodata)
// ==========================================================================

template<int H>
struct EytzBlockTable {
    static constexpr std::array<uint16_t, H> build() {
        std::array<uint16_t, H> t{};
        int si = 0;
        auto f = [&](auto& self, int i) -> void {
            if (i > H) return;
            self(self, 2 * i);
            t[i - 1] = static_cast<uint16_t>(si++);
            self(self, 2 * i + 1);
        };
        f(f, 1);
        return t;
    }
    static constexpr auto table = build();
};

inline const uint16_t* get_block_table(int H) noexcept {
    static constexpr const uint16_t* tables[] = {
        EytzBlockTable<1>::table.data(),
        EytzBlockTable<2>::table.data(),
        EytzBlockTable<4>::table.data(),
        EytzBlockTable<8>::table.data(),
        EytzBlockTable<16>::table.data(),
        EytzBlockTable<32>::table.data(),
        EytzBlockTable<64>::table.data(),
        EytzBlockTable<128>::table.data(),
        EytzBlockTable<256>::table.data(),
        EytzBlockTable<512>::table.data(),
    };
    return tables[std::countr_zero(static_cast<unsigned>(H))];
}

// ==========================================================================
// Search Strategy: Static Eytzinger (for u64 and u32 keys, BMAX=8)
// ==========================================================================

template<typename K>
struct EytzSearch {
    static constexpr int BMAX = 8;

    static int compute_n(int count) noexcept {
        if (count <= BMAX) return 0;
        return static_cast<int>(std::bit_ceil(
            static_cast<unsigned>((count + BMAX - 1) / BMAX)));
    }
    static int compute_bact(int count, int n) noexcept { return (count + n - 1) / n; }
    static int extra(int count) noexcept {
        int n = compute_n(count); return n == 0 ? 0 : 1 + n;
    }
    static void build(K* dest, const K* src, int count) noexcept {
        int n = compute_n(count);
        if (n == 0) { std::memcpy(dest, src, count * sizeof(K)); return; }
        int Bact = compute_bact(count, n);
        K* ek = dest + 1;
        int si = 0;
        auto f = [&](auto& self, int i) -> void {
            if (i > n) return;
            self(self, 2 * i);
            int idx = si * Bact;
            ek[i - 1] = (idx < count) ? src[idx] : std::numeric_limits<K>::max();
            si++;
            self(self, 2 * i + 1);
        };
        f(f, 1);
        std::memcpy(dest + 1 + n, src, count * sizeof(K));
    }
    static int search(const K* start, int count, K key) noexcept {
        if (count == 0) return -1;
        int n = compute_n(count);
        if (n == 0) {
            for (int i = 0; i < count; ++i) {
                if (start[i] == key) return i;
                if (start[i] > key) return -1;
            }
            return -1;
        }
        int Bact = compute_bact(count, n);
        const K* ek = start + 1;
        const uint16_t* blk = get_block_table(n);
        const K* keys = start + 1 + n;
        unsigned i = 1; int block = 0;
        while (i <= static_cast<unsigned>(n)) {
            __builtin_prefetch(&ek[4 * i - 1], 0, 0);
            bool r = (key >= ek[i - 1]);
            block = r ? static_cast<int>(blk[i - 1]) : block;
            i = 2 * i + (r ? 1 : 0);
        }
        int ks = block * Bact;
        int kl = std::min(Bact, count - ks);
        for (int j = 0; j < kl; ++j) {
            if (keys[ks + j] == key) return ks + j;
            if (keys[ks + j] > key) return -1;
        }
        return -1;
    }
};

// ==========================================================================
// Search Strategy: Old Idx (for u16 keys)
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
};

// ==========================================================================
// Unified Search Dispatch
// ==========================================================================

template<typename K>
struct KnSearch {
    static constexpr bool use_eytz = sizeof(K) >= 4;
    using impl = std::conditional_t<use_eytz, EytzSearch<K>, IdxSearch<K>>;
    static int  extra(int c)                    noexcept { return impl::extra(c); }
    static void build(K* d, const K* s, int c)  noexcept { impl::build(d, s, c); }
    static int  search(const K* s, int c, K k)  noexcept { return impl::search(s, c, k); }
    static K*       keys_ptr(K* s, int c)       noexcept { return s + extra(c); }
    static const K* keys_ptr(const K* s, int c) noexcept { return s + extra(c); }
};

// ==========================================================================
// CompactOps  – builds/searches/mutates compact leaf nodes
//
// Layout: [header (1-2 u64)][search_overlay + sorted_keys][values]
//   flags=0 → not internal, not split (leaf compact)
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
        int ex = KnSearch<K>::extra(static_cast<int>(count));
        size_t sb = (static_cast<size_t>(ex) + count) * sizeof(K);
        sb = (sb + 7) & ~size_t{7};
        size_t vb = count * sizeof(VST);
        vb = (vb + 7) & ~size_t{7};
        return header_u64(skip) + (sb + vb) / 8;
    }

    // ==================================================================
    // Factory: build from pre-sorted working arrays
    //   flags=0 after alloc_node (zeroed) → leaf compact naturally
    // ==================================================================

    template<int BITS>
    static uint64_t* make_leaf(const typename suffix_traits<BITS>::type* sorted_keys,
                               const VST* values, uint32_t count,
                               uint8_t skip, uint64_t prefix, ALLOC& alloc) {
        using K = typename suffix_traits<BITS>::type;
        uint64_t* node = alloc_node(alloc, size_u64<BITS>(count, skip));
        auto* h = get_header(node);
        h->count = count; h->skip = skip;
        // flags remains 0 → not internal, not split (leaf compact)
        if (skip > 0) set_prefix(node, prefix);

        K*   kd = keys_data_<BITS>(node, count);
        VST* vd = vals_<BITS>(node, count);
        if (count > 0) {
            std::memcpy(kd, sorted_keys, count * sizeof(K));
            std::memcpy(vd, values, count * sizeof(VST));
            KnSearch<K>::build(search_start_<BITS>(node), kd, static_cast<int>(count));
        }
        return node;
    }

    // ==================================================================
    // Iterate entries: cb(K suffix, VST value_slot)
    // ==================================================================

    template<int BITS, typename Fn>
    static void for_each(const uint64_t* node, const NodeHeader* h, Fn&& cb) {
        using K = typename suffix_traits<BITS>::type;
        const K*   kd = keys_data_<BITS>(node, h->count);
        const VST* vd = vals_<BITS>(node, h->count);
        for (uint32_t i = 0; i < h->count; ++i) cb(kd[i], vd[i]);
    }

    // ==================================================================
    // Destroy all values + deallocate node
    // ==================================================================

    template<int BITS>
    static void destroy_and_dealloc(uint64_t* node, ALLOC& alloc) {
        auto* h = get_header(node);
        if constexpr (!VT::is_inline) {
            VST* vd = vals_<BITS>(node, h->count);
            for (uint32_t i = 0; i < h->count; ++i) VT::destroy(vd[i], alloc);
        }
        dealloc_node(alloc, node, size_u64<BITS>(h->count, h->skip));
    }

    // ==================================================================
    // Find
    // ==================================================================

    template<int BITS>
    static const VALUE* find(const uint64_t* node, const NodeHeader* h,
                             uint64_t ik) noexcept {
        using K = typename suffix_traits<BITS>::type;
        K suffix = static_cast<K>(KOps::template extract_suffix<BITS>(ik));
        int idx = KnSearch<K>::search(search_start_<BITS>(node),
                                       static_cast<int>(h->count), suffix);
        if (idx < 0) return nullptr;
        return VT::as_ptr(vals_<BITS>(node, h->count)[idx]);
    }

    // ==================================================================
    // Insert  (returns needs_split=true when count >= COMPACT_MAX)
    // ==================================================================

    struct CompactInsertResult { uint64_t* node; bool inserted; bool needs_split; };

    template<int BITS>
    static CompactInsertResult insert(uint64_t* node, NodeHeader* h,
                                      uint64_t ik, VST value, ALLOC& alloc) {
        using K = typename suffix_traits<BITS>::type;
        K suffix = static_cast<K>(KOps::template extract_suffix<BITS>(ik));
        K*   kd = keys_data_<BITS>(node, h->count);
        VST* vd = vals_<BITS>(node, h->count);

        int idx = binary_search_for_insert(kd, static_cast<size_t>(h->count), suffix);
        if (idx >= 0) {
            VT::destroy(vd[idx], alloc);
            VT::write_slot(&vd[idx], value);
            return {node, false, false};
        }
        size_t ins = static_cast<size_t>(-(idx + 1));
        if (h->count >= COMPACT_MAX) return {node, false, true};

        size_t nc = h->count + 1;
        uint64_t* nn = alloc_node(alloc, size_u64<BITS>(nc, h->skip));
        auto* nh = get_header(nn); *nh = *h;
        nh->count = static_cast<uint32_t>(nc);
        if (h->skip > 0) set_prefix(nn, get_prefix(node));

        K*   nk = keys_data_<BITS>(nn, nc);
        VST* nv = vals_<BITS>(nn, nc);
        std::memcpy(nk, kd, ins * sizeof(K));
        std::memcpy(nv, vd, ins * sizeof(VST));
        nk[ins] = suffix;
        VT::write_slot(&nv[ins], value);
        std::memcpy(nk + ins + 1, kd + ins, (h->count - ins) * sizeof(K));
        std::memcpy(nv + ins + 1, vd + ins, (h->count - ins) * sizeof(VST));
        KnSearch<K>::build(search_start_<BITS>(nn), nk, static_cast<int>(nc));

        dealloc_node(alloc, node, size_u64<BITS>(h->count, h->skip));
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
        uint32_t count = h->count;
        K*   kd = keys_data_<BITS>(node, count);
        VST* vd = vals_<BITS>(node, count);

        int idx = binary_search_for_insert(kd, static_cast<size_t>(count), suffix);
        if (idx < 0) return {node, false};
        VT::destroy(vd[idx], alloc);

        uint32_t nc = count - 1;
        if (nc == 0) {
            dealloc_node(alloc, node, size_u64<BITS>(count, h->skip));
            return {nullptr, true};
        }
        uint64_t* nn = alloc_node(alloc, size_u64<BITS>(nc, h->skip));
        auto* nh = get_header(nn); *nh = *h; nh->count = nc;
        if (h->skip > 0) set_prefix(nn, get_prefix(node));

        K*   nk = keys_data_<BITS>(nn, nc);
        VST* nv = vals_<BITS>(nn, nc);
        std::memcpy(nk, kd, idx * sizeof(K));
        std::memcpy(nv, vd, idx * sizeof(VST));
        std::memcpy(nk + idx, kd + idx + 1, (nc - idx) * sizeof(K));
        std::memcpy(nv + idx, vd + idx + 1, (nc - idx) * sizeof(VST));
        KnSearch<K>::build(search_start_<BITS>(nn), nk, static_cast<int>(nc));

        dealloc_node(alloc, node, size_u64<BITS>(count, h->skip));
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
        return KnSearch<K>::keys_ptr(search_start_<BITS>(node), static_cast<int>(count));
    }
    template<int BITS>
    static auto keys_data_(const uint64_t* node, size_t count) noexcept {
        using K = typename suffix_traits<BITS>::type;
        return KnSearch<K>::keys_ptr(search_start_<BITS>(node), static_cast<int>(count));
    }

    template<int BITS>
    static VST* vals_(uint64_t* node, size_t count) noexcept {
        using K = typename suffix_traits<BITS>::type;
        int ex = KnSearch<K>::extra(static_cast<int>(count));
        size_t sb = (static_cast<size_t>(ex) + count) * sizeof(K);
        sb = (sb + 7) & ~size_t{7};
        return reinterpret_cast<VST*>(
            reinterpret_cast<char*>(node + header_u64(get_header(node)->skip)) + sb);
    }
    template<int BITS>
    static const VST* vals_(const uint64_t* node, size_t count) noexcept {
        using K = typename suffix_traits<BITS>::type;
        int ex = KnSearch<K>::extra(static_cast<int>(count));
        size_t sb = (static_cast<size_t>(ex) + count) * sizeof(K);
        sb = (sb + 7) & ~size_t{7};
        return reinterpret_cast<const VST*>(
            reinterpret_cast<const char*>(node + header_u64(get_header(node)->skip)) + sb);
    }
};

} // namespace kn3

#endif // KNTRIE_COMPACT_HPP
