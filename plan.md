# Value Type Normalization & Inline Storage

## Goal

Reduce template instantiations for trivial types and enable inline storage for non-trivial types up to 64 bytes (one cache line), eliminating pointer chasing during iteration.

## Three categories

| Cat | IS_INLINE | has_destructor | Storage | Example |
|-----|-----------|----------------|---------|---------|
| A | yes | no | normalized u8/u16/u32/u64/array<u64,N> | int, float, POD |
| B | yes | yes | real T | string, vector, map |
| C | no | yes | pointer to T | >64B or !nothrow_move |

## Slot movement: std::copy vs std::move

Two cases based on whether source and destination can overlap:

**No overlap (realloc, new node, split):** `std::copy` — compiler optimizes to `memcpy` for trivial types.

**Overlap (in-place insert gap, erase compaction):** `std::move` / `std::move_backward` — compiler optimizes to `memmove` for trivial types (handles overlap).

* A: compiler optimizes both to memcpy/memmove (trivially copyable)
* B: real copy/move-assigns; C++26 trivial relocation may optimize further
* C: memcpy/memmove of pointers

`if constexpr` splits:

* insert: A raw write, B placement-new, C alloc + placement-new
* erase: A nothing, B dtor, C dtor + dealloc
* destroy leaf: A nothing, B dtor all live, C dtor + dealloc all

## Steps

### Step 1: Benchmark std::move vs memcpy for trivial types ✓ DONE

Tested: uint32_t, uint64_t, array<u64,2>, array<u64,8>
Sizes: N=16, 64, 256, 1024, 4096
Both forward (non-overlapping) and backward (overlapping shift-by-1).

Result: ratios 0.86–1.06 across all cases — pure noise. Compiler generates identical code for std::move and memmove on trivial types.

Conclusion: use std::copy (no overlap, maps to memcpy) and std::move/std::move_backward (overlap, maps to memmove) universally for all three categories. No manual memcpy/memmove calls needed.

### Step 2: Value type classification

Add metaprogramming in kntrie_support.hpp:

* trivially_copyable && sizeof <= 64 → A
* nothrow_move_constructible && sizeof <= 64 → B
* else → C

A normalizes: 1→u8, 2→u16, 3-4→u32, 5-8→u64, 9-64→array<u64,ceil(sz/8)>

### Step 3: VT refactor

Replace current IS_INLINE bool with two bools (IS_INLINE, HAS_DESTRUCTOR). Update VT operations:

* store/load: bit_cast for A, placement-new/move for B, pointer for C
* destroy: noop for A, dtor for B, dtor+dealloc for C
* shift (no overlap): std::copy for all (memcpy for A/C, real copy for B)
* shift (overlap): std::move/std::move_backward for all (memmove for A/C, real move for B)

### Step 4: Leaf operation updates

Update kntrie_compact.hpp insert/erase/destroy to use new VT. Existing `if constexpr (IS_INLINE)` splits become:

* `if constexpr (IS_INLINE && !HAS_DESTRUCTOR)` → A path
* `if constexpr (IS_INLINE && HAS_DESTRUCTOR)` → B path
* `if constexpr (!IS_INLINE)` → C path

### Step 5: Key+Value normalization in kntrie.hpp

kntrie<K,V> normalizes both key and value before forwarding to kntrie_impl. Conversions via bit_cast on the way in and out.

### Step 6: Test & benchmark

* Correctness: existing tests + new tests with string/vector values
* Benchmark: compare B (inline string) vs C (pointer to string) iteration
* Template bloat: verify A deduplication (objdump symbol count)
