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

// ==========================================================================
// Tracking allocator — measures real heap usage for map/umap
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
        return static_cast<T*>(::operator new(bytes));
    }
    void deallocate(T* p, size_t n) noexcept {
        g_alloc_total -= n * sizeof(T);
        ::operator delete(p);
    }
    template<typename U> bool operator==(const TrackingAlloc<U>&) const noexcept { return true; }
};

static double now_ms() {
    using clk = std::chrono::high_resolution_clock;
    static auto t0 = clk::now();
    return std::chrono::duration<double, std::milli>(clk::now() - t0).count();
}

template<typename T>
static void do_not_optimize(T const& val) {
    asm volatile("" : : "r,m"(val) : "memory");
}

template<typename KEY>
struct Workload {
    std::vector<KEY> keys;
    std::vector<KEY> erase_keys;
    std::vector<KEY> find_keys;
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

    w.find_keys = raw;
    std::shuffle(w.find_keys.begin(), w.find_keys.end(), rng);
    return w;
}

static int iters_for(size_t n) {
    if      (n <= 200)      return 2000;
    else if (n <= 1000)     return 500;
    else if (n <= 10000)    return 50;
    else if (n <= 100000)   return 5;
    else                    return 1;
}

struct Row {
    const char* key_type;
    std::string pattern;
    size_t n;
    const char* container;
    double find_ms;
    double insert_ms;
    double erase_ms;
    size_t mem_bytes;
};

constexpr int TRIALS = 3;

template<typename KEY>
static Row bench_kntrie(Workload<KEY>& w, size_t n, int fi,
                        const char* key_type, const std::string& pattern,
                        std::mt19937_64& rng) {
    double best_fnd = 1e18, best_ins = 1e18, best_ers = 1e18;
    size_t mem = 0;

    // Pre-generate shuffled find orders
    std::vector<std::vector<KEY>> find_orders(fi);
    for (int r = 0; r < fi; ++r) {
        find_orders[r] = w.find_keys;
        std::shuffle(find_orders[r].begin(), find_orders[r].end(), rng);
    }

    for (int t = 0; t < TRIALS; ++t) {
        std::shuffle(w.keys.begin(), w.keys.end(), rng);
        gteitelbaum::kntrie<KEY, uint64_t> trie;
        double t0 = now_ms();
        for (auto k : w.keys) trie.insert(k, static_cast<uint64_t>(k));
        best_ins = std::min(best_ins, now_ms() - t0);
        if (t == 0) mem = trie.memory_usage();

        uint64_t cs = 0;
        double t1 = now_ms();
        for (int r = 0; r < fi; ++r)
            for (auto k : find_orders[r]) { auto* v = trie.find_value(k); cs += v ? *v : 0; }
        best_fnd = std::min(best_fnd, (now_ms() - t1) / fi);
        do_not_optimize(cs);

        std::shuffle(w.erase_keys.begin(), w.erase_keys.end(), rng);
        double t2 = now_ms();
        for (auto k : w.erase_keys) trie.erase(k);
        best_ers = std::min(best_ers, now_ms() - t2);
    }
    return {key_type, pattern, n, "kntrie", best_fnd, best_ins, best_ers, mem};
}

template<typename KEY>
static Row bench_map(Workload<KEY>& w, size_t n, int fi,
                     const char* key_type, const std::string& pattern,
                     std::mt19937_64& rng) {
    double best_fnd = 1e18, best_ins = 1e18, best_ers = 1e18;

    // Memory: one tracked run
    size_t mem;
    {
        using MapT = std::map<KEY, uint64_t, std::less<KEY>,
                              TrackingAlloc<std::pair<const KEY, uint64_t>>>;
        g_alloc_total = 0;
        MapT m;
        for (auto k : w.keys) m.emplace(k, static_cast<uint64_t>(k));
        mem = g_alloc_total;
    }

    // Pre-generate shuffled find orders
    std::vector<std::vector<KEY>> find_orders(fi);
    for (int r = 0; r < fi; ++r) {
        find_orders[r] = w.find_keys;
        std::shuffle(find_orders[r].begin(), find_orders[r].end(), rng);
    }

    for (int t = 0; t < TRIALS; ++t) {
        std::shuffle(w.keys.begin(), w.keys.end(), rng);
        std::map<KEY, uint64_t> m;
        double t0 = now_ms();
        for (auto k : w.keys) m.emplace(k, static_cast<uint64_t>(k));
        best_ins = std::min(best_ins, now_ms() - t0);

        uint64_t cs = 0;
        double t1 = now_ms();
        for (int r = 0; r < fi; ++r)
            for (auto k : find_orders[r]) { auto it = m.find(k); cs += (it != m.end()) ? it->second : 0; }
        best_fnd = std::min(best_fnd, (now_ms() - t1) / fi);
        do_not_optimize(cs);

        std::shuffle(w.erase_keys.begin(), w.erase_keys.end(), rng);
        double t2 = now_ms();
        for (auto k : w.erase_keys) m.erase(k);
        best_ers = std::min(best_ers, now_ms() - t2);
    }
    return {key_type, pattern, n, "map", best_fnd, best_ins, best_ers, mem};
}

template<typename KEY>
static Row bench_umap(Workload<KEY>& w, size_t n, int fi,
                      const char* key_type, const std::string& pattern,
                      std::mt19937_64& rng) {
    double best_fnd = 1e18, best_ins = 1e18, best_ers = 1e18;

    // Memory: one tracked run
    size_t mem;
    {
        using UMapT = std::unordered_map<KEY, uint64_t, std::hash<KEY>, std::equal_to<KEY>,
                                          TrackingAlloc<std::pair<const KEY, uint64_t>>>;
        g_alloc_total = 0;
        UMapT m;
        m.reserve(w.keys.size());
        for (auto k : w.keys) m.emplace(k, static_cast<uint64_t>(k));
        mem = g_alloc_total;
    }

    // Pre-generate shuffled find orders
    std::vector<std::vector<KEY>> find_orders(fi);
    for (int r = 0; r < fi; ++r) {
        find_orders[r] = w.find_keys;
        std::shuffle(find_orders[r].begin(), find_orders[r].end(), rng);
    }

    for (int t = 0; t < TRIALS; ++t) {
        std::shuffle(w.keys.begin(), w.keys.end(), rng);
        std::unordered_map<KEY, uint64_t> m;
        m.reserve(w.keys.size());
        double t0 = now_ms();
        for (auto k : w.keys) m.emplace(k, static_cast<uint64_t>(k));
        best_ins = std::min(best_ins, now_ms() - t0);

        uint64_t cs = 0;
        double t1 = now_ms();
        for (int r = 0; r < fi; ++r)
            for (auto k : find_orders[r]) { auto it = m.find(k); cs += (it != m.end()) ? it->second : 0; }
        best_fnd = std::min(best_fnd, (now_ms() - t1) / fi);
        do_not_optimize(cs);

        std::shuffle(w.erase_keys.begin(), w.erase_keys.end(), rng);
        double t2 = now_ms();
        for (auto k : w.erase_keys) m.erase(k);
        best_ers = std::min(best_ers, now_ms() - t2);
    }
    return {key_type, pattern, n, "umap", best_fnd, best_ins, best_ers, mem};
}

template<typename KEY>
static void bench_all(size_t target_n, const std::string& pattern,
                      const char* key_type, std::vector<Row>& rows) {
    std::mt19937_64 rng(42);
    int fi = iters_for(target_n);
    auto w = make_workload<KEY>(target_n, pattern, fi, rng);
    size_t n = w.keys.size();

    rows.push_back(bench_kntrie(w, n, fi, key_type, pattern, rng));
    if (n <= 1000000)
        rows.push_back(bench_map(w, n, fi, key_type, pattern, rng));
    rows.push_back(bench_umap(w, n, fi, key_type, pattern, rng));
}

// ==========================================================================
// HTML output — self-contained page with React + Recharts from CDN
// ==========================================================================

static void emit_html(const std::vector<Row>& rows) {
    // Group by (pattern, N) — merge int32 and uint64 into one object
    struct DataPoint {
        std::string pattern;
        size_t N;
        double vals[6][4]; // [container*keytype] x [find, insert, erase, mem]
        bool has[6];
    };

    // container+keytype -> index: kntrie_i32=0, kntrie_u64=1, map_i32=2, map_u64=3, umap_i32=4, umap_u64=5
    auto cidx = [](const char* c, const char* kt) -> int {
        int base = 0;
        if (std::strcmp(c, "map") == 0) base = 2;
        else if (std::strcmp(c, "umap") == 0) base = 4;
        return base + (std::strstr(kt, "uint64") ? 1 : 0);
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
        int ci = cidx(r.container, r.key_type);
        points[pi].vals[ci][0] = r.find_ms;
        points[pi].vals[ci][1] = r.insert_ms;
        points[pi].vals[ci][2] = r.erase_ms;
        points[pi].vals[ci][3] = static_cast<double>(r.mem_bytes);
        points[pi].has[ci] = true;
    }

    std::sort(points.begin(), points.end(), [](const DataPoint& a, const DataPoint& b) {
        if (a.pattern != b.pattern) return a.pattern < b.pattern;
        return a.N < b.N;
    });

    const char* names[] = {"kntrie_int32", "kntrie_uint64", "map_int32", "map_uint64", "umap_int32", "umap_uint64"};
    const char* suffixes[] = {"find", "insert", "erase", "mem"};

    // HTML preamble — Chart.js from CDN, zero framework deps
    std::printf("%s", R"HTML(<!DOCTYPE html>
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
  <h2>kntrie Benchmark</h2>
  <p class="sub">Log-log · Per-entry · Lower is better · Solid = i32, Dashed = u64</p>
  <div class="btns">
    <button class="active" onclick="show('random')">random</button>
    <button onclick="show('sequential')">sequential</button>
  </div>
  <div class="chart-box"><h3>Find (ns/entry)</h3><canvas id="c_find"></canvas></div>
  <div class="chart-box"><h3>Insert (ns/entry)</h3><canvas id="c_insert"></canvas></div>
  <div class="chart-box"><h3>Erase N/2 (ns/entry)</h3><canvas id="c_erase"></canvas></div>
  <div class="chart-box"><h3>Memory (B/entry)</h3><canvas id="c_mem"></canvas></div>
</div>
<script>
)HTML");

    // Emit data blob
    std::printf("const RAW_DATA = [\n");
    for (auto& p : points) {
        std::printf("  {pattern:\"%s\",N:%zu", p.pattern.c_str(), p.N);
        for (int ci = 0; ci < 6; ++ci) {
            if (!p.has[ci]) continue;
            for (int mi = 0; mi < 4; ++mi) {
                if (mi == 3)
                    std::printf(",%s_%s:%.0f", names[ci], suffixes[mi], p.vals[ci][mi]);
                else
                    std::printf(",%s_%s:%.4f", names[ci], suffixes[mi], p.vals[ci][mi]);
            }
        }
        std::printf("},\n");
    }
    std::printf("];\n\n");

    // Chart.js template — plain JS, no framework
    std::printf("%s",
R"JS(
const LINES = [
  { key: "kntrie_int32", color: "#3b82f6", dash: [], width: 2.5, label: "kntrie i32" },
  { key: "kntrie_uint64", color: "#93c5fd", dash: [6,3], width: 1.5, label: "kntrie u64" },
  { key: "map_int32", color: "#ef4444", dash: [], width: 2.5, label: "map i32" },
  { key: "map_uint64", color: "#fca5a5", dash: [6,3], width: 1.5, label: "map u64" },
  { key: "umap_int32", color: "#22c55e", dash: [], width: 2.5, label: "umap i32" },
  { key: "umap_uint64", color: "#86efac", dash: [6,3], width: 1.5, label: "umap u64" },
];

const MEM_LINES = [
  ...LINES,
  { key: "raw_int32", color: "#888", dash: [], width: 1, label: "raw i32" },
  { key: "raw_uint64", color: "#555", dash: [3,3], width: 1, label: "raw u64" },
];

const METRICS = [
  { id: "find", suffix: "find", convert: (ms, n) => (ms * 1e6) / n },
  { id: "insert", suffix: "insert", convert: (ms, n) => (ms * 1e6) / n },
  { id: "erase", suffix: "erase", convert: (ms, n) => (ms * 1e6) / (n / 2) },
  { id: "mem", suffix: "mem", convert: (b, n) => b / n },
];

function buildData(pattern, metric) {
  const lines = metric.id === "mem" ? MEM_LINES : LINES;
  return RAW_DATA
    .filter(r => r.pattern === pattern)
    .map(r => {
      const pt = { N: r.N };
      for (const l of lines) {
        if (l.key === "raw_int32") { pt[l.key] = 12; continue; }
        if (l.key === "raw_uint64") { pt[l.key] = 16; continue; }
        const raw = r[l.key + "_" + metric.suffix];
        if (raw != null) pt[l.key] = metric.convert(raw, r.N);
      }
      return pt;
    });
}

const charts = {};

function makeChart(canvasId, metric) {
  const ctx = document.getElementById(canvasId).getContext("2d");
  const lines = metric.id === "mem" ? MEM_LINES : LINES;
  const data = buildData("random", metric);

  charts[canvasId] = new Chart(ctx, {
    type: "line",
    data: {
      labels: data.map(d => d.N),
      datasets: lines.map(l => ({
        label: l.label,
        data: data.map(d => d[l.key] ?? null),
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
    const lines = m.id === "mem" ? MEM_LINES : LINES;
    const data = buildData(pattern, m);
    chart.data.labels = data.map(d => d.N);
    lines.forEach((l, i) => {
      chart.data.datasets[i].data = data.map(d => d[l.key] ?? null);
    });
    chart.update("none");
  }
}
)JS");

    std::printf("</script>\n</body>\n</html>\n");
}

int main() {
    std::vector<size_t> sizes;
    constexpr double MAX_N = 6000000;  // change to 10000 for quick test
    for (double n = 100; n < MAX_N; n *= 1.5)
        sizes.push_back(static_cast<size_t>(n));

    auto capped_sizes = [&](auto key_tag) {
        using KEY = decltype(key_tag);
        size_t max_n = static_cast<size_t>(std::numeric_limits<KEY>::max());
        std::vector<size_t> result;
        for (auto n : sizes) {
            if (n >= max_n) {
                result.push_back(max_n);
                break;
            }
            result.push_back(n);
        }
        return result;
    };

    const char* patterns[] = {"random", "sequential"};
    std::vector<Row> rows;

    for (auto* pat : patterns) {
        for (auto n : sizes) {
            std::fprintf(stderr, "u64 %s N=%zu...\n", pat, n);
            bench_all<uint64_t>(n, pat, "uint64_t", rows);
        }
        for (auto n : capped_sizes(int32_t{})) {
            std::fprintf(stderr, "i32 %s N=%zu...\n", pat, n);
            bench_all<int32_t>(n, pat, "int32_t", rows);
        }
    }

    emit_html(rows);
    return 0;
}
