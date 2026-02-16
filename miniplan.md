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
| kntrie_support.hpp | 337 | node_header, tagged ptrs, bitmap256 layout constants, next_narrow_t |
| kntrie_compact.hpp | 612 | CO<NK>: compact leaf ops |
| kntrie_bitmask.hpp | 1092 | BM: bitmask/chain ops (add/remove/build/wrap/collapse) |
| kntrie_ops.hpp | 759 | Ops<NK>: find + insert + desc/skip/dealloc helpers + build/convert |
| kntrie_impl.hpp | 983 | Erase, iter, build_leaf_from_arrays, leaf_for_each_u64 |
| kntrie.hpp | 240 | Public API, KEY→UK, iterators |

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

- [x] 1.5.1 Add private helpers to bitmask_ops — DONE
      - `chain_hs_` (made public, needed by impl too), `lookup_at_`

- [x] 1.5.2 Add skip chain read accessors — DONE
      - `skip_byte`, `skip_bytes`, `chain_lookup`, `chain_child`, `chain_set_child`
      - `chain_desc_array`, `chain_desc_array_mut`, `chain_bitmap`, `chain_child_count`
      - `chain_children`, `chain_children_mut`, `chain_for_each_child`
      - `embed_child`, `set_embed_child`

- [x] 1.5.3 Add tagged pointer accessors for iteration — DONE
      - `bitmap_ref(bm_tagged)`, `child_at(bm_tagged, slot)`, `first_child(bm_tagged)`

- [x] 1.5.4 Refactor existing `lookup` to use private `lookup_at_(node, hs, idx)` — DONE
      - `lookup` → `lookup_at_(node, 1, idx)`
      - `chain_lookup` → `lookup_at_(node, chain_hs_(sc), idx)`

- [x] 1.5.5 Replace raw BM access in iteration functions — DONE
      - iter_next_node_: bitmap_ref, child_at (+ template keyword for find_slot)
      - iter_prev_node_: same
      - descend_min_: bitmap_ref, first_child
      - descend_max_: bitmap_ref, child_at

- [x] 1.5.6 Replace raw BM access in `insert_skip_chain_` — DONE
      - Skip byte loop → BO::skip_byte
      - Final lookup → BO::chain_lookup
      - Child write → BO::chain_set_child
      - Desc access → BO::chain_desc_array_mut

- [x] 1.5.7 Replace raw BM access in `erase_skip_chain_` — DONE
      - Skip byte loop → BO::skip_byte
      - Final lookup → BO::chain_lookup
      - Child access → BO::chain_set_child, chain_children_mut
      - Desc → BO::chain_desc_array_mut
      - Collapse → BO::chain_bitmap, BO::skip_bytes
      - Removal path: kept raw final_offset/real_ch as locals (moves to BM in Phase 2B)

- [x] 1.5.8 Replace raw BM access in collect/remove/stats — DONE
      - collect_entries_tagged_: BO::skip_byte, BO::chain_bitmap, BO::chain_children
      - dealloc_bitmask_subtree_: BO::chain_for_each_child
      - remove_node_: BO::chain_for_each_child
      - collect_stats_: BO::chain_for_each_child
      - sum_children_desc_: BO::chain_desc_array

**⏸ STOP**: Present zip of all headers. Wait for confirmation.
Compile + test + ASAN.

---

## Phase 2: Move helpers to ops + chain mutations to BM

**Goal**: Stateless helpers move to ops. BM layout operations move to BM.

### 2A: Refactor BM add/remove to shared core

- [x] 2A.1 Extract `add_child` core into private `add_child_at_(node, h, hs, ...)` — DONE
      - Generalized hs param, prefix_u64 copy for realloc includes embeds
- [x] 2A.2 Extract `remove_child` core into private `remove_child_at_(node, h, hs, ...)` — DONE
      - Same generalization
- [x] 2A.3 Add chain wrappers + fix_embeds_ — DONE
      - `chain_add_child` → `add_child_at_` + `fix_embeds_` on realloc
      - `chain_remove_child` → `remove_child_at_` + `fix_embeds_` on realloc
      - `fix_embeds_` fixes embed internal pointers + final sentinel
      - `chain_hs_` made public (needed by impl)
- [x] 2A.4 Replace `add_child_to_chain_` with `BO::chain_add_child` — DONE
      - Deleted ~80-line function from impl
- [x] 2A.5 Replace erase chain removal block with `BO::chain_remove_child` — DONE
      - Deleted ~50-line block, replaced with single call + nc check

**⏸ STOP**: Present zip. Wait for confirmation. Compile + test + ASAN.

### 2B: Move chain structural ops to BM

- [x] 2B.1 Move `build_remainder_tagged_` → `BO::build_remainder` — DONE
      - Dropped hdr param (reads from node header internally)
      - Uses chain_bitmap, chain_children, chain_desc_array, skip_byte internally
- [x] 2B.2 Move `wrap_bitmask_chain_` → `BO::wrap_in_chain` — DONE
      - Uses skip_bytes, chain_bitmap, chain_children, chain_desc_array internally
      - Deallocates old child, returns tagged pointer
- [x] 2B.3 Add `collapse_info` struct + extractors — DONE
      - `BO::chain_collapse_info(node, sc)` for skip chain collapse
      - `BO::standalone_collapse_info(node)` for standalone collapse
      - Both return {sole_child, bytes[], total_skip, sole_entries}
      - Replaced ~20-line standalone collapse in erase_node_
      - Replaced ~20-line chain collapse in erase_skip_chain_
- [x] 2B.4 Update all call sites in impl — DONE
      - 1 build_remainder call, 6 wrap_in_chain calls, 2 collapse blocks

**⏸ STOP**: Present zip. Wait for confirmation. Compile + test + ASAN.

### 2C: Move NK-independent helpers to ops

- [x] 2C.1 Move descriptor helpers to ops — DONE
      - sum_children_desc_, set_desc_capped_, inc_descendants_, dec_or_recompute_desc_
      - tagged_count and sum_tagged_array were already in support.hpp
- [x] 2C.2 Move skip/node helpers to ops — DONE
      - prepend_skip_(+alloc), remove_skip_(+alloc), dealloc_bitmask_subtree_(+alloc)
- [~] 2C.3 Move destroy/cleanup to ops — DEFERRED
      - remove_node_ and destroy_leaf_ have suffix_type dispatch (needs KEY_BITS)
      - Will move naturally when insert/erase are NK-templated (Phase 3+)
- [~] 2C.4 Move iter_result_t — DEFERRED (uses KEY type, minor)
- [x] 2C.5 Update all call sites in impl — DONE
      - All helpers now called as Ops::xxx(args..., alloc_)
- [~] 2C.6 Update remove_all_/destructor — DEFERRED (depends on remove_node_ staying in impl)

**⏸ STOP**: Present zip. Wait for confirmation. Compile + test + ASAN.

---

## Phase 3: Template insert on NK

**Goal**: Insert path uses NK struct-level template. Eliminates suffix_type dispatch.

### 3A: Move leaf/build helpers

- [x] 3A.1 `make_single_leaf_` → `Ops::make_single_leaf_(NK suffix, VST, ALLOC&)` — DONE
      - sizeof(NK)==1 → BO::make_single_bitmap, else → CO::make_leaf
- [x] 3A.2 `leaf_for_each_u64_` → `Ops::leaf_for_each_aligned_` — DONE
      - sizeof(NK)==1 → BO::for_each_bitmap, else → CO::for_each
      - Shift is constexpr (64 - NK_BITS)
- [x] 3A.3 `convert_to_bitmask_tagged_` → `Ops::convert_to_bitmask_tagged_` — DONE
      - Takes NK suffix instead of IK, uses leaf_for_each_aligned_
- [x] 3A.4 `build_node_from_arrays_tagged_` → `Ops::` — DONE
      - Added build_leaf_ helper (NK compile-time dispatch)
      - Narrowing: `if (bits-8 <= NK_BITS/2) Narrow::build_node_...`
- [x] 3A.5 `build_bitmask_from_arrays_tagged_` → `Ops::` — DONE
      - Same narrowing pattern as 3A.4

- [x] 3A.6 `split_on_prefix_` → `Ops::split_on_prefix_<BITS>` — DONE
      - Recursive `make_leaf_descended_<BITS>` for NK narrowing
      - `insert_leaf_skip_<BITS>` consumes prefix bytes one-at-a-time with narrowing
- [x] 3A.7 `split_skip_at_` → `Ops::split_skip_at_<BITS>` — DONE
      - Same narrowing pattern via `insert_chain_skip_<BITS>`

**⏸ STOP**: Present zip. Wait for confirmation. Compile + test + ASAN.

### 3B: Move insert dispatch

- [x] 3B.1 `leaf_insert_` → `Ops::leaf_insert_<BITS,INSERT,ASSIGN>` — DONE
      - sizeof(NK)==1 → BO::bitmap_insert, else → CO::insert
      - needs_split → convert_to_bitmask_tagged_ (now in Ops)
- [x] 3B.2 Merged `insert_skip_chain_` into recursive `insert_chain_skip_<BITS>` — DONE
      - Byte-at-a-time recursion with narrowing at NK/2 boundaries
- [x] 3B.3 `insert_node_` → `Ops::insert_node_<BITS,INSERT,ASSIGN>` — DONE
      - NK from struct, BITS template param
      - Leaf prefix walk: `insert_leaf_skip_<BITS>` recursive
      - Chain walk: `insert_chain_skip_<BITS>` recursive
      - Final bitmask: `insert_final_bitmask_<BITS>` with narrowing
- [x] 3B.4 `insert_dispatch_` thinned to 10 lines — DONE
      - IK→NK0 conversion, delegates to `Ops::insert_node_<KEY_BITS,...>`
- [x] 3B.5 Deleted all old insert code from impl — DONE
      - insert_node_, leaf_insert_, insert_skip_chain_, split_skip_at_
      - make_single_leaf_ (old IK), convert_to_bitmask_tagged_ (old)
      - split_on_prefix_tagged_, build_node/bitmask_from_arrays_tagged_ (old)
      - impl: 1456→983 (−473 lines)

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
| 2026-02-16 | 1.5 | 1.5.1-1.5.8 | BM read accessors + chain methods. Replaced ~30 raw layout accesses in impl. Tests pass + ASAN |
| 2026-02-16 | 2A | 2A.1-2A.5 | add_child_at_/remove_child_at_/fix_embeds_ cores. chain_add/remove_child. Deleted ~130 lines from impl |
| 2026-02-16 | 2B | 2B.1-2B.4 | build_remainder, wrap_in_chain, collapse_info moved to BM. Impl -129 lines |
| 2026-02-16 | 2C | 2C.1-2C.5 | Desc helpers, skip helpers, dealloc_bitmask_subtree_ moved to Ops. Impl -116 lines. Deferred: remove_node_/destroy_leaf_ (NK-dependent) |
| 2026-02-16 | 3A | 3A.1-3A.5 | NK-dependent helpers added to Ops: make_single_leaf_, leaf_for_each_aligned_, build_leaf_, build_node_from_arrays_tagged_, build_bitmask_from_arrays_tagged_, convert_to_bitmask_tagged_. Narrowing at NK/2 boundaries. Deferred 3A.6-7 (runtime prefix consumption) |
| 2026-02-16 | 3A+3B | 3A.6-7, 3B.1-5 | Full insert path moved to Ops with recursive byte-at-a-time narrowing. split_on_prefix_, split_skip_at_, insert_node_, leaf_insert_, insert_chain_skip_, insert_final_bitmask_ all in Ops. Old IK-based code deleted. impl 1456→983 (−473). ops 387→759 (+372) |
