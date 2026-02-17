# kntrie Naming Conventions

Goal: fast visual parsing — know where a name came from at a glance.
Not hungarian notation. Not type encoding. Just origin and role.

## Variable Suffixes

| Suffix | Meaning | At a glance |
|--------|---------|-------------|
| `_v` | Data member (field) | Our state — careful about mutation |
| `_p` | Pointer | Could be null, needs deref |
| `_r` | Reference | Alias to something else |
| `_m` | Moved-from (rvalue ref) | Consumed after use |
| `_l` | Lambda | Captures state, watch lifetime |
| `_` | Function argument | Came from caller, don't own it |
| *(none)* | Local variable | Defined nearby, easy to find |

Suffixes **stack** right-to-left (innermost type first):
- `is_found_rpp` = reference to pointer-to-pointer to bool
- `node_pp` = pointer to pointer to node

## Boolean Names

Booleans (variables and functions) start with `is_` or `has_`:
- `is_` for state: `is_leaf`, `is_skip`, `is_empty`
- `has_` for containment: `has_bit`, `has_child`

## Casing Rules

| Kind | Case | Examples |
|------|------|---------|
| Classes, structs | `lower_snake` | `kntrie_ops`, `node_header` |
| Functions, methods | `lower_snake` | `find_node`, `descend_min` |
| Variables | `lower_snake` | `skip_count`, `byte` |
| Namespaces | `lower_snake` | `gteitelbaum` |
| File names | `lower_snake` | `kntrie_ops.hpp` |
| Enum values | `ALL_CAPS` | `UNFILTERED`, `BRANCHLESS` |
| `constexpr` / `static const` | `ALL_CAPS` | `LEAF_BIT`, `SENTINEL_TAGGED` |
| Template params | `ALL_CAPS` | `BITS`, `NK`, `VALUE`, `ALLOC` |
| `using` / type aliases | `ALL_CAPS` | `BO`, `CO`, `NNK`, `VST` |
| Macros | `ALL_CAPS` | `KNTRIE_OPS_HPP` |

**Exception**: `using` aliases required by STL interface are `lower_snake`:
`using value_type = ...`, `using iterator = ...`, `using size_type = ...`

## Functions

- Functions **never** end in `_` or any variable suffix (`_v`, `_p`, `_r`, `_m`, `_l`)
- If you see `name_()` — that's variable `name_` being called (lambda/functor), not a function

## Types

- POD structs use `struct x_t`: `insert_result_t`, `iter_ops_result_t`, `node_header_t`
- Non-POD uses `class x`: `kntrie`, `kntrie_impl`
- POSIX reserves `_t` globally; safe inside `gteitelbaum::` namespace

## Templates

- Always `template<typename T>`, never `template<class T>`
- `class` is misleading when params are primitives like `uint64_t`, `int`

## Quick Reference

```cpp
template<int BITS, typename NK>                 // ALL_CAPS, typename not class
struct kntrie_ops {                             // lower_snake struct (stateless ops)
    using BO = bitmask_ops<VALUE, ALLOC>;       // ALL_CAPS alias
    using NARROW = kntrie_ops<NNK, VALUE, ALLOC>; // ALL_CAPS alias
    static constexpr int NK_BITS = 8;           // ALL_CAPS const

    uint64_t root_v;                            // data member
    size_t   size_v;                            // data member

    static void find_node(uint64_t ptr_,        // function: no trailing _
                          NK ik_) {             // args: trailing _
        auto* hdr_p = get_header(node);         // local pointer
        uint8_t skip = hdr_p->skip();           // local value
        bool is_leaf = ptr_ & LEAF_BIT;         // local bool
        auto visit_l = [&](uint64_t c_) {};     // lambda
    }
};
```
