# kntrie Benchmark Results

**N = 100000 elements**

Each test runs 3 times, best time reported.

Comparison columns show how std::map/unordered_map compare to kntrie:
- "2x slower" means std::map took 2x longer than kntrie
- "2x faster" means std::map was 2x faster than kntrie
- "2x larger" means std::map used 2x more memory than kntrie

## int32_t key

### int32_t key, int value

#### Random Pattern

| Metric | kntrie | std::map | std::unordered_map | kntrie vs map | kntrie vs umap |
|--------|--------|----------|-------------------|---------------|----------------|
| Find | 4.45 ms | 27.55 ms | 1.90 ms | 6.19x slower | 2.34x faster |
| Iterate | 1.91 ms | 6.08 ms | 2.77 ms | 3.19x slower | 1.45x slower |
| Insert | 10.31 ms | 18.59 ms | 12.97 ms | 1.80x slower | 1.26x slower |
| Erase | 11.28 ms | 24.69 ms | 5.16 ms | 2.19x slower | 2.19x faster |
| Memory | 1.59 MB | 3.81 MB | 3.27 MB | 2.40x larger | 2.06x larger |

#### Sequential Pattern

| Metric | kntrie | std::map | std::unordered_map | kntrie vs map | kntrie vs umap |
|--------|--------|----------|-------------------|---------------|----------------|
| Find | 0.87 ms | 6.19 ms | 0.31 ms | 7.16x slower | 2.80x faster |
| Iterate | 2.64 ms | 0.83 ms | 0.32 ms | 3.17x faster | 8.37x faster |
| Insert | 6.80 ms | 7.82 ms | 2.44 ms | 1.15x slower | 2.78x faster |
| Erase | 6.45 ms | 3.67 ms | 1.30 ms | 1.76x faster | 4.94x faster |
| Memory | 1.91 MB | 3.81 MB | 3.27 MB | 1.99x larger | 1.71x larger |

### int32_t key, Value256 (32 bytes) value

#### Random Pattern

| Metric | kntrie | std::map | std::unordered_map | kntrie vs map | kntrie vs umap |
|--------|--------|----------|-------------------|---------------|----------------|
| Find | 4.66 ms | 33.09 ms | 2.04 ms | 7.11x slower | 2.28x faster |
| Iterate | 1.74 ms | 6.93 ms | 4.51 ms | 3.98x slower | 2.59x slower |
| Insert | 16.32 ms | 25.89 ms | 13.63 ms | 1.59x slower | 1.20x faster |
| Erase | 13.58 ms | 29.31 ms | 6.01 ms | 2.16x slower | 2.26x faster |
| Memory | 4.64 MB | 6.87 MB | 5.90 MB | 1.48x larger | 1.27x larger |

#### Sequential Pattern

| Metric | kntrie | std::map | std::unordered_map | kntrie vs map | kntrie vs umap |
|--------|--------|----------|-------------------|---------------|----------------|
| Find | 0.94 ms | 7.26 ms | 0.35 ms | 7.70x slower | 2.66x faster |
| Iterate | 2.78 ms | 2.58 ms | 0.55 ms | 1.08x faster | 5.07x faster |
| Insert | 7.41 ms | 9.21 ms | 3.16 ms | 1.24x slower | 2.35x faster |
| Erase | 6.47 ms | 4.12 ms | 1.34 ms | 1.57x faster | 4.83x faster |
| Memory | 4.97 MB | 6.87 MB | 5.90 MB | 1.38x larger | 1.19x larger |

## uint64_t key

### uint64_t key, int value

#### Random Pattern

| Metric | kntrie | std::map | std::unordered_map | kntrie vs map | kntrie vs umap |
|--------|--------|----------|-------------------|---------------|----------------|
| Find | 3.96 ms | 28.41 ms | 1.73 ms | 7.17x slower | 2.29x faster |
| Iterate | 1.63 ms | 6.20 ms | 2.67 ms | 3.80x slower | 1.64x slower |
| Insert | 9.49 ms | 20.77 ms | 9.27 ms | 2.19x slower | 1.02x faster |
| Erase | 10.89 ms | 26.34 ms | 5.00 ms | 2.42x slower | 2.18x faster |
| Memory | 1.59 MB | 4.58 MB | 3.92 MB | 2.88x larger | 2.47x larger |

#### Sequential Pattern

| Metric | kntrie | std::map | std::unordered_map | kntrie vs map | kntrie vs umap |
|--------|--------|----------|-------------------|---------------|----------------|
| Find | 2.39 ms | 6.71 ms | 0.31 ms | 2.81x slower | 7.73x faster |
| Iterate | 4.88 ms | 0.67 ms | 0.28 ms | 7.25x faster | 17.36x faster |
| Insert | 7.81 ms | 6.95 ms | 2.23 ms | 1.12x faster | 3.50x faster |
| Erase | 7.19 ms | 3.55 ms | 1.20 ms | 2.02x faster | 5.97x faster |
| Memory | 1.62 MB | 4.58 MB | 3.92 MB | 2.82x larger | 2.42x larger |

### uint64_t key, Value256 (32 bytes) value

#### Random Pattern

| Metric | kntrie | std::map | std::unordered_map | kntrie vs map | kntrie vs umap |
|--------|--------|----------|-------------------|---------------|----------------|
| Find | 4.31 ms | 32.63 ms | 2.08 ms | 7.57x slower | 2.07x faster |
| Iterate | 1.65 ms | 6.96 ms | 4.52 ms | 4.23x slower | 2.75x slower |
| Insert | 12.65 ms | 21.89 ms | 13.96 ms | 1.73x slower | 1.10x slower |
| Erase | 16.88 ms | 28.70 ms | 6.71 ms | 1.70x slower | 2.52x faster |
| Memory | 4.64 MB | 6.87 MB | 5.90 MB | 1.48x larger | 1.27x larger |

#### Sequential Pattern

| Metric | kntrie | std::map | std::unordered_map | kntrie vs map | kntrie vs umap |
|--------|--------|----------|-------------------|---------------|----------------|
| Find | 2.49 ms | 5.91 ms | 0.35 ms | 2.37x slower | 7.20x faster |
| Iterate | 5.07 ms | 0.77 ms | 0.50 ms | 6.62x faster | 10.06x faster |
| Insert | 9.19 ms | 7.87 ms | 2.95 ms | 1.17x faster | 3.11x faster |
| Erase | 9.08 ms | 3.63 ms | 1.24 ms | 2.50x faster | 7.34x faster |
| Memory | 4.67 MB | 6.87 MB | 5.90 MB | 1.47x larger | 1.26x larger |

