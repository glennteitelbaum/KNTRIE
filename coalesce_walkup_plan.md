# Fix: O(N²) erase coalesce → O(N) walk-up

## Problem
`maybe_coalesce_` was called at every bitmask level on unwind, each time walking the
**entire subtree downward** to count entries via `count_entries_tagged_`. For N erases
this is O(N²).

## Solution
Pass `subtree_entries` up through the erase return. Each level only counts siblings
once (with early exit), never re-walks the child.

## Changes needed

### 1. kntrie_support.hpp — DONE
```cpp
struct erase_result_t {
    uint64_t tagged_ptr;
    bool erased;
    uint16_t subtree_entries;  // capped at COMPACT_MAX+1
};
```

### 2. kntrie_compact.hpp — DONE
All 4 erase returns get third field:
- not-found: `{tag_leaf(node), false, 0}`
- fully-erased: `{0, true, 0}`
- realloc shrink: `{tag_leaf(nn), true, static_cast<uint16_t>(nc)}`
- in-place: `{tag_leaf(node), true, static_cast<uint16_t>(nc)}`

### 3. kntrie_bitmask.hpp — DONE
Same pattern for `bitmap_erase`: 4 return sites get third field.

### 4. kntrie_impl.hpp — PARTIALLY DONE

#### New constant
```cpp
static constexpr uint16_t COALESCE_CAP = static_cast<uint16_t>(COMPACT_MAX + 1);
```

#### 3 new helpers (replace old count_entries_tagged_ and maybe_coalesce_) — DONE

- **`count_subtree_capped_(tagged)`** — recursive count with early exit > COMPACT_MAX
- **`count_children_capped_(node, sc, exclude_slot=-1)`** — sum children of a bitmask,
  skipping one slot, early exit > COMPACT_MAX
- **`do_coalesce_(node, hdr, bits, total_entries)`** — caller already verified
  total ≤ COMPACT_MAX; collects entries, strips skip bytes (`wk[i] <<= sc*8`,
  `leaf_bits = bits - sc*8`), builds leaf, prepends skip, deallocs subtree.
  Returns `{tag_leaf(leaf), true, total_entries}`.

#### erase_node_ standalone bitmask — DONE

Logic:
- Not-found / not-erased: `return {tag_bitmask(node), false, 0}`
- Child survived: `child_ent = cr.subtree_entries`. If > COMPACT_MAX → return CAP.
  Else count siblings excluding this slot. If total > COMPACT_MAX → return CAP.
  Else `do_coalesce_`.
- Child fully erased + remove_child → nc==0: `return {0, true, 0}`
- nc==1 collapse (leaf or bitmask wrap): count sole child, return its count
- nc>1: `count_children_capped_`, if ≤ COMPACT_MAX → `do_coalesce_`, else return CAP

#### erase_skip_chain_ — DONE (was the last piece)

Same pattern as standalone but:
- Uses `orig_bits` for coalesce (includes skip bytes)
- `count_children_capped_(node, sc, slot)` passes sc for skip chain offset
- Not-found returns: `, 0` third field
- Child survived: same coalesce check — child_ent + siblings, early exit CAP
- Child erased, nc==0: `return {0, true, 0}`
- nc==1 collapse: count sole child, return count
- nc>1: `count_children_capped_(node, sc)`, coalesce or return CAP
- All shrink-realloc logic stays the same

#### Deleted
- `maybe_coalesce_` (replaced by walk-up pattern)
- `count_entries_tagged_` (replaced by `count_subtree_capped_`)

## Key insight
Each node in the tree gets counted at most once across the entire unwind — at the
lowest bitmask level where total might be ≤ COMPACT_MAX. Higher levels see CAP
propagated up and bail immediately. So total work per erase is
O(subtree_size_at_coalesce_point) which is bounded by COMPACT_MAX (4096).

## Current state of kntrie_impl.hpp
- erase_node_ standalone: DONE (walk-up coalesce)
- erase_skip_chain_: DONE (walk-up coalesce, shrink-realloc, nc==1 collapse)
- Helpers (count_subtree_capped_, count_children_capped_, do_coalesce_): DONE
- maybe_coalesce_ / count_entries_tagged_: DELETED

## Testing needed
- Compile with -O2 -march=x86-64-v3
- test_sanity, test_big with ASAN
- test_huge optimized
- test_coalesce (>COMPACT_MAX sequential, erase back below)
- Benchmark to 3M — erase should now be linear
