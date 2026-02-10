# kntrie3 Benchmark Results (Post Embed Removal)

Compiler: `g++ -std=c++23 -O2 -march=x86-64-v3`  
Features: root[256] array, size-class allocation, no embedding

## Random Pattern

| N | | Insert(ms) | Read(ms) | Erase(ms) | Memory(KB) | Bytes/entry |
|---|-|------------|----------|-----------|------------|-------------|
| 1K | kntrie | 0.08 | 0.02 | 0.05 | 20.8 | 21.3 |
| | std::map | 0.13 | 0.06 | 0.11 | 70.3 | 72.0 |
| | unordered_map | 0.03 | 0.01 | 0.02 | 70.6 | 72.2 |
| 10K | kntrie | 1.45 | 0.35 | 0.86 | 180.4 | 18.5 |
| | std::map | 2.68 | 1.03 | 1.41 | 396.0 | 40.6 |
| | unordered_map | 0.30 | 0.15 | 0.26 | 705.3 | 72.2 |
| 100K | kntrie | 21.00 | 5.58 | 13.34 | 1761.0 | 18.0 |
| | std::map | 43.64 | 34.85 | 35.90 | 4356.0 | 44.6 |
| | unordered_map | 6.42 | 2.65 | 6.43 | 7092.9 | 72.6 |
| 1M | kntrie | 1507.13 | 110.92 | 1463.41 | 17138.0 | 17.5 |
| | std::map | 1715.23 | 841.82 | 768.96 | 49104.0 | 50.3 |
| | unordered_map | 108.25 | 36.80 | 118.97 | 70752.5 | 72.5 |

## Sequential Pattern

| N | | Insert(ms) | Read(ms) | Erase(ms) | Memory(KB) | Bytes/entry |
|---|-|------------|----------|-----------|------------|-------------|
| 1K | kntrie | 0.18 | 0.05 | 0.13 | 22.0 | 22.5 |
| | std::map | 0.11 | 0.05 | 0.11 | 70.3 | 72.0 |
| | unordered_map | 0.02 | 0.00 | 0.01 | 70.6 | 72.2 |
| 10K | kntrie | 3.86 | 0.11 | 0.67 | 82.2 | 8.4 |
| | std::map | 2.45 | 1.02 | 1.41 | 396.0 | 40.6 |
| | unordered_map | 0.16 | 0.03 | 0.15 | 705.3 | 72.2 |
| 100K | kntrie | 19.57 | 2.59 | 6.69 | 802.5 | 8.2 |
| | std::map | 53.91 | 32.86 | 29.09 | 5016.0 | 51.4 |
| | unordered_map | 2.29 | 1.17 | 4.28 | 7092.9 | 72.6 |
| 1M | kntrie | 268.35 | 57.30 | 120.57 | 8005.6 | 8.2 |
| | std::map | 1124.67 | 767.05 | 738.16 | 52272.0 | 53.5 |
| | unordered_map | 36.85 | 15.63 | 88.31 | 70752.5 | 72.5 |

## Dense16 Pattern

| N | | Insert(ms) | Read(ms) | Erase(ms) | Memory(KB) | Bytes/entry |
|---|-|------------|----------|-----------|------------|-------------|
| 787 | kntrie | 0.13 | 0.04 | 0.10 | 16.0 | 20.8 |
| | std::map | 0.09 | 0.04 | 0.08 | 55.3 | 72.0 |
| | unordered_map | 0.02 | 0.01 | 0.02 | 55.6 | 72.4 |
| 7.9K | kntrie | 2.55 | 0.08 | 0.34 | 68.1 | 8.8 |
| | std::map | 2.62 | 0.79 | 1.11 | 396.0 | 51.0 |
| | unordered_map | 0.21 | 0.09 | 0.20 | 560.0 | 72.2 |
| 78.7K | kntrie | 11.75 | 1.90 | 4.61 | 654.9 | 8.5 |
| | std::map | 39.60 | 23.51 | 23.59 | 4224.0 | 55.0 |
| | unordered_map | 3.45 | 1.54 | 3.87 | 5531.3 | 72.0 |
| 787K | kntrie | 158.78 | 44.71 | 84.45 | 6533.0 | 8.5 |
| | std::map | 1089.17 | 572.57 | 545.16 | 41580.0 | 54.1 |
| | unordered_map | 56.53 | 21.47 | 88.17 | 55709.8 | 72.5 |

## Summary: kntrie vs std::map

| Metric | Random | Sequential | Dense16 |
|--------|--------|------------|---------|
| Memory | 2.5–3.4x better | 3.2–8.6x better | 3.5–8.2x better |
| Read | 2.9–7.6x faster | 1.1–13.4x faster | 1.1–12.8x faster |
| Erase | 1.6–2.7x faster (10K–100K), 0.5x at 1M | 1.2–6.1x faster | 0.8–6.5x faster |
| Insert | 1.1–1.9x faster (10K+), slower at 1K | 0.6–4.2x (varies) | 1.0–6.9x faster |

## Summary: kntrie vs std::unordered_map

| Metric | Random | Sequential | Dense16 |
|--------|--------|------------|---------|
| Memory | 3.4–4.1x better | 3.2–8.8x better | 3.5–8.5x better |
| Read | 2.3–3.0x slower | 3.7–7.1x slower (small), 1.1x (dense) | 1.1–4.2x slower |
| Erase | 2.5x slower (1K) to 12.3x slower (1M) | 4.5–13x slower | 1.0–5x slower |

## Notes

- Embedding infrastructure fully removed — zero benefit for u64 keys at practical sizes.
- Memory: 8.2–22.5 bytes/entry vs 40–72 for std containers.
- Read performance dominates std::map by 3–13x at scale; gap widens with N.
- kntrie 1M random erase (1463ms) is the known compact→split transition outlier.
- Sequential/dense16 erase improved significantly vs prior version (e.g. 100K seq: 16.67→6.69ms).
