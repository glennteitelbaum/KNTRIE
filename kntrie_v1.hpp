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
    static constexpr bool is_signed_key = std::is_signed_v<KEY>;
    static constexpr size_t key_bits = sizeof(KEY) * 8;
    static constexpr size_t bits_per_level = 6;
    static constexpr bool value_inline = sizeof(VALUE) <= 8 && std::is_trivially_copyable_v<VALUE>;
    
    // Root array: indexed by top (key_bits % 6) bits, or 6 bits if key_bits % 6 == 0
    static constexpr size_t root_bits = (key_bits % 6 == 0) ? 6 : (key_bits % 6);
    static constexpr size_t root_size = size_t{1} << root_bits;
    static constexpr size_t bits_after_root = key_bits - root_bits;
    
    using stored_value_type = std::conditional_t<value_inline, VALUE, uint64_t>;
    using value_alloc_type = typename std::allocator_traits<ALLOC>::template rebind_alloc<VALUE>;
    
    // =========================================================================
    // Pointer Tagging (1 bit)
    // =========================================================================
    //
    // Bit 63: LIST (0 = BITMASK, 1 = LIST)
    // Bits 62-0: ADDRESS
    //
    // Pointer tagging:
    //   Bit 63: LEAF (1 = data is values, 0 = data is child pointers)
    //   Bit 62: LIST (1 = LIST node, 0 = BITMASK node)
    //
    // With fixed-length keys, at any node level ALL entries are either:
    //   - Terminal (values) if no key bits remain after this node
    //   - Non-terminal (children) if key bits remain
    // So LEAF is determined by position in trie, not per-entry.
    //
    // Types:
    //   BITMASK: 64-way bitmap index (6 bits)
    //   LIST2: 32 slots, 12-bit keys
    //   LIST4: 16 slots, 18/24/30-bit keys
    //   LIST8: 8 slots, 36/42/48/54/60-bit keys
    // =========================================================================
    
    static constexpr uint64_t LEAF_BIT = 1ULL << 63;
    static constexpr uint64_t LIST_BIT = 1ULL << 62;
    static constexpr uint64_t ADDR_MASK = (1ULL << 62) - 1;
    
    static constexpr bool is_leaf(uint64_t ptr) noexcept { return ptr & LEAF_BIT; }
    static constexpr bool is_list(uint64_t ptr) noexcept { return ptr & LIST_BIT; }
    static constexpr uint64_t* get_addr(uint64_t ptr) noexcept {
        return reinterpret_cast<uint64_t*>(ptr & ADDR_MASK);
    }
    
    static constexpr uint64_t make_bitmask_leaf(uint64_t* addr) noexcept {
        return reinterpret_cast<uint64_t>(addr) | LEAF_BIT;
    }
    static constexpr uint64_t make_bitmask_internal(uint64_t* addr) noexcept {
        return reinterpret_cast<uint64_t>(addr);
    }
    static constexpr uint64_t make_list_leaf(uint64_t* addr) noexcept {
        return reinterpret_cast<uint64_t>(addr) | LEAF_BIT | LIST_BIT;
    }
    static constexpr uint64_t make_list_internal(uint64_t* addr) noexcept {
        return reinterpret_cast<uint64_t>(addr) | LIST_BIT;
    }
    
    // Convenience: make pointer with given leaf/list flags
    static constexpr uint64_t make_ptr(uint64_t* addr, bool leaf, bool list) noexcept {
        uint64_t ptr = reinterpret_cast<uint64_t>(addr);
        if (leaf) ptr |= LEAF_BIT;
        if (list) ptr |= LIST_BIT;
        return ptr;
    }
    
    // =========================================================================
    // Node Header (64 bytes = 8 words, debug version)
    // =========================================================================
    //
    // Word 0: skip, leaf_bits, count (direct children), unused, descendants
    // Word 1: prefix
    // Words 2-7: child2..child10 (entry counts at levels 2-10 down)
    //
    // =========================================================================
    
    struct NodeHeader {
        uint8_t skip;
        uint8_t leaf_bits;
        uint8_t count;       // Direct children (level 1) = child1
        uint8_t unused;
        uint32_t descendants; // Total leaf values in subtree
        uint64_t prefix;
        
        // Count of entries at each level below this one
        // child1 = count (direct entries at this node)
        // child2 = sum of children's count
        // child3 = sum of grandchildren's count, etc.
        uint32_t child2;
        uint32_t child3;
        uint32_t child4;
        uint32_t child5;
        uint32_t child6;
        uint32_t child7;
        uint32_t child8;
        uint32_t child9;
        uint32_t child10;
        uint32_t padding[3]; // Pad to 64 bytes
    };
    
    static_assert(sizeof(NodeHeader) == 64, "NodeHeader must be 64 bytes");
    
    static NodeHeader* get_header(uint64_t* node) noexcept {
        return reinterpret_cast<NodeHeader*>(node);
    }
    static const NodeHeader* get_header(const uint64_t* node) noexcept {
        return reinterpret_cast<const NodeHeader*>(node);
    }
    
    // Data starts at word 8 (after 64-byte header)
    static constexpr size_t DATA_OFFSET = 8;
    
    // Initialize child level fields to zero
    static void init_debug_fields(NodeHeader* h) noexcept {
        h->child2 = 0;
        h->child3 = 0;
        h->child4 = 0;
        h->child5 = 0;
        h->child6 = 0;
        h->child7 = 0;
        h->child8 = 0;
        h->child9 = 0;
        h->child10 = 0;
    }
    
    // Add child's stats to parent (shift levels up)
    static void add_child_stats(NodeHeader* parent, const NodeHeader* child) noexcept {
        // child's count becomes parent's child2 contribution
        // child's child2 becomes parent's child3, etc.
        parent->child2 += child->count;
        parent->child3 += child->child2;
        parent->child4 += child->child3;
        parent->child5 += child->child4;
        parent->child6 += child->child5;
        parent->child7 += child->child6;
        parent->child8 += child->child7;
        parent->child9 += child->child8;
        parent->child10 += child->child9;
    }
    
    // Subtract child's stats from parent (for updates)
    static void sub_child_stats(NodeHeader* parent, const NodeHeader* child) noexcept {
        parent->child2 -= child->count;
        parent->child3 -= child->child2;
        parent->child4 -= child->child3;
        parent->child5 -= child->child4;
        parent->child6 -= child->child5;
        parent->child7 -= child->child6;
        parent->child8 -= child->child7;
        parent->child9 -= child->child8;
        parent->child10 -= child->child9;
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
    
    // =========================================================================
    // Bit Extraction
    // =========================================================================
    
    static constexpr size_t extract_root_index(uint64_t internal_key) noexcept {
        return static_cast<size_t>(internal_key >> (64 - root_bits));
    }
    
    // Extract 6-bit index at given bit position
    static constexpr uint8_t extract_index(uint64_t internal_key, size_t bits_remaining) noexcept {
        size_t shift = bits_remaining + (64 - key_bits);
        return static_cast<uint8_t>((internal_key >> shift) & 0x3F);
    }
    
    // Extract prefix bits (skip levels worth)
    static constexpr uint64_t extract_prefix(uint64_t internal_key, size_t bits_remaining, size_t skip) noexcept {
        size_t prefix_bits = skip * 6;
        size_t shift = bits_remaining - prefix_bits + (64 - key_bits);
        uint64_t mask = (1ULL << prefix_bits) - 1;
        return (internal_key >> shift) & mask;
    }
    
    // Extract N bits for LIST key (high bits of current suffix)
    static constexpr uint64_t extract_leaf_key(uint64_t internal_key, size_t bits_remaining, size_t leaf_bits) noexcept {
        size_t shift = (64 - key_bits) + (bits_remaining - leaf_bits);
        uint64_t mask = (1ULL << leaf_bits) - 1;
        return (internal_key >> shift) & mask;
    }
    
    // =========================================================================
    // BITMASK Layout
    // [header][presence_bitmap:64][data...]
    // Data is either all values (LEAF) or all children (INTERNAL) based on pointer tag
    // =========================================================================
    
    static uint64_t get_bitmap(const uint64_t* node, uint8_t skip) noexcept {
        return node[DATA_OFFSET];
    }
    
    static void set_bitmap(uint64_t* node, uint8_t skip, uint64_t bitmap) noexcept {
        node[DATA_OFFSET] = bitmap;
    }
    
    static uint64_t* get_bitmask_data(uint64_t* node, uint8_t skip) noexcept {
        return node + DATA_OFFSET + 1;
    }
    
    static const uint64_t* get_bitmask_data(const uint64_t* node, uint8_t skip) noexcept {
        return node + DATA_OFFSET + 1;
    }
    
    // =========================================================================
    // LIST Layout
    // [header][keys...][data...]
    // Data is either all values (LEAF) or all children (INTERNAL) based on pointer tag
    // 
    // LIST2: 32 slots, 12-bit keys (stored as int16)
    // LIST4: 16 slots, 18/24/30-bit keys (stored as int32)
    // LIST8: 8 slots, 36/42/48/54/60-bit keys (stored as int64)
    // =========================================================================
    
    static constexpr size_t LIST_MAX_SLOTS = 64;  // All LIST types now hold up to 64
    static constexpr size_t LIST2_SLOTS = LIST_MAX_SLOTS;
    static constexpr size_t LIST4_SLOTS = LIST_MAX_SLOTS;
    static constexpr size_t LIST8_SLOTS = LIST_MAX_SLOTS;
    
    // Keys start directly at DATA_OFFSET
    static int16_t* list2_keys(uint64_t* node, uint8_t skip) noexcept {
        return reinterpret_cast<int16_t*>(node + DATA_OFFSET);
    }
    static const int16_t* list2_keys(const uint64_t* node, uint8_t skip) noexcept {
        return reinterpret_cast<const int16_t*>(node + DATA_OFFSET);
    }
    
    static int32_t* list4_keys(uint64_t* node, uint8_t skip) noexcept {
        return reinterpret_cast<int32_t*>(node + DATA_OFFSET);
    }
    static const int32_t* list4_keys(const uint64_t* node, uint8_t skip) noexcept {
        return reinterpret_cast<const int32_t*>(node + DATA_OFFSET);
    }
    
    static int64_t* list8_keys(uint64_t* node, uint8_t skip) noexcept {
        return reinterpret_cast<int64_t*>(node + DATA_OFFSET);
    }
    static const int64_t* list8_keys(const uint64_t* node, uint8_t skip) noexcept {
        return reinterpret_cast<const int64_t*>(node + DATA_OFFSET);
    }
    
    // Key size per element
    static constexpr size_t list_key_size(uint8_t leaf_bits) noexcept {
        if (leaf_bits <= 12) return sizeof(int16_t);
        if (leaf_bits <= 30) return sizeof(int32_t);
        return sizeof(int64_t);
    }
    
    // Key array bytes for a given count (variable size)
    static constexpr size_t list_keys_bytes(uint8_t leaf_bits, size_t count) noexcept {
        return list_key_size(leaf_bits) * count;
    }
    
    static constexpr size_t list_max_slots(uint8_t leaf_bits) noexcept {
        return LIST_MAX_SLOTS;  // All types now 64
    }
    
    // Data (values/children) start after key array - needs count for variable sizing
    static uint64_t* list_data(uint64_t* node, uint8_t leaf_bits, size_t count) noexcept {
        size_t key_bytes = list_keys_bytes(leaf_bits, count);
        // Align to 8 bytes for uint64_t data
        key_bytes = (key_bytes + 7) & ~size_t{7};
        return reinterpret_cast<uint64_t*>(
            reinterpret_cast<char*>(node + DATA_OFFSET) + key_bytes);
    }
    static const uint64_t* list_data(const uint64_t* node, uint8_t leaf_bits, size_t count) noexcept {
        size_t key_bytes = list_keys_bytes(leaf_bits, count);
        key_bytes = (key_bytes + 7) & ~size_t{7};
        return reinterpret_cast<const uint64_t*>(
            reinterpret_cast<const char*>(node + DATA_OFFSET) + key_bytes);
    }
    
    static uint64_t* list_children(uint64_t* node, uint8_t leaf_bits, size_t count) noexcept {
        return list_data(node, leaf_bits, count);
    }
    static const uint64_t* list_children(const uint64_t* node, uint8_t leaf_bits, size_t count) noexcept {
        return list_data(node, leaf_bits, count);
    }
    
    // =========================================================================
    // Node Type Selection
    // =========================================================================
    //
    // bits_remaining = 6 → BITMASK (always)
    // bits_remaining = 12 AND count ≤ 64 → LIST2
    // bits_remaining ∈ {18,24,30} AND count ≤ 64 → LIST4
    // bits_remaining ≥ 36 AND count ≤ 64 → LIST8
    // else → BITMASK
    // =========================================================================
    
    static constexpr bool should_use_list8(size_t bits_remaining, size_t count) noexcept {
        return bits_remaining >= 36 && count <= LIST_MAX_SLOTS;
    }
    
    static constexpr bool should_use_list4(size_t bits_remaining, size_t count) noexcept {
        return bits_remaining >= 18 && bits_remaining <= 30 && count <= LIST_MAX_SLOTS;
    }
    
    static constexpr bool should_use_list2(size_t bits_remaining, size_t count) noexcept {
        return bits_remaining == 12 && count <= LIST_MAX_SLOTS;
    }
    
    // =========================================================================
    // Bitmap Helpers
    // =========================================================================
    
    static constexpr bool bitmap_has(uint64_t bitmap, uint8_t index) noexcept {
        return bitmap & (1ULL << index);
    }
    
    static int calc_slot(uint64_t bitmap, uint8_t index) noexcept {
        if (index >= 63) {
            return std::popcount(bitmap) - 1;
        }
        uint64_t mask = (1ULL << (index + 1)) - 1;
        return std::popcount(bitmap & mask) - 1;
    }
    
    // =========================================================================
    // Value Storage
    // =========================================================================
    
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
    
    // =========================================================================
    // Allocation
    // =========================================================================
    
    uint64_t* alloc_node(size_t u64_count) {
        uint64_t* node = alloc_.allocate(u64_count);
        // Zero-initialize header (first 8 words = 64 bytes)
        for (size_t i = 0; i < DATA_OFFSET; ++i) {
            node[i] = 0;
        }
        return node;
    }
    
    void dealloc_node(uint64_t* node, size_t u64_count) noexcept {
        alloc_.deallocate(node, u64_count);
    }
    
    // BITMASK node size: header + presence_bitmap + data
    static constexpr size_t bitmask_size_u64(size_t count, uint8_t skip) noexcept {
        return DATA_OFFSET + 1 + count;  // header + 1 bitmap + data
    }
    
    // LIST node size: header + keys (variable) + data (variable)
    // Now allocates exact fit based on count
    static constexpr size_t list_size_u64(uint8_t leaf_bits, size_t count) noexcept {
        size_t key_bytes = list_keys_bytes(leaf_bits, count);
        // Align key_bytes to 8 for uint64_t data
        key_bytes = (key_bytes + 7) & ~size_t{7};
        size_t bytes = DATA_OFFSET * 8 + key_bytes + count * sizeof(uint64_t);
        return (bytes + 7) / 8;
    }
    
    // Alias for backward compatibility (will refactor call sites later)
    static constexpr size_t list_leaf_size_u64(uint8_t leaf_bits, size_t count) noexcept {
        return list_size_u64(leaf_bits, count);
    }
    static constexpr size_t list_internal_size_u64(uint8_t leaf_bits, size_t count) noexcept {
        return list_size_u64(leaf_bits, count);
    }
    
    // Allocate empty BITMASK node (as leaf since it's empty)
    uint64_t alloc_empty_bitmask() {
        uint64_t* node = alloc_node(bitmask_size_u64(0, 0));
        NodeHeader* h = get_header(node);
        h->skip = 0;
        h->leaf_bits = 6;
        h->count = 0;
        h->descendants = 0;
        h->prefix = 0;
        set_bitmap(node, 0, 0);
        return make_bitmask_leaf(node);  // Empty node is a leaf
    }
    
    // Legacy name for compatibility
    uint64_t alloc_empty_terminal() { return alloc_empty_bitmask(); }
    
    // =========================================================================
    // Member Data
    // =========================================================================
    
    uint64_t root_[root_size];
    size_t size_;
    [[no_unique_address]] ALLOC alloc_;
    
public:
    // =========================================================================
    // Constructor / Destructor
    // =========================================================================
    
    kntrie() : size_(0), alloc_() {
        for (size_t i = 0; i < root_size; ++i) {
            root_[i] = alloc_empty_terminal();
        }
    }
    
    explicit kntrie(const ALLOC& a) : size_(0), alloc_(a) {
        for (size_t i = 0; i < root_size; ++i) {
            root_[i] = alloc_empty_terminal();
        }
    }
    
    ~kntrie() {
        clear();
        for (size_t i = 0; i < root_size; ++i) {
            uint64_t* node = get_addr(root_[i]);
            dealloc_node(node, bitmask_size_u64(0, 0));
        }
    }
    
    kntrie(const kntrie&) = delete;
    kntrie& operator=(const kntrie&) = delete;
    kntrie(kntrie&&) = delete;
    kntrie& operator=(kntrie&&) = delete;
    
    // =========================================================================
    // Size / Capacity
    // =========================================================================
    
    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }
    [[nodiscard]] size_type size() const noexcept { return size_; }
    
    // =========================================================================
    // Clear
    // =========================================================================
    
    void clear() noexcept {
        for (size_t i = 0; i < root_size; ++i) {
            clear_node(root_[i]);
            root_[i] = alloc_empty_terminal();
        }
        size_ = 0;
    }
    
private:
    void clear_node(uint64_t ptr) noexcept {
        if (ptr == 0) return;
        
        uint64_t* node = get_addr(ptr);
        NodeHeader* h = get_header(node);
        uint8_t skip = h->skip;
        uint8_t count = h->count;
        uint8_t leaf_bits = h->leaf_bits;
        
        if (is_list(ptr)) {
            uint64_t* data = list_data(node, leaf_bits, count);
            
            if (is_leaf(ptr)) {
                // All entries are values - destroy if needed
                if constexpr (!value_inline) {
                    for (size_t i = 0; i < count; ++i) {
                        destroy_value(static_cast<stored_value_type>(data[i]));
                    }
                }
            } else {
                // All entries are children - recurse
                for (size_t i = 0; i < count; ++i) {
                    clear_node(data[i]);
                }
            }
            dealloc_node(node, list_size_u64(leaf_bits, count));
        } else {
            // BITMASK node
            uint64_t bitmap = get_bitmap(node, skip);
            size_t bm_count = std::popcount(bitmap);
            uint64_t* data = get_bitmask_data(node, skip);
            
            if (is_leaf(ptr)) {
                // All entries are values - destroy if needed
                if constexpr (!value_inline) {
                    for (size_t i = 0; i < bm_count; ++i) {
                        destroy_value(static_cast<stored_value_type>(data[i]));
                    }
                }
            } else {
                // All entries are children - recurse
                for (size_t i = 0; i < bm_count; ++i) {
                    clear_node(data[i]);
                }
            }
            dealloc_node(node, bitmask_size_u64(bm_count, skip));
        }
    }
    
public:
    // =========================================================================
    // Find
    // =========================================================================
    
    const VALUE* find_value(const KEY& key) const noexcept {
        uint64_t internal_key = key_to_internal(key);
        size_t root_idx = extract_root_index(internal_key);
        return find_recursive(root_[root_idx], internal_key, bits_after_root);
    }
    
    bool contains(const KEY& key) const noexcept {
        return find_value(key) != nullptr;
    }
    
    size_type count(const KEY& key) const noexcept {
        return contains(key) ? 1 : 0;
    }
    
    // Debug: collect statistics about node types
    struct DebugStats {
        size_t list8_leaf = 0, list8_internal = 0, list8_entries = 0, list8_children = 0;
        size_t list4_leaf = 0, list4_internal = 0, list4_entries = 0, list4_children = 0;
        size_t list2_leaf = 0, list2_internal = 0, list2_entries = 0, list2_children = 0;
        size_t bitmask_terminal = 0, bitmask_internal = 0, bitmask_entries = 0, bitmask_children = 0;
        size_t total_depth = 0, max_depth = 0, leaf_count = 0;
        size_t total_nodes = 0;
        size_t memory_bytes = 0;
        // Histograms: count of nodes with N entries/children
        size_t bitmask_internal_hist[65] = {};  // 0-64 children
        size_t bitmask_leaf_hist[65] = {};      // 0-64 entries
        size_t list8_leaf_hist[9] = {};         // 0-8 entries
        size_t list4_leaf_hist[17] = {};        // 0-16 entries
        size_t list2_leaf_hist[33] = {};        // 0-32 entries
    };
    
    DebugStats debug_stats() const noexcept {
        DebugStats s;
        for (size_t i = 0; i < root_size; ++i) {
            collect_debug_stats(root_[i], 1, s);
        }
        return s;
    }

private:
    void collect_debug_stats(uint64_t ptr, size_t depth, DebugStats& s) const noexcept {
        if (ptr == 0) return;
        
        uint64_t* node = get_addr(ptr);
        NodeHeader* h = get_header(node);
        uint8_t count = h->count;
        uint8_t leaf_bits = h->leaf_bits;
        s.total_nodes++;
        
        if (is_list(ptr)) {
            const uint64_t* data = list_data(node, leaf_bits, count);
            s.memory_bytes += list_size_u64(leaf_bits, count) * 8;
            
            if (is_leaf(ptr)) {
                // All entries are values
                if (leaf_bits <= 12) { 
                    s.list2_leaf++; 
                    s.list2_entries += count; 
                    s.list2_leaf_hist[count]++;
                }
                else if (leaf_bits <= 30) { 
                    s.list4_leaf++; 
                    s.list4_entries += count; 
                    s.list4_leaf_hist[count]++;
                }
                else { 
                    s.list8_leaf++; 
                    s.list8_entries += count; 
                    s.list8_leaf_hist[count]++;
                }
                
                s.total_depth += depth * count;
                s.leaf_count += count;
                if (depth > s.max_depth) s.max_depth = depth;
            } else {
                // All entries are children
                if (leaf_bits <= 12) { s.list2_internal++; s.list2_children += count; }
                else if (leaf_bits <= 30) { s.list4_internal++; s.list4_children += count; }
                else { s.list8_internal++; s.list8_children += count; }
                
                for (uint8_t i = 0; i < count; ++i) {
                    collect_debug_stats(data[i], depth + 1, s);
                }
            }
        } else {
            // BITMASK node
            uint64_t bitmap = get_bitmap(node, h->skip);
            size_t bm_count = std::popcount(bitmap);
            const uint64_t* data = get_bitmask_data(node, h->skip);
            s.memory_bytes += bitmask_size_u64(bm_count, h->skip) * 8;
            
            if (is_leaf(ptr)) {
                // All entries are values
                s.bitmask_terminal++;
                s.bitmask_entries += bm_count;
                s.bitmask_leaf_hist[bm_count]++;
                s.total_depth += depth * bm_count;
                s.leaf_count += bm_count;
                if (depth > s.max_depth) s.max_depth = depth;
            } else {
                // All entries are children
                s.bitmask_internal++;
                s.bitmask_children += bm_count;
                s.bitmask_internal_hist[bm_count]++;
                for (size_t i = 0; i < bm_count; ++i) {
                    collect_debug_stats(data[i], depth + 1, s);
                }
            }
        }
    }

private:
    const VALUE* find_recursive(uint64_t ptr, uint64_t internal_key, size_t bits_remaining) const noexcept {
        if (ptr == 0) return nullptr;
        
        uint64_t* node = get_addr(ptr);
        NodeHeader* h = get_header(node);
        uint8_t skip = h->skip;
        
        // Check prefix if skip > 0
        if (skip > 0) {
            uint64_t expected = extract_prefix(internal_key, bits_remaining, skip);
            uint64_t actual = get_header(node)->prefix;
            if (expected != actual) return nullptr;
            bits_remaining -= skip * 6;
        }
        
        if (is_list(ptr)) {
            return find_in_list(node, ptr, internal_key, bits_remaining);
        } else {
            return find_in_bitmask(node, ptr, internal_key, bits_remaining);
        }
    }
    
    const VALUE* find_in_bitmask(uint64_t* node, uint64_t ptr, uint64_t internal_key, size_t bits_remaining) const noexcept {
        NodeHeader* h = get_header(node);
        uint8_t skip = h->skip;
        
        uint8_t index = extract_index(internal_key, bits_remaining - 6);
        uint64_t bitmap = get_bitmap(node, skip);
        
        if (!bitmap_has(bitmap, index)) return nullptr;
        
        int slot = calc_slot(bitmap, index);
        const uint64_t* data = get_bitmask_data(node, skip);
        
        if (is_leaf(ptr)) {
            // All entries are values - data array uses uint64_t slots
            if constexpr (value_inline) {
                return reinterpret_cast<const VALUE*>(&data[slot]);
            } else {
                return reinterpret_cast<VALUE*>(data[slot]);
            }
        } else {
            // All entries are children - recurse
            return find_recursive(data[slot], internal_key, bits_remaining - 6);
        }
    }
    
    const VALUE* find_in_list(uint64_t* node, uint64_t ptr, uint64_t internal_key, size_t bits_remaining) const noexcept {
        NodeHeader* h = get_header(node);
        uint8_t skip = h->skip;
        uint8_t count = h->count;
        uint8_t leaf_bits = h->leaf_bits;
        
        uint64_t search_key = extract_leaf_key(internal_key, bits_remaining, leaf_bits);
        
        // Search based on key size
        int found_idx = -1;
        if (leaf_bits <= 12) {
            const int16_t* keys = list2_keys(node, skip);
            uint16_t sk = static_cast<uint16_t>(search_key);
            for (uint8_t i = 0; i < count; ++i) {
                if (static_cast<uint16_t>(keys[i]) == sk) { found_idx = i; break; }
            }
        } else if (leaf_bits <= 30) {
            const int32_t* keys = list4_keys(node, skip);
            uint32_t sk = static_cast<uint32_t>(search_key);
            for (uint8_t i = 0; i < count; ++i) {
                if (static_cast<uint32_t>(keys[i]) == sk) { found_idx = i; break; }
            }
        } else {
            const int64_t* keys = list8_keys(node, skip);
            for (uint8_t i = 0; i < count; ++i) {
                if (static_cast<uint64_t>(keys[i]) == search_key) { found_idx = i; break; }
            }
        }
        
        if (found_idx < 0) return nullptr;
        
        const uint64_t* data = list_data(node, leaf_bits, count);
        
        if (is_leaf(ptr)) {
            // All entries are values - data uses uint64_t slots
            if constexpr (value_inline) {
                return reinterpret_cast<const VALUE*>(&data[found_idx]);
            } else {
                return reinterpret_cast<VALUE*>(data[found_idx]);
            }
        } else {
            // All entries are children - recurse
            return find_recursive(data[found_idx], internal_key, bits_remaining - leaf_bits);
        }
    }
    
public:
    // =========================================================================
    // Insert
    // =========================================================================
    
    std::pair<bool, bool> insert(const KEY& key, const VALUE& value) {
        uint64_t internal_key = key_to_internal(key);
        size_t root_idx = extract_root_index(internal_key);
        
        stored_value_type sv = store_value(value);
        
        auto [new_ptr, inserted] = insert_recursive(root_[root_idx], internal_key, sv, bits_after_root);
        
        root_[root_idx] = new_ptr;
        
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
        uint64_t new_ptr;
        bool inserted;
    };
    
    InsertResult insert_recursive(uint64_t ptr, uint64_t internal_key, stored_value_type value, size_t bits_remaining) {
        uint64_t* node = get_addr(ptr);
        NodeHeader* h = get_header(node);
        uint8_t skip = h->skip;
        
        // Check prefix match
        if (skip > 0) {
            uint64_t expected = extract_prefix(internal_key, bits_remaining, skip);
            uint64_t actual = get_header(node)->prefix;
            if (expected != actual) {
                // Prefix mismatch - split
                return split_on_prefix(ptr, internal_key, value, bits_remaining, expected, actual);
            }
            bits_remaining -= skip * 6;
        }
        
        if (is_list(ptr)) {
            return insert_into_list(ptr, internal_key, value, bits_remaining);
        } else {
            return insert_into_bitmask(ptr, internal_key, value, bits_remaining);
        }
    }
    
    // =========================================================================
    // Collapse: Convert BITMASK subtree to LIST when small enough
    // =========================================================================
    
    // Count total leaf values in subtree
    size_t count_descendants(uint64_t ptr) const noexcept {
        if (ptr == 0) return 0;
        
        uint64_t* node = get_addr(ptr);
        NodeHeader* h = get_header(node);
        uint8_t skip = h->skip;
        uint8_t leaf_bits = h->leaf_bits;
        
        if (is_leaf(ptr)) {
            // All entries are values
            return h->count;
        }
        
        // Internal node - recurse into children
        const uint64_t* data;
        size_t count;
        
        if (is_list(ptr)) {
            count = h->count;
            data = list_data(node, leaf_bits, count);
        } else {
            uint64_t bitmap = get_bitmap(node, skip);
            data = get_bitmask_data(node, skip);
            count = std::popcount(bitmap);
        }
        
        size_t total = 0;
        for (size_t i = 0; i < count; ++i) {
            total += count_descendants(data[i]);
        }
        return total;
    }
    
    // Entry for collecting from subtree
    struct CollectedEntry {
        uint64_t key_suffix;  // Key bits within this subtree
        stored_value_type value;
    };
    
    // Collect all entries from subtree, building key_suffix from traversal
    void collect_entries(uint64_t ptr, uint64_t prefix, size_t prefix_bits,
                         CollectedEntry* entries, size_t& idx) const noexcept {
        if (ptr == 0) return;
        
        uint64_t* node = get_addr(ptr);
        NodeHeader* h = get_header(node);
        uint8_t skip = h->skip;
        
        // Add skip prefix bits
        if (skip > 0) {
            uint64_t node_prefix = get_header(node)->prefix;
            prefix = (prefix << (skip * 6)) | node_prefix;
            prefix_bits += skip * 6;
        }
        
        if (is_list(ptr)) {
            uint8_t leaf_bits = h->leaf_bits;
            const uint64_t* data = list_data(node, leaf_bits, h->count);
            
            if (is_leaf(ptr)) {
                // All entries are values
                if (leaf_bits <= 12) {
                    const int16_t* keys = list2_keys(node, skip);
                    for (uint8_t i = 0; i < h->count; ++i) {
                        uint64_t entry_key = (prefix << leaf_bits) | static_cast<uint16_t>(keys[i]);
                        stored_value_type val;
                        if constexpr (value_inline) {
                            std::memcpy(&val, &data[i], sizeof(stored_value_type));
                        } else {
                            val = static_cast<stored_value_type>(data[i]);
                        }
                        entries[idx++] = {entry_key, val};
                    }
                } else if (leaf_bits <= 30) {
                    const int32_t* keys = list4_keys(node, skip);
                    for (uint8_t i = 0; i < h->count; ++i) {
                        uint64_t entry_key = (prefix << leaf_bits) | static_cast<uint32_t>(keys[i]);
                        stored_value_type val;
                        if constexpr (value_inline) {
                            std::memcpy(&val, &data[i], sizeof(stored_value_type));
                        } else {
                            val = static_cast<stored_value_type>(data[i]);
                        }
                        entries[idx++] = {entry_key, val};
                    }
                } else {
                    const int64_t* keys = list8_keys(node, skip);
                    for (uint8_t i = 0; i < h->count; ++i) {
                        uint64_t entry_key = (prefix << leaf_bits) | static_cast<uint64_t>(keys[i]);
                        stored_value_type val;
                        if constexpr (value_inline) {
                            std::memcpy(&val, &data[i], sizeof(stored_value_type));
                        } else {
                            val = static_cast<stored_value_type>(data[i]);
                        }
                        entries[idx++] = {entry_key, val};
                    }
                }
            } else {
                // All entries are children - recurse
                if (leaf_bits <= 12) {
                    const int16_t* keys = list2_keys(node, skip);
                    for (uint8_t i = 0; i < h->count; ++i) {
                        uint64_t entry_key = (prefix << leaf_bits) | static_cast<uint16_t>(keys[i]);
                        collect_entries(data[i], entry_key, prefix_bits + leaf_bits, entries, idx);
                    }
                } else if (leaf_bits <= 30) {
                    const int32_t* keys = list4_keys(node, skip);
                    for (uint8_t i = 0; i < h->count; ++i) {
                        uint64_t entry_key = (prefix << leaf_bits) | static_cast<uint32_t>(keys[i]);
                        collect_entries(data[i], entry_key, prefix_bits + leaf_bits, entries, idx);
                    }
                } else {
                    const int64_t* keys = list8_keys(node, skip);
                    for (uint8_t i = 0; i < h->count; ++i) {
                        uint64_t entry_key = (prefix << leaf_bits) | static_cast<uint64_t>(keys[i]);
                        collect_entries(data[i], entry_key, prefix_bits + leaf_bits, entries, idx);
                    }
                }
            }
        } else {
            uint64_t bitmap = get_bitmap(node, skip);
            const uint64_t* data = get_bitmask_data(node, skip);
            
            if (is_leaf(ptr)) {
                // All entries are values
                int slot = 0;
                for (uint8_t bit = 0; bit < 64; ++bit) {
                    if (!(bitmap & (1ULL << bit))) continue;
                    uint64_t entry_key = (prefix << 6) | bit;
                    stored_value_type val;
                    if constexpr (value_inline) {
                        std::memcpy(&val, &data[slot], sizeof(stored_value_type));
                    } else {
                        val = static_cast<stored_value_type>(data[slot]);
                    }
                    entries[idx++] = {entry_key, val};
                    slot++;
                }
            } else {
                // All entries are children - recurse
                int slot = 0;
                for (uint8_t bit = 0; bit < 64; ++bit) {
                    if (!(bitmap & (1ULL << bit))) continue;
                    uint64_t entry_key = (prefix << 6) | bit;
                    collect_entries(data[slot], entry_key, prefix_bits + 6, entries, idx);
                    slot++;
                }
            }
        }
    }
    
    // Free subtree without destroying values (they've been moved to new node)
    void free_subtree_no_destroy(uint64_t ptr) noexcept {
        if (ptr == 0) return;
        
        uint64_t* node = get_addr(ptr);
        NodeHeader* h = get_header(node);
        uint8_t skip = h->skip;
        uint8_t leaf_bits = h->leaf_bits;
        
        if (is_leaf(ptr)) {
            // No children to recurse into, just free this node
            if (is_list(ptr)) {
                dealloc_node(node, list_size_u64(leaf_bits, h->count));
            } else {
                uint64_t bitmap = get_bitmap(node, skip);
                dealloc_node(node, bitmask_size_u64(std::popcount(bitmap), skip));
            }
        } else {
            // Internal node - recurse into all children
            if (is_list(ptr)) {
                const uint64_t* data = list_data(node, leaf_bits, h->count);
                for (uint8_t i = 0; i < h->count; ++i) {
                    free_subtree_no_destroy(data[i]);
                }
                dealloc_node(node, list_size_u64(leaf_bits, h->count));
            } else {
                uint64_t bitmap = get_bitmap(node, skip);
                size_t bm_count = std::popcount(bitmap);
                const uint64_t* data = get_bitmask_data(node, skip);
                for (size_t i = 0; i < bm_count; ++i) {
                    free_subtree_no_destroy(data[i]);
                }
                dealloc_node(node, bitmask_size_u64(bm_count, skip));
            }
        }
    }
    
    // Check if a node has any non-terminal (child) entries
    bool has_child_entries(uint64_t ptr) const noexcept {
        // With LEAF bit, this is simple: internal nodes have children
        return ptr != 0 && !is_leaf(ptr);
    }
    
    // Try to collapse a BITMASK subtree to a LIST node if small enough
    uint64_t try_collapse_to_list(uint64_t ptr, size_t bits_remaining) {
        // Only collapse BITMASK nodes that have children
        if (is_list(ptr)) return ptr;  // Already a LIST
        if (!has_child_entries(ptr)) return ptr;  // All entries are terminal, nothing to collapse
        if (bits_remaining <= 6) return ptr;  // Already at terminal level
        
        size_t count = count_descendants(ptr);
        
        // Check if we should collapse based on count and bits_remaining
        size_t max_list8 = LIST8_SLOTS;
        size_t max_list4 = LIST4_SLOTS;
        size_t max_list2 = LIST2_SLOTS;
        
        bool should_collapse = false;
        size_t target_leaf_bits = 0;
        
        if (bits_remaining >= 36 && count <= max_list8) {
            should_collapse = true;
            target_leaf_bits = std::min(bits_remaining, size_t{60});
            target_leaf_bits = (target_leaf_bits / 6) * 6;
        } else if (bits_remaining >= 18 && bits_remaining <= 30 && count <= max_list4) {
            should_collapse = true;
            target_leaf_bits = std::min(bits_remaining, size_t{30});
            target_leaf_bits = (target_leaf_bits / 6) * 6;
        } else if (bits_remaining == 12 && count <= max_list2) {
            should_collapse = true;
            target_leaf_bits = 12;
        }
        
        if (!should_collapse) return ptr;
        
        // Collect all entries
        CollectedEntry entries[LIST2_SLOTS + 1];
        size_t idx = 0;
        collect_entries(ptr, 0, 0, entries, idx);
        
        // Free old subtree (values moved, not destroyed)
        free_subtree_no_destroy(ptr);
        
        // Create appropriate LIST-LEAF
        size_t skip = (bits_remaining - target_leaf_bits) / 6;
        
        uint64_t* node = alloc_node(list_leaf_size_u64(target_leaf_bits, count));
        NodeHeader* h = get_header(node);
        h->skip = static_cast<uint8_t>(skip);
        h->leaf_bits = static_cast<uint8_t>(target_leaf_bits);
        h->count = static_cast<uint8_t>(count);
        h->descendants = h->count;
        h->prefix = 0;
        
        // Extract and set prefix if skip > 0
        if (skip > 0 && count > 0) {
            // Get prefix from first entry
            uint64_t first_key = entries[0].key_suffix;
            uint64_t prefix = first_key >> target_leaf_bits;
            get_header(node)->prefix = prefix;
        }
        
        // Fill keys and values - data uses uint64_t slots
        uint64_t* data = list_data(node, static_cast<uint8_t>(target_leaf_bits), count);
        uint64_t leaf_mask = (1ULL << target_leaf_bits) - 1;
        
        if (target_leaf_bits <= 12) {
            int16_t* keys = list2_keys(node, skip);
            for (size_t i = 0; i < count; ++i) {
                keys[i] = static_cast<int16_t>(entries[i].key_suffix & leaf_mask);
                if constexpr (value_inline) {
                    std::memcpy(&data[i], &entries[i].value, sizeof(stored_value_type));
                } else {
                    data[i] = static_cast<uint64_t>(entries[i].value);
                }
            }
        } else if (target_leaf_bits <= 30) {
            int32_t* keys = list4_keys(node, skip);
            for (size_t i = 0; i < count; ++i) {
                keys[i] = static_cast<int32_t>(entries[i].key_suffix & leaf_mask);
                if constexpr (value_inline) {
                    std::memcpy(&data[i], &entries[i].value, sizeof(stored_value_type));
                } else {
                    data[i] = static_cast<uint64_t>(entries[i].value);
                }
            }
        } else {
            int64_t* keys = list8_keys(node, skip);
            for (size_t i = 0; i < count; ++i) {
                keys[i] = static_cast<int64_t>(entries[i].key_suffix & leaf_mask);
                if constexpr (value_inline) {
                    std::memcpy(&data[i], &entries[i].value, sizeof(stored_value_type));
                } else {
                    data[i] = static_cast<uint64_t>(entries[i].value);
                }
            }
        }
        
        return make_list_leaf(node);
    }
    
    InsertResult insert_into_bitmask(uint64_t ptr, uint64_t internal_key, stored_value_type value, size_t bits_remaining) {
        uint64_t* node = get_addr(ptr);
        NodeHeader* h = get_header(node);
        uint8_t skip = h->skip;
        
        uint64_t bitmap = get_bitmap(node, skip);
        uint8_t index = extract_index(internal_key, bits_remaining - 6);
        
        // Handle empty node at wrong level
        if (bitmap == 0 && bits_remaining > 6) {
            dealloc_node(node, bitmask_size_u64(0, skip));
            uint64_t new_node = create_leaf_for_insert(internal_key, value, bits_remaining);
            return {new_node, true};
        }
        
        if (bitmap_has(bitmap, index)) {
            int slot = calc_slot(bitmap, index);
            uint64_t* data = get_bitmask_data(node, skip);
            
            if (is_leaf(ptr)) {
                // All entries are values - update existing value
                if constexpr (!value_inline) {
                    destroy_value(static_cast<stored_value_type>(data[slot]));
                }
                if constexpr (value_inline) {
                    std::memcpy(&data[slot], &value, sizeof(stored_value_type));
                } else {
                    data[slot] = static_cast<uint64_t>(value);
                }
                return {ptr, false};
            } else {
                // All entries are children - recurse
                uint64_t old_child = data[slot];
                NodeHeader old_child_h_copy = *get_header(get_addr(old_child));
                
                auto [new_child, inserted] = insert_recursive(data[slot], internal_key, value, bits_remaining - 6);
                
                if (new_child != old_child) {
                    sub_child_stats(h, &old_child_h_copy);
                    data[slot] = new_child;
                    add_child_stats(h, get_header(get_addr(new_child)));
                }
                if (inserted) {
                    h->descendants++;
                    uint64_t collapsed = try_collapse_to_list(ptr, bits_remaining);
                    return {collapsed, inserted};
                }
                return {ptr, inserted};
            }
        }
        
        // Index not present - add new entry
        size_t old_count = std::popcount(bitmap);
        size_t new_count = old_count + 1;
        uint64_t new_bitmap = bitmap | (1ULL << index);
        int slot = calc_slot(new_bitmap, index);
        
        uint64_t* new_node = alloc_node(bitmask_size_u64(new_count, skip));
        *get_header(new_node) = *h;
        get_header(new_node)->count = static_cast<uint8_t>(new_count);
        get_header(new_node)->descendants = h->descendants + 1;
        set_bitmap(new_node, skip, new_bitmap);
        
        uint64_t* old_data = get_bitmask_data(node, skip);
        uint64_t* new_data = get_bitmask_data(new_node, skip);
        
        // Determine if we're at leaf level or need to create child
        bool at_leaf_level = (bits_remaining == 6);
        
        if (at_leaf_level) {
            // Add new value entry
            for (int i = 0; i < slot; ++i) new_data[i] = old_data[i];
            if constexpr (value_inline) {
                std::memcpy(&new_data[slot], &value, sizeof(stored_value_type));
            } else {
                new_data[slot] = static_cast<uint64_t>(value);
            }
            for (size_t i = slot; i < old_count; ++i) new_data[i + 1] = old_data[i];
            
            dealloc_node(node, bitmask_size_u64(old_count, skip));
            return {make_bitmask_leaf(new_node), true};
        } else {
            // Create new child for remaining key bits
            uint64_t child_ptr = create_leaf_for_insert(internal_key, value, bits_remaining - 6);
            add_child_stats(get_header(new_node), get_header(get_addr(child_ptr)));
            
            for (int i = 0; i < slot; ++i) new_data[i] = old_data[i];
            new_data[slot] = child_ptr;
            for (size_t i = slot; i < old_count; ++i) new_data[i + 1] = old_data[i];
            
            dealloc_node(node, bitmask_size_u64(old_count, skip));
            return {make_bitmask_internal(new_node), true};
        }
    }
    
    InsertResult insert_into_list(uint64_t ptr, uint64_t internal_key, stored_value_type value, size_t bits_remaining) {
        uint64_t* node = get_addr(ptr);
        NodeHeader* h = get_header(node);
        uint8_t skip = h->skip;
        uint8_t count = h->count;
        uint8_t leaf_bits = h->leaf_bits;
        
        uint64_t search_key = extract_leaf_key(internal_key, bits_remaining, leaf_bits);
        uint64_t* data = list_data(node, leaf_bits, count);
        
        // Search for existing key
        int found_idx = -1;
        if (leaf_bits <= 12) {
            const int16_t* keys = list2_keys(node, skip);
            uint16_t sk = static_cast<uint16_t>(search_key);
            for (uint8_t i = 0; i < count; ++i) {
                if (static_cast<uint16_t>(keys[i]) == sk) { found_idx = i; break; }
            }
        } else if (leaf_bits <= 30) {
            const int32_t* keys = list4_keys(node, skip);
            uint32_t sk = static_cast<uint32_t>(search_key);
            for (uint8_t i = 0; i < count; ++i) {
                if (static_cast<uint32_t>(keys[i]) == sk) { found_idx = i; break; }
            }
        } else {
            const int64_t* keys = list8_keys(node, skip);
            for (uint8_t i = 0; i < count; ++i) {
                if (static_cast<uint64_t>(keys[i]) == search_key) { found_idx = i; break; }
            }
        }
        
        if (found_idx >= 0) {
            if (is_leaf(ptr)) {
                // Update existing value - data uses uint64_t slots
                if constexpr (!value_inline) {
                    destroy_value(static_cast<stored_value_type>(data[found_idx]));
                }
                if constexpr (value_inline) {
                    std::memcpy(&data[found_idx], &value, sizeof(stored_value_type));
                } else {
                    data[found_idx] = static_cast<uint64_t>(value);
                }
                return {ptr, false};
            } else {
                // Recurse into child
                uint64_t old_child = data[found_idx];
                NodeHeader old_child_h_copy = *get_header(get_addr(old_child));
                
                auto [new_child, inserted] = insert_recursive(data[found_idx], internal_key, value, bits_remaining - leaf_bits);
                
                if (new_child != old_child) {
                    sub_child_stats(h, &old_child_h_copy);
                    data[found_idx] = new_child;
                    add_child_stats(h, get_header(get_addr(new_child)));
                }
                if (inserted) {
                    h->descendants++;
                }
                return {ptr, inserted};
            }
        }
        
        // Need to insert new entry
        size_t max_count = list_max_slots(leaf_bits);
        
        bool at_leaf_level = (bits_remaining == leaf_bits);
        
        // If list is full, convert to BITMASK
        if (count >= max_count) {
            if (is_leaf(ptr)) {
                return split_list_to_bitmask(ptr, internal_key, value, bits_remaining);
            } else {
                // LIST-INTERNAL is full - convert to BITMASK-INTERNAL
                return convert_list_internal_to_bitmask(ptr, internal_key, value, bits_remaining);
            }
        }
        
        // Add new entry
        size_t new_count = count + 1;
        
        uint64_t* new_node = alloc_node(list_size_u64(leaf_bits, new_count));
        *get_header(new_node) = *h;
        get_header(new_node)->count = static_cast<uint8_t>(new_count);
        get_header(new_node)->descendants = h->descendants + 1;
        
        // Copy keys and add new one
        if (leaf_bits <= 12) {
            const int16_t* old_keys = list2_keys(node, skip);
            int16_t* new_keys = list2_keys(new_node, skip);
            for (size_t i = 0; i < count; ++i) new_keys[i] = old_keys[i];
            new_keys[count] = static_cast<int16_t>(search_key);
        } else if (leaf_bits <= 30) {
            const int32_t* old_keys = list4_keys(node, skip);
            int32_t* new_keys = list4_keys(new_node, skip);
            for (size_t i = 0; i < count; ++i) new_keys[i] = old_keys[i];
            new_keys[count] = static_cast<int32_t>(search_key);
        } else {
            const int64_t* old_keys = list8_keys(node, skip);
            int64_t* new_keys = list8_keys(new_node, skip);
            for (size_t i = 0; i < count; ++i) new_keys[i] = old_keys[i];
            new_keys[count] = static_cast<int64_t>(search_key);
        }
        
        // Copy data and add new entry
        uint64_t* old_data = list_data(node, leaf_bits, count);
        uint64_t* new_data = list_data(new_node, leaf_bits, new_count);
        for (size_t i = 0; i < count; ++i) new_data[i] = old_data[i];
        
        if (at_leaf_level) {
            // Store value - data uses uint64_t slots
            if constexpr (value_inline) {
                std::memcpy(&new_data[count], &value, sizeof(stored_value_type));
            } else {
                new_data[count] = static_cast<uint64_t>(value);
            }
            dealloc_node(node, list_size_u64(leaf_bits, count));
            return {make_list_leaf(new_node), true};
        } else {
            // Create child for remaining bits
            uint64_t child_ptr = create_leaf_for_insert(internal_key, value, bits_remaining - leaf_bits);
            new_data[count] = child_ptr;
            add_child_stats(get_header(new_node), get_header(get_addr(child_ptr)));
            dealloc_node(node, list_size_u64(leaf_bits, count));
            return {make_list_internal(new_node), true};
        }
    }
    
    // Create appropriate leaf node for single-value insert
    // Use LIST for sparse data (large bits_remaining), BITMASK for dense (6 bits)
    uint64_t create_leaf_for_insert(uint64_t internal_key, stored_value_type value, size_t bits_remaining) {
        if (bits_remaining >= 36) {
            return create_list8_leaf(internal_key, value, bits_remaining);
        } else if (bits_remaining >= 18) {
            return create_list4_leaf(internal_key, value, bits_remaining);
        } else if (bits_remaining == 12) {
            return create_list2_leaf(internal_key, value, bits_remaining);
        } else {
            // bits_remaining == 6: use BITMASK (no skip needed)
            return create_bitmask_leaf(internal_key, value);
        }
    }
    
    uint64_t create_list8_leaf(uint64_t internal_key, stored_value_type value, size_t bits_remaining) {
        // Calculate skip: we want leaf_bits to be a multiple of 6, max 60
        // bits_remaining = skip*6 + leaf_bits
        // Want largest leaf_bits <= 60 that's multiple of 6
        size_t leaf_bits = std::min(bits_remaining, size_t{60});
        leaf_bits = (leaf_bits / 6) * 6;  // Round down to multiple of 6
        size_t skip = (bits_remaining - leaf_bits) / 6;
        
        uint64_t* node = alloc_node(list_size_u64(static_cast<uint8_t>(leaf_bits), 1));
        NodeHeader* h = get_header(node);
        h->skip = static_cast<uint8_t>(skip);
        h->leaf_bits = static_cast<uint8_t>(leaf_bits);
        h->count = 1;
        h->descendants = h->count;
        
        if (skip > 0) {
            uint64_t prefix = extract_prefix(internal_key, bits_remaining, skip);
            get_header(node)->prefix = prefix;
        }
        
        int64_t* keys = list8_keys(node, skip);
        uint64_t leaf_key = extract_leaf_key(internal_key, bits_remaining - skip * 6, leaf_bits);
        keys[0] = static_cast<int64_t>(leaf_key);
        
        // Store value - data uses uint64_t slots
        uint64_t* data = list_data(node, static_cast<uint8_t>(leaf_bits), 1);
        if constexpr (value_inline) {
            std::memcpy(&data[0], &value, sizeof(stored_value_type));
        } else {
            data[0] = static_cast<uint64_t>(value);
        }
        
        return make_list_leaf(node);
    }
    
    uint64_t create_list4_leaf(uint64_t internal_key, stored_value_type value, size_t bits_remaining) {
        size_t leaf_bits = std::min(bits_remaining, size_t{30});
        leaf_bits = (leaf_bits / 6) * 6;
        size_t skip = (bits_remaining - leaf_bits) / 6;
        
        uint64_t* node = alloc_node(list_size_u64(static_cast<uint8_t>(leaf_bits), 1));
        NodeHeader* h = get_header(node);
        h->skip = static_cast<uint8_t>(skip);
        h->leaf_bits = static_cast<uint8_t>(leaf_bits);
        h->count = 1;
        h->descendants = h->count;
        
        if (skip > 0) {
            uint64_t prefix = extract_prefix(internal_key, bits_remaining, skip);
            get_header(node)->prefix = prefix;
        }
        
        int32_t* keys = list4_keys(node, skip);
        uint32_t leaf_key = static_cast<uint32_t>(extract_leaf_key(internal_key, bits_remaining - skip * 6, leaf_bits));
        keys[0] = static_cast<int32_t>(leaf_key);
        
        // Store value - data uses uint64_t slots
        uint64_t* data = list_data(node, static_cast<uint8_t>(leaf_bits), 1);
        if constexpr (value_inline) {
            std::memcpy(&data[0], &value, sizeof(stored_value_type));
        } else {
            data[0] = static_cast<uint64_t>(value);
        }
        
        return make_list_leaf(node);
    }
    
    uint64_t create_list2_leaf(uint64_t internal_key, stored_value_type value, size_t bits_remaining) {
        size_t leaf_bits = 12;
        size_t skip = (bits_remaining - leaf_bits) / 6;
        
        uint64_t* node = alloc_node(list_size_u64(static_cast<uint8_t>(leaf_bits), 1));
        NodeHeader* h = get_header(node);
        h->skip = static_cast<uint8_t>(skip);
        h->leaf_bits = static_cast<uint8_t>(leaf_bits);
        h->count = 1;
        h->descendants = h->count;
        
        if (skip > 0) {
            uint64_t prefix = extract_prefix(internal_key, bits_remaining, skip);
            get_header(node)->prefix = prefix;
        }
        
        int16_t* keys = list2_keys(node, skip);
        uint16_t leaf_key = static_cast<uint16_t>(extract_leaf_key(internal_key, bits_remaining - skip * 6, leaf_bits));
        keys[0] = static_cast<int16_t>(leaf_key);
        
        // Store value - data uses uint64_t slots
        uint64_t* data = list_data(node, static_cast<uint8_t>(leaf_bits), 1);
        if constexpr (value_inline) {
            std::memcpy(&data[0], &value, sizeof(stored_value_type));
        } else {
            data[0] = static_cast<uint64_t>(value);
        }
        
        return make_list_leaf(node);
    }
    
    uint64_t create_bitmask_leaf(uint64_t internal_key, stored_value_type value) {
        uint64_t* node = alloc_node(bitmask_size_u64(1, 0));
        NodeHeader* h = get_header(node);
        h->skip = 0;
        h->leaf_bits = 6;
        h->count = 1;
        h->descendants = h->count;
        h->prefix = 0;
        
        uint8_t index = extract_index(internal_key, 0);
        set_bitmap(node, 0, 1ULL << index);
        
        // Store value - data uses uint64_t slots
        uint64_t* data = get_bitmask_data(node, 0);
        if constexpr (value_inline) {
            std::memcpy(&data[0], &value, sizeof(stored_value_type));
        } else {
            data[0] = static_cast<uint64_t>(value);
        }
        
        return make_bitmask_leaf(node);
    }
    
    InsertResult split_on_prefix(uint64_t ptr, uint64_t internal_key, stored_value_type value,
                                  size_t bits_remaining, uint64_t expected, uint64_t actual) {
        uint64_t* node = get_addr(ptr);
        NodeHeader* h = get_header(node);
        uint8_t skip = h->skip;
        
        // Find common prefix length
        size_t prefix_bits = skip * 6;
        uint64_t diff = expected ^ actual;
        size_t leading_zeros = std::countl_zero(diff);
        size_t common_bits = leading_zeros - (64 - prefix_bits);
        size_t common_levels = common_bits / 6;
        
        // Indices where they diverge
        uint8_t new_index = (expected >> (prefix_bits - (common_levels + 1) * 6)) & 0x3F;
        uint8_t old_index = (actual >> (prefix_bits - (common_levels + 1) * 6)) & 0x3F;
        
        // Create BITMASK internal node with 2 children
        uint64_t* new_internal = alloc_node(bitmask_size_u64(2, common_levels));
        NodeHeader* nh = get_header(new_internal);
        nh->skip = static_cast<uint8_t>(common_levels);
        nh->leaf_bits = 6;
        nh->count = 2;
        nh->descendants = h->descendants + 1;  // Old subtree + new entry
        
        if (common_levels > 0) {
            uint64_t common_prefix = actual >> (prefix_bits - common_levels * 6);
            get_header(new_internal)->prefix = common_prefix;
        }
        
        set_bitmap(new_internal, common_levels, (1ULL << new_index) | (1ULL << old_index));
        
        // Create new leaf for the new key
        size_t new_bits_remaining = bits_remaining - (common_levels + 1) * 6;
        uint64_t new_child = create_leaf_for_insert(internal_key, value, new_bits_remaining);
        
        // Adjust old node
        uint64_t old_child = adjust_node_skip(ptr, skip, common_levels + 1, actual, prefix_bits);
        
        // Place children in correct order
        uint64_t* children = get_bitmask_data(new_internal, common_levels);
        if (new_index < old_index) {
            children[0] = new_child;
            children[1] = old_child;
        } else {
            children[0] = old_child;
            children[1] = new_child;
        }
        
        // Add children's stats to parent
        add_child_stats(nh, get_header(get_addr(new_child)));
        add_child_stats(nh, get_header(get_addr(old_child)));
        
        return {make_bitmask_internal(new_internal), true};
    }
    
    uint64_t adjust_node_skip(uint64_t ptr, uint8_t old_skip, size_t levels_consumed, 
                               uint64_t old_prefix, size_t old_prefix_bits) {
        uint64_t* old_node = get_addr(ptr);
        NodeHeader* old_h = get_header(old_node);
        bool was_leaf = is_leaf(ptr);
        bool was_list = is_list(ptr);
        
        uint8_t remaining_skip = old_skip - levels_consumed;
        
        if (was_list) {
            uint8_t count = old_h->count;
            uint8_t leaf_bits = old_h->leaf_bits;
            
            // Allocate new node with same size
            uint64_t* new_node = alloc_node(list_size_u64(leaf_bits, count));
            NodeHeader* new_h = get_header(new_node);
            new_h->skip = remaining_skip;
            new_h->leaf_bits = leaf_bits;
            new_h->count = count;
            new_h->descendants = old_h->descendants;
            new_h->child2 = old_h->child2;
            new_h->child3 = old_h->child3;
            new_h->child4 = old_h->child4;
            new_h->child5 = old_h->child5;
            new_h->child6 = old_h->child6;
            new_h->child7 = old_h->child7;
            new_h->child8 = old_h->child8;
            new_h->child9 = old_h->child9;
            new_h->child10 = old_h->child10;
            
            if (remaining_skip > 0) {
                uint64_t new_prefix = old_prefix & ((1ULL << (remaining_skip * 6)) - 1);
                get_header(new_node)->prefix = new_prefix;
            }
            
            // Copy keys (starting at DATA_OFFSET)
            std::memcpy(new_node + DATA_OFFSET, old_node + DATA_OFFSET, list_keys_bytes(leaf_bits, count));
            
            // Copy data
            uint64_t* old_data = list_data(old_node, leaf_bits, count);
            uint64_t* new_data = list_data(new_node, leaf_bits, count);
            for (size_t i = 0; i < count; ++i) new_data[i] = old_data[i];
            
            dealloc_node(old_node, list_size_u64(leaf_bits, count));
            return make_ptr(new_node, was_leaf, true);
        } else {
            uint64_t old_bitmap = get_bitmap(old_node, old_skip);
            size_t count = std::popcount(old_bitmap);
            
            uint64_t* new_node = alloc_node(bitmask_size_u64(count, remaining_skip));
            NodeHeader* new_h = get_header(new_node);
            new_h->skip = remaining_skip;
            new_h->leaf_bits = 6;
            new_h->count = static_cast<uint8_t>(count);
            new_h->descendants = old_h->descendants;
            new_h->child2 = old_h->child2;
            new_h->child3 = old_h->child3;
            new_h->child4 = old_h->child4;
            new_h->child5 = old_h->child5;
            new_h->child6 = old_h->child6;
            new_h->child7 = old_h->child7;
            new_h->child8 = old_h->child8;
            new_h->child9 = old_h->child9;
            new_h->child10 = old_h->child10;
            
            if (remaining_skip > 0) {
                uint64_t new_prefix = old_prefix & ((1ULL << (remaining_skip * 6)) - 1);
                get_header(new_node)->prefix = new_prefix;
            }
            
            set_bitmap(new_node, remaining_skip, old_bitmap);
            
            // Copy data
            uint64_t* old_data = get_bitmask_data(old_node, old_skip);
            uint64_t* new_data = get_bitmask_data(new_node, remaining_skip);
            for (size_t i = 0; i < count; ++i) new_data[i] = old_data[i];
            
            dealloc_node(old_node, bitmask_size_u64(count, old_skip));
            return make_ptr(new_node, was_leaf, false);
        }
    }
    
    // Convert a full LIST-INTERNAL to BITMASK-INTERNAL (preserving children)
    InsertResult convert_list_internal_to_bitmask(uint64_t ptr, uint64_t internal_key, stored_value_type value, size_t bits_remaining) {
        uint64_t* node = get_addr(ptr);
        NodeHeader* h = get_header(node);
        uint8_t skip = h->skip;
        uint8_t count = h->count;
        uint8_t leaf_bits = h->leaf_bits;
        uint64_t old_prefix = h->prefix;
        
        // Collect all values from descendants, then rebuild
        // First count total descendants
        size_t total_values = h->descendants + 1;  // +1 for new value
        
        // Collect all entries (key, value pairs) - use class-level CollectedEntry
        CollectedEntry* entries = static_cast<CollectedEntry*>(alloca(total_values * sizeof(CollectedEntry)));
        size_t entry_idx = 0;
        
        // Recursively collect all leaf values
        uint64_t prefix_shifted = old_prefix << leaf_bits;
        const uint64_t* old_children = list_children(node, leaf_bits, count);
        
        for (size_t i = 0; i < count; ++i) {
            uint64_t child_key;
            if (leaf_bits <= 12) {
                child_key = prefix_shifted | static_cast<uint16_t>(list2_keys(node, skip)[i]);
            } else if (leaf_bits <= 30) {
                child_key = prefix_shifted | static_cast<uint32_t>(list4_keys(node, skip)[i]);
            } else {
                child_key = prefix_shifted | static_cast<uint64_t>(list8_keys(node, skip)[i]);
            }
            collect_entries(old_children[i], child_key, skip * 6 + leaf_bits, entries, entry_idx);
        }
        
        // Add new entry
        // full_bits = total bits in subtree = skip*6 + bits_remaining
        size_t full_bits = skip * 6 + bits_remaining;
        // Extract the correct portion of the internal key
        // Using same logic as extract_leaf_key but for full subtree
        size_t shift = (64 - key_bits);  // Align to where key bits start
        uint64_t new_entry_key = (internal_key >> shift) & ((1ULL << full_bits) - 1);
        entries[entry_idx++] = {new_entry_key, value};
        
        // Free old subtree (values moved, not destroyed)
        free_subtree_no_destroy(ptr);
        
        // Create new structure using create_child_from_entries
        struct Entry { uint64_t key; stored_value_type val; };
        Entry* rebuild_entries = static_cast<Entry*>(alloca(entry_idx * sizeof(Entry)));
        for (size_t i = 0; i < entry_idx; ++i) {
            rebuild_entries[i] = {entries[i].key_suffix, entries[i].value};
        }
        
        uint64_t new_ptr = create_child_from_entries(rebuild_entries, entry_idx, full_bits);
        return {new_ptr, true};
    }
    
    InsertResult split_list_to_bitmask(uint64_t ptr, uint64_t internal_key, stored_value_type value, size_t bits_remaining) {
        uint64_t* node = get_addr(ptr);
        NodeHeader* h = get_header(node);
        uint8_t skip = h->skip;
        uint8_t count = h->count;
        uint8_t leaf_bits = h->leaf_bits;
        uint64_t old_prefix = h->prefix;
        
        // LIST nodes only contain terminal entries (values)
        // Gather all entries including new one
        struct Entry { uint64_t key; stored_value_type val; };
        Entry entries[LIST2_SLOTS + 1];
        size_t total = count + 1;
        
        uint64_t new_leaf_key = extract_leaf_key(internal_key, bits_remaining, leaf_bits);
        uint64_t prefix_shifted = old_prefix << leaf_bits;
        const uint64_t* data = list_data(node, leaf_bits, count);
        
        if (leaf_bits <= 12) {
            const int16_t* keys = list2_keys(node, skip);
            for (size_t i = 0; i < count; ++i) {
                stored_value_type v;
                if constexpr (value_inline) {
                    std::memcpy(&v, &data[i], sizeof(stored_value_type));
                } else {
                    v = static_cast<stored_value_type>(data[i]);
                }
                entries[i] = {prefix_shifted | static_cast<uint16_t>(keys[i]), v};
            }
        } else if (leaf_bits <= 30) {
            const int32_t* keys = list4_keys(node, skip);
            for (size_t i = 0; i < count; ++i) {
                stored_value_type v;
                if constexpr (value_inline) {
                    std::memcpy(&v, &data[i], sizeof(stored_value_type));
                } else {
                    v = static_cast<stored_value_type>(data[i]);
                }
                entries[i] = {prefix_shifted | static_cast<uint32_t>(keys[i]), v};
            }
        } else {
            const int64_t* keys = list8_keys(node, skip);
            for (size_t i = 0; i < count; ++i) {
                stored_value_type v;
                if constexpr (value_inline) {
                    std::memcpy(&v, &data[i], sizeof(stored_value_type));
                } else {
                    v = static_cast<stored_value_type>(data[i]);
                }
                entries[i] = {prefix_shifted | static_cast<uint64_t>(keys[i]), v};
            }
        }
        entries[count] = {prefix_shifted | new_leaf_key, value};
        
        dealloc_node(node, list_leaf_size_u64(leaf_bits, count));
        
        size_t full_bits = skip * 6 + leaf_bits;
        uint64_t new_ptr = create_child_from_entries(entries, total, full_bits);
        return {new_ptr, true};
    }
    
    uint64_t create_child_from_entries(void* entries_ptr, size_t count, size_t bits_remaining) {
        struct Entry { uint64_t key; stored_value_type val; };
        Entry* entries = static_cast<Entry*>(entries_ptr);
        
        // bits_remaining == 6 → always BITMASK with all terminal entries
        if (bits_remaining == 6) {
            uint64_t bitmap = 0;
            for (size_t i = 0; i < count; ++i) {
                uint8_t idx = entries[i].key & 0x3F;
                bitmap |= (1ULL << idx);
            }
            
            size_t num_slots = std::popcount(bitmap);
            uint64_t* node = alloc_node(bitmask_size_u64(num_slots, 0));
            NodeHeader* h = get_header(node);
            h->skip = 0;
            h->leaf_bits = 6;
            h->count = static_cast<uint8_t>(num_slots);
            h->descendants = static_cast<uint32_t>(count);  // descendants is actual entry count
            h->prefix = 0;
            set_bitmap(node, 0, bitmap);
            // This is a LEAF node (all entries are values)
            
            uint64_t* data = get_bitmask_data(node, 0);
            for (size_t i = 0; i < count; ++i) {
                uint8_t idx = entries[i].key & 0x3F;
                int slot = calc_slot(bitmap, idx);
                if constexpr (value_inline) {
                    std::memcpy(&data[slot], &entries[i].val, sizeof(stored_value_type));
                } else {
                    data[slot] = static_cast<uint64_t>(entries[i].val);
                }
            }
            
            return make_bitmask_leaf(node);
        }
        
        // Try to fit in a LIST-LEAF based on bits_remaining and count
        // LIST2: leaf_bits=12, max 32 entries
        // LIST4: leaf_bits=18,24,30, max 16 entries  
        // LIST8: leaf_bits=36,42,48,54,60, max 8 entries
        
        if (bits_remaining == 12 && count <= LIST2_SLOTS) {
            uint64_t* node = alloc_node(list_size_u64(12, count));
            NodeHeader* h = get_header(node);
            h->skip = 0;
            h->leaf_bits = 12;
            h->count = static_cast<uint8_t>(count);
            h->descendants = h->count;
            h->prefix = 0;
            
            // This is a LEAF node (all entries are values)
            
            int16_t* keys = list2_keys(node, 0);
            uint64_t* data = list_data(node, 12, count);
            for (size_t i = 0; i < count; ++i) {
                keys[i] = static_cast<int16_t>(entries[i].key);
                if constexpr (value_inline) {
                    std::memcpy(&data[i], &entries[i].val, sizeof(stored_value_type));
                } else {
                    data[i] = static_cast<uint64_t>(entries[i].val);
                }
            }
            return make_list_leaf(node);
        }
        
        if (bits_remaining >= 18 && bits_remaining <= 30 && count <= LIST4_SLOTS) {
            uint8_t lb = static_cast<uint8_t>(bits_remaining);
            uint64_t* node = alloc_node(list_size_u64(lb, count));
            NodeHeader* h = get_header(node);
            h->skip = 0;
            h->leaf_bits = lb;
            h->count = static_cast<uint8_t>(count);
            h->descendants = h->count;
            h->prefix = 0;
            
            // This is a LEAF node (all entries are values)
            
            uint64_t mask = (1ULL << bits_remaining) - 1;
            int32_t* keys = list4_keys(node, 0);
            uint64_t* data = list_data(node, lb, count);
            for (size_t i = 0; i < count; ++i) {
                keys[i] = static_cast<int32_t>(entries[i].key & mask);
                if constexpr (value_inline) {
                    std::memcpy(&data[i], &entries[i].val, sizeof(stored_value_type));
                } else {
                    data[i] = static_cast<uint64_t>(entries[i].val);
                }
            }
            return make_list_leaf(node);
        }
        
        if (bits_remaining >= 36 && bits_remaining <= 60 && count <= LIST8_SLOTS) {
            uint8_t lb = static_cast<uint8_t>(bits_remaining);
            uint64_t* node = alloc_node(list_size_u64(lb, count));
            NodeHeader* h = get_header(node);
            h->skip = 0;
            h->leaf_bits = lb;
            h->count = static_cast<uint8_t>(count);
            h->descendants = h->count;
            h->prefix = 0;
            
            // This is a LEAF node (all entries are values)
            
            uint64_t mask = (1ULL << bits_remaining) - 1;
            int64_t* keys = list8_keys(node, 0);
            uint64_t* data = list_data(node, lb, count);
            for (size_t i = 0; i < count; ++i) {
                keys[i] = static_cast<int64_t>(entries[i].key & mask);
                if constexpr (value_inline) {
                    std::memcpy(&data[i], &entries[i].val, sizeof(stored_value_type));
                } else {
                    data[i] = static_cast<uint64_t>(entries[i].val);
                }
            }
            return make_list_leaf(node);
        }
        
        // Need to split - try LIST-INTERNAL before BITMASK-INTERNAL
        // For internal nodes, always use BITMASK (not LIST-INTERNAL)
        // LIST nodes should only be leaf nodes with terminal values
        
        // Fall through to BITMASK-INTERNAL creation
        if (bits_remaining > 18) {
            for (size_t leaf_bits = std::min(size_t{30}, bits_remaining - 6); leaf_bits >= 18; leaf_bits -= 6) {
                size_t child_bits = bits_remaining - leaf_bits;
                if (child_bits < 6) continue;
                
                uint64_t key_shift = child_bits;
                uint64_t key_mask = (1ULL << leaf_bits) - 1;
                
                uint32_t unique_keys[LIST4_SLOTS];
                size_t num_unique = 0;
                bool fits = true;
                
                for (size_t i = 0; i < count && fits; ++i) {
                    uint32_t key_prefix = static_cast<uint32_t>((entries[i].key >> key_shift) & key_mask);
                    bool found = false;
                    for (size_t j = 0; j < num_unique; ++j) {
                        if (unique_keys[j] == key_prefix) { found = true; break; }
                    }
                    if (!found) {
                        if (num_unique >= LIST4_SLOTS) { fits = false; break; }
                        unique_keys[num_unique++] = key_prefix;
                    }
                }
                
                if (fits && num_unique > 0) {
                    uint8_t lb = static_cast<uint8_t>(leaf_bits);
                    uint64_t* node = alloc_node(list_size_u64(lb, num_unique));
                    NodeHeader* h = get_header(node);
                    h->skip = 0;
                    h->leaf_bits = lb;
                    h->count = static_cast<uint8_t>(num_unique);
                    h->descendants = static_cast<uint32_t>(count);
                    h->prefix = 0;
                    
                    // This is an INTERNAL node (all entries are children)
                    
                    int32_t* keys = list4_keys(node, 0);
                    uint64_t* children = list_children(node, lb, num_unique);
                    uint64_t child_mask = (1ULL << child_bits) - 1;
                    
                    // Count entries per unique key
                    size_t key_counts[LIST_MAX_SLOTS] = {0};
                    for (size_t i = 0; i < count; ++i) {
                        uint32_t key_prefix = static_cast<uint32_t>((entries[i].key >> key_shift) & key_mask);
                        for (size_t k = 0; k < num_unique; ++k) {
                            if (key_prefix == unique_keys[k]) { key_counts[k]++; break; }
                        }
                    }
                    
                    for (size_t k = 0; k < num_unique; ++k) {
                        keys[k] = static_cast<int32_t>(unique_keys[k]);
                        
                        Entry* child_entries = static_cast<Entry*>(alloca(key_counts[k] * sizeof(Entry)));
                        size_t ce_count = 0;
                        for (size_t i = 0; i < count; ++i) {
                            uint32_t key_prefix = static_cast<uint32_t>((entries[i].key >> key_shift) & key_mask);
                            if (key_prefix == unique_keys[k]) {
                                child_entries[ce_count++] = {entries[i].key & child_mask, entries[i].val};
                            }
                        }
                        
                        uint64_t child_ptr = create_child_from_entries(child_entries, ce_count, child_bits);
                        children[k] = child_ptr;
                        add_child_stats(h, get_header(get_addr(child_ptr)));
                    }
                    
                    return make_list_internal(node);
                }
            }
        }
        
        // Try LIST2-INTERNAL (32 slots, 12 bit keys)
        if (bits_remaining > 12) {
            size_t leaf_bits = 12;
            size_t child_bits = bits_remaining - leaf_bits;
            
            uint64_t key_shift = child_bits;
            uint16_t unique_keys[LIST2_SLOTS];
            size_t num_unique = 0;
            bool fits = true;
            
            for (size_t i = 0; i < count && fits; ++i) {
                uint16_t key_prefix = static_cast<uint16_t>((entries[i].key >> key_shift) & 0xFFF);
                bool found = false;
                for (size_t j = 0; j < num_unique; ++j) {
                    if (unique_keys[j] == key_prefix) { found = true; break; }
                }
                if (!found) {
                    if (num_unique >= LIST2_SLOTS) { fits = false; break; }
                    unique_keys[num_unique++] = key_prefix;
                }
            }
            
            if (fits && num_unique > 0) {
                uint64_t* node = alloc_node(list_size_u64(12, num_unique));
                NodeHeader* h = get_header(node);
                h->skip = 0;
                h->leaf_bits = 12;
                h->count = static_cast<uint8_t>(num_unique);
                h->descendants = static_cast<uint32_t>(count);
                h->prefix = 0;
                
                // This is an INTERNAL node (all entries are children)
                
                int16_t* keys = list2_keys(node, 0);
                uint64_t* children = list_children(node, 12, num_unique);
                uint64_t child_mask = (1ULL << child_bits) - 1;
                
                // Count entries per unique key
                size_t key_counts[LIST_MAX_SLOTS] = {0};
                for (size_t i = 0; i < count; ++i) {
                    uint16_t key_prefix = static_cast<uint16_t>((entries[i].key >> key_shift) & 0xFFF);
                    for (size_t k = 0; k < num_unique; ++k) {
                        if (key_prefix == unique_keys[k]) { key_counts[k]++; break; }
                    }
                }
                
                for (size_t k = 0; k < num_unique; ++k) {
                    keys[k] = static_cast<int16_t>(unique_keys[k]);
                    
                    Entry* child_entries = static_cast<Entry*>(alloca(key_counts[k] * sizeof(Entry)));
                    size_t ce_count = 0;
                    for (size_t i = 0; i < count; ++i) {
                        uint16_t key_prefix = static_cast<uint16_t>((entries[i].key >> key_shift) & 0xFFF);
                        if (key_prefix == unique_keys[k]) {
                            child_entries[ce_count++] = {entries[i].key & child_mask, entries[i].val};
                        }
                    }
                    
                    uint64_t child_ptr = create_child_from_entries(child_entries, ce_count, child_bits);
                    children[k] = child_ptr;
                    add_child_stats(h, get_header(get_addr(child_ptr)));
                }
                
                return make_list_internal(node);
            }
        }
        
        // Fall back to BITMASK-INTERNAL (6 bit dispatch)
        // First check for skip compression: all entries same bucket
        size_t bucket_counts[64] = {0};
        size_t index_shift = bits_remaining - 6;
        
        for (size_t i = 0; i < count; ++i) {
            uint8_t idx = (entries[i].key >> index_shift) & 0x3F;
            bucket_counts[idx]++;
        }
        
        size_t num_children = 0;
        uint64_t bitmap = 0;
        int single_bucket = -1;
        for (size_t i = 0; i < 64; ++i) {
            if (bucket_counts[i] > 0) {
                num_children++;
                bitmap |= (1ULL << i);
                single_bucket = i;
            }
        }
        
        // Skip compression: if all entries go to same bucket, accumulate skip
        if (num_children == 1 && bits_remaining > 6) {
            uint64_t child_mask = (1ULL << index_shift) - 1;
            Entry* child_entries = static_cast<Entry*>(alloca(count * sizeof(Entry)));
            for (size_t i = 0; i < count; ++i) {
                child_entries[i] = {entries[i].key & child_mask, entries[i].val};
            }
            
            // Recursively create child, then wrap with skip
            uint64_t child_ptr = create_child_from_entries(child_entries, count, bits_remaining - 6);
            
            // Get child's skip to combine
            uint64_t* child_node = get_addr(child_ptr);
            NodeHeader* child_h = get_header(child_node);
            uint8_t child_skip = child_h->skip;
            
            // Create new node with combined skip
            uint8_t new_skip = child_skip + 1;
            uint64_t new_prefix = (static_cast<uint64_t>(single_bucket) << (child_skip * 6)) | child_h->prefix;
            
            // Just update the child's header in place
            child_h->skip = new_skip;
            child_h->prefix = new_prefix;
            
            return child_ptr;
        }
        
        uint64_t* node = alloc_node(bitmask_size_u64(num_children, 0));
        NodeHeader* h = get_header(node);
        h->skip = 0;
        h->leaf_bits = 6;
        h->count = static_cast<uint8_t>(num_children);
        h->descendants = static_cast<uint32_t>(count);
        h->prefix = 0;
        set_bitmap(node, 0, bitmap);
        // This is an INTERNAL node (all entries are children)
        
        uint64_t* children = get_bitmask_data(node, 0);
        size_t child_idx = 0;
        uint64_t child_mask = (1ULL << index_shift) - 1;
        
        for (uint8_t bucket = 0; bucket < 64; ++bucket) {
            if (bucket_counts[bucket] == 0) continue;
            
            // Allocate based on actual bucket count
            Entry* bucket_entries = static_cast<Entry*>(alloca(bucket_counts[bucket] * sizeof(Entry)));
            size_t bc = 0;
            for (size_t i = 0; i < count; ++i) {
                uint8_t idx = (entries[i].key >> index_shift) & 0x3F;
                if (idx == bucket) {
                    bucket_entries[bc++] = {entries[i].key & child_mask, entries[i].val};
                }
            }
            
            uint64_t child_ptr = create_child_from_entries(bucket_entries, bc, bits_remaining - 6);
            children[child_idx++] = child_ptr;
            
            // Add child's stats to parent
            add_child_stats(h, get_header(get_addr(child_ptr)));
        }
        
        return make_bitmask_internal(node);
    }
};

} // namespace kn

#endif // KNTRIE_HPP
