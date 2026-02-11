# kntrie Performance Concepts

## Benchmark Summary vs std::map

All ratios are conservative (rounded down). Range spans random and sequential workloads.

### uint64_t

| N | Find | Insert | Erase | B/entry |
|---|------|--------|-------|---------|
| 1K | 1.5x–3x | SAME–1.25x | 1.75x–2x | 3x |
| 10K | 1.5x–2x | SAME–1.5x | 2x | 4x–7x |
| 100K | 2x–6x | 2x–3x | 4x | 4x–7x |

### int32_t

| N | Find | Insert | Erase | B/entry |
|---|------|--------|-------|---------|
| 1K | 3x–8x | SAME | 1.75x | 4x–5x |
| 10K | 3x–10x | 1.5x | 2x–3x | 5x–7x |
| 100K | 7x–25x | 2x–4x | 5x–6x | 5x–7x |

## The Real Complexity: O(N) × O(M)

Textbook complexity treats memory access as uniform cost. In practice, every pointer chase or array lookup pays a cost determined by where the data lives in the memory hierarchy — L1, L2, L3, or DRAM. The cost of a cache miss depends on the total memory footprint M, because M determines which cache level the working set occupies.

This makes the true cost of N lookups closer to O(N × miss_cost(M)) for all three structures. The table below shows how per-lookup cost scales when N (and M) grow 10× from 10K to 100K entries:

### Cost-per-lookup growth (10K → 100K, ~10× memory growth)

| | map (per hop, logN removed) | umap | kntrie |
|---|---|---|---|
| Theoretical | 1.0× | 1.0× | 1.0× |
| Measured | 2.2–2.7× | 2.4–3.7× | 1.1–1.8× |

Map's O(log N) is well known, but after factoring out the log N tree depth, each individual hop still gets 2–3× more expensive as N grows — pure memory hierarchy effect. The unordered_map, supposedly O(1), degrades just as badly: its hash-then-chase-pointer pattern scatters across the same oversized heap.

The kntrie's advantage is twofold. First, 4–7× smaller memory footprint means the working set stays in faster cache levels longer — at 100K int32_t entries, the trie fits in ~1MB (L2/L3 boundary) while map and umap sit at 7MB (deep L3 or DRAM). Second, the trie's dense node layout gives spatial locality — adjacent keys share cache lines, so one fetch services multiple lookups. The sequential patterns barely degrade at all (1.1–1.2×) because the trie naturally groups nearby keys into the same nodes.

In short: all three data structures pay an O(M) tax on every operation. The kntrie just has a much smaller M.
