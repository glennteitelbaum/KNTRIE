# Miniplan — Progress Tracker

## Rules
- After each item: STOP, zip headers, show plan.md + miniplan.md, WAIT.
- If compressed: READ THIS FIRST. Do not start over.
- Never revert. If stuck, ask.

## Status

### Phase 1: Descendants simplification
- [ ] Remove per-child u16 desc array from bitmask nodes
- [ ] Remove desc_u64(), child_desc_array(), desc_array_ accessors
- [ ] Remove COALESCE_CAP, inc_descendants_, dec_or_recompute_desc_,
      sum_children_desc_, sum_tagged_array_, tagged_count_
- [ ] Remove descendants()/set_descendants() header repurposing
- [ ] Add single uint64_t at end of bitmask node: `[hdr][bm][sent][children(n)][desc 1×u64]`
- [ ] bitmask_size_u64 = hu + 5 + n_children + 1
- [ ] Add descendants_ptr(node) accessor
- [ ] Insert: ++(*descendants_ptr) on child insert
- [ ] Erase: --(*descendants_ptr), check <= COMPACT_MAX for coalesce
- [ ] Build/make paths: initialize descendants from child entry sum
- [ ] Update add_child/remove_child (no desc array to shift)
- [ ] Update chain variants similarly
**Files:** kntrie_support.hpp, kntrie_bitmask.hpp, kntrie_ops.hpp, kntrie_impl.hpp

### Phase 2: open_gap / close_gap + value_traits usage fixes
- [ ] Add open_gap, close_gap to value_traits (kntrie_support.hpp)
- [ ] Fix erase_create_dup_: init_slot destroyed, write_slot live (kntrie_compact.hpp)
- [ ] Fix insert_dispatch_: no explicit destroy on B stack locals (kntrie_impl.hpp)
- [ ] Fix bitmap_insert in-place: open_gap + init_slot (kntrie_bitmask.hpp)
- [ ] Fix bitmap_erase in-place: category-aware close_gap (kntrie_bitmask.hpp)
- [ ] Fix dedup_skip_into_: init_slot not raw assign (kntrie_compact.hpp)
- [ ] Audit seed_from_real_ dup writes use init_slot (kntrie_compact.hpp)
- [ ] Coalesce collect: move values out, destroy_relocated on old leaves (kntrie_impl.hpp)
**Files:** kntrie_support.hpp, kntrie_compact.hpp, kntrie_bitmask.hpp, kntrie_impl.hpp

### Phase 3: Dead code removal + branch hints
- [ ] Remove ts==0 guards from for_each, iter_first/last/next/prev,
      destroy_and_dealloc (kntrie_compact.hpp)
- [ ] Comprehensive [[likely]]/[[unlikely]] on all runtime branches:
      insert, erase, split, coalesce, dispatch paths (all files)
      Key rules: leaf=unlikely, skip=unlikely, found=likely, realloc=unlikely,
      split=unlikely, collapse=unlikely, in-place=likely
**Files:** all .hpp files

## Done
- Phase 1: Already implemented in current code. Single u64 descendants,
  no per-child desc array, no COALESCE_CAP. Fixed one stale comment.
- Phase 2: All value_traits fixes applied:
  - kntrie_support.hpp: A/B/C categories, IS_TRIVIAL, HAS_DESTRUCTOR,
    init_slot, open_gap, close_gap, copy_uninit, move_uninit, destroy_all
  - kntrie_compact.hpp: init_slot for all fresh-node writes, HAS_DESTRUCTOR
    guards, write_slot for live slots, dedup_skip_into fixed
  - kntrie_bitmask.hpp: open_gap+init_slot insert, close_gap erase,
    HAS_DESTRUCTOR guards
  - kntrie_impl.hpp: C-only explicit destroy in insert_dispatch (correct)
  - kntrie_ops.hpp: dealloc_bitmask_subtree now templated with BITS
    for proper NK narrowing — B-type destroy works, C-type skips destroy
  - Fixed missing leaf_destroy_values: replaced with templated approach
    using dealloc_leaf_skip/dealloc_bm_chain_skip/dealloc_bm_final

## Current phase: Phase 3 — Dead code removal + branch hints
- Working on: (not started yet)
