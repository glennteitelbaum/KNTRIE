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
| Find | 4.20 ms | 24.53 ms | 1.87 ms | 5.84x slower | 2.25x faster |
| Iterate | 1.88 ms | 5.75 ms | 2.58 ms | 3.06x slower | 1.37x slower |
| Insert | 10.38 ms | 17.51 ms | 12.92 ms | 1.69x slower | 1.24x slower |
| Erase | 11.09 ms | 23.65 ms | 5.13 ms | 2.13x slower | 2.16x faster |
| Memory | 1.59 MB | 3.81 MB | 3.27 MB | 2.40x larger | 2.06x larger |

#### Sequential Pattern

| Metric | kntrie | std::map | std::unordered_map | kntrie vs map | kntrie vs umap |
|--------|--------|----------|-------------------|---------------|----------------|
| Find | 0.93 ms | 6.22 ms | 0.31 ms | 6.66x slower | 3.02x faster |
| Iterate | 2.68 ms | 0.93 ms | 0.29 ms | 2.89x faster | 9.13x faster |
| Insert | 6.98 ms | 8.08 ms | 2.38 ms | 1.16x slower | 2.93x faster |
| Erase | 7.59 ms | 3.68 ms | 1.12 ms | 2.06x faster | 6.81x faster |
| Memory | 1.91 MB | 3.81 MB | 3.27 MB | 1.99x larger | 1.71x larger |

### int32_t key, Value256 (32 bytes) value

#### Random Pattern

| Metric | kntrie | std::map | std::unordered_map | kntrie vs map | kntrie vs umap |
|--------|--------|----------|-------------------|---------------|----------------|
| Find | 4.72 ms | 34.74 ms | 2.02 ms | 7.37x slower | 2.34x faster |
| Iterate | 1.77 ms | 7.14 ms | 4.27 ms | 4.04x slower | 2.42x slower |
| Insert | 16.38 ms | 25.18 ms | 13.46 ms | 1.54x slower | 1.22x faster |
| Erase | 14.03 ms | 29.68 ms | 6.04 ms | 2.11x slower | 2.32x faster |
| Memory | 4.64 MB | 6.87 MB | 5.90 MB | 1.48x larger | 1.27x larger |

#### Sequential Pattern

| Metric | kntrie | std::map | std::unordered_map | kntrie vs map | kntrie vs umap |
|--------|--------|----------|-------------------|---------------|----------------|
| Find | 0.88 ms | 7.14 ms | 0.34 ms | 8.11x slower | 2.60x faster |
| Iterate | 2.73 ms | 2.37 ms | 0.59 ms | 1.15x faster | 4.66x faster |
| Insert | 7.49 ms | 8.24 ms | 2.77 ms | 1.10x slower | 2.70x faster |
| Erase | 6.30 ms | 4.00 ms | 1.37 ms | 1.58x faster | 4.60x faster |
| Memory | 4.97 MB | 6.87 MB | 5.90 MB | 1.38x larger | 1.19x larger |

## uint64_t key

### uint64_t key, int value

#### Random Pattern

| Metric | kntrie | std::map | std::unordered_map | kntrie vs map | kntrie vs umap |
|--------|--------|----------|-------------------|---------------|----------------|
| Find | 4.08 ms | 26.31 ms | 1.61 ms | 6.46x slower | 2.54x faster |
| Iterate | 1.64 ms | 5.63 ms | 2.16 ms | 3.44x slower | 1.32x slower |
| Insert | 9.67 ms | 19.40 ms | 8.53 ms | 2.01x slower | 1.13x faster |
| Erase | 11.02 ms | 24.30 ms | 4.39 ms | 2.20x slower | 2.51x faster |
| Memory | 1.59 MB | 4.58 MB | 3.92 MB | 2.88x larger | 2.47x larger |

#### Sequential Pattern

| Metric | kntrie | std::map | std::unordered_map | kntrie vs map | kntrie vs umap |
|--------|--------|----------|-------------------|---------------|----------------|
| Find | 2.44 ms | 6.35 ms | 0.31 ms | 2.60x slower | 7.99x faster |
| Iterate | 4.88 ms | 0.55 ms | 0.25 ms | 8.89x faster | 19.65x faster |
| Insert | 7.80 ms | 6.43 ms | 2.20 ms | 1.21x faster | 3.55x faster |
| Erase | 7.23 ms | 3.42 ms | 1.09 ms | 2.11x faster | 6.61x faster |
| Memory | 1.62 MB | 4.58 MB | 3.92 MB | 2.82x larger | 2.42x larger |

### uint64_t key, Value256 (32 bytes) value

#### Random Pattern

| Metric | kntrie | std::map | std::unordered_map | kntrie vs map | kntrie vs umap |
|--------|--------|----------|-------------------|---------------|----------------|
| Find | 4.24 ms | 28.01 ms | 1.98 ms | 6.61x slower | 2.14x faster |
| Iterate | 1.65 ms | 6.28 ms | 4.43 ms | 3.81x slower | 2.69x slower |
| Insert | 12.40 ms | 19.95 ms | 13.34 ms | 1.61x slower | 1.08x slower |
| Erase | 16.00 ms | 26.58 ms | 6.50 ms | 1.66x slower | 2.46x faster |
| Memory | 4.64 MB | 6.87 MB | 5.90 MB | 1.48x larger | 1.27x larger |

#### Sequential Pattern

| Metric | kntrie | std::map | std::unordered_map | kntrie vs map | kntrie vs umap |
|--------|--------|----------|-------------------|---------------|----------------|
| Find | 2.45 ms | 5.51 ms | 0.37 ms | 2.25x slower | 6.69x faster |
| Iterate | 5.07 ms | 0.81 ms | 0.51 ms | 6.23x faster | 9.91x faster |
| Insert | 8.90 ms | 7.60 ms | 2.87 ms | 1.17x faster | 3.10x faster |
| Erase | 9.03 ms | 3.56 ms | 1.09 ms | 2.53x faster | 8.28x faster |
| Memory | 4.67 MB | 6.87 MB | 5.90 MB | 1.47x larger | 1.26x larger |

