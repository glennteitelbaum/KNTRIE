#include "kntrie3.hpp"

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

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// Benchmark driver
// ---------------------------------------------------------------------------

struct Result {
    const char* name;
    double insert_ms;
    double read_ms;
    size_t memory_bytes;
};

static void print_header() {
    std::printf("%-20s %12s %12s %12s %12s %12s %12s %12s\n",
                "Container", "Insert(ms)", "Read(ms)", "Memory(KB)",
                "Ins rel", "Read rel", "Mem rel", "ns/read");
    std::printf("%-20s %12s %12s %12s %12s %12s %12s %12s\n",
                "--------------------", "----------", "----------", "----------",
                "----------", "----------", "----------", "----------");
}

static void print_row(const Result& r, const Result& base, size_t n) {
    double ins_rel  = r.insert_ms  / base.insert_ms;
    double read_rel = r.read_ms    / base.read_ms;
    double mem_rel  = static_cast<double>(r.memory_bytes) / base.memory_bytes;
    double ns_read  = r.read_ms * 1e6 / n;
    std::printf("%-20s %12.2f %12.2f %12.1f %11.2fx %11.2fx %11.2fx %12.1f\n",
                r.name, r.insert_ms, r.read_ms,
                r.memory_bytes / 1024.0,
                ins_rel, read_rel, mem_rel, ns_read);
}

// ---------------------------------------------------------------------------
// Benchmark functions
// ---------------------------------------------------------------------------

static Result bench_kntrie3(const std::vector<uint64_t>& keys,
                            const std::vector<uint64_t>& lookup_keys,
                            int read_iters) {
    Result res{"kntrie3", 0, 0, 0};

    kn3::kntrie3<uint64_t, uint64_t> trie;

    double t0 = now_ms();
    for (auto k : keys)
        trie.insert(k, k);
    res.insert_ms = now_ms() - t0;

    if (trie.size() != keys.size()) {
        std::fprintf(stderr, "kntrie3: size mismatch %zu vs %zu\n",
                     trie.size(), keys.size());
    }

    res.memory_bytes = trie.memory_usage();

    uint64_t checksum = 0;
    double t1 = now_ms();
    for (int iter = 0; iter < read_iters; ++iter) {
        for (auto k : lookup_keys) {
            auto* v = trie.find_value(k);
            checksum += v ? *v : 0;
        }
    }
    res.read_ms = (now_ms() - t1) / read_iters;
    do_not_optimize(checksum);

    return res;
}

static Result bench_stdmap(const std::vector<uint64_t>& keys,
                           const std::vector<uint64_t>& lookup_keys,
                           int read_iters) {
    Result res{"std::map", 0, 0, 0};

    size_t rss0 = rss_bytes();
    std::map<uint64_t, uint64_t> m;

    double t0 = now_ms();
    for (auto k : keys)
        m.emplace(k, k);
    res.insert_ms = now_ms() - t0;

    size_t rss1 = rss_bytes();
    res.memory_bytes = (rss1 > rss0) ? (rss1 - rss0) : (m.size() * 72);

    uint64_t checksum = 0;
    double t1 = now_ms();
    for (int iter = 0; iter < read_iters; ++iter) {
        for (auto k : lookup_keys) {
            auto it = m.find(k);
            checksum += (it != m.end()) ? it->second : 0;
        }
    }
    res.read_ms = (now_ms() - t1) / read_iters;
    do_not_optimize(checksum);

    return res;
}

static Result bench_unorderedmap(const std::vector<uint64_t>& keys,
                                 const std::vector<uint64_t>& lookup_keys,
                                 int read_iters) {
    Result res{"std::unordered_map", 0, 0, 0};

    size_t rss0 = rss_bytes();
    std::unordered_map<uint64_t, uint64_t> m;
    m.reserve(keys.size());

    double t0 = now_ms();
    for (auto k : keys)
        m.emplace(k, k);
    res.insert_ms = now_ms() - t0;

    size_t rss1 = rss_bytes();
    res.memory_bytes = (rss1 > rss0) ? (rss1 - rss0)
                     : (m.size() * 64 + m.bucket_count() * 8);

    uint64_t checksum = 0;
    double t1 = now_ms();
    for (int iter = 0; iter < read_iters; ++iter) {
        for (auto k : lookup_keys) {
            auto it = m.find(k);
            checksum += (it != m.end()) ? it->second : 0;
        }
    }
    res.read_ms = (now_ms() - t1) / read_iters;
    do_not_optimize(checksum);

    return res;
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

static void usage(const char* prog) {
    std::fprintf(stderr,
        "Usage: %s <N> [pattern] [read_iters]\n"
        "  N           number of elements\n"
        "  pattern     random (default), sequential, dense16, sparse\n"
        "  read_iters  number of read timing iterations (default: auto)\n",
        prog);
}

int main(int argc, char** argv) {
    if (argc < 2) { usage(argv[0]); return 1; }

    size_t n = std::strtoull(argv[1], nullptr, 10);
    if (n == 0) { usage(argv[0]); return 1; }

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
    } else if (pattern == "sparse") {
        for (size_t i = 0; i < n; ++i)
            keys[i] = rng();
    } else {
        for (size_t i = 0; i < n; ++i)
            keys[i] = rng();
    }

    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    n = keys.size();

    std::shuffle(keys.begin(), keys.end(), rng);

    std::vector<uint64_t> lookup_keys = keys;
    std::shuffle(lookup_keys.begin(), lookup_keys.end(), rng);

    std::printf("=== kntrie3 benchmark ===\n");
    std::printf("N = %zu unique keys, pattern = %s, read_iters = %d\n\n",
                n, pattern.c_str(), read_iters);

    Result r_trie   = bench_kntrie3(keys, lookup_keys, read_iters);
    Result r_map    = bench_stdmap(keys, lookup_keys, read_iters);
    Result r_umap   = bench_unorderedmap(keys, lookup_keys, read_iters);

    print_header();
    print_row(r_trie, r_trie, n);
    print_row(r_map,  r_trie, n);
    print_row(r_umap, r_trie, n);

    std::printf("\nBytes/entry:\n");
    std::printf("  kntrie3:            %6.1f\n", double(r_trie.memory_bytes) / n);
    std::printf("  std::map:           %6.1f\n", double(r_map.memory_bytes) / n);
    std::printf("  std::unordered_map: %6.1f\n", double(r_umap.memory_bytes) / n);

    return 0;
}
