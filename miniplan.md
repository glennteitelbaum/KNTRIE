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
| kntrie_support.hpp | 324 | node_header, tagged ptrs, bitmap256 layout constants, next_narrow_t |
| kntrie_compact.hpp | 612 | CO<NK>: compact leaf ops |
| kntrie_bitmask.hpp | 1092 | BM: bitmask/chain ops (add/remove/build/wrap/collapse) |
| kntrie_ops.hpp | 1173 | Ops<NK>: find + insert + erase + coalesce + collect + build helpers |
| kntrie_impl.hpp | 604 | Iteration (suffix_type dispatch), destroy, stats |
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

- [x] 4A.1-4 + 4B: Full erase path moved to Ops, no CoalesceFn callback.
      - leaf_erase_: sizeof(NK)==1 → BO::bitmap_erase, else → CO::erase
      - erase_node_<BITS>: recursive byte-at-a-time narrowing (no CoalesceFn)
      - erase_leaf_skip_<BITS>: prefix consumption with narrowing
      - erase_chain_skip_<BITS>: embed walk with narrowing
      - erase_final_bitmask_<BITS>: lookup + recurse + coalesce inline
      - do_coalesce_<BITS>: collect entries + build leaf, all in Ops
      - collect_entries_<BITS>: recursive walk with narrowing (leaf/bm skip + children)
      - collect_leaf_skip_<BITS>, collect_bm_skip_<BITS>, collect_bm_children_<BITS>
      - coalesce_build_skip_<BITS>: narrow through skip to find correct NK for build_leaf_
      - Deleted from impl: do_coalesce_, collect_entries_tagged_, build_leaf_from_arrays_,
        leaf_for_each_u64_, suffix_type_for()
      - NK-native arrays: eliminated uint64_t intermediary. collect/build/convert all
        use NK-typed arrays. At narrowing boundaries: child returns NNK[], parent widens.
        leaf_for_each_ returns (NK,VST). build_leaf_ takes NK*. collected_t = {NK[],VST[],count}.
      - impl: 983→604 (−379). ops: 759→1173 (+414). support: 337→324 (−13)

**⏸ STOP**: Present zip. Wait for confirmation. Compile + test + ASAN.

### 4B: Move erase dispatch

- [x] Done as part of 4A above. erase() is 8 lines.

**⏸ STOP**: Present zip. Wait for confirmation. Compile + test + ASAN.

---

## Phase 5: Template iteration on NK

**Goal**: Iteration uses NK. Eliminates 4×25-line leaf dispatch.

- [x] 5.1 combine_suffix_ → inline in leaf helpers with `(IK(suffix) << (IK_BITS - NK_BITS)) >> bits`
- [x] 5.2 descend_min_ → ops (compile-time recursive: leaf_skip_, chain_skip_, bm_final_)
- [x] 5.3 descend_max_ → ops (same structure)
- [x] 5.4 iter_next_node_ → ops (compile-time: leaf_skip_, chain_skip_, bm_final_)
- [x] 5.5 iter_prev_node_ → ops (same structure)
- [x] 5.6 impl iter_first_/last_/next_/prev_ are 4 one-liners calling Ops
      - Deleted: combine_suffix_, descend_min_, descend_max_, iter_next_node_,
        iter_prev_node_, leaf_first_, leaf_last_, leaf_next_dispatch_, leaf_prev_dispatch_
      - Added iter_ops_result_t<IK,VST> to support.hpp as free struct
      - impl: 604→314 (−290). ops: 1173→1706 (+533). support: 324→335 (+11)

**⏸ STOP**: Present zip. Wait for confirmation. Compile + test + ASAN.

---

## Phase 5.5: Simplify node_header + destroy/stats to Ops

**Prerequisite**: ALL suffix_type_ reads eliminated (Phases 3-5).
ALL is_leaf() / set_bitmask() calls replaced by tagged ptr checks.

- [x] 5.5.0 Move destroy_leaf_, remove_node_, collect_stats_ to Ops
      - destroy_leaf_: sizeof(NK)==1 → BO, else → CO (no suffix_type)
      - remove_subtree_<BITS>: recursive with NK narrowing
      - collect_stats_<BITS>: recursive with NK narrowing
      - Deleted from impl: remove_node_, destroy_leaf_, old collect_stats_
      - Removed CO16/CO32/CO64 aliases from impl (no longer used)

- [x] 5.5.1 Verified zero remaining reads of suffix_type/is_leaf/set_bitmask/BITMASK_BIT

- [x] 5.5.2 Replaced node_header struct:
      - Removed flags_ (was: bit 0 = BITMASK_BIT, bits 1-3 = skip)
      - Removed suffix_type_ field
      - Added skip_count_ (direct, no bit extraction) + reserved_
      - Still 8 bytes (sizeof(node_header) == 8)

- [x] 5.5.3 Removed from kntrie_support.hpp:
      - BITMASK_BIT constant, is_leaf(), set_bitmask()
      - suffix_type(), set_suffix_type()

- [x] 5.5.4 Removed set_suffix_type() calls in compact.hpp (line 100)

- [x] 5.5.5 Removed set_bitmask() calls in bitmask.hpp (lines 420, 464)
      - Also removed set_suffix_type(0) calls (lines 839, 860)

- [x] 5.5.6 skip() now reads skip_count_ directly (no bit extraction)

Line counts: impl 314→261, support 335→326, ops 1706→1878

**⏸ STOP**: Present zip. Wait for confirmation. Compile + test + ASAN.

---

## Phase 6: Clean up

- [x] 6.1 Dead code removed from kntrie_impl.hpp:
      - CO16/CO32/CO64 removed (Phase 5.5)
      - All iteration, destroy, stats moved to Ops
      - insert_dispatch_ kept — thin wrapper used by 3 public methods

- [x] 6.2 kntrie_impl.hpp is 261 lines (target ~200, close enough — remainder is
      insert_dispatch_ + debug helpers)

- [x] 6.3 Final audit:
      - `grep 'BITMAP256_U64' kntrie_ops.hpp` → 1 hit (find hot path, intentional)
      - `grep 'suffix_type' *.hpp` → only comments
      - `grep 'is_leaf|set_bitmask|BITMASK_BIT' *.hpp` → only `bool is_leaf` local var

- [x] 6.4 Full test suite + ASAN: PASS

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
| 2026-02-16 | 4A+4B | All | Full erase+coalesce in Ops. No CoalesceFn — do_coalesce_<BITS>, collect_entries_<BITS> return NK-typed collected_t. NK-native arrays throughout: no uint64_t intermediary. build_leaf_ takes NK*. leaf_for_each_ returns (NK,VST). Parent widens at narrowing boundaries. impl 983→604 (−379). ops 759→1173 (+414). support 337→324 (−13) |
| 2026-02-17 | 5 | All | Full iteration in Ops. descend_min/max_, iter_next/prev_node_ all compile-time recursive with NK narrowing. NK ik mirrors find pattern. Leaf helpers: leaf_first/last/next/prev_ with compile-time CO/BO dispatch. Formula: `(IK(suffix) << (IK_BITS - NK_BITS)) >> bits`. impl 604→314 (−290). ops 1173→1706 (+533) |
| 2026-02-17 | 5.5 | All | destroy_leaf_, remove_subtree_<BITS>, collect_stats_<BITS> moved to Ops with NK narrowing. Simplified node_header: removed flags_, suffix_type_, BITMASK_BIT, is_leaf(), set_bitmask(). skip_count_ direct field. Removed CO16/CO32/CO64 from impl. impl 314→261 (−53). support 335→326 (−9). ops 1706→1878 (+172) |
| 2026-02-17 | 6 | All | Audit clean. All suffix_type/is_leaf/set_bitmask eliminated. impl 261 lines. |
