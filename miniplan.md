# Miniplan: Implementation by File

## PROCESS RULES
1. Work on one file at a time, in order (1→2→3→4→5).
2. For each TODO item: write/change the code, save the file.
3. After saving: update miniplan.md — move the completed TODO item to KEEP.
4. When a file has zero TODO items remaining: STOP. Create a zip of ALL header files, benchmark.cpp, bench_jsx.cpp, miniplan.md, and plan.md. Wait for user to say continue.
5. Do not start the next file until user says continue.
6. If unsure about anything, ask — do not guess.

---

## FILE 1: kntrie_support.hpp ✅ COMPLETE

### KEEP
- Constants: BITMAP256_U64, COMPACT_MAX, BOT_LEAF_MAX, HEADER_U64
- round_up_u64, step_up_u64, should_shrink_u64
- prefix_t (std::array<uint16_t, 2>)
- SENTINEL_NODE (alignas(64) constinit)
- value_traits<VALUE, ALLOC> (IS_INLINE, slot_type, store/as_ptr/destroy/write_slot)
- alloc_node, dealloc_node helpers
- erase_result_t
- node_header: suffix_type() / set_suffix_type() using bits 14-13
- key_ops<KEY>: internal_key_t, IK_BITS, to_internal(), to_key()
- suffix_type_for(int bits): returns 1/2/3
- dispatch_suffix(uint8_t stype, Fn): nested bit test
- slot_table<K, VST>: templated on K

### REMOVED
- suffix_traits<BITS>
- KeyOps<KEY>
- InsertResult

---

## FILE 2: kntrie_compact.hpp ✅ COMPLETE (already updated in prior session)

### KEEP
- jump_search<K> — identical logic
- All compact leaf layout logic (header + sorted keys + aligned values + dups)
- Dup seeding: seed_from_real, seed_with_insert, seed_with_skip
- insert_consume_dup, erase_create_dup
- for_each, destroy_and_dealloc
- compact_ops<KEY,VALUE,ALLOC> templated on K (not BITS)
- size_u64<K>, total_slots<K>, keys<K>, vals<K> (public)
- make_leaf<K> with stype parameter
- find<K>, insert<K>, erase<K> take K suffix directly
- compact_insert_result_t: {node, inserted, needs_split}

### REMOVED
- All extract_suffix / extract_top8 calls (caller provides K suffix)

---

## FILE 3: kntrie_bitmask.hpp ✅ COMPLETE (already updated in prior session, overflow removed)

### KEEP
- bitmap256 struct entirely: has_bit, set_bit, clear_bit, popcount, find_slot<MODE>, find_next_set, arr_* helpers, from_indices
- split_ops<KEY,VALUE,ALLOC>: layout 10+n, branchless_top_child→{child,is_leaf}, lookup_child, set_child, add_child_as_leaf/internal, remove_child, mark_internal, make_split, for_each_child, dealloc, child_count
- fan_ops<KEY,VALUE,ALLOC>: layout 6+n, branchless_child→child only, lookup_child, set_child, add_child, remove_child, make_fan, for_each_child, dealloc, child_count
- bitmap_leaf_ops<KEY,VALUE,ALLOC>: layout 5+ceil, find, insert (INSERT/ASSIGN), erase, make_single, make_from_sorted, for_each, destroy_and_dealloc, dealloc_only
- insert_result_t: {node, inserted} — no overflow field

### REMOVED
- BitmaskOps monolith
- overflow field from bitmap_leaf_ops::insert_result_t (bitmap256 never overflows)

---

## FILE 4: kntrie_impl.hpp

### KEEP (structural shape only)
- Class shell: kntrie_impl<KEY, VALUE, ALLOC> (was kntrie3)
- root_[256], size_, alloc_ members
- Constructor (fill root_ with SENTINEL_NODE), destructor calls remove_all
- empty(), size(), clear()

### TODO — Find
- Loop-based find_value: root index → root fan descend → main loop
- compact_find: nested bit test dispatch (0b10/0b01), u16 fallthrough
- Main loop: skip/prefix check → is_leaf [[unlikely]] break → split branchless → is_leaf [[unlikely]] break → fan branchless → repeat
- child_leaf path: always bitmap256_find
- Sentinel miss flows: safe through all paths

### TODO — Insert
- descent_entry_t stack[10], parent_type enum
- propagate helper
- insert_dispatch<INSERT, ASSIGN>: root compact, root fan, main loop
- compact_insert dispatch: nested bit tests, no u8
- bot_leaf_insert dispatch: bitmap256 (never overflows) or compact
- make_single_leaf(ik, sv, bits): dispatch_suffix → compact make_leaf
- make_single_bot_leaf(ik, sv, bits): bits≤8 → bitmap_leaf_ops::make_single, else make_single_leaf

### TODO — Erase
- Same descent as insert, simpler (no conversion)
- compact_erase dispatch: nested bit tests
- bot_leaf_erase dispatch: bitmap256 or compact
- remove_from_parent: cascade upward when child fully erased

### TODO — Build/Convert (runtime bits)
- build_node_from_arrays(suf, vals, count, bits): compact or skip-compress or split
- build_split_from_arrays: group by top 8, bitmap256 for ≤8 bits, compact or fan otherwise
- build_fan_from_range: group by 8 bits → children → fan_ops::make_fan
- convert_to_bitmask: compact leaf overflow → collect + build_node_from_arrays
- convert_bot_leaf_to_fan: bot-leaf overflow → collect + group + fan
- split_on_prefix: prefix mismatch → new split with old + new leaf

### TODO — Remove-All
- Context-based walk: root → compact or fan
- remove_fan_children: child is compact or split
- remove_split_children: leaf child is compact(is_leaf=true) or bitmap256(is_leaf=false), internal child is fan
- destroy_compact: nested bit test dispatch

### TODO — Stats
- Same context walk as remove_all
- debug_stats_t: compact_leaves, bitmap_leaves, split_nodes, fan_nodes, total_entries/nodes/bytes
- memory_usage() → debug_stats().total_bytes

---

## FILE 5: kntrie.hpp

### KEEP (mostly unchanged)
- Public API: insert, insert_or_assign, assign, erase, find_value, contains, count, operator[], at
- Iterator stubs
- value_type, key_type, etc. type aliases
- debug_stats, debug_root_info forwarding

### TODO (minor)
- Change namespace from gteitelbaum to gteitelbaum (already correct)
- Change impl type from kn3::kntrie3 to gteitelbaum::kntrie_impl (or inline)
- Forward find_value, insert, erase to impl
- memory_usage, debug_stats forward to impl

---

## FILE 6: benchmark.cpp

### KEEP
- All benchmark logic, timing, workload generation, markdown output

### TODO
- Update namespace/class references if kntrie.hpp public API changed
- Verify it compiles against new headers

---

## FILE 7: bench_jsx.cpp

### KEEP
- All benchmark logic, JSX output, tracking allocator, workload generation

### TODO
- Update namespace/class references if kntrie.hpp public API changed
- Verify it compiles against new headers
