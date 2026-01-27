# kntrie Implementation Plan

## Overview

`kntrie<KEY, VALUE, ALLOC>` - A trie-based ordered container for integer keys with `std::map`-like interface.

---

## 1. Key Conversion

### 1.1 Byte Order
- Detect native endianness at compile time
- If little-endian: use `std::byteswap` to convert to big-endian
- Big-endian keys ensure lexicographic order matches numeric order when traversing MSB-first

### 1.2 Sign Handling
- Use `std::is_signed_v<KEY>` to detect signed types
- For signed keys: flip the sign bit (XOR with `1ULL << (bits-1)`)
- This maps: `INT_MIN → 0x00...`, `0 → 0x80...`, `INT_MAX → 0xFF...`

### 1.3 Key Width Normalization
- Keys stored in leaves as `uint64_t` (zero-extended before conversion)
- Iterator stores key in native `KEY` type (converted back)
- Conversion functions:
  - `key_to_internal(KEY k) → uint64_t`
  - `internal_to_key(uint64_t k) → KEY`

---

## 2. Trie Structure

### 2.1 Branching
- **6 bits per level** (64-way branching)
- Single `uint64_t` bitmap per internal node
- Levels by key size:
  | Key Type | Bits | Levels (ceil(bits/6)) |
  |----------|------|----------------------|
  | uint8_t  | 8    | 2                    |
  | uint16_t | 16   | 3                    |
  | uint32_t | 32   | 6                    |
  | uint64_t | 64   | 11                   |

### 2.2 Internal Node Layout
```
[bitmap: uint64_t][child_ptr_0][child_ptr_1]...[child_ptr_N]
```
- `bitmap`: Bit i set means child for index i exists
- Children stored in **popcount order** (sparse array)
- Total size: `(1 + popcount(bitmap)) * sizeof(uint64_t)`

### 2.3 Leaf Node Layout
```
[count: uint64_t][key_0][key_1]...[key_{count-1}][val_0][val_1]...[val_{count-1}]
```
- Keys stored as full converted `uint64_t` values
- Keys maintained in **sorted order** within leaf
- Max 64 entries per leaf
- Values:
  - If `sizeof(VALUE) <= 8` and trivially copyable: stored inline as `uint64_t`
  - Else: stored as `VALUE*` cast to `uint64_t`, trie owns allocation

### 2.4 Pointer Tagging
- Pointer to **leaf**: high bit (bit 63) SET
- Pointer to **internal node**: high bit CLEAR
- Before dereferencing leaf pointer: mask off high bit

---

## 3. Node Operations

### 3.1 Child Lookup (ITERATIVE - read path, OPTIMIZED)
```cpp
uint64_t* get_child(uint64_t* node, uint8_t index) const noexcept {
    uint64_t bitmap = node[0];
    uint64_t below = bitmap << (63 - index);
    if (!(below & (1ULL << 63))) [[unlikely]] return nullptr;
    int slot = std::popcount(below);
    return reinterpret_cast<uint64_t*>(node[slot]); // slot is already correct (1-based due to bitmap)
}
```
Note: The shift-then-test-high-bit pattern is faster than mask-and-test.

### 3.2 Child Insert (recursive write path)
- Set bit in bitmap
- Reallocate node array with +1 slot
- Insert new pointer at correct popcount position
- Copy existing pointers around it

### 3.3 Child Remove (recursive write path)
- Clear bit in bitmap
- Reallocate node array with -1 slot
- Remove pointer at popcount position
- Shift remaining pointers

---

## 4. Leaf Operations

### 4.1 Leaf Search (ITERATIVE - LINEAR SCAN)
- Linear scan on sorted keys (N ≤ 64, loop is faster than binary search)
- Return index if found, or insertion point if not

### 4.2 Leaf Insert
- Linear scan for position
- If duplicate: update value, return
- If count < 64: shift keys/values, insert
- If count == 64: **split leaf**

### 4.3 Leaf Remove
- Linear scan for key
- If found: shift keys/values down, decrement count
- Check for **merge opportunity**

### 4.4 Leaf Split
- Create parent internal node
- Group keys by their current-level 6-bit index
- Create child leaves for each group
- Connect to parent

### 4.5 Leaf Merge (cascading)
- After removal, check if all children of internal node are leaves
- If total entries across all child leaves ≤ 64: merge into single leaf
- Recursively check parent for further merge opportunities

---

## 5. Core Class Structure

### 5.1 Main Class
```cpp
template<typename KEY, typename VALUE, typename ALLOC = std::allocator<uint64_t>>
class kntrie {
    // Type traits
    static constexpr bool is_signed_key = std::is_signed_v<KEY>;
    static constexpr size_t key_bits = sizeof(KEY) * 8;
    static constexpr size_t max_depth = (key_bits + 5) / 6;
    static constexpr bool value_inline = sizeof(VALUE) <= 8 && std::is_trivially_copyable_v<VALUE>;
    
    // Tag constants
    static constexpr uint64_t LEAF_TAG = 1ULL << 63;
    
    // Members
    uint64_t* root_;      // Tagged pointer to root node (can be leaf)
    size_t size_;         // Number of entries
    ALLOC alloc_;
    
    // Key conversion
    static uint64_t key_to_internal(KEY k);
    static KEY internal_to_key(uint64_t k);
    
    // ...
};
```

### 5.2 Iterator Classes
```cpp
template<bool Const>
class iterator_impl {
    using trie_ptr = std::conditional_t<Const, const kntrie*, kntrie*>;
    
    trie_ptr trie_;
    KEY key_;             // Native key type (converted back from internal)
    VALUE value_copy_;    // Copy of value
    bool valid_;          // false for end()
    
public:
    // Read-only access
    const KEY& key() const;
    const VALUE& value() const;
    std::pair<const KEY, const VALUE&> operator*() const;
    
    // Traversal
    iterator_impl& operator++();        // Find next via trie search
    iterator_impl& operator--();        // Find prev via trie search
    
    // Modifications via parent (non-const only)
    iterator_impl insert(KEY k, const VALUE& v);  // calls trie_->insert()
    iterator_impl erase();                         // calls trie_->erase()
};

using iterator = iterator_impl<false>;
using const_iterator = iterator_impl<true>;
using reverse_iterator = std::reverse_iterator<iterator>;
using const_reverse_iterator = std::reverse_iterator<const_iterator>;
```

---

## 6. Public Interface (std::map compatible)

### 6.1 Capacity
```cpp
bool empty() const noexcept;
size_t size() const noexcept;
size_t max_size() const noexcept;
```

### 6.2 Iterators
```cpp
iterator begin() noexcept;
iterator end() noexcept;
const_iterator begin() const noexcept;
const_iterator end() const noexcept;
const_iterator cbegin() const noexcept;
const_iterator cend() const noexcept;

reverse_iterator rbegin() noexcept;
reverse_iterator rend() noexcept;
// ... const versions
```

### 6.3 Lookup
```cpp
iterator find(const KEY& key);
const_iterator find(const KEY& key) const;
bool contains(const KEY& key) const;
size_t count(const KEY& key) const;          // 0 or 1

iterator lower_bound(const KEY& key);
iterator upper_bound(const KEY& key);
std::pair<iterator, iterator> equal_range(const KEY& key);
// ... const versions
```

### 6.4 Modifiers
```cpp
std::pair<iterator, bool> insert(const std::pair<KEY, VALUE>& value);
std::pair<iterator, bool> insert(KEY key, const VALUE& value);

template<typename... Args>
std::pair<iterator, bool> emplace(Args&&... args);

iterator erase(iterator pos);
iterator erase(const_iterator pos);
size_t erase(const KEY& key);               // Returns 0 or 1
iterator erase(iterator first, iterator last);

void clear() noexcept;
void swap(kntrie& other) noexcept;
```

### 6.5 Observers
```cpp
ALLOC get_allocator() const noexcept;
```

---

## 7. Memory Management

### 7.1 Allocation
- All allocations via `ALLOC::allocate(n)` where n is uint64_t count
- Compute size from bitmap (internal) or count (leaf)

### 7.2 Value Storage
- Inline values: `reinterpret_cast` / `memcpy` to/from `uint64_t`
- Pointer values: Allocate `VALUE` via rebind allocator, copy construct, store pointer as `uint64_t`

### 7.3 Deallocation Strategy

**Node deallocation is a simple memory free** - no recursive descent:
- Internal node: Just deallocate the node's memory
- Leaf node: Just deallocate the node's memory

**clear() handles full cleanup recursively:**
- Traverses entire trie
- For each leaf: deallocates VALUE pointers (if not inline)
- For each node: deallocates node memory
- Destructor calls clear()

**During erase/merge operations:**
1. Create new structure first (new merged leaf, etc.)
2. Copy/move value pointers (uint64_t copies) to new structure
3. Deallocate old node memory only (values already moved)
4. Only the **erased** value's pointer gets deallocated (if not inline)

This approach enables fast move semantics and avoids complex ownership tracking.

---

## 8. Implementation Order

### Phase 1: Core Infrastructure
1. `kntrie.hpp` - Main class skeleton
2. Key conversion utilities
3. Node allocation/deallocation helpers
4. Pointer tagging utilities

### Phase 2: Read Operations
5. `find()` - iterative lookup
6. `contains()` / `count()`
7. `begin()` / `end()` - find min/max
8. Iterator `++` / `--`
9. `lower_bound()` / `upper_bound()`

### Phase 3: Write Operations
10. `insert()` - with leaf split
11. `erase()` - with leaf merge
12. `clear()`
13. `emplace()`

### Phase 4: Completeness
14. Range erase
15. Copy/move constructors
16. Comparison operators
17. `swap()`

---

## 9. File Structure

```
kntrie/
├── kntrie.hpp              # Main header (single-header library)
│   ├── key_utils           # Conversion functions
│   ├── node_utils          # Node manipulation
│   ├── kntrie class        # Main container
│   └── iterator classes    # Iterator implementation
├── test_kntrie.cpp         # Unit tests
└── plan.md                 # This file
```

---

## 10. Open Questions Resolved

| Question | Resolution |
|----------|------------|
| Branching factor | 6-bit (64-way) |
| Key storage in leaf | Full converted uint64_t |
| Value storage | Inline if ≤8 bytes + trivially copyable, else pointer |
| Iterator invalidation | Never (copies), unless trie deleted |
| Iterator mutability | Read-only, ops via parent pointer |
| Packing optimization | Deferred |
| Exception safety | Propagate, no rollback |
| Leaf search | Linear scan (faster than binary for N ≤ 64) |
| Node deallocation | Memory free only; clear() handles recursive cleanup |

---

## 11. Notes

- Read paths (find, iteration) are **iterative** for performance
- Write paths (insert, erase) are **recursive** for clarity
- Leaf searches use **linear scan** (faster than binary for small N)
- Node deallocation is **memory-only** (no recursive descent)
- `clear()` handles full recursive cleanup
- Create-then-delete pattern for merge/split safety
- Correctness over optimization - we can tune later
- Single-threaded only for now
