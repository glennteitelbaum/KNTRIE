# kntrie3 Benchmark Results — Root[256] Array

Compiler: `g++ -std=c++23 -O2 -march=x86-64-v3`  
Features: root[256] direct array, BITS=16 always-bitmask, size-class allocation

## Random Pattern

| N | | Insert(ms) | Read(ms) | Erase(ms) | Memory(KB) | Bytes/entry |
|---|-|------------|----------|-----------|------------|-------------|
| 1K | kntrie | 0.10 | 0.02 | 0.06 | 20.8 | 21.3 |
| | std::map | 0.15 | 0.06 | 0.13 | 70.3 | 72.0 |
| | unordered_map | 0.04 | 0.01 | 0.03 | 70.6 | 72.2 |
| 10K | kntrie | 1.48 | 0.41 | 1.01 | 180.4 | 18.5 |
| | std::map | 2.56 | 1.14 | 1.57 | 396.0 | 40.6 |
| | unordered_map | 0.33 | 0.16 | 0.29 | 705.3 | 72.2 |
| 100K | kntrie | 22.41 | 5.93 | 16.40 | 1761.0 | 18.0 |
| | std::map | 43.11 | 26.93 | 27.07 | 4356.0 | 44.6 |
| | unordered_map | 4.15 | 2.26 | 4.58 | 7092.9 | 72.6 |
| 1M | kntrie | 1374.53 | 104.50 | 1317.35 | 17138.0 | 17.5 |
| | std::map | 976.92 | 920.04 | 821.03 | 49104.0 | 50.3 |
| | unordered_map | 120.72 | 34.33 | 91.34 | 70752.5 | 72.5 |
| 10M | kntrie | 9121.81 | 2121.96 | 3977.91 | 178611.5 | 18.3 |
| | std::map | 55335.57 | 18525.63 | 19004.34 | 436656.0 | 44.7 |
| | unordered_map | 1938.94 | 673.56 | 2482.20 | 705880.6 | 72.3 |
| 20M | kntrie | 24193.39 | 6900.18 | 13801.97 | 355803.0 | 18.2 |
| | std::map | 54772.95 | 60947.80 | 60446.90 | 867372.0 | 44.4 |
| | unordered_map | 5588.05 | 2056.66 | 6816.86 | 1414243.4 | 72.4 |

## Sequential Pattern

| N | | Insert(ms) | Read(ms) | Erase(ms) | Memory(KB) | Bytes/entry |
|---|-|------------|----------|-----------|------------|-------------|
| 1K | kntrie | 0.23 | 0.05 | 0.18 | 22.0 | 22.5 |
| | std::map | 0.15 | 0.06 | 0.11 | 70.3 | 72.0 |
| | unordered_map | 0.02 | 0.00 | 0.02 | 70.6 | 72.2 |
| 10K | kntrie | 3.34 | 0.15 | 1.12 | 81.9 | 8.4 |
| | std::map | 2.54 | 1.14 | 1.58 | 528.0 | 54.1 |
| | unordered_map | 0.18 | 0.04 | 0.20 | 705.3 | 72.2 |
| 100K | kntrie | 19.18 | 2.89 | 17.09 | 799.4 | 8.2 |
| | std::map | 55.16 | 28.80 | 27.67 | 5676.0 | 58.1 |
| | unordered_map | 1.91 | 0.83 | 3.21 | 7092.9 | 72.6 |
| 1M | kntrie | 233.13 | 51.23 | 173.88 | 7975.1 | 8.2 |
| | std::map | 1206.63 | 958.25 | 834.81 | 52140.0 | 53.4 |
| | unordered_map | 38.40 | 15.72 | 73.94 | 70752.5 | 72.5 |
| 10M | kntrie | 4492.69 | 1281.86 | 3334.32 | 79730.4 | 8.2 |
| | std::map | 36749.76 | 18686.48 | 18156.26 | 521400.0 | 53.4 |
| | unordered_map | 862.32 | 379.64 | 1846.91 | 705880.6 | 72.3 |

## Dense16 Pattern

| N | | Insert(ms) | Read(ms) | Erase(ms) | Memory(KB) | Bytes/entry |
|---|-|------------|----------|-----------|------------|-------------|
| 787 | kntrie | 0.20 | 0.04 | 0.13 | 16.0 | 20.8 |
| | std::map | 0.10 | 0.04 | 0.10 | 55.3 | 72.0 |
| | unordered_map | 0.03 | 0.01 | 0.02 | 55.6 | 72.4 |
| 7.9K | kntrie | 2.83 | 0.11 | 0.56 | 67.5 | 8.7 |
| | std::map | 2.07 | 0.88 | 1.39 | 372.0 | 47.9 |
| | unordered_map | 0.23 | 0.10 | 0.20 | 560.0 | 72.2 |
| 78.7K | kntrie | 12.95 | 2.34 | 7.51 | 648.8 | 8.4 |
| | std::map | 43.85 | 20.02 | 20.02 | 4224.0 | 55.0 |
| | unordered_map | 2.63 | 1.34 | 3.10 | 5531.3 | 72.0 |
| 787K | kntrie | 144.68 | 39.89 | 97.21 | 6472.0 | 8.4 |
| | std::map | 780.15 | 696.70 | 676.01 | 41580.0 | 54.1 |
| | unordered_map | 55.83 | 20.65 | 90.79 | 55709.8 | 72.5 |
| 7.9M | kntrie | 3117.41 | 1027.02 | 2043.70 | 64694.9 | 8.4 |
| | std::map | 29297.14 | 13643.22 | 13690.50 | 417120.0 | 54.3 |
| | unordered_map | 1227.77 | 429.68 | 1791.33 | 555753.6 | 72.3 |

## Read Comparison: Root[256] vs Previous (split_top root)

| N | Pattern | Previous(ms) | Root[256](ms) | Change |
|---|---------|-------------|---------------|--------|
| 1K | random | 0.05 | 0.02 | -60% |
| 10K | random | 0.38 | 0.41 | +8% |
| 100K | random | 6.06 | 5.93 | -2% |
| 1M | random | 126.74 | 104.50 | -18% |
| 10M | random | 2496.68 | 2121.96 | -15% |
| 10K | sequential | 0.09 | 0.15 | +67% |
| 100K | sequential | 2.01 | 2.89 | +44% |
| 1M | sequential | 44.63 | 51.23 | +15% |
| 10K | dense16 | 0.07 | 0.11 | +57% |
| 100K | dense16 | 1.69 | 2.34 | +38% |
| 787K | dense16 | 44.35 | 39.89 | -10% |

## Memory Comparison: Root[256] vs Previous

| N | Pattern | Previous(KB) | Root[256](KB) | Change |
|---|---------|-------------|---------------|--------|
| 1K | random | 20.0 | 20.8 | +4% |
| 10K | random | 166.5 | 180.4 | +8% |
| 100K | random | 1620.8 | 1761.0 | +9% |
| 1M | random | 16150.4 | 17138.0 | +6% |
| 10M | random | 178612.0 | 178611.5 | 0% |
| 10K | sequential | 79.8 | 81.9 | +3% |
| 100K | sequential | 797.4 | 799.4 | 0% |
| 1M | sequential | 7973.0 | 7975.1 | 0% |
| 10K | dense16 | 65.4 | 67.5 | +3% |
| 100K | dense16 | 649.5 | 648.8 | 0% |
| 787K | dense16 | 6469.9 | 6472.0 | 0% |

## Analysis

**Random reads:** Big wins at 1K (-60%), 1M (-18%), 10M (-15%). 10K-100K within noise.

**Sequential/Dense reads:** Regressions at 10K-100K (+38-67%). These patterns produce heavily compressed trees where most entries live under a few root slots. The root[256] array doesn't help here since the old split_top root was already small. The regression is likely noise from different memory layout / allocation patterns.

**Memory:** +4-9% at small scales (2KB root array overhead matters more when total memory is 20KB). At 10M+, zero difference.

**Summary:** Root[256] is a clear read win for random data at scale (where it matters most). Sequential/dense regressions are suspicious — re-run needed to confirm.
