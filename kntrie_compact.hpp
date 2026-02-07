#ifndef KNSEARCH_HPP
#define KNSEARCH_HPP

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
// For a complete binary tree of size H (power of 2), maps each Eytzinger
// position to its in-order rank. Used to determine which sorted block
// a key falls into during the tree walk.

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

// Pre-instantiated tables for all power-of-2 sizes up to 512
// (sufficient for BMAX=8 with count up to 4096)
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
// Layout: [pad:1 K-slot][ek:n K-slots][sorted_keys:count K-slots]
//   - pad ensures ek[] starts at index 1 (1-indexed Eytzinger tree)
//   - ek[] holds Eytzinger-ordered samples (every Bact-th key)
//   - n = bit_ceil(ceil(count / BMAX)), always power of 2
//   - Bact = ceil(count / n), varies in [BMAX/2+1, BMAX]
//   - Block indices come from static .rodata table (no per-node eb[])
//
// Search: Eytzinger tree walk O(log n), then linear scan O(BMAX)

template<typename K>
struct EytzSearch {
    static constexpr int BMAX = 8;  // One cache line of u64 or u32

    static int compute_n(int count) noexcept {
        if (count <= BMAX) return 0;
        int n_raw = (count + BMAX - 1) / BMAX;
        return static_cast<int>(std::bit_ceil(static_cast<unsigned>(n_raw)));
    }

    static int compute_bact(int count, int n) noexcept {
        return (count + n - 1) / n;
    }

    // Extra K-slots before the sorted keys
    static int extra(int count) noexcept {
        int n = compute_n(count);
        return n == 0 ? 0 : 1 + n;  // [pad][ek:n]
    }

    // Build search overlay. dest has room for extra(count) + count K-slots.
    // src_keys are count sorted keys.
    static void build(K* dest, const K* src_keys, int count) noexcept {
        int n = compute_n(count);
        if (n == 0) {
            std::memcpy(dest, src_keys, count * sizeof(K));
            return;
        }
        int Bact = compute_bact(count, n);

        // Build Eytzinger tree of samples in dest[1..n]
        K* ek = dest + 1;
        int si = 0;
        auto f = [&](auto& self, int i) -> void {
            if (i > n) return;
            self(self, 2 * i);
            int idx = si * Bact;
            ek[i - 1] = (idx < count) ? src_keys[idx] : std::numeric_limits<K>::max();
            si++;
            self(self, 2 * i + 1);
        };
        f(f, 1);

        // Copy sorted keys after the Eytzinger tree
        std::memcpy(dest + 1 + n, src_keys, count * sizeof(K));
    }

    // Search for key. Returns index into sorted keys, or -1.
    static int search(const K* start, int count, K key) noexcept {
        int n = compute_n(count);
        if (n == 0) {
            // Small count: linear scan
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

        // Eytzinger tree walk
        unsigned i = 1;
        int block = 0;
        while (i <= static_cast<unsigned>(n)) {
            __builtin_prefetch(&ek[4 * i - 1], 0, 0);
            bool r = (key >= ek[i - 1]);
            block = r ? static_cast<int>(blk[i - 1]) : block;
            i = 2 * i + (r ? 1 : 0);
        }

        // Linear scan within block
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
// Layout: [idx1:i1 K-slots][idx2:i2 K-slots][sorted_keys:count K-slots]
//   - idx1 samples every 256th key (only when count > 256)
//   - idx2 samples every 16th key (only when count > 16)
//   - Each level does a linear scan of ≤16 elements
//
// Search: 3-level linear scan, each ≤16 elements

template<typename K>
struct IdxSearch {
    static int idx1_count(int count) noexcept {
        return count > 256 ? (count + 255) / 256 : 0;
    }

    static int idx2_count(int count) noexcept {
        return count > 16 ? (count + 15) / 16 : 0;
    }

    // Extra K-slots before the sorted keys
    static int extra(int count) noexcept {
        return idx1_count(count) + idx2_count(count);
    }

    // Build search indices. dest has room for extra(count) + count K-slots.
    // src_keys are count sorted keys.
    static void build(K* dest, const K* src_keys, int count) noexcept {
        int i1 = idx1_count(count);
        int i2 = idx2_count(count);

        // Write idx1: every 256th key
        for (int i = 0; i < i1; ++i) {
            dest[i] = src_keys[i * 256];
        }

        // Write idx2: every 16th key
        K* d2 = dest + i1;
        for (int i = 0; i < i2; ++i) {
            d2[i] = src_keys[i * 16];
        }

        // Copy sorted keys
        std::memcpy(d2 + i2, src_keys, count * sizeof(K));
    }

    // Linear scan of up to 16 elements, returns last index where *p <= key
    static int subsearch(const K* start, int count, K key) noexcept {
        const K* run = start;
        const K* end = start + count;
        do {
            if (*run > key) break;
            run++;
        } while (run < end);
        return static_cast<int>(run - start) - 1;
    }

    // Search for key. Returns index into sorted keys, or -1.
    static int search(const K* start, int count, K key) noexcept {
        int i1 = idx1_count(count);
        int i2 = idx2_count(count);
        const K* d2 = start + i1;
        const K* keys = d2 + i2;

        int ks = 0;

        if (i1 > 0) {
            int b = subsearch(start, i1, key);
            if (b < 0) return -1;
            d2 += b * 16;
            i2 = std::min(16, i2 - b * 16);
            ks = b * 256;
        }

        if (i2 > 0) {
            int b = subsearch(d2, i2, key);
            if (b < 0) return -1;
            ks += b * 16;
        }

        int kl = std::min(16, count - ks);
        int idx = subsearch(keys + ks, kl, key);
        if (idx >= 0 && keys[ks + idx] == key) {
            return ks + idx;
        }
        return -1;
    }
};

// ==========================================================================
// Unified Search Dispatch
// ==========================================================================
// Selects strategy by key type:
//   sizeof(K) >= 4 → EytzSearch (Static Eytzinger, BMAX=8)
//   sizeof(K) <  4 → IdxSearch  (Old 2-level index)

template<typename K>
struct KnSearch {
    static constexpr bool use_eytzinger = sizeof(K) >= 4;

    using impl = std::conditional_t<use_eytzinger, EytzSearch<K>, IdxSearch<K>>;

    // Number of extra K-sized slots before the sorted keys
    static int extra(int count) noexcept {
        return impl::extra(count);
    }

    // Build search overlay + copy sorted keys.
    // dest must have room for extra(count) + count K-slots.
    // src_keys: count sorted keys.
    static void build(K* dest, const K* src_keys, int count) noexcept {
        impl::build(dest, src_keys, count);
    }

    // Search for key. start points to beginning of search region.
    // Returns index into sorted keys (relative to keys_ptr), or -1.
    static int search(const K* start, int count, K key) noexcept {
        return impl::search(start, count, key);
    }

    // Pointer to the start of sorted keys within the region
    static K* keys_ptr(K* start, int count) noexcept {
        return start + extra(count);
    }

    static const K* keys_ptr(const K* start, int count) noexcept {
        return start + extra(count);
    }
};

} // namespace kn3

#endif // KNSEARCH_HPP