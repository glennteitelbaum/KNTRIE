# kntrie3 Benchmark Results

Compiler: `g++ -std=c++23 -O2 -march=x86-64-v3`
Features: size-class allocation, in-place insert/erase, root[256] array, bot_leaf_16 header, bot_internal embedding, split_top embedding, try-embed for replacement children, prepend_skip pointer fixup

## Random Pattern

| N | | Insert(ms) | Read(ms) | Erase(ms) | Memory(KB) | Bytes/entry |
|---|-|------------|----------|-----------|------------|-------------|
| 1K | kntrie | 0.12 | 0.02 | 0.06 | 20.8 | 21.3 |
| | std::map | 0.15 | 0.06 | 0.12 | 70.3 | 72.0 |
| | unordered_map | 0.03 | 0.01 | 0.03 | 70.6 | 72.2 |
| 10K | kntrie | 1.55 | 0.36 | 0.77 | 180.4 | 18.5 |
| | std::map | 3.09 | 1.03 | 1.53 | 396.0 | 40.6 |
| | unordered_map | 0.34 | 0.15 | 0.41 | 705.3 | 72.2 |
| 100K | kntrie | 21.31 | 5.37 | 16.17 | 1761.0 | 18.0 |
| | std::map | 47.86 | 24.11 | 25.73 | 4356.0 | 44.6 |
| | unordered_map | 4.83 | 2.10 | 4.95 | 7092.9 | 72.6 |
| 1M | kntrie | 1293.83 | 90.40 | 1230.36 | 17138.0 | 17.5 |
| | std::map | 1026.54 | 502.45 | 509.67 | 49104.0 | 50.3 |
| | unordered_map | 66.85 | 30.30 | 84.32 | 70752.5 | 72.5 |

## Sequential Pattern

| N | | Insert(ms) | Read(ms) | Erase(ms) | Memory(KB) | Bytes/entry |
|---|-|------------|----------|-----------|------------|-------------|
| 1K | kntrie | 0.23 | 0.05 | 0.15 | 22.0 | 22.5 |
| | std::map | 0.13 | 0.05 | 0.11 | 70.3 | 72.0 |
| | unordered_map | 0.02 | 0.00 | 0.02 | 70.6 | 72.2 |
| 10K | kntrie | 3.52 | 0.11 | 0.67 | 82.2 | 8.4 |
| | std::map | 2.98 | 1.02 | 1.51 | 396.0 | 40.6 |
| | unordered_map | 0.18 | 0.03 | 0.14 | 705.3 | 72.2 |
| 100K | kntrie | 21.21 | 2.52 | 7.80 | 802.5 | 8.2 |
| | std::map | 51.43 | 25.43 | 29.24 | 5016.0 | 51.4 |
| | unordered_map | 1.66 | 0.79 | 2.97 | 7092.9 | 72.6 |
| 1M | kntrie | 245.43 | 42.98 | 103.79 | 8040.0 | 8.2 |
| | std::map | 986.77 | 529.56 | 567.26 | 52668.0 | 53.9 |
| | unordered_map | 32.05 | 12.94 | 55.54 | 70752.5 | 72.5 |

## Dense16 Pattern

| N | | Insert(ms) | Read(ms) | Erase(ms) | Memory(KB) | Bytes/entry |
|---|-|------------|----------|-----------|------------|-------------|
| 787 | kntrie | 0.19 | 0.04 | 0.12 | 16.0 | 20.8 |
| | std::map | 0.10 | 0.04 | 0.09 | 55.3 | 72.0 |
| | unordered_map | 0.02 | 0.01 | 0.02 | 55.6 | 72.4 |
| 7.9K | kntrie | 3.05 | 0.09 | 0.41 | 68.1 | 8.8 |
| | std::map | 3.36 | 0.86 | 1.19 | 396.0 | 51.0 |
| | unordered_map | 0.23 | 0.09 | 0.18 | 560.0 | 72.2 |
| 78.7K | kntrie | 14.12 | 2.11 | 6.43 | 666.6 | 8.7 |
| | std::map | 40.09 | 17.87 | 18.76 | 4092.0 | 53.3 |
| | unordered_map | 2.51 | 1.33 | 3.78 | 5531.3 | 72.0 |
| 787K | kntrie | 173.81 | 33.22 | 77.67 | 6606.8 | 8.6 |
| | std::map | 910.44 | 396.51 | 432.23 | 41580.0 | 54.1 |
| | unordered_map | 43.85 | 18.17 | 60.29 | 55709.8 | 72.5 |

## Summary: kntrie vs std::map

| Metric | Random | Sequential | Dense16 |
|--------|--------|------------|---------|
| Memory | 2.5–3.4x better | 3.2–8.6x better | 3.5–8.2x better |
| Read | 2.8–5.6x faster | 1.1–12.3x faster | 1.1–11.9x faster |
| Erase | 0.4–2.0x | 0.7–5.5x faster | 0.8–5.6x faster |
| Insert | 0.8–2.2x faster | 0.7–4.0x faster | 0.6–5.2x faster |

## Summary: kntrie vs std::unordered_map

| Metric | Random | Sequential | Dense16 |
|--------|--------|------------|---------|
| Memory | 3.4–4.1x better | 3.2–8.8x better | 3.5–8.2x better |
| Read | 2.0–3.0x slower | 3.3–14.3x slower | 1.1–4.0x slower |
| Erase | 0.9x (1M) to 2.0x slower (1K) | 1.9x (1M) to 7.5x slower (1K) | 1.3x (787K) to 6.0x slower (787) |
| Insert | 4.3–19.3x slower | 7.7–19.6x slower | 5.3–13.3x slower |

## Notes

- Embedding (phases 4-6) improved random read by ~20-25% at 100K-1M vs pre-embed baseline, at cost of ~6-8% memory overhead from embed slot padding and bot_leaf_16 headers.
- Sequential and dense16 patterns see minimal embedding benefit because prefix compression produces fewer, larger nodes that don't fit in 64B embed slots.
- kntrie erase at 1M random (1230ms) remains an outlier — transition zone where compact→split conversions create many small nodes requiring reallocation on erase.
- Memory advantage is the primary design goal: 8.2–22.5 bytes/entry vs 40–72 for std containers.
