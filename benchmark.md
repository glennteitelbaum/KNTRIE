# kntrie3 Benchmark Results (Phase 1)

Compiler: `g++ -std=c++23 -O2 -march=x86-64-v3`
Features: Phase 1 structural changes — fixed 2-u64 header, root[256] array, template insert dispatch

In "vs" rows, >1x means kntrie is better (competitor is slower/larger).

## uint64_t Keys — Random

| N | | Insert(ms) | Read(ms) | Memory(KB) | B/entry |
|---|-|------------|----------|------------|---------|
| 1K | kntrie | 0.10 | 0.02 | 21.7 | 22.2 |
| | std::map | 0.15 | 0.03 | 70.3 | 72.0 |
| | _map vs_ | _1.47x_ | _1.55x_ | _3.24x_ | _3.24x_ |
| | unordered_map | 0.05 | 0.01 | 70.6 | 72.2 |
| | _umap vs_ | _0.44x_ | _0.82x_ | _3.25x_ | _3.25x_ |
| 10K | kntrie | 1.12 | 0.30 | 175.7 | 18.0 |
| | std::map | 2.74 | 0.45 | 396.0 | 40.6 |
| | _map vs_ | _2.44x_ | _1.48x_ | _2.25x_ | _2.25x_ |
| | unordered_map | 0.43 | 0.17 | 705.3 | 72.2 |
| | _umap vs_ | _0.38x_ | _0.55x_ | _4.01x_ | _4.01x_ |
| 100K | kntrie | 15.08 | 4.89 | 1701.0 | 17.4 |
| | std::map | 52.31 | 9.90 | 5940.0 | 60.8 |
| | _map vs_ | _3.47x_ | _2.02x_ | _3.49x_ | _3.49x_ |
| | unordered_map | 4.79 | 2.34 | 7092.9 | 72.6 |
| | _umap vs_ | _0.32x_ | _0.48x_ | _4.17x_ | _4.17x_ |
| 1M | kntrie | 1107.10 | 131.25 | 16386.0 | 16.8 |
| | std::map | 1066.14 | 229.99 | 62172.0 | 63.7 |
| | _map vs_ | _0.96x_ | _1.75x_ | _3.79x_ | _3.79x_ |
| | unordered_map | 73.81 | 34.39 | 70752.5 | 72.5 |
| | _umap vs_ | _0.07x_ | _0.26x_ | _4.32x_ | _4.32x_ |

## uint64_t Keys — Sequential

| N | | Insert(ms) | Read(ms) | Memory(KB) | B/entry |
|---|-|------------|----------|------------|---------|
| 1K | kntrie | 0.15 | 0.04 | 18.0 | 18.4 |
| | std::map | 0.14 | 0.03 | 70.3 | 72.0 |
| | _map vs_ | _0.93x_ | _0.63x_ | _3.91x_ | _3.91x_ |
| | unordered_map | 0.02 | 0.00 | 70.6 | 72.2 |
| | _umap vs_ | _0.14x_ | _0.08x_ | _3.92x_ | _3.92x_ |
| 10K | kntrie | 2.40 | 0.12 | 82.4 | 8.4 |
| | std::map | 2.67 | 0.46 | 528.0 | 54.1 |
| | _map vs_ | _1.11x_ | _3.91x_ | _6.40x_ | _6.40x_ |
| | unordered_map | 0.19 | 0.04 | 705.3 | 72.2 |
| | _umap vs_ | _0.08x_ | _0.30x_ | _8.56x_ | _8.56x_ |
| 100K | kntrie | 19.89 | 2.56 | 805.6 | 8.2 |
| | std::map | 53.74 | 9.90 | 5676.0 | 58.1 |
| | _map vs_ | _2.70x_ | _3.86x_ | _7.05x_ | _7.05x_ |
| | unordered_map | 2.05 | 0.85 | 7092.9 | 72.6 |
| | _umap vs_ | _0.10x_ | _0.33x_ | _8.80x_ | _8.80x_ |
| 1M | kntrie | 241.08 | 40.79 | 8036.1 | 8.2 |
| | std::map | 1266.26 | 232.99 | 58212.0 | 59.6 |
| | _map vs_ | _5.25x_ | _5.71x_ | _7.24x_ | _7.24x_ |
| | unordered_map | 32.93 | 14.04 | 70752.5 | 72.5 |
| | _umap vs_ | _0.14x_ | _0.34x_ | _8.80x_ | _8.80x_ |

## uint64_t Keys — Dense16

| N | | Insert(ms) | Read(ms) | Memory(KB) | B/entry |
|---|-|------------|----------|------------|---------|
| 787 | kntrie | 0.11 | 0.03 | 16.0 | 20.8 |
| | std::map | 0.12 | 0.02 | 55.3 | 72.0 |
| | _map vs_ | _1.04x_ | _0.64x_ | _3.46x_ | _3.46x_ |
| | unordered_map | 0.02 | 0.01 | 55.6 | 72.4 |
| | _umap vs_ | _0.19x_ | _0.24x_ | _3.48x_ | _3.48x_ |
| 7.9K | kntrie | 2.24 | 0.11 | 68.6 | 8.8 |
| | std::map | 2.38 | 0.36 | 396.0 | 51.0 |
| | _map vs_ | _1.06x_ | _3.38x_ | _5.77x_ | _5.77x_ |
| | unordered_map | 0.22 | 0.10 | 560.0 | 72.2 |
| | _umap vs_ | _0.10x_ | _0.95x_ | _8.17x_ | _8.17x_ |
| 78.7K | kntrie | 12.24 | 1.96 | 661.0 | 8.6 |
| | std::map | 33.02 | 7.16 | 4092.0 | 53.3 |
| | _map vs_ | _2.70x_ | _3.65x_ | _6.19x_ | _6.19x_ |
| | unordered_map | 3.03 | 1.39 | 5531.3 | 72.0 |
| | _umap vs_ | _0.25x_ | _0.71x_ | _8.37x_ | _8.37x_ |
| 787K | kntrie | 151.78 | 33.24 | 6594.0 | 8.6 |
| | std::map | 816.11 | 170.42 | 41580.0 | 54.1 |
| | _map vs_ | _5.38x_ | _5.13x_ | _6.31x_ | _6.31x_ |
| | unordered_map | 45.48 | 20.37 | 55709.8 | 72.5 |
| | _umap vs_ | _0.30x_ | _0.61x_ | _8.45x_ | _8.45x_ |

## int32_t Keys — Random

| N | | Insert(ms) | Read(ms) | Memory(KB) | B/entry |
|---|-|------------|----------|------------|---------|
| 1K | kntrie | 0.10 | 0.02 | 18.4 | 18.9 |
| | std::map | 0.13 | 0.07 | 70.3 | 72.0 |
| | _map vs_ | _1.36x_ | _3.80x_ | _3.81x_ | _3.81x_ |
| | unordered_map | 0.03 | 0.01 | 70.6 | 72.2 |
| | _umap vs_ | _0.35x_ | _0.78x_ | _3.83x_ | _3.83x_ |
| 10K | kntrie | 0.73 | 0.30 | 134.4 | 13.8 |
| | std::map | 1.96 | 1.13 | 703.1 | 72.0 |
| | _map vs_ | _2.68x_ | _3.75x_ | _5.23x_ | _5.23x_ |
| | unordered_map | 0.32 | 0.17 | 705.3 | 72.2 |
| | _umap vs_ | _0.44x_ | _0.58x_ | _5.25x_ | _5.25x_ |
| 100K | kntrie | 10.05 | 4.77 | 1291.0 | 13.2 |
| | std::map | 53.00 | 26.45 | 4620.0 | 47.3 |
| | _map vs_ | _5.27x_ | _5.55x_ | _3.58x_ | _3.58x_ |
| | unordered_map | 4.86 | 2.31 | 7092.8 | 72.6 |
| | _umap vs_ | _0.48x_ | _0.48x_ | _5.49x_ | _5.49x_ |
| 1M | kntrie | 649.91 | 87.97 | 12510.9 | 12.8 |
| | std::map | 659.64 | 616.39 | 24816.0 | 25.4 |
| | _map vs_ | _1.01x_ | _7.01x_ | _1.98x_ | _1.98x_ |
| | unordered_map | 69.58 | 34.37 | 70745.7 | 72.5 |
| | _umap vs_ | _0.11x_ | _0.39x_ | _5.65x_ | _5.65x_ |

## int32_t Keys — Sequential

| N | | Insert(ms) | Read(ms) | Memory(KB) | B/entry |
|---|-|------------|----------|------------|---------|
| 1K | kntrie | 0.11 | 0.04 | 14.0 | 14.3 |
| | std::map | 0.12 | 0.06 | 70.3 | 72.0 |
| | _map vs_ | _1.09x_ | _1.49x_ | _5.02x_ | _5.02x_ |
| | unordered_map | 0.02 | 0.00 | 70.6 | 72.2 |
| | _umap vs_ | _0.17x_ | _0.08x_ | _5.04x_ | _5.04x_ |
| 10K | kntrie | 1.73 | 0.10 | 82.4 | 8.4 |
| | std::map | 1.57 | 1.14 | 703.1 | 72.0 |
| | _map vs_ | _0.91x_ | _10.98x_ | _8.53x_ | _8.53x_ |
| | unordered_map | 0.18 | 0.04 | 705.3 | 72.2 |
| | _umap vs_ | _0.10x_ | _0.35x_ | _8.56x_ | _8.56x_ |
| 100K | kntrie | 14.01 | 1.32 | 805.4 | 8.2 |
| | std::map | 39.38 | 26.01 | 3960.0 | 40.6 |
| | _map vs_ | _2.81x_ | _19.64x_ | _4.92x_ | _4.92x_ |
| | unordered_map | 2.02 | 0.79 | 7092.9 | 72.6 |
| | _umap vs_ | _0.14x_ | _0.59x_ | _8.81x_ | _8.81x_ |
| 1M | kntrie | 159.33 | 26.00 | 8036.0 | 8.2 |
| | std::map | 804.25 | 647.85 | 25344.0 | 26.0 |
| | _map vs_ | _5.05x_ | _24.91x_ | _3.15x_ | _3.15x_ |
| | unordered_map | 29.13 | 13.73 | 70752.5 | 72.5 |
| | _umap vs_ | _0.18x_ | _0.53x_ | _8.80x_ | _8.80x_ |

## int32_t Keys — Dense16

| N | | Insert(ms) | Read(ms) | Memory(KB) | B/entry |
|---|-|------------|----------|------------|---------|
| 787 | kntrie | 0.09 | 0.03 | 12.0 | 15.6 |
| | std::map | 0.10 | 0.05 | 55.3 | 72.0 |
| | _map vs_ | _1.07x_ | _1.48x_ | _4.61x_ | _4.61x_ |
| | unordered_map | 0.02 | 0.01 | 55.6 | 72.4 |
| | _umap vs_ | _0.26x_ | _0.27x_ | _4.63x_ | _4.63x_ |
| 7.9K | kntrie | 1.17 | 0.08 | 68.6 | 8.8 |
| | std::map | 1.24 | 0.88 | 558.6 | 72.0 |
| | _map vs_ | _1.06x_ | _10.56x_ | _8.15x_ | _8.15x_ |
| | unordered_map | 0.22 | 0.10 | 560.0 | 72.2 |
| | _umap vs_ | _0.18x_ | _1.23x_ | _8.17x_ | _8.17x_ |
| 78.7K | kntrie | 7.92 | 1.05 | 660.5 | 8.6 |
| | std::map | 30.15 | 18.87 | 3432.0 | 44.7 |
| | _map vs_ | _3.81x_ | _17.98x_ | _5.20x_ | _5.20x_ |
| | unordered_map | 2.62 | 1.36 | 5531.3 | 72.0 |
| | _umap vs_ | _0.33x_ | _1.29x_ | _8.37x_ | _8.37x_ |
| 787K | kntrie | 96.34 | 20.43 | 6593.4 | 8.6 |
| | std::map | 563.75 | 447.28 | 27588.0 | 35.9 |
| | _map vs_ | _5.85x_ | _21.89x_ | _4.18x_ | _4.18x_ |
| | unordered_map | 42.55 | 20.31 | 55709.8 | 72.5 |
| | _umap vs_ | _0.44x_ | _0.99x_ | _8.45x_ | _8.45x_ |

## Notes

- Phase 1 structural changes: fixed 2-u64 header (was 1-2), root[256] array (was single root node), template insert dispatch
- int32_t keys show dramatically better read performance vs std::map due to shallower trie (2 levels vs 4 for uint64_t)
- int32_t sequential 1M: 24.91x faster reads vs std::map, 8.2 bytes/entry
- int32_t dense16 787K: 21.89x faster reads vs std::map, nearly matching unordered_map read speed
- Memory converges to 8.2-8.8 bytes/entry for compressed patterns regardless of key type
- kntrie reads are 2-4x slower than unordered_map for random uint64_t, but gap narrows for int32_t and compressed patterns
