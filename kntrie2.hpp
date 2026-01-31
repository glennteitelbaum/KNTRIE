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
    static constexpr size_t max_leaf_bits = 60;
};

template<> struct key_traits<48> {
    using leaf_key_type = uint64_t;      // 48 bits
    using internal_key_type = uint16_t;
    static constexpr size_t max_leaf_bits = 48;
};

template<> struct key_traits<36> {
    using leaf_key_type = uint64_t;      // 36 bits
    using internal_key_type = uint16_t;
    static constexpr size_t max_leaf_bits = 36;
};

template<> struct key_traits<24> {
    using leaf_key_type = uint32_t;      // 24 bits
    using internal_key_type = uint16_t;
    static constexpr size_t max_leaf_bits = 24;
};

template<> struct key_traits<12> {
    using leaf_key_type = uint16_t;      // 12 bits
    using internal_key_type = uint16_t;
    static constexpr size_t max_leaf_bits = 12;
};

// Special for 32-bit key level 1 (6-bit, single bitmap)
template<> struct key_traits<30> {
    using leaf_key_type = uint32_t;      // 30 bits
    using internal_key_type = uint16_t;
    static constexpr size_t max_leaf_bits = 30;
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
    static constexpr size_t bits_after_root = key_bits - root_bits;
    
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
    
    // Offsets in uint64_t units
    static constexpr size_t BITMAP_OFFSET = HEADER_U64;  // Bitmap starts after header
    static constexpr size_t KEYS_OFFSET = HEADER_U64;    // Keys start after header (compact)
    
    // ==========================================================================
    // Node Header (16 bytes = 2 uint64_t)
    // ==========================================================================
    
    struct alignas(16) NodeHeader {
        uint64_t prefix;      // skip chunks packed (12 bits each)
        uint32_t count;       // total entries
        uint16_t top_count;   // buckets when split (0 = compact)
        uint8_t skip;         // number of 12-bit chunks to skip
        uint8_t flags;        // bit 0: is_leaf
        
        bool is_leaf() const noexcept { return flags & 1; }
        bool is_split() const noexcept { return top_count > 0; }
        void set_leaf(bool v) noexcept { flags = (flags & ~1) | (v ? 1 : 0); }
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
    
    static constexpr KEY internal_to_key(uint64_t internal) noexcept {
        if constexpr (is_signed_key) {
            constexpr uint64_t sign_bit = 1ULL << (key_bits - 1);
            internal ^= sign_bit;
        }
        return static_cast<KEY>(internal);
    }
    
    // ==========================================================================
    // Bit Extraction
    // ==========================================================================
    
    // Extract root index (top bits)
    static constexpr size_t extract_root_index(uint64_t internal_key) noexcept {
        return static_cast<size_t>(internal_key >> bits_after_root);
    }
    
    // Extract 12-bit chunk at given position (BITS = remaining bits after extraction)
    template<int BITS>
    static constexpr uint16_t extract_chunk(uint64_t internal_key) noexcept {
        return static_cast<uint16_t>((internal_key >> (BITS - 12)) & 0xFFF);
    }
    
    // Extract 6-bit index for bitmap (used in split nodes)
    static constexpr uint8_t extract_top6(uint16_t chunk) noexcept {
        return static_cast<uint8_t>(chunk >> 6);
    }
    
    static constexpr uint8_t extract_bot6(uint16_t chunk) noexcept {
        return static_cast<uint8_t>(chunk & 0x3F);
    }
    
    // Extract prefix (skip * 12 bits)
    static constexpr uint64_t extract_prefix(uint64_t internal_key, int bits_remaining, int skip) noexcept {
        int prefix_bits = skip * 12;
        uint64_t mask = (1ULL << prefix_bits) - 1;
        return (internal_key >> (bits_remaining - prefix_bits)) & mask;
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
    
    // Compact node size: header + keys (uint16_t) + data (uint64_t)
    static constexpr size_t compact_size_u64(size_t count) noexcept {
        size_t keys_bytes = count * sizeof(uint16_t);
        size_t keys_u64 = (keys_bytes + 7) / 8;
        return HEADER_U64 + keys_u64 + count;
    }
    
    // Split top node size: header + bitmap + child_ptrs
    static constexpr size_t split_top_size_u64(size_t child_count) noexcept {
        return HEADER_U64 + 1 + child_count;  // header + bitmap + children
    }
    
    // Split bottom node size: bitmap + data (no header)
    static constexpr size_t split_bot_size_u64(size_t entry_count) noexcept {
        return 1 + entry_count;  // bitmap + data
    }
    
    // ==========================================================================
    // Node Accessors
    // ==========================================================================
    
    // Compact node: [header][keys...][data...]
    static uint16_t* compact_keys(uint64_t* node) noexcept {
        return reinterpret_cast<uint16_t*>(node + KEYS_OFFSET);
    }
    static const uint16_t* compact_keys(const uint64_t* node) noexcept {
        return reinterpret_cast<const uint16_t*>(node + KEYS_OFFSET);
    }
    
    static uint64_t* compact_data(uint64_t* node, size_t count) noexcept {
        size_t keys_u64 = (count * sizeof(uint16_t) + 7) / 8;
        return node + HEADER_U64 + keys_u64;
    }
    static const uint64_t* compact_data(const uint64_t* node, size_t count) noexcept {
        size_t keys_u64 = (count * sizeof(uint16_t) + 7) / 8;
        return node + HEADER_U64 + keys_u64;
    }
    
    // Split top node: [header][bitmap][child_ptrs...]
    static uint64_t& top_bitmap(uint64_t* node) noexcept {
        return node[BITMAP_OFFSET];
    }
    static uint64_t top_bitmap(const uint64_t* node) noexcept {
        return node[BITMAP_OFFSET];
    }
    
    static uint64_t* top_children(uint64_t* node) noexcept {
        return node + BITMAP_OFFSET + 1;
    }
    static const uint64_t* top_children(const uint64_t* node) noexcept {
        return node + BITMAP_OFFSET + 1;
    }
    
    // Split bottom node: [bitmap][data...] (no header)
    static uint64_t& bot_bitmap(uint64_t* bot) noexcept {
        return bot[0];
    }
    static uint64_t bot_bitmap(const uint64_t* bot) noexcept {
        return bot[0];
    }
    
    static uint64_t* bot_data(uint64_t* bot) noexcept {
        return bot + 1;
    }
    static const uint64_t* bot_data(const uint64_t* bot) noexcept {
        return bot + 1;
    }
    
    // ==========================================================================
    // Bitmap Helpers
    // ==========================================================================
    
    // Check if bit is set and return slot index
    static int bitmap_slot(uint64_t bitmap, uint8_t index) noexcept {
        uint64_t before = bitmap << (63 - index);
        if (!(before & (1ULL << 63))) return -1;
        return std::popcount(before) - 1;
    }
    
    // ==========================================================================
    // Search Helpers
    // ==========================================================================
    
    // Linear search in compact internal node (keys sorted for early exit)
    static uint64_t* search_internal_compact(uint64_t* node, const NodeHeader* h, uint16_t chunk) noexcept {
        const uint16_t* keys = compact_keys(node);
        const uint64_t* data = compact_data(node, h->count);
        
        for (uint32_t i = 0; i < h->count; ++i) {
            if (keys[i] == chunk) {
                return reinterpret_cast<uint64_t*>(data[i]);
            }
            if (keys[i] > chunk) {
                return nullptr;  // Early exit - keys are sorted
            }
        }
        return nullptr;
    }
    
    // Linear search in compact leaf node, returns pointer to value
    static const uint64_t* search_leaf_compact(const uint64_t* node, const NodeHeader* h, uint16_t chunk) noexcept {
        const uint16_t* keys = compact_keys(node);
        const uint64_t* data = compact_data(node, h->count);
        
        for (uint32_t i = 0; i < h->count; ++i) {
            if (keys[i] == chunk) {
                return &data[i];
            }
            if (keys[i] > chunk) {
                return nullptr;
            }
        }
        return nullptr;
    }
    
    // Bitmap search in split internal node
    static uint64_t* search_internal_split(uint64_t* node, uint16_t chunk) noexcept {
        uint8_t top_idx = extract_top6(chunk);
        uint8_t bot_idx = extract_bot6(chunk);
        
        // Search top bitmap
        int top_slot = bitmap_slot(top_bitmap(node), top_idx);
        if (top_slot < 0) return nullptr;
        
        // Get bottom node
        uint64_t* bot = reinterpret_cast<uint64_t*>(top_children(node)[top_slot]);
        
        // Search bottom bitmap
        int bot_slot = bitmap_slot(bot_bitmap(bot), bot_idx);
        if (bot_slot < 0) return nullptr;
        
        return reinterpret_cast<uint64_t*>(bot_data(bot)[bot_slot]);
    }
    
    // Bitmap search in split leaf node
    static const uint64_t* search_leaf_split(const uint64_t* node, uint16_t chunk) noexcept {
        uint8_t top_idx = extract_top6(chunk);
        uint8_t bot_idx = extract_bot6(chunk);
        
        int top_slot = bitmap_slot(top_bitmap(node), top_idx);
        if (top_slot < 0) return nullptr;
        
        const uint64_t* bot = reinterpret_cast<const uint64_t*>(top_children(node)[top_slot]);
        
        int bot_slot = bitmap_slot(bot_bitmap(bot), bot_idx);
        if (bot_slot < 0) return nullptr;
        
        return &bot_data(bot)[bot_slot];
    }
    
    // ==========================================================================
    // Find Implementation - Template Recursion
    // ==========================================================================
    
    // Terminal case (should not be reached in normal operation)
    template<int BITS>
    const VALUE* find_impl(uint64_t* node, const NodeHeader* header, uint64_t key,
                           uint64_t skip_prefix, int skip_remaining) const noexcept {
        if constexpr (BITS <= 0) {
            return nullptr;
        } else {
            uint16_t chunk = extract_chunk<BITS>(key);
            
            // Handle skip prefix
            if (skip_remaining > 0) {
                uint16_t skip_chunk = static_cast<uint16_t>((skip_prefix >> (skip_remaining - 12)) & 0xFFF);
                if (chunk != skip_chunk) return nullptr;
                return find_impl<BITS - 12>(node, header, key, skip_prefix, skip_remaining - 12);
            }
            
            // Check if entering new skip region
            if (header->skip > 0) {
                uint16_t skip_chunk = static_cast<uint16_t>((header->prefix >> ((header->skip - 1) * 12)) & 0xFFF);
                if (chunk != skip_chunk) return nullptr;
                return find_impl<BITS - 12>(node, header, key, header->prefix, (header->skip - 1) * 12);
            }
            
            // At a decision point - search node
            if (header->is_leaf()) {
                const uint64_t* val_ptr;
                if (header->is_split()) {
                    val_ptr = search_leaf_split(node, chunk);
                } else {
                    val_ptr = search_leaf_compact(node, header, chunk);
                }
                if (!val_ptr) return nullptr;
                
                if constexpr (value_inline) {
                    return reinterpret_cast<const VALUE*>(val_ptr);
                } else {
                    return reinterpret_cast<const VALUE*>(*val_ptr);
                }
            } else {
                uint64_t* child;
                if (header->is_split()) {
                    child = search_internal_split(node, chunk);
                } else {
                    child = search_internal_compact(node, header, chunk);
                }
                
                if (!child) return nullptr;
                
                return find_impl<BITS - 12>(child, get_header(child), key, 0, 0);
            }
        }
    }
    
    // ==========================================================================
    // Insert Helpers
    // ==========================================================================
    
    // Find insertion position (sorted order)
    static size_t find_insert_pos(const uint16_t* keys, size_t count, uint16_t chunk) noexcept {
        size_t pos = 0;
        while (pos < count && keys[pos] < chunk) ++pos;
        return pos;
    }
    
    // Create empty compact node
    uint64_t* alloc_empty_compact(bool is_leaf) {
        uint64_t* node = alloc_node(compact_size_u64(0));
        NodeHeader* h = get_header(node);
        h->count = 0;
        h->top_count = 0;
        h->skip = 0;
        h->set_leaf(is_leaf);
        return node;
    }
    
    // Create single-entry compact leaf
    template<int BITS>
    uint64_t* create_single_leaf(uint64_t key, stored_value_type value, int skip) {
        uint64_t* node = alloc_node(compact_size_u64(1));
        NodeHeader* h = get_header(node);
        h->count = 1;
        h->top_count = 0;
        h->skip = static_cast<uint8_t>(skip);
        h->set_leaf(true);
        
        if (skip > 0) {
            h->prefix = extract_prefix(key, BITS + skip * 12, skip);
        }
        
        uint16_t chunk = extract_chunk<BITS>(key);
        compact_keys(node)[0] = chunk;
        
        uint64_t* data = compact_data(node, 1);
        if constexpr (value_inline) {
            std::memcpy(&data[0], &value, sizeof(stored_value_type));
        } else {
            data[0] = static_cast<uint64_t>(value);
        }
        
        return node;
    }
    
    // ==========================================================================
    // Insert Implementation
    // ==========================================================================
    
    struct InsertResult {
        uint64_t* new_node;
        bool inserted;
    };
    
    template<int BITS>
    InsertResult insert_impl(uint64_t* node, NodeHeader* header, uint64_t key,
                             stored_value_type value, uint64_t skip_prefix, int skip_remaining) {
        if constexpr (BITS <= 0) {
            return {node, false};
        } else {
            uint16_t chunk = extract_chunk<BITS>(key);
            
            // Handle skip prefix
            if (skip_remaining > 0) {
                uint16_t skip_chunk = static_cast<uint16_t>((skip_prefix >> (skip_remaining - 12)) & 0xFFF);
                if (chunk != skip_chunk) {
                    // Prefix mismatch - need to split
                    return split_on_prefix<BITS>(node, header, key, value, skip_prefix, skip_remaining, chunk, skip_chunk);
                }
                return insert_impl<BITS - 12>(node, header, key, value, skip_prefix, skip_remaining - 12);
            }
            
            // Check if entering new skip region
            if (header->skip > 0) {
                uint16_t skip_chunk = static_cast<uint16_t>((header->prefix >> ((header->skip - 1) * 12)) & 0xFFF);
                if (chunk != skip_chunk) {
                    // Prefix mismatch at node boundary
                    return split_on_prefix<BITS>(node, header, key, value, header->prefix, header->skip * 12, chunk, skip_chunk);
                }
                return insert_impl<BITS - 12>(node, header, key, value, header->prefix, (header->skip - 1) * 12);
            }
            
            // At a decision point - insert into node
            if (header->is_leaf()) {
                return insert_into_leaf<BITS>(node, header, key, value, chunk);
            } else {
                return insert_into_internal<BITS>(node, header, key, value, chunk);
            }
        }
    }
    
    template<int BITS>
    InsertResult insert_into_leaf(uint64_t* node, NodeHeader* header, uint64_t key,
                                   stored_value_type value, uint16_t chunk) {
        if (header->is_split()) {
            return insert_into_split_leaf<BITS>(node, header, key, value, chunk);
        }
        
        // Compact leaf
        uint16_t* keys = compact_keys(node);
        uint64_t* data = compact_data(node, header->count);
        
        // Check for existing key
        for (uint32_t i = 0; i < header->count; ++i) {
            if (keys[i] == chunk) {
                // Update existing
                if constexpr (!value_inline) {
                    destroy_value(static_cast<stored_value_type>(data[i]));
                }
                if constexpr (value_inline) {
                    std::memcpy(&data[i], &value, sizeof(stored_value_type));
                } else {
                    data[i] = static_cast<uint64_t>(value);
                }
                return {node, false};
            }
        }
        
        // Need to add new entry
        if (header->count >= COMPACT_MAX) {
            // Convert to split
            return convert_to_split_leaf<BITS>(node, header, key, value, chunk);
        }
        
        // Add to compact node
        size_t new_count = header->count + 1;
        uint64_t* new_node = alloc_node(compact_size_u64(new_count));
        NodeHeader* new_h = get_header(new_node);
        *new_h = *header;
        new_h->count = static_cast<uint32_t>(new_count);
        
        uint16_t* new_keys = compact_keys(new_node);
        uint64_t* new_data = compact_data(new_node, new_count);
        
        // Insert in sorted order
        size_t pos = find_insert_pos(keys, header->count, chunk);
        
        for (size_t i = 0; i < pos; ++i) {
            new_keys[i] = keys[i];
            new_data[i] = data[i];
        }
        new_keys[pos] = chunk;
        if constexpr (value_inline) {
            std::memcpy(&new_data[pos], &value, sizeof(stored_value_type));
        } else {
            new_data[pos] = static_cast<uint64_t>(value);
        }
        for (size_t i = pos; i < header->count; ++i) {
            new_keys[i + 1] = keys[i];
            new_data[i + 1] = data[i];
        }
        
        dealloc_node(node, compact_size_u64(header->count));
        return {new_node, true};
    }
    
    template<int BITS>
    InsertResult insert_into_split_leaf(uint64_t* node, NodeHeader* header, uint64_t key,
                                         stored_value_type value, uint16_t chunk) {
        uint8_t top_idx = extract_top6(chunk);
        uint8_t bot_idx = extract_bot6(chunk);
        
        uint64_t& top_bm = top_bitmap(node);
        uint64_t* children = top_children(node);
        
        int top_slot = bitmap_slot(top_bm, top_idx);
        
        if (top_slot < 0) {
            // Need to add new bottom node
            size_t top_count = std::popcount(top_bm);
            size_t new_top_count = top_count + 1;
            
            // Allocate new bottom with single entry
            uint64_t* new_bot = alloc_node(split_bot_size_u64(1));
            bot_bitmap(new_bot) = 1ULL << bot_idx;
            if constexpr (value_inline) {
                std::memcpy(&bot_data(new_bot)[0], &value, sizeof(stored_value_type));
            } else {
                bot_data(new_bot)[0] = static_cast<uint64_t>(value);
            }
            
            // Allocate new top node
            uint64_t* new_node = alloc_node(split_top_size_u64(new_top_count));
            NodeHeader* new_h = get_header(new_node);
            *new_h = *header;
            new_h->count++;
            new_h->top_count = static_cast<uint16_t>(new_top_count);
            
            // Insert in sorted order
            uint64_t new_top_bm = top_bm | (1ULL << top_idx);
            top_bitmap(new_node) = new_top_bm;
            
            int new_slot = bitmap_slot(new_top_bm, top_idx);
            uint64_t* new_children = top_children(new_node);
            
            for (int i = 0; i < new_slot; ++i) {
                new_children[i] = children[i];
            }
            new_children[new_slot] = reinterpret_cast<uint64_t>(new_bot);
            for (size_t i = new_slot; i < top_count; ++i) {
                new_children[i + 1] = children[i];
            }
            
            dealloc_node(node, split_top_size_u64(top_count));
            return {new_node, true};
        }
        
        // Bottom node exists
        uint64_t* bot = reinterpret_cast<uint64_t*>(children[top_slot]);
        uint64_t& bot_bm = bot_bitmap(bot);
        
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
        
        // Add to bottom node
        size_t bot_count = std::popcount(bot_bm);
        size_t new_bot_count = bot_count + 1;
        
        uint64_t* new_bot = alloc_node(split_bot_size_u64(new_bot_count));
        uint64_t new_bot_bm = bot_bm | (1ULL << bot_idx);
        bot_bitmap(new_bot) = new_bot_bm;
        
        int new_bot_slot = bitmap_slot(new_bot_bm, bot_idx);
        uint64_t* old_bot_data = bot_data(bot);
        uint64_t* new_bot_data = bot_data(new_bot);
        
        for (int i = 0; i < new_bot_slot; ++i) {
            new_bot_data[i] = old_bot_data[i];
        }
        if constexpr (value_inline) {
            std::memcpy(&new_bot_data[new_bot_slot], &value, sizeof(stored_value_type));
        } else {
            new_bot_data[new_bot_slot] = static_cast<uint64_t>(value);
        }
        for (size_t i = new_bot_slot; i < bot_count; ++i) {
            new_bot_data[i + 1] = old_bot_data[i];
        }
        
        children[top_slot] = reinterpret_cast<uint64_t>(new_bot);
        header->count++;
        
        dealloc_node(bot, split_bot_size_u64(bot_count));
        return {node, true};
    }
    
    template<int BITS>
    InsertResult insert_into_internal(uint64_t* node, NodeHeader* header, uint64_t key,
                                       stored_value_type value, uint16_t chunk) {
        if (header->is_split()) {
            return insert_into_split_internal<BITS>(node, header, key, value, chunk);
        }
        
        // Compact internal
        uint16_t* keys = compact_keys(node);
        uint64_t* data = compact_data(node, header->count);
        
        // Check for existing child
        for (uint32_t i = 0; i < header->count; ++i) {
            if (keys[i] == chunk) {
                // Recurse into child
                uint64_t* child = reinterpret_cast<uint64_t*>(data[i]);
                auto [new_child, inserted] = insert_impl<BITS - 12>(child, get_header(child), key, value, 0, 0);
                
                if (new_child != child) {
                    data[i] = reinterpret_cast<uint64_t>(new_child);
                }
                return {node, inserted};
            }
        }
        
        // Need to add new child
        if (header->count >= COMPACT_MAX) {
            // Convert to split
            return convert_to_split_internal<BITS>(node, header, key, value, chunk);
        }
        
        // Create child for remaining bits
        uint64_t* child = create_single_leaf<BITS - 12>(key, value, 0);
        
        // Add to compact node
        size_t new_count = header->count + 1;
        uint64_t* new_node = alloc_node(compact_size_u64(new_count));
        NodeHeader* new_h = get_header(new_node);
        *new_h = *header;
        new_h->count = static_cast<uint32_t>(new_count);
        
        uint16_t* new_keys = compact_keys(new_node);
        uint64_t* new_data = compact_data(new_node, new_count);
        
        size_t pos = find_insert_pos(keys, header->count, chunk);
        
        for (size_t i = 0; i < pos; ++i) {
            new_keys[i] = keys[i];
            new_data[i] = data[i];
        }
        new_keys[pos] = chunk;
        new_data[pos] = reinterpret_cast<uint64_t>(child);
        for (size_t i = pos; i < header->count; ++i) {
            new_keys[i + 1] = keys[i];
            new_data[i + 1] = data[i];
        }
        
        dealloc_node(node, compact_size_u64(header->count));
        return {new_node, true};
    }
    
    template<int BITS>
    InsertResult insert_into_split_internal(uint64_t* node, NodeHeader* header, uint64_t key,
                                             stored_value_type value, uint16_t chunk) {
        uint8_t top_idx = extract_top6(chunk);
        uint8_t bot_idx = extract_bot6(chunk);
        
        uint64_t& top_bm = top_bitmap(node);
        uint64_t* children = top_children(node);
        
        int top_slot = bitmap_slot(top_bm, top_idx);
        
        if (top_slot < 0) {
            // Create new child leaf and new bottom node
            uint64_t* child = create_single_leaf<BITS - 12>(key, value, 0);
            
            size_t top_count = std::popcount(top_bm);
            size_t new_top_count = top_count + 1;
            
            uint64_t* new_bot = alloc_node(split_bot_size_u64(1));
            bot_bitmap(new_bot) = 1ULL << bot_idx;
            bot_data(new_bot)[0] = reinterpret_cast<uint64_t>(child);
            
            uint64_t* new_node = alloc_node(split_top_size_u64(new_top_count));
            NodeHeader* new_h = get_header(new_node);
            *new_h = *header;
            new_h->count++;
            new_h->top_count = static_cast<uint16_t>(new_top_count);
            
            uint64_t new_top_bm = top_bm | (1ULL << top_idx);
            top_bitmap(new_node) = new_top_bm;
            
            int new_slot = bitmap_slot(new_top_bm, top_idx);
            uint64_t* new_children = top_children(new_node);
            
            for (int i = 0; i < new_slot; ++i) {
                new_children[i] = children[i];
            }
            new_children[new_slot] = reinterpret_cast<uint64_t>(new_bot);
            for (size_t i = new_slot; i < top_count; ++i) {
                new_children[i + 1] = children[i];
            }
            
            dealloc_node(node, split_top_size_u64(top_count));
            return {new_node, true};
        }
        
        // Bottom node exists
        uint64_t* bot = reinterpret_cast<uint64_t*>(children[top_slot]);
        uint64_t& bot_bm = bot_bitmap(bot);
        
        int bot_slot = bitmap_slot(bot_bm, bot_idx);
        
        if (bot_slot >= 0) {
            // Recurse into existing child
            uint64_t* child = reinterpret_cast<uint64_t*>(bot_data(bot)[bot_slot]);
            auto [new_child, inserted] = insert_impl<BITS - 12>(child, get_header(child), key, value, 0, 0);
            
            if (new_child != child) {
                bot_data(bot)[bot_slot] = reinterpret_cast<uint64_t>(new_child);
            }
            return {node, inserted};
        }
        
        // Add new child to bottom node
        uint64_t* child = create_single_leaf<BITS - 12>(key, value, 0);
        
        size_t bot_count = std::popcount(bot_bm);
        size_t new_bot_count = bot_count + 1;
        
        uint64_t* new_bot = alloc_node(split_bot_size_u64(new_bot_count));
        uint64_t new_bot_bm = bot_bm | (1ULL << bot_idx);
        bot_bitmap(new_bot) = new_bot_bm;
        
        int new_bot_slot = bitmap_slot(new_bot_bm, bot_idx);
        uint64_t* old_bot_data = bot_data(bot);
        uint64_t* new_bot_data = bot_data(new_bot);
        
        for (int i = 0; i < new_bot_slot; ++i) {
            new_bot_data[i] = old_bot_data[i];
        }
        new_bot_data[new_bot_slot] = reinterpret_cast<uint64_t>(child);
        for (size_t i = new_bot_slot; i < bot_count; ++i) {
            new_bot_data[i + 1] = old_bot_data[i];
        }
        
        children[top_slot] = reinterpret_cast<uint64_t>(new_bot);
        header->count++;
        
        dealloc_node(bot, split_bot_size_u64(bot_count));
        return {node, true};
    }
    
    template<int BITS>
    InsertResult convert_to_split_leaf(uint64_t* node, NodeHeader* header, uint64_t key,
                                        stored_value_type value, uint16_t new_chunk) {
        // Collect all entries
        uint16_t* keys = compact_keys(node);
        uint64_t* data = compact_data(node, header->count);
        
        // Build top bitmap
        uint64_t new_top_bm = 0;
        for (uint32_t i = 0; i < header->count; ++i) {
            new_top_bm |= (1ULL << extract_top6(keys[i]));
        }
        new_top_bm |= (1ULL << extract_top6(new_chunk));
        
        size_t top_count = std::popcount(new_top_bm);
        
        // Allocate new top node
        uint64_t* new_node = alloc_node(split_top_size_u64(top_count));
        NodeHeader* new_h = get_header(new_node);
        *new_h = *header;
        new_h->count = header->count + 1;
        new_h->top_count = static_cast<uint16_t>(top_count);
        top_bitmap(new_node) = new_top_bm;
        
        // Count entries per top bucket
        size_t bucket_counts[64] = {0};
        for (uint32_t i = 0; i < header->count; ++i) {
            bucket_counts[extract_top6(keys[i])]++;
        }
        bucket_counts[extract_top6(new_chunk)]++;
        
        // Create bottom nodes
        uint64_t* children = top_children(new_node);
        size_t child_idx = 0;
        
        for (uint8_t t = 0; t < 64; ++t) {
            if (!(new_top_bm & (1ULL << t))) continue;
            
            uint64_t bot_bm = 0;
            for (uint32_t i = 0; i < header->count; ++i) {
                if (extract_top6(keys[i]) == t) {
                    bot_bm |= (1ULL << extract_bot6(keys[i]));
                }
            }
            if (extract_top6(new_chunk) == t) {
                bot_bm |= (1ULL << extract_bot6(new_chunk));
            }
            
            size_t bot_count = std::popcount(bot_bm);
            uint64_t* bot = alloc_node(split_bot_size_u64(bot_count));
            bot_bitmap(bot) = bot_bm;
            
            // Fill bottom data
            for (uint32_t i = 0; i < header->count; ++i) {
                if (extract_top6(keys[i]) == t) {
                    int slot = bitmap_slot(bot_bm, extract_bot6(keys[i]));
                    bot_data(bot)[slot] = data[i];
                }
            }
            if (extract_top6(new_chunk) == t) {
                int slot = bitmap_slot(bot_bm, extract_bot6(new_chunk));
                if constexpr (value_inline) {
                    std::memcpy(&bot_data(bot)[slot], &value, sizeof(stored_value_type));
                } else {
                    bot_data(bot)[slot] = static_cast<uint64_t>(value);
                }
            }
            
            children[child_idx++] = reinterpret_cast<uint64_t>(bot);
        }
        
        dealloc_node(node, compact_size_u64(header->count));
        return {new_node, true};
    }
    
    template<int BITS>
    InsertResult convert_to_split_internal(uint64_t* node, NodeHeader* header, uint64_t key,
                                            stored_value_type value, uint16_t new_chunk) {
        // Similar to convert_to_split_leaf but creates child node for new entry
        uint16_t* keys = compact_keys(node);
        uint64_t* data = compact_data(node, header->count);
        
        uint64_t new_top_bm = 0;
        for (uint32_t i = 0; i < header->count; ++i) {
            new_top_bm |= (1ULL << extract_top6(keys[i]));
        }
        new_top_bm |= (1ULL << extract_top6(new_chunk));
        
        size_t top_count = std::popcount(new_top_bm);
        
        uint64_t* new_node = alloc_node(split_top_size_u64(top_count));
        NodeHeader* new_h = get_header(new_node);
        *new_h = *header;
        new_h->count = header->count + 1;
        new_h->top_count = static_cast<uint16_t>(top_count);
        top_bitmap(new_node) = new_top_bm;
        
        size_t bucket_counts[64] = {0};
        for (uint32_t i = 0; i < header->count; ++i) {
            bucket_counts[extract_top6(keys[i])]++;
        }
        bucket_counts[extract_top6(new_chunk)]++;
        
        // Create child for new entry
        uint64_t* new_child = create_single_leaf<BITS - 12>(key, value, 0);
        
        uint64_t* children = top_children(new_node);
        size_t child_idx = 0;
        
        for (uint8_t t = 0; t < 64; ++t) {
            if (!(new_top_bm & (1ULL << t))) continue;
            
            uint64_t bot_bm = 0;
            for (uint32_t i = 0; i < header->count; ++i) {
                if (extract_top6(keys[i]) == t) {
                    bot_bm |= (1ULL << extract_bot6(keys[i]));
                }
            }
            if (extract_top6(new_chunk) == t) {
                bot_bm |= (1ULL << extract_bot6(new_chunk));
            }
            
            size_t bot_count = std::popcount(bot_bm);
            uint64_t* bot = alloc_node(split_bot_size_u64(bot_count));
            bot_bitmap(bot) = bot_bm;
            
            for (uint32_t i = 0; i < header->count; ++i) {
                if (extract_top6(keys[i]) == t) {
                    int slot = bitmap_slot(bot_bm, extract_bot6(keys[i]));
                    bot_data(bot)[slot] = data[i];
                }
            }
            if (extract_top6(new_chunk) == t) {
                int slot = bitmap_slot(bot_bm, extract_bot6(new_chunk));
                bot_data(bot)[slot] = reinterpret_cast<uint64_t>(new_child);
            }
            
            children[child_idx++] = reinterpret_cast<uint64_t>(bot);
        }
        
        dealloc_node(node, compact_size_u64(header->count));
        return {new_node, true};
    }
    
    template<int BITS>
    InsertResult split_on_prefix(uint64_t* node, NodeHeader* header, uint64_t key,
                                  stored_value_type value, uint64_t prefix,
                                  int prefix_bits, uint16_t new_chunk, uint16_t old_chunk) {
        // Find divergence point
        int common_chunks = 0;
        int remaining = prefix_bits;
        
        while (remaining >= 12) {
            uint16_t p_chunk = static_cast<uint16_t>((prefix >> (remaining - 12)) & 0xFFF);
            uint16_t k_chunk = extract_chunk<BITS + prefix_bits - common_chunks * 12>(key);
            
            // Already checked and matched at higher levels
            if (remaining == prefix_bits) {
                // First chunk after common portion - this is where divergence is
                break;
            }
            common_chunks++;
            remaining -= 12;
        }
        
        // Create new internal node with 2 children
        uint64_t* new_internal = alloc_node(compact_size_u64(2));
        NodeHeader* nh = get_header(new_internal);
        nh->count = 2;
        nh->top_count = 0;
        nh->skip = static_cast<uint8_t>(common_chunks);
        nh->set_leaf(false);
        
        if (common_chunks > 0) {
            nh->prefix = prefix >> (prefix_bits - common_chunks * 12);
        }
        
        // Create new leaf for the new key
        uint64_t* new_leaf = create_single_leaf<BITS - 12>(key, value, 0);
        
        // Adjust old node's skip
        int old_remaining_skip = (prefix_bits / 12) - common_chunks - 1;
        if (old_remaining_skip > 0) {
            header->skip = static_cast<uint8_t>(old_remaining_skip);
            header->prefix = prefix & ((1ULL << (old_remaining_skip * 12)) - 1);
        } else {
            header->skip = 0;
            header->prefix = 0;
        }
        
        // Place children in sorted order
        uint16_t* keys = compact_keys(new_internal);
        uint64_t* data = compact_data(new_internal, 2);
        
        if (new_chunk < old_chunk) {
            keys[0] = new_chunk;
            keys[1] = old_chunk;
            data[0] = reinterpret_cast<uint64_t>(new_leaf);
            data[1] = reinterpret_cast<uint64_t>(node);
        } else {
            keys[0] = old_chunk;
            keys[1] = new_chunk;
            data[0] = reinterpret_cast<uint64_t>(node);
            data[1] = reinterpret_cast<uint64_t>(new_leaf);
        }
        
        return {new_internal, true};
    }
    
    // ==========================================================================
    // Clear Implementation
    // ==========================================================================
    
    template<int BITS>
    void clear_impl(uint64_t* node) noexcept {
        if (!node) return;
        
        NodeHeader* h = get_header(node);
        
        if (h->is_leaf()) {
            // Destroy values if needed
            if constexpr (!value_inline) {
                if (h->is_split()) {
                    uint64_t top_bm = top_bitmap(node);
                    const uint64_t* children = top_children(node);
                    size_t top_count = std::popcount(top_bm);
                    
                    for (size_t t = 0; t < top_count; ++t) {
                        uint64_t* bot = reinterpret_cast<uint64_t*>(children[t]);
                        size_t bot_count = std::popcount(bot_bitmap(bot));
                        const uint64_t* d = bot_data(bot);
                        for (size_t b = 0; b < bot_count; ++b) {
                            destroy_value(static_cast<stored_value_type>(d[b]));
                        }
                        dealloc_node(bot, split_bot_size_u64(bot_count));
                    }
                    dealloc_node(node, split_top_size_u64(top_count));
                } else {
                    const uint64_t* data = compact_data(node, h->count);
                    for (uint32_t i = 0; i < h->count; ++i) {
                        destroy_value(static_cast<stored_value_type>(data[i]));
                    }
                    dealloc_node(node, compact_size_u64(h->count));
                }
            } else {
                if (h->is_split()) {
                    uint64_t top_bm = top_bitmap(node);
                    const uint64_t* children = top_children(node);
                    size_t top_count = std::popcount(top_bm);
                    
                    for (size_t t = 0; t < top_count; ++t) {
                        uint64_t* bot = reinterpret_cast<uint64_t*>(children[t]);
                        dealloc_node(bot, split_bot_size_u64(std::popcount(bot_bitmap(bot))));
                    }
                    dealloc_node(node, split_top_size_u64(top_count));
                } else {
                    dealloc_node(node, compact_size_u64(h->count));
                }
            }
        } else {
            // Recurse on children
            if constexpr (BITS > 12) {
                if (h->is_split()) {
                    uint64_t top_bm = top_bitmap(node);
                    const uint64_t* children = top_children(node);
                    size_t top_count = std::popcount(top_bm);
                    
                    for (size_t t = 0; t < top_count; ++t) {
                        uint64_t* bot = reinterpret_cast<uint64_t*>(children[t]);
                        size_t bot_count = std::popcount(bot_bitmap(bot));
                        const uint64_t* d = bot_data(bot);
                        for (size_t b = 0; b < bot_count; ++b) {
                            clear_impl<BITS - 12>(reinterpret_cast<uint64_t*>(d[b]));
                        }
                        dealloc_node(bot, split_bot_size_u64(bot_count));
                    }
                    dealloc_node(node, split_top_size_u64(top_count));
                } else {
                    const uint64_t* data = compact_data(node, h->count);
                    for (uint32_t i = 0; i < h->count; ++i) {
                        clear_impl<BITS - 12>(reinterpret_cast<uint64_t*>(data[i]));
                    }
                    dealloc_node(node, compact_size_u64(h->count));
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
        uint64_t* node = root_[root_idx];
        return find_impl<START_BITS>(node, get_header(node), ik, 0, 0);
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
