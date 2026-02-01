#ifndef KNTRIE3_HPP
#define KNTRIE3_HPP

#include <cstdint>
#include <cstring>
#include <bit>
#include <memory>
#include <type_traits>
#include <utility>
#include <algorithm>
#include <cassert>
#include <array>

namespace kn3 {

// 256-bit bitmap operations
struct Bitmap256 {
    uint64_t words[4] = {0, 0, 0, 0};
    
    bool find_slot(uint8_t index, int& slot) const noexcept {
        const int word = index >> 6;
        const int bit = index & 63;
        
        uint64_t before = words[word] << (63 - bit);
        if (!(before & (1ULL << 63))) return false;
        
        int pc0 = std::popcount(words[0]);
        int pc1 = std::popcount(words[1]);
        int pc2 = std::popcount(words[2]);
        
        slot = std::popcount(before) - 1;
        slot += pc0 & -int(word > 0);
        slot += pc1 & -int(word > 1);
        slot += pc2 & -int(word > 2);
        
        return true;
    }
    
    bool has_bit(uint8_t index) const noexcept {
        return words[index >> 6] & (1ULL << (index & 63));
    }
    
    void set_bit(uint8_t index) noexcept {
        words[index >> 6] |= (1ULL << (index & 63));
    }
    
    void clear_bit(uint8_t index) noexcept {
        words[index >> 6] &= ~(1ULL << (index & 63));
    }
    
    int popcount() const noexcept {
        return std::popcount(words[0]) + std::popcount(words[1]) +
               std::popcount(words[2]) + std::popcount(words[3]);
    }
    
    int slot_for_insert(uint8_t index) const noexcept {
        const int word = index >> 6;
        const int bit = index & 63;
        
        int pc0 = std::popcount(words[0]);
        int pc1 = std::popcount(words[1]);
        int pc2 = std::popcount(words[2]);
        
        int slot = std::popcount(words[word] & ((1ULL << bit) - 1));
        slot += pc0 & -int(word > 0);
        slot += pc1 & -int(word > 1);
        slot += pc2 & -int(word > 2);
        
        return slot;
    }
    
    // Count set bits below the given index (not including it)
    int count_below(uint8_t index) const noexcept {
        return slot_for_insert(index);  // Same logic
    }
    
    int find_next_set(int start) const noexcept {
        if (start >= 256) return -1;
        
        int word = start >> 6;
        int bit = start & 63;
        
        uint64_t mask = ~((1ULL << bit) - 1);
        uint64_t masked = words[word] & mask;
        if (masked) {
            return (word << 6) + std::countr_zero(masked);
        }
        
        for (int w = word + 1; w < 4; ++w) {
            if (words[w]) {
                return (w << 6) + std::countr_zero(words[w]);
            }
        }
        return -1;
    }
};

template<typename KEY, typename VALUE, typename ALLOC = std::allocator<uint64_t>>
class kntrie3 {
    static_assert(std::is_integral_v<KEY>, "KEY must be an integral type");
    
public:
    using key_type = KEY;
    using mapped_type = VALUE;
    using size_type = std::size_t;
    using allocator_type = ALLOC;
    
private:
    static constexpr bool is_signed_key = std::is_signed_v<KEY>;
    static constexpr size_t key_bits = sizeof(KEY) * 8;
    static constexpr bool value_inline = sizeof(VALUE) <= 8 && std::is_trivially_copyable_v<VALUE>;
    
    using value_slot_type = std::conditional_t<value_inline, VALUE, VALUE*>;
    using value_alloc_type = typename std::allocator_traits<ALLOC>::template rebind_alloc<VALUE>;
    
    // ==========================================================================
    // Node Header (16 bytes)
    // ==========================================================================
    struct NodeHeader {
        uint32_t count;      // Total entries
        uint16_t top_count;  // For split: number of top-level entries
        uint8_t skip;        // Skip compression levels (each = 16 bits)
        uint8_t flags;       // Bit 0: is_leaf, Bit 1: is_split
        uint64_t prefix;     // Skip prefix
        
        bool is_leaf() const noexcept { return flags & 1; }
        bool is_split() const noexcept { return flags & 2; }
        void set_leaf(bool v) noexcept { flags = (flags & ~1) | (v ? 1 : 0); }
        void set_split(bool v) noexcept { flags = (flags & ~2) | (v ? 2 : 0); }
    };
    
    static_assert(sizeof(NodeHeader) == 16, "NodeHeader must be 16 bytes");
    
    static NodeHeader* get_header(uint64_t* node) noexcept {
        return reinterpret_cast<NodeHeader*>(node);
    }
    static const NodeHeader* get_header(const uint64_t* node) noexcept {
        return reinterpret_cast<const NodeHeader*>(node);
    }
    
    static constexpr size_t HEADER_U64 = 2;  // 16 bytes
    static constexpr size_t BITMAP256_U64 = 4;  // 32 bytes
    static constexpr size_t COMPACT_MAX = 4096;
    static constexpr size_t BOT_LEAF_MAX = 4096;
    
    // ==========================================================================
    // Idx Search (cache-friendly alternative to binary search)
    // Layout: [idx1][idx2][keys] where idx1 samples every 256, idx2 every 16
    // ==========================================================================
    
    template<typename K>
    static int idx_subsearch(const K* start, int count, K key) noexcept {
        [[assume(count <= 16)]];
        const K* run = start;
        const K* end = start + count;
        do {
            if (*run > key) break;
            run++;
        } while (run < end);
        return static_cast<int>(run - start) - 1;
    }
    
    // Search in indexed layout [idx1][idx2][keys], returns index into keys or -1
    template<typename K>
    static int idx_search(const K* start, int count, K key) noexcept {
        [[assume(count <= 4096)]];
        
        int idx1_size = idx1_count(count);
        int idx2_size = idx2_count(count);
        const K* idx2 = start + idx1_size;
        const K* keys = idx2 + idx2_size;
        
        int key_start = 0;
        
        if (idx1_size > 0) {
            int b1 = idx_subsearch(start, idx1_size, key);
            if (b1 < 0) return -1;
            
            idx2 = idx2 + b1 * 16;
            idx2_size = std::min(16, idx2_size - b1 * 16);
            key_start = b1 * 256;
        }
        
        if (idx2_size > 0) {
            int b2 = idx_subsearch(idx2, idx2_size, key);
            if (b2 < 0) return -1;
            key_start += b2 * 16;
        }
        
        int key_len = std::min(16, count - key_start);
        int idx = idx_subsearch(keys + key_start, key_len, key);
        if (idx >= 0 && keys[key_start + idx] == key) {
            return key_start + idx;
        }
        return -1;
    }
    
    // Calculate idx sizes - use ceiling division to cover all entries
    static constexpr int idx1_count(int count) noexcept {
        return count > 256 ? (count + 255) / 256 : 0;
    }
    
    static constexpr int idx2_count(int count) noexcept {
        return count > 16 ? (count + 15) / 16 : 0;
    }
    
    static constexpr int idx_total(int count) noexcept {
        return idx1_count(count) + idx2_count(count);
    }
    
    // Build indices from sorted keys into pre-allocated buffer [idx1][idx2][keys]
    template<typename K>
    static void build_indices(K* dest, const K* src_keys, int count) noexcept {
        int i1_size = idx1_count(count);
        int i2_size = idx2_count(count);
        
        // Write idx1
        for (int i = 0; i < i1_size; ++i) {
            dest[i] = src_keys[i * 256];
        }
        
        // Write idx2
        K* idx2_dest = dest + i1_size;
        for (int i = 0; i < i2_size; ++i) {
            idx2_dest[i] = src_keys[i * 16];
        }
        
        // Write keys
        K* keys_dest = idx2_dest + i2_size;
        std::memcpy(keys_dest, src_keys, count * sizeof(K));
    }
    
    // For insert: returns index if found, or -(insertion_point + 1) if not found
    // Uses binary search on the keys portion (after indices)
    template<typename K>
    static int binary_search_for_insert(const K* keys_data, size_t count, K target) noexcept {
        auto it = std::lower_bound(keys_data, keys_data + count, target);
        if (it != keys_data + count && *it == target) {
            return static_cast<int>(it - keys_data);
        }
        return -(static_cast<int>(it - keys_data) + 1);
    }
    
    // Insert into sorted array, shifting elements
    template<typename K>
    static void sorted_insert(K* keys, value_slot_type* values, size_t count, size_t insert_pos, K key, value_slot_type value) noexcept {
        // Shift elements right
        for (size_t i = count; i > insert_pos; --i) {
            keys[i] = keys[i - 1];
            values[i] = values[i - 1];
        }
        keys[insert_pos] = key;
        values[insert_pos] = value;
    }
    
    // ==========================================================================
    // Key Conversion  
    // ==========================================================================
    
    static constexpr uint64_t key_to_internal(KEY k) noexcept {
        uint64_t result;
        if constexpr (sizeof(KEY) == 1) {
            result = static_cast<uint8_t>(k);
        } else if constexpr (sizeof(KEY) == 2) {
            result = static_cast<uint16_t>(k);
        } else if constexpr (sizeof(KEY) == 4) {
            result = static_cast<uint32_t>(k);
        } else {
            result = static_cast<uint64_t>(k);
        }
        if constexpr (is_signed_key) {
            constexpr uint64_t sign_bit = 1ULL << (key_bits - 1);
            result ^= sign_bit;
        }
        result <<= (64 - key_bits);
        return result;
    }
    
    static constexpr KEY internal_to_key(uint64_t internal) noexcept {
        internal >>= (64 - key_bits);
        if constexpr (is_signed_key) {
            constexpr uint64_t sign_bit = 1ULL << (key_bits - 1);
            internal ^= sign_bit;
        }
        return static_cast<KEY>(internal);
    }
    
    // ==========================================================================
    // Bit Extraction
    // ==========================================================================
    
    // Extract top 8 bits of remaining BITS
    // Internal key has actual key bits in positions [63 down to 64-key_bits]
    // At level where BITS remain, we've consumed (key_bits - BITS) bits already
    template<int BITS>
    static constexpr uint8_t extract_top8(uint64_t ik) noexcept {
        static_assert(BITS >= 8 && BITS <= 64);
        if constexpr (BITS > static_cast<int>(key_bits)) {
            // Invalid - BITS should never exceed key_bits in real usage
            return 0;
        } else {
            // Top 8 bits of the BITS portion = shift by (64 - (key_bits - BITS) - 8) = (56 - key_bits + BITS)
            constexpr int shift = 56 - static_cast<int>(key_bits) + BITS;
            return static_cast<uint8_t>((ik >> shift) & 0xFF);
        }
    }
    
    // Extract BITS-width suffix (all remaining bits at this level)
    // The suffix occupies positions [64-key_bits+BITS-1 down to 64-key_bits]
    template<int BITS>
    static constexpr uint64_t extract_suffix(uint64_t ik) noexcept {
        if constexpr (BITS >= 64) {
            return ik;
        } else if constexpr (BITS > static_cast<int>(key_bits)) {
            // Invalid - shouldn't happen
            return 0;
        } else {
            // Shift right to align suffix to bit 0, then mask
            constexpr int shift = 64 - static_cast<int>(key_bits);
            constexpr uint64_t mask = (1ULL << BITS) - 1;
            return (ik >> shift) & mask;
        }
    }
    
    // Extract skip prefix (top skip*16 bits of the remaining BITS portion)
    template<int BITS>
    static constexpr uint64_t extract_prefix(uint64_t ik, int skip) noexcept {
        if constexpr (BITS > static_cast<int>(key_bits)) {
            return 0;
        } else {
            int prefix_bits = skip * 16;
            // Prefix is at top of the BITS portion, which starts at position (64 - key_bits + BITS - 1)
            int shift = 64 - static_cast<int>(key_bits) + BITS - prefix_bits;
            uint64_t mask = (1ULL << prefix_bits) - 1;
            return (ik >> shift) & mask;
        }
    }
    
    // ==========================================================================
    // Suffix traits for storage type selection
    // ==========================================================================
    
    template<int BITS>
    struct suffix_traits {
        using type = std::conditional_t<BITS <= 8, uint8_t,
                     std::conditional_t<BITS <= 16, uint16_t,
                     std::conditional_t<BITS <= 32, uint32_t, uint64_t>>>;
        static constexpr size_t size = sizeof(type);
    };
    
    // ==========================================================================
    // Node Layout Sizes
    // ==========================================================================
    
    // Compact leaf: [header][idx1...][idx2...][keys...][values...]
    template<int BITS>
    static constexpr size_t leaf_compact_size_u64(size_t count) noexcept {
        using K = typename suffix_traits<BITS>::type;
        int i1 = idx1_count(static_cast<int>(count));
        int i2 = idx2_count(static_cast<int>(count));
        size_t idx_bytes = (i1 + i2) * sizeof(K);
        idx_bytes = (idx_bytes + 7) & ~size_t{7};
        size_t key_bytes = count * sizeof(K);
        key_bytes = (key_bytes + 7) & ~size_t{7};
        size_t val_bytes = count * sizeof(value_slot_type);
        val_bytes = (val_bytes + 7) & ~size_t{7};
        return HEADER_U64 + (idx_bytes + key_bytes + val_bytes) / 8;
    }
    
    // Split top: [header][top_bm_256][bot_is_leaf_bm_256][bot_ptrs...]
    // At BITS=16: [header][top_bm_256][bot_ptrs...] (no bot_is_leaf needed)
    template<int BITS>
    static constexpr size_t split_top_size_u64(size_t top_count) noexcept {
        if constexpr (BITS == 16) {
            return HEADER_U64 + BITMAP256_U64 + top_count;
        } else {
            return HEADER_U64 + BITMAP256_U64 + BITMAP256_U64 + top_count;
        }
    }
    
    // Bottom LEAF: 
    //   BITS=16: [bm_256][values...] - bitmap indicates which 8-bit suffixes are present
    //   BITS>16: [count:u32 padded][idx1...][idx2...][(BITS-8)-bit suffixes...][values...]
    template<int BITS>
    static constexpr size_t bot_leaf_size_u64(size_t count) noexcept {
        if constexpr (BITS == 16) {
            // Bitmap-based: [bm_256][values...]
            size_t val_bytes = count * sizeof(value_slot_type);
            val_bytes = (val_bytes + 7) & ~size_t{7};
            return BITMAP256_U64 + val_bytes / 8;
        } else {
            // List-based with idx: [count:u32 padded][idx1][idx2][suffixes...][values...]
            constexpr int suffix_bits = BITS - 8;
            using S = typename suffix_traits<suffix_bits>::type;
            int i1 = idx1_count(static_cast<int>(count));
            int i2 = idx2_count(static_cast<int>(count));
            size_t idx_bytes = (i1 + i2) * sizeof(S);
            idx_bytes = (idx_bytes + 7) & ~size_t{7};
            size_t suffix_bytes = count * sizeof(S);
            suffix_bytes = (suffix_bytes + 7) & ~size_t{7};
            size_t val_bytes = count * sizeof(value_slot_type);
            val_bytes = (val_bytes + 7) & ~size_t{7};
            return 1 + (idx_bytes + suffix_bytes) / 8 + val_bytes / 8;
        }
    }
    
    // Bottom INTERNAL: [bm_256][child_ptrs...]
    static constexpr size_t bot_internal_size_u64(size_t count) noexcept {
        return BITMAP256_U64 + count;
    }
    
    // ==========================================================================
    // Node Accessors
    // ==========================================================================
    
    // Compact leaf accessors
    // Layout: [header][idx1...][idx2...][keys...][values...]
    // leaf_keys returns start of [idx1][idx2][keys] region for idx_search
    template<int BITS>
    static auto leaf_keys(uint64_t* node) noexcept {
        using K = typename suffix_traits<BITS>::type;
        return reinterpret_cast<K*>(node + HEADER_U64);
    }
    
    template<int BITS>
    static auto leaf_keys(const uint64_t* node) noexcept {
        using K = typename suffix_traits<BITS>::type;
        return reinterpret_cast<const K*>(node + HEADER_U64);
    }
    
    // Returns pointer to actual keys (after idx1 and idx2)
    template<int BITS>
    static auto leaf_keys_data(uint64_t* node, size_t count) noexcept {
        using K = typename suffix_traits<BITS>::type;
        int idx_total_count = idx_total(static_cast<int>(count));
        return reinterpret_cast<K*>(node + HEADER_U64) + idx_total_count;
    }
    
    template<int BITS>
    static auto leaf_keys_data(const uint64_t* node, size_t count) noexcept {
        using K = typename suffix_traits<BITS>::type;
        int idx_total_count = idx_total(static_cast<int>(count));
        return reinterpret_cast<const K*>(node + HEADER_U64) + idx_total_count;
    }
    
    template<int BITS>
    static value_slot_type* leaf_values(uint64_t* node, size_t count) noexcept {
        using K = typename suffix_traits<BITS>::type;
        int i1 = idx1_count(static_cast<int>(count));
        int i2 = idx2_count(static_cast<int>(count));
        size_t idx_bytes = (i1 + i2) * sizeof(K);
        idx_bytes = (idx_bytes + 7) & ~size_t{7};
        size_t key_bytes = count * sizeof(K);
        key_bytes = (key_bytes + 7) & ~size_t{7};
        return reinterpret_cast<value_slot_type*>(reinterpret_cast<char*>(node + HEADER_U64) + idx_bytes + key_bytes);
    }
    
    template<int BITS>
    static const value_slot_type* leaf_values(const uint64_t* node, size_t count) noexcept {
        using K = typename suffix_traits<BITS>::type;
        int i1 = idx1_count(static_cast<int>(count));
        int i2 = idx2_count(static_cast<int>(count));
        size_t idx_bytes = (i1 + i2) * sizeof(K);
        idx_bytes = (idx_bytes + 7) & ~size_t{7};
        size_t key_bytes = count * sizeof(K);
        key_bytes = (key_bytes + 7) & ~size_t{7};
        return reinterpret_cast<const value_slot_type*>(reinterpret_cast<const char*>(node + HEADER_U64) + idx_bytes + key_bytes);
    }
    
    // Split top accessors
    static Bitmap256& top_bitmap(uint64_t* node) noexcept {
        return *reinterpret_cast<Bitmap256*>(node + HEADER_U64);
    }
    
    static const Bitmap256& top_bitmap(const uint64_t* node) noexcept {
        return *reinterpret_cast<const Bitmap256*>(node + HEADER_U64);
    }
    
    // bot_is_leaf bitmap (only for BITS > 16)
    static Bitmap256& bot_is_leaf_bitmap(uint64_t* node) noexcept {
        return *reinterpret_cast<Bitmap256*>(node + HEADER_U64 + BITMAP256_U64);
    }
    
    static const Bitmap256& bot_is_leaf_bitmap(const uint64_t* node) noexcept {
        return *reinterpret_cast<const Bitmap256*>(node + HEADER_U64 + BITMAP256_U64);
    }
    
    template<int BITS>
    static uint64_t* top_children(uint64_t* node) noexcept {
        if constexpr (BITS == 16) {
            return node + HEADER_U64 + BITMAP256_U64;
        } else {
            return node + HEADER_U64 + BITMAP256_U64 + BITMAP256_U64;
        }
    }
    
    template<int BITS>
    static const uint64_t* top_children(const uint64_t* node) noexcept {
        if constexpr (BITS == 16) {
            return node + HEADER_U64 + BITMAP256_U64;
        } else {
            return node + HEADER_U64 + BITMAP256_U64 + BITMAP256_U64;
        }
    }
    
    // Bottom LEAF accessors:
    //   BITS=16: [bm_256][values...] - bitmap based, O(1) lookup
    //   BITS>16: [count:u32 padded][(BITS-8)-bit suffixes...][values...] - list based
    
    // For BITS=16: access the bitmap
    template<int BITS>
    static Bitmap256& bot_leaf_bitmap(uint64_t* bot) noexcept {
        static_assert(BITS == 16, "bot_leaf_bitmap only for BITS=16");
        return *reinterpret_cast<Bitmap256*>(bot);
    }
    
    template<int BITS>
    static const Bitmap256& bot_leaf_bitmap(const uint64_t* bot) noexcept {
        static_assert(BITS == 16, "bot_leaf_bitmap only for BITS=16");
        return *reinterpret_cast<const Bitmap256*>(bot);
    }
    
    template<int BITS>
    static uint32_t bot_leaf_count(const uint64_t* bot) noexcept {
        if constexpr (BITS == 16) {
            return bot_leaf_bitmap<16>(bot).popcount();
        } else {
            return *reinterpret_cast<const uint32_t*>(bot);
        }
    }
    
    template<int BITS>
    static void set_bot_leaf_count(uint64_t* bot, uint32_t count) noexcept {
        static_assert(BITS != 16, "set_bot_leaf_count not used for BITS=16 (count derived from bitmap)");
        *reinterpret_cast<uint32_t*>(bot) = count;
    }
    
    // Suffixes only for BITS > 16 (list-based)
    // Returns start of [idx1][idx2][suffixes] region for idx_search
    template<int BITS>
    static auto bot_leaf_suffixes(uint64_t* bot) noexcept {
        static_assert(BITS > 16, "bot_leaf_suffixes only for BITS>16");
        constexpr int suffix_bits = BITS - 8;
        using S = typename suffix_traits<suffix_bits>::type;
        return reinterpret_cast<S*>(bot + 1);  // After count
    }
    
    template<int BITS>
    static auto bot_leaf_suffixes(const uint64_t* bot) noexcept {
        static_assert(BITS > 16, "bot_leaf_suffixes only for BITS>16");
        constexpr int suffix_bits = BITS - 8;
        using S = typename suffix_traits<suffix_bits>::type;
        return reinterpret_cast<const S*>(bot + 1);
    }
    
    // Returns pointer to actual suffix data (after idx1 and idx2)
    template<int BITS>
    static auto bot_leaf_suffixes_data(uint64_t* bot, size_t count) noexcept {
        static_assert(BITS > 16, "bot_leaf_suffixes_data only for BITS>16");
        constexpr int suffix_bits = BITS - 8;
        using S = typename suffix_traits<suffix_bits>::type;
        int idx_total_count = idx_total(static_cast<int>(count));
        return reinterpret_cast<S*>(bot + 1) + idx_total_count;
    }
    
    template<int BITS>
    static auto bot_leaf_suffixes_data(const uint64_t* bot, size_t count) noexcept {
        static_assert(BITS > 16, "bot_leaf_suffixes_data only for BITS>16");
        constexpr int suffix_bits = BITS - 8;
        using S = typename suffix_traits<suffix_bits>::type;
        int idx_total_count = idx_total(static_cast<int>(count));
        return reinterpret_cast<const S*>(bot + 1) + idx_total_count;
    }
    
    template<int BITS>
    static value_slot_type* bot_leaf_values(uint64_t* bot, [[maybe_unused]] size_t count) noexcept {
        if constexpr (BITS == 16) {
            return reinterpret_cast<value_slot_type*>(bot + BITMAP256_U64);
        } else {
            constexpr int suffix_bits = BITS - 8;
            using S = typename suffix_traits<suffix_bits>::type;
            int i1 = idx1_count(static_cast<int>(count));
            int i2 = idx2_count(static_cast<int>(count));
            size_t idx_bytes = (i1 + i2) * sizeof(S);
            idx_bytes = (idx_bytes + 7) & ~size_t{7};
            size_t suffix_bytes = count * sizeof(S);
            suffix_bytes = (suffix_bytes + 7) & ~size_t{7};
            return reinterpret_cast<value_slot_type*>(reinterpret_cast<char*>(bot + 1) + idx_bytes + suffix_bytes);
        }
    }
    
    template<int BITS>
    static const value_slot_type* bot_leaf_values(const uint64_t* bot, [[maybe_unused]] size_t count) noexcept {
        if constexpr (BITS == 16) {
            return reinterpret_cast<const value_slot_type*>(bot + BITMAP256_U64);
        } else {
            constexpr int suffix_bits = BITS - 8;
            using S = typename suffix_traits<suffix_bits>::type;
            int i1 = idx1_count(static_cast<int>(count));
            int i2 = idx2_count(static_cast<int>(count));
            size_t idx_bytes = (i1 + i2) * sizeof(S);
            idx_bytes = (idx_bytes + 7) & ~size_t{7};
            size_t suffix_bytes = count * sizeof(S);
            suffix_bytes = (suffix_bytes + 7) & ~size_t{7};
            return reinterpret_cast<const value_slot_type*>(reinterpret_cast<const char*>(bot + 1) + idx_bytes + suffix_bytes);
        }
    }
    
    // Bottom INTERNAL accessors: [bm_256][child_ptrs...]
    static Bitmap256& bot_bitmap(uint64_t* bot) noexcept {
        return *reinterpret_cast<Bitmap256*>(bot);
    }
    
    static const Bitmap256& bot_bitmap(const uint64_t* bot) noexcept {
        return *reinterpret_cast<const Bitmap256*>(bot);
    }
    
    static uint64_t* bot_internal_children(uint64_t* bot) noexcept {
        return bot + BITMAP256_U64;
    }
    
    static const uint64_t* bot_internal_children(const uint64_t* bot) noexcept {
        return bot + BITMAP256_U64;
    }
    
    // ==========================================================================
    // Allocation
    // ==========================================================================
    
    uint64_t* alloc_node(size_t u64_count) {
        uint64_t* node = alloc_.allocate(u64_count);
        std::memset(node, 0, u64_count * sizeof(uint64_t));
        return node;
    }
    
    void dealloc_node(uint64_t* node, size_t u64_count) noexcept {
        alloc_.deallocate(node, u64_count);
    }
    
    // ==========================================================================
    // Value Storage
    // ==========================================================================
    
    value_slot_type store_value(const VALUE& val) {
        if constexpr (value_inline) {
            return val;
        } else {
            value_alloc_type va(alloc_);
            VALUE* ptr = std::allocator_traits<value_alloc_type>::allocate(va, 1);
            std::allocator_traits<value_alloc_type>::construct(va, ptr, val);
            return reinterpret_cast<uint64_t>(ptr);
        }
    }
    
    VALUE load_value(value_slot_type stored) const noexcept {
        if constexpr (value_inline) {
            return stored;
        } else {
            return *reinterpret_cast<VALUE*>(stored);
        }
    }
    
    void destroy_value(value_slot_type stored) noexcept {
        if constexpr (!value_inline) {
            value_alloc_type va(alloc_);
            VALUE* ptr = reinterpret_cast<VALUE*>(stored);
            std::allocator_traits<value_alloc_type>::destroy(va, ptr);
            std::allocator_traits<value_alloc_type>::deallocate(va, ptr, 1);
        }
    }
    
    // ==========================================================================
    // Member Data
    // ==========================================================================
    
    uint64_t* root_;
    size_t size_;
    [[no_unique_address]] ALLOC alloc_;
    
public:
    // ==========================================================================
    // Constructor / Destructor
    // ==========================================================================
    
    kntrie3() : size_(0), alloc_() {
        root_ = alloc_node(leaf_compact_size_u64<static_cast<int>(key_bits)>(0));
        NodeHeader* h = get_header(root_);
        h->count = 0;
        h->set_leaf(true);
    }
    
    ~kntrie3() {
        remove_all();
    }
    
    kntrie3(const kntrie3&) = delete;
    kntrie3& operator=(const kntrie3&) = delete;
    
    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }
    [[nodiscard]] size_type size() const noexcept { return size_; }
    
    void clear() noexcept {
        remove_all();
        root_ = alloc_node(leaf_compact_size_u64<static_cast<int>(key_bits)>(0));
        NodeHeader* h = get_header(root_);
        h->count = 0;
        h->skip = 0;
        h->prefix = 0;
        h->set_leaf(true);
        h->set_split(false);
        size_ = 0;
    }
    
    void remove_all() noexcept {
        if (root_) {
            remove_all_impl<static_cast<int>(key_bits)>(root_);
            root_ = nullptr;
        }
        size_ = 0;
    }
    
private:
    template<int BITS>
    void remove_all_impl(uint64_t* node) noexcept {
        if constexpr (BITS <= 0) {
            return;
        } else {
            if (!node) return;
            
            NodeHeader* h = get_header(node);
            
            // Handle skip - dispatch to correct bit width
            if (h->skip > 0) {
                int actual_bits = BITS - h->skip * 16;
                if (actual_bits == 48) { remove_all_at_bits<48>(node, h); return; }
                if (actual_bits == 32) { remove_all_at_bits<32>(node, h); return; }
                if (actual_bits == 16) { remove_all_at_bits<16>(node, h); return; }
                // Fall through - shouldn't happen but handle gracefully
                return;
            }
            
            remove_all_at_bits<BITS>(node, h);
        }
    }
    
    template<int BITS>
    void remove_all_at_bits(uint64_t* node, NodeHeader* h) noexcept {
        if constexpr (BITS <= 0) {
            return;
        } else {
            if (h->is_leaf() && !h->is_split()) {
                // Compact leaf
                if constexpr (!value_inline) {
                    value_slot_type* values = leaf_values<BITS>(node, h->count);
                    for (uint32_t i = 0; i < h->count; ++i) {
                        destroy_value(static_cast<value_slot_type>(values[i]));
                    }
                }
                dealloc_node(node, leaf_compact_size_u64<BITS>(h->count));
            } else if (h->is_split()) {
                remove_all_split<BITS>(node, h);
            }
        }
    }
    
    template<int BITS>
    void remove_all_split(uint64_t* node, NodeHeader* h) noexcept {
        if constexpr (BITS <= 0) {
            return;
        } else {
            const Bitmap256& top_bm = top_bitmap(node);
            uint64_t* top_ch = top_children<BITS>(node);
            
            int slot = 0;
            for (int idx = top_bm.find_next_set(0); idx >= 0; idx = top_bm.find_next_set(idx + 1)) {
                uint64_t* bot = reinterpret_cast<uint64_t*>(top_ch[slot]);
                
                bool is_leaf;
                if constexpr (BITS == 16) {
                    is_leaf = true;
                } else {
                    is_leaf = bot_is_leaf_bitmap(node).has_bit(idx);
                }
                
                if (is_leaf) {
                    uint32_t bot_count = bot_leaf_count<BITS>(bot);
                    if constexpr (!value_inline) {
                        value_slot_type* values = bot_leaf_values<BITS>(bot, bot_count);
                        for (uint32_t i = 0; i < bot_count; ++i) {
                            destroy_value(static_cast<value_slot_type>(values[i]));
                        }
                    }
                    dealloc_node(bot, bot_leaf_size_u64<BITS>(bot_count));
                } else {
                    if constexpr (BITS > 16) {
                        const Bitmap256& bot_bm = bot_bitmap(bot);
                        int bot_count = bot_bm.popcount();
                        uint64_t* children = bot_internal_children(bot);
                        
                        for (int i = 0; i < bot_count; ++i) {
                            remove_all_impl<BITS - 16>(reinterpret_cast<uint64_t*>(children[i]));
                        }
                        dealloc_node(bot, bot_internal_size_u64(bot_count));
                    }
                }
                ++slot;
            }
            dealloc_node(node, split_top_size_u64<BITS>(h->top_count));
        }
    }
    
public:
    // Debug functions
    struct RootInfo {
        uint32_t count;
        uint16_t top_count;
        uint8_t skip;
        bool is_leaf;
        bool is_split;
        uint64_t prefix;
    };
    
    RootInfo debug_root_info() const {
        const NodeHeader* h = get_header(root_);
        return {h->count, h->top_count, h->skip, h->is_leaf(), h->is_split(), h->prefix};
    }
    
    uint64_t debug_key_to_internal(KEY k) const {
        return key_to_internal(k);
    }
    
    struct BitmapInfo {
        int popcount;
        int set_bits[256];
    };
    
    BitmapInfo debug_top_bitmap() const {
        BitmapInfo info{};
        const Bitmap256& bm = top_bitmap(root_);
        info.popcount = bm.popcount();
        int idx = 0;
        for (int i = 0; i < 256 && idx < info.popcount; ++i) {
            if (bm.has_bit(i)) {
                info.set_bits[idx++] = i;
            }
        }
        return info;
    }
    
    struct FindTrace {
        uint8_t top_idx;
        bool top_found;
        int top_slot;
        uint64_t bot_ptr;
        uint64_t bot_word0;
        uint64_t bot_word1;
        uint32_t bot_count;
        int search_result;
    };
    
    FindTrace debug_find_trace(KEY k) const {
        FindTrace t{};
        uint64_t ik = key_to_internal(k);
        
        // Assuming SPLIT at 64 bits
        t.top_idx = static_cast<uint8_t>(ik >> 56);
        
        const Bitmap256& top_bm = top_bitmap(root_);
        t.top_found = top_bm.find_slot(t.top_idx, t.top_slot);
        
        if (t.top_found) {
            const uint64_t* top_ch = top_children<64>(root_);
            const uint64_t* bot = reinterpret_cast<const uint64_t*>(top_ch[t.top_slot]);
            t.bot_ptr = reinterpret_cast<uint64_t>(bot);
            t.bot_word0 = bot[0];
            t.bot_word1 = bot[1];
            t.bot_count = bot_leaf_count<64>(bot);
            
            constexpr int suffix_bits = 64 - 8;
            using S = typename suffix_traits<suffix_bits>::type;
            S suffix = static_cast<S>(ik & ((1ULL << suffix_bits) - 1));
            
            const S* suffixes = bot_leaf_suffixes<64>(bot);  // Points to [idx1][idx2][suffixes]
            t.search_result = idx_search(suffixes, static_cast<int>(t.bot_count), suffix);
        }
        
        return t;
    }
    
    // Debug: get first N words of root
    std::array<uint64_t, 20> debug_dump_root() const {
        std::array<uint64_t, 20> result{};
        for (int i = 0; i < 20; ++i) {
            result[i] = root_[i];
        }
        return result;
    }
    
    // ==========================================================================
    // Find
    // ==========================================================================
    
    const VALUE* find_value(const KEY& key) const noexcept {
        uint64_t ik = key_to_internal(key);
        NodeHeader h = *get_header(root_);
        return find_impl<static_cast<int>(key_bits)>(root_, ik, h, 0, 0);
    }
    
    bool contains(const KEY& key) const noexcept {
        return find_value(key) != nullptr;
    }
    
private:
    // Extract top 16 bits of remaining BITS portion
    template<int BITS>
    static constexpr uint16_t extract_top16(uint64_t ik) noexcept {
        static_assert(BITS >= 16 && BITS <= 64);
        constexpr int shift = 64 - static_cast<int>(key_bits) + BITS - 16;
        return static_cast<uint16_t>((ik >> shift) & 0xFFFF);
    }
    
    // Get 16-bit chunk from skip prefix
    // skip_left is how many 16-bit chunks remain to check (1-based)
    // Prefix stores skip*16 bits, MSB first
    static constexpr uint16_t get_skip_chunk(uint64_t prefix, int skip, int skip_left) noexcept {
        // skip_left=1 means we want the lowest 16 bits
        // skip_left=skip means we want the highest 16 bits
        int shift = (skip_left - 1) * 16;
        return static_cast<uint16_t>((prefix >> shift) & 0xFFFF);
    }
    
    // Base case: BITS == 16
    template<int BITS>
    const VALUE* find_impl(const uint64_t* node, uint64_t ik, NodeHeader h, 
                           uint64_t skip, int skip_left) const noexcept
        requires (BITS == 16)
    {
        // At BITS=16, we're at terminal level
        // COMPACT is always leaf, BOT_LEAF is bitmap-based
        if (!h.is_split()) {
            return find_in_compact_leaf<16>(node, &h, ik);
        } else {
            return find_in_split_leaf_16(node, ik);
        }
    }
    
    // Recursive case: BITS > 16
    template<int BITS>
    const VALUE* find_impl(const uint64_t* node, uint64_t ik, NodeHeader h,
                           uint64_t skip, int skip_left) const noexcept
        requires (BITS > 16)
    {
        uint16_t key_chunk = extract_top16<BITS>(ik);
        
        if (skip_left > 0) {
            // Consuming skip from earlier
            uint16_t skip_chunk = get_skip_chunk(skip, h.skip, skip_left);
            if (key_chunk != skip_chunk) return nullptr;
            skip_left--;
        } else if (h.skip >= 1) {
            // New node has skip - check top 16 bits
            uint16_t skip_chunk = get_skip_chunk(h.prefix, h.skip, h.skip);
            if (key_chunk != skip_chunk) return nullptr;
            
            if (h.skip > 1) {
                skip = h.prefix;
                skip_left = h.skip - 1;
            }
            h.skip = 0;  // consumed - won't re-trigger
        } else if (h.is_leaf()) {
            // Terminal - find value
            if (!h.is_split()) {
                return find_in_compact_leaf<BITS>(node, &h, ik);
            } else {
                return find_in_split_leaf<BITS>(node, ik);
            }
        } else {
            // Internal - descend
            const uint64_t* child = get_child<BITS>(node, &h, ik);
            if (!child) return nullptr;
            node = child;
            h = *get_header(child);
        }
        
        return find_impl<BITS - 16>(node, ik, h, skip, skip_left);
    }
    
    // Find child pointer in SPLIT internal node
    template<int BITS>
    const uint64_t* get_child(const uint64_t* node, const NodeHeader* h, uint64_t ik) const noexcept {
        // SPLIT internal: [header][top_bm_256][bot_is_leaf_bm_256][bot_ptrs...]
        uint8_t top_idx = extract_top8<BITS>(ik);
        
        const Bitmap256& top_bm = top_bitmap(node);
        int top_slot;
        if (!top_bm.find_slot(top_idx, top_slot)) return nullptr;
        
        const uint64_t* top_ch = top_children<BITS>(node);
        const uint64_t* bot = reinterpret_cast<const uint64_t*>(top_ch[top_slot]);
        
        // Check if bot is leaf
        if (bot_is_leaf_bitmap(node).has_bit(top_idx)) {
            return nullptr;  // Can't descend into leaf
        }
        
        // bot is internal - descend through it
        uint8_t bot_idx = extract_top8<BITS - 8>(ik);
        const Bitmap256& bot_bm = bot_bitmap(bot);
        int bot_slot;
        if (!bot_bm.find_slot(bot_idx, bot_slot)) return nullptr;
        
        const uint64_t* children = bot_internal_children(bot);
        return reinterpret_cast<const uint64_t*>(children[bot_slot]);
    }
    
    template<int BITS>
    const VALUE* find_in_compact_leaf(const uint64_t* node, const NodeHeader* h, uint64_t ik) const noexcept {
        using K = typename suffix_traits<BITS>::type;
        K suffix = static_cast<K>(extract_suffix<BITS>(ik));
        
        const K* keys = leaf_keys<BITS>(node);  // Points to [idx1][idx2][keys]
        const value_slot_type* values = leaf_values<BITS>(node, h->count);
        
        int idx = idx_search(keys, static_cast<int>(h->count), suffix);
        if (idx < 0) return nullptr;
        
        if constexpr (value_inline) {
            return reinterpret_cast<const VALUE*>(&values[idx]);
        } else {
            return values[idx];
        }
    }
    
    // SPLIT node at BITS > 16: check bot_is_leaf_bitmap to determine if bot is leaf or internal
    template<int BITS>
    const VALUE* find_in_split(const uint64_t* node, uint64_t ik) const noexcept 
        requires (BITS > 16)
    {
        uint8_t top_idx = extract_top8<BITS>(ik);
        
        const Bitmap256& top_bm = top_bitmap(node);
        int top_slot;
        if (!top_bm.find_slot(top_idx, top_slot)) {
            return nullptr;
        }
        
        const uint64_t* top_ch = top_children<BITS>(node);
        const uint64_t* bot = reinterpret_cast<const uint64_t*>(top_ch[top_slot]);
        
        // Check if this bottom is a leaf or internal
        bool is_bot_leaf = bot_is_leaf_bitmap(node).has_bit(top_idx);
        
        if (is_bot_leaf) {
            return find_in_bot_leaf<BITS>(bot, ik);
        } else {
            // bot_internal: [bitmap256][child_ptrs...]
            // Lookup in bot bitmap and recurse to child node
            uint8_t bot_idx = extract_top8<BITS - 8>(ik);
            const Bitmap256& bot_bm = bot_bitmap(bot);
            int bot_slot;
            if (!bot_bm.find_slot(bot_idx, bot_slot)) return nullptr;
            
            const uint64_t* children = bot_internal_children(bot);
            const uint64_t* child = reinterpret_cast<const uint64_t*>(children[bot_slot]);
            
            // Recurse with child's header
            NodeHeader child_h = *get_header(child);
            return find_impl<BITS - 16>(child, ik, child_h, 0, 0);
        }
    }
    
    // SPLIT leaf at BITS == 16: simpler structure
    const VALUE* find_in_split_leaf_16(const uint64_t* node, uint64_t ik) const noexcept {
        uint8_t top_idx = extract_top8<16>(ik);
        
        const Bitmap256& top_bm = top_bitmap(node);
        int top_slot;
        if (!top_bm.find_slot(top_idx, top_slot)) return nullptr;
        
        const uint64_t* top_ch = top_children<16>(node);
        const uint64_t* bot = reinterpret_cast<const uint64_t*>(top_ch[top_slot]);
        
        // At 16-bit, bot leaf is bitmap-based
        uint8_t suffix = static_cast<uint8_t>(extract_suffix<8>(ik));
        const Bitmap256& bm = bot_leaf_bitmap<16>(bot);
        
        if (!bm.has_bit(suffix)) return nullptr;
        
        int slot = bm.count_below(suffix);
        const value_slot_type* values = bot_leaf_values<16>(bot, 0);
        
        if constexpr (value_inline) {
            return reinterpret_cast<const VALUE*>(&values[slot]);
        } else {
            return values[slot];
        }
    }
    
    // SPLIT leaf at BITS > 16: top bitmap -> bot_leaf (list-based)
    template<int BITS>
    const VALUE* find_in_split_leaf(const uint64_t* node, uint64_t ik) const noexcept
        requires (BITS > 16)
    {
        uint8_t top_idx = extract_top8<BITS>(ik);
        
        const Bitmap256& top_bm = top_bitmap(node);
        int top_slot;
        if (!top_bm.find_slot(top_idx, top_slot)) return nullptr;
        
        const uint64_t* top_ch = top_children<BITS>(node);
        const uint64_t* bot = reinterpret_cast<const uint64_t*>(top_ch[top_slot]);
        
        return find_in_bot_leaf<BITS>(bot, ik);
    }
    
    template<int BITS>
    const VALUE* find_in_bot_leaf(const uint64_t* bot, uint64_t ik) const noexcept {
        // List-based: [count][idx1][idx2][(BITS-8)-bit suffixes...][values...]
        uint32_t count = bot_leaf_count<BITS>(bot);
        
        constexpr int suffix_bits = BITS - 8;
        using S = typename suffix_traits<suffix_bits>::type;
        S suffix = static_cast<S>(extract_suffix<suffix_bits>(ik));
        
        const S* suffixes = bot_leaf_suffixes<BITS>(bot);  // Points to [idx1][idx2][suffixes]
        const value_slot_type* values = bot_leaf_values<BITS>(bot, count);
        
        int idx = idx_search(suffixes, static_cast<int>(count), suffix);
        if (idx < 0) return nullptr;
        
        if constexpr (value_inline) {
            return reinterpret_cast<const VALUE*>(&values[idx]);
        } else {
            return values[idx];
        }
    }
    
public:
    // ==========================================================================
    // Insert
    // ==========================================================================
    
    std::pair<bool, bool> insert(const KEY& key, const VALUE& value) {
        uint64_t ik = key_to_internal(key);
        value_slot_type sv = store_value(value);
        
        auto [new_root, inserted] = insert_impl<static_cast<int>(key_bits)>(root_, ik, sv);
        root_ = new_root;
        
        if (inserted) {
            ++size_;
            return {true, true};
        } else {
            destroy_value(sv);
            return {true, false};
        }
    }
    
private:
    struct InsertResult {
        uint64_t* node;
        bool inserted;
    };
    
    template<int BITS>
    InsertResult insert_impl(uint64_t* node, uint64_t ik, value_slot_type value)
        requires (BITS <= 0)
    {
        return {node, false};
    }
    
    template<int BITS>
    InsertResult insert_impl(uint64_t* node, uint64_t ik, value_slot_type value)
        requires (BITS > 0)
    {
        NodeHeader* h = get_header(node);
        
        // Handle skip
        if (h->skip > 0) [[unlikely]] {
            uint64_t expected = extract_prefix<BITS>(ik, h->skip);
            if (expected != h->prefix) {
                return split_on_prefix<BITS>(node, h, ik, value, expected);
            }
            
            int actual_bits = BITS - h->skip * 16;
            if (actual_bits == 48) return insert_at_bits<48>(node, h, ik, value);
            if (actual_bits == 32) return insert_at_bits<32>(node, h, ik, value);
            if (actual_bits == 16) return insert_at_bits<16>(node, h, ik, value);
        }
        
        return insert_at_bits<BITS>(node, h, ik, value);
    }
    
    template<int BITS>
    InsertResult insert_at_bits(uint64_t* node, NodeHeader* h, uint64_t ik, value_slot_type value)
        requires (BITS <= 0)
    {
        return {node, false};
    }
    
    template<int BITS>
    InsertResult insert_at_bits(uint64_t* node, NodeHeader* h, uint64_t ik, value_slot_type value)
        requires (BITS > 0)
    {
        if (h->is_leaf() && !h->is_split()) {
            return insert_into_compact_leaf<BITS>(node, h, ik, value);
        } else if (h->is_split()) {
            return insert_into_split<BITS>(node, h, ik, value);
        }
        return {node, false};
    }
    
    template<int BITS>
    InsertResult insert_into_compact_leaf(uint64_t* node, NodeHeader* h, uint64_t ik, value_slot_type value)
        requires (BITS > 0)
    {
        using K = typename suffix_traits<BITS>::type;
        K suffix = static_cast<K>(extract_suffix<BITS>(ik));
        
        // Get pointers to actual keys data (after indices) and values
        K* keys_data = leaf_keys_data<BITS>(node, h->count);
        value_slot_type* values = leaf_values<BITS>(node, h->count);
        
        // Binary search on actual keys for existing or insertion point
        int idx = binary_search_for_insert(keys_data, h->count, suffix);
        
        if (idx >= 0) {
            // Found existing - update value
            if constexpr (!value_inline) {
                destroy_value(static_cast<value_slot_type>(values[idx]));
            }
            if constexpr (value_inline) {
                std::memcpy(&values[idx], &value, sizeof(value_slot_type));
            } else {
                values[idx] = static_cast<uint64_t>(value);
            }
            return {node, false};
        }
        
        // Not found - idx encodes insertion point
        size_t insert_pos = static_cast<size_t>(-(idx + 1));
        
        // Need to insert new
        if (h->count >= COMPACT_MAX) {
            return convert_to_split<BITS>(node, h, ik, value);
        }
        
        // Add entry in sorted position - create new node
        size_t new_count = h->count + 1;
        uint64_t* new_node = alloc_node(leaf_compact_size_u64<BITS>(new_count));
        NodeHeader* new_h = get_header(new_node);
        *new_h = *h;
        new_h->count = static_cast<uint32_t>(new_count);
        
        // Build new keys array with inserted element
        K* new_keys_data = leaf_keys_data<BITS>(new_node, new_count);
        value_slot_type* new_values = leaf_values<BITS>(new_node, new_count);
        
        // Copy before insertion point
        std::memcpy(new_keys_data, keys_data, insert_pos * sizeof(K));
        std::memcpy(new_values, values, insert_pos * sizeof(value_slot_type));
        
        // Insert new entry
        new_keys_data[insert_pos] = suffix;
        if constexpr (value_inline) {
            std::memcpy(&new_values[insert_pos], &value, sizeof(value_slot_type));
        } else {
            new_values[insert_pos] = static_cast<uint64_t>(value);
        }
        
        // Copy after insertion point
        std::memcpy(new_keys_data + insert_pos + 1, keys_data + insert_pos, (h->count - insert_pos) * sizeof(K));
        std::memcpy(new_values + insert_pos + 1, values + insert_pos, (h->count - insert_pos) * sizeof(value_slot_type));
        
        // Build indices from the new keys
        K* new_idx_start = leaf_keys<BITS>(new_node);
        build_indices(new_idx_start, new_keys_data, static_cast<int>(new_count));
        
        dealloc_node(node, leaf_compact_size_u64<BITS>(h->count));
        return {new_node, true};
    }
    
    template<int BITS>
    InsertResult convert_to_split(uint64_t* node, NodeHeader* h, uint64_t ik, value_slot_type value)
        requires (BITS > 0)
    {
        using K = typename suffix_traits<BITS>::type;
        
        K* old_keys = leaf_keys_data<BITS>(node, h->count);
        value_slot_type* old_values = leaf_values<BITS>(node, h->count);
        
        K new_suffix = static_cast<K>(extract_suffix<BITS>(ik));
        
        // Build top bitmap from high 8 bits
        Bitmap256 new_top_bm{};
        uint16_t bucket_counts[256] = {0};
        
        for (uint32_t i = 0; i < h->count; ++i) {
            uint8_t top_idx = static_cast<uint8_t>(old_keys[i] >> (BITS - 8));
            new_top_bm.set_bit(top_idx);
            bucket_counts[top_idx]++;
        }
        uint8_t new_top_idx = static_cast<uint8_t>(new_suffix >> (BITS - 8));
        new_top_bm.set_bit(new_top_idx);
        bucket_counts[new_top_idx]++;
        
        size_t new_top_count = new_top_bm.popcount();
        
        // PREFIX COMPRESSION: If all entries fall into ONE bucket, use skip
        // Only possible when BITS > 16 (need at least 16 bits to skip)
        if constexpr (BITS > 16) {
            if (new_top_count == 1) {
                // All entries share same high 8 bits - check next 8 bits too
                // We can skip entire 16-bit levels if they're uniform
                
                // Build bitmap for bottom 8 bits (within the top bucket)
                Bitmap256 bot_bm{};
                constexpr int suffix_bits = BITS - 8;
                for (uint32_t i = 0; i < h->count; ++i) {
                    uint8_t bot_idx = static_cast<uint8_t>(old_keys[i] >> (suffix_bits - 8));
                    bot_bm.set_bit(bot_idx);
                }
                uint8_t new_bot_idx = static_cast<uint8_t>(new_suffix >> (suffix_bits - 8));
                bot_bm.set_bit(new_bot_idx);
                
                if (bot_bm.popcount() == 1) {
                    // Entire 16-bit level is uniform - accumulate skip and recurse
                    uint8_t skip_prefix_hi = new_top_idx;
                    uint8_t skip_prefix_lo = new_bot_idx;
                    uint16_t skip_prefix = (static_cast<uint16_t>(skip_prefix_hi) << 8) | skip_prefix_lo;
                    
                    // Shift all suffixes down by 16 bits and recurse
                    constexpr int child_bits = BITS - 16;
                    constexpr uint64_t child_mask = (1ULL << child_bits) - 1;
                    
                    // Collect entries with shifted keys
                    size_t total_count = h->count + 1;
                    std::unique_ptr<uint64_t[]> child_suffixes(new uint64_t[total_count]);
                    std::unique_ptr<value_slot_type[]> child_values(new value_slot_type[total_count]);
                    
                    for (uint32_t i = 0; i < h->count; ++i) {
                        child_suffixes[i] = static_cast<uint64_t>(old_keys[i]) & child_mask;
                        child_values[i] = old_values[i];
                    }
                    child_suffixes[h->count] = static_cast<uint64_t>(new_suffix) & child_mask;
                    child_values[h->count] = value;
                    
                    // Create child node at lower BITS level
                    uint64_t child_ptr = create_child_no_prefix<child_bits>(
                        child_suffixes.get(), child_values.get(), total_count);
                    
                    // Update skip/prefix on child - add our prefix at HIGH bits
                    uint64_t* child_node = reinterpret_cast<uint64_t*>(child_ptr);
                    NodeHeader* child_h = get_header(child_node);
                    uint64_t old_prefix = child_h->prefix;
                    uint8_t old_skip = child_h->skip;
                    // Combine parent's skip with child's skip, plus our new level
                    child_h->skip = h->skip + old_skip + 1;
                    // Build combined prefix: parent_prefix | our_skip_prefix | child_prefix
                    uint64_t combined = h->prefix;
                    combined = (combined << 16) | skip_prefix;
                    combined = (combined << (16 * old_skip)) | old_prefix;
                    child_h->prefix = combined;
                    
                    dealloc_node(node, leaf_compact_size_u64<BITS>(h->count));
                    return {child_node, true};
                }
            }
        }
        
        uint64_t* new_node = alloc_node(split_top_size_u64<BITS>(new_top_count));
        NodeHeader* new_h = get_header(new_node);
        new_h->count = h->count + 1;
        new_h->top_count = static_cast<uint16_t>(new_top_count);
        new_h->skip = h->skip;
        new_h->prefix = h->prefix;
        new_h->set_leaf(true);  // All bottoms start as leaves
        new_h->set_split(true);
        
        top_bitmap(new_node) = new_top_bm;
        
        if constexpr (BITS > 16) {
            bot_is_leaf_bitmap(new_node) = new_top_bm;  // All are leaves initially
        }
        
        uint64_t* new_top_ch = top_children<BITS>(new_node);
        
        // Create bottom LEAFs for each bucket
        constexpr int suffix_bits = BITS - 8;
        using S = typename suffix_traits<suffix_bits>::type;
        constexpr uint64_t suffix_mask = (1ULL << suffix_bits) - 1;
        
        S new_bot_suffix = static_cast<S>(new_suffix & suffix_mask);
        
        int top_slot = 0;
        for (int top_idx = 0; top_idx < 256; ++top_idx) {
            if (!new_top_bm.has_bit(top_idx)) continue;
            
            size_t bot_count = bucket_counts[top_idx];
            uint64_t* bot = alloc_node(bot_leaf_size_u64<BITS>(bot_count));
            
            if constexpr (BITS == 16) {
                // Bitmap-based: [bm_256][values...]
                Bitmap256& bot_bm = bot_leaf_bitmap<16>(bot);
                bot_bm = Bitmap256{};  // Clear
                value_slot_type* values = bot_leaf_values<16>(bot, bot_count);
                
                // First pass: set bits and collect (suffix, value) pairs
                struct Entry { uint8_t suffix; value_slot_type value; };
                Entry entries[256];
                size_t entry_count = 0;
                
                bool need_insert_new = (new_top_idx == top_idx);
                
                for (uint32_t i = 0; i < h->count; ++i) {
                    if ((old_keys[i] >> 8) == static_cast<uint64_t>(top_idx)) {
                        uint8_t old_bot_suffix = static_cast<uint8_t>(old_keys[i] & 0xFF);
                        entries[entry_count++] = {old_bot_suffix, old_values[i]};
                        bot_bm.set_bit(old_bot_suffix);
                    }
                }
                
                if (need_insert_new) {
                    uint8_t new_bot_suffix_8 = static_cast<uint8_t>(new_suffix & 0xFF);
                    entries[entry_count++] = {new_bot_suffix_8, value};
                    bot_bm.set_bit(new_bot_suffix_8);
                }
                
                // Second pass: write values in bitmap order
                for (size_t i = 0; i < entry_count; ++i) {
                    int slot = bot_bm.count_below(entries[i].suffix);
                    values[slot] = entries[i].value;
                }
            } else {
                // List-based: [count][idx1][idx2][suffixes...][values...]
                set_bot_leaf_count<BITS>(bot, static_cast<uint32_t>(bot_count));
                
                S* suffixes_data = bot_leaf_suffixes_data<BITS>(bot, bot_count);
                value_slot_type* values = bot_leaf_values<BITS>(bot, bot_count);
                
                bool need_insert_new = (new_top_idx == top_idx);
                bool inserted_new = false;
                size_t idx = 0;
                
                for (uint32_t i = 0; i < h->count; ++i) {
                    if ((old_keys[i] >> (BITS - 8)) == static_cast<uint64_t>(top_idx)) {
                        S old_bot_suffix = static_cast<S>(old_keys[i] & suffix_mask);
                        
                        // Insert new entry before this one if it belongs here
                        if (need_insert_new && !inserted_new && new_bot_suffix < old_bot_suffix) {
                            suffixes_data[idx] = new_bot_suffix;
                            values[idx] = value;
                            idx++;
                            inserted_new = true;
                        }
                        
                        suffixes_data[idx] = old_bot_suffix;
                        values[idx] = old_values[i];
                        idx++;
                    }
                }
                
                // Insert new entry at end if not yet inserted
                if (need_insert_new && !inserted_new) {
                    suffixes_data[idx] = new_bot_suffix;
                    values[idx] = value;
                }
                
                // Build indices
                S* idx_start = bot_leaf_suffixes<BITS>(bot);
                build_indices(idx_start, suffixes_data, static_cast<int>(bot_count));
            }
            
            new_top_ch[top_slot++] = reinterpret_cast<uint64_t>(bot);
        }
        
        dealloc_node(node, leaf_compact_size_u64<BITS>(h->count));
        return {new_node, true};
    }
    
    // Create a child node, with recursive prefix compression if needed
    template<int CHILD_BITS>
    uint64_t create_child_no_prefix(uint64_t* suffixes, value_slot_type* values, size_t count)
        requires (CHILD_BITS > 0)
    {
        // If it fits in a compact leaf, create one
        if (count <= COMPACT_MAX) {
            uint64_t* child = alloc_node(leaf_compact_size_u64<CHILD_BITS>(count));
            NodeHeader* child_h = get_header(child);
            child_h->count = static_cast<uint32_t>(count);
            child_h->skip = 0;
            child_h->prefix = 0;
            child_h->set_leaf(true);
            
            using ChildK = typename suffix_traits<CHILD_BITS>::type;
            ChildK* child_keys_data = leaf_keys_data<CHILD_BITS>(child, count);
            value_slot_type* child_values = leaf_values<CHILD_BITS>(child, count);
            
            // Sort and copy using insertion sort
            for (size_t i = 0; i < count; ++i) {
                ChildK key = static_cast<ChildK>(suffixes[i]);
                value_slot_type val = values[i];
                size_t j = i;
                while (j > 0 && child_keys_data[j-1] > key) {
                    child_keys_data[j] = child_keys_data[j-1];
                    child_values[j] = child_values[j-1];
                    j--;
                }
                child_keys_data[j] = key;
                child_values[j] = val;
            }
            
            // Build indices
            ChildK* idx_start = leaf_keys<CHILD_BITS>(child);
            build_indices(idx_start, child_keys_data, static_cast<int>(count));
            
            return reinterpret_cast<uint64_t>(child);
        }
        
        // Too many entries - need to create SPLIT structure
        Bitmap256 top_bm{};
        uint16_t bucket_counts[256] = {0};
        
        for (size_t i = 0; i < count; ++i) {
            uint8_t idx = static_cast<uint8_t>(suffixes[i] >> (CHILD_BITS - 8));
            top_bm.set_bit(idx);
            bucket_counts[idx]++;
        }
        
        size_t top_count = top_bm.popcount();
        
        // PREFIX COMPRESSION: If all entries share high 8 bits AND we have room to skip
        if constexpr (CHILD_BITS > 16) {
            if (top_count == 1) {
                // Check if the next 8 bits are also uniform
                int single_top = top_bm.find_next_set(0);
                Bitmap256 bot_bm{};
                constexpr int suffix_bits = CHILD_BITS - 8;
                
                for (size_t i = 0; i < count; ++i) {
                    uint8_t bot_idx = static_cast<uint8_t>(suffixes[i] >> (suffix_bits - 8));
                    bot_bm.set_bit(bot_idx);
                }
                
                if (bot_bm.popcount() == 1) {
                    // Both 8-bit levels are uniform - skip this 16-bit level
                    int single_bot = bot_bm.find_next_set(0);
                    uint16_t skip_prefix = (static_cast<uint16_t>(single_top) << 8) | single_bot;
                    
                    constexpr int child_bits = CHILD_BITS - 16;
                    constexpr uint64_t child_mask = (1ULL << child_bits) - 1;
                    
                    // Shift all suffixes down by 16 bits
                    for (size_t i = 0; i < count; ++i) {
                        suffixes[i] = suffixes[i] & child_mask;
                    }
                    
                    // Recurse
                    uint64_t child_ptr = create_child_no_prefix<child_bits>(suffixes, values, count);
                    
                    // Add the skip prefix to the child
                    // Our skip_prefix goes at the HIGH bits (we're a level above the child)
                    uint64_t* child_node = reinterpret_cast<uint64_t*>(child_ptr);
                    NodeHeader* child_h = get_header(child_node);
                    uint64_t old_prefix = child_h->prefix;
                    uint8_t old_skip = child_h->skip;
                    child_h->skip = old_skip + 1;
                    child_h->prefix = (static_cast<uint64_t>(skip_prefix) << (16 * old_skip)) | old_prefix;
                    
                    return child_ptr;
                }
            }
        }
        
        uint64_t* split_node = alloc_node(split_top_size_u64<CHILD_BITS>(top_count));
        NodeHeader* split_h = get_header(split_node);
        split_h->count = static_cast<uint32_t>(count);
        split_h->top_count = static_cast<uint16_t>(top_count);
        split_h->skip = 0;
        split_h->prefix = 0;
        split_h->set_split(true);
        split_h->set_leaf(true);
        
        top_bitmap(split_node) = top_bm;
        if constexpr (CHILD_BITS > 16) {
            bot_is_leaf_bitmap(split_node) = top_bm;
        }
        
        uint64_t* top_ch = top_children<CHILD_BITS>(split_node);
        constexpr int suffix_bits = CHILD_BITS - 8;
        constexpr uint64_t suffix_mask = (1ULL << suffix_bits) - 1;
        
        int slot = 0;
        for (int bucket = 0; bucket < 256; ++bucket) {
            if (!top_bm.has_bit(bucket)) continue;
            
            size_t bot_count = bucket_counts[bucket];
            
            // Check if bot_count would exceed BOT_LEAF_MAX (only possible when CHILD_BITS > 16)
            bool need_bot_internal = false;
            if constexpr (CHILD_BITS > 16) {
                need_bot_internal = (bot_count > BOT_LEAF_MAX);
            }
            
            if (need_bot_internal) {
                if constexpr (CHILD_BITS > 16) {
                // Need to create bot_internal, not bot_leaf
                // First, further subdivide by next 8 bits
                Bitmap256 bot_inner_bm{};
                uint16_t bot_inner_counts[256] = {0};
                
                for (size_t i = 0; i < count; ++i) {
                    if ((suffixes[i] >> (CHILD_BITS - 8)) == static_cast<uint64_t>(bucket)) {
                        uint8_t inner_idx = static_cast<uint8_t>((suffixes[i] >> (suffix_bits - 8)) & 0xFF);
                        bot_inner_bm.set_bit(inner_idx);
                        bot_inner_counts[inner_idx]++;
                    }
                }
                
                size_t bot_inner_count = bot_inner_bm.popcount();
                uint64_t* bot_internal = alloc_node(bot_internal_size_u64(bot_inner_count));
                bot_bitmap(bot_internal) = bot_inner_bm;
                uint64_t* bot_children = bot_internal_children(bot_internal);
                
                constexpr int child_bits = CHILD_BITS - 16;
                constexpr uint64_t child_mask = (1ULL << child_bits) - 1;
                
                int inner_slot = 0;
                for (int inner_bucket = 0; inner_bucket < 256; ++inner_bucket) {
                    if (!bot_inner_bm.has_bit(inner_bucket)) continue;
                    
                    size_t child_count = bot_inner_counts[inner_bucket];
                    
                    // Collect entries for this inner bucket
                    std::unique_ptr<uint64_t[]> child_suffixes(new uint64_t[child_count]);
                    std::unique_ptr<value_slot_type[]> child_vals(new value_slot_type[child_count]);
                    size_t ci = 0;
                    
                    for (size_t i = 0; i < count; ++i) {
                        if ((suffixes[i] >> (CHILD_BITS - 8)) == static_cast<uint64_t>(bucket) &&
                            ((suffixes[i] >> (suffix_bits - 8)) & 0xFF) == static_cast<uint64_t>(inner_bucket)) {
                            child_suffixes[ci] = suffixes[i] & child_mask;
                            child_vals[ci] = values[i];
                            ci++;
                        }
                    }
                    
                    // Recursively create child
                    uint64_t child_ptr = create_child_no_prefix<child_bits>(
                        child_suffixes.get(), child_vals.get(), child_count);
                    bot_children[inner_slot++] = child_ptr;
                }
                
                top_ch[slot++] = reinterpret_cast<uint64_t>(bot_internal);
                bot_is_leaf_bitmap(split_node).clear_bit(bucket);
                } // end if constexpr
            } else {
                // Normal case: create bot_leaf
                uint64_t* bot = alloc_node(bot_leaf_size_u64<CHILD_BITS>(bot_count));
                
                if constexpr (CHILD_BITS == 16) {
                    // Bitmap-based: [bm_256][values...]
                    Bitmap256& bot_bm = bot_leaf_bitmap<16>(bot);
                    bot_bm = Bitmap256{};
                    value_slot_type* bot_values = bot_leaf_values<16>(bot, bot_count);
                    
                    // Collect entries for this bucket, set bits, and populate values
                    struct Entry { uint8_t suffix; value_slot_type value; };
                    Entry entries[256];
                    size_t bi = 0;
                    
                    for (size_t i = 0; i < count; ++i) {
                        if ((suffixes[i] >> 8) == static_cast<uint64_t>(bucket)) {
                            uint8_t suf = static_cast<uint8_t>(suffixes[i] & 0xFF);
                            entries[bi++] = {suf, values[i]};
                            bot_bm.set_bit(suf);
                        }
                    }
                    
                    // Write values in bitmap order
                    for (size_t i = 0; i < bi; ++i) {
                        int slot_idx = bot_bm.count_below(entries[i].suffix);
                        bot_values[slot_idx] = entries[i].value;
                    }
                } else {
                    // List-based: [count][suffixes...][values...]
                    set_bot_leaf_count<CHILD_BITS>(bot, static_cast<uint32_t>(bot_count));
                    
                    using S = typename suffix_traits<suffix_bits>::type;
                    S* bot_suffixes_data = bot_leaf_suffixes_data<CHILD_BITS>(bot, bot_count);
                    value_slot_type* bot_values = bot_leaf_values<CHILD_BITS>(bot, bot_count);
                    
                    // Collect and sort entries for this bucket
                    size_t bi = 0;
                    for (size_t i = 0; i < count; ++i) {
                        if ((suffixes[i] >> (CHILD_BITS - 8)) == static_cast<uint64_t>(bucket)) {
                            S suf = static_cast<S>(suffixes[i] & suffix_mask);
                            value_slot_type val = values[i];
                            
                            // Insert in sorted order
                            size_t j = bi;
                            while (j > 0 && bot_suffixes_data[j-1] > suf) {
                                bot_suffixes_data[j] = bot_suffixes_data[j-1];
                                bot_values[j] = bot_values[j-1];
                                j--;
                            }
                            bot_suffixes_data[j] = suf;
                            bot_values[j] = val;
                            bi++;
                        }
                    }
                    
                    // Build indices
                    S* idx_start = bot_leaf_suffixes<CHILD_BITS>(bot);
                    build_indices(idx_start, bot_suffixes_data, static_cast<int>(bot_count));
                }
                
                top_ch[slot++] = reinterpret_cast<uint64_t>(bot);
            }
        }
        
        // Update leaf flag
        if constexpr (CHILD_BITS > 16) {
            const Bitmap256& is_leaf_bm = bot_is_leaf_bitmap(split_node);
            bool any_leaf = false;
            for (int idx = top_bm.find_next_set(0); idx >= 0; idx = top_bm.find_next_set(idx + 1)) {
                if (is_leaf_bm.has_bit(idx)) { any_leaf = true; break; }
            }
            if (!any_leaf) split_h->set_leaf(false);
        }
        
        return reinterpret_cast<uint64_t>(split_node);
    }
    
    template<int BITS>
    InsertResult insert_into_split(uint64_t* node, NodeHeader* h, uint64_t ik, value_slot_type value)
        requires (BITS > 0)
    {
        uint8_t top_idx = extract_top8<BITS>(ik);
        
        Bitmap256& top_bm = top_bitmap(node);
        uint64_t* top_ch = top_children<BITS>(node);
        
        int top_slot;
        bool top_exists = top_bm.find_slot(top_idx, top_slot);
        
        if (!top_exists) {
            return add_new_bottom_leaf<BITS>(node, h, ik, value, top_idx);
        }
        
        bool is_leaf;
        if constexpr (BITS == 16) {
            is_leaf = true;
        } else {
            is_leaf = bot_is_leaf_bitmap(node).has_bit(top_idx);
        }
        
        uint64_t* bot = reinterpret_cast<uint64_t*>(top_ch[top_slot]);
        
        if (is_leaf) {
            return insert_into_bot_leaf<BITS>(node, h, top_idx, top_slot, bot, ik, value);
        } else {
            if constexpr (BITS > 16) {
                return insert_into_bot_internal<BITS>(node, h, top_idx, top_slot, bot, ik, value);
            } else {
                // At BITS=16, bottom is always leaf, should never get here
                return {node, false};
            }
        }
    }
    
    template<int BITS>
    InsertResult add_new_bottom_leaf(uint64_t* node, NodeHeader* h, uint64_t ik, value_slot_type value, uint8_t top_idx)
        requires (BITS > 0)
    {
        Bitmap256& top_bm = top_bitmap(node);
        size_t old_top_count = h->top_count;
        size_t new_top_count = old_top_count + 1;
        
        int insert_slot = top_bm.slot_for_insert(top_idx);
        
        uint64_t* new_node = alloc_node(split_top_size_u64<BITS>(new_top_count));
        NodeHeader* new_h = get_header(new_node);
        *new_h = *h;
        new_h->count = h->count + 1;
        new_h->top_count = static_cast<uint16_t>(new_top_count);
        
        Bitmap256& new_top_bm = top_bitmap(new_node);
        new_top_bm = top_bm;
        new_top_bm.set_bit(top_idx);
        
        if constexpr (BITS > 16) {
            Bitmap256& new_is_leaf = bot_is_leaf_bitmap(new_node);
            new_is_leaf = bot_is_leaf_bitmap(node);
            new_is_leaf.set_bit(top_idx);
        }
        
        uint64_t* old_ch = top_children<BITS>(node);
        uint64_t* new_ch = top_children<BITS>(new_node);
        
        for (int i = 0; i < insert_slot; ++i) new_ch[i] = old_ch[i];
        for (size_t i = insert_slot; i < old_top_count; ++i) new_ch[i + 1] = old_ch[i];
        
        // Create new bottom LEAF with single entry
        uint64_t* new_bot = alloc_node(bot_leaf_size_u64<BITS>(1));
        
        if constexpr (BITS == 16) {
            // Bitmap-based: [bm_256][values...]
            Bitmap256& new_bm = bot_leaf_bitmap<16>(new_bot);
            new_bm = Bitmap256{};
            uint8_t suffix = static_cast<uint8_t>(extract_suffix<8>(ik));
            new_bm.set_bit(suffix);
            
            value_slot_type* values = bot_leaf_values<16>(new_bot, 1);
            if constexpr (value_inline) {
                std::memcpy(&values[0], &value, sizeof(value_slot_type));
            } else {
                values[0] = static_cast<uint64_t>(value);
            }
        } else {
            // List-based: [count][idx1][idx2][suffixes...][values...]
            constexpr int suffix_bits = BITS - 8;
            using S = typename suffix_traits<suffix_bits>::type;
            
            set_bot_leaf_count<BITS>(new_bot, 1);
            
            S* suffixes_data = bot_leaf_suffixes_data<BITS>(new_bot, 1);
            value_slot_type* values = bot_leaf_values<BITS>(new_bot, 1);
            
            suffixes_data[0] = static_cast<S>(extract_suffix<suffix_bits>(ik));
            if constexpr (value_inline) {
                std::memcpy(&values[0], &value, sizeof(value_slot_type));
            } else {
                values[0] = static_cast<uint64_t>(value);
            }
            
            // Build indices (no-op for count=1, but keep for consistency)
            S* idx_start = bot_leaf_suffixes<BITS>(new_bot);
            build_indices(idx_start, suffixes_data, 1);
        }
        
        new_ch[insert_slot] = reinterpret_cast<uint64_t>(new_bot);
        
        dealloc_node(node, split_top_size_u64<BITS>(old_top_count));
        return {new_node, true};
    }
    
    template<int BITS>
    InsertResult insert_into_bot_leaf(uint64_t* node, NodeHeader* h, uint8_t top_idx, int top_slot,
                                       uint64_t* bot, uint64_t ik, value_slot_type value)
        requires (BITS > 0)
    {
        if constexpr (BITS == 16) {
            // Bitmap-based: [bm_256][values...]
            uint8_t suffix = static_cast<uint8_t>(extract_suffix<8>(ik));
            Bitmap256& bot_bm = bot_leaf_bitmap<16>(bot);
            uint32_t count = bot_bm.popcount();
            value_slot_type* values = bot_leaf_values<16>(bot, count);
            
            if (bot_bm.has_bit(suffix)) {
                // Found existing - update value
                int slot = bot_bm.count_below(suffix);
                if constexpr (!value_inline) {
                    destroy_value(static_cast<value_slot_type>(values[slot]));
                }
                if constexpr (value_inline) {
                    std::memcpy(&values[slot], &value, sizeof(value_slot_type));
                } else {
                    values[slot] = static_cast<uint64_t>(value);
                }
                return {node, false};
            }
            
            // Not found - add new entry
            // Reallocate with new entry
            uint32_t new_count = count + 1;
            uint64_t* new_bot = alloc_node(bot_leaf_size_u64<16>(new_count));
            Bitmap256& new_bm = bot_leaf_bitmap<16>(new_bot);
            new_bm = bot_bm;
            new_bm.set_bit(suffix);
            
            value_slot_type* new_values = bot_leaf_values<16>(new_bot, new_count);
            
            // Copy values, inserting new one at correct slot
            int insert_slot = new_bm.count_below(suffix);
            
            // Copy before insertion point
            std::memcpy(new_values, values, insert_slot * sizeof(value_slot_type));
            
            // Insert new value
            if constexpr (value_inline) {
                std::memcpy(&new_values[insert_slot], &value, sizeof(value_slot_type));
            } else {
                new_values[insert_slot] = static_cast<uint64_t>(value);
            }
            
            // Copy after insertion point
            std::memcpy(new_values + insert_slot + 1, values + insert_slot, (count - insert_slot) * sizeof(value_slot_type));
            
            top_children<16>(node)[top_slot] = reinterpret_cast<uint64_t>(new_bot);
            h->count++;
            
            dealloc_node(bot, bot_leaf_size_u64<16>(count));
            return {node, true};
        } else {
            // List-based: [count][idx1][idx2][suffixes...][values...]
            uint32_t count = bot_leaf_count<BITS>(bot);
            
            constexpr int suffix_bits = BITS - 8;
            using S = typename suffix_traits<suffix_bits>::type;
            S suffix = static_cast<S>(extract_suffix<suffix_bits>(ik));
            
            // Get actual suffixes data (after indices)
            S* suffixes_data = bot_leaf_suffixes_data<BITS>(bot, count);
            value_slot_type* values = bot_leaf_values<BITS>(bot, count);
            
            // Binary search on actual suffixes for existing or insertion point
            int idx = binary_search_for_insert(suffixes_data, count, suffix);
            
            if (idx >= 0) {
                // Found existing - update value
                if constexpr (!value_inline) {
                    destroy_value(static_cast<value_slot_type>(values[idx]));
                }
                if constexpr (value_inline) {
                    std::memcpy(&values[idx], &value, sizeof(value_slot_type));
                } else {
                    values[idx] = static_cast<uint64_t>(value);
                }
                return {node, false};
            }
            
            // Not found - idx encodes insertion point
            size_t insert_pos = static_cast<size_t>(-(idx + 1));
            
            // Need to add - check overflow
            if (count >= BOT_LEAF_MAX) {
                if constexpr (BITS > 16) {
                    return convert_bot_leaf_to_internal<BITS>(node, h, top_idx, top_slot, bot, count, ik, value);
                }
            }
            
            // Reallocate with new entry in sorted position
            uint32_t new_count = count + 1;
            uint64_t* new_bot = alloc_node(bot_leaf_size_u64<BITS>(new_count));
            set_bot_leaf_count<BITS>(new_bot, new_count);
            
            S* new_suffixes_data = bot_leaf_suffixes_data<BITS>(new_bot, new_count);
            value_slot_type* new_values = bot_leaf_values<BITS>(new_bot, new_count);
            
            // Copy before insertion point
            std::memcpy(new_suffixes_data, suffixes_data, insert_pos * sizeof(S));
            std::memcpy(new_values, values, insert_pos * sizeof(value_slot_type));
            
            // Insert new entry
            new_suffixes_data[insert_pos] = suffix;
            if constexpr (value_inline) {
                std::memcpy(&new_values[insert_pos], &value, sizeof(value_slot_type));
            } else {
                new_values[insert_pos] = static_cast<uint64_t>(value);
            }
            
            // Copy after insertion point
            std::memcpy(new_suffixes_data + insert_pos + 1, suffixes_data + insert_pos, (count - insert_pos) * sizeof(S));
            std::memcpy(new_values + insert_pos + 1, values + insert_pos, (count - insert_pos) * sizeof(value_slot_type));
            
            // Build indices from the new suffixes
            S* new_idx_start = bot_leaf_suffixes<BITS>(new_bot);
            build_indices(new_idx_start, new_suffixes_data, static_cast<int>(new_count));
            
            top_children<BITS>(node)[top_slot] = reinterpret_cast<uint64_t>(new_bot);
            h->count++;
            
            dealloc_node(bot, bot_leaf_size_u64<BITS>(count));
            return {node, true};
        }
    }

    template<int BITS>
    InsertResult convert_bot_leaf_to_internal(uint64_t* node, NodeHeader* h, uint8_t top_idx, int top_slot,
                                               uint64_t* bot, uint32_t count, uint64_t ik, value_slot_type value)
        requires (BITS > 16)
    {
        constexpr int suffix_bits = BITS - 8;
        using S = typename suffix_traits<suffix_bits>::type;
        
        S* old_suffixes = bot_leaf_suffixes_data<BITS>(bot, count);  // Actual suffixes after indices
        value_slot_type* old_values = bot_leaf_values<BITS>(bot, count);
        
        // Group by high 8 bits (the "bot_idx" within this level)
        Bitmap256 bot_bm{};
        uint16_t bucket_counts[256] = {0};
        
        for (uint32_t i = 0; i < count; ++i) {
            uint8_t bot_idx = static_cast<uint8_t>(old_suffixes[i] >> (suffix_bits - 8));
            bot_bm.set_bit(bot_idx);
            bucket_counts[bot_idx]++;
        }
        
        // Add new entry
        S new_suffix = static_cast<S>(extract_suffix<suffix_bits>(ik));
        uint8_t new_bot_idx = static_cast<uint8_t>(new_suffix >> (suffix_bits - 8));
        bot_bm.set_bit(new_bot_idx);
        bucket_counts[new_bot_idx]++;
        
        int bot_child_count = bot_bm.popcount();
        
        // Allocate new bottom INTERNAL
        uint64_t* new_bot = alloc_node(bot_internal_size_u64(bot_child_count));
        bot_bitmap(new_bot) = bot_bm;
        uint64_t* children = bot_internal_children(new_bot);
        
        // Create child leaves at BITS-16
        constexpr int child_bits = BITS - 16;
        using ChildK = typename suffix_traits<child_bits>::type;
        constexpr uint64_t child_mask = (1ULL << child_bits) - 1;
        
        ChildK new_child_suffix = static_cast<ChildK>(new_suffix & child_mask);
        
        int slot = 0;
        for (int bot_idx = 0; bot_idx < 256; ++bot_idx) {
            if (!bot_bm.has_bit(bot_idx)) continue;
            
            size_t child_count = bucket_counts[bot_idx];
            uint64_t* child = alloc_node(leaf_compact_size_u64<child_bits>(child_count));
            NodeHeader* child_h = get_header(child);
            child_h->count = static_cast<uint32_t>(child_count);
            child_h->set_leaf(true);
            
            ChildK* child_keys_data = leaf_keys_data<child_bits>(child, child_count);
            value_slot_type* child_values = leaf_values<child_bits>(child, child_count);
            
            bool need_insert_new = (new_bot_idx == bot_idx);
            bool inserted_new = false;
            size_t ci = 0;
            
            for (uint32_t i = 0; i < count; ++i) {
                if ((old_suffixes[i] >> (suffix_bits - 8)) == static_cast<uint64_t>(bot_idx)) {
                    ChildK old_child_suffix = static_cast<ChildK>(old_suffixes[i] & child_mask);
                    
                    // Insert new entry before this one if it belongs here
                    if (need_insert_new && !inserted_new && new_child_suffix < old_child_suffix) {
                        child_keys_data[ci] = new_child_suffix;
                        child_values[ci] = value;
                        ci++;
                        inserted_new = true;
                    }
                    
                    child_keys_data[ci] = old_child_suffix;
                    child_values[ci] = old_values[i];
                    ci++;
                }
            }
            
            // Insert new entry at end if not yet inserted
            if (need_insert_new && !inserted_new) {
                child_keys_data[ci] = new_child_suffix;
                child_values[ci] = value;
            }
            
            // Build indices for the child
            ChildK* child_idx_start = leaf_keys<child_bits>(child);
            build_indices(child_idx_start, child_keys_data, static_cast<int>(child_count));
            
            children[slot++] = reinterpret_cast<uint64_t>(child);
        }
        
        // Update parent
        top_children<BITS>(node)[top_slot] = reinterpret_cast<uint64_t>(new_bot);
        bot_is_leaf_bitmap(node).clear_bit(top_idx);
        h->count++;
        
        // Check if any bottom is still a leaf
        const Bitmap256& top_bm = top_bitmap(node);
        const Bitmap256& is_leaf_bm = bot_is_leaf_bitmap(node);
        bool any_leaf = false;
        for (int idx = top_bm.find_next_set(0); idx >= 0; idx = top_bm.find_next_set(idx + 1)) {
            if (is_leaf_bm.has_bit(idx)) { any_leaf = true; break; }
        }
        if (!any_leaf) h->set_leaf(false);
        
        dealloc_node(bot, bot_leaf_size_u64<BITS>(count));
        return {node, true};
    }
    
    template<int BITS>
    InsertResult insert_into_bot_internal(uint64_t* node, NodeHeader* h, uint8_t top_idx, int top_slot,
                                           uint64_t* bot, uint64_t ik, value_slot_type value)
        requires (BITS > 16)
    {
        uint8_t bot_idx = extract_top8<BITS - 8>(ik);
        
        Bitmap256& bot_bm = bot_bitmap(bot);
        uint64_t* children = bot_internal_children(bot);
        
        int bot_slot;
        bool bot_exists = bot_bm.find_slot(bot_idx, bot_slot);
        
        if (bot_exists) {
            auto [new_child, inserted] = insert_impl<BITS - 16>(
                reinterpret_cast<uint64_t*>(children[bot_slot]), ik, value);
            children[bot_slot] = reinterpret_cast<uint64_t>(new_child);
            if (inserted) h->count++;
            return {node, inserted};
        } else {
            int bot_count = bot_bm.popcount();
            int insert_slot = bot_bm.slot_for_insert(bot_idx);
            int new_bot_count = bot_count + 1;
            
            uint64_t* new_bot = alloc_node(bot_internal_size_u64(new_bot_count));
            Bitmap256& new_bot_bm = bot_bitmap(new_bot);
            new_bot_bm = bot_bm;
            new_bot_bm.set_bit(bot_idx);
            
            uint64_t* new_children = bot_internal_children(new_bot);
            for (int i = 0; i < insert_slot; ++i) new_children[i] = children[i];
            for (int i = insert_slot; i < bot_count; ++i) new_children[i + 1] = children[i];
            
            constexpr int child_bits = BITS - 16;
            uint64_t* child = alloc_node(leaf_compact_size_u64<child_bits>(1));
            NodeHeader* child_h = get_header(child);
            child_h->count = 1;
            child_h->set_leaf(true);
            
            using ChildK = typename suffix_traits<child_bits>::type;
            ChildK* child_keys_data = leaf_keys_data<child_bits>(child, 1);
            child_keys_data[0] = static_cast<ChildK>(extract_suffix<child_bits>(ik));
            if constexpr (value_inline) {
                std::memcpy(&leaf_values<child_bits>(child, 1)[0], &value, sizeof(value_slot_type));
            } else {
                leaf_values<child_bits>(child, 1)[0] = static_cast<uint64_t>(value);
            }
            
            // Build indices (no-op for count=1)
            ChildK* idx_start = leaf_keys<child_bits>(child);
            build_indices(idx_start, child_keys_data, 1);
            
            new_children[insert_slot] = reinterpret_cast<uint64_t>(child);
            top_children<BITS>(node)[top_slot] = reinterpret_cast<uint64_t>(new_bot);
            h->count++;
            
            dealloc_node(bot, bot_internal_size_u64(bot_count));
            return {node, true};
        }
    }
    
    template<int BITS>
    InsertResult split_on_prefix(uint64_t* node, NodeHeader* h, uint64_t ik, value_slot_type value, uint64_t expected)
        requires (BITS > 0)
    {
        // Prefix mismatch - need to create a new structure
        // Find where the prefixes diverge (which 16-bit chunk)
        uint64_t actual = h->prefix;
        int skip = h->skip;
        
        // Find first differing 16-bit chunk (scanning from HIGH to LOW)
        // common_levels = number of matching 16-bit chunks from the top
        int common_levels = 0;
        for (int i = skip - 1; i >= 0; --i) {
            uint16_t expected_chunk = (expected >> (i * 16)) & 0xFFFF;
            uint16_t actual_chunk = (actual >> (i * 16)) & 0xFFFF;
            if (expected_chunk != actual_chunk) {
                break;
            }
            common_levels++;
        }
        
        // The differing chunk is at index (skip - 1 - common_levels) from bit 0
        int diff_chunk_idx = skip - 1 - common_levels;
        uint16_t new_chunk = (expected >> (diff_chunk_idx * 16)) & 0xFFFF;
        uint16_t old_chunk = (actual >> (diff_chunk_idx * 16)) & 0xFFFF;
        
        uint8_t new_top = (new_chunk >> 8) & 0xFF;
        uint8_t old_top = (old_chunk >> 8) & 0xFF;
        
        if (new_top == old_top) {
            // Same top 8 bits, different bottom 8 bits - need to create SPLIT with bot_internal
            uint8_t new_bot = new_chunk & 0xFF;
            uint8_t old_bot = old_chunk & 0xFF;
            
            // Create new SPLIT node with one bucket
            uint64_t* split_node = alloc_node(split_top_size_u64<BITS>(1));
            NodeHeader* split_h = get_header(split_node);
            split_h->count = h->count + 1;
            split_h->top_count = 1;
            split_h->skip = static_cast<uint8_t>(common_levels);
            if (common_levels > 0) {
                // Extract common prefix from expected (high common_levels chunks)
                split_h->prefix = expected >> ((skip - common_levels) * 16);
            } else {
                split_h->prefix = 0;
            }
            split_h->set_split(true);
            split_h->set_leaf(false);
            
            Bitmap256 top_bm{};
            top_bm.set_bit(new_top);
            top_bitmap(split_node) = top_bm;
            
            if constexpr (BITS > 16) {
                bot_is_leaf_bitmap(split_node) = Bitmap256{};  // Not a leaf
            }
            
            // Create bot_internal with 2 children
            uint64_t* bot_internal = alloc_node(bot_internal_size_u64(2));
            Bitmap256 bot_bm{};
            bot_bm.set_bit(new_bot);
            bot_bm.set_bit(old_bot);
            bot_bitmap(bot_internal) = bot_bm;
            
            uint64_t* children = bot_internal_children(bot_internal);
            
            // Adjust old node's skip and prefix (remaining levels after the differing chunk)
            int remaining_skip = diff_chunk_idx;  // Chunks below the differing one
            h->skip = static_cast<uint8_t>(remaining_skip);
            if (remaining_skip > 0) {
                uint64_t mask = (1ULL << (remaining_skip * 16)) - 1;
                h->prefix = actual & mask;
            } else {
                h->prefix = 0;
            }
            
            // Create new leaf for the new key
            constexpr int child_bits = BITS - 16;
            uint64_t* new_leaf = alloc_node(leaf_compact_size_u64<child_bits>(1));
            NodeHeader* new_h = get_header(new_leaf);
            new_h->count = 1;
            new_h->skip = static_cast<uint8_t>(remaining_skip);
            if (remaining_skip > 0) {
                new_h->prefix = expected & ((1ULL << (remaining_skip * 16)) - 1);
            } else {
                new_h->prefix = 0;
            }
            new_h->set_leaf(true);
            
            using ChildK = typename suffix_traits<child_bits>::type;
            ChildK* new_keys_data = leaf_keys_data<child_bits>(new_leaf, 1);
            new_keys_data[0] = static_cast<ChildK>(extract_suffix<child_bits>(ik));
            if constexpr (value_inline) {
                std::memcpy(&leaf_values<child_bits>(new_leaf, 1)[0], &value, sizeof(value_slot_type));
            } else {
                leaf_values<child_bits>(new_leaf, 1)[0] = static_cast<uint64_t>(value);
            }
            
            // Build indices (no-op for count=1)
            ChildK* idx_start = leaf_keys<child_bits>(new_leaf);
            build_indices(idx_start, new_keys_data, 1);
            
            // Place in correct order
            if (new_bot < old_bot) {
                children[0] = reinterpret_cast<uint64_t>(new_leaf);
                children[1] = reinterpret_cast<uint64_t>(node);
            } else {
                children[0] = reinterpret_cast<uint64_t>(node);
                children[1] = reinterpret_cast<uint64_t>(new_leaf);
            }
            
            top_children<BITS>(split_node)[0] = reinterpret_cast<uint64_t>(bot_internal);
            return {split_node, true};
        } else {
            // Different top 8 bits - create SPLIT with 2 buckets
            uint64_t* split_node = alloc_node(split_top_size_u64<BITS>(2));
            NodeHeader* split_h = get_header(split_node);
            split_h->count = h->count + 1;
            split_h->top_count = 2;
            split_h->skip = static_cast<uint8_t>(common_levels);
            if (common_levels > 0) {
                split_h->prefix = expected >> ((skip - common_levels) * 16);
            } else {
                split_h->prefix = 0;
            }
            split_h->set_split(true);
            split_h->set_leaf(false);  // Children are bot_internal, not bot_leaf
            
            Bitmap256 top_bm{};
            top_bm.set_bit(new_top);
            top_bm.set_bit(old_top);
            top_bitmap(split_node) = top_bm;
            
            if constexpr (BITS > 16) {
                // Both are internal (pointing to child nodes)
                bot_is_leaf_bitmap(split_node) = Bitmap256{};
            }
            
            // Adjust old node's skip and prefix for remaining levels
            int remaining_skip = diff_chunk_idx;  // Chunks below the differing one
            
            // Create bot_leaf for old entries
            uint8_t old_bot = old_chunk & 0xFF;
            uint64_t* old_bot_leaf = alloc_node(bot_internal_size_u64(1));
            Bitmap256 old_bot_bm{};
            old_bot_bm.set_bit(old_bot);
            bot_bitmap(old_bot_leaf) = old_bot_bm;
            
            h->skip = static_cast<uint8_t>(remaining_skip);
            if (remaining_skip > 0) {
                uint64_t mask = (1ULL << (remaining_skip * 16)) - 1;
                h->prefix = actual & mask;
            } else {
                h->prefix = 0;
            }
            bot_internal_children(old_bot_leaf)[0] = reinterpret_cast<uint64_t>(node);
            
            // Create bot_leaf for new entry
            uint8_t new_bot = new_chunk & 0xFF;
            constexpr int child_bits = BITS - 16;
            uint64_t* new_leaf = alloc_node(leaf_compact_size_u64<child_bits>(1));
            NodeHeader* new_h = get_header(new_leaf);
            new_h->count = 1;
            new_h->skip = static_cast<uint8_t>(remaining_skip);
            if (remaining_skip > 0) {
                new_h->prefix = expected & ((1ULL << (remaining_skip * 16)) - 1);
            } else {
                new_h->prefix = 0;
            }
            new_h->set_leaf(true);
            
            using ChildK = typename suffix_traits<child_bits>::type;
            ChildK* new_keys_data = leaf_keys_data<child_bits>(new_leaf, 1);
            new_keys_data[0] = static_cast<ChildK>(extract_suffix<child_bits>(ik));
            if constexpr (value_inline) {
                std::memcpy(&leaf_values<child_bits>(new_leaf, 1)[0], &value, sizeof(value_slot_type));
            } else {
                leaf_values<child_bits>(new_leaf, 1)[0] = static_cast<uint64_t>(value);
            }
            
            // Build indices (no-op for count=1)
            ChildK* idx_start = leaf_keys<child_bits>(new_leaf);
            build_indices(idx_start, new_keys_data, 1);
            
            uint64_t* new_bot_leaf = alloc_node(bot_internal_size_u64(1));
            Bitmap256 new_bot_bm{};
            new_bot_bm.set_bit(new_bot);
            bot_bitmap(new_bot_leaf) = new_bot_bm;
            bot_internal_children(new_bot_leaf)[0] = reinterpret_cast<uint64_t>(new_leaf);
            
            // Place in correct order
            uint64_t* top_ch = top_children<BITS>(split_node);
            if (new_top < old_top) {
                top_ch[0] = reinterpret_cast<uint64_t>(new_bot_leaf);
                top_ch[1] = reinterpret_cast<uint64_t>(old_bot_leaf);
            } else {
                top_ch[0] = reinterpret_cast<uint64_t>(old_bot_leaf);
                top_ch[1] = reinterpret_cast<uint64_t>(new_bot_leaf);
            }
            
            return {split_node, true};
        }
    }
    
public:
    struct DebugStats {
        struct Level {
            size_t compact_leaf = 0;
            size_t compact_leaf_compressed = 0;  // With skip > 0
            size_t split_nodes = 0;
            size_t split_nodes_compressed = 0;   // With skip > 0
            size_t bot_leaf = 0;
            size_t bot_internal = 0;
            size_t entries = 0;
            size_t nodes = 0;
            size_t bytes = 0;
            size_t leaf_hist[258] = {};
        };
        Level levels[4];
        size_t total_nodes = 0;
        size_t total_bytes = 0;
        size_t total_entries = 0;
    };
    
    DebugStats debug_stats() const noexcept {
        DebugStats s{};
        collect_stats<static_cast<int>(key_bits)>(root_, s);
        for (int i = 0; i < 4; ++i) {
            s.total_nodes += s.levels[i].nodes;
            s.total_bytes += s.levels[i].bytes;
            s.total_entries += s.levels[i].entries;
        }
        return s;
    }
    
private:
    template<int BITS>
    void collect_stats(const uint64_t* node, DebugStats& s) const noexcept {
        if constexpr (BITS <= 0) return;
        else {
            if (!node) return;
            
            const NodeHeader* h = get_header(node);
            
            // Handle skip - dispatch to correct bit width
            if (h->skip > 0) {
                int actual_bits = BITS - h->skip * 16;
                if (actual_bits == 48) { collect_stats_at_bits<48>(node, h, s, true); return; }
                if (actual_bits == 32) { collect_stats_at_bits<32>(node, h, s, true); return; }
                if (actual_bits == 16) { collect_stats_at_bits<16>(node, h, s, true); return; }
                return;
            }
            
            collect_stats_at_bits<BITS>(node, h, s, false);
        }
    }
    
    template<int BITS>
    void collect_stats_at_bits(const uint64_t* node, const NodeHeader* h, DebugStats& s, bool compressed) const noexcept {
        if constexpr (BITS <= 0) return;
        else {
            constexpr int level_idx = (static_cast<int>(key_bits) - BITS) / 16;
            auto& L = s.levels[level_idx < 4 ? level_idx : 3];
            
            if (h->is_leaf() && !h->is_split()) {
                L.compact_leaf++;
                if (compressed) L.compact_leaf_compressed++;
                L.nodes++;
                L.entries += h->count;
                L.bytes += leaf_compact_size_u64<BITS>(h->count) * 8;
                L.leaf_hist[h->count < 257 ? h->count : 257]++;
            } else if (h->is_split()) {
                L.split_nodes++;
                if (compressed) L.split_nodes_compressed++;
                L.nodes++;
                L.bytes += split_top_size_u64<BITS>(h->top_count) * 8;
                
                const Bitmap256& top_bm = top_bitmap(node);
                const uint64_t* top_ch = top_children<BITS>(node);
                
                int slot = 0;
                for (int idx = top_bm.find_next_set(0); idx >= 0; idx = top_bm.find_next_set(idx + 1)) {
                    const uint64_t* bot = reinterpret_cast<const uint64_t*>(top_ch[slot]);
                    
                    bool is_leaf = (BITS == 16) || bot_is_leaf_bitmap(node).has_bit(idx);
                    
                    if (is_leaf) {
                        L.bot_leaf++;
                        uint32_t bot_count = bot_leaf_count<BITS>(bot);
                        L.entries += bot_count;
                        L.bytes += bot_leaf_size_u64<BITS>(bot_count) * 8;
                    } else {
                        if constexpr (BITS > 16) {
                            L.bot_internal++;
                            const Bitmap256& bot_bm = bot_bitmap(bot);
                            int bot_count = bot_bm.popcount();
                            L.bytes += bot_internal_size_u64(bot_count) * 8;
                            
                            const uint64_t* children = bot_internal_children(bot);
                            for (int i = 0; i < bot_count; ++i) {
                                collect_stats<BITS - 16>(reinterpret_cast<const uint64_t*>(children[i]), s);
                            }
                        }
                    }
                    ++slot;
                }
            }
        }
    }
};

} // namespace kn3

#endif // KNTRIE3_HPP
