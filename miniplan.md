# Miniplan: Unified Node Design Refactor

## PROCESS RULES
1. Work on one file at a time, in order.
2. After each change within a file: update the changed header, mark TODO item DONE in miniplan.
3. After a file is complete: zip all changed headers + miniplan.md + plan.md. STOP. Wait for user to say continue.
4. Do not start the next file until user says continue.
5. If unsure about anything, ask — do not guess.
6. Never revert.
7. Reference plan.md for design details.
8. If chat gets compressed, read transcript before continuing.

---

## File 1: kntrie_support.hpp

New file from scratch. Foundation for everything else.

### TODO
- [x] Namespace `gteitelbaum`
- [x] Constants: `HEADER_U64=2`, `BITMAP256_U64=4`, `COMPACT_MAX=4096`, `BOT_LEAF_MAX=4096`
- [x] `SENTINEL_NODE[6]` — alignas(64), constinit, all zeros
- [x] `node_header` (16 bytes): flags_, entries_, alloc_u64_, skip_len_, prefix_[6], pad_[3]
  - is_leaf(), set_bitmask(), suffix_type(), set_suffix_type()
  - entries(), set_entries(), alloc_u64(), set_alloc_u64()
  - skip(), set_skip(), prefix_bytes(), set_prefix()
  - static_assert(sizeof == 16)
- [x] `get_header()` — const and non-const overloads
- [x] `key_ops<KEY>`: IK type, IK_BITS, KEY_BITS, IS_SIGNED, to_internal(), to_key()
- [x] `suffix_type_for(int bits)` — 0/1/2/3 for ≤8/≤16/≤32/>32
- [x] `round_up_u64()`, `step_up_u64()`, `should_shrink_u64()` — unchanged logic
- [x] `slot_table<K, VST>` — templated on K type, uses HEADER_U64=2
- [x] `value_traits<VALUE, ALLOC>` — unchanged (is_inline, store, as_ptr, destroy, write_slot)
- [x] `alloc_node()`, `dealloc_node()` — unchanged
- [x] Result types: `insert_result_t { uint64_t* node; bool inserted; bool needs_split; }`
- [x] Result types: `erase_result_t { uint64_t* node; bool erased; }`

### DONE
All items complete.

---

## File 2: kntrie_compact.hpp

New file from scratch. Compact leaf operations templated on K type.

### TODO
- [x] `jump_search<K>` — search() and search_insert(), unchanged logic
- [x] `compact_ops<K, VALUE, ALLOC>`:
  - [x] `size_u64(count)` — uses HEADER_U64=2
  - [x] `total_slots(alloc_u64)` — via slot_table
  - [x] `keys_(node)`, `vals_(node, total)` — layout helpers, offset from HEADER_U64=2
  - [x] `find<K>(node, hdr, suffix)` — jump_search on entries or total_slots
  - [x] `make_leaf(keys, vals, count, skip, prefix, alloc)` — set suffix_type in header
  - [x] `for_each(node, hdr, cb)` — below 256: contiguous; above 256: skip dups
  - [x] `destroy_and_dealloc(node, alloc)` — destroy values + free
  - [x] `insert(node, hdr, suffix, value, alloc)` — below 256: memmove into unused; above 256: consume dup; realloc when full
  - [x] `erase(node, hdr, suffix, alloc)` — below 256: memmove left; above 256: create dup; shrink realloc
  - [x] `seed_from_real_()` — for above-256 dup seeding (realloc path)

### DONE
All items complete.

---

## File 3: kntrie_bitmask.hpp

New file from scratch. Unified bitmask node + bitmap256 leaf.

### TODO
- [x] `bitmap256` struct (unchanged logic):
  - has_bit, set_bit, clear_bit, popcount
  - find_slot<MODE> (FAST_EXIT, BRANCHLESS, UNFILTERED)
  - find_next_set
  - from_indices
  - arr_fill_sorted, arr_insert, arr_remove, arr_copy_insert, arr_copy_remove
- [x] `bitmask_ops<VALUE, ALLOC>`:
  - **Bitmask node (internal):**
    - [x] Layout: header(2) + bitmap(4) + sentinel(1) + children(n) = 7+n
    - [x] `bitmask_size_u64(n_children)`
    - [x] `branchless_child(node, idx)` — returns pointer only, no is_leaf
    - [x] `lookup(node, idx)` — FAST_EXIT -> {child, slot, found}
    - [x] `set_child(node, slot, ptr)`
    - [x] `add_child(node, hdr, idx, ptr, alloc)` — in-place or realloc
    - [x] `remove_child(node, hdr, slot, idx, alloc)` — in-place or realloc; returns nullptr if empty
    - [x] `make_bitmask(indices, ptrs, count, skip, prefix, alloc)`
    - [x] `for_each_child(node, cb)` — cb(idx, slot, child_ptr)
    - [x] `dealloc_bitmask(node, alloc)`
  - **Bitmap256 leaf (suffix_type=0):**
    - [x] Layout: header(2) + bitmap(4) + values(n) = 6+ceil(n*sizeof(VST)/8)
    - [x] `bitmap_leaf_size_u64(count)`
    - [x] `bitmap_find(node, hdr, suffix_u8)` — FAST_EXIT -> value
    - [x] `bitmap_insert(node, suffix_u8, value, alloc)` — in-place or realloc
    - [x] `bitmap_erase(node, suffix_u8, alloc)` — in-place or realloc; nullptr if empty
    - [x] `make_bitmap_leaf(sorted_suffixes, values, count, alloc)` — sets suffix_type=0
    - [x] `make_single_bitmap(suffix_u8, value, alloc)`
    - [x] `for_each_bitmap(node, cb)` — cb(suffix_u8, value_slot)
    - [x] `bitmap_destroy_and_dealloc(node, alloc)`

### DONE
All items complete.

---

## File 4: kntrie_impl.hpp

New file from scratch. Implementation class with loop find, recursive insert/erase.

**Key conventions (from plan section 20):**
- `bits` tracks KEY_BITS remaining (not IK_BITS)
- Working arrays (build_node_from_arrays) use uint64_t, bit-63-aligned
- Suffix extraction: `K(ik >> (IK_BITS - W))` where W = suffix width
- leaf_for_each_u64 emits bit-63-aligned: `uint64_t(K) << (64 - W)`
- Type aliases: CO16, CO32, CO64 for compact_ops; BO for bitmask_ops

### TODO
- [ ] Class `gteitelbaum::kntrie_impl<KEY, VALUE, ALLOC>`
  - type aliases (KO, IK, VT, VST, BO, CO16/CO32/CO64, IK_BITS, KEY_BITS)
  - `root_[256]`, `size_`, `alloc_`
  - Constructor (fill root_ with SENTINEL_NODE), destructor (remove_all)
  - `empty()`, `size()`, `clear()`
- [ ] **find_value (loop):**
  - [ ] Root index: `ri = ik >> (IK_BITS-8)`, `ik <<= 8`
  - [ ] `goto noskip` — first node after root never has skip
  - [ ] Main loop: skip check (u8 chunks, shift ik), `noskip:` is_leaf break, extract `ti = ik >> (IK_BITS-8)`, `ik <<= 8`, branchless_child descend
  - [ ] Leaf dispatch: suffix_type → bitmap_find / CO16/CO32/CO64::find
  - [ ] `if constexpr` guards for KEY_BITS > 16 / > 32
  - [ ] `contains()` via find_value
- [ ] **insert (recursive):**
  - [ ] `insert()`, `insert_or_assign()`, `assign()` — template on INSERT/ASSIGN
  - [ ] `insert_node<INSERT,ASSIGN>(node, ik, value, bits)` — skip check, leaf/bitmask dispatch
  - [ ] `leaf_insert<INSERT,ASSIGN>(node, hdr, ik, value, bits)` — suffix_type dispatch
  - [ ] `make_single_leaf(ik, value, bits)` — suffix_type_for(bits) dispatch
  - [ ] Root: `bits = KEY_BITS - 8`
- [ ] **erase (recursive):**
  - [ ] `erase(key)` — `ri`, `ik <<= 8`, `bits = KEY_BITS - 8`
  - [ ] `erase_node(node, ik, bits)` — skip check, leaf/bitmask dispatch
  - [ ] `leaf_erase(node, hdr, ik)` — suffix_type dispatch
- [ ] **Conversion:**
  - [ ] `convert_to_bitmask(node, hdr, ik, value, bits)` — leaf_for_each_u64 → build_node_from_arrays
  - [ ] `leaf_for_each_u64(node, hdr, cb)` — emit bit-63-aligned uint64_t
  - [ ] `build_node_from_arrays(suf, vals, count, bits)` — bit-63-aligned, skip compress, leaf/bitmask
  - [ ] `build_bitmask_from_arrays(suf, vals, count, bits)` — group by `suf>>56`, recurse
  - [ ] `split_on_prefix(node, hdr, ik, value, actual, skip, common, bits)`
- [ ] **Cleanup:**
  - [ ] `remove_all()` — walk root_[256]
  - [ ] `remove_node(node)` — leaf→destroy_leaf, bitmask→for_each_child+recurse+dealloc_bitmask
  - [ ] `destroy_leaf(node, hdr)` — switch suffix_type: 0→bitmap, 1→CO16, 2→CO32, 3→CO64
- [ ] **Stats:**
  - [ ] `debug_stats_t` struct
  - [ ] `debug_stats()` / `collect_stats(node, s)` — self-describing walk
  - [ ] `memory_usage()`, `debug_root_info()`

### DONE
(none yet)

---

## File 5: kntrie.hpp

Update existing wrapper to use new kntrie_impl.

### TODO
- [ ] Update `#include` to use `kntrie_impl.hpp`
- [ ] Update internal `impl_` type to `gteitelbaum::kntrie_impl<KEY, VALUE, ALLOC>`
- [ ] Verify all public API methods delegate correctly
- [ ] `debug_stats()` / `memory_usage()` / `debug_root_info()` updated

### DONE
(none yet)

---

## File 6: Verification

### TODO
- [ ] Compile all: `g++ -std=c++23 -O2 -march=x86-64-v3`
- [ ] Run benchmark at 1K/10K/100K — no crashes
- [ ] Compare find/insert/erase numbers vs old baseline
- [ ] Check bytes-per-entry — verify HEADER_U64=2 doesn't regress badly
- [ ] Sequential pattern — verify skip compression working
- [ ] Run bench_jsx — verify JSX output clean

### DONE
(none yet)
