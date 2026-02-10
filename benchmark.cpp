#include "kntrie.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <map>
#include <unordered_map>
#include <vector>
#include <random>
#include <numeric>
#include <algorithm>
#include <string>

static double now_ms() {
    using clk = std::chrono::high_resolution_clock;
    static auto t0 = clk::now();
    return std::chrono::duration<double, std::milli>(clk::now() - t0).count();
}

template<typename T>
static void do_not_optimize(T const& val) {
    asm volatile("" : : "r,m"(val) : "memory");
}

static size_t rss_bytes() {
    FILE* f = std::fopen("/proc/self/statm", "r");
    if (!f) return 0;
    unsigned long pages = 0;
    if (std::fscanf(f, "%*lu %lu", &pages) != 1) pages = 0;
    std::fclose(f);
    return pages * 4096UL;
}

struct Result {
    const char* name;
    double insert_ms;
    double read_ms;
    double erase_ms;
    size_t memory_bytes;
};

static void print_header() {
    std::printf("%-20s %12s %12s %12s %12s %12s %12s %12s %12s\n",
                "Container", "Insert(ms)", "Read(ms)", "Erase(ms)", "Memory(KB)",
                "Ins rel", "Read rel", "Erase rel", "Mem rel");
    std::printf("%-20s %12s %12s %12s %12s %12s %12s %12s %12s\n",
                "--------------------", "----------", "----------", "----------",
                "----------", "----------", "----------", "----------", "----------");
}

static void print_row(const Result& r, const Result& base, size_t n) {
    double ins_rel   = r.insert_ms / base.insert_ms;
    double read_rel  = r.read_ms   / base.read_ms;
    double erase_rel = base.erase_ms > 0 ? r.erase_ms / base.erase_ms : 0;
    double mem_rel   = static_cast<double>(r.memory_bytes) / base.memory_bytes;
    std::printf("%-20s %12.2f %12.2f %12.2f %12.1f %11.2fx %11.2fx %11.2fx %11.2fx\n",
                r.name, r.insert_ms, r.read_ms, r.erase_ms,
                r.memory_bytes / 1024.0,
                ins_rel, read_rel, erase_rel, mem_rel);
}

using LookupRounds = std::vector<std::vector<uint64_t>>;

static Result bench_kntrie(const std::vector<uint64_t>& keys,
                            const LookupRounds& rounds) {
    Result res{"kntrie", 0, 0, 0, 0};
    gteitelbaum::kntrie<uint64_t, uint64_t> trie;

    double t0 = now_ms();
    for (auto k : keys) trie.insert(k, k);
    res.insert_ms = now_ms() - t0;

    if (trie.size() != keys.size())
        std::fprintf(stderr, "kntrie: size mismatch %zu vs %zu\n", trie.size(), keys.size());

    res.memory_bytes = trie.memory_usage();

    uint64_t checksum = 0;
    double t1 = now_ms();
    for (auto& lk : rounds) {
        for (auto k : lk) {
            auto* v = trie.find_value(k);
            checksum += v ? *v : 0;
        }
    }
    res.read_ms = (now_ms() - t1) / static_cast<int>(rounds.size());
    do_not_optimize(checksum);

    // Erase benchmark (shuffled order)
    double t2 = now_ms();
    for (auto k : rounds[0]) trie.erase(k);
    res.erase_ms = now_ms() - t2;
    if (!trie.empty())
        std::fprintf(stderr, "kntrie: not empty after erase, %zu remaining\n", trie.size());
    return res;
}

static Result bench_stdmap(const std::vector<uint64_t>& keys,
                           const LookupRounds& rounds) {
    Result res{"std::map", 0, 0, 0, 0};
    size_t rss0 = rss_bytes();
    std::map<uint64_t, uint64_t> m;

    double t0 = now_ms();
    for (auto k : keys) m.emplace(k, k);
    res.insert_ms = now_ms() - t0;

    size_t rss1 = rss_bytes();
    res.memory_bytes = (rss1 > rss0) ? (rss1 - rss0) : (m.size() * 72);

    uint64_t checksum = 0;
    double t1 = now_ms();
    for (auto& lk : rounds) {
        for (auto k : lk) {
            auto it = m.find(k);
            checksum += (it != m.end()) ? it->second : 0;
        }
    }
    res.read_ms = (now_ms() - t1) / static_cast<int>(rounds.size());
    do_not_optimize(checksum);

    double t2 = now_ms();
    for (auto k : rounds[0]) m.erase(k);
    res.erase_ms = now_ms() - t2;
    return res;
}

static Result bench_unorderedmap(const std::vector<uint64_t>& keys,
                                 const LookupRounds& rounds) {
    Result res{"std::unordered_map", 0, 0, 0, 0};
    size_t rss0 = rss_bytes();
    std::unordered_map<uint64_t, uint64_t> m;
    m.reserve(keys.size());

    double t0 = now_ms();
    for (auto k : keys) m.emplace(k, k);
    res.insert_ms = now_ms() - t0;

    size_t rss1 = rss_bytes();
    res.memory_bytes = (rss1 > rss0) ? (rss1 - rss0) : (m.size() * 64 + m.bucket_count() * 8);

    uint64_t checksum = 0;
    double t1 = now_ms();
    for (auto& lk : rounds) {
        for (auto k : lk) {
            auto it = m.find(k);
            checksum += (it != m.end()) ? it->second : 0;
        }
    }
    res.read_ms = (now_ms() - t1) / static_cast<int>(rounds.size());
    do_not_optimize(checksum);

    double t2 = now_ms();
    for (auto k : rounds[0]) m.erase(k);
    res.erase_ms = now_ms() - t2;
    return res;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "Usage: %s <N> [pattern] [read_iters]\n", argv[0]);
        return 1;
    }

    size_t n = std::strtoull(argv[1], nullptr, 10);
    if (n == 0) return 1;

    std::string pattern = (argc >= 3) ? argv[2] : "random";
    int read_iters = 0;
    if (argc >= 4) read_iters = std::atoi(argv[3]);
    if (read_iters <= 0) {
        if      (n <= 1000)    read_iters = 5000;
        else if (n <= 10000)   read_iters = 500;
        else if (n <= 100000)  read_iters = 50;
        else if (n <= 1000000) read_iters = 5;
        else                   read_iters = 1;
    }

    std::vector<uint64_t> keys(n);
    std::mt19937_64 rng(42);

    if (pattern == "sequential") {
        std::iota(keys.begin(), keys.end(), 0ULL);
    } else if (pattern == "dense16") {
        for (size_t i = 0; i < n; ++i)
            keys[i] = 0x123400000000ULL + (rng() % (n * 2));
    } else if (pattern == "bell") {
        std::normal_distribution<double> dist(128.0, 40.0);
        std::mt19937 rng32(42);
        for (size_t i = 0; i < n; ++i) {
            uint64_t k = 0;
            for (int b = 0; b < 8; ++b) {
                int v = static_cast<int>(dist(rng32));
                v = std::clamp(v, 0, 255);
                k = (k << 8) | static_cast<uint64_t>(v);
            }
            keys[i] = k;
        }
    } else {
        for (size_t i = 0; i < n; ++i) keys[i] = rng();
    }

    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    n = keys.size();
    std::shuffle(keys.begin(), keys.end(), rng);

    LookupRounds rounds(read_iters);
    for (int i = 0; i < read_iters; ++i) {
        rounds[i] = keys;
        std::shuffle(rounds[i].begin(), rounds[i].end(), rng);
    }

    std::printf("=== kntrie benchmark ===\nN = %zu unique keys, pattern = %s, read_iters = %d\n\n",
                n, pattern.c_str(), read_iters);

    Result r_trie = bench_kntrie(keys, rounds);
    Result r_map  = bench_stdmap(keys, rounds);
    Result r_umap = bench_unorderedmap(keys, rounds);

    print_header();
    print_row(r_trie, r_trie, n);
    print_row(r_map,  r_trie, n);
    print_row(r_umap, r_trie, n);

    std::printf("\nBytes/entry:\n");
    std::printf("  kntrie:            %6.1f\n", double(r_trie.memory_bytes) / n);
    std::printf("  std::map:           %6.1f\n", double(r_map.memory_bytes) / n);
    std::printf("  std::unordered_map: %6.1f\n", double(r_umap.memory_bytes) / n);
    return 0;
}
