# Dup Tombstone Implementation Plan v3

## Terminology

- `entries`: count of unique real entries (stored in header)
- `total`: `SlotTable<BITS, VST>::max_slots(alloc_u64)` — physical slot count, property of allocation size
- `dups`: `total - entries` — derived, never stored
- All dup slots hold the same key AND same value (including same T*) as their neighbor
- There is no "canonical" copy — any occurrence of a key is equally valid
- JumpSearch finds the LAST match, which is always valid

## Header Changes

**Remove dups from header.** Bits 4-15 of `flags_` no longer used for dups.
Remove `dups()` and `set_dups()` accessors.

```cpp
// flags_ layout (simplified):
//   bit 0:      is_bitmask
//   bits 1-2:   skip (0-3)
//   bits 3-15:  reserved
```

## Derived dups

Every operation computes:
```cpp
uint16_t total = SlotTable<BITS, VST>::max_slots(h->alloc_u64);
uint16_t dups  = total - h->entries;
```

This is always correct because:
- `make_leaf` fills `total` slots (entries + seeded dups)
- Insert consumes a dup: entries++, total unchanged → dups decreases by 1
- Erase creates a dup: entries--, total unchanged → dups increases by 1
- Resize: new total from new alloc, new dups = new total - entries

## Layout

`vals_<BITS>(node, count)` currently takes entry count to compute offset.
**Must pass `total`** (from SlotTable) since keys array has `total` elements.

Every call site: `vals_<BITS>(node, SlotTable<BITS, VST>::max_slots(h->alloc_u64))`

## size_u64 (unchanged)

`size_u64<BITS>(count)` computes exact u64s needed for `count` slots.
Used with real entry count for shrink/grow decisions:
- `size_u64(entries + 1)` for insert grow check
- `size_u64(entries - 1)` for erase shrink check

## Operation: Find

```
find<BITS>(node, h, ik):
  suffix = extract_suffix(ik)
  total = SlotTable<BITS, VST>::max_slots(h->alloc_u64)
  idx = JumpSearch::search(keys, total, suffix)
  if idx < 0: return nullptr
  return as_ptr(vals_<BITS>(node, total)[idx])
```

No behavior change. JumpSearch over more slots, finds last match.

## Operation: Insert (dups > 0, in-place)

No realloc. Consume one dup.

```
insert<BITS>(node, h, ik, value, alloc):
  suffix = extract_suffix(ik)
  total = max_slots(h->alloc_u64)
  dups = total - h->entries
  kd = keys_<BITS>(node)
  vd = vals_<BITS>(node, total)

  idx = JumpSearch::search_insert(kd, total, suffix)
  
  if idx >= 0:  // key exists — update path
    if ASSIGN:
      if !is_inline: destroy(vd[idx])
      write_slot(&vd[idx], value)
      // Walk LEFT: update dups with same key (share new T*)
      for i = idx - 1; i >= 0 && kd[i] == suffix; --i:
        vd[i] = vd[idx]  // don't destroy — old T* already destroyed above
    return {node, false, false}
  
  if !INSERT: return {node, false, false}
  
  ins = -(idx + 1)  // insertion point
  
  if dups > 0:
    // Find nearest dup: scan left and right from ins
    left_dup = -1
    for i = ins - 1; i >= 1; --i:
      if kd[i] == kd[i-1]: left_dup = i; break
    
    right_dup = -1  
    for i = ins; i < total - 1; ++i:
      if kd[i] == kd[i+1]: right_dup = i; break
    
    dup_pos = pick_closer(left_dup, right_dup, ins)
    
    if dup_pos < ins:
      memmove(kd + dup_pos, kd + dup_pos + 1, (ins - 1 - dup_pos) * sizeof(K))
      memmove(vd + dup_pos, vd + dup_pos + 1, (ins - 1 - dup_pos) * sizeof(VST))
      write_pos = ins - 1
    else:
      memmove(kd + ins + 1, kd + ins, (dup_pos - ins) * sizeof(K))
      memmove(vd + ins + 1, vd + ins, (dup_pos - ins) * sizeof(VST))
      write_pos = ins
    
    kd[write_pos] = suffix
    write_slot(&vd[write_pos], value)
    h->entries++
    return {node, true, false}
  
  // dups == 0: need realloc
  if h->entries >= COMPACT_MAX: return {node, false, true}  // needs_split
  ... realloc path (see Resize section) ...
```

## Operation: Insert (dups == 0, realloc)

```
  needed = size_u64<BITS>(entries + 1)
  au64 = round_up_u64(needed)
  nn = alloc_node(alloc, au64)
  new_total = max_slots(au64)
  new_dups = new_total - (entries + 1)
  
  // Build new node: merge existing entries + new entry, then seed dups
  // Source has 0 dups (old total == entries), so no dedup needed
  // Just insert new key in sorted position and seed
  
  // Setup header
  nh->entries = entries + 1
  nh->alloc_u64 = au64
  ... copy skip/prefix ...
  
  // Merge + seed into new arrays
  seed_with_insert<BITS>(nn, kd_old, vd_old, entries, suffix, value, new_total)
  
  dealloc_node(alloc, node, h->alloc_u64)
  return {nn, true, false}
```

## Operation: Erase (in-place)

```
erase<BITS>(node, h, ik, alloc):
  suffix = extract_suffix(ik)
  total = max_slots(h->alloc_u64)
  kd = keys_<BITS>(node)
  vd = vals_<BITS>(node, total)
  
  idx = JumpSearch::search(kd, total, suffix)
  if idx < 0: return {node, false}
  
  nc = h->entries - 1
  
  if nc == 0:
    // Last real entry. Destroy and dealloc.
    // (dups may exist but they're copies of this same entry)
    if !is_inline: destroy(vd[idx])
    dealloc_node(...)
    return {nullptr, true}
  
  needed = size_u64<BITS>(nc)
  if should_shrink_u64(h->alloc_u64, needed):
    // Realloc smaller — dedup, skip erased key, seed (see Resize section)
    ...
  
  // In-place: convert run of this key to neighbor dups
  // Find full run of this key
  first = idx
  while first > 0 && kd[first - 1] == suffix: first--
  // run is [first .. idx], length = idx - first + 1
  
  // Destroy value ONCE (all slots in run share same T*)
  if !is_inline: destroy(vd[first])
  
  // Overwrite entire run with neighbor
  if first > 0:
    neighbor_key = kd[first - 1]
    neighbor_val = vd[first - 1]
  else:
    neighbor_key = kd[idx + 1]
    neighbor_val = vd[idx + 1]
  
  for i = first; i <= idx; ++i:
    kd[i] = neighbor_key
    vd[i] = neighbor_val
  
  h->entries = nc
  // dups automatically increased by 1 (total unchanged, entries decreased)
  return {node, true}
```

## Operation: Erase (realloc shrink)

```
  needed = size_u64<BITS>(nc)
  au64 = round_up_u64(needed)
  nn = alloc_node(alloc, au64)
  new_total = max_slots(au64)
  new_dups = new_total - nc
  
  // Dedup source, skip erased key, seed into new node
  // Source: total slots with dups mixed in
  
  nh->entries = nc
  nh->alloc_u64 = au64
  ... copy skip/prefix ...
  
  seed_with_skip<BITS>(nn, kd, vd, total, suffix, nc, new_total)
  
  // Destroy erased value (only if not shared — but during dedup we find it)
  // Actually: handle destroy before seeding, during the dedup scan
  
  dealloc_node(alloc, node, h->alloc_u64)
  return {nn, true}
```

## Seed Algorithms

### seed_from_real<BITS>(node, real_keys, real_vals, n_entries, total)

Seeds `total - n_entries` dups evenly among `n_entries` real entries.
Used when we have a clean array of unique entries.

```
  if n_entries == total:
    memcpy keys and vals
    return
  
  kd = keys_<BITS>(node)
  vd = vals_<BITS>(node, total)
  n_dups = total - n_entries
  
  stride = n_entries / (n_dups + 1)
  remainder = n_entries % (n_dups + 1)
  
  write = 0; src = 0; placed = 0
  while placed < n_dups:
    chunk = stride + (placed < remainder ? 1 : 0)
    memcpy(kd + write, real_keys + src, chunk * sizeof(K))
    memcpy(vd + write, real_vals + src, chunk * sizeof(VST))
    write += chunk; src += chunk
    kd[write] = kd[write - 1]
    vd[write] = vd[write - 1]
    write++; placed++
  
  remaining = n_entries - src
  memcpy(kd + write, real_keys + src, remaining * sizeof(K))
  memcpy(vd + write, real_vals + src, remaining * sizeof(VST))
```

### seed_with_insert<BITS>(node, old_keys, old_vals, old_count, new_suffix, new_val, total)

Dedup source (if any dups), merge in new entry at sorted position, seed into total slots.
Source `old_count` is the physical slot count (may contain dups from old node).

```
  // Temp arrays for real entries + new entry
  n_entries = old_entries_count + 1  // caller provides real count via header
  ... collect real entries from source (skip dups), insert new key in order ...
  seed_from_real<BITS>(node, temp_keys, temp_vals, n_entries, total)
```

### seed_with_skip<BITS>(node, old_keys, old_vals, old_total, skip_suffix, n_entries, total)

Dedup source, skip one occurrence of skip_suffix, seed into total slots.

```
  // Collect real entries from source, skipping dups and one occurrence of skip_suffix
  // Destroy the skipped entry's value (T*) during collection
  ... collect into temp, skipping dups and one skip_suffix ...
  seed_from_real<BITS>(node, temp_keys, temp_vals, n_entries, total)
```

Note: For T*, during dedup collection, all slots with same key share same T*.
Only the LAST occurrence in a run gets its T* carried forward to temp. 
Skipped dups don't get destroyed (same pointer as the surviving entry).
The skip_suffix entry gets destroyed.

## Operation: Update (insert_or_assign, existing key)

JumpSearch finds LAST match at `idx`. Update that slot and walk left:

```
  if !is_inline: destroy(vd[idx])
  write_slot(&vd[idx], value)
  for i = idx - 1; i >= 0 && kd[i] == suffix; --i:
    vd[i] = vd[idx]  // share new T* or copy inline value
```

## Operation: for_each (skip dups)

```
for_each<BITS>(node, h, cb):
  total = max_slots(h->alloc_u64)
  kd = keys_<BITS>(node)
  vd = vals_<BITS>(node, total)
  for i = 0; i < total; ++i:
    if i > 0 && kd[i] == kd[i-1]: continue
    cb(kd[i], vd[i])
```

## Operation: destroy_and_dealloc (avoid double-free T*)

```
destroy_and_dealloc<BITS>(node, alloc):
  h = get_header(node)
  total = max_slots(h->alloc_u64)
  if !is_inline:
    kd = keys_<BITS>(node)
    vd = vals_<BITS>(node, total)
    for i = 0; i < total; ++i:
      if i > 0 && kd[i] == kd[i-1]: continue  // same T* as prev
      destroy(vd[i], alloc)
  dealloc_node(alloc, node, h->alloc_u64)
```

## make_leaf changes

After allocating with round_up_u64, seed dups from the padding space:

```
make_leaf<BITS>(sorted_keys, values, count, skip, prefix, alloc):
  needed = size_u64<BITS>(count)
  au64 = round_up_u64(needed)
  total = max_slots(au64)
  
  nn = alloc_node(alloc, au64)
  ... setup header with entries = count ...
  
  if total == count:
    // No room for dups, copy directly (existing behavior)
    memcpy keys, memcpy vals
  else:
    // Seed dups
    seed_from_real<BITS>(nn, sorted_keys, values, count, total)
  
  return nn
```

## convert_to_split (skip dups)

When converting compact leaf to split, iterate with dup-skipping:
```
  total = max_slots(h->alloc_u64)
  // ... collect unique entries only ...
  for i = 0; i < total; ++i:
    if i > 0 && kd[i] == kd[i-1]: continue
    // add to working arrays
```

## Summary of header changes

**Remove:**
- `NodeHeader::dups()` 
- `NodeHeader::set_dups()`
- Bits 4-15 usage in flags_

**Keep:**
- `SlotTable<BITS, VST>::max_slots(alloc_u64)` — the single source of truth

## Functions removed

- `insert_in_place_` — replaced by dup-consumption logic in insert
- `erase_in_place_` — replaced by O(1) dup-creation logic in erase

## Functions added

- `seed_from_real<BITS>()` — seed dups evenly into node from clean array
- `seed_with_insert<BITS>()` — dedup + merge new entry + seed (for insert realloc)
- `seed_with_skip<BITS>()` — dedup + skip erased entry + seed (for erase realloc)

## Files changed

### kntrie_support.hpp
- Remove `dups()` / `set_dups()` from NodeHeader
- Clean up flags_ comment

### kntrie_compact.hpp
- All `vals_()` calls: pass `max_slots(alloc_u64)` 
- `find`: search over total slots
- `insert`: dup-consumption (in-place) or realloc+seed
- `erase`: O(1) dup-creation (in-place) or realloc+dedup+seed
- Remove `insert_in_place_`, `erase_in_place_`
- `for_each`: skip dups
- `destroy_and_dealloc`: skip dups for T*
- `make_leaf`: seed dups
- Add seed helpers

### kntrie_impl.hpp
- `convert_to_split`: use dup-skipping for_each
- `convert_root_child_to_bot_internal_`: same

### kntrie_bitmask.hpp
- Bot-leaf BITS>16 delegates to CompactOps — inherits all fixes
- Bot-leaf-16 (bitmap): NO changes (no dups)