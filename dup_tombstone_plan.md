# Dup Tombstone Design

## Problem

Compact leaf insert and erase are O(n) due to memmove of keys and values arrays.
At ~4000 entries with u64 key+value, each insert/erase shifts ~16-32KB of data.
This dominates cost at 250K-1M entries in the random pattern.

## Core Idea

Maintain duplicate entries (copies of adjacent keys+values) distributed throughout
the sorted arrays. These dups act as free slots that can be consumed by insert
or created by erase, reducing shifts from O(n) to O(gap_distance).

The node is always at full allocated capacity: `entries + dups = total_slots_for_alloc`.

## Header Changes

```
NodeHeader::flags_ (uint16_t):
  bit 0:     leaf/bitmask
  bits 1-2:  skip (max 3)
  bit 3:     (reserved)
  bits 4-15: dups (12 bits, max 4095)
```

Accessors:
```cpp
uint16_t dups() const noexcept { return flags_ >> 4; }
void set_dups(uint16_t d) noexcept {
    flags_ = (flags_ & 0x000F) | (d << 4);
}
```

`entries` remains the count of real (unique) entries. Total occupied slots = `entries + dups`.

## Node Layout

Unchanged structure: `[header][sorted_keys][values]`

But the arrays have `entries + dups` slots. Dup keys are copies of their left
neighbor. Dup values are copies of the same neighbor's value.

Example — 8 real entries, 4 dups (every 2nd entry):
```
keys:   [1, 1, 2, 3, 3, 4, 5, 5, 6, 7, 7, 8]
values: [a, a, b, c, c, d, e, e, f, g, g, h]
```

JumpSearch finds the **last** match for any key, which is always a valid entry.
Dups are invisible to search — finding key=1 returns position 1 (the dup),
which holds the correct value `a`.

## Total Slots Calculation

Given `alloc_u64` and `skip`, compute max total slots:

```cpp
template<int BITS>
static constexpr size_t max_slots(size_t alloc_u64, uint8_t skip) noexcept {
    using K = typename suffix_traits<BITS>::type;
    size_t avail = (alloc_u64 - header_u64(skip)) * 8;
    // Find max total where ceil8(total*sizeof(K)) + ceil8(total*sizeof(VST)) <= avail
    size_t total = avail / (sizeof(K) + sizeof(VST));
    while (total > 0) {
        size_t kb = (total * sizeof(K) + 7) & ~size_t{7};
        size_t vb = (total * sizeof(VST) + 7) & ~size_t{7};
        if (kb + vb <= avail) break;
        --total;
    }
    return total;
}
```

On any allocation: `dups = max_slots(alloc_u64, skip) - entries`.

## Seeding Strategy

When building or resizing a node with `entries` real entries into an allocation
with room for `total` slots, distribute `dups = total - entries` dup slots
evenly among the real entries.

Algorithm:
```
stride = entries / (dups + 1)   // entries between dups
remainder = entries % (dups + 1)

write_pos = 0
src_pos = 0
dups_placed = 0
for each dup to place:
    chunk = stride + (dups_placed < remainder ? 1 : 0)
    copy keys[src_pos .. src_pos+chunk) to output[write_pos ..]
    copy vals[src_pos .. src_pos+chunk) to output[write_pos ..]
    write_pos += chunk
    src_pos += chunk
    // place dup: copy of previous entry
    output_key[write_pos] = output_key[write_pos - 1]
    output_val[write_pos] = output_val[write_pos - 1]
    write_pos++
    dups_placed++
// copy remaining real entries
copy keys[src_pos .. entries) to output[write_pos ..]
copy vals[src_pos .. entries) to output[write_pos ..]
```

Example: 8 entries, alloc fits 12 total, 4 dups, stride=2:
```
src:  [1, 2, 3, 4, 5, 6, 7, 8]
out:  [1, 2, 2, 3, 4, 4, 5, 6, 6, 7, 8, 8]
              ^        ^        ^        ^  dups
```

## Erase — O(1)

```
1. Find key at position idx (via JumpSearch) in 0..entries+dups
2. Overwrite key[idx] with key[idx-1] or key[idx+1]
   (copy the value too)
3. Destroy old value if T* (only if non-inline)
4. entries--, dups++
5. Check should_shrink(alloc, size_u64_for(entries)):
   - If yes: realloc smaller, dedup, reseed
```

No memmove. One key write, one value write (or two if T*: destroy + copy).

Edge case: erasing first entry (idx=0) — copy from right neighbor.
Edge case: erasing where idx is adjacent to a dup — still works, creates
another dup of the same key. Harmless, dedup handles later.

## Insert (dups > 0) — O(gap_distance)

```
1. search_insert to find insertion point ins in 0..entries+dups
2. Check if key exists (update path):
   - If found: overwrite value at found position. Done.
3. Find nearest dup:
   - Scan left from ins: find i where key[i] == key[i-1]
   - Scan right from ins: find i where key[i] == key[i+1]
   - Pick closer one at position dup_pos
4. Shift the range between dup_pos and ins:
   - If dup_pos < ins: shift [dup_pos+1 .. ins) left by 1, insert at ins-1
   - If dup_pos > ins: shift [ins .. dup_pos) right by 1, insert at ins
5. Write new key+value at freed position
6. entries++, dups--
```

Average shift distance: `total_slots / (2 * dups)`.
With 4096 slots and 256 dups: ~8 entries shifted.
With 4096 slots and 64 dups: ~32 entries shifted.
Still much better than ~2000.

## Insert (dups == 0) — Resize Up

```
1. Compute needed = size_u64(entries + 1, skip)
2. alloc_u64 = round_up_u64(needed)  // jumps to next size class
3. Allocate new node
4. Copy entries (no dedup needed since dups==0)
5. Seed dups for new allocation
6. Insert the new entry using normal dups>0 path
```

## Dedup (during resize only)

Forward scan with read/write pointers:

```
read = 0, write = 0
while read < entries + dups:
    if read > 0 && key[read] == key[read-1]:
        read++          // skip dup, do NOT destroy value (it's a copy)
        continue
    if write != read:
        key[write] = key[read]
        val[write] = val[read]
    write++
    read++
// write == entries (real count)
```

O(entries + dups) but only happens during realloc which is already O(n).

## Impact on Find

**None.** JumpSearch over `entries + dups` slots. Finds last match.
Dups have identical key+value to their neighbor. Any match returns correct data.

## Impact on Update (insert existing key)

JumpSearch finds the last occurrence. Update overwrites that slot.
If the key has a dup, the dup now holds a stale value.

Fix: after updating position `idx`, check `if (idx > 0 && key[idx-1] == suffix)`
and update that value too. One extra comparison, adjacent memory. Repeat leftward
for multiple dups of same key (rare — at most 2 in practice).

## Impact on Convert-to-Split

`for_each` iterates all `entries + dups` slots. Must skip dups during
conversion. Simple check: `if (i > 0 && key[i] == key[i-1]) continue;`

## Impact on JumpSearch

`search()` and `search_insert()` receive `entries + dups` as count.
No code change needed — dups are valid sorted entries.

`search_insert()` for finding insertion point: returns position in the
full array including dups. This is correct — we insert among all slots.

## Resize Triggers

| Operation | Trigger | Action |
|-----------|---------|--------|
| Insert, dups > 0 | — | Consume dup, small shift |
| Insert, dups == 0 | Always | Realloc up + seed dups |
| Erase | should_shrink(alloc, size_for(entries)) | Realloc down + dedup + seed |
| Erase | !should_shrink | O(1) dup creation |

## Expected Performance

At 4000 entries with 100 dups (reasonable density):

| Operation | Before | After |
|-----------|--------|-------|
| Erase | O(2000) memmove | O(1) |
| Insert | O(2000) memmove | O(20) shift |
| Find | unchanged | unchanged |
| Memory | unchanged | unchanged (same alloc, slots redistributed) |

## Implementation Order

1. Header: add dups accessors to flags_
2. max_slots calculation
3. Seeding function
4. Dedup function
5. Erase: O(1) path
6. Insert: nearest-dup path
7. Update: propagate to adjacent dups
8. for_each: skip dups
9. Convert-to-split: skip dups
10. Resize paths: integrate dedup + reseed
