#include "kntrie.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <chrono>
#include <map>
#include <unordered_map>
#include <vector>
#include <random>
#include <numeric>
#include <algorithm>
#include <string>
#include <set>

static constexpr int RUNS = 3;

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

    static Result best(const Result* runs, int n) {
        Result b = runs[0];
        for (int i = 1; i < n; ++i) {
            if (runs[i].find_ms    < b.find_ms)    b.find_ms    = runs[i].find_ms;
            if (runs[i].insert_ms  < b.insert_ms)  b.insert_ms  = runs[i].insert_ms;
            if (runs[i].erase_ms   < b.erase_ms)   b.erase_ms   = runs[i].erase_ms;
            if (runs[i].churn_ms   < b.churn_ms)   b.churn_ms   = runs[i].churn_ms;
            if (runs[i].find2_ms   < b.find2_ms)   b.find2_ms   = runs[i].find2_ms;
        }
        return b;
    }
};

static void fmt_vs(char* buf, size_t sz, double ratio) {
    if (ratio > 1.005)
        std::snprintf(buf, sz, "**%.2fx**", ratio);
    else
        std::snprintf(buf, sz, "%.2fx", ratio);
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
    std::printf("| %s | %s | %.2f | %.2f | %.1f | %.1f | %.2f | %.2f | %.2f | %.1f | %.1f |\n",
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

struct RunResults {
    Result trie, map, umap;
    size_t n;
};

template<typename KEY>
static RunResults run_one(size_t n, const std::string& pattern, int find_iters,
                          bool print_hdr) {
    std::mt19937_64 rng(42);
    auto w = make_workload<KEY>(n, pattern, find_iters, rng);
    n = w.keys.size();

    if (print_hdr) md_header();

    Result trie_runs[RUNS], map_runs[RUNS], umap_runs[RUNS];
    for (int r = 0; r < RUNS; ++r) {
        trie_runs[r] = bench_kntrie(w);
        map_runs[r]  = bench_stdmap(w);
        umap_runs[r] = bench_unorderedmap(w);
    }
    Result r_trie = Result::best(trie_runs, RUNS);
    Result r_map  = Result::best(map_runs,  RUNS);
    Result r_umap = Result::best(umap_runs, RUNS);

    char nlabel[32];
    fmt_n(n, nlabel, sizeof(nlabel));

    md_row(nlabel, "kntrie", r_trie, n);
    md_row("", "map", r_map, n);
    md_vs_row("map vs", r_map, r_trie);
    md_row("", "umap", r_umap, n);
    md_vs_row("umap vs", r_umap, r_trie);

    return {r_trie, r_map, r_umap, n};
}

// ==================================================================
// Summary: conservative rounding for vs-map ratios
// ==================================================================

static const char* round_ratio(double v, char* buf, size_t sz) {
    if (v >= 0.8 && v <= 1.3) { std::snprintf(buf, sz, "SAME"); return buf; }
    if (v < 2.0) {
        double r = std::floor(v * 4.0) / 4.0; // to nearest 0.25, round down
        int frac = static_cast<int>(r * 4) % 4;
        if (frac == 1)      std::snprintf(buf, sz, "%.2fx", r); // .25
        else if (frac == 3) std::snprintf(buf, sz, "%.2fx", r); // .75
        else                std::snprintf(buf, sz, "%.1fx", r); // .0 or .5
    } else if (v < 10.0) {
        double r = std::floor(v);
        std::snprintf(buf, sz, "%.0fx", r);
    } else {
        double r = std::floor(v / 5.0) * 5.0;
        std::snprintf(buf, sz, "%.0fx", r);
    }
    return buf;
}

static void fmt_range(double lo, double hi, char* buf, size_t sz) {
    char lo_buf[32], hi_buf[32];
    round_ratio(lo, lo_buf, sizeof(lo_buf));
    round_ratio(hi, hi_buf, sizeof(hi_buf));
    if (std::strcmp(lo_buf, hi_buf) == 0)
        std::snprintf(buf, sz, "%s", lo_buf);
    else
        std::snprintf(buf, sz, "%s–%s", lo_buf, hi_buf);
}

struct SummaryEntry {
    size_t n;
    double find_lo, find_hi;
    double ins_lo, ins_hi;
    double erase_lo, erase_hi;
    double bpe_lo, bpe_hi;
};

static void print_summary(const char* type_name,
                           const std::vector<SummaryEntry>& entries) {
    std::printf("## Summary: %s vs std::map\n\n", type_name);
    std::printf("| N | Find | Insert | Erase | B/entry |\n");
    std::printf("|---|------|--------|-------|--------|\n");
    for (auto& e : entries) {
        char nlabel[32], f[64], ins[64], er[64], bpe[64];
        fmt_n(e.n, nlabel, sizeof(nlabel));
        fmt_range(e.find_lo, e.find_hi, f, sizeof(f));
        fmt_range(e.ins_lo, e.ins_hi, ins, sizeof(ins));
        fmt_range(e.erase_lo, e.erase_hi, er, sizeof(er));
        fmt_range(e.bpe_lo, e.bpe_hi, bpe, sizeof(bpe));
        std::printf("| %s | %s | %s | %s | %s |\n", nlabel, f, ins, er, bpe);
    }
    std::printf("\n");
}

static int iters_for(size_t n) {
    if      (n <= 1000)    return 5000;
    else if (n <= 10000)   return 500;
    else if (n <= 100000)  return 50;
    else if (n <= 1000000) return 5;
    else                   return 1;
}

int main(int argc, char** argv) {
    std::vector<size_t> sizes;
    for (int i = 1; i < argc; ++i) {
        char* end;
        size_t v = std::strtoul(argv[i], &end, 10);
        if (*end == 'k' || *end == 'K') v *= 1000;
        else if (*end == 'm' || *end == 'M') v *= 1000000;
        if (v > 0) sizes.push_back(v);
    }
    if (sizes.empty()) sizes = {1000, 10000, 100000};

    const char* patterns[] = {"random", "sequential"};
    constexpr int N_PATTERNS = 2;

    std::printf("# kntrie3 Benchmark Results\n\n");
    std::printf("Compiler: `g++ -std=c++23 -O2 -march=x86-64-v3`\n\n");
    std::printf("Workload: insert N, find N (all hit), erase N/2, churn N/4 old + N/4 new, find N (25%% miss)\n\n");
    std::printf("Best of %d runs per configuration.\n\n", RUNS);
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

    // Collect results: [pattern_idx][size_idx]
    std::vector<std::vector<RunResults>> u64_results(N_PATTERNS);
    std::vector<std::vector<RunResults>> i32_results(N_PATTERNS);

    for (int pi = 0; pi < N_PATTERNS; ++pi) {
        std::printf("## uint64_t — %s\n\n", patterns[pi]);
        bool first = true;
        for (auto n : sizes) {
            auto rr = run_one<uint64_t>(n, patterns[pi], iters_for(n), first);
            u64_results[pi].push_back(rr);
            first = false;
        }
        std::printf("\n");

        std::printf("## int32_t — %s\n\n", patterns[pi]);
        first = true;
        for (auto n : sizes) {
            auto rr = run_one<int32_t>(n, patterns[pi], iters_for(n), first);
            i32_results[pi].push_back(rr);
            first = false;
        }
        std::printf("\n");
    }

    // Build summaries: for each size, range across patterns
    auto build_summary = [&](const std::vector<std::vector<RunResults>>& all)
            -> std::vector<SummaryEntry> {
        std::vector<SummaryEntry> entries;
        for (size_t si = 0; si < sizes.size(); ++si) {
            SummaryEntry e{};
            e.n = all[0][si].n;
            e.find_lo = e.find_hi = 1e30;
            e.ins_lo = e.ins_hi = 1e30;
            e.erase_lo = e.erase_hi = 1e30;
            e.bpe_lo = e.bpe_hi = 1e30;
            bool first = true;
            for (int pi = 0; pi < N_PATTERNS; ++pi) {
                auto& rr = all[pi][si];
                double f_ratio = rr.map.find_ms / rr.trie.find_ms;
                double i_ratio = rr.map.insert_ms / rr.trie.insert_ms;
                double e_ratio = rr.map.erase_ms / rr.trie.erase_ms;
                double b_ratio = double(rr.map.mem_bytes) / rr.trie.mem_bytes;
                if (first) {
                    e.find_lo = e.find_hi = f_ratio;
                    e.ins_lo = e.ins_hi = i_ratio;
                    e.erase_lo = e.erase_hi = e_ratio;
                    e.bpe_lo = e.bpe_hi = b_ratio;
                    first = false;
                } else {
                    e.find_lo = std::min(e.find_lo, f_ratio);
                    e.find_hi = std::max(e.find_hi, f_ratio);
                    e.ins_lo = std::min(e.ins_lo, i_ratio);
                    e.ins_hi = std::max(e.ins_hi, i_ratio);
                    e.erase_lo = std::min(e.erase_lo, e_ratio);
                    e.erase_hi = std::max(e.erase_hi, e_ratio);
                    e.bpe_lo = std::min(e.bpe_lo, b_ratio);
                    e.bpe_hi = std::max(e.bpe_hi, b_ratio);
                }
            }
            entries.push_back(e);
        }
        return entries;
    };

    auto u64_summary = build_summary(u64_results);
    auto i32_summary = build_summary(i32_results);

    print_summary("uint64_t", u64_summary);
    print_summary("int32_t", i32_summary);

    return 0;
}
