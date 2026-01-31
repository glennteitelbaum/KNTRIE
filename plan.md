# kntrie2 Rewrite Plan

## Overview

Rewrite kntrie2 with:
- 12-bit chunk processing (6/6 split)
- Bitmap-based split nodes (bottom is separate node, not embedded)
- Template recursion keyed on BITS remaining
- Tail recursion only (single recursive call at end)
- Header passed to avoid re-fetching

---

## Key Bit Layouts

### 64-bit Keys

```
| 4 bits | 12 bits | 12 bits | 12 bits | 12 bits | 12 bits |
| root   | level 1 | level 2 | level 3 | level 4 | level 5 |
|  [16]  |   60→48 |  48→36  |  36→24  |  24→12  |  12→0   |
```

- `root_[16]` - array indexed by `key >> 60`
- Each root slot always points to valid node (empty compact node if unused)
- 5 levels of 12-bit processing
- Total: 4 + 12*5 = 64 ✓

### 32-bit Keys

```
| 2 bits | 6 bits  | 12 bits | 12 bits |
| root   | level 1 | level 2 | level 3 |
|  [4]   |  30→24  |  24→12  |  12→0   |
```

- `root_[4]` - array indexed by `key >> 30`
- Each root slot always points to 6-bit bitmap node (bitmap=0 if unused)
- Level 1: special 6-bit handling (single bitmap, no split)
- Levels 2-3: standard 12-bit processing
- Total: 2 + 6 + 12 + 12 = 32 ✓

---

## Node Header (16 bytes = 2 uint64_t)

```cpp
struct alignas(16) NodeHeader {
    uint64_t prefix;      // skip chunks packed (12 bits each)
    uint32_t count;       // total entries
    uint16_t top_count;   // buckets when split (0 = compact)
    uint8_t skip;         // number of 12-bit chunks to skip
    uint8_t flags;        // bit 0: is_leaf
};
```

---

## Node Layouts

### Compact Node (count ≤ 64)

**Internal (12-bit keys):**
```
[NodeHeader: 16 bytes]
[uint16_t keys[count]]      // 12-bit keys stored in 16-bit
[padding to 8-byte align]
[uint64_t children[count]]  // child pointers
```

**Leaf:**
```
[NodeHeader: 16 bytes]
[uint16_t keys[count]]      // 12-bit keys
[padding to 8-byte align]
[uint64_t values[count]]    // values or value pointers
```

- Linear scan to find key (sorted for early exit)
- Max 64 entries

### Split Node (count > 64)

**Top node:**
```
[NodeHeader: 16 bytes]
[uint64_t top_bitmap]           // bit i set = bottom[i] exists
[uint64_t child_ptrs[popcount]] // pointers to bottom nodes
```

**Bottom node (no header):**
```
[uint64_t bitmap]               // bit i set = entry[i] exists  
[uint64_t data[popcount]]       // children (internal) or values (leaf)
```

- 6/6 split: both bitmaps fit in uint64_t (64 possible values each)

---

## Implementation Status

### Completed ✅
- Key traits for all bit widths (60, 48, 36, 24, 12, 30)
- 16-byte NodeHeader
- Key conversion (signed/unsigned, 32/64 bit)
- Compact node layout and accessors
- Split node layout and accessors
- `find_impl<BITS>` template with skip handling
- `insert_impl<BITS>` with:
  - Compact leaf/internal insert
  - Split leaf/internal insert
  - Compact → split conversion
  - Prefix split handling
- `clear_impl<BITS>` for destruction
- Public API: find_value, contains, count, insert, clear

### Not Yet Tested
- 32-bit key special handling (6-bit first level)
- Compile test
- Runtime correctness vs kntrie_v1
- Memory leak check (ASAN)
- Performance comparison

---

## Files

- `kntrie_v1.hpp` - Original implementation (renamed from kntrie.hpp)
- `kntrie2.hpp` - New implementation following this plan

---

## Next Steps

1. Compile test kntrie2.hpp
2. Create test harness comparing kntrie_v1 vs kntrie2
3. Fix any bugs found
4. Run ASAN memory check
5. Performance benchmarks
