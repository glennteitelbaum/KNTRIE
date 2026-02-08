# kntrie Benchmark Results

Compiled with `g++ -std=c++23 -O2 -march=x86-64-v3`. Keys shuffled before insert. Lookup rounds pre-generated: each round is an independent shuffle of all keys. All three containers read the same rounds in the same order. Shuffle cost is excluded from read timing.

---

## Random — full 64-bit range, uniform distribution

| N | Container | Insert(ms) | Read(ms) | ns/read | Bytes/entry |
|--:|-----------|----------:|----------:|--------:|------------:|
| 1K | **kntrie** | 0.31 | 0.04 | 42.3 | **16.5** |
| | std::map | 0.13 | 0.06 | 58.3 | 72.0 |
| | unordered_map | 0.03 | 0.01 | 12.2 | 72.2 |
| 10K | **kntrie** | 10.20 | 0.40 | 40.5 | **17.0** |
| | std::map | 2.32 | 1.04 | 104.0 | 54.1 |
| | unordered_map | 0.30 | 0.15 | 14.9 | 72.2 |
| 100K | **kntrie** | 40.15 | 6.00 | 60.0 | **16.6** |
| | std::map | 52.51 | 32.92 | 329.2 | 59.5 |
| | unordered_map | 5.23 | 2.56 | 25.6 | 72.6 |
| 1M | **kntrie** | 2,718 | 118.91 | 118.9 | **16.5** |
| | std::map | 1,030 | 862.54 | 862.5 | 63.6 |
| | unordered_map | 93.54 | 35.71 | 35.7 | 72.5 |

---

## Sequential — keys 0, 1, 2, ... N (best case for skip compression)

| N | Container | Insert(ms) | Read(ms) | ns/read | Bytes/entry |
|--:|-----------|----------:|----------:|--------:|------------:|
| 1K | **kntrie** | 0.31 | 0.04 | 41.6 | **16.5** |
| | std::map | 0.12 | 0.06 | 57.3 | 72.0 |
| | unordered_map | 0.02 | 0.00 | 3.5 | 72.2 |
| 10K | **kntrie** | 10.12 | 0.09 | 9.1 | **8.2** |
| | std::map | 2.13 | 1.02 | 101.8 | 54.1 |
| | unordered_map | 0.15 | 0.03 | 3.3 | 72.2 |
| 100K | **kntrie** | 28.46 | 2.31 | 23.1 | **8.2** |
| | std::map | 40.52 | 29.64 | 296.4 | 58.1 |
| | unordered_map | 2.23 | 1.15 | 11.5 | 72.6 |
| 1M | **kntrie** | 315.40 | 43.86 | 43.9 | **8.2** |
| | std::map | 1,093 | 962.86 | 962.9 | 59.6 |
| | unordered_map | 37.17 | 18.00 | 18.0 | 72.5 |

---

## Dense16 — clustered in narrow range (`0x123400000000 + rng() % 2N`)

Note: dedup reduces N (e.g. 1M → 787K, 10K → 7945).

| N (actual) | Container | Insert(ms) | Read(ms) | ns/read | Bytes/entry |
|-----------:|-----------|----------:|----------:|--------:|------------:|
| 787 | **kntrie** | 0.45 | 0.03 | 40.4 | **16.6** |
| | std::map | 0.10 | 0.04 | 52.9 | 72.0 |
| | unordered_map | 0.02 | 0.01 | 9.3 | 72.4 |
| 7,945 | **kntrie** | 9.44 | 0.08 | 9.5 | **8.4** |
| | std::map | 1.92 | 0.78 | 97.6 | 51.0 |
| | unordered_map | 0.19 | 0.09 | 11.5 | 72.2 |
| 78,653 | **kntrie** | 28.18 | 1.91 | 24.3 | **8.4** |
| | std::map | 32.16 | 22.91 | 291.3 | 53.3 |
| | unordered_map | 2.84 | 1.41 | 17.9 | 72.0 |
| 787,084 | **kntrie** | 293.08 | 36.10 | 45.9 | **8.4** |
| | std::map | 655.11 | 628.69 | 798.8 | 54.3 |
| | unordered_map | 49.41 | 20.43 | 26.0 | 72.5 |

---

## Key Observations

### Memory compression

| Pattern | Bytes/entry | vs std::map | vs unordered_map |
|---------|------------:|------------:|-----------------:|
| Random | 16.5 | **3.6–4.4×** smaller | **4.4×** smaller |
| Sequential | 8.2 | **6.6–8.8×** smaller | **8.8×** smaller |
| Dense16 | 8.4 | **6.1–8.6×** smaller | **8.6×** smaller |

Skip compression halves memory when keys share common prefixes (sequential, dense16).

### Read performance

- **vs std::map:** 1.4–22× faster across all patterns and sizes. Gap widens dramatically at scale.
- **vs unordered_map:** 2.7–3.3× slower on random. Faster at 8K dense16 (9.5 vs 11.5 ns). At 1M sequential, 2.4× slower (43.9 vs 18.0 ns).
- Sequential/dense16 reads are ~2.6× faster than random at same scale — skip compression reduces nodes traversed.

### Insert performance

Insert is the weak point due to alloc/dealloc on every mutation. Sequential at 1M is 8.6× faster than random (315ms vs 2,718ms) because prefix compression reduces tree depth and node count.

### Pattern impact

Sequential and dense16 are the sweet spots: common prefixes trigger skip compression, cutting memory to ~8 bytes/entry and roughly halving read latency compared to random data.
