#ifndef KNTRIE2_HPP
#define KNTRIE2_HPP

#include <cstdint>
#include <cstring>
#include <bit>
#include <memory>
#include <type_traits>
#include <utility>

namespace kn {

// =============================================================================
// Key Traits - defines types and sizes for each level
// =============================================================================

template<int BITS> struct key_traits;

template<> struct key_traits<60> {
    using leaf_key_type = uint64_t;      // 60 bits stored in 64
    using internal_key_type = uint16_t;  // 12-bit chunks for internal nodes
    static constexpr size_t leaf_key_size = sizeof(uint64_t);
};

template<> struct key_traits<48> {
    using leaf_key_type = uint64_t;      // 48 bits
    using internal_key_type = uint16_t;
    static constexpr size_t leaf_key_size = sizeof(uint64_t);
};

template<> struct key_traits<36> {
    using leaf_key_type = uint64_t;      // 36 bits
    using internal_key_type = uint16_t;
    static constexpr size_t leaf_key_size = sizeof(uint64_t);
};

template<> struct key_traits<24> {
    using leaf_key_type = uint32_t;      // 24 bits
    using internal_key_type = uint16_t;
    static constexpr size_t leaf_key_size = sizeof(uint32_t);
};

template<> struct key_traits<12> {
    using leaf_key_type = uint16_t;      // 12 bits
    using internal_key_type = uint16_t;
    static constexpr size_t leaf_key_size = sizeof(uint16_t);
};

// Special for 32-bit key level 1 (6-bit, single bitmap - always internal)
template<> struct key_traits<30> {
    using leaf_key_type = uint32_t;      // 30 bits
    using internal_key_type = uint16_t;
    static constexpr size_t leaf_key_size = sizeof(uint32_t);
};

// =============================================================================
// Main kntrie2 class
// =============================================================================

template<typename KEY, typename VALUE, typename ALLOC = std::allocator<uint64_t>>
class kntrie2 {
    static_assert(std::is_integral_v<KEY>, "KEY must be an integral type");
    static_assert(sizeof(KEY) == 4 || sizeof(KEY) == 8, "KEY must be 32 or 64 bits");
    
public:
    using key_type = KEY;
    using mapped_type = VALUE;
    using value_type = std::pair<const KEY, VALUE>;
    using size_type = std::size_t;
    using difference_type = std::ptrdiff_t;
    using allocator_type = ALLOC;
    
private:
    static constexpr bool is_signed_key = std::is_signed_v<KEY>;
    static constexpr size_t key_bits = sizeof(KEY) * 8;
    static constexpr bool value_inline = sizeof(VALUE) <= 8 && std::is_trivially_copyable_v<VALUE>;
    
    // Root configuration based on key size
    static constexpr size_t root_bits = (key_bits == 64) ? 4 : 2;
    static constexpr size_t root_size = size_t{1} << root_bits;
    
    // For 64-bit: root[16] → 60 bits remaining (5 levels of 12-bit)
    // For 32-bit: root[4] → 30 bits remaining (first level 6-bit, then 2 levels of 12-bit)
    static constexpr int START_BITS = (key_bits == 64) ? 60 : 30;
    
    using stored_value_type = std::conditional_t<value_inline, VALUE, uint64_t>;
    using value_alloc_type = typename std::allocator_traits<ALLOC>::template rebind_alloc<VALUE>;
    
    // ==========================================================================
    // Constants
    // ==========================================================================
    
    static constexpr size_t COMPACT_MAX = 64;
    static constexpr size_t HEADER_U64 = 2;  // Header is 16 bytes = 2 uint64_t
    static constexpr size_t BITMAP_OFFSET = HEADER_U64;
    
    // ==========================================================================
    // Node Header (16 bytes)
    // ==========================================================================
    
    struct alignas(16) NodeHeader {
        uint64_t prefix;      // skip chunks packed (12 bits each)
        uint32_t count;       // total entries
        uint16_t top_count;   // buckets when split (0 = compact)
        uint8_t skip;         // number of 12-bit chunks to skip
        uint8_t flags;        // bit 0: is_leaf
        
        bool is_leaf() const noexcept { return flags & 1; }
        void set_leaf(bool v) noexcept { flags = (flags & ~1) | (v ? 1 : 0); }
        bool is_split() const noexcept { return top_count > 0; }
    };
    
    static_assert(sizeof(NodeHeader) == 16, "NodeHeader must be 16 bytes");
    
    static NodeHeader* get_header(uint64_t* node) noexcept {
        return reinterpret_cast<NodeHeader*>(node);
    }
    static const NodeHeader* get_header(const uint64_t* node) noexcept {
        return reinterpret_cast<const NodeHeader*>(node);
    }
    
    // ==========================================================================
    // Key Conversion
    // ==========================================================================
    
    static constexpr uint64_t key_to_internal(KEY k) noexcept {
        uint64_t result;
        if constexpr (sizeof(KEY) == 4) {
            result = static_cast<uint32_t>(k);
        } else {
            result = static_cast<uint64_t>(k);
        }
        if constexpr (is_signed_key) {
            constexpr uint64_t sign_bit = 1ULL << (key_bits - 1);
            result ^= sign_bit;
        }
        return result;
    }
    
    static constexpr size_t extract_root_index(uint64_t ik) noexcept {
        return static_cast<size_t>(ik >> (key_bits - root_bits));
    }
    
    // Extract 12-bit chunk at given BITS level
    template<int BITS>
    static constexpr uint16_t extract_chunk(uint64_t ik) noexcept {
        if constexpr (BITS <= 12) {
            return static_cast<uint16_t>(ik & 0xFFF);
        } else {
            return static_cast<uint16_t>((ik >> (BITS - 12)) & 0xFFF);
        }
    }
    
    // Extract full BITS-width suffix for leaf storage
    template<int BITS>
    static constexpr typename key_traits<BITS>::leaf_key_type extract_suffix(uint64_t ik) noexcept {
        using K = typename key_traits<BITS>::leaf_key_type;
        constexpr uint64_t mask = (1ULL << BITS) - 1;
        return static_cast<K>(ik & mask);
    }
    
    // Extract prefix for skip compression
    static constexpr uint64_t extract_prefix(uint64_t ik, int bits_pos, int skip_count) noexcept {
        // Extract skip_count * 12 bits starting at bits_pos
        int total_bits = skip_count * 12;
        int shift = bits_pos - total_bits;
        uint64_t mask = (1ULL << total_bits) - 1;
        return (ik >> shift) & mask;
    }
    
    // ==========================================================================
    // Value Storage
    // ==========================================================================
    
    stored_value_type store_value(const VALUE& val) {
        if constexpr (value_inline) {
            return val;
        } else {
            value_alloc_type va(alloc_);
            VALUE* ptr = std::allocator_traits<value_alloc_type>::allocate(va, 1);
            std::allocator_traits<value_alloc_type>::construct(va, ptr, val);
            return reinterpret_cast<uint64_t>(ptr);
        }
    }
    
    VALUE load_value(stored_value_type stored) const noexcept {
        if constexpr (value_inline) {
            return stored;
        } else {
            return *reinterpret_cast<VALUE*>(stored);
        }
    }
    
    void destroy_value(stored_value_type stored) noexcept {
        if constexpr (!value_inline) {
            value_alloc_type va(alloc_);
            VALUE* ptr = reinterpret_cast<VALUE*>(stored);
            std::allocator_traits<value_alloc_type>::destroy(va, ptr);
            std::allocator_traits<value_alloc_type>::deallocate(va, ptr, 1);
        }
    }
    
    // ==========================================================================
    // Memory Allocation
    // ==========================================================================
    
    uint64_t* alloc_node(size_t u64_count) {
        uint64_t* node = alloc_.allocate(u64_count);
        std::memset(node, 0, u64_count * sizeof(uint64_t));
        return node;
    }
    
    void dealloc_node(uint64_t* node, size_t u64_count) noexcept {
        alloc_.deallocate(node, u64_count);
    }
    
    // Internal compact node size: header + uint16_t keys + uint64_t children
    static constexpr size_t internal_compact_size_u64(size_t count) noexcept {
        size_t keys_bytes = count * sizeof(uint16_t);
        size_t keys_u64 = (keys_bytes + 7) / 8;
        return HEADER_U64 + keys_u64 + count;
    }
    
    // Leaf compact node size: header + BITS-width keys + uint64_t values
    template<int BITS>
    static constexpr size_t leaf_compact_size_u64(size_t count) noexcept {
        size_t keys_bytes = count * key_traits<BITS>::leaf_key_size;
        size_t keys_u64 = (keys_bytes + 7) / 8;
        return HEADER_U64 + keys_u64 + count;
    }
    
    // Split top node size: header + bitmap + child_ptrs
    static constexpr size_t split_top_size_u64(size_t child_count) noexcept {
        return HEADER_U64 + 1 + child_count;
    }
    
    // Split bottom node size: bitmap + data (no header)
    static constexpr size_t split_bot_size_u64(size_t entry_count) noexcept {
        return 1 + entry_count;
    }
    
    // ==========================================================================
    // Internal Node Accessors (12-bit keys)
    // ==========================================================================
    
    static uint16_t* internal_keys(uint64_t* node) noexcept {
        return reinterpret_cast<uint16_t*>(node + HEADER_U64);
    }
    static const uint16_t* internal_keys(const uint64_t* node) noexcept {
        return reinterpret_cast<const uint16_t*>(node + HEADER_U64);
    }
    
    static uint64_t* internal_children(uint64_t* node, size_t count) noexcept {
        size_t keys_u64 = (count * sizeof(uint16_t) + 7) / 8;
        return node + HEADER_U64 + keys_u64;
    }
    static const uint64_t* internal_children(const uint64_t* node, size_t count) noexcept {
        size_t keys_u64 = (count * sizeof(uint16_t) + 7) / 8;
        return node + HEADER_U64 + keys_u64;
    }
    
    // ==========================================================================
    // Leaf Node Accessors (BITS-width keys)
    // ==========================================================================
    
    template<int BITS>
    static typename key_traits<BITS>::leaf_key_type* leaf_keys(uint64_t* node) noexcept {
        using K = typename key_traits<BITS>::leaf_key_type;
        return reinterpret_cast<K*>(node + HEADER_U64);
    }
    
    template<int BITS>
    static const typename key_traits<BITS>::leaf_key_type* leaf_keys(const uint64_t* node) noexcept {
        using K = typename key_traits<BITS>::leaf_key_type;
        return reinterpret_cast<const K*>(node + HEADER_U64);
    }
    
    template<int BITS>
    static uint64_t* leaf_values(uint64_t* node, size_t count) noexcept {
        size_t keys_bytes = count * key_traits<BITS>::leaf_key_size;
        size_t keys_u64 = (keys_bytes + 7) / 8;
        return node + HEADER_U64 + keys_u64;
    }
    
    template<int BITS>
    static const uint64_t* leaf_values(const uint64_t* node, size_t count) noexcept {
        size_t keys_bytes = count * key_traits<BITS>::leaf_key_size;
        size_t keys_u64 = (keys_bytes + 7) / 8;
        return node + HEADER_U64 + keys_u64;
    }
    
    // ==========================================================================
    // Split Node Accessors
    // ==========================================================================
    
    static uint64_t& top_bitmap(uint64_t* node) noexcept { return node[BITMAP_OFFSET]; }
    static uint64_t top_bitmap(const uint64_t* node) noexcept { return node[BITMAP_OFFSET]; }
    
    static uint64_t* top_children(uint64_t* node) noexcept { return node + BITMAP_OFFSET + 1; }
    static const uint64_t* top_children(const uint64_t* node) noexcept { return node + BITMAP_OFFSET + 1; }
    
    static uint64_t& bot_bitmap(uint64_t* bot) noexcept { return bot[0]; }
    static uint64_t bot_bitmap(const uint64_t* bot) noexcept { return bot[0]; }
    
    static uint64_t* bot_data(uint64_t* bot) noexcept { return bot + 1; }
    static const uint64_t* bot_data(const uint64_t* bot) noexcept { return bot + 1; }
    
    // ==========================================================================
    // Bitmap Helpers
    // ==========================================================================
    
    static uint8_t extract_top6(uint16_t chunk) noexcept { return chunk >> 6; }
    static uint8_t extract_bot6(uint16_t chunk) noexcept { return chunk & 0x3F; }
    
    static int bitmap_slot(uint64_t bitmap, uint8_t index) noexcept {
        uint64_t mask = 1ULL << index;
        if (!(bitmap & mask)) return -1;
        uint64_t below = bitmap & (mask - 1);
        return std::popcount(below);
    }
    
    // ==========================================================================
    // Search Helpers
    // ==========================================================================
    
    // Linear search in compact internal node (12-bit keys)
    static uint64_t* search_internal_compact(uint64_t* node, const NodeHeader* h, uint16_t chunk) noexcept {
        const uint16_t* keys = internal_keys(node);
        const uint64_t* children = internal_children(node, h->count);
        
        for (uint32_t i = 0; i < h->count; ++i) {
            if (keys[i] == chunk) {
                return reinterpret_cast<uint64_t*>(children[i]);
            }
        }
        return nullptr;
    }
    
    // Linear search in compact leaf node (BITS-width keys)
    template<int BITS>
    static const uint64_t* search_leaf_compact(const uint64_t* node, const NodeHeader* h, uint64_t ik) noexcept {
        using K = typename key_traits<BITS>::leaf_key_type;
        K suffix = extract_suffix<BITS>(ik);
        
        const K* keys = leaf_keys<BITS>(node);
        const uint64_t* values = leaf_values<BITS>(node, h->count);
        
        for (uint32_t i = 0; i < h->count; ++i) {
            if (keys[i] == suffix) {
                return &values[i];
            }
        }
        return nullptr;
    }
    
    // Bitmap search in split internal node
    static uint64_t* search_internal_split(uint64_t* node, uint16_t chunk) noexcept {
        uint8_t top_idx = extract_top6(chunk);
        uint8_t bot_idx = extract_bot6(chunk);
        
        int top_slot = bitmap_slot(top_bitmap(node), top_idx);
        if (top_slot < 0) return nullptr;
        
        uint64_t* bot = reinterpret_cast<uint64_t*>(top_children(node)[top_slot]);
        
        int bot_slot = bitmap_slot(bot_bitmap(bot), bot_idx);
        if (bot_slot < 0) return nullptr;
        
        return reinterpret_cast<uint64_t*>(bot_data(bot)[bot_slot]);
    }
    
    // Bitmap search in split leaf node
    template<int BITS>
    static const uint64_t* search_leaf_split(const uint64_t* node, uint64_t ik) noexcept {
        using K = typename key_traits<BITS>::leaf_key_type;
        K suffix = extract_suffix<BITS>(ik);
        
        // For split leaves, we use top 6 bits of suffix for top bitmap
        // and bottom 6 bits for bottom bitmap
        // But wait - for BITS=60, suffix is 60 bits, so we need to split differently
        // Actually, split is based on the 12-bit chunk at this level
        uint16_t chunk = extract_chunk<BITS>(ik);
        uint8_t top_idx = extract_top6(chunk);
        uint8_t bot_idx = extract_bot6(chunk);
        
        int top_slot = bitmap_slot(top_bitmap(node), top_idx);
        if (top_slot < 0) return nullptr;
        
        const uint64_t* bot = reinterpret_cast<const uint64_t*>(top_children(node)[top_slot]);
        
        int bot_slot = bitmap_slot(bot_bitmap(bot), bot_idx);
        if (bot_slot < 0) return nullptr;
        
        // Now we have the slot, but we need to compare full suffix
        // Actually in split leaf, each bottom entry stores (suffix, value)
        // Hmm, the plan says bottom node is just [bitmap][data...]
        // For split leaf, data is values. But how do we distinguish keys?
        // 
        // Rethinking: In split mode, the 12-bit chunk IS the key for this level.
        // The value is stored directly. No suffix needed because position encodes key.
        return &bot_data(bot)[bot_slot];
    }
    
    // ==========================================================================
    // Find Implementation
    // ==========================================================================
    
    template<int BITS>
    const VALUE* find_impl(uint64_t* node, const NodeHeader* h, uint64_t ik,
                           uint64_t skip_prefix, int skip_remaining) const noexcept {
        if constexpr (BITS <= 0) {
            return nullptr;
        } else {
            uint16_t chunk = extract_chunk<BITS>(ik);
            
            // Handle skip prefix consumption
            if (skip_remaining > 0) {
                uint16_t skip_chunk = static_cast<uint16_t>((skip_prefix >> (skip_remaining - 12)) & 0xFFF);
                if (chunk != skip_chunk) return nullptr;
                return find_impl<BITS - 12>(node, h, ik, skip_prefix, skip_remaining - 12);
            }
            
            // Check if entering new skip region
            if (h->skip > 0) {
                uint16_t skip_chunk = static_cast<uint16_t>((h->prefix >> ((h->skip - 1) * 12)) & 0xFFF);
                if (chunk != skip_chunk) return nullptr;
                return find_impl<BITS - 12>(node, h, ik, h->prefix, (h->skip - 1) * 12);
            }
            
            // At decision point
            if (h->is_leaf()) {
                const uint64_t* val_ptr;
                if (h->is_split()) {
                    val_ptr = search_leaf_split<BITS>(node, ik);
                } else {
                    val_ptr = search_leaf_compact<BITS>(node, h, ik);
                }
                if (!val_ptr) return nullptr;
                
                if constexpr (value_inline) {
                    return reinterpret_cast<const VALUE*>(val_ptr);
                } else {
                    return reinterpret_cast<const VALUE*>(*val_ptr);
                }
            } else {
                uint64_t* child;
                if (h->is_split()) {
                    child = search_internal_split(node, chunk);
                } else {
                    child = search_internal_compact(node, h, chunk);
                }
                
                if (!child) return nullptr;
                return find_impl<BITS - 12>(child, get_header(child), ik, 0, 0);
            }
        }
    }
    
    // ==========================================================================
    // Insert Implementation
    // ==========================================================================
    
    struct InsertResult {
        uint64_t* new_node;
        bool inserted;
    };
    
    // Create empty compact node
    uint64_t* alloc_empty_compact(bool is_leaf) {
        size_t size = is_leaf ? leaf_compact_size_u64<12>(0) : internal_compact_size_u64(0);
        uint64_t* node = alloc_node(size);
        NodeHeader* h = get_header(node);
        h->count = 0;
        h->top_count = 0;
        h->skip = 0;
        h->set_leaf(is_leaf);
        return node;
    }
    
    // Create single-entry leaf node at given BITS level
    template<int BITS>
    uint64_t* create_single_leaf(uint64_t ik, stored_value_type value) {
        if constexpr (BITS <= 0) {
            return nullptr;  // Should never happen
        } else {
            using K = typename key_traits<BITS>::leaf_key_type;
            
            uint64_t* node = alloc_node(leaf_compact_size_u64<BITS>(1));
            NodeHeader* h = get_header(node);
            h->count = 1;
            h->top_count = 0;
            h->skip = 0;
            h->set_leaf(true);
            
            K suffix = extract_suffix<BITS>(ik);
            leaf_keys<BITS>(node)[0] = suffix;
            
            uint64_t* values = leaf_values<BITS>(node, 1);
            if constexpr (value_inline) {
                std::memcpy(&values[0], &value, sizeof(stored_value_type));
            } else {
                values[0] = static_cast<uint64_t>(value);
            }
            
            return node;
        }
    }
    
    template<int BITS>
    InsertResult insert_impl(uint64_t* node, NodeHeader* h, uint64_t ik, stored_value_type value,
                             uint64_t skip_prefix, int skip_remaining) {
        if constexpr (BITS <= 0) {
            return {node, false};
        } else {
            uint16_t chunk = extract_chunk<BITS>(ik);
            
            // Handle skip prefix
            if (skip_remaining > 0) {
                uint16_t skip_chunk = static_cast<uint16_t>((skip_prefix >> (skip_remaining - 12)) & 0xFFF);
                if (chunk != skip_chunk) {
                    // Prefix mismatch - need to split
                    return split_on_prefix<BITS>(node, h, ik, value, skip_prefix, skip_remaining, chunk, skip_chunk);
                }
                return insert_impl<BITS - 12>(node, h, ik, value, skip_prefix, skip_remaining - 12);
            }
            
            // Check if entering new skip region  
            if (h->skip > 0) {
                uint16_t skip_chunk = static_cast<uint16_t>((h->prefix >> ((h->skip - 1) * 12)) & 0xFFF);
                if (chunk != skip_chunk) {
                    return split_on_prefix<BITS>(node, h, ik, value, h->prefix, h->skip * 12, chunk, skip_chunk);
                }
                return insert_impl<BITS - 12>(node, h, ik, value, h->prefix, (h->skip - 1) * 12);
            }
            
            // At decision point
            if (h->is_leaf()) {
                return insert_into_leaf<BITS>(node, h, ik, value);
            } else {
                return insert_into_internal<BITS>(node, h, ik, value, chunk);
            }
        }
    }
    
    template<int BITS>
    InsertResult insert_into_leaf(uint64_t* node, NodeHeader* h, uint64_t ik, stored_value_type value) {
        using K = typename key_traits<BITS>::leaf_key_type;
        K suffix = extract_suffix<BITS>(ik);
        
        if (h->is_split()) {
            return insert_into_split_leaf<BITS>(node, h, ik, value);
        }
        
        // Compact leaf - search for existing key
        K* keys = leaf_keys<BITS>(node);
        uint64_t* values = leaf_values<BITS>(node, h->count);
        
        for (uint32_t i = 0; i < h->count; ++i) {
            if (keys[i] == suffix) {
                // Update existing
                if constexpr (!value_inline) {
                    destroy_value(static_cast<stored_value_type>(values[i]));
                }
                if constexpr (value_inline) {
                    std::memcpy(&values[i], &value, sizeof(stored_value_type));
                } else {
                    values[i] = static_cast<uint64_t>(value);
                }
                return {node, false};
            }
        }
        
        // Need to add new entry
        if (h->count >= COMPACT_MAX) {
            return convert_leaf_to_split<BITS>(node, h, ik, value);
        }
        
        // Grow compact leaf
        size_t new_count = h->count + 1;
        uint64_t* new_node = alloc_node(leaf_compact_size_u64<BITS>(new_count));
        NodeHeader* new_h = get_header(new_node);
        *new_h = *h;
        new_h->count = static_cast<uint32_t>(new_count);
        
        K* new_keys = leaf_keys<BITS>(new_node);
        uint64_t* new_values = leaf_values<BITS>(new_node, new_count);
        
        // Copy existing
        for (uint32_t i = 0; i < h->count; ++i) {
            new_keys[i] = keys[i];
            new_values[i] = values[i];
        }
        
        // Add new entry
        new_keys[h->count] = suffix;
        if constexpr (value_inline) {
            std::memcpy(&new_values[h->count], &value, sizeof(stored_value_type));
        } else {
            new_values[h->count] = static_cast<uint64_t>(value);
        }
        
        dealloc_node(node, leaf_compact_size_u64<BITS>(h->count));
        return {new_node, true};
    }
    
    template<int BITS>
    InsertResult insert_into_internal(uint64_t* node, NodeHeader* h, uint64_t ik, 
                                       stored_value_type value, uint16_t chunk) {
        if constexpr (BITS <= 12) {
            // At BITS=12, should only have leaves, not internal nodes
            // This shouldn't happen - fall back to treating as leaf
            return insert_into_leaf<BITS>(node, h, ik, value);
        } else {
            if (h->is_split()) {
                return insert_into_split_internal<BITS>(node, h, ik, value, chunk);
            }
            
            // Compact internal - search for existing chunk
            uint16_t* keys = internal_keys(node);
            uint64_t* children = internal_children(node, h->count);
            
            for (uint32_t i = 0; i < h->count; ++i) {
                if (keys[i] == chunk) {
                    // Found - recurse into child
                    uint64_t* child = reinterpret_cast<uint64_t*>(children[i]);
                    auto [new_child, inserted] = insert_impl<BITS - 12>(
                        child, get_header(child), ik, value, 0, 0);
                    if (new_child != child) {
                        children[i] = reinterpret_cast<uint64_t>(new_child);
                    }
                    return {node, inserted};
                }
            }
            
            // Need to add new child
            if (h->count >= COMPACT_MAX) {
                return convert_internal_to_split<BITS>(node, h, ik, value, chunk);
            }
            
            // Create new child leaf
            uint64_t* child = create_single_leaf<BITS - 12>(ik, value);
            
            // Grow compact internal
            size_t new_count = h->count + 1;
            uint64_t* new_node = alloc_node(internal_compact_size_u64(new_count));
            NodeHeader* new_h = get_header(new_node);
            *new_h = *h;
            new_h->count = static_cast<uint32_t>(new_count);
            
            uint16_t* new_keys = internal_keys(new_node);
            uint64_t* new_children = internal_children(new_node, new_count);
            
            // Copy existing
            for (uint32_t i = 0; i < h->count; ++i) {
                new_keys[i] = keys[i];
                new_children[i] = children[i];
            }
            
            // Add new child
            new_keys[h->count] = chunk;
            new_children[h->count] = reinterpret_cast<uint64_t>(child);
            
            dealloc_node(node, internal_compact_size_u64(h->count));
            return {new_node, true};
        }
    }
    
    // Split leaf insert (bitmap-based)
    template<int BITS>
    InsertResult insert_into_split_leaf(uint64_t* node, NodeHeader* h, uint64_t ik, stored_value_type value) {
        uint16_t chunk = extract_chunk<BITS>(ik);
        uint8_t top_idx = extract_top6(chunk);
        uint8_t bot_idx = extract_bot6(chunk);
        
        uint64_t top_bm = top_bitmap(node);
        uint64_t* top_ch = top_children(node);
        
        int top_slot = bitmap_slot(top_bm, top_idx);
        
        if (top_slot < 0) {
            // Need new bottom node
            uint64_t* bot = alloc_node(split_bot_size_u64(1));
            bot_bitmap(bot) = 1ULL << bot_idx;
            if constexpr (value_inline) {
                std::memcpy(&bot_data(bot)[0], &value, sizeof(stored_value_type));
            } else {
                bot_data(bot)[0] = static_cast<uint64_t>(value);
            }
            
            // Reallocate top with new child
            size_t old_top_count = h->top_count;
            size_t new_top_count = old_top_count + 1;
            
            uint64_t* new_node = alloc_node(split_top_size_u64(new_top_count));
            NodeHeader* new_h = get_header(new_node);
            *new_h = *h;
            new_h->count = h->count + 1;
            new_h->top_count = static_cast<uint16_t>(new_top_count);
            
            // Insert into sorted position
            uint64_t new_top_bm = top_bm | (1ULL << top_idx);
            top_bitmap(new_node) = new_top_bm;
            
            int new_slot = bitmap_slot(new_top_bm, top_idx);
            uint64_t* new_top_ch = top_children(new_node);
            
            for (size_t i = 0; i < static_cast<size_t>(new_slot); ++i) {
                new_top_ch[i] = top_ch[i];
            }
            new_top_ch[new_slot] = reinterpret_cast<uint64_t>(bot);
            for (size_t i = new_slot; i < old_top_count; ++i) {
                new_top_ch[i + 1] = top_ch[i];
            }
            
            dealloc_node(node, split_top_size_u64(old_top_count));
            return {new_node, true};
        }
        
        // Have bottom node
        uint64_t* bot = reinterpret_cast<uint64_t*>(top_ch[top_slot]);
        uint64_t bot_bm = bot_bitmap(bot);
        
        int bot_slot = bitmap_slot(bot_bm, bot_idx);
        
        if (bot_slot >= 0) {
            // Update existing
            if constexpr (!value_inline) {
                destroy_value(static_cast<stored_value_type>(bot_data(bot)[bot_slot]));
            }
            if constexpr (value_inline) {
                std::memcpy(&bot_data(bot)[bot_slot], &value, sizeof(stored_value_type));
            } else {
                bot_data(bot)[bot_slot] = static_cast<uint64_t>(value);
            }
            return {node, false};
        }
        
        // Add to bottom
        size_t old_bot_count = std::popcount(bot_bm);
        size_t new_bot_count = old_bot_count + 1;
        
        uint64_t* new_bot = alloc_node(split_bot_size_u64(new_bot_count));
        uint64_t new_bot_bm = bot_bm | (1ULL << bot_idx);
        bot_bitmap(new_bot) = new_bot_bm;
        
        int new_bot_slot = bitmap_slot(new_bot_bm, bot_idx);
        uint64_t* old_data = bot_data(bot);
        uint64_t* new_data = bot_data(new_bot);
        
        for (size_t i = 0; i < static_cast<size_t>(new_bot_slot); ++i) {
            new_data[i] = old_data[i];
        }
        if constexpr (value_inline) {
            std::memcpy(&new_data[new_bot_slot], &value, sizeof(stored_value_type));
        } else {
            new_data[new_bot_slot] = static_cast<uint64_t>(value);
        }
        for (size_t i = new_bot_slot; i < old_bot_count; ++i) {
            new_data[i + 1] = old_data[i];
        }
        
        top_ch[top_slot] = reinterpret_cast<uint64_t>(new_bot);
        h->count++;
        
        dealloc_node(bot, split_bot_size_u64(old_bot_count));
        return {node, true};
    }
    
    // Split internal insert
    template<int BITS>
    InsertResult insert_into_split_internal(uint64_t* node, NodeHeader* h, uint64_t ik,
                                             stored_value_type value, uint16_t chunk) {
        if constexpr (BITS <= 12) {
            // At BITS=12, should only have leaves
            return {node, false};
        } else {
            uint8_t top_idx = extract_top6(chunk);
            uint8_t bot_idx = extract_bot6(chunk);
            
            uint64_t top_bm = top_bitmap(node);
            uint64_t* top_ch = top_children(node);
            
            int top_slot = bitmap_slot(top_bm, top_idx);
            
            if (top_slot < 0) {
                // Need new bottom node with one child
                uint64_t* child = create_single_leaf<BITS - 12>(ik, value);
            
            uint64_t* bot = alloc_node(split_bot_size_u64(1));
            bot_bitmap(bot) = 1ULL << bot_idx;
            bot_data(bot)[0] = reinterpret_cast<uint64_t>(child);
            
            // Reallocate top
            size_t old_top_count = h->top_count;
            size_t new_top_count = old_top_count + 1;
            
            uint64_t* new_node = alloc_node(split_top_size_u64(new_top_count));
            NodeHeader* new_h = get_header(new_node);
            *new_h = *h;
            new_h->count = h->count + 1;
            new_h->top_count = static_cast<uint16_t>(new_top_count);
            
            uint64_t new_top_bm = top_bm | (1ULL << top_idx);
            top_bitmap(new_node) = new_top_bm;
            
            int new_slot = bitmap_slot(new_top_bm, top_idx);
            uint64_t* new_top_ch = top_children(new_node);
            
            for (size_t i = 0; i < static_cast<size_t>(new_slot); ++i) {
                new_top_ch[i] = top_ch[i];
            }
            new_top_ch[new_slot] = reinterpret_cast<uint64_t>(bot);
            for (size_t i = new_slot; i < old_top_count; ++i) {
                new_top_ch[i + 1] = top_ch[i];
            }
            
            dealloc_node(node, split_top_size_u64(old_top_count));
            return {new_node, true};
        }
        
        // Have bottom node
        uint64_t* bot = reinterpret_cast<uint64_t*>(top_ch[top_slot]);
        uint64_t bot_bm = bot_bitmap(bot);
        
        int bot_slot = bitmap_slot(bot_bm, bot_idx);
        
        if (bot_slot >= 0) {
            // Recurse into existing child
            uint64_t* child = reinterpret_cast<uint64_t*>(bot_data(bot)[bot_slot]);
            auto [new_child, inserted] = insert_impl<BITS - 12>(
                child, get_header(child), ik, value, 0, 0);
            if (new_child != child) {
                bot_data(bot)[bot_slot] = reinterpret_cast<uint64_t>(new_child);
            }
            if (inserted) h->count++;
            return {node, inserted};
        }
        
        // Add new child to bottom
        uint64_t* child = create_single_leaf<BITS - 12>(ik, value);
        
        size_t old_bot_count = std::popcount(bot_bm);
        size_t new_bot_count = old_bot_count + 1;
        
        uint64_t* new_bot = alloc_node(split_bot_size_u64(new_bot_count));
        uint64_t new_bot_bm = bot_bm | (1ULL << bot_idx);
        bot_bitmap(new_bot) = new_bot_bm;
        
        int new_bot_slot = bitmap_slot(new_bot_bm, bot_idx);
        uint64_t* old_data = bot_data(bot);
        uint64_t* new_data = bot_data(new_bot);
        
        for (size_t i = 0; i < static_cast<size_t>(new_bot_slot); ++i) {
            new_data[i] = old_data[i];
        }
        new_data[new_bot_slot] = reinterpret_cast<uint64_t>(child);
        for (size_t i = new_bot_slot; i < old_bot_count; ++i) {
            new_data[i + 1] = old_data[i];
        }
        
        top_ch[top_slot] = reinterpret_cast<uint64_t>(new_bot);
        h->count++;
        
        dealloc_node(bot, split_bot_size_u64(old_bot_count));
        return {node, true};
        }
    }
    
    // Convert compact leaf to split when full
    // Convert compact leaf to split/internal when full
    // At BITS > 12: convert to internal node that routes to child leaves
    // At BITS = 12: convert to split leaf (bitmap structure)
    template<int BITS>
    InsertResult convert_leaf_to_split(uint64_t* node, NodeHeader* h, uint64_t ik, stored_value_type value) {
        if constexpr (BITS <= 12) {
            // At BITS=12, convert to split leaf (bitmap indexes 12-bit keys directly)
            using K = typename key_traits<BITS>::leaf_key_type;
            
            K* old_keys = leaf_keys<BITS>(node);
            uint64_t* old_values = leaf_values<BITS>(node, h->count);
            
            // Build top bitmap from 6-bit halves of 12-bit keys
            uint64_t new_top_bm = 0;
            for (uint32_t i = 0; i < h->count; ++i) {
                new_top_bm |= 1ULL << extract_top6(static_cast<uint16_t>(old_keys[i]));
            }
            uint16_t new_key = extract_suffix<BITS>(ik);
            new_top_bm |= 1ULL << extract_top6(new_key);
            
            size_t new_top_count = std::popcount(new_top_bm);
            
            uint64_t* new_node = alloc_node(split_top_size_u64(new_top_count));
            NodeHeader* new_h = get_header(new_node);
            new_h->count = h->count + 1;
            new_h->top_count = static_cast<uint16_t>(new_top_count);
            new_h->skip = h->skip;
            new_h->prefix = h->prefix;
            new_h->set_leaf(true);
            
            top_bitmap(new_node) = new_top_bm;
            
            uint64_t* new_top_ch = top_children(new_node);
            size_t top_slot = 0;
            
            for (uint8_t top_idx = 0; top_idx < 64; ++top_idx) {
                if (!(new_top_bm & (1ULL << top_idx))) continue;
                
                uint64_t bot_bm = 0;
                for (uint32_t i = 0; i < h->count; ++i) {
                    if (extract_top6(static_cast<uint16_t>(old_keys[i])) == top_idx) {
                        bot_bm |= 1ULL << extract_bot6(static_cast<uint16_t>(old_keys[i]));
                    }
                }
                if (extract_top6(new_key) == top_idx) {
                    bot_bm |= 1ULL << extract_bot6(new_key);
                }
                
                size_t bot_count = std::popcount(bot_bm);
                uint64_t* bot = alloc_node(split_bot_size_u64(bot_count));
                bot_bitmap(bot) = bot_bm;
                
                for (uint32_t i = 0; i < h->count; ++i) {
                    if (extract_top6(static_cast<uint16_t>(old_keys[i])) == top_idx) {
                        int slot = bitmap_slot(bot_bm, extract_bot6(static_cast<uint16_t>(old_keys[i])));
                        bot_data(bot)[slot] = old_values[i];
                    }
                }
                if (extract_top6(new_key) == top_idx) {
                    int slot = bitmap_slot(bot_bm, extract_bot6(new_key));
                    if constexpr (value_inline) {
                        std::memcpy(&bot_data(bot)[slot], &value, sizeof(stored_value_type));
                    } else {
                        bot_data(bot)[slot] = static_cast<uint64_t>(value);
                    }
                }
                
                new_top_ch[top_slot++] = reinterpret_cast<uint64_t>(bot);
            }
            
            dealloc_node(node, leaf_compact_size_u64<BITS>(h->count));
            return {new_node, true};
        } else {
            // At BITS > 12: convert to internal node
            // Group entries by top 12 bits of suffix, create child leaves
            using K = typename key_traits<BITS>::leaf_key_type;
            
            K* old_keys = leaf_keys<BITS>(node);
            uint64_t* old_values = leaf_values<BITS>(node, h->count);
            
            // Count unique 12-bit chunks
            uint16_t chunks[COMPACT_MAX + 1];
            size_t chunk_counts[COMPACT_MAX + 1] = {0};
            size_t num_chunks = 0;
            
            auto add_chunk = [&](uint16_t c) {
                for (size_t i = 0; i < num_chunks; ++i) {
                    if (chunks[i] == c) { chunk_counts[i]++; return; }
                }
                chunks[num_chunks] = c;
                chunk_counts[num_chunks] = 1;
                num_chunks++;
            };
            
            for (uint32_t i = 0; i < h->count; ++i) {
                uint16_t chunk = static_cast<uint16_t>((old_keys[i] >> (BITS - 12)) & 0xFFF);
                add_chunk(chunk);
            }
            uint16_t new_chunk = extract_chunk<BITS>(ik);
            add_chunk(new_chunk);
            
            // Create internal node
            uint64_t* new_node = alloc_node(internal_compact_size_u64(num_chunks));
            NodeHeader* new_h = get_header(new_node);
            new_h->count = static_cast<uint32_t>(num_chunks);
            new_h->top_count = 0;
            new_h->skip = h->skip;
            new_h->prefix = h->prefix;
            new_h->set_leaf(false);
            
            uint16_t* new_keys = internal_keys(new_node);
            uint64_t* new_children = internal_children(new_node, num_chunks);
            
            constexpr uint64_t child_mask = (1ULL << (BITS - 12)) - 1;
            
            for (size_t c = 0; c < num_chunks; ++c) {
                new_keys[c] = chunks[c];
                
                // Create child leaf for this chunk
                size_t child_count = chunk_counts[c];
                uint64_t* child = alloc_node(leaf_compact_size_u64<BITS - 12>(child_count));
                NodeHeader* child_h = get_header(child);
                child_h->count = static_cast<uint32_t>(child_count);
                child_h->top_count = 0;
                child_h->skip = 0;
                child_h->set_leaf(true);
                
                using ChildK = typename key_traits<BITS - 12>::leaf_key_type;
                ChildK* child_keys = leaf_keys<BITS - 12>(child);
                uint64_t* child_values = leaf_values<BITS - 12>(child, child_count);
                
                size_t child_idx = 0;
                for (uint32_t i = 0; i < h->count; ++i) {
                    uint16_t entry_chunk = static_cast<uint16_t>((old_keys[i] >> (BITS - 12)) & 0xFFF);
                    if (entry_chunk == chunks[c]) {
                        child_keys[child_idx] = static_cast<ChildK>(old_keys[i] & child_mask);
                        child_values[child_idx] = old_values[i];
                        child_idx++;
                    }
                }
                if (new_chunk == chunks[c]) {
                    ChildK new_suffix = static_cast<ChildK>(ik & child_mask);
                    child_keys[child_idx] = new_suffix;
                    if constexpr (value_inline) {
                        std::memcpy(&child_values[child_idx], &value, sizeof(stored_value_type));
                    } else {
                        child_values[child_idx] = static_cast<uint64_t>(value);
                    }
                }
                
                new_children[c] = reinterpret_cast<uint64_t>(child);
            }
            
            dealloc_node(node, leaf_compact_size_u64<BITS>(h->count));
            return {new_node, true};
        }
    }
    
    // Convert compact internal to split when full
    template<int BITS>
    InsertResult convert_internal_to_split(uint64_t* node, NodeHeader* h, uint64_t ik,
                                            stored_value_type value, uint16_t new_chunk) {
        if constexpr (BITS <= 12) {
            return {node, false};  // Should not happen
        } else {
            uint16_t* old_keys = internal_keys(node);
            uint64_t* old_children = internal_children(node, h->count);
            
            // Build top bitmap
            uint64_t new_top_bm = 0;
            for (uint32_t i = 0; i < h->count; ++i) {
                new_top_bm |= 1ULL << extract_top6(old_keys[i]);
            }
            new_top_bm |= 1ULL << extract_top6(new_chunk);
            
            size_t new_top_count = std::popcount(new_top_bm);
            
            // Allocate split top
            uint64_t* new_node = alloc_node(split_top_size_u64(new_top_count));
            NodeHeader* new_h = get_header(new_node);
            new_h->count = h->count + 1;
            new_h->top_count = static_cast<uint16_t>(new_top_count);
            new_h->skip = h->skip;
            new_h->prefix = h->prefix;
            new_h->set_leaf(false);
            
            top_bitmap(new_node) = new_top_bm;
            
            // Create new child for new entry
            uint64_t* new_child = create_single_leaf<BITS - 12>(ik, value);
            
            // For each top bucket, create bottom
            uint64_t* new_top_ch = top_children(new_node);
            size_t top_slot = 0;
            
            for (uint8_t top_idx = 0; top_idx < 64; ++top_idx) {
                if (!(new_top_bm & (1ULL << top_idx))) continue;
                
                uint64_t bot_bm = 0;
                size_t bot_count = 0;
                
                for (uint32_t i = 0; i < h->count; ++i) {
                    if (extract_top6(old_keys[i]) == top_idx) {
                        bot_bm |= 1ULL << extract_bot6(old_keys[i]);
                        bot_count++;
                    }
                }
                if (extract_top6(new_chunk) == top_idx) {
                    bot_bm |= 1ULL << extract_bot6(new_chunk);
                    bot_count++;
                }
                
                uint64_t* bot = alloc_node(split_bot_size_u64(bot_count));
                bot_bitmap(bot) = bot_bm;
                
                for (uint32_t i = 0; i < h->count; ++i) {
                    if (extract_top6(old_keys[i]) == top_idx) {
                        int slot = bitmap_slot(bot_bm, extract_bot6(old_keys[i]));
                        bot_data(bot)[slot] = old_children[i];
                    }
                }
                if (extract_top6(new_chunk) == top_idx) {
                    int slot = bitmap_slot(bot_bm, extract_bot6(new_chunk));
                    bot_data(bot)[slot] = reinterpret_cast<uint64_t>(new_child);
                }
                
                new_top_ch[top_slot++] = reinterpret_cast<uint64_t>(bot);
            }
            
            dealloc_node(node, internal_compact_size_u64(h->count));
            return {new_node, true};
        }
    }
    
    // Split on prefix mismatch
    template<int BITS>
    InsertResult split_on_prefix(uint64_t* node, NodeHeader* h, uint64_t ik, stored_value_type value,
                                  uint64_t prefix, int prefix_bits, uint16_t new_chunk, uint16_t old_chunk) {
        if constexpr (BITS <= 12) {
            // At BITS=12, no skip compression should exist - this shouldn't happen
            return {node, false};
        } else {
            // Create new internal node with two children
            uint64_t* new_internal = alloc_node(internal_compact_size_u64(2));
            NodeHeader* new_h = get_header(new_internal);
            new_h->count = 2;
            new_h->top_count = 0;
            new_h->skip = 0;
            new_h->set_leaf(false);
            
            // Create new leaf for new key
            uint64_t* new_leaf = create_single_leaf<BITS - 12>(ik, value);
            
            // Reduce old node's skip
            h->skip = static_cast<uint8_t>((prefix_bits / 12) - 1);
            if (h->skip > 0) {
                h->prefix = prefix & ((1ULL << (h->skip * 12)) - 1);
            }
            
            uint16_t* keys = internal_keys(new_internal);
            uint64_t* children = internal_children(new_internal, 2);
            
            if (new_chunk < old_chunk) {
                keys[0] = new_chunk;
                keys[1] = old_chunk;
                children[0] = reinterpret_cast<uint64_t>(new_leaf);
                children[1] = reinterpret_cast<uint64_t>(node);
            } else {
                keys[0] = old_chunk;
                keys[1] = new_chunk;
                children[0] = reinterpret_cast<uint64_t>(node);
                children[1] = reinterpret_cast<uint64_t>(new_leaf);
            }
            
            return {new_internal, true};
        }
    }
    
    // ==========================================================================
    // Clear Implementation
    // ==========================================================================
    
    template<int BITS>
    void clear_impl(uint64_t* node) noexcept {
        if constexpr (BITS <= 0) {
            return;  // Should never happen
        } else {
            if (!node) return;
            
            NodeHeader* h = get_header(node);
            
            if (h->is_leaf()) {
                if constexpr (!value_inline) {
                    // Destroy values
                    if (h->is_split()) {
                        size_t top_count = h->top_count;
                        uint64_t* top_ch = top_children(node);
                        for (size_t t = 0; t < top_count; ++t) {
                            uint64_t* bot = reinterpret_cast<uint64_t*>(top_ch[t]);
                            size_t bot_count = std::popcount(bot_bitmap(bot));
                            for (size_t b = 0; b < bot_count; ++b) {
                                destroy_value(static_cast<stored_value_type>(bot_data(bot)[b]));
                            }
                            dealloc_node(bot, split_bot_size_u64(bot_count));
                        }
                    } else {
                        uint64_t* values = leaf_values<BITS>(node, h->count);
                        for (uint32_t i = 0; i < h->count; ++i) {
                            destroy_value(static_cast<stored_value_type>(values[i]));
                        }
                    }
                } else {
                    // Just free bottom nodes if split
                    if (h->is_split()) {
                        size_t top_count = h->top_count;
                        uint64_t* top_ch = top_children(node);
                        for (size_t t = 0; t < top_count; ++t) {
                            uint64_t* bot = reinterpret_cast<uint64_t*>(top_ch[t]);
                            size_t bot_count = std::popcount(bot_bitmap(bot));
                            dealloc_node(bot, split_bot_size_u64(bot_count));
                        }
                    }
                }
                
                if (h->is_split()) {
                    dealloc_node(node, split_top_size_u64(h->top_count));
                } else {
                    dealloc_node(node, leaf_compact_size_u64<BITS>(h->count));
                }
            } else {
                // Internal node - recurse
                if (h->is_split()) {
                    size_t top_count = h->top_count;
                    uint64_t* top_ch = top_children(node);
                    for (size_t t = 0; t < top_count; ++t) {
                        uint64_t* bot = reinterpret_cast<uint64_t*>(top_ch[t]);
                        size_t bot_count = std::popcount(bot_bitmap(bot));
                        for (size_t b = 0; b < bot_count; ++b) {
                            if constexpr (BITS > 12) {
                                clear_impl<BITS - 12>(reinterpret_cast<uint64_t*>(bot_data(bot)[b]));
                            }
                        }
                        dealloc_node(bot, split_bot_size_u64(bot_count));
                    }
                    dealloc_node(node, split_top_size_u64(top_count));
                } else {
                    uint64_t* children = internal_children(node, h->count);
                    for (uint32_t i = 0; i < h->count; ++i) {
                        if constexpr (BITS > 12) {
                            clear_impl<BITS - 12>(reinterpret_cast<uint64_t*>(children[i]));
                        }
                    }
                    dealloc_node(node, internal_compact_size_u64(h->count));
                }
            }
        }
    }
    
    // ==========================================================================
    // Member Data
    // ==========================================================================
    
    uint64_t* root_[root_size];
    size_t size_;
    [[no_unique_address]] ALLOC alloc_;
    
public:
    // ==========================================================================
    // Constructor / Destructor
    // ==========================================================================
    
    kntrie2() : size_(0), alloc_() {
        for (size_t i = 0; i < root_size; ++i) {
            root_[i] = alloc_empty_compact(true);  // Empty leaf nodes
        }
    }
    
    explicit kntrie2(const ALLOC& a) : size_(0), alloc_(a) {
        for (size_t i = 0; i < root_size; ++i) {
            root_[i] = alloc_empty_compact(true);
        }
    }
    
    ~kntrie2() {
        for (size_t i = 0; i < root_size; ++i) {
            clear_impl<START_BITS>(root_[i]);
        }
    }
    
    kntrie2(const kntrie2&) = delete;
    kntrie2& operator=(const kntrie2&) = delete;
    kntrie2(kntrie2&&) = delete;
    kntrie2& operator=(kntrie2&&) = delete;
    
    // ==========================================================================
    // Size / Capacity
    // ==========================================================================
    
    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }
    [[nodiscard]] size_type size() const noexcept { return size_; }
    
    // ==========================================================================
    // Find
    // ==========================================================================
    
    const VALUE* find_value(const KEY& key) const noexcept {
        uint64_t ik = key_to_internal(key);
        size_t root_idx = extract_root_index(ik);
        return find_impl<START_BITS>(root_[root_idx], get_header(root_[root_idx]), ik, 0, 0);
    }
    
    bool contains(const KEY& key) const noexcept {
        return find_value(key) != nullptr;
    }
    
    size_type count(const KEY& key) const noexcept {
        return contains(key) ? 1 : 0;
    }
    
    // ==========================================================================
    // Insert
    // ==========================================================================
    
    std::pair<bool, bool> insert(const KEY& key, const VALUE& value) {
        uint64_t ik = key_to_internal(key);
        size_t root_idx = extract_root_index(ik);
        
        stored_value_type sv = store_value(value);
        
        auto [new_root, inserted] = insert_impl<START_BITS>(
            root_[root_idx], get_header(root_[root_idx]), ik, sv, 0, 0);
        
        root_[root_idx] = new_root;
        
        if (inserted) {
            ++size_;
            return {true, true};
        } else {
            destroy_value(sv);
            return {true, false};
        }
    }
    
    // ==========================================================================
    // Clear
    // ==========================================================================
    
    void clear() noexcept {
        for (size_t i = 0; i < root_size; ++i) {
            clear_impl<START_BITS>(root_[i]);
            root_[i] = alloc_empty_compact(true);
        }
        size_ = 0;
    }
};

} // namespace kn

#endif // KNTRIE2_HPP
