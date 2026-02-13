# Miniplan: Implementation by File

## PROCESS RULES
1. Work on one file at a time, in order (1→2→3→4→5).
2. For each TODO item: write/change the code, save the file.
3. After saving: update miniplan.md — move the completed TODO item to KEEP.
4. When a file has zero TODO items remaining: STOP. Create a zip of ALL header files, benchmark.cpp, bench_jsx.cpp, miniplan.md, and plan.md. Wait for user to say continue.
5. Do not start the next file until user says continue.
6. If unsure about anything, ask — do not guess.

---

## FILE 1: kntrie_support.hpp

### KEEP (copy from old, no changes)
- Constants: BITMAP256_U64, COMPACT_MAX, BOT_LEAF_MAX, HEADER_U64
- round_up_u64, step_up_u64, should_shrink_u64
- prefix_t (std::array<uint16_t, 2>)
- SENTINEL_NODE (alignas(64) constinit)
- value_traits<VALUE, ALLOC> (rename IS_INLINE from is_inline, otherwise identical)
- alloc_node, dealloc_node helpers
- erase_result_t (was EraseResult)

### TODO (new or rewritten)
- node_header: add suffix_type() / set_suffix_type() using bits 14-13.
  Rest unchanged (is_leaf, entries, alloc_u64, skip, prefix).
- key_ops<KEY>: new. internal_key_t = u32 for KEY≤32bit, u64 otherwise.
  to_internal(): left-align in internal_key_t. to_key(): reverse.
- suffix_type_for(int bits): returns 1/2/3 (no 0). bits≤16→1, ≤32→2, else 3.
- dispatch_suffix(uint8_t stype, Fn): nested bit test (0b10 then 0b01), u16 fallthrough.
- slot_table<K, VST>: replaces SlotTable<BITS, VST>. Same logic, templated on K.

### REMOVE
- suffix_traits<BITS> — gone entirely
- KeyOps<KEY> — replaced by key_ops<KEY>
- InsertResult — gone (compact_ops has its own result type)

---

## FILE 2: kntrie_compact.hpp

### KEEP (copy from old, rename only)
- jump_search<K> (was JumpSearch<K>) — identical logic
- All compact leaf layout logic (header + sorted keys + aligned values + dups)
- Dup seeding: seed_from_real, seed_with_insert, seed_with_skip
- insert_consume_dup, erase_create_dup
- for_each, destroy_and_dealloc

### TODO (re-template)
- compact_ops<KEY, VALUE, ALLOC> methods: change template param from <int BITS> to <typename K>
  - size_u64<K>, total_slots<K>, keys<K>, vals<K>
  - make_leaf<K>: new param `uint8_t stype` to set in header via set_suffix_type()
  - find<K>: takes K suffix directly (caller extracts from ik)
  - insert<K>: takes K suffix directly
  - erase<K>: takes K suffix directly
  - compact_insert_result_t (was nested in old code): {node, inserted, needs_split}
- Layout helpers keys<K>/vals<K>: make public (kntrie_impl needs them for convert)

### REMOVE
- All extract_suffix / extract_top8 calls inside compact_ops — caller provides K suffix

---

## FILE 3: kntrie_bitmask.hpp

### KEEP (copy from old)
- bitmap256 struct entirely (was Bitmap256): rename, same logic
  - has_bit, set_bit, clear_bit, popcount, find_slot<MODE>, find_next_set
  - arr_fill_sorted, arr_insert, arr_remove, arr_copy_insert, arr_copy_remove
  - from_indices

### TODO (split BitmaskOps into 3 structs)

#### split_ops<KEY, VALUE, ALLOC>
- Layout: header(1) + main_bitmap(4) + is_internal_bitmap(4) + sentinel(1) + children[] = 10 + n
- branchless_top_child → returns {child, is_leaf}
- lookup_child, set_child, add_child_as_leaf, add_child_as_internal
- remove_child, mark_internal
- make_split, for_each_child, dealloc
- child_count

#### fan_ops<KEY, VALUE, ALLOC>
- Layout: header(1) + main_bitmap(4) + sentinel(1) + children[] = 6 + n
- branchless_child → returns child only (no is_leaf)
- lookup_child, set_child, add_child, remove_child
- make_fan, for_each_child, dealloc
- child_count

#### bitmap_leaf_ops<KEY, VALUE, ALLOC>
- Layout: header(1) + bitmap256(4) + VST[] = 5 + ceil(n*sizeof(VST)/8)
- Header: is_bitmask=1 (set_bitmask in make functions)
- find, insert (no overflow field in result), erase
- make_single, make_from_sorted, for_each
- destroy_and_dealloc, dealloc_only
- insert_result_t: {node, inserted} — no overflow

### REMOVE
- BitmaskOps monolith
- All <int BITS> template params on bitmask operations
- bot_leaf concept (split into bitmap_leaf_ops for 8-bit, compact_ops for wider)

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
