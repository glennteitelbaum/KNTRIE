# Iterator Implementation — Step Checklist

**UPDATE THIS FILE as each step completes. Mark done with [x].**

## Phase 1: bitmap256 helpers (kntrie_bitmask.hpp)

- [x] Add `last_set_bit()` — mirror of `first_set_bit()`, scan words 3→0
- [x] Add `adj_result` struct `{ uint8_t idx; uint16_t slot; bool found; }`
- [x] Add `next_set_after(uint8_t idx)` — single-pass forward scan
- [x] Add `prev_set_before(uint8_t idx)` — single-pass backward scan
- [x] Unit test: bitmap256 helpers (all four functions, edge cases 0/255, sparse/dense)
- [x] **STOP**: zip all headers + plan.md + miniplan.md, wait for user

## Phase 2: leaf next/prev (kntrie_compact.hpp, kntrie_bitmask.hpp)

- [ ] Compact leaf: `iter_next_suffix(node, suffix)` — binary search, return next entry
- [ ] Compact leaf: `iter_prev_suffix(node, suffix)` — binary search, return prev entry
- [ ] Bitmap256 leaf: `iter_next_suffix(node, byte)` — uses `next_set_after`
- [ ] Bitmap256 leaf: `iter_prev_suffix(node, byte)` — uses `prev_set_before`
- [ ] Compact leaf: `iter_first(node)` / `iter_last(node)` — min/max entry
- [ ] Bitmap256 leaf: `iter_first(node)` / `iter_last(node)` — min/max entry
- [ ] **STOP**: zip all headers + plan.md + miniplan.md, wait for user

## Phase 3: kntrie_impl traversal (kntrie_impl.hpp)

- [ ] Add `iter_result_t { KEY key; VALUE value; bool found; }`
- [ ] Implement `iter_first_()` — descend always-min from root
- [ ] Implement `iter_last_()` — descend always-max from root
- [ ] Implement `iter_next_(KEY key)` — descend tracking resume point, next_set_after
- [ ] Implement `iter_prev_(KEY key)` — descend tracking resume point, prev_set_before
- [ ] Key reconstruction logic (accumulate prefix bytes during descent)
- [ ] **STOP**: zip all headers + plan.md + miniplan.md, wait for user

## Phase 4: iterator class (kntrie.hpp)

- [ ] Define `iterator` class (parent ptr, key, value copy, valid bool)
- [ ] `operator++` calls `parent_->iter_next_(key_)`
- [ ] `operator--` calls `parent_->iter_prev_(key_)`
- [ ] `operator==` (both invalid = equal)
- [ ] `operator*` / `operator->` (returns key/value ref)
- [ ] `begin()` / `end()` / `rbegin()` / `rend()`
- [ ] **STOP**: zip all headers + plan.md + miniplan.md, wait for user

## Phase 5: testing

- [ ] Forward iteration matches sorted order (sequential + random)
- [ ] Reverse iteration matches reverse sorted order
- [ ] Compare with std::set iteration
- [ ] Boundary: ++end, --begin
- [ ] Empty trie: begin() == end()
- [ ] Single entry: begin valid, ++begin == end
- [ ] After erase: iterate and verify
- [ ] ASAN clean on all above
- [ ] **STOP**: zip all headers + plan.md + miniplan.md, wait for user

## Phase 6: zip and deliver

- [ ] All tests pass
- [ ] Zip all headers
