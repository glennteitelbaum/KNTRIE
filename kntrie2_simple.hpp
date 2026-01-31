#ifndef KNTRIE2_HPP
#define KNTRIE2_HPP

#include <cstdint>
#include <cstring>
#include <bit>
#include <memory>
#include <type_traits>
#include <utility>

namespace kn {

template<typename KEY, typename VALUE, typename ALLOC = std::allocator<uint64_t>>
class kntrie {
    static_assert(std::is_integral_v<KEY>, "KEY must be integral");
    static_assert(sizeof(KEY) == 4 || sizeof(KEY) == 8, "KEY must be 32 or 64 bits");
    
public:
    using key_type = KEY;
    using mapped_type = VALUE;
    using size_type = std::size_t;
    using allocator_type = ALLOC;
    
private:
    static constexpr bool is_signed_key = std::is_signed_v<KEY>;
    static constexpr size_t key_bits = sizeof(KEY) * 8;
    
    // Node types - each handles 16 bits of key space
    static constexpr uint8_t TYPE_LIST16 = 0;  // terminal
    static constexpr uint8_t TYPE_LIST32 = 1;
    static constexpr uint8_t TYPE_LIST48 = 2;
    static constexpr uint8_t TYPE_LIST64 = 3;
    
    static constexpr uint8_t FLAG_LEAF = 0x80;
    
    static constexpr bool value_inline = sizeof(VALUE) <= 8 && std::is_trivially_copyable_v<VALUE>;
    using stored_value_type = std::conditional_t<value_inline, VALUE, uint64_t>;
    using value_alloc_type = typename std::allocator_traits<ALLOC>::template rebind_alloc<VALUE>;
    
    // =========================================================================
    // NodeHeader - 16 bytes
    // =========================================================================
    
    struct NodeHeader {
        uint64_t prefix;     // skip chunks packed (16-bit each)
        uint32_t count;      // entries
        uint8_t skip;        // 16-bit chunks skipped
        uint8_t type_flags;  // type + leaf flag
        uint16_t _pad;
        
        uint8_t type() const noexcept { return type_flags & 0x03; }
        bool is_leaf() const noexcept { return type_flags & FLAG_LEAF; }
    };
    
    static_assert(sizeof(NodeHeader) == 16);
    static constexpr size_t HEADER_U64 = 2;
    
    // =========================================================================
    // Key accessors - width based on type
    // LIST16: 16-bit keys
    // LIST32: 32-bit keys  
    // LIST48: 64-bit keys (48 bits used)
    // LIST64: 64-bit keys
    // =========================================================================
    
    static NodeHeader* get_header(uint64_t* node) noexcept {
        return reinterpret_cast<NodeHeader*>(node);
    }
    static const NodeHeader* get_header(const uint64_t* node) noexcept {
        return reinterpret_cast<const NodeHeader*>(node);
    }
    
    // 16-bit keys (LIST16)
    static uint16_t* keys16(uint64_t* node) noexcept {
        return reinterpret_cast<uint16_t*>(node + HEADER_U64);
    }
    static const uint16_t* keys16(const uint64_t* node) noexcept {
        return reinterpret_cast<const uint16_t*>(node + HEADER_U64);
    }
    static uint64_t* data16(uint64_t* node, size_t count) noexcept {
        return node + HEADER_U64 + ((count * 2 + 7) >> 3);
    }
    static const uint64_t* data16(const uint64_t* node, size_t count) noexcept {
        return node + HEADER_U64 + ((count * 2 + 7) >> 3);
    }
    
    // 32-bit keys (LIST32)
    static uint32_t* keys32(uint64_t* node) noexcept {
        return reinterpret_cast<uint32_t*>(node + HEADER_U64);
    }
    static const uint32_t* keys32(const uint64_t* node) noexcept {
        return reinterpret_cast<const uint32_t*>(node + HEADER_U64);
    }
    static uint64_t* data32(uint64_t* node, size_t count) noexcept {
        return node + HEADER_U64 + ((count + 1) >> 1);
    }
    static const uint64_t* data32(const uint64_t* node, size_t count) noexcept {
        return node + HEADER_U64 + ((count + 1) >> 1);
    }
    
    // 64-bit keys (LIST48, LIST64)
    static uint64_t* keys64(uint64_t* node) noexcept {
        return node + HEADER_U64;
    }
    static const uint64_t* keys64(const uint64_t* node) noexcept {
        return node + HEADER_U64;
    }
    static uint64_t* data64(uint64_t* node, size_t count) noexcept {
        return node + HEADER_U64 + count;
    }
    static const uint64_t* data64(const uint64_t* node, size_t count) noexcept {
        return node + HEADER_U64 + count;
    }
    
    // =========================================================================
    // Node sizes (in uint64_t units)
    // =========================================================================
    
    static constexpr size_t size16_u64(size_t count) noexcept {
        return HEADER_U64 + ((count * 2 + 7) >> 3) + count;
    }
    static constexpr size_t size32_u64(size_t count) noexcept {
        return HEADER_U64 + ((count + 1) >> 1) + count;
    }
    static constexpr size_t size64_u64(size_t count) noexcept {
        return HEADER_U64 + count + count;
    }
    
    static constexpr size_t node_size(uint8_t type, size_t count) noexcept {
        if (type == TYPE_LIST16) return size16_u64(count);
        if (type == TYPE_LIST32) return size32_u64(count);
        return size64_u64(count);
    }
    
    // =========================================================================
    // Key conversion
    // =========================================================================
    
    static constexpr uint64_t key_to_internal(KEY k) noexcept {
        uint64_t result = (sizeof(KEY) == 4) ? static_cast<uint32_t>(k) : static_cast<uint64_t>(k);
        if constexpr (is_signed_key) result ^= 1ULL << (key_bits - 1);
        return result << (64 - key_bits);
    }
    
    static constexpr KEY internal_to_key(uint64_t internal) noexcept {
        internal >>= (64 - key_bits);
        if constexpr (is_signed_key) internal ^= 1ULL << (key_bits - 1);
        return static_cast<KEY>(internal);
    }
    
    static constexpr uint16_t chunk16(uint64_t k, size_t idx) noexcept {
        return static_cast<uint16_t>((k >> ((3 - idx) * 16)) & 0xFFFF);
    }
    
    static constexpr uint64_t extract_prefix(uint64_t k, size_t chunk_idx, size_t skip) noexcept {
        uint64_t prefix = 0;
        for (size_t i = 0; i < skip; ++i)
            prefix = (prefix << 16) | chunk16(k, chunk_idx + i);
        return prefix;
    }
    
    // Key extraction for each type
    static constexpr uint16_t key_list16(uint64_t k, size_t chunk_idx) noexcept {
        return chunk16(k, chunk_idx);
    }
    
    static constexpr uint32_t key_list32(uint64_t k, size_t chunk_idx) noexcept {
        return (static_cast<uint32_t>(chunk16(k, chunk_idx)) << 16) | chunk16(k, chunk_idx + 1);
    }
    
    static constexpr uint64_t key_list48(uint64_t k, size_t chunk_idx) noexcept {
        return ((static_cast<uint64_t>(chunk16(k, chunk_idx)) << 32) |
                (static_cast<uint64_t>(chunk16(k, chunk_idx + 1)) << 16) |
                 chunk16(k, chunk_idx + 2)) & ((1ULL << 48) - 1);
    }
    
    static constexpr uint64_t key_list64(uint64_t k) noexcept {
        return k;
    }
    
    // Chunks consumed by each type
    static constexpr size_t chunks_for_type(uint8_t type) noexcept {
        if (type == TYPE_LIST16) return 1;
        if (type == TYPE_LIST32) return 2;
        if (type == TYPE_LIST48) return 3;
        return 4;
    }
    
    // Child type for each type
    static constexpr uint8_t child_type(uint8_t type) noexcept {
        if (type == TYPE_LIST64) return TYPE_LIST48;
        if (type == TYPE_LIST48) return TYPE_LIST32;
        if (type == TYPE_LIST32) return TYPE_LIST16;
        return TYPE_LIST16;  // LIST16 has no children
    }
    
    // =========================================================================
    // Allocation and values
    // =========================================================================
    
    uint64_t* root_ = nullptr;
    size_t size_ = 0;
    [[no_unique_address]] ALLOC alloc_;
    
    uint64_t* alloc_node(size_t n) { return alloc_.allocate(n); }
    void dealloc_node(uint64_t* p, size_t n) noexcept { alloc_.deallocate(p, n); }
    
    stored_value_type store_value(const VALUE& v) {
        if constexpr (value_inline) return v;
        else {
            value_alloc_type va(alloc_);
            VALUE* p = std::allocator_traits<value_alloc_type>::allocate(va, 1);
            std::allocator_traits<value_alloc_type>::construct(va, p, v);
            return reinterpret_cast<uint64_t>(p);
        }
    }
    
    VALUE load_value(stored_value_type s) const noexcept {
        if constexpr (value_inline) return s;
        else return *reinterpret_cast<VALUE*>(s);
    }
    
    const VALUE* value_ptr(const uint64_t* slot) const noexcept {
        if constexpr (value_inline) return reinterpret_cast<const VALUE*>(slot);
        else return reinterpret_cast<VALUE*>(*slot);
    }
    
    void destroy_value(stored_value_type s) noexcept {
        if constexpr (!value_inline) {
            value_alloc_type va(alloc_);
            VALUE* p = reinterpret_cast<VALUE*>(s);
            std::allocator_traits<value_alloc_type>::destroy(va, p);
            std::allocator_traits<value_alloc_type>::deallocate(va, p, 1);
        }
    }
    
    // =========================================================================
    // Find implementation
    // =========================================================================
    
    const VALUE* find_in_node(const uint64_t* node, uint64_t key, size_t chunk_idx) const noexcept {
        if (!node) return nullptr;
        
        const NodeHeader* h = get_header(node);
        uint8_t type = h->type();
        size_t count = h->count;
        bool leaf = h->is_leaf();
        
        // Check skip prefix
        if (h->skip > 0) {
            if (extract_prefix(key, chunk_idx, h->skip) != h->prefix) return nullptr;
            chunk_idx += h->skip;
        }
        
        if (type == TYPE_LIST16) {
            uint16_t target = key_list16(key, chunk_idx);
            const uint16_t* k = keys16(node);
            const uint64_t* d = data16(node, count);
            for (size_t i = 0; i < count; ++i) {
                if (k[i] == target) return value_ptr(&d[i]);
            }
        } else if (type == TYPE_LIST32) {
            uint32_t target = key_list32(key, chunk_idx);
            const uint32_t* k = keys32(node);
            const uint64_t* d = data32(node, count);
            for (size_t i = 0; i < count; ++i) {
                if (k[i] == target) {
                    if (leaf) return value_ptr(&d[i]);
                    return find_in_node(reinterpret_cast<const uint64_t*>(d[i]), key, chunk_idx + 2);
                }
            }
        } else if (type == TYPE_LIST48) {
            uint64_t target = key_list48(key, chunk_idx);
            const uint64_t* k = keys64(node);
            const uint64_t* d = data64(node, count);
            constexpr uint64_t MASK48 = (1ULL << 48) - 1;
            for (size_t i = 0; i < count; ++i) {
                if ((k[i] & MASK48) == target) {
                    if (leaf) return value_ptr(&d[i]);
                    return find_in_node(reinterpret_cast<const uint64_t*>(d[i]), key, chunk_idx + 3);
                }
            }
        } else { // LIST64
            const uint64_t* k = keys64(node);
            const uint64_t* d = data64(node, count);
            for (size_t i = 0; i < count; ++i) {
                if (k[i] == key) {
                    if (leaf) return value_ptr(&d[i]);
                    return find_in_node(reinterpret_cast<const uint64_t*>(d[i]), key, chunk_idx + 4);
                }
            }
        }
        return nullptr;
    }
    
    // =========================================================================
    // Insert implementation
    // =========================================================================
    
    struct InsertResult { uint64_t* node; bool inserted; stored_value_type old_value; };
    
    // Create leaf with single entry
    uint64_t* create_leaf(uint8_t type, uint64_t key, size_t chunk_idx, stored_value_type val,
                          uint8_t skip = 0, uint64_t prefix = 0) {
        size_t sz = node_size(type, 1);
        uint64_t* node = alloc_node(sz);
        NodeHeader* h = get_header(node);
        h->prefix = prefix;
        h->count = 1;
        h->skip = skip;
        h->type_flags = type | FLAG_LEAF;
        h->_pad = 0;
        
        if (type == TYPE_LIST16) {
            keys16(node)[0] = key_list16(key, chunk_idx);
            std::memcpy(&data16(node, 1)[0], &val, sizeof(stored_value_type));
        } else if (type == TYPE_LIST32) {
            keys32(node)[0] = key_list32(key, chunk_idx);
            std::memcpy(&data32(node, 1)[0], &val, sizeof(stored_value_type));
        } else if (type == TYPE_LIST48) {
            keys64(node)[0] = key_list48(key, chunk_idx);
            std::memcpy(&data64(node, 1)[0], &val, sizeof(stored_value_type));
        } else {
            keys64(node)[0] = key;
            std::memcpy(&data64(node, 1)[0], &val, sizeof(stored_value_type));
        }
        return node;
    }
    
    InsertResult insert_impl(uint64_t* node, uint64_t key, stored_value_type val, size_t chunk_idx) {
        if (!node) {
            // Shouldn't happen if called correctly
            return {nullptr, false, {}};
        }
        
        NodeHeader* h = get_header(node);
        uint8_t type = h->type();
        size_t count = h->count;
        bool leaf = h->is_leaf();
        uint8_t skip = h->skip;
        
        // Check skip prefix
        if (skip > 0) {
            uint64_t expected = extract_prefix(key, chunk_idx, skip);
            if (expected != h->prefix) {
                // Prefix mismatch - find divergence point
                size_t common = 0;
                for (size_t i = 0; i < skip; ++i) {
                    uint16_t old_c = (h->prefix >> ((skip - 1 - i) * 16)) & 0xFFFF;
                    uint16_t new_c = (expected >> ((skip - 1 - i) * 16)) & 0xFFFF;
                    if (old_c != new_c) break;
                    common++;
                }
                
                uint16_t old_div = (h->prefix >> ((skip - 1 - common) * 16)) & 0xFFFF;
                uint16_t new_div = (expected >> ((skip - 1 - common) * 16)) & 0xFFFF;
                
                // Create parent at divergence (LIST16 internal with 16-bit dispatch keys)
                uint64_t* parent = alloc_node(size16_u64(2));
                NodeHeader* ph = get_header(parent);
                ph->count = 2;
                ph->skip = static_cast<uint8_t>(common);
                ph->type_flags = TYPE_LIST16;  // internal
                ph->_pad = 0;
                ph->prefix = common > 0 ? (expected >> ((skip - common) * 16)) : 0;
                
                // Adjust old node's skip
                size_t rem = skip - common - 1;
                h->skip = static_cast<uint8_t>(rem);
                h->prefix = rem > 0 ? (h->prefix & ((1ULL << (rem * 16)) - 1)) : 0;
                
                // Create new leaf
                size_t new_chunk = chunk_idx + common + 1;
                uint8_t new_type = (new_chunk >= 3) ? TYPE_LIST16 : 
                                   (new_chunk >= 2) ? TYPE_LIST32 :
                                   (new_chunk >= 1) ? TYPE_LIST48 : TYPE_LIST64;
                uint64_t* newl = create_leaf(new_type, key, new_chunk, val);
                
                uint16_t* pk = keys16(parent);
                uint64_t* pd = data16(parent, 2);
                if (old_div < new_div) {
                    pk[0] = old_div; pd[0] = reinterpret_cast<uint64_t>(node);
                    pk[1] = new_div; pd[1] = reinterpret_cast<uint64_t>(newl);
                } else {
                    pk[0] = new_div; pd[0] = reinterpret_cast<uint64_t>(newl);
                    pk[1] = old_div; pd[1] = reinterpret_cast<uint64_t>(node);
                }
                return {parent, true, {}};
            }
            chunk_idx += skip;
        }
        
        // Insert based on type
        if (type == TYPE_LIST16) {
            uint16_t target = key_list16(key, chunk_idx);
            uint16_t* k = keys16(node);
            uint64_t* d = data16(node, count);
            
            for (size_t i = 0; i < count; ++i) {
                if (k[i] == target) {
                    // Exists - update
                    stored_value_type old = static_cast<stored_value_type>(d[i]);
                    std::memcpy(&d[i], &val, sizeof(stored_value_type));
                    return {node, false, old};
                }
            }
            
            // Add new entry
            
            size_t nc = count + 1;
            uint64_t* nn = alloc_node(size16_u64(nc));
            NodeHeader* nh = get_header(nn);
            nh->prefix = h->prefix; nh->count = nc; nh->skip = h->skip;
            nh->type_flags = h->type_flags; nh->_pad = 0;
            std::memcpy(keys16(nn), k, count * 2);
            std::memcpy(data16(nn, nc), d, count * 8);
            keys16(nn)[count] = target;
            std::memcpy(&data16(nn, nc)[count], &val, sizeof(stored_value_type));
            dealloc_node(node, size16_u64(count));
            return {nn, true, {}};
            
        } else if (type == TYPE_LIST32) {
            uint32_t target = key_list32(key, chunk_idx);
            uint32_t* k = keys32(node);
            uint64_t* d = data32(node, count);
            
            for (size_t i = 0; i < count; ++i) {
                if (k[i] == target) {
                    if (leaf) {
                        stored_value_type old = static_cast<stored_value_type>(d[i]);
                        std::memcpy(&d[i], &val, sizeof(stored_value_type));
                        return {node, false, old};
                    }
                    // Recurse
                    InsertResult r = insert_impl(reinterpret_cast<uint64_t*>(d[i]), key, val, chunk_idx + 2);
                    d[i] = reinterpret_cast<uint64_t>(r.node);
                    return {node, r.inserted, r.old_value};
                }
            }
            
            
            size_t nc = count + 1;
            uint64_t* nn = alloc_node(size32_u64(nc));
            NodeHeader* nh = get_header(nn);
            nh->prefix = h->prefix; nh->count = nc; nh->skip = h->skip;
            nh->type_flags = h->type_flags; nh->_pad = 0;
            std::memcpy(keys32(nn), k, count * 4);
            std::memcpy(data32(nn, nc), d, count * 8);
            keys32(nn)[count] = target;
            
            if (leaf) {
                std::memcpy(&data32(nn, nc)[count], &val, sizeof(stored_value_type));
            } else {
                uint64_t* ch = create_leaf(child_type(type), key, chunk_idx + 2, val);
                data32(nn, nc)[count] = reinterpret_cast<uint64_t>(ch);
            }
            dealloc_node(node, size32_u64(count));
            return {nn, true, {}};
            
        } else if (type == TYPE_LIST48) {
            uint64_t target = key_list48(key, chunk_idx);
            uint64_t* k = keys64(node);
            uint64_t* d = data64(node, count);
            constexpr uint64_t MASK48 = (1ULL << 48) - 1;
            
            for (size_t i = 0; i < count; ++i) {
                if ((k[i] & MASK48) == target) {
                    if (leaf) {
                        stored_value_type old = static_cast<stored_value_type>(d[i]);
                        std::memcpy(&d[i], &val, sizeof(stored_value_type));
                        return {node, false, old};
                    }
                    InsertResult r = insert_impl(reinterpret_cast<uint64_t*>(d[i]), key, val, chunk_idx + 3);
                    d[i] = reinterpret_cast<uint64_t>(r.node);
                    return {node, r.inserted, r.old_value};
                }
            }
            
            
            size_t nc = count + 1;
            uint64_t* nn = alloc_node(size64_u64(nc));
            NodeHeader* nh = get_header(nn);
            nh->prefix = h->prefix; nh->count = nc; nh->skip = h->skip;
            nh->type_flags = h->type_flags; nh->_pad = 0;
            std::memcpy(keys64(nn), k, count * 8);
            std::memcpy(data64(nn, nc), d, count * 8);
            keys64(nn)[count] = target;
            
            if (leaf) {
                std::memcpy(&data64(nn, nc)[count], &val, sizeof(stored_value_type));
            } else {
                uint64_t* ch = create_leaf(child_type(type), key, chunk_idx + 3, val);
                data64(nn, nc)[count] = reinterpret_cast<uint64_t>(ch);
            }
            dealloc_node(node, size64_u64(count));
            return {nn, true, {}};
            
        } else { // LIST64
            uint64_t* k = keys64(node);
            uint64_t* d = data64(node, count);
            
            for (size_t i = 0; i < count; ++i) {
                if (k[i] == key) {
                    if (leaf) {
                        stored_value_type old = static_cast<stored_value_type>(d[i]);
                        std::memcpy(&d[i], &val, sizeof(stored_value_type));
                        return {node, false, old};
                    }
                    InsertResult r = insert_impl(reinterpret_cast<uint64_t*>(d[i]), key, val, chunk_idx + 4);
                    d[i] = reinterpret_cast<uint64_t>(r.node);
                    return {node, r.inserted, r.old_value};
                }
            }
            
            
            size_t nc = count + 1;
            uint64_t* nn = alloc_node(size64_u64(nc));
            NodeHeader* nh = get_header(nn);
            nh->prefix = h->prefix; nh->count = nc; nh->skip = h->skip;
            nh->type_flags = h->type_flags; nh->_pad = 0;
            std::memcpy(keys64(nn), k, count * 8);
            std::memcpy(data64(nn, nc), d, count * 8);
            keys64(nn)[count] = key;
            
            if (leaf) {
                std::memcpy(&data64(nn, nc)[count], &val, sizeof(stored_value_type));
            } else {
                uint64_t* ch = create_leaf(child_type(type), key, chunk_idx + 4, val);
                data64(nn, nc)[count] = reinterpret_cast<uint64_t>(ch);
            }
            dealloc_node(node, size64_u64(count));
            return {nn, true, {}};
        }
    }
    
    // =========================================================================
    // Clear
    // =========================================================================
    
    void clear_node(uint64_t* node) noexcept {
        if (!node) return;
        const NodeHeader* h = get_header(node);
        uint8_t type = h->type();
        size_t count = h->count;
        bool leaf = h->is_leaf();
        
        if (type == TYPE_LIST16) {
            const uint64_t* d = data16(node, count);
            if (leaf) {
                if constexpr (!value_inline) 
                    for (size_t i = 0; i < count; ++i) destroy_value(static_cast<stored_value_type>(d[i]));
            } else {
                for (size_t i = 0; i < count; ++i) clear_node(reinterpret_cast<uint64_t*>(d[i]));
            }
            dealloc_node(node, size16_u64(count));
        } else if (type == TYPE_LIST32) {
            const uint64_t* d = data32(node, count);
            if (leaf) {
                if constexpr (!value_inline) 
                    for (size_t i = 0; i < count; ++i) destroy_value(static_cast<stored_value_type>(d[i]));
            } else {
                for (size_t i = 0; i < count; ++i) clear_node(reinterpret_cast<uint64_t*>(d[i]));
            }
            dealloc_node(node, size32_u64(count));
        } else {
            const uint64_t* d = data64(node, count);
            if (leaf) {
                if constexpr (!value_inline) 
                    for (size_t i = 0; i < count; ++i) destroy_value(static_cast<stored_value_type>(d[i]));
            } else {
                for (size_t i = 0; i < count; ++i) clear_node(reinterpret_cast<uint64_t*>(d[i]));
            }
            dealloc_node(node, size64_u64(count));
        }
    }
    
public:
    kntrie() = default;
    explicit kntrie(const ALLOC& a) : alloc_(a) {}
    ~kntrie() { clear_node(root_); }
    
    kntrie(const kntrie&) = delete;
    kntrie& operator=(const kntrie&) = delete;
    kntrie(kntrie&& o) noexcept : root_(o.root_), size_(o.size_), alloc_(std::move(o.alloc_)) {
        o.root_ = nullptr; o.size_ = 0;
    }
    kntrie& operator=(kntrie&& o) noexcept {
        if (this != &o) {
            clear_node(root_);
            root_ = o.root_; size_ = o.size_; alloc_ = std::move(o.alloc_);
            o.root_ = nullptr; o.size_ = 0;
        }
        return *this;
    }
    
    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }
    [[nodiscard]] size_type size() const noexcept { return size_; }
    
    const VALUE* find(const KEY& k) const noexcept {
        return find_in_node(root_, key_to_internal(k), 0);
    }
    
    bool contains(const KEY& k) const noexcept { return find(k) != nullptr; }
    
    std::pair<bool, bool> insert(const KEY& k, const VALUE& v) {
        uint64_t ik = key_to_internal(k);
        stored_value_type sv = store_value(v);
        
        constexpr uint8_t root_type = (sizeof(KEY) == 4) ? TYPE_LIST32 : TYPE_LIST64;
        
        if (!root_) {
            root_ = create_leaf(root_type, ik, 0, sv);
            size_ = 1;
            return {true, true};
        }
        
        InsertResult r = insert_impl(root_, ik, sv, 0);
        root_ = r.node;
        if (r.inserted) { size_++; return {true, true}; }
        destroy_value(sv);
        return {false, false};
    }
    
    void clear() noexcept { clear_node(root_); root_ = nullptr; size_ = 0; }
    
    // Debug stats
    struct DebugStats {
        size_t list16_leaf = 0, list16_internal = 0;
        size_t list32_leaf = 0, list32_internal = 0;
        size_t list48_leaf = 0, list48_internal = 0;
        size_t list64_leaf = 0, list64_internal = 0;
        size_t total_bytes = 0;
    };
    
    void collect_stats(const uint64_t* node, DebugStats& s) const {
        if (!node) return;
        const NodeHeader* h = get_header(node);
        uint8_t type = h->type();
        size_t count = h->count;
        bool leaf = h->is_leaf();
        
        s.total_bytes += node_size(type, count) * 8;
        
        if (type == TYPE_LIST16) {
            if (leaf) s.list16_leaf++; else s.list16_internal++;
            if (!leaf) {
                const uint64_t* d = data16(node, count);
                for (size_t i = 0; i < count; ++i) collect_stats(reinterpret_cast<const uint64_t*>(d[i]), s);
            }
        } else if (type == TYPE_LIST32) {
            if (leaf) s.list32_leaf++; else s.list32_internal++;
            if (!leaf) {
                const uint64_t* d = data32(node, count);
                for (size_t i = 0; i < count; ++i) collect_stats(reinterpret_cast<const uint64_t*>(d[i]), s);
            }
        } else if (type == TYPE_LIST48) {
            if (leaf) s.list48_leaf++; else s.list48_internal++;
            if (!leaf) {
                const uint64_t* d = data64(node, count);
                for (size_t i = 0; i < count; ++i) collect_stats(reinterpret_cast<const uint64_t*>(d[i]), s);
            }
        } else {
            if (leaf) s.list64_leaf++; else s.list64_internal++;
            if (!leaf) {
                const uint64_t* d = data64(node, count);
                for (size_t i = 0; i < count; ++i) collect_stats(reinterpret_cast<const uint64_t*>(d[i]), s);
            }
        }
    }
    
    DebugStats get_stats() const { DebugStats s; collect_stats(root_, s); return s; }
};

} // namespace kn

#endif
