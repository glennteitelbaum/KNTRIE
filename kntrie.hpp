#ifndef KNTRIE_HPP
#define KNTRIE_HPP

#include <cstdint>
#include <cstring>
#include <bit>
#include <memory>
#include <type_traits>
#include <utility>
#include <iterator>
#include <limits>

namespace kn {

template<typename KEY, typename VALUE, typename ALLOC = std::allocator<uint64_t>>
class kntrie {
    static_assert(std::is_integral_v<KEY>, "KEY must be an integral type");
    
public:
    // Type aliases
    using key_type = KEY;
    using mapped_type = VALUE;
    using value_type = std::pair<const KEY, VALUE>;
    using size_type = std::size_t;
    using difference_type = std::ptrdiff_t;
    using allocator_type = ALLOC;
    
private:
    // Type traits
    static constexpr bool is_signed_key = std::is_signed_v<KEY>;
    static constexpr size_t key_bits = sizeof(KEY) * 8;
    static constexpr size_t max_depth = (key_bits + 5) / 6;
    static constexpr size_t bits_per_level = 6;
    static constexpr size_t max_leaf_entries = 64;
    static constexpr bool value_inline = sizeof(VALUE) <= 8 && std::is_trivially_copyable_v<VALUE>;
    
    // Tag constants
    static constexpr uint64_t LEAF_TAG = 1ULL << 63;
    static constexpr uint64_t PTR_MASK = ~LEAF_TAG;
    
    // Value allocator type
    using value_alloc_type = typename std::allocator_traits<ALLOC>::template rebind_alloc<VALUE>;
    
    // Members
    uint64_t root_;       // Tagged pointer to root node (stored as uint64_t)
    size_t size_;
    [[no_unique_address]] ALLOC alloc_;
    
    // =========================================================================
    // Key Conversion
    // =========================================================================
    
    static constexpr uint64_t key_to_internal(KEY k) noexcept {
        // Convert to unsigned of same size
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
        
        // Flip sign bit for signed types to make sortable
        // This maps: INT_MIN -> 0, 0 -> 0x80..., INT_MAX -> 0xFF...
        if constexpr (is_signed_key) {
            constexpr uint64_t sign_bit = 1ULL << (key_bits - 1);
            result ^= sign_bit;
        }
        
        // Shift to high bits for MSB-first traversal
        // No byteswap needed - shifting preserves numeric order
        result <<= (64 - key_bits);
        
        return result;
    }
    
    static constexpr KEY internal_to_key(uint64_t internal) noexcept {
        // Shift back from high bits
        internal >>= (64 - key_bits);
        
        // Flip sign bit back for signed types
        if constexpr (is_signed_key) {
            constexpr uint64_t sign_bit = 1ULL << (key_bits - 1);
            internal ^= sign_bit;
        }
        
        // No byteswap needed - just cast back
        return static_cast<KEY>(internal);
    }
    
    // Extract 6-bit index at given shift position
    static constexpr uint8_t extract_index(uint64_t internal_key, size_t shift) noexcept {
        return static_cast<uint8_t>((internal_key >> shift) & 0x3F);
    }
    
    // =========================================================================
    // Pointer Tagging
    // =========================================================================
    
    static constexpr bool is_leaf(uint64_t tagged_ptr) noexcept {
        return (tagged_ptr & LEAF_TAG) != 0;
    }
    
    static constexpr uint64_t* untag_ptr(uint64_t tagged_ptr) noexcept {
        return reinterpret_cast<uint64_t*>(tagged_ptr & PTR_MASK);
    }
    
    static constexpr uint64_t tag_as_leaf(uint64_t* ptr) noexcept {
        return reinterpret_cast<uint64_t>(ptr) | LEAF_TAG;
    }
    
    static constexpr uint64_t tag_as_internal(uint64_t* ptr) noexcept {
        return reinterpret_cast<uint64_t>(ptr);
    }
    
    // =========================================================================
    // Node Allocation
    // =========================================================================
    
    uint64_t* alloc_node(size_t count) {
        return alloc_.allocate(count);
    }
    
    void dealloc_node(uint64_t* node, size_t count) noexcept {
        alloc_.deallocate(node, count);
    }
    
    // Allocate leaf: [count][keys...][values...]
    uint64_t* alloc_leaf(size_t entry_count) {
        // count + keys + values
        size_t node_size = 1 + entry_count + entry_count;
        uint64_t* leaf = alloc_node(node_size);
        leaf[0] = entry_count;
        return leaf;
    }
    
    void dealloc_leaf(uint64_t* leaf) noexcept {
        size_t count = leaf[0];
        size_t node_size = 1 + count + count;
        dealloc_node(leaf, node_size);
    }
    
    // Allocate internal node: [bitmap][children...]
    uint64_t* alloc_internal(size_t child_count) {
        size_t node_size = 1 + child_count;
        uint64_t* node = alloc_node(node_size);
        node[0] = 0; // empty bitmap
        return node;
    }
    
    void dealloc_internal(uint64_t* node) noexcept {
        uint64_t bitmap = node[0];
        size_t child_count = std::popcount(bitmap);
        size_t node_size = 1 + child_count;
        dealloc_node(node, node_size);
    }
    
    // =========================================================================
    // Value Storage
    // =========================================================================
    
    uint64_t store_value(const VALUE& val) {
        if constexpr (value_inline) {
            uint64_t stored = 0;
            std::memcpy(&stored, &val, sizeof(VALUE));
            return stored;
        } else {
            value_alloc_type va(alloc_);
            VALUE* ptr = std::allocator_traits<value_alloc_type>::allocate(va, 1);
            std::allocator_traits<value_alloc_type>::construct(va, ptr, val);
            return reinterpret_cast<uint64_t>(ptr);
        }
    }
    
    VALUE load_value(uint64_t stored) const noexcept {
        if constexpr (value_inline) {
            VALUE val;
            std::memcpy(&val, &stored, sizeof(VALUE));
            return val;
        } else {
            return *reinterpret_cast<VALUE*>(stored);
        }
    }
    
    VALUE& value_ref(uint64_t stored) const noexcept {
        if constexpr (value_inline) {
            // Can't return ref to inline, this shouldn't be called
            // This is only for pointer case
            static VALUE dummy{};
            return dummy;
        } else {
            return *reinterpret_cast<VALUE*>(stored);
        }
    }
    
    void destroy_value(uint64_t stored) noexcept {
        if constexpr (!value_inline) {
            value_alloc_type va(alloc_);
            VALUE* ptr = reinterpret_cast<VALUE*>(stored);
            std::allocator_traits<value_alloc_type>::destroy(va, ptr);
            std::allocator_traits<value_alloc_type>::deallocate(va, ptr, 1);
        }
    }
    
    // =========================================================================
    // Internal Node Operations
    // =========================================================================
    
    // Get child pointer (returns 0 if not found)
    static uint64_t get_child(const uint64_t* node, uint8_t index) noexcept {
        uint64_t bitmap = node[0];
        uint64_t below = bitmap << (63 - index);
        if (!(below & (1ULL << 63))) [[unlikely]] return 0;
        int slot = std::popcount(below);
        return node[slot];
    }
    
    // Calculate slot for index (assumes bit is set)
    static int calc_slot(uint64_t bitmap, uint8_t index) noexcept {
        uint64_t below = bitmap << (63 - index);
        return std::popcount(below);
    }
    
    // Insert child into internal node, returns new node
    uint64_t* insert_child(uint64_t* node, uint8_t index, uint64_t child_ptr) {
        uint64_t bitmap = node[0];
        size_t old_count = std::popcount(bitmap);
        size_t new_count = old_count + 1;
        
        // Allocate new node
        uint64_t* new_node = alloc_node(1 + new_count);
        new_node[0] = bitmap | (1ULL << index);
        
        int slot = calc_slot(new_node[0], index);
        
        // Copy children before slot
        for (int i = 1; i < slot; ++i) {
            new_node[i] = node[i];
        }
        
        // Insert new child
        new_node[slot] = child_ptr;
        
        // Copy children after slot
        for (size_t i = slot; i <= old_count; ++i) {
            new_node[i + 1] = node[i];
        }
        
        // Dealloc old node
        dealloc_node(node, 1 + old_count);
        
        return new_node;
    }
    
    // Remove child from internal node, returns new node (or nullptr if empty)
    uint64_t* remove_child(uint64_t* node, uint8_t index) {
        uint64_t bitmap = node[0];
        size_t old_count = std::popcount(bitmap);
        
        if (old_count == 1) {
            // Node becomes empty
            dealloc_node(node, 2);
            return nullptr;
        }
        
        size_t new_count = old_count - 1;
        int slot = calc_slot(bitmap, index);
        
        // Allocate new node
        uint64_t* new_node = alloc_node(1 + new_count);
        new_node[0] = bitmap & ~(1ULL << index);
        
        // Copy children before slot
        for (int i = 1; i < slot; ++i) {
            new_node[i] = node[i];
        }
        
        // Skip removed child, copy rest
        for (size_t i = slot + 1; i <= old_count; ++i) {
            new_node[i - 1] = node[i];
        }
        
        // Dealloc old node
        dealloc_node(node, 1 + old_count);
        
        return new_node;
    }
    
    // Update child pointer in internal node
    void update_child(uint64_t* node, uint8_t index, uint64_t new_child_ptr) noexcept {
        int slot = calc_slot(node[0], index);
        node[slot] = new_child_ptr;
    }
    
    // =========================================================================
    // Leaf Operations
    // =========================================================================
    
    // Leaf layout: [count][key0][key1]...[keyN-1][val0][val1]...[valN-1]
    static size_t leaf_count(const uint64_t* leaf) noexcept {
        return static_cast<size_t>(leaf[0]);
    }
    
    static const uint64_t* leaf_keys(const uint64_t* leaf) noexcept {
        return leaf + 1;
    }
    
    static uint64_t* leaf_keys(uint64_t* leaf) noexcept {
        return leaf + 1;
    }
    
    static const uint64_t* leaf_values(const uint64_t* leaf) noexcept {
        size_t count = leaf_count(leaf);
        return leaf + 1 + count;
    }
    
    static uint64_t* leaf_values(uint64_t* leaf) noexcept {
        size_t count = leaf_count(leaf);
        return leaf + 1 + count;
    }
    
    // Linear search in leaf, returns index if found, or ~insertion_point if not
    static size_t leaf_search(const uint64_t* leaf, uint64_t internal_key) noexcept {
        size_t count = leaf_count(leaf);
        const uint64_t* keys = leaf_keys(leaf);
        
        for (size_t i = 0; i < count; ++i) {
            if (keys[i] == internal_key) {
                return i;
            }
            if (keys[i] > internal_key) {
                return ~i;
            }
        }
        return ~count;
    }
    
    // Find first key >= internal_key, returns count if all are less
    static size_t leaf_lower_bound(const uint64_t* leaf, uint64_t internal_key) noexcept {
        size_t count = leaf_count(leaf);
        const uint64_t* keys = leaf_keys(leaf);
        
        for (size_t i = 0; i < count; ++i) {
            if (keys[i] >= internal_key) {
                return i;
            }
        }
        return count;
    }
    
    // Find first key > internal_key, returns count if all are <=
    static size_t leaf_upper_bound(const uint64_t* leaf, uint64_t internal_key) noexcept {
        size_t count = leaf_count(leaf);
        const uint64_t* keys = leaf_keys(leaf);
        
        for (size_t i = 0; i < count; ++i) {
            if (keys[i] > internal_key) {
                return i;
            }
        }
        return count;
    }
    
    // Insert into leaf (assumes count < 64 and key not present)
    uint64_t* leaf_insert(uint64_t* leaf, size_t pos, uint64_t internal_key, uint64_t stored_value) {
        size_t old_count = leaf_count(leaf);
        size_t new_count = old_count + 1;
        
        uint64_t* new_leaf = alloc_leaf(new_count);
        uint64_t* new_keys = leaf_keys(new_leaf);
        uint64_t* new_vals = leaf_values(new_leaf);
        
        const uint64_t* old_keys = leaf_keys(leaf);
        const uint64_t* old_vals = leaf_values(leaf);
        
        // Copy keys before pos
        for (size_t i = 0; i < pos; ++i) {
            new_keys[i] = old_keys[i];
        }
        // Insert new key
        new_keys[pos] = internal_key;
        // Copy keys after pos
        for (size_t i = pos; i < old_count; ++i) {
            new_keys[i + 1] = old_keys[i];
        }
        
        // Copy values before pos
        for (size_t i = 0; i < pos; ++i) {
            new_vals[i] = old_vals[i];
        }
        // Insert new value
        new_vals[pos] = stored_value;
        // Copy values after pos
        for (size_t i = pos; i < old_count; ++i) {
            new_vals[i + 1] = old_vals[i];
        }
        
        dealloc_leaf(leaf);
        return new_leaf;
    }
    
    // Remove from leaf
    uint64_t* leaf_remove(uint64_t* leaf, size_t pos) {
        size_t old_count = leaf_count(leaf);
        
        if (old_count == 1) {
            // Leaf becomes empty
            dealloc_leaf(leaf);
            return nullptr;
        }
        
        size_t new_count = old_count - 1;
        
        uint64_t* new_leaf = alloc_leaf(new_count);
        uint64_t* new_keys = leaf_keys(new_leaf);
        uint64_t* new_vals = leaf_values(new_leaf);
        
        const uint64_t* old_keys = leaf_keys(leaf);
        const uint64_t* old_vals = leaf_values(leaf);
        
        // Copy keys, skipping pos
        for (size_t i = 0; i < pos; ++i) {
            new_keys[i] = old_keys[i];
        }
        for (size_t i = pos + 1; i < old_count; ++i) {
            new_keys[i - 1] = old_keys[i];
        }
        
        // Copy values, skipping pos
        for (size_t i = 0; i < pos; ++i) {
            new_vals[i] = old_vals[i];
        }
        for (size_t i = pos + 1; i < old_count; ++i) {
            new_vals[i - 1] = old_vals[i];
        }
        
        dealloc_leaf(leaf);
        return new_leaf;
    }
    
    // =========================================================================
    // Leaf Split
    // =========================================================================
    
    // Split a full leaf into an internal node with child leaves
    // Returns tagged pointer to new internal node
    uint64_t split_leaf(uint64_t* leaf, size_t shift) {
        size_t count = leaf_count(leaf);
        const uint64_t* keys = leaf_keys(leaf);
        const uint64_t* vals = leaf_values(leaf);
        
        // Group entries by their 6-bit index at current shift
        // First pass: count entries per bucket
        size_t bucket_counts[64] = {0};
        for (size_t i = 0; i < count; ++i) {
            uint8_t idx = extract_index(keys[i], shift);
            bucket_counts[idx]++;
        }
        
        // Count non-empty buckets
        size_t num_children = 0;
        for (size_t i = 0; i < 64; ++i) {
            if (bucket_counts[i] > 0) num_children++;
        }
        
        // Allocate internal node
        uint64_t* internal = alloc_node(1 + num_children);
        uint64_t bitmap = 0;
        
        // Create child leaves
        size_t child_slot = 1;
        for (size_t bucket = 0; bucket < 64; ++bucket) {
            if (bucket_counts[bucket] == 0) continue;
            
            bitmap |= (1ULL << bucket);
            
            // Allocate child leaf
            uint64_t* child = alloc_leaf(bucket_counts[bucket]);
            uint64_t* child_keys = leaf_keys(child);
            uint64_t* child_vals = leaf_values(child);
            
            // Copy entries belonging to this bucket
            size_t dest = 0;
            for (size_t i = 0; i < count; ++i) {
                uint8_t idx = extract_index(keys[i], shift);
                if (idx == bucket) {
                    child_keys[dest] = keys[i];
                    child_vals[dest] = vals[i];
                    dest++;
                }
            }
            
            internal[child_slot++] = tag_as_leaf(child);
        }
        
        internal[0] = bitmap;
        
        // Free old leaf (entries moved, not destroyed)
        dealloc_leaf(leaf);
        
        return tag_as_internal(internal);
    }
    
    // =========================================================================
    // Leaf Merge Check
    // =========================================================================
    
    // Check if all children of internal node are leaves with total count <= 64
    bool can_merge(const uint64_t* internal) const noexcept {
        uint64_t bitmap = internal[0];
        size_t num_children = std::popcount(bitmap);
        
        size_t total = 0;
        for (size_t i = 1; i <= num_children; ++i) {
            uint64_t child_ptr = internal[i];
            if (!is_leaf(child_ptr)) {
                return false;
            }
            uint64_t* child = untag_ptr(child_ptr);
            total += leaf_count(child);
            if (total > max_leaf_entries) {
                return false;
            }
        }
        return true;
    }
    
    // Merge all children of internal node into single leaf
    uint64_t* merge_children(uint64_t* internal) {
        uint64_t bitmap = internal[0];
        size_t num_children = std::popcount(bitmap);
        
        // Count total entries
        size_t total = 0;
        for (size_t i = 1; i <= num_children; ++i) {
            uint64_t* child = untag_ptr(internal[i]);
            total += leaf_count(child);
        }
        
        // Allocate merged leaf
        uint64_t* merged = alloc_leaf(total);
        uint64_t* merged_keys = leaf_keys(merged);
        uint64_t* merged_vals = leaf_values(merged);
        
        // Copy from children in order (they're already sorted by bucket)
        size_t dest = 0;
        for (uint8_t bucket = 0; bucket < 64; ++bucket) {
            if (!(bitmap & (1ULL << bucket))) continue;
            
            int slot = calc_slot(bitmap, bucket);
            uint64_t* child = untag_ptr(internal[slot]);
            size_t child_cnt = leaf_count(child);
            const uint64_t* child_keys = leaf_keys(child);
            const uint64_t* child_vals = leaf_values(child);
            
            for (size_t i = 0; i < child_cnt; ++i) {
                merged_keys[dest] = child_keys[i];
                merged_vals[dest] = child_vals[i];
                dest++;
            }
            
            // Free child leaf
            dealloc_leaf(child);
        }
        
        // Free internal node
        dealloc_internal(internal);
        
        return merged;
    }
    
    // =========================================================================
    // Recursive clear
    // =========================================================================
    
    void clear_node(uint64_t tagged_ptr) noexcept {
        if (tagged_ptr == 0) return;
        
        if (is_leaf(tagged_ptr)) {
            uint64_t* leaf = untag_ptr(tagged_ptr);
            size_t count = leaf_count(leaf);
            
            // Destroy values if not inline
            if constexpr (!value_inline) {
                uint64_t* vals = leaf_values(leaf);
                for (size_t i = 0; i < count; ++i) {
                    destroy_value(vals[i]);
                }
            }
            
            dealloc_leaf(leaf);
        } else {
            uint64_t* internal = untag_ptr(tagged_ptr);
            uint64_t bitmap = internal[0];
            size_t num_children = std::popcount(bitmap);
            
            // Recursively clear children
            for (size_t i = 1; i <= num_children; ++i) {
                clear_node(internal[i]);
            }
            
            dealloc_internal(internal);
        }
    }
    
    // =========================================================================
    // Find helpers (iterative for read path)
    // =========================================================================
    
    // Returns {leaf_ptr, index} or {nullptr, 0} if not found
    std::pair<uint64_t*, size_t> find_in_trie(uint64_t internal_key) const noexcept {
        if (root_ == 0) return {nullptr, 0};
        
        uint64_t current = root_;
        size_t shift = 64 - bits_per_level;
        
        while (true) {
            if (is_leaf(current)) {
                uint64_t* leaf = untag_ptr(current);
                size_t idx = leaf_search(leaf, internal_key);
                if (idx < leaf_count(leaf)) { // Not inverted (found)
                    return {leaf, idx};
                }
                return {nullptr, 0};
            }
            
            uint64_t* node = untag_ptr(current);
            uint8_t index = extract_index(internal_key, shift);
            current = get_child(node, index);
            
            if (current == 0) return {nullptr, 0};
            
            shift -= bits_per_level;
        }
    }
    
    // Find minimum entry in subtree
    std::pair<uint64_t*, size_t> find_min(uint64_t tagged_ptr) const noexcept {
        if (tagged_ptr == 0) return {nullptr, 0};
        
        uint64_t current = tagged_ptr;
        
        while (!is_leaf(current)) {
            uint64_t* node = untag_ptr(current);
            uint64_t bitmap = node[0];
            // Find lowest set bit
            int lowest = std::countr_zero(bitmap);
            int slot = 1; // First child is always at slot 1
            current = node[slot];
        }
        
        uint64_t* leaf = untag_ptr(current);
        return {leaf, 0}; // First entry in leaf
    }
    
    // Find maximum entry in subtree
    std::pair<uint64_t*, size_t> find_max(uint64_t tagged_ptr) const noexcept {
        if (tagged_ptr == 0) return {nullptr, 0};
        
        uint64_t current = tagged_ptr;
        
        while (!is_leaf(current)) {
            uint64_t* node = untag_ptr(current);
            uint64_t bitmap = node[0];
            size_t num_children = std::popcount(bitmap);
            current = node[num_children]; // Last child
        }
        
        uint64_t* leaf = untag_ptr(current);
        size_t count = leaf_count(leaf);
        return {leaf, count - 1}; // Last entry in leaf
    }
    
    // Find next entry after given key (for iterator++)
    std::pair<uint64_t*, size_t> find_next(uint64_t internal_key) const noexcept {
        // Strategy: find upper bound
        if (root_ == 0) return {nullptr, 0};
        
        uint64_t* best_leaf = nullptr;
        size_t best_idx = 0;
        
        uint64_t current = root_;
        size_t shift = 64 - bits_per_level;
        
        while (true) {
            if (is_leaf(current)) {
                uint64_t* leaf = untag_ptr(current);
                size_t idx = leaf_upper_bound(leaf, internal_key);
                if (idx < leaf_count(leaf)) {
                    return {leaf, idx};
                }
                // Return best found so far (from higher branch)
                return {best_leaf, best_idx};
            }
            
            uint64_t* node = untag_ptr(current);
            uint64_t bitmap = node[0];
            uint8_t target_index = extract_index(internal_key, shift);
            
            // Look for higher branches we could take later
            uint64_t higher_mask = bitmap & ~((2ULL << target_index) - 1);
            if (higher_mask != 0) {
                // There's a higher branch - find its minimum
                int higher_bit = std::countr_zero(higher_mask);
                int slot = calc_slot(bitmap, higher_bit);
                auto [leaf, idx] = find_min(node[slot]);
                if (leaf != nullptr) {
                    best_leaf = leaf;
                    best_idx = idx;
                }
            }
            
            // Try to follow target branch
            uint64_t child = get_child(node, target_index);
            if (child == 0) {
                return {best_leaf, best_idx};
            }
            
            current = child;
            shift -= bits_per_level;
        }
    }
    
    // Find previous entry before given key (for iterator--)
    std::pair<uint64_t*, size_t> find_prev(uint64_t internal_key) const noexcept {
        if (root_ == 0) return {nullptr, 0};
        
        uint64_t* best_leaf = nullptr;
        size_t best_idx = 0;
        
        uint64_t current = root_;
        size_t shift = 64 - bits_per_level;
        
        while (true) {
            if (is_leaf(current)) {
                uint64_t* leaf = untag_ptr(current);
                size_t count = leaf_count(leaf);
                const uint64_t* keys = leaf_keys(leaf);
                
                // Find largest key < internal_key
                for (size_t i = count; i-- > 0;) {
                    if (keys[i] < internal_key) {
                        return {leaf, i};
                    }
                }
                // Return best found so far (from lower branch)
                return {best_leaf, best_idx};
            }
            
            uint64_t* node = untag_ptr(current);
            uint64_t bitmap = node[0];
            uint8_t target_index = extract_index(internal_key, shift);
            
            // Look for lower branches we could take later
            uint64_t lower_mask = bitmap & ((1ULL << target_index) - 1);
            if (lower_mask != 0) {
                // There's a lower branch - find its maximum
                int higher_bit = 63 - std::countl_zero(lower_mask);
                int slot = calc_slot(bitmap, higher_bit);
                auto [leaf, idx] = find_max(node[slot]);
                if (leaf != nullptr) {
                    best_leaf = leaf;
                    best_idx = idx;
                }
            }
            
            // Try to follow target branch
            uint64_t child = get_child(node, target_index);
            if (child == 0) {
                return {best_leaf, best_idx};
            }
            
            current = child;
            shift -= bits_per_level;
        }
    }
    
    // =========================================================================
    // Recursive Insert
    // =========================================================================
    
    // Returns {new_tagged_ptr, inserted, old_value_if_updated}
    struct InsertResult {
        uint64_t new_ptr;
        bool inserted;
        uint64_t old_stored_value; // Only valid if !inserted
    };
    
    InsertResult insert_recursive(uint64_t tagged_ptr, uint64_t internal_key, 
                                   uint64_t stored_value, size_t shift) {
        if (tagged_ptr == 0) {
            // Create new leaf with single entry
            uint64_t* leaf = alloc_leaf(1);
            leaf_keys(leaf)[0] = internal_key;
            leaf_values(leaf)[0] = stored_value;
            return {tag_as_leaf(leaf), true, 0};
        }
        
        if (is_leaf(tagged_ptr)) {
            uint64_t* leaf = untag_ptr(tagged_ptr);
            size_t search_result = leaf_search(leaf, internal_key);
            
            if (search_result < leaf_count(leaf)) {
                // Key exists, update value
                uint64_t* vals = leaf_values(leaf);
                uint64_t old_val = vals[search_result];
                vals[search_result] = stored_value;
                return {tagged_ptr, false, old_val};
            }
            
            // Key not found, insert
            size_t insert_pos = ~search_result;
            size_t count = leaf_count(leaf);
            
            if (count < max_leaf_entries) {
                // Room in leaf
                uint64_t* new_leaf = leaf_insert(leaf, insert_pos, internal_key, stored_value);
                return {tag_as_leaf(new_leaf), true, 0};
            }
            
            // Need to split
            // First insert into temporary oversized leaf, then split
            uint64_t* temp = alloc_leaf(count + 1);
            uint64_t* temp_keys = leaf_keys(temp);
            uint64_t* temp_vals = leaf_values(temp);
            const uint64_t* old_keys = leaf_keys(leaf);
            const uint64_t* old_vals = leaf_values(leaf);
            
            for (size_t i = 0; i < insert_pos; ++i) {
                temp_keys[i] = old_keys[i];
                temp_vals[i] = old_vals[i];
            }
            temp_keys[insert_pos] = internal_key;
            temp_vals[insert_pos] = stored_value;
            for (size_t i = insert_pos; i < count; ++i) {
                temp_keys[i + 1] = old_keys[i];
                temp_vals[i + 1] = old_vals[i];
            }
            
            dealloc_leaf(leaf);
            
            return {split_leaf(temp, shift), true, 0};
        }
        
        // Internal node
        uint64_t* node = untag_ptr(tagged_ptr);
        uint8_t index = extract_index(internal_key, shift);
        uint64_t child = get_child(node, index);
        
        InsertResult result = insert_recursive(child, internal_key, stored_value, 
                                                shift - bits_per_level);
        
        if (child == 0) {
            // New child created
            uint64_t* new_node = insert_child(node, index, result.new_ptr);
            return {tag_as_internal(new_node), result.inserted, result.old_stored_value};
        } else if (result.new_ptr != child) {
            // Child changed
            update_child(node, index, result.new_ptr);
        }
        
        return {tagged_ptr, result.inserted, result.old_stored_value};
    }
    
    // =========================================================================
    // Recursive Erase
    // =========================================================================
    
    struct EraseResult {
        uint64_t new_ptr;      // New tagged pointer for this subtree
        bool erased;           // Was something erased?
        bool check_merge;      // Should parent check for merge?
    };
    
    EraseResult erase_recursive(uint64_t tagged_ptr, uint64_t internal_key, size_t shift) {
        if (tagged_ptr == 0) {
            return {0, false, false};
        }
        
        if (is_leaf(tagged_ptr)) {
            uint64_t* leaf = untag_ptr(tagged_ptr);
            size_t search_result = leaf_search(leaf, internal_key);
            
            if (search_result >= leaf_count(leaf)) {
                // Not found (inverted result)
                return {tagged_ptr, false, false};
            }
            
            // Found - destroy value and remove
            destroy_value(leaf_values(leaf)[search_result]);
            
            uint64_t* new_leaf = leaf_remove(leaf, search_result);
            if (new_leaf == nullptr) {
                return {0, true, true};
            }
            return {tag_as_leaf(new_leaf), true, true};
        }
        
        // Internal node
        uint64_t* node = untag_ptr(tagged_ptr);
        uint8_t index = extract_index(internal_key, shift);
        uint64_t child = get_child(node, index);
        
        if (child == 0) {
            return {tagged_ptr, false, false};
        }
        
        EraseResult result = erase_recursive(child, internal_key, shift - bits_per_level);
        
        if (!result.erased) {
            return {tagged_ptr, false, false};
        }
        
        if (result.new_ptr == 0) {
            // Child was deleted
            uint64_t* new_node = remove_child(node, index);
            if (new_node == nullptr) {
                return {0, true, true};
            }
            
            // Check if we can merge
            if (can_merge(new_node)) {
                uint64_t* merged = merge_children(new_node);
                return {tag_as_leaf(merged), true, true};
            }
            
            return {tag_as_internal(new_node), true, false};
        }
        
        // Update child pointer
        if (result.new_ptr != child) {
            update_child(node, index, result.new_ptr);
        }
        
        // Check merge if child suggests it
        if (result.check_merge && can_merge(node)) {
            uint64_t* merged = merge_children(node);
            return {tag_as_leaf(merged), true, true};
        }
        
        return {tagged_ptr, true, false};
    }
    
public:
    // =========================================================================
    // Iterator
    // =========================================================================
    
    template<bool Const>
    class iterator_impl {
        friend class kntrie;
        
        using trie_type = std::conditional_t<Const, const kntrie, kntrie>;
        using trie_ptr = trie_type*;
        
        trie_ptr trie_;
        KEY key_;
        VALUE value_copy_;
        bool valid_;
        
        iterator_impl(trie_ptr trie, KEY key, const VALUE& value, bool valid)
            : trie_(trie), key_(key), value_copy_(value), valid_(valid) {}
        
        iterator_impl(trie_ptr trie, bool valid)
            : trie_(trie), key_{}, value_copy_{}, valid_(valid) {}
        
    public:
        using iterator_category = std::bidirectional_iterator_tag;
        using value_type = std::pair<const KEY, VALUE>;
        using difference_type = std::ptrdiff_t;
        using pointer = const value_type*;
        using reference = const value_type&;
        
        iterator_impl() : trie_(nullptr), key_{}, value_copy_{}, valid_(false) {}
        
        // Convert non-const to const
        template<bool C = Const, typename = std::enable_if_t<C>>
        iterator_impl(const iterator_impl<false>& other)
            : trie_(other.trie_), key_(other.key_), 
              value_copy_(other.value_copy_), valid_(other.valid_) {}
        
        std::pair<const KEY, const VALUE&> operator*() const {
            return {key_, value_copy_};
        }
        
        const KEY& key() const { return key_; }
        const VALUE& value() const { return value_copy_; }
        
        iterator_impl& operator++() {
            if (!valid_ || !trie_) {
                valid_ = false;
                return *this;
            }
            
            uint64_t internal_key = key_to_internal(key_);
            auto [leaf, idx] = trie_->find_next(internal_key);
            
            if (leaf == nullptr) {
                valid_ = false;
            } else {
                key_ = internal_to_key(leaf_keys(leaf)[idx]);
                value_copy_ = trie_->load_value(leaf_values(leaf)[idx]);
            }
            return *this;
        }
        
        iterator_impl operator++(int) {
            iterator_impl tmp = *this;
            ++(*this);
            return tmp;
        }
        
        iterator_impl& operator--() {
            if (!trie_) return *this;
            
            if (!valid_) {
                // end() -> last element
                auto [leaf, idx] = trie_->find_max(trie_->root_);
                if (leaf != nullptr) {
                    key_ = internal_to_key(leaf_keys(leaf)[idx]);
                    value_copy_ = trie_->load_value(leaf_values(leaf)[idx]);
                    valid_ = true;
                }
                return *this;
            }
            
            uint64_t internal_key = key_to_internal(key_);
            auto [leaf, idx] = trie_->find_prev(internal_key);
            
            if (leaf == nullptr) {
                // No previous element - undefined behavior for std::map
                // but we'll leave iterator at current position
            } else {
                key_ = internal_to_key(leaf_keys(leaf)[idx]);
                value_copy_ = trie_->load_value(leaf_values(leaf)[idx]);
            }
            return *this;
        }
        
        iterator_impl operator--(int) {
            iterator_impl tmp = *this;
            --(*this);
            return tmp;
        }
        
        bool operator==(const iterator_impl& other) const {
            if (!valid_ && !other.valid_) return true;
            if (valid_ != other.valid_) return false;
            return trie_ == other.trie_ && key_ == other.key_;
        }
        
        bool operator!=(const iterator_impl& other) const {
            return !(*this == other);
        }
        
        // Non-const iterator methods that delegate to parent
        template<bool C = Const, typename = std::enable_if_t<!C>>
        std::pair<iterator_impl, bool> insert(const KEY& k, const VALUE& v) {
            return trie_->insert(k, v);
        }
        
        template<bool C = Const, typename = std::enable_if_t<!C>>
        iterator_impl erase() {
            if (!valid_) return *this;
            KEY k = key_;
            ++(*this); // Move to next before erasing
            trie_->erase(k);
            return *this;
        }
    };
    
    using iterator = iterator_impl<false>;
    using const_iterator = iterator_impl<true>;
    using reverse_iterator = std::reverse_iterator<iterator>;
    using const_reverse_iterator = std::reverse_iterator<const_iterator>;
    
    // =========================================================================
    // Constructors / Destructor
    // =========================================================================
    
    kntrie() : root_(0), size_(0), alloc_() {}
    
    explicit kntrie(const ALLOC& alloc) : root_(0), size_(0), alloc_(alloc) {}
    
    ~kntrie() {
        clear();
    }
    
    // Copy constructor
    kntrie(const kntrie& other) : root_(0), size_(0), alloc_(other.alloc_) {
        for (auto it = other.begin(); it != other.end(); ++it) {
            insert(it.key(), it.value());
        }
    }
    
    // Move constructor
    kntrie(kntrie&& other) noexcept 
        : root_(other.root_), size_(other.size_), alloc_(std::move(other.alloc_)) {
        other.root_ = 0;
        other.size_ = 0;
    }
    
    // Copy assignment
    kntrie& operator=(const kntrie& other) {
        if (this != &other) {
            clear();
            for (auto it = other.begin(); it != other.end(); ++it) {
                insert(it.key(), it.value());
            }
        }
        return *this;
    }
    
    // Move assignment
    kntrie& operator=(kntrie&& other) noexcept {
        if (this != &other) {
            clear();
            root_ = other.root_;
            size_ = other.size_;
            alloc_ = std::move(other.alloc_);
            other.root_ = 0;
            other.size_ = 0;
        }
        return *this;
    }
    
    // =========================================================================
    // Capacity
    // =========================================================================
    
    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }
    [[nodiscard]] size_type size() const noexcept { return size_; }
    [[nodiscard]] size_type max_size() const noexcept { 
        return std::numeric_limits<size_type>::max(); 
    }
    
    // =========================================================================
    // Iterators
    // =========================================================================
    
    iterator begin() noexcept {
        if (root_ == 0) return end();
        auto [leaf, idx] = find_min(root_);
        if (leaf == nullptr) return end();
        KEY k = internal_to_key(leaf_keys(leaf)[idx]);
        VALUE v = load_value(leaf_values(leaf)[idx]);
        return iterator(this, k, v, true);
    }
    
    const_iterator begin() const noexcept {
        if (root_ == 0) return end();
        auto [leaf, idx] = find_min(root_);
        if (leaf == nullptr) return end();
        KEY k = internal_to_key(leaf_keys(leaf)[idx]);
        VALUE v = load_value(leaf_values(leaf)[idx]);
        return const_iterator(this, k, v, true);
    }
    
    iterator end() noexcept {
        return iterator(this, false);
    }
    
    const_iterator end() const noexcept {
        return const_iterator(this, false);
    }
    
    const_iterator cbegin() const noexcept { return begin(); }
    const_iterator cend() const noexcept { return end(); }
    
    reverse_iterator rbegin() noexcept { return reverse_iterator(end()); }
    reverse_iterator rend() noexcept { return reverse_iterator(begin()); }
    const_reverse_iterator rbegin() const noexcept { return const_reverse_iterator(end()); }
    const_reverse_iterator rend() const noexcept { return const_reverse_iterator(begin()); }
    const_reverse_iterator crbegin() const noexcept { return const_reverse_iterator(cend()); }
    const_reverse_iterator crend() const noexcept { return const_reverse_iterator(cbegin()); }
    
    // =========================================================================
    // Lookup
    // =========================================================================
    
    iterator find(const KEY& key) {
        uint64_t internal_key = key_to_internal(key);
        auto [leaf, idx] = find_in_trie(internal_key);
        if (leaf == nullptr) return end();
        VALUE v = load_value(leaf_values(leaf)[idx]);
        return iterator(this, key, v, true);
    }
    
    const_iterator find(const KEY& key) const {
        uint64_t internal_key = key_to_internal(key);
        auto [leaf, idx] = find_in_trie(internal_key);
        if (leaf == nullptr) return end();
        VALUE v = load_value(leaf_values(leaf)[idx]);
        return const_iterator(this, key, v, true);
    }
    
    bool contains(const KEY& key) const {
        uint64_t internal_key = key_to_internal(key);
        auto [leaf, idx] = find_in_trie(internal_key);
        return leaf != nullptr;
    }
    
    size_type count(const KEY& key) const {
        return contains(key) ? 1 : 0;
    }
    
    iterator lower_bound(const KEY& key) {
        uint64_t internal_key = key_to_internal(key);
        
        // First check exact match
        auto [leaf, idx] = find_in_trie(internal_key);
        if (leaf != nullptr) {
            VALUE v = load_value(leaf_values(leaf)[idx]);
            return iterator(this, key, v, true);
        }
        
        // Find first >= key
        // This is like find_next but for >= instead of >
        if (root_ == 0) return end();
        
        uint64_t* best_leaf = nullptr;
        size_t best_idx = 0;
        
        uint64_t current = root_;
        size_t shift = 64 - bits_per_level;
        
        while (true) {
            if (is_leaf(current)) {
                uint64_t* lf = untag_ptr(current);
                size_t i = leaf_lower_bound(lf, internal_key);
                if (i < leaf_count(lf)) {
                    KEY k = internal_to_key(leaf_keys(lf)[i]);
                    VALUE v = load_value(leaf_values(lf)[i]);
                    return iterator(this, k, v, true);
                }
                break;
            }
            
            uint64_t* node = untag_ptr(current);
            uint64_t bitmap = node[0];
            uint8_t target_index = extract_index(internal_key, shift);
            
            // Look for higher branches
            uint64_t higher_mask = bitmap & ~((1ULL << target_index) - 1) & ~(1ULL << target_index);
            if (higher_mask != 0) {
                int higher_bit = std::countr_zero(higher_mask);
                int slot = calc_slot(bitmap, higher_bit);
                auto [lf, i] = find_min(node[slot]);
                if (lf != nullptr) {
                    best_leaf = lf;
                    best_idx = i;
                }
            }
            
            uint64_t child = get_child(node, target_index);
            if (child == 0) break;
            
            current = child;
            shift -= bits_per_level;
        }
        
        if (best_leaf != nullptr) {
            KEY k = internal_to_key(leaf_keys(best_leaf)[best_idx]);
            VALUE v = load_value(leaf_values(best_leaf)[best_idx]);
            return iterator(this, k, v, true);
        }
        
        return end();
    }
    
    const_iterator lower_bound(const KEY& key) const {
        return const_cast<kntrie*>(this)->lower_bound(key);
    }
    
    iterator upper_bound(const KEY& key) {
        uint64_t internal_key = key_to_internal(key);
        auto [leaf, idx] = find_next(internal_key);
        if (leaf == nullptr) return end();
        KEY k = internal_to_key(leaf_keys(leaf)[idx]);
        VALUE v = load_value(leaf_values(leaf)[idx]);
        return iterator(this, k, v, true);
    }
    
    const_iterator upper_bound(const KEY& key) const {
        return const_cast<kntrie*>(this)->upper_bound(key);
    }
    
    std::pair<iterator, iterator> equal_range(const KEY& key) {
        return {lower_bound(key), upper_bound(key)};
    }
    
    std::pair<const_iterator, const_iterator> equal_range(const KEY& key) const {
        return {lower_bound(key), upper_bound(key)};
    }
    
    // =========================================================================
    // Modifiers
    // =========================================================================
    
    std::pair<iterator, bool> insert(const std::pair<KEY, VALUE>& value) {
        return insert(value.first, value.second);
    }
    
    std::pair<iterator, bool> insert(const KEY& key, const VALUE& value) {
        uint64_t internal_key = key_to_internal(key);
        uint64_t stored_value = store_value(value);
        
        InsertResult result = insert_recursive(root_, internal_key, stored_value, 
                                                64 - bits_per_level);
        root_ = result.new_ptr;
        
        if (result.inserted) {
            ++size_;
            return {iterator(this, key, value, true), true};
        } else {
            // Key existed, value was updated, destroy our new stored value
            // and return the old value
            destroy_value(stored_value);
            VALUE old_val = load_value(result.old_stored_value);
            return {iterator(this, key, old_val, true), false};
        }
    }
    
    template<typename... Args>
    std::pair<iterator, bool> emplace(Args&&... args) {
        // For now, just construct a pair and insert
        // A more optimal implementation would construct in-place
        auto pair = std::pair<KEY, VALUE>(std::forward<Args>(args)...);
        return insert(pair.first, pair.second);
    }
    
    iterator erase(iterator pos) {
        if (!pos.valid_) return pos;
        KEY k = pos.key_;
        ++pos; // Move to next before erasing
        erase(k);
        return pos;
    }
    
    iterator erase(const_iterator pos) {
        if (!pos.valid_) return end();
        KEY k = pos.key_;
        erase(k);
        // Find next element
        return upper_bound(k);
    }
    
    size_type erase(const KEY& key) {
        uint64_t internal_key = key_to_internal(key);
        EraseResult result = erase_recursive(root_, internal_key, 64 - bits_per_level);
        root_ = result.new_ptr;
        
        if (result.erased) {
            --size_;
            return 1;
        }
        return 0;
    }
    
    iterator erase(iterator first, iterator last) {
        while (first != last) {
            first = erase(first);
        }
        return first;
    }
    
    void clear() noexcept {
        clear_node(root_);
        root_ = 0;
        size_ = 0;
    }
    
    void swap(kntrie& other) noexcept {
        std::swap(root_, other.root_);
        std::swap(size_, other.size_);
        std::swap(alloc_, other.alloc_);
    }
    
    // =========================================================================
    // Observers
    // =========================================================================
    
    allocator_type get_allocator() const noexcept {
        return alloc_;
    }
};

// Non-member swap
template<typename K, typename V, typename A>
void swap(kntrie<K, V, A>& lhs, kntrie<K, V, A>& rhs) noexcept {
    lhs.swap(rhs);
}

} // namespace kn

#endif // KNTRIE_HPP
