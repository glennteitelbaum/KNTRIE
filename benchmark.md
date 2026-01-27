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
| Find | 4.54 ms | 21.70 ms | 1.56 ms | 4.78x slower | 2.91x faster |
| Iterate | 1.95 ms | 3.62 ms | 1.69 ms | 1.86x slower | 1.15x faster |
| Insert | 11.44 ms | 17.14 ms | 11.10 ms | 1.50x slower | 1.03x faster |
| Erase | 12.41 ms | 23.38 ms | 4.02 ms | 1.88x slower | 3.09x faster |
| Memory | 1.59 MB | 3.81 MB | 3.27 MB | 2.40x larger | 2.06x larger |

#### Sequential Pattern

| Metric | kntrie | std::map | std::unordered_map | kntrie vs map | kntrie vs umap |
|--------|--------|----------|-------------------|---------------|----------------|
| Find | 0.98 ms | 7.54 ms | 0.35 ms | 7.72x slower | 2.77x faster |
| Iterate | 2.96 ms | 0.69 ms | 0.25 ms | 4.30x faster | 11.81x faster |
| Insert | 7.26 ms | 8.36 ms | 2.92 ms | 1.15x slower | 2.49x faster |
| Erase | 7.00 ms | 4.06 ms | 1.58 ms | 1.72x faster | 4.43x faster |
| Memory | 1.91 MB | 3.81 MB | 3.27 MB | 1.99x larger | 1.71x larger |

### int32_t key, Value256 (32 bytes) value

#### Random Pattern

| Metric | kntrie | std::map | std::unordered_map | kntrie vs map | kntrie vs umap |
|--------|--------|----------|-------------------|---------------|----------------|
| Find | 4.96 ms | 28.23 ms | 1.75 ms | 5.69x slower | 2.83x faster |
| Iterate | 2.00 ms | 4.95 ms | 2.62 ms | 2.47x slower | 1.31x slower |
| Insert | 16.66 ms | 19.97 ms | 10.17 ms | 1.20x slower | 1.64x faster |
| Erase | 13.98 ms | 27.53 ms | 4.24 ms | 1.97x slower | 3.30x faster |
| Memory | 4.64 MB | 6.87 MB | 5.90 MB | 1.48x larger | 1.27x larger |

#### Sequential Pattern

| Metric | kntrie | std::map | std::unordered_map | kntrie vs map | kntrie vs umap |
|--------|--------|----------|-------------------|---------------|----------------|
| Find | 1.02 ms | 8.41 ms | 0.36 ms | 8.28x slower | 2.82x faster |
| Iterate | 3.04 ms | 1.95 ms | 0.44 ms | 1.56x faster | 6.84x faster |
| Insert | 8.53 ms | 8.47 ms | 2.74 ms | 1.01x faster | 3.11x faster |
| Erase | 7.28 ms | 4.13 ms | 1.44 ms | 1.76x faster | 5.05x faster |
| Memory | 4.97 MB | 6.87 MB | 5.90 MB | 1.38x larger | 1.19x larger |

## uint64_t key

### uint64_t key, int value

#### Random Pattern

| Metric | kntrie | std::map | std::unordered_map | kntrie vs map | kntrie vs umap |
|--------|--------|----------|-------------------|---------------|----------------|
| Find | 4.45 ms | 24.05 ms | 1.55 ms | 5.41x slower | 2.87x faster |
| Iterate | 1.82 ms | 4.14 ms | 1.80 ms | 2.28x slower | 1.01x faster |
| Insert | 10.69 ms | 18.67 ms | 7.85 ms | 1.75x slower | 1.36x faster |
| Erase | 12.10 ms | 24.97 ms | 3.90 ms | 2.06x slower | 3.10x faster |
| Memory | 1.59 MB | 4.58 MB | 3.92 MB | 2.88x larger | 2.47x larger |

#### Sequential Pattern

| Metric | kntrie | std::map | std::unordered_map | kntrie vs map | kntrie vs umap |
|--------|--------|----------|-------------------|---------------|----------------|
| Find | 2.78 ms | 7.18 ms | 0.35 ms | 2.59x slower | 8.04x faster |
| Iterate | 5.54 ms | 0.55 ms | 0.22 ms | 10.10x faster | 24.81x faster |
| Insert | 8.75 ms | 7.01 ms | 2.59 ms | 1.25x faster | 3.38x faster |
| Erase | 8.33 ms | 3.83 ms | 1.33 ms | 2.18x faster | 6.26x faster |
| Memory | 1.62 MB | 4.58 MB | 3.92 MB | 2.82x larger | 2.42x larger |

### uint64_t key, Value256 (32 bytes) value

#### Random Pattern

| Metric | kntrie | std::map | std::unordered_map | kntrie vs map | kntrie vs umap |
|--------|--------|----------|-------------------|---------------|----------------|
| Find | 4.78 ms | 25.76 ms | 1.76 ms | 5.39x slower | 2.71x faster |
| Iterate | 1.87 ms | 4.58 ms | 2.84 ms | 2.46x slower | 1.52x slower |
| Insert | 13.94 ms | 19.18 ms | 9.95 ms | 1.38x slower | 1.40x faster |
| Erase | 16.83 ms | 25.90 ms | 4.52 ms | 1.54x slower | 3.72x faster |
| Memory | 4.64 MB | 6.87 MB | 5.90 MB | 1.48x larger | 1.27x larger |

#### Sequential Pattern

| Metric | kntrie | std::map | std::unordered_map | kntrie vs map | kntrie vs umap |
|--------|--------|----------|-------------------|---------------|----------------|
| Find | 2.80 ms | 6.07 ms | 0.36 ms | 2.17x slower | 7.74x faster |
| Iterate | 5.58 ms | 0.64 ms | 0.44 ms | 8.76x faster | 12.68x faster |
| Insert | 10.01 ms | 6.84 ms | 2.96 ms | 1.46x faster | 3.38x faster |
| Erase | 10.31 ms | 3.87 ms | 1.35 ms | 2.66x faster | 7.63x faster |
| Memory | 4.67 MB | 6.87 MB | 5.90 MB | 1.47x larger | 1.26x larger |

