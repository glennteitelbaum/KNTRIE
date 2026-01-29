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

#if defined(__AVX512F__) && defined(__AVX512BW__)
#define KNTRIE_USE_AVX512 1
#include <immintrin.h>
#endif

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
    static constexpr size_t max_depth = (key_bits + 5) / 6;
    static constexpr size_t bits_per_level = 6;
    static constexpr size_t first_level_bits = 6;  // All levels use 6 bits
    static constexpr size_t max_leaf_entries = 64;
    static constexpr bool value_inline = sizeof(VALUE) <= 8 && std::is_trivially_copyable_v<VALUE>;
    
    // What we store in leaf for values: VALUE directly if inline, else pointer
    using stored_value_type = std::conditional_t<value_inline, VALUE, uint64_t>;
    
    static constexpr uint64_t LEAF_TAG = 1ULL << 63;
    static constexpr uint64_t SKIP_SHIFT = 59;
    static constexpr uint64_t SKIP_MASK = 0xFULL << SKIP_SHIFT;
    static constexpr uint64_t ADDR_MASK = (1ULL << 59) - 1;
    
    using value_alloc_type = typename std::allocator_traits<ALLOC>::template rebind_alloc<VALUE>;
    
    uint64_t root_;
    size_t size_;
    [[no_unique_address]] ALLOC alloc_;
    
    static constexpr bool high_bit_set(uint64_t v) noexcept {
        return static_cast<int64_t>(v) < 0;
    }
    
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
    
    static constexpr uint64_t extract_prefix(uint64_t internal_key, size_t shift, size_t skip) noexcept {
        size_t prefix_bits = skip * 6;
        size_t end_shift = shift - prefix_bits + 6;
        uint64_t mask = (1ULL << prefix_bits) - 1;
        return (internal_key >> end_shift) & mask;
    }
    
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
        return node[0];
    }
    
    static uint64_t* get_children(uint64_t* node, uint64_t skip) noexcept {
        return node + children_offset(skip);
    }
    
    static const uint64_t* get_children(const uint64_t* node, uint64_t skip) noexcept {
        return node + children_offset(skip);
    }
    
    // =========================================================================
    // Compact Leaf Layout
    // [uint64_t count][KEY keys...][padding][stored_value_type values...]
    // =========================================================================
    
    static constexpr size_t leaf_keys_offset() noexcept {
        return sizeof(uint64_t); // After count
    }
    
    static constexpr size_t leaf_values_offset(size_t count) noexcept {
        size_t keys_end = sizeof(uint64_t) + sizeof(KEY) * count;
        constexpr size_t val_align = alignof(stored_value_type);
        return (keys_end + val_align - 1) & ~(val_align - 1);
    }
    
    static constexpr size_t leaf_size_bytes(size_t count) noexcept {
        return leaf_values_offset(count) + sizeof(stored_value_type) * count;
    }
    
    static constexpr size_t leaf_size_u64(size_t count) noexcept {
        return (leaf_size_bytes(count) + 7) / 8;
    }
    
    uint64_t* alloc_node(size_t count) {
        return alloc_.allocate(count);
    }
    
    void dealloc_node(uint64_t* node, size_t count) noexcept {
        alloc_.deallocate(node, count);
    }
    
    uint64_t* alloc_leaf(size_t entry_count) {
        size_t node_size = leaf_size_u64(entry_count);
        uint64_t* leaf = alloc_node(node_size);
        leaf[0] = entry_count;
        return leaf;
    }
    
    void dealloc_leaf(uint64_t* leaf) noexcept {
        size_t count = leaf[0];
        size_t node_size = leaf_size_u64(count);
        dealloc_node(leaf, node_size);
    }
    
    uint64_t* alloc_internal(size_t child_count, uint64_t skip) {
        size_t node_size = children_offset(skip) + child_count;
        uint64_t* node = alloc_node(node_size);
        node[bitmap_offset(skip)] = 0;
        return node;
    }
    
    void dealloc_internal(uint64_t* node, uint64_t skip) noexcept {
        uint64_t bitmap = get_bitmap(node, skip);
        size_t child_count = std::popcount(bitmap);
        size_t node_size = children_offset(skip) + child_count;
        dealloc_node(node, node_size);
    }
    
    // =========================================================================
    // Leaf Accessors
    // =========================================================================
    
    static size_t leaf_count(const uint64_t* leaf) noexcept {
        return static_cast<size_t>(leaf[0]);
    }
    
    static KEY* leaf_keys(uint64_t* leaf) noexcept {
        return reinterpret_cast<KEY*>(reinterpret_cast<char*>(leaf) + leaf_keys_offset());
    }
    
    static const KEY* leaf_keys(const uint64_t* leaf) noexcept {
        return reinterpret_cast<const KEY*>(reinterpret_cast<const char*>(leaf) + leaf_keys_offset());
    }
    
    static stored_value_type* leaf_values(uint64_t* leaf, size_t count) noexcept {
        return reinterpret_cast<stored_value_type*>(
            reinterpret_cast<char*>(leaf) + leaf_values_offset(count));
    }
    
    static const stored_value_type* leaf_values(const uint64_t* leaf, size_t count) noexcept {
        return reinterpret_cast<const stored_value_type*>(
            reinterpret_cast<const char*>(leaf) + leaf_values_offset(count));
    }
    
    static stored_value_type* leaf_values(uint64_t* leaf) noexcept {
        return leaf_values(leaf, leaf_count(leaf));
    }
    
    static const stored_value_type* leaf_values(const uint64_t* leaf) noexcept {
        return leaf_values(leaf, leaf_count(leaf));
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
    
    static int calc_slot(uint64_t bitmap, uint8_t index) noexcept {
        uint64_t shifted = bitmap << (63 - index);
        return std::popcount(shifted);
    }
    
    static uint64_t get_child(const uint64_t* node, uint64_t skip, uint8_t index) noexcept {
        uint64_t bitmap = get_bitmap(node, skip);
        uint64_t shifted = bitmap << (63 - index);
        if (!high_bit_set(shifted)) [[unlikely]] return 0;
        int slot = std::popcount(shifted);
        return get_children(node, skip)[slot - 1];
    }
    
    uint64_t* insert_child(uint64_t* node, uint64_t skip, uint8_t index, uint64_t child_ptr) {
        uint64_t bitmap = get_bitmap(node, skip);
        size_t old_count = std::popcount(bitmap);
        size_t new_count = old_count + 1;
        uint64_t new_bitmap = bitmap | (1ULL << index);
        int slot = calc_slot(new_bitmap, index) - 1;
        uint64_t* new_node = alloc_internal(new_count, skip);
        if (skip > 0) new_node[0] = node[0];
        new_node[bitmap_offset(skip)] = new_bitmap;
        const uint64_t* old_children = get_children(node, skip);
        uint64_t* new_children = get_children(new_node, skip);
        for (int i = 0; i < slot; ++i) new_children[i] = old_children[i];
        new_children[slot] = child_ptr;
        for (size_t i = slot; i < old_count; ++i) new_children[i + 1] = old_children[i];
        dealloc_internal(node, skip);
        return new_node;
    }
    
    uint64_t* remove_child(uint64_t* node, uint64_t skip, uint8_t index) {
        uint64_t bitmap = get_bitmap(node, skip);
        size_t old_count = std::popcount(bitmap);
        if (old_count == 1) { dealloc_internal(node, skip); return nullptr; }
        size_t new_count = old_count - 1;
        int slot = calc_slot(bitmap, index) - 1;
        uint64_t new_bitmap = bitmap & ~(1ULL << index);
        uint64_t* new_node = alloc_internal(new_count, skip);
        if (skip > 0) new_node[0] = node[0];
        new_node[bitmap_offset(skip)] = new_bitmap;
        const uint64_t* old_children = get_children(node, skip);
        uint64_t* new_children = get_children(new_node, skip);
        for (int i = 0; i < slot; ++i) new_children[i] = old_children[i];
        for (size_t i = slot + 1; i < old_count; ++i) new_children[i - 1] = old_children[i];
        dealloc_internal(node, skip);
        return new_node;
    }
    
    void update_child(uint64_t* node, uint64_t skip, uint8_t index, uint64_t new_child_ptr) noexcept {
        uint64_t bitmap = get_bitmap(node, skip);
        int slot = calc_slot(bitmap, index) - 1;
        get_children(node, skip)[slot] = new_child_ptr;
    }
    
    // =========================================================================
    // Leaf Operations (using native KEY comparison)
    // =========================================================================
    
    static size_t leaf_search(const uint64_t* leaf, KEY key) noexcept {
        size_t count = leaf_count(leaf);
        const KEY* keys = leaf_keys(leaf);
        for (size_t i = 0; i < count; ++i) {
            if (keys[i] == key) return i;
            if (keys[i] > key) [[unlikely]] return ~i;
        }
        return ~count;
    }
    
    static size_t leaf_lower_bound(const uint64_t* leaf, KEY key) noexcept {
        size_t count = leaf_count(leaf);
        const KEY* keys = leaf_keys(leaf);
        for (size_t i = 0; i < count; ++i) if (keys[i] >= key) return i;
        return count;
    }
    
    static size_t leaf_upper_bound(const uint64_t* leaf, KEY key) noexcept {
        size_t count = leaf_count(leaf);
        const KEY* keys = leaf_keys(leaf);
        for (size_t i = 0; i < count; ++i) if (keys[i] > key) return i;
        return count;
    }
    
    uint64_t* leaf_insert(uint64_t* leaf, size_t pos, KEY key, stored_value_type stored_value) {
        size_t old_count = leaf_count(leaf);
        size_t new_count = old_count + 1;
        uint64_t* new_leaf = alloc_leaf(new_count);
        KEY* new_keys = leaf_keys(new_leaf);
        stored_value_type* new_vals = leaf_values(new_leaf, new_count);
        const KEY* old_keys = leaf_keys(leaf);
        const stored_value_type* old_vals = leaf_values(leaf, old_count);
        for (size_t i = 0; i < pos; ++i) { new_keys[i] = old_keys[i]; new_vals[i] = old_vals[i]; }
        new_keys[pos] = key; new_vals[pos] = stored_value;
        for (size_t i = pos; i < old_count; ++i) { new_keys[i+1] = old_keys[i]; new_vals[i+1] = old_vals[i]; }
        dealloc_leaf(leaf);
        return new_leaf;
    }
    
    uint64_t* leaf_remove(uint64_t* leaf, size_t pos) {
        size_t old_count = leaf_count(leaf);
        if (old_count == 1) { dealloc_leaf(leaf); return nullptr; }
        size_t new_count = old_count - 1;
        uint64_t* new_leaf = alloc_leaf(new_count);
        KEY* new_keys = leaf_keys(new_leaf);
        stored_value_type* new_vals = leaf_values(new_leaf, new_count);
        const KEY* old_keys = leaf_keys(leaf);
        const stored_value_type* old_vals = leaf_values(leaf, old_count);
        for (size_t i = 0; i < pos; ++i) { new_keys[i] = old_keys[i]; new_vals[i] = old_vals[i]; }
        for (size_t i = pos+1; i < old_count; ++i) { new_keys[i-1] = old_keys[i]; new_vals[i-1] = old_vals[i]; }
        dealloc_leaf(leaf);
        return new_leaf;
    }
    
    uint64_t split_leaf(uint64_t* leaf, size_t shift) {
        size_t count = leaf_count(leaf);
        const KEY* keys = leaf_keys(leaf);
        const stored_value_type* vals = leaf_values(leaf, count);
        size_t bucket_counts[64] = {0};
        for (size_t i = 0; i < count; ++i) {
            uint64_t ik = key_to_internal(keys[i]);
            bucket_counts[extract_index(ik, shift)]++;
        }
        size_t num_children = 0;
        for (size_t i = 0; i < 64; ++i) if (bucket_counts[i] > 0) num_children++;
        uint64_t* internal = alloc_internal(num_children, 0);
        uint64_t bitmap = 0;
        size_t child_slot = 0;
        uint64_t* children = get_children(internal, 0);
        for (size_t bucket = 0; bucket < 64; ++bucket) {
            if (bucket_counts[bucket] == 0) continue;
            bitmap |= (1ULL << bucket);
            uint64_t* child = alloc_leaf(bucket_counts[bucket]);
            KEY* child_keys = leaf_keys(child);
            stored_value_type* child_vals = leaf_values(child, bucket_counts[bucket]);
            size_t dest = 0;
            for (size_t i = 0; i < count; ++i) {
                uint64_t ik = key_to_internal(keys[i]);
                if (extract_index(ik, shift) == bucket) {
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
    
    static bool has_single_child(const uint64_t* node, uint64_t skip) noexcept {
        return std::popcount(get_bitmap(node, skip)) == 1;
    }
    
    static uint64_t get_single_child(const uint64_t* node, uint64_t skip) noexcept {
        return get_children(node, skip)[0];
    }
    
    static uint8_t get_single_child_index(const uint64_t* node, uint64_t skip) noexcept {
        return static_cast<uint8_t>(std::countr_zero(get_bitmap(node, skip)));
    }
    
    uint64_t try_collapse(uint64_t ptr, size_t /*shift*/) {
        if (is_leaf(ptr)) return ptr;
        uint64_t current_skip = get_skip(ptr);
        uint64_t* node = get_addr(ptr);
        if (!has_single_child(node, current_skip)) return ptr;
        uint64_t child_ptr = get_single_child(node, current_skip);
        if (is_leaf(child_ptr)) return ptr;
        uint64_t child_skip = get_skip(child_ptr);
        uint64_t* child_node = get_addr(child_ptr);
        uint64_t new_skip = current_skip + 1 + child_skip;
        // Limit skip to max_depth - 1 (can't skip more levels than exist)
        if (new_skip >= max_depth) return ptr;
        uint64_t parent_prefix = (current_skip > 0) ? get_prefix(node) : 0;
        uint8_t my_index = get_single_child_index(node, current_skip);
        uint64_t child_prefix = (child_skip > 0) ? get_prefix(child_node) : 0;
        uint64_t new_prefix;
        if (current_skip > 0) new_prefix = (parent_prefix << 6) | my_index;
        else new_prefix = my_index;
        if (child_skip > 0) new_prefix = (new_prefix << (child_skip * 6)) | child_prefix;
        uint64_t child_bitmap = get_bitmap(child_node, child_skip);
        size_t num_children = std::popcount(child_bitmap);
        uint64_t* new_node = alloc_internal(num_children, new_skip);
        new_node[0] = new_prefix;
        new_node[bitmap_offset(new_skip)] = child_bitmap;
        const uint64_t* child_children = get_children(child_node, child_skip);
        uint64_t* new_children = get_children(new_node, new_skip);
        for (size_t i = 0; i < num_children; ++i) new_children[i] = child_children[i];
        dealloc_internal(node, current_skip);
        dealloc_internal(child_node, child_skip);
        return make_internal_ptr(new_node, new_skip);
    }
    
    bool can_merge(const uint64_t* internal, uint64_t skip) const noexcept {
        uint64_t bitmap = get_bitmap(internal, skip);
        size_t num_children = std::popcount(bitmap);
        const uint64_t* children = get_children(internal, skip);
        size_t total = 0;
        for (size_t i = 0; i < num_children; ++i) {
            if (!is_leaf(children[i])) return false;
            total += leaf_count(get_addr(children[i]));
            if (total > max_leaf_entries) return false;
        }
        return true;
    }
    
    uint64_t* merge_children(uint64_t* internal, uint64_t skip) {
        uint64_t bitmap = get_bitmap(internal, skip);
        size_t num_children = std::popcount(bitmap);
        const uint64_t* children_arr = get_children(internal, skip);
        size_t total = 0;
        for (size_t i = 0; i < num_children; ++i) total += leaf_count(get_addr(children_arr[i]));
        uint64_t* merged = alloc_leaf(total);
        KEY* merged_keys = leaf_keys(merged);
        stored_value_type* merged_vals = leaf_values(merged, total);
        size_t dest = 0;
        for (uint8_t bucket = 0; bucket < 64; ++bucket) {
            if (!(bitmap & (1ULL << bucket))) continue;
            int slot = calc_slot(bitmap, bucket) - 1;
            uint64_t* child = get_addr(children_arr[slot]);
            size_t child_cnt = leaf_count(child);
            const KEY* child_keys = leaf_keys(child);
            const stored_value_type* child_vals = leaf_values(child, child_cnt);
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
    
    void clear_node(uint64_t ptr) noexcept {
        if (ptr == 0) return;
        if (is_leaf(ptr)) {
            uint64_t* leaf = get_addr(ptr);
            if constexpr (!value_inline) {
                size_t count = leaf_count(leaf);
                stored_value_type* vals = leaf_values(leaf, count);
                for (size_t i = 0; i < count; ++i) destroy_value(vals[i]);
            }
            dealloc_leaf(leaf);
        } else {
            uint64_t skip = get_skip(ptr);
            uint64_t* internal = get_addr(ptr);
            uint64_t bitmap = get_bitmap(internal, skip);
            size_t num_children = std::popcount(bitmap);
            const uint64_t* children = get_children(internal, skip);
            for (size_t i = 0; i < num_children; ++i) clear_node(children[i]);
            dealloc_internal(internal, skip);
        }
    }
    
    std::pair<uint64_t*, size_t> find_in_trie(KEY key) const noexcept {
        uint64_t internal_key = key_to_internal(key);
        uint64_t ptr = root_;
        size_t shift = 64 - first_level_bits;
        while (!high_bit_set(ptr)) [[likely]] {
            const uint64_t* node = get_addr(ptr);
            if (ptr & SKIP_MASK) [[unlikely]] {
                uint64_t skip = (ptr >> SKIP_SHIFT) & 0xF;
                uint64_t expected = extract_prefix(internal_key, shift, skip);
                if (expected != node[0]) [[unlikely]] return {nullptr, 0};
                shift -= skip * 6;
                ++node;
            }
            uint64_t bitmap = node[0];
            uint8_t index = extract_index(internal_key, shift);
            uint64_t shifted = bitmap << (63 - index);
            if (!high_bit_set(shifted)) [[unlikely]] return {nullptr, 0};
            ptr = node[1 + std::popcount(shifted) - 1];
            shift -= 6;
        }
        uint64_t* leaf = get_addr(ptr);
        size_t count = leaf_count(leaf);
        const KEY* keys = leaf_keys(leaf);
#ifdef KNTRIE_USE_AVX512
        size_t i = 0;
        if constexpr (sizeof(KEY) == 1) {
            __m512i target = _mm512_set1_epi8(static_cast<int8_t>(key));
            for (; i + 64 <= count; i += 64) {
                __m512i chunk = _mm512_loadu_si512(keys + i);
                __mmask64 eq = _mm512_cmpeq_epi8_mask(chunk, target);
                if (eq) return {leaf, i + __builtin_ctzll(eq)};
            }
        } else if constexpr (sizeof(KEY) == 2) {
            __m512i target = _mm512_set1_epi16(static_cast<int16_t>(key));
            for (; i + 32 <= count; i += 32) {
                __m512i chunk = _mm512_loadu_si512(keys + i);
                __mmask32 eq = _mm512_cmpeq_epi16_mask(chunk, target);
                if (eq) return {leaf, i + __builtin_ctz(eq)};
            }
        } else if constexpr (sizeof(KEY) == 4) {
            __m512i target = _mm512_set1_epi32(static_cast<int32_t>(key));
            for (; i + 16 <= count; i += 16) {
                __m512i chunk = _mm512_loadu_si512(keys + i);
                __mmask16 eq = _mm512_cmpeq_epi32_mask(chunk, target);
                if (eq) return {leaf, i + __builtin_ctz(eq)};
            }
        } else if constexpr (sizeof(KEY) == 8) {
            __m512i target = _mm512_set1_epi64(static_cast<int64_t>(key));
            for (; i + 8 <= count; i += 8) {
                __m512i chunk = _mm512_loadu_si512(keys + i);
                __mmask8 eq = _mm512_cmpeq_epi64_mask(chunk, target);
                if (eq) return {leaf, i + __builtin_ctz(eq)};
            }
        }
        for (; i < count; ++i) {
            if (keys[i] == key) return {leaf, i};
        }
        return {nullptr, 0};
#else
        for (size_t i = 0; i < count; ++i) {
            if (keys[i] == key) return {leaf, i};
            if (keys[i] > key) [[unlikely]] return {nullptr, 0};
        }
        return {nullptr, 0};
#endif
    }
    
    std::pair<uint64_t*, size_t> find_min(uint64_t ptr) const noexcept {
        if (ptr == 0) return {nullptr, 0};
        while (!is_leaf(ptr)) {
            uint64_t skip = get_skip(ptr);
            ptr = get_children(get_addr(ptr), skip)[0];
        }
        return {get_addr(ptr), 0};
    }
    
    std::pair<uint64_t*, size_t> find_max(uint64_t ptr) const noexcept {
        if (ptr == 0) return {nullptr, 0};
        while (!is_leaf(ptr)) {
            uint64_t skip = get_skip(ptr);
            uint64_t* node = get_addr(ptr);
            ptr = get_children(node, skip)[std::popcount(get_bitmap(node, skip)) - 1];
        }
        uint64_t* leaf = get_addr(ptr);
        return {leaf, leaf_count(leaf) - 1};
    }
    
    std::pair<uint64_t*, size_t> find_next(KEY key) const noexcept {
        uint64_t internal_key = key_to_internal(key);
        if (root_ == 0) return {nullptr, 0};
        uint64_t* best_leaf = nullptr;
        size_t best_idx = 0;
        uint64_t ptr = root_;
        size_t shift = 64 - first_level_bits;
        while (!is_leaf(ptr)) {
            uint64_t skip = get_skip(ptr);
            uint64_t* node = get_addr(ptr);
            if (skip > 0) {
                uint64_t expected = extract_prefix(internal_key, shift, skip);
                uint64_t actual = get_prefix(node);
                if (expected != actual) {
                    if (actual > expected) { auto [l,i] = find_min(ptr); return {l,i}; }
                    return {best_leaf, best_idx};
                }
                shift -= skip * 6;
            }
            uint64_t bitmap = get_bitmap(node, skip);
            uint8_t target_index = extract_index(internal_key, shift);
            uint64_t higher_mask = bitmap & ~((2ULL << target_index) - 1);
            if (higher_mask != 0) {
                int higher_bit = std::countr_zero(higher_mask);
                auto [l,i] = find_min(get_children(node, skip)[calc_slot(bitmap, higher_bit) - 1]);
                if (l) { best_leaf = l; best_idx = i; }
            }
            uint64_t child = get_child(node, skip, target_index);
            if (child == 0) return {best_leaf, best_idx};
            ptr = child;
            shift -= 6;
        }
        uint64_t* leaf = get_addr(ptr);
        size_t idx = leaf_upper_bound(leaf, key);
        if (idx < leaf_count(leaf)) return {leaf, idx};
        return {best_leaf, best_idx};
    }
    
    std::pair<uint64_t*, size_t> find_prev(KEY key) const noexcept {
        uint64_t internal_key = key_to_internal(key);
        if (root_ == 0) return {nullptr, 0};
        uint64_t* best_leaf = nullptr;
        size_t best_idx = 0;
        uint64_t ptr = root_;
        size_t shift = 64 - first_level_bits;
        while (!is_leaf(ptr)) {
            uint64_t skip = get_skip(ptr);
            uint64_t* node = get_addr(ptr);
            if (skip > 0) {
                uint64_t expected = extract_prefix(internal_key, shift, skip);
                uint64_t actual = get_prefix(node);
                if (expected != actual) {
                    if (actual < expected) { auto [l,i] = find_max(ptr); return {l,i}; }
                    return {best_leaf, best_idx};
                }
                shift -= skip * 6;
            }
            uint64_t bitmap = get_bitmap(node, skip);
            uint8_t target_index = extract_index(internal_key, shift);
            uint64_t lower_mask = bitmap & ((1ULL << target_index) - 1);
            if (lower_mask != 0) {
                int bit = 63 - std::countl_zero(lower_mask);
                auto [l,i] = find_max(get_children(node, skip)[calc_slot(bitmap, bit) - 1]);
                if (l) { best_leaf = l; best_idx = i; }
            }
            uint64_t child = get_child(node, skip, target_index);
            if (child == 0) return {best_leaf, best_idx};
            ptr = child;
            shift -= 6;
        }
        uint64_t* leaf = get_addr(ptr);
        const KEY* keys = leaf_keys(leaf);
        for (size_t i = leaf_count(leaf); i-- > 0;) if (keys[i] < key) return {leaf, i};
        return {best_leaf, best_idx};
    }
    
    struct InsertResult { uint64_t new_ptr; bool inserted; stored_value_type old_stored_value; };
    
    InsertResult insert_recursive(uint64_t ptr, KEY key, stored_value_type stored_value, size_t shift) {
        uint64_t internal_key = key_to_internal(key);
        
        if (ptr == 0) {
            uint64_t* leaf = alloc_leaf(1);
            leaf_keys(leaf)[0] = key;
            leaf_values(leaf, 1)[0] = stored_value;
            return {make_leaf_ptr(leaf), true, {}};
        }
        if (is_leaf(ptr)) {
            uint64_t* leaf = get_addr(ptr);
            size_t search_result = leaf_search(leaf, key);
            if (search_result < leaf_count(leaf)) {
                stored_value_type* vals = leaf_values(leaf);
                stored_value_type old_val = vals[search_result];
                vals[search_result] = stored_value;
                return {ptr, false, old_val};
            }
            size_t insert_pos = ~search_result;
            size_t count = leaf_count(leaf);
            if (count < max_leaf_entries) {
                uint64_t* new_leaf = leaf_insert(leaf, insert_pos, key, stored_value);
                return {make_leaf_ptr(new_leaf), true, {}};
            }
            // Split
            uint64_t* temp = alloc_leaf(count + 1);
            KEY* temp_keys = leaf_keys(temp);
            stored_value_type* temp_vals = leaf_values(temp, count + 1);
            const KEY* old_keys = leaf_keys(leaf);
            const stored_value_type* old_vals = leaf_values(leaf, count);
            for (size_t i = 0; i < insert_pos; ++i) { temp_keys[i] = old_keys[i]; temp_vals[i] = old_vals[i]; }
            temp_keys[insert_pos] = key; temp_vals[insert_pos] = stored_value;
            for (size_t i = insert_pos; i < count; ++i) { temp_keys[i+1] = old_keys[i]; temp_vals[i+1] = old_vals[i]; }
            dealloc_leaf(leaf);
            return {split_leaf(temp, shift), true, {}};
        }
        uint64_t skip = get_skip(ptr);
        uint64_t* node = get_addr(ptr);
        if (skip > 0) {
            uint64_t expected = extract_prefix(internal_key, shift, skip);
            uint64_t actual = get_prefix(node);
            if (expected != actual) {
                size_t prefix_bits = skip * 6;
                uint64_t diff = expected ^ actual;
                size_t leading_zeros = std::countl_zero(diff);
                // leading_zeros counts zeros in 64-bit value; adjust for prefix width
                size_t common_bits = leading_zeros - (64 - prefix_bits);
                size_t common_levels = common_bits / 6;
                uint8_t new_index = (expected >> (prefix_bits - (common_levels + 1) * 6)) & 0x3F;
                uint8_t old_index = (actual >> (prefix_bits - (common_levels + 1) * 6)) & 0x3F;
                uint64_t* new_internal = alloc_internal(2, common_levels);
                if (common_levels > 0) new_internal[0] = actual >> (prefix_bits - common_levels * 6);
                new_internal[bitmap_offset(common_levels)] = (1ULL << new_index) | (1ULL << old_index);
                uint64_t* new_leaf = alloc_leaf(1);
                leaf_keys(new_leaf)[0] = key;
                leaf_values(new_leaf, 1)[0] = stored_value;
                uint64_t remaining_skip = skip - common_levels - 1;
                uint64_t* adjusted_old;
                if (remaining_skip == 0) {
                    uint64_t old_bitmap = get_bitmap(node, skip);
                    size_t old_children_count = std::popcount(old_bitmap);
                    adjusted_old = alloc_internal(old_children_count, 0);
                    adjusted_old[0] = old_bitmap;
                    const uint64_t* oc = get_children(node, skip);
                    uint64_t* nc = get_children(adjusted_old, 0);
                    for (size_t i = 0; i < old_children_count; ++i) nc[i] = oc[i];
                    dealloc_internal(node, skip);
                } else {
                    uint64_t old_bitmap = get_bitmap(node, skip);
                    size_t old_children_count = std::popcount(old_bitmap);
                    adjusted_old = alloc_internal(old_children_count, remaining_skip);
                    adjusted_old[0] = actual & ((1ULL << (remaining_skip * 6)) - 1);
                    adjusted_old[bitmap_offset(remaining_skip)] = old_bitmap;
                    const uint64_t* oc = get_children(node, skip);
                    uint64_t* nc = get_children(adjusted_old, remaining_skip);
                    for (size_t i = 0; i < old_children_count; ++i) nc[i] = oc[i];
                    dealloc_internal(node, skip);
                }
                uint64_t* new_children = get_children(new_internal, common_levels);
                if (new_index < old_index) {
                    new_children[0] = make_leaf_ptr(new_leaf);
                    new_children[1] = make_internal_ptr(adjusted_old, remaining_skip);
                } else {
                    new_children[0] = make_internal_ptr(adjusted_old, remaining_skip);
                    new_children[1] = make_leaf_ptr(new_leaf);
                }
                return {make_internal_ptr(new_internal, common_levels), true, {}};
            }
            shift -= skip * 6;
        }
        uint8_t index = extract_index(internal_key, shift);
        uint64_t child = get_child(node, skip, index);
        InsertResult result = insert_recursive(child, key, stored_value, shift - bits_per_level);
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
    
    struct EraseResult { uint64_t new_ptr; bool erased; bool check_merge; };
    
    EraseResult erase_recursive(uint64_t ptr, KEY key, size_t shift) {
        uint64_t internal_key = key_to_internal(key);
        if (ptr == 0) return {0, false, false};
        if (is_leaf(ptr)) {
            uint64_t* leaf = get_addr(ptr);
            size_t search_result = leaf_search(leaf, key);
            if (search_result >= leaf_count(leaf)) return {ptr, false, false};
            destroy_value(leaf_values(leaf)[search_result]);
            uint64_t* new_leaf = leaf_remove(leaf, search_result);
            if (new_leaf == nullptr) return {0, true, true};
            return {make_leaf_ptr(new_leaf), true, true};
        }
        uint64_t skip = get_skip(ptr);
        uint64_t* node = get_addr(ptr);
        if (skip > 0) {
            uint64_t expected = extract_prefix(internal_key, shift, skip);
            if (expected != get_prefix(node)) return {ptr, false, false};
            shift -= skip * 6;
        }
        uint8_t index = extract_index(internal_key, shift);
        uint64_t child = get_child(node, skip, index);
        if (child == 0) return {ptr, false, false};
        EraseResult result = erase_recursive(child, key, shift - bits_per_level);
        if (!result.erased) return {ptr, false, false};
        if (result.new_ptr == 0) {
            uint64_t* new_node = remove_child(node, skip, index);
            if (new_node == nullptr) return {0, true, true};
            if (can_merge(new_node, skip)) return {make_leaf_ptr(merge_children(new_node, skip)), true, true};
            uint64_t new_ptr = make_internal_ptr(new_node, skip);
            new_ptr = try_collapse(new_ptr, shift + skip * 6);
            return {new_ptr, true, false};
        }
        if (result.new_ptr != child) update_child(node, skip, index, result.new_ptr);
        if (result.check_merge && can_merge(node, skip)) return {make_leaf_ptr(merge_children(node, skip)), true, true};
        uint64_t new_ptr = make_internal_ptr(node, skip);
        new_ptr = try_collapse(new_ptr, shift + skip * 6);
        return {new_ptr, true, false};
    }
    
public:
    template<bool Const>
    class iterator_impl {
        friend class kntrie;
        using trie_type = std::conditional_t<Const, const kntrie, kntrie>;
        trie_type* trie_;
        KEY key_;
        VALUE value_copy_;
        bool valid_;
        iterator_impl(trie_type* t, KEY k, const VALUE& v, bool valid) : trie_(t), key_(k), value_copy_(v), valid_(valid) {}
        iterator_impl(trie_type* t, bool valid) : trie_(t), key_{}, value_copy_{}, valid_(valid) {}
    public:
        using iterator_category = std::bidirectional_iterator_tag;
        using value_type = std::pair<const KEY, VALUE>;
        using difference_type = std::ptrdiff_t;
        using pointer = const value_type*;
        using reference = const value_type&;
        iterator_impl() : trie_(nullptr), key_{}, value_copy_{}, valid_(false) {}
        template<bool C = Const, typename = std::enable_if_t<C>>
        iterator_impl(const iterator_impl<false>& o) : trie_(o.trie_), key_(o.key_), value_copy_(o.value_copy_), valid_(o.valid_) {}
        std::pair<const KEY, const VALUE&> operator*() const { return {key_, value_copy_}; }
        const KEY& key() const { return key_; }
        const VALUE& value() const { return value_copy_; }
        iterator_impl& operator++() {
            if (!valid_ || !trie_) { valid_ = false; return *this; }
            auto [l,i] = trie_->find_next(key_);
            if (!l) valid_ = false;
            else { key_ = leaf_keys(l)[i]; value_copy_ = trie_->load_value(leaf_values(l)[i]); }
            return *this;
        }
        iterator_impl operator++(int) { auto t = *this; ++(*this); return t; }
        iterator_impl& operator--() {
            if (!trie_) return *this;
            if (!valid_) {
                auto [l,i] = trie_->find_max(trie_->root_);
                if (l) { key_ = leaf_keys(l)[i]; value_copy_ = trie_->load_value(leaf_values(l)[i]); valid_ = true; }
                return *this;
            }
            auto [l,i] = trie_->find_prev(key_);
            if (l) { key_ = leaf_keys(l)[i]; value_copy_ = trie_->load_value(leaf_values(l)[i]); }
            return *this;
        }
        iterator_impl operator--(int) { auto t = *this; --(*this); return t; }
        bool operator==(const iterator_impl& o) const {
            if (!valid_ && !o.valid_) return true;
            if (valid_ != o.valid_) return false;
            return trie_ == o.trie_ && key_ == o.key_;
        }
        bool operator!=(const iterator_impl& o) const { return !(*this == o); }
    };
    
    using iterator = iterator_impl<false>;
    using const_iterator = iterator_impl<true>;
    using reverse_iterator = std::reverse_iterator<iterator>;
    using const_reverse_iterator = std::reverse_iterator<const_iterator>;
    
    kntrie() : root_(0), size_(0), alloc_() { root_ = make_leaf_ptr(alloc_leaf(0)); }
    explicit kntrie(const ALLOC& a) : root_(0), size_(0), alloc_(a) { root_ = make_leaf_ptr(alloc_leaf(0)); }
    ~kntrie() { clear(); }
    kntrie(const kntrie& o) : root_(0), size_(0), alloc_(o.alloc_) {
        root_ = make_leaf_ptr(alloc_leaf(0));
        for (auto it = o.begin(); it != o.end(); ++it) insert(it.key(), it.value());
    }
    kntrie(kntrie&& o) noexcept : root_(o.root_), size_(o.size_), alloc_(std::move(o.alloc_)) {
        o.root_ = make_leaf_ptr(o.alloc_.allocate(leaf_size_u64(0)));
        get_addr(o.root_)[0] = 0;
        o.size_ = 0;
    }
    kntrie& operator=(const kntrie& o) {
        if (this != &o) { clear(); for (auto it = o.begin(); it != o.end(); ++it) insert(it.key(), it.value()); }
        return *this;
    }
    kntrie& operator=(kntrie&& o) noexcept {
        if (this != &o) {
            clear_node(root_); root_ = o.root_; size_ = o.size_; alloc_ = std::move(o.alloc_);
            o.root_ = make_leaf_ptr(o.alloc_.allocate(leaf_size_u64(0)));
            get_addr(o.root_)[0] = 0;
            o.size_ = 0;
        }
        return *this;
    }
    
    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }
    [[nodiscard]] size_type size() const noexcept { return size_; }
    [[nodiscard]] size_type max_size() const noexcept { return std::numeric_limits<size_type>::max(); }
    
    iterator begin() noexcept {
        if (size_ == 0) return end();
        auto [l,i] = find_min(root_);
        if (!l) return end();
        return iterator(this, leaf_keys(l)[i], load_value(leaf_values(l)[i]), true);
    }
    const_iterator begin() const noexcept {
        if (size_ == 0) return end();
        auto [l,i] = find_min(root_);
        if (!l) return end();
        return const_iterator(this, leaf_keys(l)[i], load_value(leaf_values(l)[i]), true);
    }
    iterator end() noexcept { return iterator(this, false); }
    const_iterator end() const noexcept { return const_iterator(this, false); }
    const_iterator cbegin() const noexcept { return begin(); }
    const_iterator cend() const noexcept { return end(); }
    reverse_iterator rbegin() noexcept { return reverse_iterator(end()); }
    reverse_iterator rend() noexcept { return reverse_iterator(begin()); }
    const_reverse_iterator rbegin() const noexcept { return const_reverse_iterator(end()); }
    const_reverse_iterator rend() const noexcept { return const_reverse_iterator(begin()); }
    const_reverse_iterator crbegin() const noexcept { return const_reverse_iterator(cend()); }
    const_reverse_iterator crend() const noexcept { return const_reverse_iterator(cbegin()); }
    
    iterator find(const KEY& key) {
        auto [l,i] = find_in_trie(key);
        if (!l) return end();
        return iterator(this, key, load_value(leaf_values(l)[i]), true);
    }
    const_iterator find(const KEY& key) const {
        auto [l,i] = find_in_trie(key);
        if (!l) return end();
        return const_iterator(this, key, load_value(leaf_values(l)[i]), true);
    }
    bool contains(const KEY& key) const { return find_in_trie(key).first != nullptr; }
    size_type count(const KEY& key) const { return contains(key) ? 1 : 0; }
    
    iterator lower_bound(const KEY& key) {
        auto [l,i] = find_in_trie(key);
        if (l) return iterator(this, key, load_value(leaf_values(l)[i]), true);
        if (size_ == 0) return end();
        uint64_t ik = key_to_internal(key);
        uint64_t* best_leaf = nullptr; size_t best_idx = 0;
        uint64_t ptr = root_; size_t shift = 64 - first_level_bits;
        while (!is_leaf(ptr)) {
            uint64_t skip = get_skip(ptr);
            uint64_t* node = get_addr(ptr);
            if (skip > 0) {
                uint64_t exp = extract_prefix(ik, shift, skip);
                uint64_t act = get_prefix(node);
                if (exp != act) {
                    if (act > exp) { auto [lf,ii] = find_min(ptr); if (lf) return iterator(this, leaf_keys(lf)[ii], load_value(leaf_values(lf)[ii]), true); }
                    break;
                }
                shift -= skip * 6;
            }
            uint64_t bitmap = get_bitmap(node, skip);
            uint8_t ti = extract_index(ik, shift);
            uint64_t higher_mask = bitmap & ~((1ULL << ti) - 1) & ~(1ULL << ti);
            if (higher_mask != 0) {
                int hb = std::countr_zero(higher_mask);
                auto [lf,ii] = find_min(get_children(node, skip)[calc_slot(bitmap, hb) - 1]);
                if (lf) { best_leaf = lf; best_idx = ii; }
            }
            uint64_t child = get_child(node, skip, ti);
            if (child == 0) break;
            ptr = child; shift -= 6;
        }
        if (is_leaf(ptr)) {
            uint64_t* lf = get_addr(ptr);
            size_t ii = leaf_lower_bound(lf, key);
            if (ii < leaf_count(lf)) return iterator(this, leaf_keys(lf)[ii], load_value(leaf_values(lf)[ii]), true);
        }
        if (best_leaf) return iterator(this, leaf_keys(best_leaf)[best_idx], load_value(leaf_values(best_leaf)[best_idx]), true);
        return end();
    }
    const_iterator lower_bound(const KEY& key) const { return const_cast<kntrie*>(this)->lower_bound(key); }
    
    iterator upper_bound(const KEY& key) {
        auto [l,i] = find_next(key);
        if (!l) return end();
        return iterator(this, leaf_keys(l)[i], load_value(leaf_values(l)[i]), true);
    }
    const_iterator upper_bound(const KEY& key) const { return const_cast<kntrie*>(this)->upper_bound(key); }
    
    std::pair<iterator, iterator> equal_range(const KEY& key) { return {lower_bound(key), upper_bound(key)}; }
    std::pair<const_iterator, const_iterator> equal_range(const KEY& key) const { return {lower_bound(key), upper_bound(key)}; }
    
    std::pair<iterator, bool> insert(const std::pair<KEY, VALUE>& v) { return insert(v.first, v.second); }
    std::pair<iterator, bool> insert(const KEY& key, const VALUE& value) {
        stored_value_type sv = store_value(value);
        InsertResult r = insert_recursive(root_, key, sv, 64 - first_level_bits);
        root_ = r.new_ptr;
        if (r.inserted) { ++size_; return {iterator(this, key, value, true), true}; }
        destroy_value(sv);
        return {iterator(this, key, load_value(r.old_stored_value), true), false};
    }
    template<typename... Args> std::pair<iterator, bool> emplace(Args&&... args) {
        auto p = std::pair<KEY, VALUE>(std::forward<Args>(args)...);
        return insert(p.first, p.second);
    }
    
    iterator erase(iterator pos) { if (!pos.valid_) return pos; KEY k = pos.key_; ++pos; erase(k); return pos; }
    iterator erase(const_iterator pos) { if (!pos.valid_) return end(); KEY k = pos.key_; erase(k); return upper_bound(k); }
    size_type erase(const KEY& key) {
        EraseResult r = erase_recursive(root_, key, 64 - bits_per_level);
        root_ = r.new_ptr;
        if (root_ == 0) root_ = make_leaf_ptr(alloc_leaf(0));
        if (r.erased) { --size_; return 1; }
        return 0;
    }
    iterator erase(iterator first, iterator last) { while (first != last) first = erase(first); return first; }
    
    void clear() noexcept {
        clear_node(root_);
        size_ = 0;
        uint64_t* leaf = alloc_.allocate(leaf_size_u64(0));
        leaf[0] = 0;
        root_ = make_leaf_ptr(leaf);
    }
    
    void swap(kntrie& o) noexcept { std::swap(root_, o.root_); std::swap(size_, o.size_); std::swap(alloc_, o.alloc_); }
    allocator_type get_allocator() const noexcept { return alloc_; }
};

template<typename K, typename V, typename A>
void swap(kntrie<K, V, A>& l, kntrie<K, V, A>& r) noexcept { l.swap(r); }

} // namespace kn

#endif // KNTRIE_HPP
