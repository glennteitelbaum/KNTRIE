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

using KEY = uint64_t;

struct Workload {
    std::vector<KEY> keys;          // insert keys
    std::vector<KEY> erase_keys;    // N/2 keys to erase
    std::vector<KEY> find_fnd;      // N keys, 100% found, shuffled
    std::vector<KEY> find_mix;      // N+N/4 keys shuffled, first N used (75% hit)
    int find_iters;
};

static Workload make_workload(size_t n, const std::string& pattern,
                               int find_iters, std::mt19937_64& rng) {
    Workload w;
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

    // FND: 100% hit
    w.find_fnd = raw;
    std::shuffle(w.find_fnd.begin(), w.find_fnd.end(), rng);

    // MIX: N + N/4 keys, shuffled, use first N for lookup
    size_t extra = n / 4;
    w.find_mix = raw;
    if (pattern == "sequential") {
        for (size_t i = 0; i < extra; ++i)
            w.find_mix.push_back(static_cast<KEY>(n + i));
    } else {
        std::set<KEY> existing(raw.begin(), raw.end());
        while (w.find_mix.size() < n + extra) {
            KEY k = static_cast<KEY>(rng());
            if (!existing.count(k)) {
                w.find_mix.push_back(k);
                existing.insert(k);
            }
        }
    }
    std::shuffle(w.find_mix.begin(), w.find_mix.end(), rng);
    w.find_mix.resize(n);  // use first N

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
    std::string pattern;
    size_t n;
    const char* container;
    double find_fnd_ms;
    double find_mix_ms;
    double insert_ms;
    double erase_ms;
    size_t mem_bytes;
};

constexpr int TRIALS = 3;

static void bench_all(size_t target_n, const std::string& pattern,
                      std::vector<Row>& rows) {
    std::mt19937_64 rng(42);
    int fi = iters_for(target_n);
    auto w = make_workload(target_n, pattern, fi, rng);
    size_t n = w.keys.size();
    bool do_map = (n <= 1000000);

    // Memory: one tracked run per container
    size_t kntrie_mem, map_mem = 0, umap_mem;
    {
        using TrieT = gteitelbaum::kntrie<KEY, uint64_t, TrackingAlloc<uint64_t>>;
        g_alloc_total = 0;
        TrieT trie;
        for (auto k : w.keys) trie.insert(k, static_cast<uint64_t>(k));
        kntrie_mem = g_alloc_total;
    }
    if (do_map) {
        using MapT = std::map<KEY, uint64_t, std::less<KEY>,
                              TrackingAlloc<std::pair<const KEY, uint64_t>>>;
        g_alloc_total = 0;
        MapT m;
        for (auto k : w.keys) m.emplace(k, static_cast<uint64_t>(k));
        map_mem = g_alloc_total;
    }
    {
        using UMapT = std::unordered_map<KEY, uint64_t, std::hash<KEY>, std::equal_to<KEY>,
                                          TrackingAlloc<std::pair<const KEY, uint64_t>>>;
        g_alloc_total = 0;
        UMapT m;
        m.reserve(w.keys.size());
        for (auto k : w.keys) m.emplace(k, static_cast<uint64_t>(k));
        umap_mem = g_alloc_total;
    }

    // Pre-generate find orders (shared across containers)
    std::vector<std::vector<KEY>> fnd_orders(fi), mix_orders(fi);
    for (int r = 0; r < fi; ++r) {
        fnd_orders[r] = w.find_fnd;
        std::shuffle(fnd_orders[r].begin(), fnd_orders[r].end(), rng);
        mix_orders[r] = w.find_mix;
        std::shuffle(mix_orders[r].begin(), mix_orders[r].end(), rng);
    }

    double k_fnd = 1e18, k_mix = 1e18, k_ins = 1e18, k_ers = 1e18;
    double m_fnd = 1e18, m_mix = 1e18, m_ins = 1e18, m_ers = 1e18;
    double u_fnd = 1e18, u_mix = 1e18, u_ins = 1e18, u_ers = 1e18;

    for (int t = 0; t < TRIALS; ++t) {
        // --- kntrie ---
        std::shuffle(w.keys.begin(), w.keys.end(), rng);
        {
            gteitelbaum::kntrie<KEY, uint64_t> trie;
            double t0 = now_ms();
            for (auto k : w.keys) trie.insert(k, static_cast<uint64_t>(k));
            k_ins = std::min(k_ins, now_ms() - t0);

            uint64_t cs = 0;
            double t1 = now_ms();
            for (int r = 0; r < fi; ++r)
                for (auto k : fnd_orders[r]) { auto* v = trie.find_value(k); cs += v ? *v : 0; }
            k_fnd = std::min(k_fnd, (now_ms() - t1) / fi);
            do_not_optimize(cs);

            cs = 0;
            double t1m = now_ms();
            for (int r = 0; r < fi; ++r)
                for (auto k : mix_orders[r]) { auto* v = trie.find_value(k); cs += v ? *v : 0; }
            k_mix = std::min(k_mix, (now_ms() - t1m) / fi);
            do_not_optimize(cs);

            std::shuffle(w.erase_keys.begin(), w.erase_keys.end(), rng);
            double t2 = now_ms();
            for (auto k : w.erase_keys) trie.erase(k);
            k_ers = std::min(k_ers, now_ms() - t2);
        }

        // --- map ---
        if (do_map) {
            std::shuffle(w.keys.begin(), w.keys.end(), rng);
            std::map<KEY, uint64_t> m;
            double t0 = now_ms();
            for (auto k : w.keys) m.emplace(k, static_cast<uint64_t>(k));
            m_ins = std::min(m_ins, now_ms() - t0);

            uint64_t cs = 0;
            double t1 = now_ms();
            for (int r = 0; r < fi; ++r)
                for (auto k : fnd_orders[r]) { auto it = m.find(k); cs += (it != m.end()) ? it->second : 0; }
            m_fnd = std::min(m_fnd, (now_ms() - t1) / fi);
            do_not_optimize(cs);

            cs = 0;
            double t1m = now_ms();
            for (int r = 0; r < fi; ++r)
                for (auto k : mix_orders[r]) { auto it = m.find(k); cs += (it != m.end()) ? it->second : 0; }
            m_mix = std::min(m_mix, (now_ms() - t1m) / fi);
            do_not_optimize(cs);

            std::shuffle(w.erase_keys.begin(), w.erase_keys.end(), rng);
            double t2 = now_ms();
            for (auto k : w.erase_keys) m.erase(k);
            m_ers = std::min(m_ers, now_ms() - t2);
        }

        // --- umap ---
        std::shuffle(w.keys.begin(), w.keys.end(), rng);
        {
            std::unordered_map<KEY, uint64_t> m;
            m.reserve(w.keys.size());
            double t0 = now_ms();
            for (auto k : w.keys) m.emplace(k, static_cast<uint64_t>(k));
            u_ins = std::min(u_ins, now_ms() - t0);

            uint64_t cs = 0;
            double t1 = now_ms();
            for (int r = 0; r < fi; ++r)
                for (auto k : fnd_orders[r]) { auto it = m.find(k); cs += (it != m.end()) ? it->second : 0; }
            u_fnd = std::min(u_fnd, (now_ms() - t1) / fi);
            do_not_optimize(cs);

            cs = 0;
            double t1m = now_ms();
            for (int r = 0; r < fi; ++r)
                for (auto k : mix_orders[r]) { auto it = m.find(k); cs += (it != m.end()) ? it->second : 0; }
            u_mix = std::min(u_mix, (now_ms() - t1m) / fi);
            do_not_optimize(cs);

            std::shuffle(w.erase_keys.begin(), w.erase_keys.end(), rng);
            double t2 = now_ms();
            for (auto k : w.erase_keys) m.erase(k);
            u_ers = std::min(u_ers, now_ms() - t2);
        }
    }

    rows.push_back({pattern, n, "kntrie", k_fnd, k_mix, k_ins, k_ers, kntrie_mem});
    if (do_map)
        rows.push_back({pattern, n, "map", m_fnd, m_mix, m_ins, m_ers, map_mem});
    rows.push_back({pattern, n, "umap", u_fnd, u_mix, u_ins, u_ers, umap_mem});
}

// ==========================================================================
// HTML output — self-contained page with Chart.js from CDN
// ==========================================================================

static void emit_html(const std::vector<Row>& rows) {
    struct DataPoint {
        std::string pattern;
        size_t N;
        double vals[3][5]; // [kntrie=0, map=1, umap=2] x [fnd, mix, insert, erase, mem]
        bool has[3];
    };

    auto cidx = [](const char* c) -> int {
        if (std::strcmp(c, "kntrie") == 0) return 0;
        if (std::strcmp(c, "map") == 0) return 1;
        return 2; // umap
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
        points[pi].vals[ci][1] = r.find_mix_ms;
        points[pi].vals[ci][2] = r.insert_ms;
        points[pi].vals[ci][3] = r.erase_ms;
        points[pi].vals[ci][4] = static_cast<double>(r.mem_bytes);
        points[pi].has[ci] = true;
    }

    std::sort(points.begin(), points.end(), [](const DataPoint& a, const DataPoint& b) {
        if (a.pattern != b.pattern) return a.pattern < b.pattern;
        return a.N < b.N;
    });

    const char* names[] = {"kntrie", "map", "umap"};
    const char* suffixes[] = {"fnd", "mix", "insert", "erase", "mem"};

    // HTML preamble
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
  <h2>kntrie Benchmark (u64)</h2>
  <p class="sub">Log-log · Per-entry · Lower is better · FND=100% hit, MIX=75% hit</p>
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
        for (int ci = 0; ci < 3; ++ci) {
            if (!p.has[ci]) continue;
            for (int mi = 0; mi < 5; ++mi) {
                if (mi == 4)
                    std::printf(",%s_%s:%.0f", names[ci], suffixes[mi], p.vals[ci][mi]);
                else
                    std::printf(",%s_%s:%.4f", names[ci], suffixes[mi], p.vals[ci][mi]);
            }
        }
        std::printf("},\n");
    }
    std::printf("];\n\n");

    // Chart.js template
    std::printf("%s",
R"JS(
const LINES_FIND = [
  { key: "kntrie", suffix: "fnd", color: "#3b82f6", dash: [],    width: 2.5, label: "kntrie FND" },
  { key: "kntrie", suffix: "mix", color: "#93c5fd", dash: [6,3], width: 1.5, label: "kntrie MIX" },
  { key: "map",    suffix: "fnd", color: "#ef4444", dash: [],    width: 2.5, label: "map FND" },
  { key: "map",    suffix: "mix", color: "#fca5a5", dash: [6,3], width: 1.5, label: "map MIX" },
  { key: "umap",   suffix: "fnd", color: "#22c55e", dash: [],    width: 2.5, label: "umap FND" },
  { key: "umap",   suffix: "mix", color: "#86efac", dash: [6,3], width: 1.5, label: "umap MIX" },
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

const METRICS = [
  { id: "find",   lines: LINES_FIND,  convert: (ms, n) => (ms * 1e6) / n },
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

int main(int argc, char* argv[]) {
    std::vector<size_t> sizes;
    double MAX_N = 3000000;
    if (argc > 1) MAX_N = std::atof(argv[1]);
    for (double n = 100; n < MAX_N; n *= 1.5)
        sizes.push_back(static_cast<size_t>(n));

    const char* patterns[] = {"random", "sequential"};
    std::vector<Row> rows;

    for (auto* pat : patterns) {
        for (auto n : sizes) {
            std::fprintf(stderr, "u64 %s N=%zu...\n", pat, n);
            bench_all(n, pat, rows);
        }
    }

    emit_html(rows);
    return 0;
}
