# kntrie refactor — miniplan

## CRITICAL RULES

**STOP AND PRESENT**: After EVERY phase (before compiling/testing),
stop, present a zip of ALL header files, and wait for confirmation
to continue.

**UPDATE THIS FILE**: At every file change — even minor ones — update
this miniplan with what was done. Mark steps with [DONE], add notes
about issues encountered. This file is the single source of truth
for progress. If the chat gets compressed, check this file and the
transcript before doing anything.

**NEVER REVERT**: If something breaks, stop and ask questions.

---

## Current file inventory

| File | Lines | Role |
|------|-------|------|
| kntrie_support.hpp | 307 | node_header, tagged ptrs, bitmap256 layout constants |
| kntrie_compact.hpp | ~600 | CO<NK>: compact leaf ops |
| kntrie_bitmask.hpp | 816 | BM: bitmask node ops, bitmap leaf ops |
| kntrie_impl.hpp | 1942 | Everything else (find, insert, erase, iter, build) |
| kntrie.hpp | 227 | Public API, KEY→UK, iterators |

---

## Phase 1: Create kntrie_ops.hpp, move find_ops

**Goal**: New file with find only. Zero behavior change.

- [x] 1.1 Create `kntrie_ops.hpp` with include guard, includes — DONE
- [x] 1.2 Define `template<typename NK, typename VALUE, typename ALLOC> struct kntrie_ops` — DONE
      with type aliases: BO, VT, VST, CO, NK_BITS, NNK, Narrow
- [x] 1.3 Move `find_node_<BITS>` (two overloads) into ops — DONE
      - Removed `template<typename NK>`, NK is struct param
      - Narrowing calls `Narrow::template find_node_<BITS-8>` instead of `find_node_<BITS-8, NNK>`
- [x] 1.4 Move `find_leaf_<BITS>` (two overloads) into ops — DONE
      - BITS>8 version uses `CO::find` (struct-level CO = compact_ops<NK, V, A>)
      - BITS==8 version uses `BO::bitmap_find`
- [x] 1.5 Update `kntrie_impl.hpp`: — DONE
      - Changed include to `#include "kntrie_ops.hpp"` (replaces compact+bitmask includes)
      - Added `using NK0 = ...` and `using Ops = kntrie_ops<NK0, VALUE, ALLOC>` as class-level aliases
      - Changed `find_value` to `Ops::template find_node_<KEY_BITS>(...)`
      - Removed entire `find_ops` struct (~77 lines)
      - Also added `next_narrow_t<NK>` to kntrie_support.hpp
- [x] 1.6 Verify include chain: kntrie.hpp → impl → ops → bitmask + compact — DONE

**⏸ STOP**: Present zip of all headers. Wait for confirmation.
Compile: `g++ -std=c++23 -O2 -march=x86-64-v3 -fsanitize=address`
Run: all tests.

---

## Phase 1.5: Add BM read accessors

**Goal**: BM gets public methods for skip chain + bitmask access.
Replace raw layout math in impl. Zero behavior change.

- [ ] 1.5.1 Add private helpers to bitmask_ops:
      ```cpp
      static constexpr size_t chain_hs_(uint8_t sc) noexcept {
          return 1 + static_cast<size_t>(sc) * 6;
      }
      static void fix_embeds_(uint64_t* node, uint8_t sc) noexcept { ... }
      ```

- [ ] 1.5.2 Add skip chain read accessors:
      - `skip_byte(node, e)` — read single skip byte at embed position
      - `skip_bytes(node, sc, out)` — copy all skip bytes to buffer
      - `chain_lookup(node, sc, idx)` → calls `lookup_at_` with chain_hs_
      - `chain_child(node, sc, slot)` — read tagged child at slot
      - `chain_set_child(node, sc, slot, tagged)` — write child at slot
      - `chain_desc_array(node, sc)` — const + mutable versions
      - `chain_for_each_child(node, sc, cb)` — iterate final bitmap children

- [ ] 1.5.3 Add tagged pointer accessors for iteration:
      - `bitmap_ref(bm_tagged)` → bitmap256 const ref from tagged ptr
      - `child_at(bm_tagged, slot)` → tagged child at slot
      - `first_child(bm_tagged)` → tagged child at slot 0

- [ ] 1.5.4 Refactor existing `lookup` to use private `lookup_at_(node, hs, idx)`:
      - Extract core into `lookup_at_(node, hs, idx)` private method
      - `lookup(node, idx)` calls `lookup_at_(node, 1, idx)`
      - `chain_lookup` calls `lookup_at_(node, chain_hs_(sc), idx)`

- [ ] 1.5.5 Replace raw BM access in `kntrie_impl.hpp` iteration functions:
      - `iter_next_node_`: replace `bm[BITMAP256_U64 + 1 + slot]` → `BO::child_at(ptr, slot)`
      - `iter_prev_node_`: same
      - `descend_min_`: replace `bm[BITMAP256_U64 + 1]` → `BO::first_child(ptr)` etc.
      - `descend_max_`: same
      - Replace `reinterpret_cast<const bitmap256*>(bm)` → `BO::bitmap_ref(ptr)`

- [ ] 1.5.6 Replace raw BM access in `insert_skip_chain_`:
      - Skip byte reads: `BO::skip_byte(node, e)` instead of `node+1+e*6` + `single_bit_index()`
      - Final bitmap lookup: `BO::chain_lookup(node, sc, byte)`
      - Child read/write: `BO::chain_child(node, sc, slot)`, `BO::chain_set_child(...)`
      - Desc access: `BO::chain_desc_array(node, sc)`

- [ ] 1.5.7 Replace raw BM access in `erase_skip_chain_`:
      - Same pattern as insert: skip_byte, chain_lookup, chain_child, chain_desc_array

- [ ] 1.5.8 Replace raw BM access in `collect_entries_tagged_`, `remove_node_`, `collect_stats_`:
      - Skip byte reads + final bitmap iteration via `BO::chain_for_each_child(node, sc, cb)`

**⏸ STOP**: Present zip of all headers. Wait for confirmation.
Compile + test + ASAN.

---

## Phase 2: Move helpers to ops + chain mutations to BM

**Goal**: Stateless helpers move to ops. BM layout operations move to BM.

### 2A: Refactor BM add/remove to shared core

- [ ] 2A.1 Extract `add_child` core into private `add_child_at_(node, h, hs, ...)`
      - Current `add_child` becomes `add_child_at_` with hs parameter
      - Public `add_child(...)` wraps: `return add_child_at_(node, h, 1, ...)`

- [ ] 2A.2 Extract `remove_child` core into private `remove_child_at_(node, h, hs, ...)`
      - Current `remove_child` becomes `remove_child_at_` with hs parameter
      - Public `remove_child(...)` wraps: `return remove_child_at_(node, h, 1, ...)`

- [ ] 2A.3 Add chain wrappers:
      - `chain_add_child(node, h, sc, ...)` → `add_child_at_` + `fix_embeds_`
      - `chain_remove_child(node, h, sc, ...)` → `remove_child_at_` + `fix_embeds_`

- [ ] 2A.4 Replace `add_child_to_chain_` in impl with `BO::chain_add_child` calls
      - Delete `add_child_to_chain_` from impl (~75 lines)

- [ ] 2A.5 Replace erase chain removal block in impl with `BO::chain_remove_child` calls
      - Delete the ~61-line block in `erase_skip_chain_`

**⏸ STOP**: Present zip. Wait for confirmation. Compile + test + ASAN.

### 2B: Move chain structural ops to BM

- [ ] 2B.1 Move `build_remainder_tagged_` (impl:913-954) → `BO::build_remainder`
      - Add `ALLOC& alloc` parameter
      - Replace raw offset math with internal BM helpers

- [ ] 2B.2 Move `wrap_bitmask_chain_` (impl:1536-1569) → `BO::wrap_in_chain`
      - Add `ALLOC& alloc` parameter

- [ ] 2B.3 Add `BO::chain_collapse_info(node, sc)` and `BO::standalone_collapse_info(node)`
      - Extract sole-child info gathering from erase paths
      - Delete the ~28-line collapse block in `erase_skip_chain_`
      - Delete the ~22-line standalone collapse in `erase_node_`

- [ ] 2B.4 Update all call sites in impl to use new BM methods

**⏸ STOP**: Present zip. Wait for confirmation. Compile + test + ASAN.

### 2C: Move NK-independent helpers to ops

- [ ] 2C.1 Move descriptor helpers to ops (add `ALLOC&` where needed):
      - `tagged_count_` (line 1266)
      - `sum_children_desc_` (line 1274)
      - `set_desc_capped_` (line 1290)
      - `inc_descendants_` (line 1296)
      - `dec_or_recompute_desc_` (line 1303)
      - `sum_tagged_array_` (line 1318)

- [ ] 2C.2 Move skip/node helpers to ops:
      - `prepend_skip_` (line 1475) — add ALLOC& param
      - `remove_skip_` (line 1512) — add ALLOC& param

- [ ] 2C.3 Move destroy/cleanup to ops:
      - `remove_node_` (line 1868) — add ALLOC& param
      - `destroy_leaf_` (line 1893) — add ALLOC& param
      - `dealloc_bitmask_subtree_` (line 1416) — add ALLOC& param

- [ ] 2C.4 Move `iter_result_t` to kntrie_support.hpp, template on `<IK, VALUE>`

- [ ] 2C.5 Update all call sites in impl to use `Ops::xxx(args..., alloc_)`

- [ ] 2C.6 Update `remove_all_` and destructor to call `Ops::remove_node_`

**⏸ STOP**: Present zip. Wait for confirmation. Compile + test + ASAN.

---

## Phase 3: Template insert on NK

**Goal**: Insert path uses NK struct-level template. Eliminates suffix_type dispatch.

### 3A: Move leaf/build helpers

- [ ] 3A.1 Move `make_single_leaf_` (impl:1577) → `Ops::make_single_leaf_`
      - IK param → NK param
      - Use `CO::make_leaf(...)` instead of suffix_type dispatch

- [ ] 3A.2 Move `leaf_for_each_u64_` (impl:1652) → `Ops::leaf_for_each_aligned_`
      - Replace 4-way suffix_type dispatch with:
        `if constexpr (sizeof(NK)==1) BO::for_each_bitmap else CO::for_each`

- [ ] 3A.3 Move `convert_to_bitmask_tagged_` (impl:1606) → ops
      - Use `leaf_for_each_aligned_` instead of `leaf_for_each_u64_`

- [ ] 3A.4 Move `build_node_from_arrays_tagged_` (impl:1684) → ops
      - Template on NK, narrowing via `Narrow::build_node_from_arrays_tagged_`

- [ ] 3A.5 Move `build_bitmask_from_arrays_tagged_` (impl:1759) → ops
      - Template on NK for child subtree construction

- [ ] 3A.6 Move `split_on_prefix_tagged_` (impl:1797) → ops

- [ ] 3A.7 Move `split_skip_at_` (impl:861) → ops
      - Use `BO::build_remainder`, `BO::wrap_in_chain` instead of raw layout
      - Use `BO::skip_byte` / `BO::skip_bytes` instead of raw embed access

**⏸ STOP**: Present zip. Wait for confirmation. Compile + test + ASAN.

### 3B: Move insert dispatch

- [ ] 3B.1 Move `leaf_insert_` (impl:687) → `Ops::leaf_insert_`
      - Replace suffix_type dispatch with `CO::insert`

- [ ] 3B.2 Merge `insert_skip_chain_` logic into `insert_node_`
      - Single function, skip loop uses BO:: accessors
      - Use `BO::chain_add_child` for child addition

- [ ] 3B.3 Move `insert_node_` (impl:610) → `Ops::insert_node_<INSERT, ASSIGN>`
      - IK param → NK param
      - Add narrowing: `Narrow::template insert_node_<INSERT, ASSIGN>` at boundary
      - All leaf dispatch via CO (compile-time)
      - All skip chain ops via BO methods

- [ ] 3B.4 Thin down `insert_dispatch_` in impl to ~10 lines:
      - Convert IK → NK0, call `Ops::template insert_node_<INSERT, ASSIGN>`

**⏸ STOP**: Present zip. Wait for confirmation. Compile + test + ASAN.

---

## Phase 4: Template erase on NK

**Goal**: Erase path uses NK struct-level template.

### 4A: Move erase helpers

- [ ] 4A.1 Move `build_leaf_from_arrays_` (impl:1436) → ops
      - Template on NK, use `CO::make_leaf`

- [ ] 4A.2 Move `collect_entries_tagged_` (impl:1363) → ops
      - Template on NK, narrowing via `Narrow::collect_entries_tagged_`
      - Use `BO::chain_for_each_child` instead of raw access

- [ ] 4A.3 Move `do_coalesce_` (impl:1328) → ops
      - Template on NK, calls collect_entries + build_leaf

- [ ] 4A.4 Move `leaf_erase_` (impl:1223) → ops
      - Replace suffix_type dispatch with `CO::erase`

**⏸ STOP**: Present zip. Wait for confirmation. Compile + test + ASAN.

### 4B: Move erase dispatch

- [ ] 4B.1 Merge `erase_skip_chain_` into `erase_node_`
      - Skip loop uses `BO::skip_byte`, `BO::chain_lookup`
      - Child removal via `BO::chain_remove_child`
      - Collapse via `BO::chain_collapse_info`

- [ ] 4B.2 Move `erase_node_` (impl:964) → `Ops::erase_node_`
      - IK param → NK param
      - Add narrowing at boundary
      - All leaf dispatch via CO, all BM ops via BO

- [ ] 4B.3 Thin down erase in impl to ~8 lines:
      - Convert IK → NK0, call `Ops::erase_node_`

**⏸ STOP**: Present zip. Wait for confirmation. Compile + test + ASAN.

---

## Phase 5: Template iteration on NK

**Goal**: Iteration uses NK. Eliminates 4×25-line leaf dispatch.

- [ ] 5.1 Move `combine_suffix_` (impl:380) → ops
      - Compile-time shift based on NK, no suffix_type switch

- [ ] 5.2 Move `descend_min_` (impl:392) → ops
      - Template on `<IK>` (IK is method-level, NK is struct-level)
      - Use `BO::bitmap_ref`, `BO::first_child`, `BO::child_at`
      - Leaf dispatch: `CO::iter_first` or `BO::bitmap_iter_first`
      - Narrowing via `Narrow::template descend_min_<IK>`

- [ ] 5.3 Move `descend_max_` (impl:417) → ops
      - Same pattern as descend_min_

- [ ] 5.4 Move `iter_next_node_` (impl:273) → ops
      - Template on `<IK>`
      - Use BO:: accessors for bitmask child access
      - Leaf dispatch via CO/BO compile-time
      - Narrowing via `Narrow::template iter_next_node_<IK>`

- [ ] 5.5 Move `iter_prev_node_` (impl:326) → ops
      - Same pattern

- [ ] 5.6 Thin down iter_first_/last_/next_/prev_ in impl to 4 one-liners:
      - Each calls `Ops::template descend_min_<IK>(...)` etc.

**⏸ STOP**: Present zip. Wait for confirmation. Compile + test + ASAN.

---

## Phase 5.5: Simplify node_header

**Prerequisite**: ALL suffix_type_ reads eliminated (Phases 3-5).
ALL is_leaf() / set_bitmask() calls replaced by tagged ptr checks.

- [ ] 5.5.1 Grep to verify zero remaining reads of:
      - `suffix_type()` / `set_suffix_type()`
      - `is_leaf()` / `set_bitmask()` / `BITMASK_BIT`

- [ ] 5.5.2 Replace `node_header` struct in kntrie_support.hpp:
      ```cpp
      struct node_header {
          uint16_t skip_count_  = 0;  // max 6, bits 3-15 free
          uint16_t entries_     = 0;
          uint16_t alloc_u64_   = 0;
          uint16_t total_slots_ = 0;
      };
      ```

- [ ] 5.5.3 Remove from kntrie_support.hpp:
      - `BITMASK_BIT` constant
      - `is_leaf()`, `set_bitmask()` methods
      - `suffix_type()`, `set_suffix_type()` methods
      - `suffix_type_for()` function (if present)

- [ ] 5.5.4 Update kntrie_compact.hpp:
      - Remove `set_suffix_type()` calls in make_leaf, grow_and_insert, etc.

- [ ] 5.5.5 Update kntrie_bitmask.hpp:
      - Remove `set_bitmask()` calls in make_bitmask, make_skip_chain, etc.

- [ ] 5.5.6 Update skip accessors:
      - `skip()` → reads `skip_count_` directly (uint16, no bit extraction)
      - `set_skip()` → writes `skip_count_` directly
      - `is_skip()` → `skip_count_ != 0`

**⏸ STOP**: Present zip. Wait for confirmation. Compile + test + ASAN.

---

## Phase 6: Clean up

- [ ] 6.1 Remove dead code from kntrie_impl.hpp:
      - All moved functions should be gone
      - Remove `CO16/CO32/CO64` type aliases
      - Remove `leaf_insert_`, `leaf_erase_` (replaced by CO::insert/erase)
      - Remove `leaf_for_each_u64_` (replaced by ops leaf_for_each_aligned_)
      - Remove `insert_dispatch_` if inlined

- [ ] 6.2 Verify kntrie_impl.hpp is ~200 lines

- [ ] 6.3 Final audit:
      - `grep 'BITMAP256_U64\|node + 1 + .*\* 6\|final_offset' kntrie_ops.hpp`
        → should find zero results (all BM layout in BM)
      - `grep 'suffix_type' *.hpp` → should only find comments/docs
      - `grep 'is_leaf\|set_bitmask\|BITMASK_BIT' *.hpp` → zero results

- [ ] 6.4 Run full test suite + ASAN + benchmarks

**⏸ STOP**: Present zip. Wait for confirmation.

---

## Progress log

| Date | Phase | Step | Notes |
|------|-------|------|-------|
| 2026-02-16 | 1 | 1.1-1.6 | Created kntrie_ops.hpp, moved find_ops, added next_narrow_t to support |
