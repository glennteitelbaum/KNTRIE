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
    if (std::fscanf(f, "%*u %lu", &pages) != 1) pages = 0;
    std::fclose(f);
    return pages * 4096UL;
}

struct Result {
    const char* name;
    double insert_ms;
    double read_ms;
    size_t memory_bytes;
};

static void print_header() {
    std::printf("%-20s %12s %12s %12s %12s\n",
                "Container", "Insert(ms)", "Read(ms)", "Memory(KB)", "Bytes/entry");
    std::printf("%-20s %12s %12s %12s %12s\n",
                "--------------------", "----------", "----------",
                "----------", "-----------");
}

static void print_row(const Result& r, size_t n) {
    std::printf("%-20s %12.2f %12.2f %12.1f %12.1f\n",
                r.name, r.insert_ms, r.read_ms,
                r.memory_bytes / 1024.0,
                double(r.memory_bytes) / n);
}

static void print_vs_row(const char* name, const Result& r, const Result& base) {
    double ins_rel  = r.insert_ms  / base.insert_ms;
    double read_rel = r.read_ms    / base.read_ms;
    double mem_rel  = static_cast<double>(r.memory_bytes) / base.memory_bytes;
    std::printf("%-20s %11.2fx %11.2fx %11.2fx %11.2fx\n",
                name, ins_rel, read_rel, mem_rel, mem_rel);
}

template<typename KEY>
using LookupRounds = std::vector<std::vector<KEY>>;

template<typename KEY>
static Result bench_kntrie(const std::vector<KEY>& keys,
                            const LookupRounds<KEY>& rounds) {
    Result res{"kntrie", 0, 0, 0};
    gteitelbaum::kntrie<KEY, uint64_t> trie;

    double t0 = now_ms();
    for (auto k : keys) trie.insert(k, static_cast<uint64_t>(k));
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
    return res;
}

template<typename KEY>
static Result bench_stdmap(const std::vector<KEY>& keys,
                           const LookupRounds<KEY>& rounds) {
    Result res{"std::map", 0, 0, 0};
    size_t rss0 = rss_bytes();
    std::map<KEY, uint64_t> m;

    double t0 = now_ms();
    for (auto k : keys) m.emplace(k, static_cast<uint64_t>(k));
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
    return res;
}

template<typename KEY>
static Result bench_unorderedmap(const std::vector<KEY>& keys,
                                 const LookupRounds<KEY>& rounds) {
    Result res{"std::unordered_map", 0, 0, 0};
    size_t rss0 = rss_bytes();
    std::unordered_map<KEY, uint64_t> m;
    m.reserve(keys.size());

    double t0 = now_ms();
    for (auto k : keys) m.emplace(k, static_cast<uint64_t>(k));
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
    return res;
}

template<typename KEY>
static void run_bench(const char* key_label, size_t n, const std::string& pattern,
                      int read_iters, std::mt19937_64& rng) {
    std::vector<KEY> keys(n);

    if (pattern == "sequential") {
        for (size_t i = 0; i < n; ++i) keys[i] = static_cast<KEY>(i);
    } else if (pattern == "dense16") {
        constexpr uint64_t base = std::is_same_v<KEY, int32_t> ? 0x1234ULL : 0x123400000000ULL;
        for (size_t i = 0; i < n; ++i)
            keys[i] = static_cast<KEY>(base + (rng() % (n * 2)));
    } else {
        for (size_t i = 0; i < n; ++i) keys[i] = static_cast<KEY>(rng());
    }

    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    n = keys.size();
    std::shuffle(keys.begin(), keys.end(), rng);

    LookupRounds<KEY> rounds(read_iters);
    for (int i = 0; i < read_iters; ++i) {
        rounds[i] = keys;
        std::shuffle(rounds[i].begin(), rounds[i].end(), rng);
    }

    std::printf("=== %s — %s, N=%zu, iters=%d ===\n\n",
                key_label, pattern.c_str(), n, read_iters);

    Result r_trie = bench_kntrie(keys, rounds);
    Result r_map  = bench_stdmap(keys, rounds);
    Result r_umap = bench_unorderedmap(keys, rounds);

    print_header();
    print_row(r_trie, n);
    print_row(r_map,  n);
    print_vs_row("  map vs kntrie", r_map, r_trie);
    print_row(r_umap, n);
    print_vs_row("  umap vs kntrie", r_umap, r_trie);
    std::printf("\n");
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

    std::mt19937_64 rng(42);
    run_bench<uint64_t>("kntrie<uint64_t, uint64_t>", n, pattern, read_iters, rng);
    rng.seed(42);
    run_bench<int32_t>("kntrie<int32_t, uint64_t>", n, pattern, read_iters, rng);

    return 0;
}
