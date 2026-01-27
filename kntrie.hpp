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
    
    // Pointer layout:
    // Bit 63: leaf tag
    // Bits 59-62: skip count (4 bits, 0-15)
    // Bits 0-58: address (59 bits)
    static constexpr uint64_t LEAF_TAG = 1ULL << 63;
    static constexpr uint64_t SKIP_SHIFT = 59;
    static constexpr uint64_t SKIP_MASK = 0xFULL << SKIP_SHIFT;
    static constexpr uint64_t ADDR_MASK = (1ULL << 59) - 1;
    
    // Value allocator type
    using value_alloc_type = typename std::allocator_traits<ALLOC>::template rebind_alloc<VALUE>;
    
    // Members
    uint64_t root_;
    size_t size_;
    [[no_unique_address]] ALLOC alloc_;
    
    // =========================================================================
    // Bit utilities
    // =========================================================================
    
    static constexpr bool high_bit_set(uint64_t v) noexcept {
        return static_cast<int64_t>(v) < 0;
    }
    
    // =========================================================================
    // Pointer tagging
    // =========================================================================
    
    static constexpr bool is_leaf(uint64_t ptr) noexcept {
        return high_bit_set(ptr);
    }
    
    static constexpr uint64_t get_skip(uint64_t ptr) noexcept {
        return (ptr >> SKIP_SHIFT) & 0xF;
    }
    
    static constexpr uint64_t* get_addr(uint64_t ptr) noexcept {
        return reinterpret_cast<uint64_t*>(ptr & ADDR_MASK);
    }
    
    static constexpr uint64_t make_ptr(uint64_t* addr, bool leaf, uint64_t skip) noexcept {
        uint64_t ptr = reinterpret_cast<uint64_t>(addr);
        if (leaf) ptr |= LEAF_TAG;
        ptr |= (skip << SKIP_SHIFT);
        return ptr;
    }
    
    static constexpr uint64_t make_leaf_ptr(uint64_t* addr) noexcept {
        return make_ptr(addr, true, 0);
    }
    
    static constexpr uint64_t make_internal_ptr(uint64_t* addr, uint64_t skip = 0) noexcept {
        return make_ptr(addr, false, skip);
    }
    
    // =========================================================================
    // Key Conversion
    // =========================================================================
    
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
    
    static constexpr uint8_t extract_index(uint64_t internal_key, size_t shift) noexcept {
        return static_cast<uint8_t>((internal_key >> shift) & 0x3F);
    }
    
    // Extract prefix bits from key for given skip and shift
    static constexpr uint64_t extract_prefix(uint64_t internal_key, size_t shift, size_t skip) noexcept {
        // shift is the starting position, skip is number of 6-bit chunks
        // We want bits [shift, shift - skip*6)
        size_t prefix_bits = skip * 6;
        size_t end_shift = shift - prefix_bits + 6; // +6 because shift points to current level
        uint64_t mask = (1ULL << prefix_bits) - 1;
        return (internal_key >> end_shift) & mask;
    }
    
    // =========================================================================
    // Internal Node Layout (with skip)
    // skip == 0: [bitmap][children...]
    // skip > 0:  [prefix][bitmap][children...]
    // =========================================================================
    
    static constexpr size_t bitmap_offset(uint64_t skip) noexcept {
        return skip > 0 ? 1 : 0;
    }
    
    static constexpr size_t children_offset(uint64_t skip) noexcept {
        return bitmap_offset(skip) + 1;
    }
    
    static uint64_t get_bitmap(const uint64_t* node, uint64_t skip) noexcept {
        return node[bitmap_offset(skip)];
    }
    
    static uint64_t get_prefix(const uint64_t* node) noexcept {
        return node[0]; // Only valid when skip > 0
    }
    
    static uint64_t* get_children(uint64_t* node, uint64_t skip) noexcept {
        return node + children_offset(skip);
    }
    
    static const uint64_t* get_children(const uint64_t* node, uint64_t skip) noexcept {
        return node + children_offset(skip);
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
    
    // Allocate internal node
    // skip == 0: [bitmap][children...] -> 1 + child_count
    // skip > 0:  [prefix][bitmap][children...] -> 2 + child_count
    uint64_t* alloc_internal(size_t child_count, uint64_t skip) {
        size_t node_size = children_offset(skip) + child_count;
        uint64_t* node = alloc_node(node_size);
        node[bitmap_offset(skip)] = 0; // empty bitmap
        return node;
    }
    
    void dealloc_internal(uint64_t* node, uint64_t skip) noexcept {
        uint64_t bitmap = get_bitmap(node, skip);
        size_t child_count = std::popcount(bitmap);
        size_t node_size = children_offset(skip) + child_count;
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
    
    // Calculate slot for index (assumes bit is set)
    static int calc_slot(uint64_t bitmap, uint8_t index) noexcept {
        uint64_t shifted = bitmap << (63 - index);
        return std::popcount(shifted);
    }
    
    // Get child pointer (returns 0 if not found)
    static uint64_t get_child(const uint64_t* node, uint64_t skip, uint8_t index) noexcept {
        uint64_t bitmap = get_bitmap(node, skip);
        uint64_t shifted = bitmap << (63 - index);
        if (!high_bit_set(shifted)) [[unlikely]] return 0;
        int slot = std::popcount(shifted);
        return get_children(node, skip)[slot - 1];
    }
    
    // Insert child into internal node, returns new node
    uint64_t* insert_child(uint64_t* node, uint64_t skip, uint8_t index, uint64_t child_ptr) {
        uint64_t bitmap = get_bitmap(node, skip);
        size_t old_count = std::popcount(bitmap);
        size_t new_count = old_count + 1;
        
        uint64_t new_bitmap = bitmap | (1ULL << index);
        int slot = calc_slot(new_bitmap, index) - 1;
        
        uint64_t* new_node = alloc_internal(new_count, skip);
        if (skip > 0) {
            new_node[0] = node[0]; // copy prefix
        }
        new_node[bitmap_offset(skip)] = new_bitmap;
        
        const uint64_t* old_children = get_children(node, skip);
        uint64_t* new_children = get_children(new_node, skip);
        
        for (int i = 0; i < slot; ++i) {
            new_children[i] = old_children[i];
        }
        new_children[slot] = child_ptr;
        for (size_t i = slot; i < old_count; ++i) {
            new_children[i + 1] = old_children[i];
        }
        
        dealloc_internal(node, skip);
        return new_node;
    }
    
    // Remove child from internal node, returns new node (or nullptr if empty)
    uint64_t* remove_child(uint64_t* node, uint64_t skip, uint8_t index) {
        uint64_t bitmap = get_bitmap(node, skip);
        size_t old_count = std::popcount(bitmap);
        
        if (old_count == 1) {
            dealloc_internal(node, skip);
            return nullptr;
        }
        
        size_t new_count = old_count - 1;
        int slot = calc_slot(bitmap, index) - 1;
        
        uint64_t new_bitmap = bitmap & ~(1ULL << index);
        
        uint64_t* new_node = alloc_internal(new_count, skip);
        if (skip > 0) {
            new_node[0] = node[0]; // copy prefix
        }
        new_node[bitmap_offset(skip)] = new_bitmap;
        
        const uint64_t* old_children = get_children(node, skip);
        uint64_t* new_children = get_children(new_node, skip);
        
        for (int i = 0; i < slot; ++i) {
            new_children[i] = old_children[i];
        }
        for (size_t i = slot + 1; i < old_count; ++i) {
            new_children[i - 1] = old_children[i];
        }
        
        dealloc_internal(node, skip);
        return new_node;
    }
    
    // Update child pointer in internal node
    void update_child(uint64_t* node, uint64_t skip, uint8_t index, uint64_t new_child_ptr) noexcept {
        uint64_t bitmap = get_bitmap(node, skip);
        int slot = calc_slot(bitmap, index) - 1;
        get_children(node, skip)[slot] = new_child_ptr;
    }
    
    // =========================================================================
    // Leaf Operations
    // =========================================================================
    
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
        return leaf + 1 + leaf_count(leaf);
    }
    
    static uint64_t* leaf_values(uint64_t* leaf) noexcept {
        return leaf + 1 + leaf_count(leaf);
    }
    
    // Linear search in leaf
    static size_t leaf_search(const uint64_t* leaf, uint64_t internal_key) noexcept {
        size_t count = leaf_count(leaf);
        const uint64_t* keys = leaf_keys(leaf);
        
        for (size_t i = 0; i < count; ++i) {
            if (keys[i] == internal_key) {
                return i;
            }
            if (keys[i] > internal_key) [[unlikely]] {
                return ~i;
            }
        }
        return ~count;
    }
    
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
        
        for (size_t i = 0; i < pos; ++i) {
            new_keys[i] = old_keys[i];
        }
        new_keys[pos] = internal_key;
        for (size_t i = pos; i < old_count; ++i) {
            new_keys[i + 1] = old_keys[i];
        }
        
        for (size_t i = 0; i < pos; ++i) {
            new_vals[i] = old_vals[i];
        }
        new_vals[pos] = stored_value;
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
            dealloc_leaf(leaf);
            return nullptr;
        }
        
        size_t new_count = old_count - 1;
        
        uint64_t* new_leaf = alloc_leaf(new_count);
        uint64_t* new_keys = leaf_keys(new_leaf);
        uint64_t* new_vals = leaf_values(new_leaf);
        
        const uint64_t* old_keys = leaf_keys(leaf);
        const uint64_t* old_vals = leaf_values(leaf);
        
        for (size_t i = 0; i < pos; ++i) {
            new_keys[i] = old_keys[i];
        }
        for (size_t i = pos + 1; i < old_count; ++i) {
            new_keys[i - 1] = old_keys[i];
        }
        
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
    
    uint64_t split_leaf(uint64_t* leaf, size_t shift) {
        size_t count = leaf_count(leaf);
        const uint64_t* keys = leaf_keys(leaf);
        const uint64_t* vals = leaf_values(leaf);
        
        // Group entries by their 6-bit index at current shift
        size_t bucket_counts[64] = {0};
        for (size_t i = 0; i < count; ++i) {
            uint8_t idx = extract_index(keys[i], shift);
            bucket_counts[idx]++;
        }
        
        size_t num_children = 0;
        for (size_t i = 0; i < 64; ++i) {
            if (bucket_counts[i] > 0) num_children++;
        }
        
        // Allocate internal node (no skip for split result)
        uint64_t* internal = alloc_internal(num_children, 0);
        uint64_t bitmap = 0;
        
        size_t child_slot = 0;
        uint64_t* children = get_children(internal, 0);
        
        for (size_t bucket = 0; bucket < 64; ++bucket) {
            if (bucket_counts[bucket] == 0) continue;
            
            bitmap |= (1ULL << bucket);
            
            uint64_t* child = alloc_leaf(bucket_counts[bucket]);
            uint64_t* child_keys = leaf_keys(child);
            uint64_t* child_vals = leaf_values(child);
            
            size_t dest = 0;
            for (size_t i = 0; i < count; ++i) {
                uint8_t idx = extract_index(keys[i], shift);
                if (idx == bucket) {
                    child_keys[dest] = keys[i];
                    child_vals[dest] = vals[i];
                    dest++;
                }
            }
            
            children[child_slot++] = make_leaf_ptr(child);
        }
        
        internal[0] = bitmap;
        dealloc_leaf(leaf);
        
        return make_internal_ptr(internal, 0);
    }
    
    // =========================================================================
    // Path Compression - Collapse check
    // =========================================================================
    
    // Check if internal node has exactly one child
    static bool has_single_child(const uint64_t* node, uint64_t skip) noexcept {
        uint64_t bitmap = get_bitmap(node, skip);
        return std::popcount(bitmap) == 1;
    }
    
    // Get the single child (assumes has_single_child is true)
    static uint64_t get_single_child(const uint64_t* node, uint64_t skip) noexcept {
        return get_children(node, skip)[0];
    }
    
    // Get index of single child
    static uint8_t get_single_child_index(const uint64_t* node, uint64_t skip) noexcept {
        uint64_t bitmap = get_bitmap(node, skip);
        return static_cast<uint8_t>(std::countr_zero(bitmap));
    }
    
    // Try to collapse a chain of single-child nodes
    // Returns new tagged pointer with updated skip
    uint64_t try_collapse(uint64_t ptr, size_t shift) {
        if (is_leaf(ptr)) return ptr;
        
        uint64_t current_skip = get_skip(ptr);
        uint64_t* node = get_addr(ptr);
        
        if (!has_single_child(node, current_skip)) return ptr;
        
        uint64_t child_ptr = get_single_child(node, current_skip);
        
        // Can't collapse if child is a leaf (different structure)
        if (is_leaf(child_ptr)) return ptr;
        
        uint64_t child_skip = get_skip(child_ptr);
        uint64_t* child_node = get_addr(child_ptr);
        
        // Calculate new skip
        uint64_t new_skip = current_skip + 1 + child_skip;
        
        // Can't exceed 15 (4 bits)
        if (new_skip > 15) return ptr;
        
        // Build new prefix
        uint64_t parent_prefix = (current_skip > 0) ? get_prefix(node) : 0;
        uint8_t my_index = get_single_child_index(node, current_skip);
        uint64_t child_prefix = (child_skip > 0) ? get_prefix(child_node) : 0;
        
        // New prefix = parent_prefix | my_index | child_prefix
        uint64_t new_prefix;
        if (current_skip > 0) {
            new_prefix = (parent_prefix << 6) | my_index;
        } else {
            new_prefix = my_index;
        }
        if (child_skip > 0) {
            new_prefix = (new_prefix << (child_skip * 6)) | child_prefix;
        }
        
        // Create new node with combined skip
        uint64_t child_bitmap = get_bitmap(child_node, child_skip);
        size_t num_children = std::popcount(child_bitmap);
        
        uint64_t* new_node = alloc_internal(num_children, new_skip);
        new_node[0] = new_prefix;
        new_node[bitmap_offset(new_skip)] = child_bitmap;
        
        const uint64_t* child_children = get_children(child_node, child_skip);
        uint64_t* new_children = get_children(new_node, new_skip);
        for (size_t i = 0; i < num_children; ++i) {
            new_children[i] = child_children[i];
        }
        
        // Free old nodes
        dealloc_internal(node, current_skip);
        dealloc_internal(child_node, child_skip);
        
        return make_internal_ptr(new_node, new_skip);
    }
    
    // =========================================================================
    // Merge check (for leaf merge)
    // =========================================================================
    
    bool can_merge(const uint64_t* internal, uint64_t skip) const noexcept {
        uint64_t bitmap = get_bitmap(internal, skip);
        size_t num_children = std::popcount(bitmap);
        const uint64_t* children = get_children(internal, skip);
        
        size_t total = 0;
        for (size_t i = 0; i < num_children; ++i) {
            uint64_t child_ptr = children[i];
            if (!is_leaf(child_ptr)) {
                return false;
            }
            uint64_t* child = get_addr(child_ptr);
            total += leaf_count(child);
            if (total > max_leaf_entries) {
                return false;
            }
        }
        return true;
    }
    
    uint64_t* merge_children(uint64_t* internal, uint64_t skip) {
        uint64_t bitmap = get_bitmap(internal, skip);
        size_t num_children = std::popcount(bitmap);
        const uint64_t* children_arr = get_children(internal, skip);
        
        size_t total = 0;
        for (size_t i = 0; i < num_children; ++i) {
            uint64_t* child = get_addr(children_arr[i]);
            total += leaf_count(child);
        }
        
        uint64_t* merged = alloc_leaf(total);
        uint64_t* merged_keys = leaf_keys(merged);
        uint64_t* merged_vals = leaf_values(merged);
        
        size_t dest = 0;
        for (uint8_t bucket = 0; bucket < 64; ++bucket) {
            if (!(bitmap & (1ULL << bucket))) continue;
            
            int slot = calc_slot(bitmap, bucket) - 1;
            uint64_t* child = get_addr(children_arr[slot]);
            size_t child_cnt = leaf_count(child);
            const uint64_t* child_keys = leaf_keys(child);
            const uint64_t* child_vals = leaf_values(child);
            
            for (size_t i = 0; i < child_cnt; ++i) {
                merged_keys[dest] = child_keys[i];
                merged_vals[dest] = child_vals[i];
                dest++;
            }
            
            dealloc_leaf(child);
        }
        
        dealloc_internal(internal, skip);
        return merged;
    }
    
    // =========================================================================
    // Recursive clear
    // =========================================================================
    
    void clear_node(uint64_t ptr) noexcept {
        if (ptr == 0) return;
        
        if (is_leaf(ptr)) {
            uint64_t* leaf = get_addr(ptr);
            size_t count = leaf_count(leaf);
            
            if constexpr (!value_inline) {
                uint64_t* vals = leaf_values(leaf);
                for (size_t i = 0; i < count; ++i) {
                    destroy_value(vals[i]);
                }
            }
            
            dealloc_leaf(leaf);
        } else {
            uint64_t skip = get_skip(ptr);
            uint64_t* internal = get_addr(ptr);
            uint64_t bitmap = get_bitmap(internal, skip);
            size_t num_children = std::popcount(bitmap);
            const uint64_t* children = get_children(internal, skip);
            
            for (size_t i = 0; i < num_children; ++i) {
                clear_node(children[i]);
            }
            
            dealloc_internal(internal, skip);
        }
    }
    
    // =========================================================================
    // Find (ITERATIVE - hot path)
    // =========================================================================
    
    std::pair<uint64_t*, size_t> find_in_trie(uint64_t internal_key) const noexcept {
        uint64_t ptr = root_;
        size_t shift = 64 - bits_per_level;
        
        // Hot loop: traverse internal nodes
        while (!high_bit_set(ptr)) [[likely]] {
            uint64_t skip = get_skip(ptr);
            uint64_t* node = get_addr(ptr);
            
            // Check prefix if skip > 0
            if (skip > 0) [[unlikely]] {
                uint64_t expected = extract_prefix(internal_key, shift, skip);
                uint64_t actual = get_prefix(node);
                if (expected != actual) [[unlikely]] {
                    return {nullptr, 0};
                }
                shift -= skip * 6;
            }
            
            uint64_t bitmap = get_bitmap(node, skip);
            uint8_t index = extract_index(internal_key, shift);
            uint64_t shifted = bitmap << (63 - index);
            
            if (!high_bit_set(shifted)) [[unlikely]] {
                return {nullptr, 0};
            }
            
            int slot = std::popcount(shifted) - 1;
            ptr = get_children(node, skip)[slot];
            shift -= 6;
        }
        
        // Leaf search
        uint64_t* leaf = get_addr(ptr);
        size_t count = leaf_count(leaf);
        const uint64_t* keys = leaf_keys(leaf);
        
        for (size_t i = 0; i < count; ++i) {
            if (keys[i] == internal_key) {
                return {leaf, i};
            }
            if (keys[i] > internal_key) [[unlikely]] {
                return {nullptr, 0};
            }
        }
        
        return {nullptr, 0};
    }
    
    // Find minimum entry in subtree
    std::pair<uint64_t*, size_t> find_min(uint64_t ptr) const noexcept {
        if (ptr == 0) return {nullptr, 0};
        
        while (!is_leaf(ptr)) {
            uint64_t skip = get_skip(ptr);
            uint64_t* node = get_addr(ptr);
            ptr = get_children(node, skip)[0]; // First child
        }
        
        return {get_addr(ptr), 0};
    }
    
    // Find maximum entry in subtree
    std::pair<uint64_t*, size_t> find_max(uint64_t ptr) const noexcept {
        if (ptr == 0) return {nullptr, 0};
        
        while (!is_leaf(ptr)) {
            uint64_t skip = get_skip(ptr);
            uint64_t* node = get_addr(ptr);
            uint64_t bitmap = get_bitmap(node, skip);
            size_t num_children = std::popcount(bitmap);
            ptr = get_children(node, skip)[num_children - 1]; // Last child
        }
        
        uint64_t* leaf = get_addr(ptr);
        return {leaf, leaf_count(leaf) - 1};
    }
    
    // Find next entry after given key
    std::pair<uint64_t*, size_t> find_next(uint64_t internal_key) const noexcept {
        if (root_ == 0) return {nullptr, 0};
        
        uint64_t* best_leaf = nullptr;
        size_t best_idx = 0;
        
        uint64_t ptr = root_;
        size_t shift = 64 - bits_per_level;
        
        while (!is_leaf(ptr)) {
            uint64_t skip = get_skip(ptr);
            uint64_t* node = get_addr(ptr);
            
            // Handle prefix
            if (skip > 0) {
                uint64_t expected = extract_prefix(internal_key, shift, skip);
                uint64_t actual = get_prefix(node);
                if (expected != actual) {
                    // Prefix mismatch - determine if we should go left or right
                    if (actual > expected) {
                        // All keys in this subtree are greater
                        auto [leaf, idx] = find_min(ptr);
                        return {leaf, idx};
                    }
                    // All keys in this subtree are smaller
                    return {best_leaf, best_idx};
                }
                shift -= skip * 6;
            }
            
            uint64_t bitmap = get_bitmap(node, skip);
            uint8_t target_index = extract_index(internal_key, shift);
            
            // Look for higher branches
            uint64_t higher_mask = bitmap & ~((2ULL << target_index) - 1);
            if (higher_mask != 0) {
                int higher_bit = std::countr_zero(higher_mask);
                int slot = calc_slot(bitmap, higher_bit) - 1;
                auto [leaf, idx] = find_min(get_children(node, skip)[slot]);
                if (leaf != nullptr) {
                    best_leaf = leaf;
                    best_idx = idx;
                }
            }
            
            uint64_t child = get_child(node, skip, target_index);
            if (child == 0) {
                return {best_leaf, best_idx};
            }
            
            ptr = child;
            shift -= 6;
        }
        
        uint64_t* leaf = get_addr(ptr);
        size_t idx = leaf_upper_bound(leaf, internal_key);
        if (idx < leaf_count(leaf)) {
            return {leaf, idx};
        }
        return {best_leaf, best_idx};
    }
    
    // Find previous entry before given key
    std::pair<uint64_t*, size_t> find_prev(uint64_t internal_key) const noexcept {
        if (root_ == 0) return {nullptr, 0};
        
        uint64_t* best_leaf = nullptr;
        size_t best_idx = 0;
        
        uint64_t ptr = root_;
        size_t shift = 64 - bits_per_level;
        
        while (!is_leaf(ptr)) {
            uint64_t skip = get_skip(ptr);
            uint64_t* node = get_addr(ptr);
            
            if (skip > 0) {
                uint64_t expected = extract_prefix(internal_key, shift, skip);
                uint64_t actual = get_prefix(node);
                if (expected != actual) {
                    if (actual < expected) {
                        auto [leaf, idx] = find_max(ptr);
                        return {leaf, idx};
                    }
                    return {best_leaf, best_idx};
                }
                shift -= skip * 6;
            }
            
            uint64_t bitmap = get_bitmap(node, skip);
            uint8_t target_index = extract_index(internal_key, shift);
            
            // Look for lower branches
            uint64_t lower_mask = bitmap & ((1ULL << target_index) - 1);
            if (lower_mask != 0) {
                int higher_bit = 63 - std::countl_zero(lower_mask);
                int slot = calc_slot(bitmap, higher_bit) - 1;
                auto [leaf, idx] = find_max(get_children(node, skip)[slot]);
                if (leaf != nullptr) {
                    best_leaf = leaf;
                    best_idx = idx;
                }
            }
            
            uint64_t child = get_child(node, skip, target_index);
            if (child == 0) {
                return {best_leaf, best_idx};
            }
            
            ptr = child;
            shift -= 6;
        }
        
        uint64_t* leaf = get_addr(ptr);
        const uint64_t* keys = leaf_keys(leaf);
        size_t count = leaf_count(leaf);
        
        for (size_t i = count; i-- > 0;) {
            if (keys[i] < internal_key) {
                return {leaf, i};
            }
        }
        return {best_leaf, best_idx};
    }
    
    // =========================================================================
    // Recursive Insert
    // =========================================================================
    
    struct InsertResult {
        uint64_t new_ptr;
        bool inserted;
        uint64_t old_stored_value;
    };
    
    InsertResult insert_recursive(uint64_t ptr, uint64_t internal_key, 
                                   uint64_t stored_value, size_t shift) {
        if (ptr == 0) {
            uint64_t* leaf = alloc_leaf(1);
            leaf_keys(leaf)[0] = internal_key;
            leaf_values(leaf)[0] = stored_value;
            return {make_leaf_ptr(leaf), true, 0};
        }
        
        if (is_leaf(ptr)) {
            uint64_t* leaf = get_addr(ptr);
            size_t search_result = leaf_search(leaf, internal_key);
            
            if (search_result < leaf_count(leaf)) {
                uint64_t* vals = leaf_values(leaf);
                uint64_t old_val = vals[search_result];
                vals[search_result] = stored_value;
                return {ptr, false, old_val};
            }
            
            size_t insert_pos = ~search_result;
            size_t count = leaf_count(leaf);
            
            if (count < max_leaf_entries) {
                uint64_t* new_leaf = leaf_insert(leaf, insert_pos, internal_key, stored_value);
                return {make_leaf_ptr(new_leaf), true, 0};
            }
            
            // Split
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
        uint64_t skip = get_skip(ptr);
        uint64_t* node = get_addr(ptr);
        
        // Handle prefix mismatch
        if (skip > 0) {
            uint64_t expected = extract_prefix(internal_key, shift, skip);
            uint64_t actual = get_prefix(node);
            
            if (expected != actual) {
                // Find divergence point
                size_t prefix_bits = skip * 6;
                uint64_t diff = expected ^ actual;
                size_t leading_zeros = std::countl_zero(diff);
                size_t common_bits = (leading_zeros > (64 - prefix_bits)) ? 
                                      prefix_bits : leading_zeros - (64 - prefix_bits);
                size_t common_levels = common_bits / 6;
                
                // Create new structure at divergence point
                uint8_t new_index = (expected >> (prefix_bits - (common_levels + 1) * 6)) & 0x3F;
                uint8_t old_index = (actual >> (prefix_bits - (common_levels + 1) * 6)) & 0x3F;
                
                // New internal node at divergence
                uint64_t* new_internal = alloc_internal(2, common_levels);
                
                if (common_levels > 0) {
                    uint64_t common_prefix = actual >> (prefix_bits - common_levels * 6);
                    new_internal[0] = common_prefix;
                }
                
                uint64_t bitmap = (1ULL << new_index) | (1ULL << old_index);
                new_internal[bitmap_offset(common_levels)] = bitmap;
                
                // Create new leaf for the new key
                uint64_t* new_leaf = alloc_leaf(1);
                leaf_keys(new_leaf)[0] = internal_key;
                leaf_values(new_leaf)[0] = stored_value;
                
                // Adjust old node's skip and prefix
                uint64_t remaining_skip = skip - common_levels - 1;
                uint64_t* adjusted_old;
                
                if (remaining_skip == 0) {
                    // Old node loses its prefix, becomes normal node
                    uint64_t old_bitmap = get_bitmap(node, skip);
                    size_t old_children_count = std::popcount(old_bitmap);
                    adjusted_old = alloc_internal(old_children_count, 0);
                    adjusted_old[0] = old_bitmap;
                    const uint64_t* old_children = get_children(node, skip);
                    uint64_t* new_children = get_children(adjusted_old, 0);
                    for (size_t i = 0; i < old_children_count; ++i) {
                        new_children[i] = old_children[i];
                    }
                    dealloc_internal(node, skip);
                } else {
                    // Old node keeps shorter prefix
                    uint64_t old_bitmap = get_bitmap(node, skip);
                    size_t old_children_count = std::popcount(old_bitmap);
                    adjusted_old = alloc_internal(old_children_count, remaining_skip);
                    uint64_t new_prefix = actual & ((1ULL << (remaining_skip * 6)) - 1);
                    adjusted_old[0] = new_prefix;
                    adjusted_old[bitmap_offset(remaining_skip)] = old_bitmap;
                    const uint64_t* old_children = get_children(node, skip);
                    uint64_t* new_children = get_children(adjusted_old, remaining_skip);
                    for (size_t i = 0; i < old_children_count; ++i) {
                        new_children[i] = old_children[i];
                    }
                    dealloc_internal(node, skip);
                }
                
                // Place children in correct order
                uint64_t* new_children = get_children(new_internal, common_levels);
                if (new_index < old_index) {
                    new_children[0] = make_leaf_ptr(new_leaf);
                    new_children[1] = make_internal_ptr(adjusted_old, remaining_skip);
                } else {
                    new_children[0] = make_internal_ptr(adjusted_old, remaining_skip);
                    new_children[1] = make_leaf_ptr(new_leaf);
                }
                
                return {make_internal_ptr(new_internal, common_levels), true, 0};
            }
            
            shift -= skip * 6;
        }
        
        uint8_t index = extract_index(internal_key, shift);
        uint64_t child = get_child(node, skip, index);
        
        InsertResult result = insert_recursive(child, internal_key, stored_value, 
                                                shift - bits_per_level);
        
        if (child == 0) {
            uint64_t* new_node = insert_child(node, skip, index, result.new_ptr);
            uint64_t new_ptr = make_internal_ptr(new_node, skip);
            new_ptr = try_collapse(new_ptr, shift + skip * 6);
            return {new_ptr, result.inserted, result.old_stored_value};
        } else if (result.new_ptr != child) {
            update_child(node, skip, index, result.new_ptr);
            uint64_t new_ptr = make_internal_ptr(node, skip);
            new_ptr = try_collapse(new_ptr, shift + skip * 6);
            return {new_ptr, result.inserted, result.old_stored_value};
        }
        
        return {ptr, result.inserted, result.old_stored_value};
    }
    
    // =========================================================================
    // Recursive Erase
    // =========================================================================
    
    struct EraseResult {
        uint64_t new_ptr;
        bool erased;
        bool check_merge;
    };
    
    EraseResult erase_recursive(uint64_t ptr, uint64_t internal_key, size_t shift) {
        if (ptr == 0) {
            return {0, false, false};
        }
        
        if (is_leaf(ptr)) {
            uint64_t* leaf = get_addr(ptr);
            size_t search_result = leaf_search(leaf, internal_key);
            
            if (search_result >= leaf_count(leaf)) {
                return {ptr, false, false};
            }
            
            destroy_value(leaf_values(leaf)[search_result]);
            
            uint64_t* new_leaf = leaf_remove(leaf, search_result);
            if (new_leaf == nullptr) {
                return {0, true, true};
            }
            return {make_leaf_ptr(new_leaf), true, true};
        }
        
        uint64_t skip = get_skip(ptr);
        uint64_t* node = get_addr(ptr);
        
        // Check prefix
        if (skip > 0) {
            uint64_t expected = extract_prefix(internal_key, shift, skip);
            uint64_t actual = get_prefix(node);
            if (expected != actual) {
                return {ptr, false, false};
            }
            shift -= skip * 6;
        }
        
        uint8_t index = extract_index(internal_key, shift);
        uint64_t child = get_child(node, skip, index);
        
        if (child == 0) {
            return {ptr, false, false};
        }
        
        EraseResult result = erase_recursive(child, internal_key, shift - bits_per_level);
        
        if (!result.erased) {
            return {ptr, false, false};
        }
        
        if (result.new_ptr == 0) {
            uint64_t* new_node = remove_child(node, skip, index);
            if (new_node == nullptr) {
                return {0, true, true};
            }
            
            if (can_merge(new_node, skip)) {
                uint64_t* merged = merge_children(new_node, skip);
                return {make_leaf_ptr(merged), true, true};
            }
            
            uint64_t new_ptr = make_internal_ptr(new_node, skip);
            new_ptr = try_collapse(new_ptr, shift + skip * 6);
            return {new_ptr, true, false};
        }
        
        if (result.new_ptr != child) {
            update_child(node, skip, index, result.new_ptr);
        }
        
        if (result.check_merge && can_merge(node, skip)) {
            uint64_t* merged = merge_children(node, skip);
            return {make_leaf_ptr(merged), true, true};
        }
        
        uint64_t new_ptr = make_internal_ptr(node, skip);
        new_ptr = try_collapse(new_ptr, shift + skip * 6);
        return {new_ptr, true, false};
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
            
            if (leaf != nullptr) {
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
        
        template<bool C = Const, typename = std::enable_if_t<!C>>
        std::pair<iterator_impl, bool> insert(const KEY& k, const VALUE& v) {
            return trie_->insert(k, v);
        }
        
        template<bool C = Const, typename = std::enable_if_t<!C>>
        iterator_impl erase() {
            if (!valid_) return *this;
            KEY k = key_;
            ++(*this);
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
    
    kntrie() : root_(0), size_(0), alloc_() {
        // Start with empty leaf
        uint64_t* leaf = alloc_leaf(0);
        root_ = make_leaf_ptr(leaf);
    }
    
    explicit kntrie(const ALLOC& alloc) : root_(0), size_(0), alloc_(alloc) {
        uint64_t* leaf = alloc_leaf(0);
        root_ = make_leaf_ptr(leaf);
    }
    
    ~kntrie() {
        clear();
    }
    
    kntrie(const kntrie& other) : root_(0), size_(0), alloc_(other.alloc_) {
        uint64_t* leaf = alloc_leaf(0);
        root_ = make_leaf_ptr(leaf);
        for (auto it = other.begin(); it != other.end(); ++it) {
            insert(it.key(), it.value());
        }
    }
    
    kntrie(kntrie&& other) noexcept 
        : root_(other.root_), size_(other.size_), alloc_(std::move(other.alloc_)) {
        uint64_t* leaf = other.alloc_.allocate(1);
        leaf[0] = 0;
        other.root_ = make_leaf_ptr(leaf);
        other.size_ = 0;
    }
    
    kntrie& operator=(const kntrie& other) {
        if (this != &other) {
            clear();
            for (auto it = other.begin(); it != other.end(); ++it) {
                insert(it.key(), it.value());
            }
        }
        return *this;
    }
    
    kntrie& operator=(kntrie&& other) noexcept {
        if (this != &other) {
            clear_node(root_);
            root_ = other.root_;
            size_ = other.size_;
            alloc_ = std::move(other.alloc_);
            
            uint64_t* leaf = other.alloc_.allocate(1);
            leaf[0] = 0;
            other.root_ = make_leaf_ptr(leaf);
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
        if (size_ == 0) return end();
        auto [leaf, idx] = find_min(root_);
        if (leaf == nullptr) return end();
        KEY k = internal_to_key(leaf_keys(leaf)[idx]);
        VALUE v = load_value(leaf_values(leaf)[idx]);
        return iterator(this, k, v, true);
    }
    
    const_iterator begin() const noexcept {
        if (size_ == 0) return end();
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
        
        auto [leaf, idx] = find_in_trie(internal_key);
        if (leaf != nullptr) {
            VALUE v = load_value(leaf_values(leaf)[idx]);
            return iterator(this, key, v, true);
        }
        
        if (size_ == 0) return end();
        
        uint64_t* best_leaf = nullptr;
        size_t best_idx = 0;
        
        uint64_t ptr = root_;
        size_t shift = 64 - bits_per_level;
        
        while (!is_leaf(ptr)) {
            uint64_t skip = get_skip(ptr);
            uint64_t* node = get_addr(ptr);
            
            if (skip > 0) {
                uint64_t expected = extract_prefix(internal_key, shift, skip);
                uint64_t actual = get_prefix(node);
                if (expected != actual) {
                    if (actual > expected) {
                        auto [lf, i] = find_min(ptr);
                        if (lf) {
                            KEY k = internal_to_key(leaf_keys(lf)[i]);
                            VALUE v = load_value(leaf_values(lf)[i]);
                            return iterator(this, k, v, true);
                        }
                    }
                    break;
                }
                shift -= skip * 6;
            }
            
            uint64_t bitmap = get_bitmap(node, skip);
            uint8_t target_index = extract_index(internal_key, shift);
            
            uint64_t higher_mask = bitmap & ~((1ULL << target_index) - 1) & ~(1ULL << target_index);
            if (higher_mask != 0) {
                int higher_bit = std::countr_zero(higher_mask);
                int slot = calc_slot(bitmap, higher_bit) - 1;
                auto [lf, i] = find_min(get_children(node, skip)[slot]);
                if (lf != nullptr) {
                    best_leaf = lf;
                    best_idx = i;
                }
            }
            
            uint64_t child = get_child(node, skip, target_index);
            if (child == 0) break;
            
            ptr = child;
            shift -= 6;
        }
        
        if (is_leaf(ptr)) {
            uint64_t* lf = get_addr(ptr);
            size_t i = leaf_lower_bound(lf, internal_key);
            if (i < leaf_count(lf)) {
                KEY k = internal_to_key(leaf_keys(lf)[i]);
                VALUE v = load_value(leaf_values(lf)[i]);
                return iterator(this, k, v, true);
            }
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
            destroy_value(stored_value);
            VALUE old_val = load_value(result.old_stored_value);
            return {iterator(this, key, old_val, true), false};
        }
    }
    
    template<typename... Args>
    std::pair<iterator, bool> emplace(Args&&... args) {
        auto pair = std::pair<KEY, VALUE>(std::forward<Args>(args)...);
        return insert(pair.first, pair.second);
    }
    
    iterator erase(iterator pos) {
        if (!pos.valid_) return pos;
        KEY k = pos.key_;
        ++pos;
        erase(k);
        return pos;
    }
    
    iterator erase(const_iterator pos) {
        if (!pos.valid_) return end();
        KEY k = pos.key_;
        erase(k);
        return upper_bound(k);
    }
    
    size_type erase(const KEY& key) {
        uint64_t internal_key = key_to_internal(key);
        EraseResult result = erase_recursive(root_, internal_key, 64 - bits_per_level);
        root_ = result.new_ptr;
        
        if (root_ == 0) {
            uint64_t* leaf = alloc_leaf(0);
            root_ = make_leaf_ptr(leaf);
        }
        
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
        size_ = 0;
        uint64_t* leaf = alloc_.allocate(1);
        leaf[0] = 0;
        root_ = make_leaf_ptr(leaf);
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

template<typename K, typename V, typename A>
void swap(kntrie<K, V, A>& lhs, kntrie<K, V, A>& rhs) noexcept {
    lhs.swap(rhs);
}

} // namespace kn

#endif // KNTRIE_HPP
