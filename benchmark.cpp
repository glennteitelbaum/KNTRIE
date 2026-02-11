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
#include <set>

static double now_ms() {
    using clk = std::chrono::high_resolution_clock;
    static auto t0 = clk::now();
    return std::chrono::duration<double, std::milli>(clk::now() - t0).count();
}

template<typename T>
static void do_not_optimize(T const& val) {
    asm volatile("" : : "r,m"(val) : "memory");
}

struct Result {
    const char* name;
    double find_ms;
    double insert_ms;
    size_t mem_bytes;
    double erase_ms;
    double churn_ms;
    double find2_ms;
    size_t mem2_bytes;
};

static void fmt_vs(char* buf, size_t sz, double ratio) {
    if (ratio > 1.005)
        std::snprintf(buf, sz, "**%.3fx**", ratio);
    else
        std::snprintf(buf, sz, "%.3fx", ratio);
}

template<typename KEY>
struct Workload {
    std::vector<KEY> keys;
    std::vector<KEY> erase_keys;
    std::vector<KEY> churn_keys;
    std::vector<KEY> find1_keys;
    std::vector<KEY> find2_keys;
    int find_iters;
};

template<typename KEY>
static Workload<KEY> make_workload(size_t n, const std::string& pattern,
                                    int find_iters, std::mt19937_64& rng) {
    Workload<KEY> w;
    w.find_iters = find_iters;

    std::vector<KEY> raw(n);
    if (pattern == "sequential") {
        for (size_t i = 0; i < n; ++i) raw[i] = static_cast<KEY>(i);
    } else if (pattern == "dense16") {
        constexpr uint64_t base = std::is_same_v<KEY, int32_t> ? 0x1234ULL : 0x123400000000ULL;
        for (size_t i = 0; i < n; ++i)
            raw[i] = static_cast<KEY>(base + (rng() % (n * 2)));
    } else {
        for (size_t i = 0; i < n; ++i) raw[i] = static_cast<KEY>(rng());
    }
    std::sort(raw.begin(), raw.end());
    raw.erase(std::unique(raw.begin(), raw.end()), raw.end());
    n = raw.size();
    std::shuffle(raw.begin(), raw.end(), rng);
    w.keys = raw;

    for (size_t i = 0; i < n; i += 2)
        w.erase_keys.push_back(raw[i]);

    std::vector<KEY> reinstated;
    for (size_t i = 0; i < n; i += 4)
        reinstated.push_back(raw[i]);

    std::set<KEY> original_set(raw.begin(), raw.end());
    size_t n_new = n / 4;
    std::vector<KEY> new_keys;
    new_keys.reserve(n_new);
    while (new_keys.size() < n_new) {
        KEY k = static_cast<KEY>(rng());
        if (original_set.find(k) == original_set.end()) {
            new_keys.push_back(k);
            original_set.insert(k);
        }
    }

    w.churn_keys = reinstated;
    w.churn_keys.insert(w.churn_keys.end(), new_keys.begin(), new_keys.end());
    std::shuffle(w.churn_keys.begin(), w.churn_keys.end(), rng);

    w.find1_keys = raw;
    std::shuffle(w.find1_keys.begin(), w.find1_keys.end(), rng);

    w.find2_keys = raw;
    std::shuffle(w.find2_keys.begin(), w.find2_keys.end(), rng);

    return w;
}

template<typename KEY>
static Result bench_kntrie(const Workload<KEY>& w) {
    Result res{"kntrie", 0, 0, 0, 0, 0, 0, 0};
    gteitelbaum::kntrie<KEY, uint64_t> trie;

    double t0 = now_ms();
    for (auto k : w.keys) trie.insert(k, static_cast<uint64_t>(k));
    res.insert_ms = now_ms() - t0;

    res.mem_bytes = trie.memory_usage();

    uint64_t checksum = 0;
    double t1 = now_ms();
    for (int r = 0; r < w.find_iters; ++r) {
        for (auto k : w.find1_keys) {
            auto* v = trie.find_value(k);
            checksum += v ? *v : 0;
        }
    }
    res.find_ms = (now_ms() - t1) / w.find_iters;
    do_not_optimize(checksum);

    double t2 = now_ms();
    for (auto k : w.erase_keys) trie.erase(k);
    res.erase_ms = now_ms() - t2;

    double t3 = now_ms();
    for (auto k : w.churn_keys) trie.insert(k, static_cast<uint64_t>(k));
    res.churn_ms = now_ms() - t3;

    res.mem2_bytes = trie.memory_usage();

    checksum = 0;
    double t4 = now_ms();
    for (int r = 0; r < w.find_iters; ++r) {
        for (auto k : w.find2_keys) {
            auto* v = trie.find_value(k);
            checksum += v ? *v : 0;
        }
    }
    res.find2_ms = (now_ms() - t4) / w.find_iters;
    do_not_optimize(checksum);

    return res;
}

template<typename KEY>
static Result bench_stdmap(const Workload<KEY>& w) {
    Result res{"map", 0, 0, 0, 0, 0, 0, 0};
    std::map<KEY, uint64_t> m;

    double t0 = now_ms();
    for (auto k : w.keys) m.emplace(k, static_cast<uint64_t>(k));
    res.insert_ms = now_ms() - t0;

    res.mem_bytes = m.size() * 72;

    uint64_t checksum = 0;
    double t1 = now_ms();
    for (int r = 0; r < w.find_iters; ++r) {
        for (auto k : w.find1_keys) {
            auto it = m.find(k);
            checksum += (it != m.end()) ? it->second : 0;
        }
    }
    res.find_ms = (now_ms() - t1) / w.find_iters;
    do_not_optimize(checksum);

    double t2 = now_ms();
    for (auto k : w.erase_keys) m.erase(k);
    res.erase_ms = now_ms() - t2;

    double t3 = now_ms();
    for (auto k : w.churn_keys) m.emplace(k, static_cast<uint64_t>(k));
    res.churn_ms = now_ms() - t3;

    res.mem2_bytes = m.size() * 72;

    checksum = 0;
    double t4 = now_ms();
    for (int r = 0; r < w.find_iters; ++r) {
        for (auto k : w.find2_keys) {
            auto it = m.find(k);
            checksum += (it != m.end()) ? it->second : 0;
        }
    }
    res.find2_ms = (now_ms() - t4) / w.find_iters;
    do_not_optimize(checksum);

    return res;
}

template<typename KEY>
static Result bench_unorderedmap(const Workload<KEY>& w) {
    Result res{"umap", 0, 0, 0, 0, 0, 0, 0};
    std::unordered_map<KEY, uint64_t> m;
    m.reserve(w.keys.size());

    double t0 = now_ms();
    for (auto k : w.keys) m.emplace(k, static_cast<uint64_t>(k));
    res.insert_ms = now_ms() - t0;

    res.mem_bytes = m.size() * 64 + m.bucket_count() * 8;

    uint64_t checksum = 0;
    double t1 = now_ms();
    for (int r = 0; r < w.find_iters; ++r) {
        for (auto k : w.find1_keys) {
            auto it = m.find(k);
            checksum += (it != m.end()) ? it->second : 0;
        }
    }
    res.find_ms = (now_ms() - t1) / w.find_iters;
    do_not_optimize(checksum);

    double t2 = now_ms();
    for (auto k : w.erase_keys) m.erase(k);
    res.erase_ms = now_ms() - t2;

    double t3 = now_ms();
    for (auto k : w.churn_keys) m.emplace(k, static_cast<uint64_t>(k));
    res.churn_ms = now_ms() - t3;

    res.mem2_bytes = m.size() * 64 + m.bucket_count() * 8;

    checksum = 0;
    double t4 = now_ms();
    for (int r = 0; r < w.find_iters; ++r) {
        for (auto k : w.find2_keys) {
            auto it = m.find(k);
            checksum += (it != m.end()) ? it->second : 0;
        }
    }
    res.find2_ms = (now_ms() - t4) / w.find_iters;
    do_not_optimize(checksum);

    return res;
}

static void md_header() {
    std::printf("| N | | F | I | M | B | E | C2 | F2 | M2 | B2 |\n");
    std::printf("|---|-|---|---|---|---|---|----|----|----|----|\n");
}

static void md_row(const char* nlabel, const char* name, const Result& r, size_t n) {
    std::printf("| %s | %s | %.3f | %.3f | %.1f | %.1f | %.3f | %.3f | %.3f | %.1f | %.1f |\n",
                nlabel, name, r.find_ms, r.insert_ms,
                r.mem_bytes / 1024.0, double(r.mem_bytes) / n,
                r.erase_ms, r.churn_ms, r.find2_ms,
                r.mem2_bytes / 1024.0, double(r.mem2_bytes) / n);
}

static void md_vs_row(const char* name, const Result& r, const Result& base) {
    char fd[32], ins[32], m1[32], b1[32], er[32], ch[32], fd2[32], m2[32], b2[32];
    fmt_vs(fd,  sizeof(fd),  r.find_ms    / base.find_ms);
    fmt_vs(ins, sizeof(ins), r.insert_ms  / base.insert_ms);
    double mr1 = double(r.mem_bytes) / base.mem_bytes;
    fmt_vs(m1,  sizeof(m1),  mr1);
    fmt_vs(b1,  sizeof(b1),  mr1);
    fmt_vs(er,  sizeof(er),  r.erase_ms   / base.erase_ms);
    fmt_vs(ch,  sizeof(ch),  r.churn_ms   / base.churn_ms);
    fmt_vs(fd2, sizeof(fd2), r.find2_ms   / base.find2_ms);
    double mr2 = double(r.mem2_bytes) / base.mem2_bytes;
    fmt_vs(m2,  sizeof(m2),  mr2);
    fmt_vs(b2,  sizeof(b2),  mr2);
    std::printf("| | _%s_ | _%s_ | _%s_ | _%s_ | _%s_ | _%s_ | _%s_ | _%s_ | _%s_ | _%s_ |\n",
                name, fd, ins, m1, b1, er, ch, fd2, m2, b2);
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
static void run_one(size_t n, const std::string& pattern, int find_iters,
                    std::mt19937_64& rng, bool print_hdr) {
    auto w = make_workload<KEY>(n, pattern, find_iters, rng);
    n = w.keys.size();

    if (print_hdr) md_header();

    Result r_trie = bench_kntrie(w);
    Result r_map  = bench_stdmap(w);
    Result r_umap = bench_unorderedmap(w);

    char nlabel[32];
    fmt_n(n, nlabel, sizeof(nlabel));

    md_row(nlabel, "kntrie", r_trie, n);
    md_row("", "map", r_map, n);
    md_vs_row("map vs", r_map, r_trie);
    md_row("", "umap", r_umap, n);
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
    std::printf("Workload: insert N, find N (all hit), erase N/2, churn N/4 old + N/4 new, find N (25%% miss)\n\n");
    std::printf("- N = number of entries\n");
    std::printf("- F = Find all N keys in ms (all hits)\n");
    std::printf("- I = Insert N keys in ms\n");
    std::printf("- M = Memory after insert in KB\n");
    std::printf("- B = Bytes per entry after insert\n");
    std::printf("- E = Erase N/2 keys in ms\n");
    std::printf("- C2 = Churn insert N/4 old + N/4 new in ms\n");
    std::printf("- F2 = Find all N original keys in ms (25%% misses)\n");
    std::printf("- M2 = Memory after churn in KB\n");
    std::printf("- B2 = Bytes per entry after churn\n\n");
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
