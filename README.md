### KNTRIE



A KTRIE for Integral keys



> It's what would happen if an array-of-arrays had a child with a trie



A **sorted**, **compressed** associative container for integer keys (`uint8_t` through `uint64_t`, signed and unsigned). Follows the `std::map` interface. **Header-only**, **C++23**, requires **x86-64-v3** (`popcnt`, `tzcnt`, `lzcnt`) for best performance.



* [Concepts](./concepts.md)


Performance charts:


* [u16](https://glennteitelbaum.github.io/KNTRIE/chart16.html)
* [u64](https://glennteitelbaum.github.io/KNTRIE/chart64.html)
