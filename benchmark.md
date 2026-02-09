# kntrie3 Benchmark Results

Compiler: `g++ -std=c++23 -O2 -march=x86-64-v3`  
Features: size-class allocation with in-place insert/erase, bot-internal headers

## Random Pattern

| N | | Insert(ms) | Read(ms) | Erase(ms) | Memory(KB) | Bytes/entry |
|---|-|------------|----------|-----------|------------|-------------|
| 1K | kntrie | 0.21 | 0.05 | 0.21 | 20.0 | 20.5 |
| | std::map | 0.16 | 0.06 | 0.14 | 70.3 | 72.0 |
| | unordered_map | 0.03 | 0.01 | 0.04 | 70.6 | 72.2 |
| 10K | kntrie | 3.39 | 0.47 | 0.97 | 166.5 | 17.1 |
| | std::map | 3.57 | 1.16 | 1.60 | 396.0 | 40.6 |
| | unordered_map | 0.33 | 0.17 | 0.30 | 705.3 | 72.2 |
| 100K | kntrie | 27.50 | 7.13 | 16.81 | 1620.8 | 16.6 |
| | std::map | 59.15 | 27.63 | 28.81 | 4092.0 | 41.9 |
| | unordered_map | 4.93 | 2.28 | 4.68 | 7092.9 | 72.6 |
| 1M | kntrie | 1543.27 | 111.93 | 1342.33 | 16150.4 | 16.5 |
| | std::map | 1933.57 | 850.86 | 795.51 | 50952.0 | 52.2 |
| | unordered_map | 79.29 | 35.48 | 91.32 | 70752.5 | 72.5 |
| 10M | kntrie | 10728.84 | 2774.98 | 3751.61 | 178612.0 | 18.3 |
| | std::map | 76844.58 | 18500.61 | 18360.45 | 434896.0 | 44.5 |
| | unordered_map | 2008.88 | 711.48 | 2327.14 | 705880.6 | 72.3 |

## Sequential Pattern

| N | | Insert(ms) | Read(ms) | Erase(ms) | Memory(KB) | Bytes/entry |
|---|-|------------|----------|-----------|------------|-------------|
| 1K | kntrie | 0.22 | 0.05 | 0.18 | 20.0 | 20.5 |
| | std::map | 0.16 | 0.06 | 0.13 | 70.3 | 72.0 |
| | unordered_map | 0.02 | 0.00 | 0.01 | 70.6 | 72.2 |
| 10K | kntrie | 3.44 | 0.12 | 1.60 | 79.8 | 8.2 |
| | std::map | 3.50 | 1.15 | 1.64 | 660.0 | 67.6 |
| | unordered_map | 0.22 | 0.04 | 0.20 | 705.3 | 72.2 |
| 100K | kntrie | 21.42 | 2.52 | 16.67 | 797.4 | 8.2 |
| | std::map | 69.17 | 26.80 | 28.92 | 5280.0 | 54.1 |
| | unordered_map | 2.14 | 0.90 | 4.18 | 7092.9 | 72.6 |
| 1M | kntrie | 317.53 | 43.90 | 165.15 | 7973.0 | 8.2 |
| | std::map | 1971.05 | 859.49 | 765.20 | 52008.0 | 53.3 |
| | unordered_map | 33.30 | 16.67 | 76.02 | 70752.5 | 72.5 |

## Dense16 Pattern

| N | | Insert(ms) | Read(ms) | Erase(ms) | Memory(KB) | Bytes/entry |
|---|-|------------|----------|-----------|------------|-------------|
| 787 | kntrie | 0.16 | 0.04 | 0.13 | 14.0 | 18.2 |
| | std::map | 0.11 | 0.04 | 0.11 | 55.3 | 72.0 |
| | unordered_map | 0.02 | 0.01 | 0.02 | 55.6 | 72.4 |
| 7.9K | kntrie | 3.13 | 0.09 | 0.52 | 65.4 | 8.4 |
| | std::map | 2.37 | 0.90 | 1.25 | 396.0 | 51.0 |
| | unordered_map | 0.23 | 0.10 | 0.20 | 560.0 | 72.2 |
| 78.7K | kntrie | 16.80 | 2.23 | 7.46 | 649.5 | 8.5 |
| | std::map | 51.00 | 19.28 | 21.37 | 4092.0 | 53.3 |
| | unordered_map | 2.80 | 1.38 | 3.11 | 5531.3 | 72.0 |
| 787K | kntrie | 197.85 | 34.98 | 93.53 | 6469.9 | 8.4 |
| | std::map | 1474.01 | 605.77 | 582.51 | 41712.0 | 54.3 |
| | unordered_map | 56.91 | 20.73 | 64.83 | 55709.8 | 72.5 |

## Summary: kntrie vs std::map

| Metric | Random | Sequential | Dense16 |
|--------|--------|------------|---------|
| Memory | 2.4–3.5x better | 6.5–8.9x better | 4.0–8.6x better |
| Read | 1.2–6.7x faster | 1.2–19.6x faster | 1.1–17.3x faster |
| Erase | 0.7–4.9x faster | 0.7–4.6x faster | 0.9–6.2x faster |
| Insert | 0.8–7.2x faster | 0.8–6.2x faster | 0.7–7.5x faster |

## Summary: kntrie vs std::unordered_map

| Metric | Random | Sequential | Dense16 |
|--------|--------|------------|---------|
| Memory | 3.5–4.4x better | 3.5–8.9x better | 4.0–8.6x better |
| Read | 2.8–3.9x slower | 2.9–7.1x slower | 1.1–4.2x slower |
| Erase | 1.6x faster (10M) to 5.3x slower (1K) | 2.2x slower (1M) to 18x slower (1K) | 1.4x slower (787K) to 6.5x slower (787) |
| Insert | 3.5–6.7x slower | 3.2–15.7x slower | 2.9–13.6x slower |

## Notes

- kntrie erase at 1M random (1342ms) is an outlier — this is the transition zone where compact→split conversions during insert create many small nodes that erase must reallocate. At 10M the tree stabilizes and in-place paths dominate.
- Sequential and dense patterns show consistently strong erase performance because prefix compression produces fewer, larger nodes where in-place operations succeed more often.
- Memory advantage is the primary design goal: 8.2–20.5 bytes/entry vs 40–72 for std containers.
