# Prefix Unification Plan

## The Problem

`node[2]` (leaf prefix) is stored **left-aligned** in a separate coordinate space from the root-level `ik`. This causes:

- `make_root_key` to shift prefix right by DEPTH, losing depth bytes when skip=0 (the iteration bug)
- `ik_to_pfx_space` to shift ik left, creating a second coordinate space
- complex combining logic in `prepend_skip` / `remove_skip`
- fragile byte extraction via `pfx_byte(pfx_u64, pos)` at runtime positions

## The Fix

**`node[2]` = any full root-level ik that belongs to this leaf.**

Suffix bits are don't-care (stale). Prefix bits (depth + skip bytes) are always correct because all keys in the leaf share them. Everything stays in root-level ik space. One space, one mask, one OR.

### Key formulas (all constexpr)

```cpp
// Zeros out REMAINING suffix bits in root-level ik
prefix_mask<REMAINING> = ~0ULL << (64 - KEY_BITS + REMAINING)
    where REMAINING = BITS - 8*SKIP

// Reconstruct full root-level key
make_root_key = (node[2] & prefix_mask) | suffix_to_u64(suf)

// Skip byte comparison: same extract_byte on both sides
extract_byte<BITS>(ik) vs extract_byte<BITS>(node[2])
```

## Changes by file

---

### kntrie_ops.hpp — leaf_ops_t

**DELETE** (`ik_to_pfx_space`):
```
- static uint64_t ik_to_pfx_space(uint64_t ik) noexcept {
-     return ik << (8 * DEPTH);
- }
```

**REPLACE** (`make_root_key`):
```
template<int REMAINING>
static uint64_t make_root_key(const uint64_t* node, auto suf) noexcept {
    constexpr uint64_t MASK = ~0ULL << (64 - KEY_BITS + REMAINING);
    return (leaf_prefix(node) & MASK) | suffix_to_u64<REMAINING>(suf);
}
```
Just mask-and-OR. No shifting. `MASK` is constexpr.

---

### kntrie_ops.hpp — leaf_find_at / leaf_next_at / leaf_prev_at

**Prefix comparison** — currently:
```cpp
uint64_t sik = ik_to_pfx_space(ik);
uint64_t pfx = leaf_prefix(node);
uint64_t diff = (sik ^ pfx) & MASK;
```
Replace with direct comparison in root-level coords:
```cpp
constexpr uint64_t SKIP_MASK = prefix_mask<0>() ^ prefix_mask<REMAINING>();
// SKIP_MASK isolates exactly the skip bytes in root-level coords
uint64_t diff = (ik ^ leaf_prefix(node)) & SKIP_MASK;
```
Where `prefix_mask<0>()` = `~0ULL << (64 - KEY_BITS + BITS)` (everything above BITS) and `prefix_mask<REMAINING>()` = `~0ULL << (64 - KEY_BITS + REMAINING)` (everything above REMAINING). XOR gives the skip byte region.

The divergence byte extraction also simplifies — the countl_zero result directly gives the byte position in root-level coords. Extract via `(ik >> (56 - shift))` and `(node[2] >> (56 - shift))` — same as today but on root-level values.

---

### kntrie_ops.hpp — init_leaf_fn

```
template<int BITS>
static void init_leaf_fn(uint64_t* node, uint64_t ik) noexcept {
    BO::set_leaf_fn(node, &leaf_ops_t<BITS>::LEAF_FNS[0]);
    set_leaf_prefix(node, ik);  // full root-level key, suffix bits are don't-care
}
```
Add `ik` parameter. Caller always has it. Suffix bits are stale but harmless — `make_root_key` masks them out.

---

### kntrie_ops.hpp — make_single_leaf

Already has `ik`. Change `init_leaf_fn<BITS>(node)` → `init_leaf_fn<BITS>(node, ik)`.

---

### kntrie_ops.hpp — prepend_skip

**Current**: Takes `uint64_t new_pfx` (left-aligned), combines with old prefix via shifting.

**New**: Takes `uint64_t ik` (any root-level key). Sets `node[2] = ik`. (Stale suffix bits are harmless.)

```
template<int BITS>
static void prepend_skip_fn(uint64_t* node, uint8_t new_len, uint64_t ik) noexcept {
    uint8_t old_skip = get_header(node)->skip();
    uint8_t new_skip = old_skip + new_len;
    set_leaf_prefix(node, ik);   // full key — suffix bits are don't-care
    get_header(node)->set_skip(new_skip);
    BO::set_leaf_fn(node, &leaf_ops_t<BITS>::LEAF_FNS[new_skip]);
}
```
No combining. No shifting. Just store the key.

---

### kntrie_ops.hpp — remove_skip

**Current**: Zeros prefix, sets skip=0.

**New**: `node[2]` already has the correct key bits. Just update skip count and fn pointer. `node[2]` stays as-is — the old skip bytes are now tree-consumed bytes, but they're still correct key bits. The new suffix bits (formerly skip bytes) are also correct.

Actually: `node[2]` doesn't change at all. The stale suffix bits don't matter.

```
template<int BITS>
static uint64_t* remove_skip(uint64_t* node, BLD&) {
    // node[2] unchanged — already has correct key bytes
    get_header(node)->set_skip(0);
    BO::set_leaf_fn(node, &leaf_ops_t<BITS>::LEAF_FNS[0]);
    return node;
}
```

---

### kntrie_ops.hpp — split_on_prefix

**Current**: Extracts skip bytes via `pfx_byte(pfx_u64, common)`, saves bytes to array, computes `rem_pfx` with shifting, builds `new_pfx_u64` for new leaf.

**New**: 
- `old_idx = extract_byte<BITS>(leaf_prefix(node))` — BITS is already at the divergence level
- Byte extraction for `wrap_in_chain`: extract from `node[2]` at root-level positions. Use `pfx_byte` or a new helper that reads bytes from `node[2]` at absolute position DEPTH+i.
  - Actually `pfx_byte(node[2], DEPTH+i)` if we think of pfx_byte as "extract byte i from left of u64". But pfx_byte uses `>> (56 - 8*i)`. For position DEPTH+i in the key, the byte is at bit `64 - KEY_BITS + BITS - 8*i - 8`. Simpler: just pass `node[2]` bytes to wrap_in_chain. They're real key bytes at real positions. Use `static_cast<uint8_t>(node[2] >> byte_shift<BITS-8*i>())` for each byte.
  - OR: use the ik-based extraction since ik matches node[2] in the skip region.
- New leaf prefix: `prepend_skip<BITS-8>(new_leaf, old_rem, ik, bld)` — pass ik directly.
- Old node prefix: `set_leaf_prefix(node, node[2])` — it's already correct (same key bits). Just update skip count and fn. Or call `prepend_skip` variant.

Wait — split_on_prefix REDUCES the old node's skip. The old node keeps `old_rem = skip - 1 - common` skip bytes. Its `node[2]` already has the correct key. Just update skip count + fn pointer. No prefix recalculation.

For the `saved_prefix` bytes passed to `wrap_in_chain`: these are the `common` shared skip bytes before the divergence. They need to be raw byte values. Extract them from `node[2]` (or equivalently from `ik`) at their root-level positions.

The bytes for bitmask skip chains sit at absolute key positions `DEPTH, DEPTH+1, ..., DEPTH+common-1`. The `pfx_byte(v, i)` function extracts byte `i` from the LEFT of a u64 as `v >> (56-8*i)`. For our leaf at depth DEPTH, the skip bytes start at pfx_byte position DEPTH. But `node[2]` IS in root-level coords, so `pfx_byte(node[2], DEPTH+i)` gives the correct byte. However, `pfx_byte` currently works on left-aligned prefix values where byte 0 is at bit 56. For root-level ik, the first key byte is at position `64-KEY_BITS` from the left. For u64 keys, this IS position 0 (bit 56). For u16 keys, position 0 in the key is at bit 48 in the u64. So `pfx_byte(node[2], DEPTH+i)` gives: `node[2] >> (56 - 8*(DEPTH+i))`. For u16 with DEPTH=0, i=0: `node[2] >> 56`. But the first key byte of u16 is at bit 48, not 56. The top byte of the u64 is 0 (padding). So this gives 0, not the key byte!

So `pfx_byte` doesn't work here for non-u64 keys. We need to extract bytes from root-level ik at key-relative positions. `extract_byte<BITS>` already does this correctly via `byte_shift<BITS>()`. For the skip bytes at positions 0..common-1 within the skip, we need bytes at BITS levels BITS, BITS-8, BITS-16, etc. Since split_on_prefix recurses (or we can precompute), we can extract these via the compile-time BITS parameter.

But for `wrap_in_chain`, we need raw byte values in an array. We're at runtime with `common` bytes to extract. We can compute the byte positions from compile-time DEPTH and KEY_BITS:

```cpp
for (uint8_t i = 0; i < common; ++i)
    saved_prefix[i] = static_cast<uint8_t>(node[2] >> (byte_shift<BITS>() - 8*i));
```

Or define a helper: `skip_byte_from_key<BITS>(uint64_t key, uint8_t pos)` that extracts the byte at skip position `pos` from a root-level key at tree level BITS.

---

### kntrie_ops.hpp — insert_leaf_skip / erase_leaf_skip

**Current**: Passes `pfx_u64` (left-aligned) through recursion, compares via `pfx_byte(pfx_u64, pos)`.

**New**: Compare directly: `extract_byte<BITS>(ik)` vs `extract_byte<BITS>(leaf_prefix(node))`. BITS already decrements at each recursive step, so this naturally walks through the skip bytes.

No need to pass `pfx_u64` separately — just read `leaf_prefix(node)` at each level.

Signature simplifies: remove `pfx_u64` parameter.

---

### kntrie_ops.hpp — collect_leaf_skip

**Current**: Walks prefix bytes via `pfx_byte(pfx_u64, pos)` to prepend bytes to NK suffixes.

**New**: Same logic but extract bytes via `extract_byte<BITS>(leaf_prefix(node))` since BITS decrements at each recursion.

Remove `pfx_u64` parameter — read from `leaf_prefix(node)` directly.

---

### kntrie_ops.hpp — convert_to_bitmask_tagged

**Current**: After building subtree, propagates old skip/prefix with complex combine logic.

**New**: 
- Build subtree via `build_node_from_arrays_tagged<BITS>(..., ik, ...)` — pass `ik` through so leaves get `node[2] = ik`.
- If old skip > 0 and result is a leaf: call `prepend_skip` with `ik` — just updates skip count + fn pointer. `node[2]` already has the right key.
- If old skip > 0 and result is a bitmask: `wrap_in_chain` with skip bytes extracted from `ik` (or `node[2]`). Descendant leaves' `node[2]` already have the correct key from build_node_from_arrays_tagged.

This is the KEY simplification — no more "combine old prefix into new prefix" logic.

---

### kntrie_ops.hpp — build_node_from_arrays_tagged

**Add** `uint64_t ik` parameter (any root-level key from the entries). Passed through to:
- `make_single_leaf<BITS>(ik, ...)` — which sets `node[2] = ik`
- `compact_ops::make_leaf(...)` + `init_leaf_fn<BITS>(node, ik)` — sets `node[2] = ik`
- Recursive calls: same `ik` (all entries share the prefix bits, suffix bits are don't-care)

---

### kntrie_ops.hpp — do_coalesce

Coalesces bitmask entries back into a compact leaf. Needs to set `node[2]` on the new leaf. Can reconstruct a full key from ANY collected entry's suffix: `key = suffix_to_u64<BITS>(suf[0])` OR'd with prefix bits. Or just grab `ik` from the first entry's full key.

Actually — coalesce collects entries at BITS level as NK suffixes. It doesn't have a root-level ik. But it can get one: if the coalescing node has leaves, grab any leaf's `node[2]` (which is a full key). Or: reconstruct from the collected suffix + any ancestor's known prefix. Simplest: the erase path that triggers coalesce has `ik` — pass it through.

---

### kntrie_impl.hpp — root_prefix_v

**Current**: Set to `ik` on first insert. Already a full root-level key.

**New**: Same. Already correct. The comparison `(ik ^ root_prefix_v) & mask` works because both are root-level.

When reduce_root_skip extracts bytes for wrap_in_chain, use same byte extraction as leaves: bytes sit at known positions in `root_prefix_v`. Use `pfx_byte(root_prefix_v, i)` which works because for root-level ik, the byte positions match pfx_byte's formula when KEY_BITS=64. **For KEY_BITS < 64**: root_prefix_v has the key shifted left by (64-KEY_BITS). So byte 0 of the key is at position `(64-KEY_BITS)/8` in the u64, not position 0. Need to use root-level byte extraction:

```cpp
// byte at root skip position i (i=0 is first skip byte)  
static_cast<uint8_t>(root_prefix_v >> (64 - KEY_BITS - 8 - 8*i))
```

Or equivalently: `extract_byte<KEY_BITS - 8*i>(root_prefix_v)` — reusing the same formula.

---

### kntrie_support.hpp — pfx_byte / pfx_to_bytes / pack_prefix

These helpers work on **left-aligned** prefix values. After the change, they're still needed for **bitmask skip chains** (which store raw bytes in a different format). They are NOT used for leaf prefix anymore.

Leaf prefix comparison switches to `extract_byte<BITS>` on `node[2]`. 

For bitmask chain byte extraction from root-level keys (reduce_root_skip, split_on_prefix), add a helper:

```cpp
template<int BITS>
static uint8_t key_byte(uint64_t key) noexcept {
    return static_cast<uint8_t>(key >> byte_shift<BITS>());
}
// Already exists as extract_byte<BITS>
```

For extracting N consecutive skip bytes from a root-level key starting at BITS level:
```cpp
template<int BITS>
static void extract_skip_bytes(uint64_t key, uint8_t* out, uint8_t count) noexcept {
    for (uint8_t i = 0; i < count; ++i)
        out[i] = static_cast<uint8_t>(key >> (byte_shift<BITS>() - 8*i));
}
```

---

## Summary of parameter changes

| Function | Old params | New params |
|---|---|---|
| `init_leaf_fn<BITS>` | `(node)` | `(node, ik)` |
| `make_single_leaf<BITS>` | unchanged | unchanged (already has ik) |
| `prepend_skip<BITS>` | `(node, len, pfx_u64, bld)` | `(node, len, ik, bld)` |
| `remove_skip<BITS>` | `(node, bld)` | unchanged (node[2] stays) |
| `insert_leaf_skip` | has `pfx_u64` param | remove it — read `leaf_prefix(node)` |
| `erase_leaf_skip` | has `pfx_u64` param | remove it — read `leaf_prefix(node)` |
| `collect_leaf_skip` | has `pfx_u64` param | remove it — read `leaf_prefix(node)` |
| `build_node_from_arrays_tagged` | `(suf, vals, count, bld)` | `(suf, vals, count, ik, bld)` |
| `convert_to_bitmask_tagged` | complex propagation | simplified with ik passthrough |
| `split_on_prefix` | `pfx_byte` extraction | `extract_byte<BITS>` on `node[2]` |

## What stays the same

- `suffix_to_u64` — already correct
- `to_suffix` — already correct  
- `extract_byte<BITS>` / `byte_shift<BITS>` — already correct
- Bitmask skip chain storage (raw bytes via `skip_byte`) — unchanged
- `wrap_in_chain` interface (takes raw byte array) — unchanged
- `collapse_info` — unchanged
- `root_prefix_v` storage — already a full key
- All compact_ops internals — unchanged
- All bitmap_ops internals — unchanged

## Deleted code

- `ik_to_pfx_space` — gone
- Left-aligned prefix combining in `prepend_skip_fn` — gone
- `rem_pfx` computation in `split_on_prefix` — gone
- Prefix propagation combine logic in `convert_to_bitmask_tagged` — simplified to single set
