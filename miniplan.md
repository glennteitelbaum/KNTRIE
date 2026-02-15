# Miniplan: Tagged Pointers + Bitmask Skip Chains

## Rules
- Update miniplan after each file modification (mark items done)
- Stop after each phase and show a zip of all headers + plan + miniplan (uncompiled)
- Do not compile or test until start of next phase (or Phase 4)

## Phase 1: Tagged pointers (no chains, skip=0 only)
- [x] S1: LEAF_BIT constant
- [x] S2: SENTINEL_TAGGED
- [x] S3: Header skip_ field (move skip count from node[1] byte 7 to header)
- [x] S4: Update prefix_bytes / set_prefix (leaf skip data in node[1] bytes 0-5, count from header)
- [x] S5: Helper functions (tag_leaf, tag_bitmask, untag_leaf, bm_to_node)
- [x] S6: bitmap256::single_bit_index
- [x] K1-K4: root_ type uint64_t, empty(), public API, swap/move/copy
- [x] B1: branchless_find_tagged (read path from bitmap ptr)
- [x] B3: Sentinel → SENTINEL_TAGGED in children arrays
- [x] B4: child_lookup returns tagged uint64_t
- [x] B5: set_child stores tagged uint64_t
- [x] B6: add_child takes tagged child
- [x] B7: remove_child — children are tagged (minimal change)
- [x] B8: for_each_child yields tagged uint64_t
- [x] B9: make_bitmask takes tagged children array
- [x] B11: bitmap256 leaf sentinel updates (N/A - leaves don't use sentinel)
- [x] I1: find_value new hot loop (no sentinel check)
- [x] I2: root_ usage (SENTINEL_TAGGED comparisons)
- [x] I3: insert_result_t/erase_result_t with tagged_ptr
- [x] I4: insert_node_ tagged (leaf + standalone bitmask, SENTINEL check)
- [x] I9: erase_node_ tagged (leaf + standalone, SENTINEL check)
- [x] I14: convert_to_bitmask_tagged_
- [x] I15: split_on_prefix_tagged_
- [x] I16: build_node_from_arrays_ tagged routing
- [x] I17: build_bitmask_from_arrays_ tagged children
- [x] I18: for_each tagged (SENTINEL_TAGGED check)
- [x] I19: destroy tagged (SENTINEL_TAGGED check)
- [x] I20: make_single_leaf_ (unchanged, returns raw, caller tags)
- [x] wrap_bitmask_chain_ updated for tagged pointers (returns tagged bitmask)
**→ DONE: zip all headers + plan + miniplan**

## Phase 2: Bitmask skip chains (insert path)
- [x] Compile + sanity test (fix any Phase 1 issues)
- [x] B10: make_skip_chain (one allocation: embeds + final bitmask)
- [x] I21: wrap_bitmask_chain_ rewritten to use make_skip_chain
- [x] Replace wrap_bitmask_chain_ → skip chain creation
- [x] I5: insert_skip_chain_ (walk embeds, match bytes, operate on final)
- [x] I6: add_child_to_chain_ (in-place or realloc)
- [x] I7: split_skip_at_ (divergent key splits chain)
- [x] I8: build_remainder_tagged_ (extract remainder into new chain)
- [x] erase_skip_chain_ (basic: walk embeds, erase from final, no collapse)
- [x] remove_node_ / collect_stats_ updated for skip chain awareness
**→ DONE: zip all headers + plan + miniplan**

## Phase 3: Erase collapse
- [x] Compile + sanity test (fixed wrap_bitmask_chain_ to handle child already being a skip chain)
- [x] I10: erase_skip_chain_ basic (done in Phase 2 — walk embeds, erase from final)
- [x] I11: collapse_single_child_ (standalone bitmask → absorb bitmask child into skip chain)
- [x] I12: collapse_chain_final_ (chain final drops to 1 → merge into child)
- [x] I13: maybe_coalesce_ / maybe_coalesce_chain_ (subtree → CO leaf)
- [ ] I22: remove_child_from_chain_ (deferred — in-place removal works; coalesce reclaims memory when subtree shrinks below COMPACT_MAX anyway)
- [x] Thread bits through erase path (erase_node_ has bits, erase_skip_chain_ saves orig_bits)
- [x] Verify collapse back to CO leaf: FIXED — skip bytes must be stripped from collected keys and leaf_bits reduced by sc*8 before building leaf
**→ DONE: Phase 3 complete, all ASAN clean, coalesce verified**

## Phase 4: Compile, test, benchmark
- [ ] Compile + fix all errors
- [ ] test_sanity pass
- [ ] test_big pass
- [ ] test_huge pass
- [ ] Benchmark at 100K
**→ STOP: zip all headers + plan + miniplan + benchmark HTML**
