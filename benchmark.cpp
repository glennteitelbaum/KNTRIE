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
    double read_ms;
    double insert_ms;
    double erase_ms;
    size_t memory_bytes;
};

static void fmt_vs(char* buf, size_t sz, double ratio) {
    if (ratio > 1.005)
        std::snprintf(buf, sz, "**%.2fx**", ratio);
    else
        std::snprintf(buf, sz, "%.2fx", ratio);
}

template<typename KEY>
using LookupRounds = std::vector<std::vector<KEY>>;

template<typename KEY>
static Result bench_kntrie(const std::vector<KEY>& keys,
                            const LookupRounds<KEY>& rounds) {
    Result res{"kntrie", 0, 0, 0, 0};
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

    auto erase_keys = keys;
    std::mt19937_64 erng(123);
    std::shuffle(erase_keys.begin(), erase_keys.end(), erng);
    double t2 = now_ms();
    for (auto k : erase_keys) trie.erase(k);
    res.erase_ms = now_ms() - t2;

    if (!trie.empty())
        std::fprintf(stderr, "kntrie: not empty after erase, %zu left\n", trie.size());

    return res;
}

template<typename KEY>
static Result bench_stdmap(const std::vector<KEY>& keys,
                           const LookupRounds<KEY>& rounds) {
    Result res{"std::map", 0, 0, 0, 0};
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

    auto erase_keys = keys;
    std::mt19937_64 erng(123);
    std::shuffle(erase_keys.begin(), erase_keys.end(), erng);
    double t2 = now_ms();
    for (auto k : erase_keys) m.erase(k);
    res.erase_ms = now_ms() - t2;

    return res;
}

template<typename KEY>
static Result bench_unorderedmap(const std::vector<KEY>& keys,
                                 const LookupRounds<KEY>& rounds) {
    Result res{"std::unordered_map", 0, 0, 0, 0};
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

    auto erase_keys = keys;
    std::mt19937_64 erng(123);
    std::shuffle(erase_keys.begin(), erase_keys.end(), erng);
    double t2 = now_ms();
    for (auto k : erase_keys) m.erase(k);
    res.erase_ms = now_ms() - t2;

    return res;
}

static void md_header() {
    std::printf("| N | | Find(ms) | Insert(ms) | Erase(ms) | Memory(KB) | B/entry |\n");
    std::printf("|---|-|----------|------------|-----------|------------|--------|\n");
}

static void md_row(const char* nlabel, const char* name, const Result& r, size_t n) {
    std::printf("| %s | %s | %.2f | %.2f | %.2f | %.1f | %.1f |\n",
                nlabel, name, r.read_ms, r.insert_ms, r.erase_ms,
                r.memory_bytes / 1024.0, double(r.memory_bytes) / n);
}

static void md_vs_row(const char* name, const Result& r, const Result& base) {
    char rd[32], ins[32], er[32], mem[32], bpe[32];
    fmt_vs(rd,  sizeof(rd),  r.read_ms    / base.read_ms);
    fmt_vs(ins, sizeof(ins), r.insert_ms  / base.insert_ms);
    fmt_vs(er,  sizeof(er),  r.erase_ms   / base.erase_ms);
    double mr = double(r.memory_bytes) / base.memory_bytes;
    fmt_vs(mem, sizeof(mem), mr);
    fmt_vs(bpe, sizeof(bpe), mr);
    std::printf("| | _%s_ | _%s_ | _%s_ | _%s_ | _%s_ | _%s_ |\n",
                name, rd, ins, er, mem, bpe);
}

static const char* fmt_n(size_t n, char* buf, size_t sz) {
    if (n >= 1000000) std::snprintf(buf, sz, "%.0fM", n / 1e6);
    else if (n >= 1000) {
        if (n % 1000 == 0) std::snprintf(buf, sz, "%zuK", n / 1000);
        else std::snprintf(buf, sz, "%.1fK", n / 1e3);
    } else std::snprintf(buf, sz, "%zu", n);
    return buf;
}

template<typename KEY>
static void run_one(size_t n, const std::string& pattern, int read_iters,
                    std::mt19937_64& rng, bool print_hdr) {
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

    if (print_hdr) md_header();

    Result r_trie = bench_kntrie(keys, rounds);
    Result r_map  = bench_stdmap(keys, rounds);
    Result r_umap = bench_unorderedmap(keys, rounds);

    char nlabel[32];
    fmt_n(n, nlabel, sizeof(nlabel));

    md_row(nlabel, "kntrie", r_trie, n);
    md_row("", "std::map", r_map, n);
    md_vs_row("map vs", r_map, r_trie);
    md_row("", "unordered_map", r_umap, n);
    md_vs_row("umap vs", r_umap, r_trie);
}

static int iters_for(size_t n) {
    if      (n <= 1000)    return 5000;
    else if (n <= 10000)   return 500;
    else if (n <= 100000)  return 50;
    else if (n <= 1000000) return 5;
    else                   return 1;
}

int main() {
    const size_t sizes[] = {1000, 10000, 100000, 1000000};
    const char* patterns[] = {"random", "sequential", "dense16"};

    std::printf("# kntrie3 Benchmark Results\n\n");
    std::printf("Compiler: `g++ -std=c++23 -O2 -march=x86-64-v3`\n\n");
    std::printf("In _vs_ rows, >1x means kntrie is better. **Bold** = kntrie wins.\n\n");

    for (auto* pat : patterns) {
        std::printf("## uint64_t — %s\n\n", pat);
        bool first = true;
        for (auto n : sizes) {
            std::mt19937_64 rng(42);
            run_one<uint64_t>(n, pat, iters_for(n), rng, first);
            first = false;
        }
        std::printf("\n");

        std::printf("## int32_t — %s\n\n", pat);
        first = true;
        for (auto n : sizes) {
            std::mt19937_64 rng(42);
            run_one<int32_t>(n, pat, iters_for(n), rng, first);
            first = false;
        }
        std::printf("\n");
    }

    return 0;
}
