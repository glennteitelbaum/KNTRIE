#include "kntrie.hpp"

#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <map>
#include <unordered_map>
#include <vector>
#include <random>
#include <algorithm>
#include <set>
#include <string>
#include <cstring>
#include <new>

// ==========================================================================
// Tracking allocator
// ==========================================================================

static thread_local size_t g_alloc_total = 0;

template<typename T>
struct TrackingAlloc {
    using value_type = T;
    TrackingAlloc() noexcept = default;
    template<typename U> TrackingAlloc(const TrackingAlloc<U>&) noexcept {}
    T* allocate(size_t n) {
        size_t bytes = n * sizeof(T);
        g_alloc_total += bytes;
        if constexpr (alignof(T) > __STDCPP_DEFAULT_NEW_ALIGNMENT__)
            return static_cast<T*>(::operator new(bytes, std::align_val_t{alignof(T)}));
        else
            return static_cast<T*>(::operator new(bytes));
    }
    void deallocate(T* p, size_t n) noexcept {
        g_alloc_total -= n * sizeof(T);
        if constexpr (alignof(T) > __STDCPP_DEFAULT_NEW_ALIGNMENT__)
            ::operator delete(p, std::align_val_t{alignof(T)});
        else
            ::operator delete(p);
    }
    template<typename U> bool operator==(const TrackingAlloc<U>&) const noexcept { return true; }
};

// ==========================================================================
// Helpers
// ==========================================================================

static double now_ms() {
    using clk = std::chrono::high_resolution_clock;
    static auto t0 = clk::now();
    return std::chrono::duration<double, std::milli>(clk::now() - t0).count();
}

template<typename T>
static void do_not_optimize(T const& val) {
    asm volatile("" : : "r,m"(val) : "memory");
}

// ==========================================================================
// big256_t — 256 byte trivially copyable struct for testing C-type path
// ==========================================================================

struct big256_t {
    uint8_t data[256] = {};
    bool operator==(const big256_t&) const = default;
};

template<> struct std::hash<big256_t> {
    size_t operator()(const big256_t& b) const noexcept {
        size_t h = 0;
        for (size_t i = 0; i < 256; i += 8)
            h ^= *reinterpret_cast<const size_t*>(b.data + i) + 0x9e3779b97f4a7c15 + (h << 6) + (h >> 2);
        return h;
    }
};

// ==========================================================================
// val_convert<K,V> — create a V from a K for insertion
// ==========================================================================

template<typename K, typename V>
struct val_convert {
    static V from_key(K k) {
        if constexpr (std::is_same_v<V, bool>)
            return static_cast<bool>(k & 1);
        else if constexpr (std::is_arithmetic_v<V>)
            return static_cast<V>(k);
        else if constexpr (std::is_same_v<V, std::string>)
            return std::to_string(static_cast<uint64_t>(k));
        else if constexpr (std::is_same_v<V, big256_t>) {
            big256_t b{};
            std::memcpy(b.data, &k, std::min(sizeof(k), sizeof(b.data)));
            return b;
        }
        else
            return V{};
    }
};

// ==========================================================================
// acc — accumulate for do_not_optimize (works with any V)
// ==========================================================================

template<typename V>
uint64_t acc(const V* p) {
    if (!p) return 0;
    if constexpr (std::is_arithmetic_v<V>)
        return static_cast<uint64_t>(*p);
    else if constexpr (std::is_same_v<V, std::string>)
        return p->size();
    else if constexpr (std::is_same_v<V, big256_t>)
        return p->data[0];
    else
        return 1;
}

template<typename V>
uint64_t acc(const V& v) {
    return acc(&v);
}

// ==========================================================================
// Workload — key generation and test vectors
// ==========================================================================

struct Workload {
    std::vector<uint64_t> keys;
    std::vector<uint64_t> erase_keys;
    std::vector<uint64_t> find_fnd;
    std::vector<uint64_t> find_nf;
    int find_iters;
};

template<typename K>
static Workload make_workload(size_t n, const std::string& pattern,
                               int find_iters, std::mt19937_64& rng) {
    Workload w;
    w.find_iters = find_iters;

    constexpr uint64_t KEY_MAX = (sizeof(K) >= 8) ? ~uint64_t(0)
        : (uint64_t(1) << (sizeof(K) * 8)) - 1;

    // Cap n to key range
    if (n > KEY_MAX / 2) n = static_cast<size_t>(KEY_MAX / 2);

    std::vector<uint64_t> raw(n);
    if (pattern == "sequential") {
        for (size_t i = 0; i < n; ++i)
            raw[i] = (i * 2) & KEY_MAX;
    } else {
        for (size_t i = 0; i < n; ++i)
            raw[i] = rng() & KEY_MAX;
    }
    std::sort(raw.begin(), raw.end());
    raw.erase(std::unique(raw.begin(), raw.end()), raw.end());
    n = raw.size();
    std::shuffle(raw.begin(), raw.end(), rng);
    w.keys = raw;

    for (size_t i = 0; i < n; i += 2)
        w.erase_keys.push_back(raw[i]);

    w.find_fnd = raw;
    std::shuffle(w.find_fnd.begin(), w.find_fnd.end(), rng);

    if (pattern == "sequential") {
        w.find_nf.reserve(n);
        for (size_t i = 0; i < n; ++i)
            w.find_nf.push_back((i * 2 + 1) & KEY_MAX);
    } else {
        std::set<uint64_t> existing(raw.begin(), raw.end());
        w.find_nf.reserve(n);
        while (w.find_nf.size() < n) {
            uint64_t k = rng() & KEY_MAX;
            if (!existing.count(k)) {
                w.find_nf.push_back(k);
                existing.insert(k);
            }
        }
    }
    std::shuffle(w.find_nf.begin(), w.find_nf.end(), rng);

    return w;
}

static int iters_for(size_t n) {
    if      (n <= 200)      return 2000;
    else if (n <= 1000)     return 500;
    else if (n <= 10000)    return 50;
    else if (n <= 100000)   return 5;
    else                    return 1;
}

// ==========================================================================
// Row + emit_html
// ==========================================================================

struct Row {
    std::string pattern;
    size_t n;
    const char* container;
    double find_fnd_ms;
    double find_nf_ms;
    double insert_ms;
    double erase_ms;
    double iter_ms;
    size_t mem_bytes;
};

constexpr int TRIALS = 3;

static void emit_html(const std::vector<Row>& rows,
                      const char* key_name, const char* val_name) {
    struct DataPoint {
        std::string pattern;
        size_t N;
        double vals[3][6];
        bool has[3];
    };

    auto cidx = [](const char* c) -> int {
        if (std::strcmp(c, "kntrie") == 0) return 0;
        if (std::strcmp(c, "map") == 0) return 1;
        return 2;
    };

    std::vector<DataPoint> points;

    for (auto& r : rows) {
        int pi = -1;
        for (int i = 0; i < (int)points.size(); ++i) {
            if (points[i].pattern == r.pattern && points[i].N == r.n) { pi = i; break; }
        }
        if (pi < 0) {
            pi = (int)points.size();
            DataPoint dp{};
            dp.pattern = r.pattern;
            dp.N = r.n;
            std::memset(dp.has, 0, sizeof(dp.has));
            points.push_back(dp);
        }
        int ci = cidx(r.container);
        points[pi].vals[ci][0] = r.find_fnd_ms;
        points[pi].vals[ci][1] = r.find_nf_ms;
        points[pi].vals[ci][2] = r.insert_ms;
        points[pi].vals[ci][3] = r.erase_ms;
        points[pi].vals[ci][4] = r.iter_ms;
        points[pi].vals[ci][5] = static_cast<double>(r.mem_bytes);
        points[pi].has[ci] = true;
    }

    std::sort(points.begin(), points.end(), [](const DataPoint& a, const DataPoint& b) {
        if (a.pattern != b.pattern) return a.pattern < b.pattern;
        return a.N < b.N;
    });

    const char* names[] = {"kntrie", "map", "umap"};
    const char* suffixes[] = {"fnd", "nf", "insert", "erase", "iter", "mem"};

    std::printf(R"HTML(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8"/>
<meta name="viewport" content="width=device-width, initial-scale=1"/>
<title>kntrie Benchmark</title>
<script src="https://cdn.jsdelivr.net/npm/chart.js@4.4.4/dist/chart.umd.min.js"></script>
<style>
  body { margin:0; background:#0f0f1a; color:#ddd; font-family:system-ui,sans-serif; }
  .wrap { max-width:700px; margin:0 auto; padding:16px 12px; }
  h2 { margin:0 0 4px; font-size:18px; font-weight:700; text-align:center; }
  .sub { text-align:center; color:#777; font-size:12px; margin:0 0 12px; }
  .btns { display:flex; justify-content:center; gap:8px; margin-bottom:16px; }
  .btns button { padding:6px 16px; border-radius:6px; border:1px solid #444;
    background:#1a1a2e; color:#aaa; cursor:pointer; font-size:13px; font-weight:600; }
  .btns button.active { background:#3b82f6; color:#fff; }
  .chart-box { margin-bottom:24px; }
  .chart-box h3 { margin:0 0 6px; font-size:14px; font-weight:600; text-align:center; }
  canvas { background:#12122a; border-radius:8px; }
</style>
</head>
<body>
<div class="wrap">
  <h2>kntrie Benchmark (%s &rarr; %s)</h2>
  <p class="sub">Log-log &middot; Per-entry &middot; Lower is better &middot; FND=100%% hit, NF=100%% miss</p>
  <div class="btns">
    <button class="active" onclick="show('random')">random</button>
    <button onclick="show('sequential')">sequential</button>
  </div>
  <div class="chart-box"><h3>Find (ns/entry)</h3><canvas id="c_find"></canvas></div>
  <div class="chart-box"><h3>Iteration (ns/entry)</h3><canvas id="c_iter"></canvas></div>
  <div class="chart-box"><h3>Insert (ns/entry)</h3><canvas id="c_insert"></canvas></div>
  <div class="chart-box"><h3>Erase N/2 (ns/entry)</h3><canvas id="c_erase"></canvas></div>
  <div class="chart-box"><h3>Memory (B/entry)</h3><canvas id="c_mem"></canvas></div>
</div>
<script>
)HTML", key_name, val_name);

    std::printf("const RAW_DATA = [\n");
    for (auto& p : points) {
        std::printf("  {pattern:\"%s\",N:%zu", p.pattern.c_str(), p.N);
        for (int ci = 0; ci < 3; ++ci) {
            if (!p.has[ci]) continue;
            for (int mi = 0; mi < 6; ++mi) {
                if (mi == 5)
                    std::printf(",%s_%s:%.0f", names[ci], suffixes[mi], p.vals[ci][mi]);
                else
                    std::printf(",%s_%s:%.4f", names[ci], suffixes[mi], p.vals[ci][mi]);
            }
        }
        std::printf("},\n");
    }
    std::printf("];\n\n");

    std::printf("%s",
R"JS(
const LINES_FIND = [
  { key: "kntrie", suffix: "fnd", color: "#3b82f6", dash: [],    width: 2.5, label: "kntrie FND" },
  { key: "kntrie", suffix: "nf", color: "#93c5fd", dash: [6,3], width: 1.5, label: "kntrie NF" },
  { key: "map",    suffix: "fnd", color: "#ef4444", dash: [],    width: 2.5, label: "map FND" },
  { key: "map",    suffix: "nf", color: "#fca5a5", dash: [6,3], width: 1.5, label: "map NF" },
  { key: "umap",   suffix: "fnd", color: "#22c55e", dash: [],    width: 2.5, label: "umap FND" },
  { key: "umap",   suffix: "nf", color: "#86efac", dash: [6,3], width: 1.5, label: "umap NF" },
];

const LINES_OP = [
  { key: "kntrie", suffix: "insert", color: "#3b82f6", dash: [], width: 2.5, label: "kntrie" },
  { key: "map",    suffix: "insert", color: "#ef4444", dash: [], width: 2.5, label: "map" },
  { key: "umap",   suffix: "insert", color: "#22c55e", dash: [], width: 2.5, label: "umap" },
];

const LINES_ERASE = [
  { key: "kntrie", suffix: "erase", color: "#3b82f6", dash: [], width: 2.5, label: "kntrie" },
  { key: "map",    suffix: "erase", color: "#ef4444", dash: [], width: 2.5, label: "map" },
  { key: "umap",   suffix: "erase", color: "#22c55e", dash: [], width: 2.5, label: "umap" },
];

const LINES_MEM = [
  { key: "kntrie", suffix: "mem", color: "#3b82f6", dash: [], width: 2.5, label: "kntrie" },
  { key: "map",    suffix: "mem", color: "#ef4444", dash: [], width: 2.5, label: "map" },
  { key: "umap",   suffix: "mem", color: "#22c55e", dash: [], width: 2.5, label: "umap" },
  { key: "raw",    suffix: "mem", color: "#888",    dash: [3,3], width: 1, label: "raw (16B)" },
];

const LINES_ITER = [
  { key: "kntrie", suffix: "iter", color: "#3b82f6", dash: [], width: 2.5, label: "kntrie" },
  { key: "map",    suffix: "iter", color: "#ef4444", dash: [], width: 2.5, label: "map" },
  { key: "umap",   suffix: "iter", color: "#22c55e", dash: [], width: 2.5, label: "umap" },
];

const METRICS = [
  { id: "find",   lines: LINES_FIND,  convert: (ms, n) => (ms * 1e6) / n },
  { id: "iter",   lines: LINES_ITER,  convert: (ms, n) => (ms * 1e6) / n },
  { id: "insert", lines: LINES_OP,    convert: (ms, n) => (ms * 1e6) / n },
  { id: "erase",  lines: LINES_ERASE, convert: (ms, n) => (ms * 1e6) / (n / 2) },
  { id: "mem",    lines: LINES_MEM,   convert: (b, n) => b / n },
];

function buildData(pattern, metric) {
  return RAW_DATA
    .filter(r => r.pattern === pattern)
    .map(r => {
      const pt = { N: r.N };
      for (const l of metric.lines) {
        if (l.key === "raw") { pt["raw_mem"] = 16; continue; }
        const raw = r[l.key + "_" + l.suffix];
        if (raw != null) pt[l.key + "_" + l.suffix] = metric.convert(raw, r.N);
      }
      return pt;
    });
}

const charts = {};

function makeChart(canvasId, metric) {
  const ctx = document.getElementById(canvasId).getContext("2d");
  const data = buildData("random", metric);

  charts[canvasId] = new Chart(ctx, {
    type: "line",
    data: {
      labels: data.map(d => d.N),
      datasets: metric.lines.map(l => ({
        label: l.label,
        data: data.map(d => d[l.key === "raw" ? "raw_mem" : l.key + "_" + l.suffix] ?? null),
        borderColor: l.color,
        backgroundColor: l.color + "33",
        borderWidth: l.width,
        borderDash: l.dash,
        pointRadius: 0,
        pointHitRadius: 8,
        tension: 0.2,
        spanGaps: true,
      })),
    },
    options: {
      responsive: true,
      interaction: { mode: "index", intersect: false },
      plugins: {
        legend: { display: true, labels: { color: "#bbb", font: { size: 11 }, boxWidth: 20, padding: 10 } },
        tooltip: {
          backgroundColor: "#1a1a2e",
          borderColor: "#444",
          borderWidth: 1,
          titleColor: "#aaa",
          bodyColor: "#ddd",
          callbacks: {
            title: (items) => {
              const v = items[0].parsed.x;
              if (v >= 1e6) return "N = " + (v/1e6).toFixed(1) + "M";
              if (v >= 1e3) return "N = " + (v/1e3).toFixed(1) + "K";
              return "N = " + v;
            },
            label: (item) => {
              const v = item.parsed.y;
              if (v == null) return null;
              const s = v < 0.1 ? v.toFixed(3) : v < 10 ? v.toFixed(2) : v < 1000 ? v.toFixed(1) : v.toFixed(0);
              return " " + item.dataset.label + ": " + s;
            },
          },
        },
      },
      scales: {
        x: {
          type: "logarithmic",
          title: { display: false },
          ticks: { color: "#888", font: { size: 11 },
            callback: (v) => v >= 1e6 ? (v/1e6)+"M" : v >= 1e3 ? (v/1e3)+"K" : v },
          grid: { color: "#2a2a3e" },
        },
        y: {
          type: "logarithmic",
          ticks: { color: "#888", font: { size: 10 },
            callback: (v) => v < 0.1 ? v.toFixed(2) : v < 10 ? v.toFixed(1) : v >= 1000 ? v.toFixed(0) : v.toFixed(1) },
          grid: { color: "#2a2a3e" },
        },
      },
    },
  });
  charts[canvasId]._metric = metric;
}

METRICS.forEach(m => makeChart("c_" + m.id, m));

function show(pattern) {
  document.querySelectorAll(".btns button").forEach(b => b.classList.remove("active"));
  event.target.classList.add("active");
  for (const [id, chart] of Object.entries(charts)) {
    const m = chart._metric;
    const data = buildData(pattern, m);
    chart.data.labels = data.map(d => d.N);
    m.lines.forEach((l, i) => {
      chart.data.datasets[i].data = data.map(d => d[l.key === "raw" ? "raw_mem" : l.key + "_" + l.suffix] ?? null);
    });
    chart.update("none");
  }
}
)JS");

    std::printf("</script>\n</body>\n</html>\n");
}

// ==========================================================================
// bench_all<K,V> — templated benchmark core
// ==========================================================================

template<typename K, typename V>
static void bench_all(size_t target_n, const std::string& pattern,
                      std::vector<Row>& rows, bool verbose) {
    using VC = val_convert<K, V>;
    std::mt19937_64 rng(42);
    int fi = iters_for(target_n);
    auto w = make_workload<K>(target_n, pattern, fi, rng);
    size_t n = w.keys.size();
    bool do_map = (n <= 250000);

    if (verbose)
        std::fprintf(stderr, "%s N=%zu...\n", pattern.c_str(), n);

    // Memory: one tracked run per container
    size_t kntrie_mem, map_mem = 0, umap_mem;
    {
        using TrieT = gteitelbaum::kntrie<K, V, TrackingAlloc<uint64_t>>;
        g_alloc_total = 0;
        TrieT trie;
        for (auto k : w.keys) trie.insert(static_cast<K>(k), VC::from_key(static_cast<K>(k)));
        kntrie_mem = g_alloc_total;
    }
    if (do_map) {
        using MapT = std::map<K, V, std::less<K>,
                              TrackingAlloc<std::pair<const K, V>>>;
        g_alloc_total = 0;
        MapT m;
        for (auto k : w.keys) m.emplace(static_cast<K>(k), VC::from_key(static_cast<K>(k)));
        map_mem = g_alloc_total;
    }
    {
        using UMapT = std::unordered_map<K, V, std::hash<K>, std::equal_to<K>,
                                          TrackingAlloc<std::pair<const K, V>>>;
        g_alloc_total = 0;
        UMapT m;
        m.reserve(w.keys.size());
        for (auto k : w.keys) m.emplace(static_cast<K>(k), VC::from_key(static_cast<K>(k)));
        umap_mem = g_alloc_total;
    }

    // Pre-generate find orders
    std::vector<std::vector<uint64_t>> fnd_orders(fi), nf_orders(fi);
    for (int r = 0; r < fi; ++r) {
        fnd_orders[r] = w.find_fnd;
        std::shuffle(fnd_orders[r].begin(), fnd_orders[r].end(), rng);
        nf_orders[r] = w.find_nf;
        std::shuffle(nf_orders[r].begin(), nf_orders[r].end(), rng);
    }

    double k_fnd = 1e18, k_nf = 1e18, k_ins = 1e18, k_ers = 1e18, k_iter = 1e18;
    double m_fnd = 1e18, m_nf = 1e18, m_ins = 1e18, m_ers = 1e18, m_iter = 1e18;
    double u_fnd = 1e18, u_nf = 1e18, u_ins = 1e18, u_ers = 1e18, u_iter = 1e18;

    for (int t = 0; t < TRIALS; ++t) {
        // --- kntrie ---
        std::shuffle(w.keys.begin(), w.keys.end(), rng);
        {
            gteitelbaum::kntrie<K, V> trie;
            double t0 = now_ms();
            for (auto k : w.keys) trie.insert(static_cast<K>(k), VC::from_key(static_cast<K>(k)));
            k_ins = std::min(k_ins, now_ms() - t0);

            { uint64_t is = 0; double ti = now_ms();
              for (const auto& [k,v] : trie) is += acc(v);
              k_iter = std::min(k_iter, now_ms() - ti); do_not_optimize(is); }

            uint64_t cs = 0;
            double t1 = now_ms();
            for (int r = 0; r < fi; ++r)
                for (auto k : fnd_orders[r]) { auto* v = trie.find_value(static_cast<K>(k)); cs += acc(v); }
            k_fnd = std::min(k_fnd, (now_ms() - t1) / fi);
            do_not_optimize(cs);

            cs = 0;
            double t1n = now_ms();
            for (int r = 0; r < fi; ++r)
                for (auto k : nf_orders[r]) { auto* v = trie.find_value(static_cast<K>(k)); cs += acc(v); }
            k_nf = std::min(k_nf, (now_ms() - t1n) / fi);
            do_not_optimize(cs);

            std::shuffle(w.erase_keys.begin(), w.erase_keys.end(), rng);
            double t2 = now_ms();
            for (auto k : w.erase_keys) trie.erase(static_cast<K>(k));
            k_ers = std::min(k_ers, now_ms() - t2);
        }

        // --- map ---
        if (do_map) {
            std::shuffle(w.keys.begin(), w.keys.end(), rng);
            std::map<K, V> m;
            double t0 = now_ms();
            for (auto k : w.keys) m.emplace(static_cast<K>(k), VC::from_key(static_cast<K>(k)));
            m_ins = std::min(m_ins, now_ms() - t0);

            { uint64_t is = 0; double ti = now_ms();
              for (auto& [k,v] : m) is += acc(v);
              m_iter = std::min(m_iter, now_ms() - ti); do_not_optimize(is); }

            uint64_t cs = 0;
            double t1 = now_ms();
            for (int r = 0; r < fi; ++r)
                for (auto k : fnd_orders[r]) { auto it = m.find(static_cast<K>(k)); cs += (it != m.end()) ? acc(it->second) : 0; }
            m_fnd = std::min(m_fnd, (now_ms() - t1) / fi);
            do_not_optimize(cs);

            cs = 0;
            double t1n = now_ms();
            for (int r = 0; r < fi; ++r)
                for (auto k : nf_orders[r]) { auto it = m.find(static_cast<K>(k)); cs += (it != m.end()) ? acc(it->second) : 0; }
            m_nf = std::min(m_nf, (now_ms() - t1n) / fi);
            do_not_optimize(cs);

            std::shuffle(w.erase_keys.begin(), w.erase_keys.end(), rng);
            double t2 = now_ms();
            for (auto k : w.erase_keys) m.erase(static_cast<K>(k));
            m_ers = std::min(m_ers, now_ms() - t2);
        }

        // --- umap ---
        std::shuffle(w.keys.begin(), w.keys.end(), rng);
        {
            std::unordered_map<K, V> m;
            m.reserve(w.keys.size());
            double t0 = now_ms();
            for (auto k : w.keys) m.emplace(static_cast<K>(k), VC::from_key(static_cast<K>(k)));
            u_ins = std::min(u_ins, now_ms() - t0);

            { uint64_t is = 0; double ti = now_ms();
              for (auto& [k,v] : m) is += acc(v);
              u_iter = std::min(u_iter, now_ms() - ti); do_not_optimize(is); }

            uint64_t cs = 0;
            double t1 = now_ms();
            for (int r = 0; r < fi; ++r)
                for (auto k : fnd_orders[r]) { auto it = m.find(static_cast<K>(k)); cs += (it != m.end()) ? acc(it->second) : 0; }
            u_fnd = std::min(u_fnd, (now_ms() - t1) / fi);
            do_not_optimize(cs);

            cs = 0;
            double t1n = now_ms();
            for (int r = 0; r < fi; ++r)
                for (auto k : nf_orders[r]) { auto it = m.find(static_cast<K>(k)); cs += (it != m.end()) ? acc(it->second) : 0; }
            u_nf = std::min(u_nf, (now_ms() - t1n) / fi);
            do_not_optimize(cs);

            std::shuffle(w.erase_keys.begin(), w.erase_keys.end(), rng);
            double t2 = now_ms();
            for (auto k : w.erase_keys) m.erase(static_cast<K>(k));
            u_ers = std::min(u_ers, now_ms() - t2);
        }
    }

    rows.push_back({pattern, n, "kntrie", k_fnd, k_nf, k_ins, k_ers, k_iter, kntrie_mem});
    if (do_map)
        rows.push_back({pattern, n, "map", m_fnd, m_nf, m_ins, m_ers, m_iter, map_mem});
    rows.push_back({pattern, n, "umap", u_fnd, u_nf, u_ins, u_ers, u_iter, umap_mem});
}

// ==========================================================================
// run_bench<K,V> — top-level for a given K,V pair
// ==========================================================================

template<typename K, typename V>
static void run_bench(size_t max_n, bool verbose,
                      const char* key_name, const char* val_name) {
    std::vector<size_t> sizes;
    for (double n = 100; n < static_cast<double>(max_n); n *= 1.5)
        sizes.push_back(static_cast<size_t>(n));

    const char* patterns[] = {"random", "sequential"};
    std::vector<Row> rows;

    for (auto* pat : patterns)
        for (auto n : sizes)
            bench_all<K, V>(n, pat, rows, verbose);

    emit_html(rows, key_name, val_name);
}

// ==========================================================================
// Dispatch — nested switch on key type x value type
// ==========================================================================

enum type_id {
    T_BOOL, T_U8, T_I8, T_U16, T_I16, T_U32, T_I32, T_U64, T_I64,
    T_STRING, T_BIG256, T_INVALID
};

static type_id parse_type(const char* s) {
    if (!std::strcmp(s, "bool"))   return T_BOOL;
    if (!std::strcmp(s, "u8"))     return T_U8;
    if (!std::strcmp(s, "i8"))     return T_I8;
    if (!std::strcmp(s, "u16"))    return T_U16;
    if (!std::strcmp(s, "i16"))    return T_I16;
    if (!std::strcmp(s, "u32"))    return T_U32;
    if (!std::strcmp(s, "i32"))    return T_I32;
    if (!std::strcmp(s, "u64"))    return T_U64;
    if (!std::strcmp(s, "i64"))    return T_I64;
    if (!std::strcmp(s, "string")) return T_STRING;
    if (!std::strcmp(s, "big256")) return T_BIG256;
    return T_INVALID;
}


int main(int argc, char* argv[]) {
    if (argc != 5) {
        std::fprintf(stderr,
            "Usage: %s <key_type> <val_type> <max_entries> <verbose:y/n>\n"
            "  Key types:   u16 i16 u32 i32 u64 i64\n"
            "  Value types: bool u8 i8 u16 i16 u32 i32 u64 i64 string big256\n"
            "  Example: %s u64 i32 6000000 y\n",
            argv[0], argv[0]);
        return 1;
    }

    const char* kn = argv[1];
    const char* vn = argv[2];
    size_t max_n = static_cast<size_t>(std::atof(argv[3]));
    bool verbose = (argv[4][0] == 'y' || argv[4][0] == 'Y');

    type_id kt = parse_type(kn);
    type_id vt = parse_type(vn);

    if (kt == T_INVALID || kt == T_STRING || kt == T_BIG256 || kt == T_BOOL
        || kt == T_U8 || kt == T_I8) {
        std::fprintf(stderr, "Invalid key type: %s (must be u16/i16/u32/i32/u64/i64)\n", kn);
        return 1;
    }
    if (vt == T_INVALID) {
        std::fprintf(stderr, "Invalid value type: %s\n", vn);
        return 1;
    }

    switch (kt) {
        case T_U16:
            switch (vt) {
                case T_BOOL:   run_bench<uint16_t, bool>       (max_n, verbose, kn, vn); break;
                case T_U8:     run_bench<uint16_t, uint8_t>    (max_n, verbose, kn, vn); break;
                case T_I8:     run_bench<uint16_t, int8_t>     (max_n, verbose, kn, vn); break;
                case T_U16:    run_bench<uint16_t, uint16_t>   (max_n, verbose, kn, vn); break;
                case T_I16:    run_bench<uint16_t, int16_t>    (max_n, verbose, kn, vn); break;
                case T_U32:    run_bench<uint16_t, uint32_t>   (max_n, verbose, kn, vn); break;
                case T_I32:    run_bench<uint16_t, int32_t>    (max_n, verbose, kn, vn); break;
                case T_U64:    run_bench<uint16_t, uint64_t>   (max_n, verbose, kn, vn); break;
                case T_I64:    run_bench<uint16_t, int64_t>    (max_n, verbose, kn, vn); break;
                case T_STRING: run_bench<uint16_t, std::string>(max_n, verbose, kn, vn); break;
                case T_BIG256: run_bench<uint16_t, big256_t>   (max_n, verbose, kn, vn); break;
                default: goto bad_val;
            } break;
        case T_I16:
            switch (vt) {
                case T_BOOL:   run_bench<int16_t, bool>       (max_n, verbose, kn, vn); break;
                case T_U8:     run_bench<int16_t, uint8_t>    (max_n, verbose, kn, vn); break;
                case T_I8:     run_bench<int16_t, int8_t>     (max_n, verbose, kn, vn); break;
                case T_U16:    run_bench<int16_t, uint16_t>   (max_n, verbose, kn, vn); break;
                case T_I16:    run_bench<int16_t, int16_t>    (max_n, verbose, kn, vn); break;
                case T_U32:    run_bench<int16_t, uint32_t>   (max_n, verbose, kn, vn); break;
                case T_I32:    run_bench<int16_t, int32_t>    (max_n, verbose, kn, vn); break;
                case T_U64:    run_bench<int16_t, uint64_t>   (max_n, verbose, kn, vn); break;
                case T_I64:    run_bench<int16_t, int64_t>    (max_n, verbose, kn, vn); break;
                case T_STRING: run_bench<int16_t, std::string>(max_n, verbose, kn, vn); break;
                case T_BIG256: run_bench<int16_t, big256_t>   (max_n, verbose, kn, vn); break;
                default: goto bad_val;
            } break;
        case T_U32:
            switch (vt) {
                case T_BOOL:   run_bench<uint32_t, bool>       (max_n, verbose, kn, vn); break;
                case T_U8:     run_bench<uint32_t, uint8_t>    (max_n, verbose, kn, vn); break;
                case T_I8:     run_bench<uint32_t, int8_t>     (max_n, verbose, kn, vn); break;
                case T_U16:    run_bench<uint32_t, uint16_t>   (max_n, verbose, kn, vn); break;
                case T_I16:    run_bench<uint32_t, int16_t>    (max_n, verbose, kn, vn); break;
                case T_U32:    run_bench<uint32_t, uint32_t>   (max_n, verbose, kn, vn); break;
                case T_I32:    run_bench<uint32_t, int32_t>    (max_n, verbose, kn, vn); break;
                case T_U64:    run_bench<uint32_t, uint64_t>   (max_n, verbose, kn, vn); break;
                case T_I64:    run_bench<uint32_t, int64_t>    (max_n, verbose, kn, vn); break;
                case T_STRING: run_bench<uint32_t, std::string>(max_n, verbose, kn, vn); break;
                case T_BIG256: run_bench<uint32_t, big256_t>   (max_n, verbose, kn, vn); break;
                default: goto bad_val;
            } break;
        case T_I32:
            switch (vt) {
                case T_BOOL:   run_bench<int32_t, bool>       (max_n, verbose, kn, vn); break;
                case T_U8:     run_bench<int32_t, uint8_t>    (max_n, verbose, kn, vn); break;
                case T_I8:     run_bench<int32_t, int8_t>     (max_n, verbose, kn, vn); break;
                case T_U16:    run_bench<int32_t, uint16_t>   (max_n, verbose, kn, vn); break;
                case T_I16:    run_bench<int32_t, int16_t>    (max_n, verbose, kn, vn); break;
                case T_U32:    run_bench<int32_t, uint32_t>   (max_n, verbose, kn, vn); break;
                case T_I32:    run_bench<int32_t, int32_t>    (max_n, verbose, kn, vn); break;
                case T_U64:    run_bench<int32_t, uint64_t>   (max_n, verbose, kn, vn); break;
                case T_I64:    run_bench<int32_t, int64_t>    (max_n, verbose, kn, vn); break;
                case T_STRING: run_bench<int32_t, std::string>(max_n, verbose, kn, vn); break;
                case T_BIG256: run_bench<int32_t, big256_t>   (max_n, verbose, kn, vn); break;
                default: goto bad_val;
            } break;
        case T_U64:
            switch (vt) {
                case T_BOOL:   run_bench<uint64_t, bool>       (max_n, verbose, kn, vn); break;
                case T_U8:     run_bench<uint64_t, uint8_t>    (max_n, verbose, kn, vn); break;
                case T_I8:     run_bench<uint64_t, int8_t>     (max_n, verbose, kn, vn); break;
                case T_U16:    run_bench<uint64_t, uint16_t>   (max_n, verbose, kn, vn); break;
                case T_I16:    run_bench<uint64_t, int16_t>    (max_n, verbose, kn, vn); break;
                case T_U32:    run_bench<uint64_t, uint32_t>   (max_n, verbose, kn, vn); break;
                case T_I32:    run_bench<uint64_t, int32_t>    (max_n, verbose, kn, vn); break;
                case T_U64:    run_bench<uint64_t, uint64_t>   (max_n, verbose, kn, vn); break;
                case T_I64:    run_bench<uint64_t, int64_t>    (max_n, verbose, kn, vn); break;
                case T_STRING: run_bench<uint64_t, std::string>(max_n, verbose, kn, vn); break;
                case T_BIG256: run_bench<uint64_t, big256_t>   (max_n, verbose, kn, vn); break;
                default: goto bad_val;
            } break;
        case T_I64:
            switch (vt) {
                case T_BOOL:   run_bench<int64_t, bool>       (max_n, verbose, kn, vn); break;
                case T_U8:     run_bench<int64_t, uint8_t>    (max_n, verbose, kn, vn); break;
                case T_I8:     run_bench<int64_t, int8_t>     (max_n, verbose, kn, vn); break;
                case T_U16:    run_bench<int64_t, uint16_t>   (max_n, verbose, kn, vn); break;
                case T_I16:    run_bench<int64_t, int16_t>    (max_n, verbose, kn, vn); break;
                case T_U32:    run_bench<int64_t, uint32_t>   (max_n, verbose, kn, vn); break;
                case T_I32:    run_bench<int64_t, int32_t>    (max_n, verbose, kn, vn); break;
                case T_U64:    run_bench<int64_t, uint64_t>   (max_n, verbose, kn, vn); break;
                case T_I64:    run_bench<int64_t, int64_t>    (max_n, verbose, kn, vn); break;
                case T_STRING: run_bench<int64_t, std::string>(max_n, verbose, kn, vn); break;
                case T_BIG256: run_bench<int64_t, big256_t>   (max_n, verbose, kn, vn); break;
                default: goto bad_val;
            } break;
        default:
            std::fprintf(stderr, "Invalid key type: %s\n", kn);
            return 1;
    }

    return 0;

bad_val:
    std::fprintf(stderr, "Invalid value type: %s\n", vn);
    return 1;
}
