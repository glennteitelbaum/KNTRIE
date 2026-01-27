#include "kntrie.hpp"
#include <map>
#include <unordered_map>
#include <chrono>
#include <random>
#include <vector>
#include <algorithm>
#include <iostream>
#include <fstream>
#include <iomanip>
#include <cstdint>
#include <cstring>
#include <array>

// 256-bit value type
struct Value256 {
    std::array<uint64_t, 4> data;
    
    Value256() : data{} {}
    Value256(uint64_t v) {
        data[0] = v;
        data[1] = v * 2;
        data[2] = v * 3;
        data[3] = v * 4;
    }
    
    bool operator==(const Value256& other) const {
        return data == other.data;
    }
};

// Timer utility
class Timer {
    using clock = std::chrono::high_resolution_clock;
    clock::time_point start_;
public:
    Timer() : start_(clock::now()) {}
    
    double elapsed_ms() const {
        auto end = clock::now();
        return std::chrono::duration<double, std::milli>(end - start_).count();
    }
};

// Memory tracking - global counters
namespace mem_track {
    inline size_t total_allocated = 0;
    inline size_t current_allocated = 0;
    inline size_t peak_allocated = 0;
    
    inline void reset() {
        total_allocated = 0;
        current_allocated = 0;
        peak_allocated = 0;
    }
}

// Memory tracking allocator
template<typename T>
class TrackingAllocator {
public:
    using value_type = T;
    
    TrackingAllocator() = default;
    
    template<typename U>
    TrackingAllocator(const TrackingAllocator<U>&) noexcept {}
    
    T* allocate(size_t n) {
        size_t bytes = n * sizeof(T);
        mem_track::total_allocated += bytes;
        mem_track::current_allocated += bytes;
        if (mem_track::current_allocated > mem_track::peak_allocated) {
            mem_track::peak_allocated = mem_track::current_allocated;
        }
        return static_cast<T*>(::operator new(bytes));
    }
    
    void deallocate(T* p, size_t n) noexcept {
        size_t bytes = n * sizeof(T);
        mem_track::current_allocated -= bytes;
        ::operator delete(p);
    }
    
    template<typename U>
    bool operator==(const TrackingAllocator<U>&) const { return true; }
};

// Benchmark results
struct BenchmarkResult {
    double insert_ms;
    double find_ms;
    double iterate_ms;
    double erase_ms;
    size_t memory_bytes;
};

// Run benchmark 3 times and return best
template<typename Container, typename Keys, typename MakeValue>
BenchmarkResult run_benchmark(const Keys& keys, MakeValue make_value) {
    BenchmarkResult best{1e9, 1e9, 1e9, 1e9, 0};
    
    for (int run = 0; run < 3; ++run) {
        mem_track::reset();
        
        Container container;
        
        // Insert
        Timer t_insert;
        for (const auto& k : keys) {
            container.insert({k, make_value(k)});
        }
        double insert_time = t_insert.elapsed_ms();
        
        size_t memory = mem_track::peak_allocated;
        
        // Find
        Timer t_find;
        volatile size_t found = 0;
        for (const auto& k : keys) {
            if (container.find(k) != container.end()) {
                ++found;
            }
        }
        double find_time = t_find.elapsed_ms();
        
        // Iterate
        Timer t_iterate;
        volatile size_t count = 0;
        for (auto it = container.begin(); it != container.end(); ++it) {
            ++count;
        }
        double iterate_time = t_iterate.elapsed_ms();
        
        // Erase
        Timer t_erase;
        for (const auto& k : keys) {
            container.erase(k);
        }
        double erase_time = t_erase.elapsed_ms();
        
        // Keep best times
        if (insert_time < best.insert_ms) best.insert_ms = insert_time;
        if (find_time < best.find_ms) best.find_ms = find_time;
        if (iterate_time < best.iterate_ms) best.iterate_ms = iterate_time;
        if (erase_time < best.erase_ms) best.erase_ms = erase_time;
        best.memory_bytes = memory; // Same across runs
    }
    
    return best;
}

// Generate keys
template<typename K>
std::vector<K> generate_random_keys(size_t n, uint64_t seed) {
    std::vector<K> keys;
    keys.reserve(n);
    
    std::mt19937_64 rng(seed);
    
    if constexpr (std::is_signed_v<K>) {
        std::uniform_int_distribution<int64_t> dist(
            std::numeric_limits<K>::min(),
            std::numeric_limits<K>::max()
        );
        for (size_t i = 0; i < n; ++i) {
            keys.push_back(static_cast<K>(dist(rng)));
        }
    } else {
        std::uniform_int_distribution<uint64_t> dist(
            std::numeric_limits<K>::min(),
            std::numeric_limits<K>::max()
        );
        for (size_t i = 0; i < n; ++i) {
            keys.push_back(static_cast<K>(dist(rng)));
        }
    }
    
    return keys;
}

template<typename K>
std::vector<K> generate_sequential_keys(size_t n) {
    std::vector<K> keys;
    keys.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        keys.push_back(static_cast<K>(i));
    }
    return keys;
}

// Format bytes
std::string format_bytes(size_t bytes) {
    std::ostringstream oss;
    if (bytes >= 1024 * 1024) {
        oss << std::fixed << std::setprecision(2) << (bytes / (1024.0 * 1024.0)) << " MB";
    } else if (bytes >= 1024) {
        oss << std::fixed << std::setprecision(2) << (bytes / 1024.0) << " KB";
    } else {
        oss << bytes << " B";
    }
    return oss.str();
}

// Format comparison
std::string format_comparison(double base, double compare) {
    double ratio = compare / base;
    std::ostringstream oss;
    if (ratio < 1.0) {
        oss << std::fixed << std::setprecision(2) << (1.0 / ratio) << "x faster";
    } else if (ratio > 1.0) {
        oss << std::fixed << std::setprecision(2) << ratio << "x slower";
    } else {
        oss << "same";
    }
    return oss.str();
}

std::string format_mem_comparison(size_t base, size_t compare) {
    double ratio = static_cast<double>(compare) / base;
    std::ostringstream oss;
    if (ratio < 1.0) {
        oss << std::fixed << std::setprecision(2) << (1.0 / ratio) << "x smaller";
    } else if (ratio > 1.0) {
        oss << std::fixed << std::setprecision(2) << ratio << "x larger";
    } else {
        oss << "same";
    }
    return oss.str();
}

template<typename K, typename V>
void run_all_benchmarks(std::ostream& out, const std::string& key_name, 
                        const std::string& value_name, size_t n,
                        const std::vector<K>& random_keys,
                        const std::vector<K>& seq_keys) {
    
    using KnTrieAlloc = TrackingAllocator<uint64_t>;
    using MapAlloc = TrackingAllocator<std::pair<const K, V>>;
    using UMapAlloc = TrackingAllocator<std::pair<const K, V>>;
    
    using KnTrie = kn::kntrie<K, V, KnTrieAlloc>;
    using StdMap = std::map<K, V, std::less<K>, MapAlloc>;
    using StdUMap = std::unordered_map<K, V, std::hash<K>, std::equal_to<K>, UMapAlloc>;
    
    auto make_value = [](K k) -> V {
        if constexpr (std::is_same_v<V, int>) {
            return static_cast<int>(k);
        } else {
            return V(static_cast<uint64_t>(k));
        }
    };
    
    out << "### " << key_name << " key, " << value_name << " value\n\n";
    
    // Random pattern
    out << "#### Random Pattern\n\n";
    out << "| Metric | kntrie | std::map | std::unordered_map | kntrie vs map | kntrie vs umap |\n";
    out << "|--------|--------|----------|-------------------|---------------|----------------|\n";
    
    auto kn_random = run_benchmark<KnTrie>(random_keys, make_value);
    auto map_random = run_benchmark<StdMap>(random_keys, make_value);
    auto umap_random = run_benchmark<StdUMap>(random_keys, make_value);
    
    out << "| Find | " << std::fixed << std::setprecision(2) << kn_random.find_ms << " ms | "
        << map_random.find_ms << " ms | " << umap_random.find_ms << " ms | "
        << format_comparison(kn_random.find_ms, map_random.find_ms) << " | "
        << format_comparison(kn_random.find_ms, umap_random.find_ms) << " |\n";
    
    out << "| Iterate | " << kn_random.iterate_ms << " ms | "
        << map_random.iterate_ms << " ms | " << umap_random.iterate_ms << " ms | "
        << format_comparison(kn_random.iterate_ms, map_random.iterate_ms) << " | "
        << format_comparison(kn_random.iterate_ms, umap_random.iterate_ms) << " |\n";
    
    out << "| Insert | " << kn_random.insert_ms << " ms | "
        << map_random.insert_ms << " ms | " << umap_random.insert_ms << " ms | "
        << format_comparison(kn_random.insert_ms, map_random.insert_ms) << " | "
        << format_comparison(kn_random.insert_ms, umap_random.insert_ms) << " |\n";
    
    out << "| Erase | " << kn_random.erase_ms << " ms | "
        << map_random.erase_ms << " ms | " << umap_random.erase_ms << " ms | "
        << format_comparison(kn_random.erase_ms, map_random.erase_ms) << " | "
        << format_comparison(kn_random.erase_ms, umap_random.erase_ms) << " |\n";
    
    out << "| Memory | " << format_bytes(kn_random.memory_bytes) << " | "
        << format_bytes(map_random.memory_bytes) << " | " << format_bytes(umap_random.memory_bytes) << " | "
        << format_mem_comparison(kn_random.memory_bytes, map_random.memory_bytes) << " | "
        << format_mem_comparison(kn_random.memory_bytes, umap_random.memory_bytes) << " |\n";
    
    out << "\n";
    
    // Sequential pattern
    out << "#### Sequential Pattern\n\n";
    out << "| Metric | kntrie | std::map | std::unordered_map | kntrie vs map | kntrie vs umap |\n";
    out << "|--------|--------|----------|-------------------|---------------|----------------|\n";
    
    auto kn_seq = run_benchmark<KnTrie>(seq_keys, make_value);
    auto map_seq = run_benchmark<StdMap>(seq_keys, make_value);
    auto umap_seq = run_benchmark<StdUMap>(seq_keys, make_value);
    
    out << "| Find | " << std::fixed << std::setprecision(2) << kn_seq.find_ms << " ms | "
        << map_seq.find_ms << " ms | " << umap_seq.find_ms << " ms | "
        << format_comparison(kn_seq.find_ms, map_seq.find_ms) << " | "
        << format_comparison(kn_seq.find_ms, umap_seq.find_ms) << " |\n";
    
    out << "| Iterate | " << kn_seq.iterate_ms << " ms | "
        << map_seq.iterate_ms << " ms | " << umap_seq.iterate_ms << " ms | "
        << format_comparison(kn_seq.iterate_ms, map_seq.iterate_ms) << " | "
        << format_comparison(kn_seq.iterate_ms, umap_seq.iterate_ms) << " |\n";
    
    out << "| Insert | " << kn_seq.insert_ms << " ms | "
        << map_seq.insert_ms << " ms | " << umap_seq.insert_ms << " ms | "
        << format_comparison(kn_seq.insert_ms, map_seq.insert_ms) << " | "
        << format_comparison(kn_seq.insert_ms, umap_seq.insert_ms) << " |\n";
    
    out << "| Erase | " << kn_seq.erase_ms << " ms | "
        << map_seq.erase_ms << " ms | " << umap_seq.erase_ms << " ms | "
        << format_comparison(kn_seq.erase_ms, map_seq.erase_ms) << " | "
        << format_comparison(kn_seq.erase_ms, umap_seq.erase_ms) << " |\n";
    
    out << "| Memory | " << format_bytes(kn_seq.memory_bytes) << " | "
        << format_bytes(map_seq.memory_bytes) << " | " << format_bytes(umap_seq.memory_bytes) << " | "
        << format_mem_comparison(kn_seq.memory_bytes, map_seq.memory_bytes) << " | "
        << format_mem_comparison(kn_seq.memory_bytes, umap_seq.memory_bytes) << " |\n";
    
    out << "\n";
}

int main(int argc, char* argv[]) {
    if (argc != 2) {
        std::cerr << "Usage: " << argv[0] << " <N>\n";
        return 1;
    }
    
    size_t n = std::stoull(argv[1]);
    
    std::cout << "Running benchmarks with N=" << n << "...\n";
    
    // Generate keys
    auto random_keys_32 = generate_random_keys<int32_t>(n, 12345);
    auto seq_keys_32 = generate_sequential_keys<int32_t>(n);
    
    std::ofstream out("benchmark.md");
    
    out << "# kntrie Benchmark Results\n\n";
    out << "**N = " << n << " elements**\n\n";
    out << "Each test runs 3 times, best time reported.\n\n";
    out << "Comparison columns show how std::map/unordered_map compare to kntrie:\n";
    out << "- \"2x slower\" means std::map took 2x longer than kntrie\n";
    out << "- \"2x faster\" means std::map was 2x faster than kntrie\n";
    out << "- \"2x larger\" means std::map used 2x more memory than kntrie\n\n";
    
    out << "## int32_t key\n\n";
    
    std::cout << "  Running int32_t/int benchmarks...\n";
    run_all_benchmarks<int32_t, int>(out, "int32_t", "int", n, random_keys_32, seq_keys_32);
    
    std::cout << "  Running int32_t/Value256 benchmarks...\n";
    run_all_benchmarks<int32_t, Value256>(out, "int32_t", "Value256 (32 bytes)", n, random_keys_32, seq_keys_32);
    
    out << "## uint64_t key\n\n";
    
    auto random_keys_64 = generate_random_keys<uint64_t>(n, 54321);
    auto seq_keys_64 = generate_sequential_keys<uint64_t>(n);
    
    std::cout << "  Running uint64_t/int benchmarks...\n";
    run_all_benchmarks<uint64_t, int>(out, "uint64_t", "int", n, random_keys_64, seq_keys_64);
    
    std::cout << "  Running uint64_t/Value256 benchmarks...\n";
    run_all_benchmarks<uint64_t, Value256>(out, "uint64_t", "Value256 (32 bytes)", n, random_keys_64, seq_keys_64);
    
    out.close();
    
    std::cout << "Results written to benchmark.md\n";
    
    return 0;
}
