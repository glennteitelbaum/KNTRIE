# Value Type Normalization & Inline Storage — Implementation Plan

## Workflow Rules

1. **After each checklist item**: STOP. Do NOT compile or test. Show me:
   - All headers in a zip
   - Updated plan.md
   - Updated miniplan.md
   Then WAIT for me to say "continue".

2. **If something is wrong in plan.md**: Fix it in plan.md immediately so
   you never re-derive it. Then show me the updated plan + headers.

3. **miniplan.md**: Tracks what's done, what's in progress, what's next.
   Update it after every change. If context compresses, READ miniplan.md
   and plan.md FIRST — do not start over.

4. **Never revert code.** If stuck, STOP and ask.

5. **Items can be deferred** to later phases but not indefinitely. Deferred
   items stay in the checklist marked `[DEFER]` with a reason.

6. **If chat gets compressed**: Always check miniplan.md and plan.md from
   the transcript/uploads. Do NOT start over from scratch.

---

## Goal

Reduce template instantiations for trivial types and enable inline storage
for non-trivial types up to 64 bytes (one cache line), eliminating pointer
chasing during iteration.

## Three categories

| Cat | IS_INLINE | HAS_DESTRUCTOR | Storage | Example |
|-----|-----------|----------------|---------|---------|
| A | yes | no | normalized u8/u16/u32/u64/array<u64,N> | int, float, POD |
| B | yes | yes | real T | string, vector, map |
| C | no | yes | pointer to T | >64B or !nothrow_move |

## Slot movement: std::copy vs std::move

Two cases based on whether source and destination can overlap:

**No overlap (realloc, new node, split):** `std::copy` — compiler optimizes
to `memcpy` for trivial types.

**Overlap (in-place insert gap, erase compaction):** `std::move` /
`std::move_backward` — compiler optimizes to `memmove` for trivial types.

* A: compiler optimizes both to memcpy/memmove (trivially copyable)
* B: real copy/move-assigns; C++26 trivial relocation may optimize further
* C: memcpy/memmove of pointers

`if constexpr` splits:

* insert: A raw write, B placement-new, C alloc + placement-new
* erase: A nothing, B dtor, C dtor + dealloc
* destroy leaf: A nothing, B dtor all live, C dtor + dealloc all

---

## Completed Steps

### Step 1: Benchmark std::move vs memcpy for trivial types  ✓ DONE

Result: ratios 0.86–1.06 — pure noise. Compiler generates identical code.
Conclusion: use std::copy/std::move universally.

### Step 2: Value type classification  ✓ DONE

In kntrie_support.hpp: normalized_type_t, value_traits with IS_TRIVIAL,
IS_INLINE, HAS_DESTRUCTOR, slot_type selection.

### Step 3: VT refactor  ✓ DONE (definitions only)

value_traits has: store, as_ptr, destroy, init_slot, write_slot,
copy_uninit, move_uninit, shift_left, shift_right, destroy_relocated,
destroy_all.

### Step 5: Key+Value normalization in kntrie.hpp  ✓ DONE

kntrie<K,V> normalizes A types via NORMALIZE / to_impl / to_value /
to_value_ptr.

---

## Remaining Steps — Bug Fixes for B/C Usage Sites

The value_traits *definitions* are correct. The *usage sites* still treat
all slots as trivially copyable in several places. For A types (current
benchmarks) everything works. For B/C types, these are UB/double-free bugs.

### Critical concept: initialized vs uninitialized slot memory

* **init_slot(dst, val)** — placement-new into UNINITIALIZED memory
* **write_slot(dst, val)** — copy/move-assign into INITIALIZED (or moved-from) memory
* **destroy(slot)** — call destructor, leaving memory UNINITIALIZED
* **open_gap** — shift elements right, creating one uninitialized position
* **close_gap** — shift elements left, destroying the vacated tail position

For A/C types, init_slot == write_slot == memcpy, destroy is noop/dealloc,
and open_gap/close_gap are just memmove. The distinction only matters for B.

---

### Fix 1: Add open_gap / close_gap to value_traits  (kntrie_support.hpp)

These handle the boundary between initialized and uninitialized memory
during in-place insert/erase.

```cpp
// Open a gap at position `pos` in array of `count` live elements.
// vd[count] is UNINITIALIZED. After call, vd[pos] is UNINITIALIZED
// (ready for init_slot).
static void open_gap(slot_type* vd, size_t count, size_t pos) {
    if constexpr (IS_TRIVIAL || !IS_INLINE) {
        // A/C: memmove handles everything
        std::memmove(vd + pos + 1, vd + pos,
                     (count - pos) * sizeof(slot_type));
    } else {
        // B: move-construct last into uninit, shift rest, destroy gap pos
        if (count > pos) {
            ::new (&vd[count]) slot_type(std::move(vd[count - 1]));
            if (count - 1 > pos)
                std::move_backward(vd + pos, vd + count - 1, vd + count);
            vd[pos].~slot_type();
        }
    }
}

// Close gap: destroy vd[pos], shift [pos+1..count) left by 1.
// After call, vd[count-1] is DESTROYED.
// Caller must have already extracted/saved vd[pos]'s value if needed.
static void close_gap(slot_type* vd, size_t count, size_t pos) {
    if constexpr (IS_TRIVIAL || !IS_INLINE) {
        std::memmove(vd + pos, vd + pos + 1,
                     (count - 1 - pos) * sizeof(slot_type));
    } else {
        // vd[pos] is already destroyed by caller (via VT::destroy)
        // or we destroy it here — depends on usage.
        // Safest: assume vd[pos] is live, move [pos+1..count) left,
        // then destroy the moved-from tail.
        std::move(vd + pos + 1, vd + count, vd + pos);
        vd[count - 1].~slot_type();
    }
}
```

**Edge cases:**
* `count == 1, pos == 0`: open_gap does nothing (no elements to shift),
  close_gap destroys the single element
* `pos == count - 1`: open_gap move-constructs vd[count-1] to vd[count],
  destroys vd[count-1]. close_gap: nothing to shift, destroys vd[count-1].

---

### Fix 2: erase_create_dup_  (kntrie_compact.hpp)

**Bug:** Destroys `vd[first]`, then writes neighbor_val into `vd[first..idx]`
using raw assignment. `vd[first]` is DESTROYED — raw assignment is UB for B.
The rest `vd[first+1..idx]` are live dups — assignment is fine.

**Fix:**

```cpp
static void erase_create_dup_(
        K* kd, VST* vd, int total, int idx,
        K suffix, ALLOC& alloc) {
    int first = idx;
    while (first > 0 && kd[first - 1] == suffix) --first;

    if constexpr (VT::HAS_DESTRUCTOR)
        VT::destroy(vd[first], alloc);

    K   neighbor_key;
    VST neighbor_val;
    if (first > 0) {
        neighbor_key = kd[first - 1];
        neighbor_val = vd[first - 1];
    } else {
        neighbor_key = kd[idx + 1];
        neighbor_val = vd[idx + 1];
    }

    // vd[first] is DESTROYED → init_slot (placement new)
    kd[first] = neighbor_key;
    VT::init_slot(&vd[first], neighbor_val);

    // vd[first+1..idx] are LIVE dups → write_slot (assignment)
    for (int i = first + 1; i <= idx; ++i) {
        kd[i] = neighbor_key;
        VT::write_slot(&vd[i], neighbor_val);
    }
}
```

Note: For C types, neighbor_val is a `VALUE*` — copying the pointer is fine.
Multiple dup slots share the same pointer. On erase, only ONE destroy per
unique key (which does dtor+dealloc). The dups just get overwritten with the
neighbor's pointer.

---

### Fix 3: insert_dispatch_  (kntrie_impl.hpp)

**Bug:** Creates `VST sv = VT::store(value, alloc_)` on the stack. If insert
doesn't happen, calls `VT::destroy(sv, alloc_)`. For B types, sv is a real
VALUE with a stack destructor. `VT::destroy` calls `sv.~slot_type()`, then
stack unwind calls `~slot_type()` again → double-destruct.

**Fix:** Only call `VT::destroy` when it does something beyond what the stack
destructor would do. That means: A noop (already), B skip (stack dtor handles
it), C needs explicit destroy (dealloc the pointer — stack dtor of `VALUE*`
won't free the heap allocation).

```cpp
template<bool INSERT, bool ASSIGN>
std::pair<bool, bool> insert_dispatch_(const KEY& key, const VALUE& value) {
    IK ik = KO::to_internal(key);
    VST sv = VT::store(value, alloc_);

    if (root_ == SENTINEL_TAGGED) {
        if constexpr (!INSERT) {
            if constexpr (!VT::IS_INLINE)  // C only: dealloc
                VT::destroy(sv, alloc_);
            return {true, false};
        }
        root_ = tag_leaf(make_single_leaf_(ik, sv, KEY_BITS));
        ++size_;
        return {true, true};
    }

    auto r = insert_node_<INSERT, ASSIGN>(root_, ik, sv, KEY_BITS);
    if (r.tagged_ptr != root_) root_ = r.tagged_ptr;
    if (r.inserted) { ++size_; return {true, true}; }

    // Insert didn't happen — clean up sv
    // A: noop (trivial, no resources)
    // B: DO NOT call destroy — sv lives on stack, ~slot_type() will fire
    // C: must dealloc the heap VALUE*
    if constexpr (!VT::IS_INLINE)
        VT::destroy(sv, alloc_);

    return {true, false};
}
```

**Same fix needed in all other places that store+destroy on failure:**
Search for `VT::destroy(sv, alloc_)` or `VT::destroy(value, alloc_)` in
insert paths and apply the same `if constexpr (!VT::IS_INLINE)` guard.

Affected sites in kntrie_impl.hpp:
- `insert_dispatch_` (main entry)
- `insert_node_` (leaf insert returns needs_split=false, inserted=false)
- `insert_skip_chain_` (split mismatch, value already consumed by split)

**Rule:** If `sv` is a stack local of type `VST` and `VST` has a destructor
(B category), never call `VT::destroy` on it. Let the stack handle it.

---

### Fix 4: bitmap_insert in-place  (kntrie_bitmask.hpp)

**Bug:** Current code:
```cpp
VT::shift_right(vd + isl, vd + count, vd + count + 1);
VT::init_slot(&vd[isl], std::move(value));
```
`shift_right` calls `std::move_backward` which move-assigns into `vd[count]`.
But `vd[count]` is UNINITIALIZED — UB for B types.

**Fix:** Replace with `open_gap` + `init_slot`:
```cpp
VT::open_gap(vd, count, isl);
VT::init_slot(&vd[isl], std::move(value));
```

---

### Fix 5: bitmap_erase in-place  (kntrie_bitmask.hpp)

**Bug:** Current code:
```cpp
VT::destroy(bl_vals_mut_(node, hs)[slot], alloc);
// ...
VT::shift_left(vd + slot + 1, vd + count, vd + slot);
```
After `shift_left`, `vd[count-1]` is moved-from but still "alive" for B
types — its destructor would run on some future destroy_all/dealloc, causing
issues. Actually the bigger issue: `vd[slot]` was destroyed, then shift_left
move-assigns into it — UB for B (assigning into destroyed memory).

**Fix:** Use `close_gap` which handles the destroyed→shift→destroy-tail
sequence correctly. But wait — we already destroyed `vd[slot]` before the
shift. Two approaches:

**Approach A:** Destroy first, then shift into destroyed slot:
```cpp
VT::destroy(vd[slot], alloc);
// vd[slot] is destroyed. For B: move-assign from vd[slot+1] into destroyed
// memory is UB. Need placement-new instead.
```

**Approach B (better):** Don't pre-destroy. Let close_gap handle it:
```cpp
// Don't destroy first — close_gap will move [slot+1..count) left,
// overwriting vd[slot] via move-assign (vd[slot] is still live),
// then destroy the moved-from tail vd[count-1].
// But we DO need to destroy for C (dealloc the pointer before overwriting).
```

**Cleanest approach:**
```cpp
if constexpr (!VT::IS_INLINE) {
    // C: destroy (dealloc) before overwriting the pointer
    VT::destroy(vd[slot], alloc);
}
// For A: noop destroy, memmove is fine
// For B: vd[slot] is live, move-assign overwrites it, moved-from tail destroyed
// For C: vd[slot] pointer is freed, memmove overwrites it, tail is just a pointer
VT::close_gap(vd, count, slot);
```

Actually, close_gap for B already handles this: it calls
`std::move(vd+slot+1, vd+count, vd+slot)` which move-assigns into `vd[slot]`
(the old value gets move-assigned over — its destructor runs as part of
the move-assignment operator which handles cleanup). Then it destroys
`vd[count-1]`.

For C: we must dealloc the VALUE* before the pointer gets overwritten.

For B: The move-assignment into `vd[slot]` properly destroys the old value
through B's assignment operator. std::move handles this correctly because
the destination is a live object.

**Final fix for bitmap_erase in-place:**
```cpp
if constexpr (!VT::IS_INLINE)
    VT::destroy(vd[slot], alloc);    // C: dealloc heap VALUE*
else if constexpr (VT::HAS_DESTRUCTOR) {} // B: move-assign will handle it
// A: noop

VT::close_gap(vd, count, slot);
```

Wait — close_gap destroys the tail. For B that's correct (moved-from dtor).
For C, the tail is a moved-from pointer (already freed above? No — we freed
vd[slot], not vd[count-1]). The tail pointer after memmove is a copy of
vd[count-1]'s original pointer. That pointer is still live — it was just
shifted left. So close_gap's tail destroy would call VT::destroy on a live
pointer... which would dealloc it. But that's the SAME pointer that now lives
at vd[count-2] (or wherever it shifted to). Double-free!

**Revised approach for C:**
```cpp
// C path: destroy the erased slot, then memmove pointers left, done.
// Don't destroy the tail — it's a stale copy of a still-live pointer.
VT::destroy(vd[slot], alloc);
std::memmove(vd + slot, vd + slot + 1, (count - 1 - slot) * sizeof(slot_type));
// No tail destroy — the shifted pointers are all live.
```

**For B path:**
```cpp
// B: move-assign shifts live objects left. Move-assign into vd[slot]
// replaces the erased value (B's operator= handles cleanup).
// Tail vd[count-1] is moved-from → destroy it.
std::move(vd + slot + 1, vd + count, vd + slot);
vd[count - 1].~slot_type();
```

**Revised close_gap — make it category-aware:**
```cpp
// Close gap at `pos` in array of `count` LIVE elements.
// For C: caller MUST VT::destroy(vd[pos]) first (dealloc).
// For B: vd[pos] is still live — move-assign handles cleanup.
// After: count-1 live elements in [0..count-2], vd[count-1] destroyed.
static void close_gap(slot_type* vd, size_t count, size_t pos, ALLOC& alloc) {
    if constexpr (IS_TRIVIAL || !IS_INLINE) {
        // A/C: memmove bytes, no tail destroy needed
        // (A has no dtor, C pointers are just shifted — caller freed the erased one)
        std::memmove(vd + pos, vd + pos + 1,
                     (count - 1 - pos) * sizeof(slot_type));
    } else {
        // B: move-assign handles value cleanup on overwrite
        std::move(vd + pos + 1, vd + count, vd + pos);
        vd[count - 1].~slot_type();  // destroy moved-from tail
    }
}
```

**Usage in bitmap_erase:**
```cpp
if constexpr (VT::HAS_DESTRUCTOR && !VT::IS_INLINE)
    VT::destroy(vd[slot], alloc);    // C only: dealloc heap VALUE*
VT::close_gap(vd, count, slot, alloc);
```

---

### Fix 6: dedup_skip_into_  (kntrie_compact.hpp)

**Bug:** During erase realloc, copies values with `out_v[wi] = vd[i]`.
`out_v` is freshly allocated (UNINITIALIZED). Raw assignment into uninit
memory is UB for B types.

**Fix:** Use init_slot:
```cpp
// Was: out_v[wi] = vd[i];
VT::init_slot(&out_v[wi], vd[i]);  // copy-construct into uninit
```

Also: for the skipped (erased) entry, must destroy it:
```cpp
if (!skipped && kd[i] == skip_suffix) {
    skipped = true;
    if constexpr (VT::HAS_DESTRUCTOR)
        VT::destroy(vd[i], alloc);
    continue;
}
```
This is already correct in the current code. ✓

---

### Fix 7: seed_from_real_  (kntrie_compact.hpp)

**Bug:** When seeding dups, copies value slots for dup positions:
```cpp
vd[i] = neighbor_val;  // into freshly allocated node
```
Fresh node memory is UNINITIALIZED → UB for B.

**Fix:** All writes into fresh node memory must use init_slot:
```cpp
VT::init_slot(&vd[i], neighbor_val);
```

This applies throughout seed_from_real_ and seed_with_insert_ — every
write into `dv[wi]` in these functions targets uninitialized memory.

**Already correct in current code?** Check: seed_from_real_ uses
`VT::copy_uninit(real_vals, n_entries, vd)` for the real entries (correct).
For dups, it needs init_slot. seed_with_insert_ uses `VT::init_slot` for
real entries — check that dup seeding also uses init_slot.

Current seed_with_insert_ dup code:
```cpp
dk[wi] = dk[wi - 1];
VT::init_slot(&dv[wi], dv[wi - 1]);
```
This is correct — uses init_slot for dups. ✓

Current seed_from_real_ dup code — needs checking. The current code in
seed_from_real_ after the memcpy/copy_uninit for real entries, then loops
to fill dups. Check if it uses init_slot or raw assignment.

---

### Fix 8: compact insert ASSIGN path  (kntrie_compact.hpp)

Current code for key-exists + ASSIGN:
```cpp
VT::destroy(vd[idx], alloc);
VT::init_slot(&vd[idx], std::move(value));
// Update dup copies
for (int i = idx - 1; i >= 0 && kd[i] == suffix; --i)
    VT::write_slot(&vd[i], vd[idx]);
```

This is correct:
- destroy old value at idx ✓
- init_slot into destroyed slot ✓
- write_slot into live dup slots ✓

No fix needed. ✓

---

### Fix 9: bitmap_insert ASSIGN path  (kntrie_bitmask.hpp)

Current code:
```cpp
VT::destroy(vd[slot], alloc);
VT::init_slot(&vd[slot], std::move(value));
```

Correct: destroy then init_slot into destroyed memory. ✓

---

### Fix 10: insert_consume_dup_  (kntrie_compact.hpp)

Current code shifts live dup slots around using shift_left/shift_right,
then writes the new value with write_slot. ALL slots in the array are
initialized (they're all live keys + dups). shift_left/shift_right target
initialized memory. write_slot writes into initialized (moved-from) slot.

For B types: shift_left/shift_right use std::move/std::move_backward which
move-assign into initialized destinations — correct. The overwritten dup
slot's value gets destroyed via B's move-assignment operator. Then
write_slot move-assigns the new value — correct.

**One concern:** The dup_pos slot contains a value that's a copy of its
neighbor. When we shift over it, the move-assignment destroys that copy.
For C types, that means destroying the pointer — but wait, that pointer
is shared with the neighbor! Destroying it would dealloc the VALUE that
the neighbor still points to.

**This is a real bug for C types!** The dup's VALUE* is the same pointer
as the neighbor's. shift_left/shift_right would move-assign over it, which
for C just does memcpy (pointers) — so actually for C, shift_left does
`std::memmove` of pointers, which doesn't call any destructors. The old
pointer value is simply overwritten. So this is fine. ✓

For B types: the dup slot contains an independent copy (init_slot does
copy-construction). When shift moves over it, move-assignment properly
cleans up the dup's independent value. ✓

No fix needed. ✓

---

### Fix 11: collect_entries_tagged_ + dealloc_bitmask_subtree_  (kntrie_impl.hpp)

Used during coalesce. `collect_entries_tagged_` copies value slots:
```cpp
vals[wi] = v;
```
Into freshly allocated arrays (`std::make_unique<VST[]>(count)`).

For A: trivial copy, fine.
For B: `make_unique<VST[]>` default-constructs all elements, so they're
initialized. Assignment is fine. But we're copying from live leaf slots
that we're about to dealloc — after collect, `dealloc_bitmask_subtree_`
frees the nodes WITHOUT destroying values (values are "moved" into the
new leaf). For B, the leaf slots still contain live objects that will NOT
have their destructors called. Memory leak!

**Fix:** Use move semantics in collect, then destroy_relocated on old slots:

Actually, the current architecture doesn't destroy leaf values during
`dealloc_bitmask_subtree_` — it just frees the node memory. For A/C this
is fine (A has no dtor, C pointers are copied). For B, the leaf node
memory is freed without running destructors on the B-type objects stored
inline → those objects leak their internal resources.

**But wait:** If we copy the B values into the working array, the originals
in the leaf still exist. When `dealloc_bitmask_subtree_` frees the leaf
node memory without calling destructors, the B values in that memory are
leaked. We need to either:
(a) Move values out (so originals are moved-from, then call
    destroy_relocated on the leaf), or
(b) Copy values out, then explicitly destroy the originals before dealloc.

For B types during coalesce, the simplest fix:
```cpp
// In collect loop:
vals[wi] = std::move(v);  // move from leaf into working array

// After collect, before dealloc_bitmask_subtree_:
// Need to destroy moved-from B slots in old leaves
// But dealloc_bitmask_subtree_ only frees node memory...
```

This is complex. The cleanest approach: use std::move in the collect loop
so originals are moved-from, then add a destroy pass on each leaf's values
before dealloc_bitmask_subtree_ frees the node memory.

**Fix for collect_entries_tagged_:**
```cpp
// Move values out instead of copying:
vals[wi] = std::move(v);
```

**Fix for dealloc_bitmask_subtree_:** Add a mode or separate function that
destroys B-type values in each leaf before freeing. For A/C the existing
code (just dealloc) remains correct.

---

---

### Fix 12: Descendants simplification  (kntrie_bitmask.hpp, kntrie_impl.hpp, kntrie_ops.hpp, kntrie_support.hpp)

**What was lost:** The per-child `uint16_t` desc array was replaced with a
single uncapped `uint64_t` at the end of each bitmask node. This change
got applied to stale code and was reverted.

**Old layout:**
```
[header(1)][bitmap(4)][sentinel(1)][children(n)][desc(n × u16, padded to u64)]
```

**New layout:**
```
[header(1)][bitmap(4)][sentinel(1)][children(n)][descendants(1 u64)]
```

**Changes required:**

**kntrie_support.hpp:**
- Remove `desc_u64(n)` helper
- Remove `descendants()` / `set_descendants()` from node_header
  (total_slots_ no longer repurposed for bitmask descendants)
- Remove `COALESCE_CAP` constant (no saturation needed)

**kntrie_bitmask.hpp:**
- `bitmask_size_u64`: change from `hu + 5 + n_children + desc_u64(n_children)`
  to `hu + 5 + n_children + 1` (the +1 is the single descendants u64)
- Remove `child_desc_array()` / `desc_array_()` / `desc_array_mut_()` accessors
- Add `descendants_ptr(node)` → points to `uint64_t` at end of children array:
  `real_children_(node, hs) + entries`
- `add_child`: no desc array to shift/copy. Just copy children with insert,
  then write descendants (unchanged — caller increments separately)
- `remove_child`: no desc array to shift/copy. Just copy children with remove,
  then write descendants (caller decrements separately)
- Skip chain variants (`chain_add_child`, `chain_remove_child`): same simplification

**kntrie_ops.hpp:**
- `erase_node_` / `erase_final_bitmask_`: replace `dec_or_recompute_desc_`
  with simple `--(*descendants_ptr)`. Check `<= COMPACT_MAX` for coalesce.
- Remove `dec_or_recompute_desc_` and `sum_children_desc_` entirely
- Insert: `++(*descendants_ptr)` on successful insert into child

**kntrie_impl.hpp:**
- `insert_node_`: replace `inc_descendants_(hdr)` + desc array update with
  simple `++(*BO::descendants_ptr(node))`
- `erase_node_`: replace desc machinery with simple decrement + coalesce check
- Remove `inc_descendants_`, `dec_or_recompute_desc_`, `sum_children_desc_`
- Remove `count_children_capped_`

**make_bitmask / build paths:**
- When creating bitmask nodes, initialize the descendants u64 at the end
- `make_bitmask_tagged`: set descendants = sum of child entry counts

---

### Fix 14: Comprehensive branch prediction hints  (all files)

**What was lost:** `[[likely]]` / `[[unlikely]]` on every runtime branch.
Already partially done on find path and iteration path. Needs to be applied
consistently across insert, erase, coalesce, split, and all dispatch paths.

**General principles:**
- **Leaf is `[[unlikely]]`**: Recursive descent hits many bitmask nodes but
  only ONE terminal leaf. So `if (ptr & LEAF_BIT)` is always `[[unlikely]]`.
- **Skip is `[[unlikely]]`**: Most nodes have skip=0 on random data.
  `if (skip)`, `if (hdr->is_skip())` → `[[unlikely]]`.
- **Most code paths are `[[unlikely]]`**: The fast/common path is `[[likely]]`,
  everything else (error, fallback, realloc, split, collapse, mismatch) is
  `[[unlikely]]`.
- **Found/hit is `[[likely]]`**: Key match, slot found, child exists.
- **Miss/not-found is `[[unlikely]]`**: Key mismatch, bitmap miss, empty result.
- **needs_split is `[[unlikely]]`**: Compact leaf overflow is rare.
- **Realloc paths are `[[unlikely]]`**: In-place is the common case.
- **Coalesce is `[[unlikely]]`**: Subtree rarely drops below threshold.
- **Single-child collapse is `[[unlikely]]`**: Bitmask dropping to 1 child is rare.

**Specific sites to tag:**

**kntrie_ops.hpp (find/insert/erase dispatch):**
- `if (ptr & LEAF_BIT)` → `[[unlikely]]` (all find_node, insert_node, erase_node)
- `if (skip)` / `if (pos >= skip)` → `[[unlikely]]` / appropriate
- `if (result.needs_split)` → `[[unlikely]]`
- `if (result.inserted)` → `[[likely]]` in insert paths
- `if (!result.erased)` → `[[unlikely]]` (miss on erase)
- `if (nc == 1)` (collapse) → `[[unlikely]]`
- `if (exact <= COMPACT_MAX)` (coalesce) → `[[unlikely]]`

**kntrie_compact.hpp (compact leaf ops):**
- `if (*base != suffix)` in find → `[[unlikely]]`
- `if (*base == suffix)` in insert (key exists) → depends on workload,
  leave untagged or `[[unlikely]]` for insert-heavy
- `if (entries >= COMPACT_MAX)` → `[[unlikely]]`
- `if (dups > 0)` → `[[likely]]` (most inserts consume a dup)
- `if (new_ts < ts)` in erase (shrink) → `[[unlikely]]`
- `if (nc == 0)` in erase (last entry) → `[[unlikely]]`

**kntrie_bitmask.hpp (bitmask ops):**
- `if (bm.has_bit(suffix))` in bitmap_insert (exists) → `[[unlikely]]` for insert
- `if (!bm.has_bit(suffix))` in bitmap_erase (miss) → `[[unlikely]]`
- `if (new_sz <= h->alloc_u64())` in-place → `[[likely]]`
- `if (should_shrink_u64(...))` → `[[unlikely]]`
- `if (nc == 0)` after remove → `[[unlikely]]`
- `if (needed <= h->alloc_u64())` in add_child → `[[likely]]`

**kntrie_impl.hpp (top-level dispatch):**
- `if (root_ == SENTINEL_TAGGED)` → `[[unlikely]]` (empty trie)
- `if (r.tagged_ptr != root_)` → depends, leave untagged
- `if (r.inserted)` → `[[likely]]`

**Rule:** Every `if` in a hot path gets a hint. Only truly unpredictable
branches (data-dependent, varies per node) get left untagged. When in doubt,
tag it — a wrong hint is ~1 cycle penalty, a right hint saves pipeline
flushes.

---

### Fix 13: Remove dead ts==0 / entries==0 guards  (kntrie_compact.hpp)

**What was lost:** Nodes always have ≥1 entry — empty nodes get deallocated
on erase. The `ts == 0` early-return guards are dead code that adds branches
to the iteration hot path.

**Remove from kntrie_compact.hpp:**
- `for_each`: remove `if (ts == 0) return;`
- `iter_first`: remove `if (ts == 0) return {0, nullptr, false};`
- `iter_last`: remove `if (ts == 0) return {0, nullptr, false};`
- `iter_next`: remove `if (ts == 0) return {0, nullptr, false};`
- `iter_prev`: remove `if (ts == 0) return {0, nullptr, false};`
- `destroy_and_dealloc`: remove `if (ts > 0)` guard (always true)

**Note:** The sentinel node has entries=0 but it's only hit by find (which
returns nullptr naturally from adaptive_search on 0 slots). Iteration and
destroy never reach the sentinel — they're gated by `root_ == SENTINEL_TAGGED`
checks at the kntrie_impl level.

---

## Implementation Checklist

Priority order (do these in sequence, test after each):

1. [x] **Descendants simplification**: Already done in current code.
2. [x] **Add open_gap / close_gap** to value_traits (kntrie_support.hpp)
3. [x] **Fix erase_create_dup_**: init_slot for destroyed slot, write_slot
       for live dups (kntrie_compact.hpp)
4. [x] **Fix insert_dispatch_**: guard VT::destroy with
       `if constexpr (!VT::IS_INLINE)` for failed-insert cleanup
       (kntrie_impl.hpp) — already correct
5. [x] **Fix bitmap_insert in-place**: open_gap + init_slot
       (kntrie_bitmask.hpp)
6. [x] **Fix bitmap_erase in-place**: category-aware close_gap
       (kntrie_bitmask.hpp)
7. [x] **Fix dedup_skip_into_**: write_slot for output, HAS_DESTRUCTOR guard
       (kntrie_compact.hpp)
8. [x] **Audit seed_from_real_**: copy_uninit + init_slot for dups
       (kntrie_compact.hpp)
9. [x] **Fix coalesce dealloc**: templated dealloc_bitmask_subtree with
       proper NK narrowing for B-type destroy (kntrie_ops.hpp)
10. [ ] **Remove dead ts==0 guards** from iteration/for_each/destroy
       (kntrie_compact.hpp) (Fix 13)
11. [ ] **Comprehensive branch hints**: `[[likely]]`/`[[unlikely]]` on every
       runtime branch in all hot paths — insert, erase, split, coalesce,
       dispatch (all files) (Fix 14)

## Test plan

* Existing tests pass (A types — no behavior change expected)
* New test with B type (e.g. `std::string`): insert, find, erase, iterate
* New test with C type (e.g. large struct >64B): same
* ASAN validation for all three categories
* Benchmark: compare A (baseline) vs B iteration performance
