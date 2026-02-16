# Iterator Plan

## Design

Snapshot iterators. Each iterator holds:
- `const kntrie_impl* parent_` — for re-traversal
- `KEY key_` — current key
- `VALUE value_` — copy of current value (iterator owns it)
- `bool valid_` — false for end()/rend()

No node pointers stored. Every `++`/`--` re-traverses from root using `iter_next(key)` / `iter_prev(key)`. Depth ≤ 8 for u64, bounded cost.

## Public API (kntrie.hpp)

```cpp
class iterator {
    const kntrie_impl<KEY,VALUE,ALLOC>* parent_;
    KEY key_;
    VALUE value_;
    bool valid_;
public:
    using value_type = std::pair<const KEY, VALUE>;
    
    const KEY& key() const;
    const VALUE& value() const;
    // operator* returns pair<const KEY&, const VALUE&> or similar
    
    iterator& operator++();   // iter_next(key_)
    iterator& operator--();   // iter_prev(key_)
    bool operator==(const iterator&) const;  // both invalid = equal (end==end)
};
```

- `begin()` → `iter_first()`
- `end()` → `iterator{nullptr, {}, {}, false}`
- `rbegin()` → `iter_last()`
- `rend()` → same as end

## Internal API (kntrie_impl)

Four functions, each returns `iter_result_t`:

```cpp
struct iter_result_t {
    KEY key;
    VALUE value;
    bool found;   // false = no result (begin of empty, past-end, past-rend)
};

iter_first()          — smallest key in trie
iter_last()           — largest key in trie
iter_next(KEY key)    — smallest key > key
iter_prev(KEY key)    — largest key < key
```

Each dispatches to leaf type (compact, bitmap256) via the node's suffix_type.

## Algorithm: iter_first / iter_last

Top-down descent always taking min (or max) child at each level.

**iter_first (min):**
1. Start at root tagged ptr
2. If bitmask: follow skip chain (single path), then at final bitmap use `first_set_bit()` → slot 0. Recurse into that child.
3. If leaf: return first (min) entry.

**iter_last (max):**
Same but use `last_set_bit()` → last slot / last entry at each level.

## Algorithm: iter_next(key)

Single top-down pass. Walk down following `key` byte-by-byte. Track the "resume point" — the deepest ancestor where a next-sibling exists.

```
resume_depth = -1
resume_next_slot = 0

for each level:
    byte = extract byte from key
    if bitmask node:
        r = bitmap.next_set_after(byte)
        if r.found: save (depth, r) as resume point
        descend into child for byte (must exist since key was found)
    if leaf:
        search for suffix > key's suffix
        if found: reconstruct full key, return it
        if not found: use resume point
            go back to resume_depth
            descend into resume child
            take iter_first of that subtree (min descendant via first_set_bit)
            reconstruct full key from path + min suffix
```

Skip chains: each embedded bitmap is single-bit, so there's never a "next sibling" within a skip — resume can only be at a final bitmask level. Just traverse through.

## Algorithm: iter_prev(key)

Mirror of iter_next. Use `prev_set_before(byte)` to track deepest ancestor where a prev-sibling exists.

At leaf: search for largest suffix < key's suffix.
At resume: descend into prev child, take iter_last (max descendant via `last_set_bit`).

## Bitmap256 Operations — Existing

Already implemented (replacing old `find_next_set`):

- **`for_each_set(fn)`** — single-pass iteration of all set bits. fn(uint8_t idx, int slot).
- **`first_set_bit()`** — lowest set bit index. Used by iter_first for min descent.
- **`last_set_bit()`** — highest set bit index. Needed for iter_last for max descent.

## Bitmap256 Operations — New for Iterators

### next_set_after(idx) — find smallest set bit > idx

Single pass: find bit and accumulate slot together. No redundant popcount work.

```cpp
struct adj_result { uint8_t idx; uint16_t slot; bool found; };

adj_result next_set_after(uint8_t idx) const noexcept {
    if (idx == 255) return {0, 0, false};
    int start = idx + 1;
    int w = start >> 6, b = start & 63;

    // Accumulate slot: popcount of all set bits in words before w
    int slot = 0;
    for (int i = 0; i < w; ++i)
        slot += std::popcount(words[i]);

    // Mask current word: clear bits below start
    uint64_t m = words[w] & (~0ULL << b);
    if (m) {
        int bit = (w << 6) + std::countr_zero(m);
        slot += std::popcount(words[w] & ((1ULL << (bit & 63)) - 1));
        return {static_cast<uint8_t>(bit), static_cast<uint16_t>(slot), true};
    }
    slot += std::popcount(words[w]);

    // Scan remaining words
    for (int ww = w + 1; ww < 4; ++ww) {
        if (words[ww]) {
            int bit = (ww << 6) + std::countr_zero(words[ww]);
            slot += std::popcount(words[ww] & ((1ULL << (bit & 63)) - 1));
            return {static_cast<uint8_t>(bit), static_cast<uint16_t>(slot), true};
        }
        slot += std::popcount(words[ww]);
    }
    return {0, 0, false};
}
```

### prev_set_before(idx) — find largest set bit < idx

Single pass: find bit, compute slot only when found.

```cpp
adj_result prev_set_before(uint8_t idx) const noexcept {
    if (idx == 0) return {0, 0, false};
    int last = idx - 1;
    int w = last >> 6, b = last & 63;

    // Mask current word: keep bits 0..b inclusive
    uint64_t m = words[w] & ((2ULL << b) - 1);

    // Scan backward
    for (int ww = w; ww >= 0; --ww) {
        uint64_t bits = (ww == w) ? m : words[ww];
        if (bits) {
            int bit = (ww << 6) + 63 - std::countl_zero(bits);
            // Compute slot: popcount of words[0..ww-1] + bits below hit in words[ww]
            int slot = 0;
            for (int i = 0; i < ww; ++i)
                slot += std::popcount(words[i]);
            slot += std::popcount(words[ww] & ((1ULL << (bit & 63)) - 1));
            return {static_cast<uint8_t>(bit), static_cast<uint16_t>(slot), true};
        }
    }
    return {0, 0, false};
}
```

## Leaf Operations Needed

### Compact leaf: next/prev after key suffix

Sorted keys already. Binary search to find position of key.

- **next**: if found at pos i, return entry i+1 (if exists). If not found, insertion point IS the next.
- **prev**: if found at pos i, return entry i-1 (if exists). If not found, insertion point - 1.

### Bitmap256 leaf: next/prev suffix

Suffix is a single byte (bit index in bitmap). Use `next_set_after(suffix)` / `prev_set_before(suffix)` on the leaf's bitmap. Slot gives value index.

## Key Reconstruction

As we descend, accumulate prefix bytes. At each bitmask level, the byte consumed is known. Skip chain bytes are known. At the leaf, the suffix provides remaining bits. Reconstruct full KEY from accumulated prefix + leaf suffix.

Implementation: maintain a `uint64_t prefix` and `int prefix_bits` during descent. Shift and OR bytes in. At leaf, combine with suffix.

## Edge Cases

- Empty trie: begin() == end()
- Single entry: begin() valid, ++begin() == end()
- iter_next on last key: returns found=false → becomes end()
- iter_prev on first key: returns found=false → becomes rend()
- Key not currently in trie (erased between snapshots): iter_next/prev still work since they search for >/< rather than exact match

## File Organization

- `bitmap256::for_each_set()`, `first_set_bit()`, `last_set_bit()` (existing/add) → kntrie_bitmask.hpp
- `bitmap256::next_set_after()`, `prev_set_before()` (new) → kntrie_bitmask.hpp
- `find_next_set()` removed — replaced by above
- `iter_first_`, `iter_last_`, `iter_next_`, `iter_prev_` → kntrie_impl.hpp
- `iterator` class, `begin()`, `end()` → kntrie.hpp

## Testing

- Sequential insert N, iterate forward, verify sorted order
- Sequential insert N, iterate backward, verify reverse sorted order
- Random insert N, iterate forward, compare with std::set
- iter_next / iter_prev at boundaries (first/last key)
- Empty trie iteration
- After erase: iterate and verify consistency
