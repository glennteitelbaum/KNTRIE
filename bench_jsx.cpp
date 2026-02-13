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
static Row bench_kntrie(const Workload<KEY>& w, size_t n, int fi,
                        const char* key_type, const std::string& pattern) {
    double best_fnd = 1e18, best_ins = 1e18, best_ers = 1e18;
    size_t mem = 0;

    for (int t = 0; t < TRIALS; ++t) {
        gteitelbaum::kntrie<KEY, uint64_t> trie;
        double t0 = now_ms();
        for (auto k : w.keys) trie.insert(k, static_cast<uint64_t>(k));
        best_ins = std::min(best_ins, now_ms() - t0);
        if (t == 0) mem = trie.memory_usage();

        uint64_t cs = 0;
        double t1 = now_ms();
        for (int r = 0; r < fi; ++r)
            for (auto k : w.find_keys) { auto* v = trie.find_value(k); cs += v ? *v : 0; }
        best_fnd = std::min(best_fnd, (now_ms() - t1) / fi);
        do_not_optimize(cs);

        double t2 = now_ms();
        for (auto k : w.erase_keys) trie.erase(k);
        best_ers = std::min(best_ers, now_ms() - t2);
    }
    return {key_type, pattern, n, "kntrie", best_fnd, best_ins, best_ers, mem};
}

template<typename KEY>
static Row bench_map(const Workload<KEY>& w, size_t n, int fi,
                     const char* key_type, const std::string& pattern) {
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

    for (int t = 0; t < TRIALS; ++t) {
        std::map<KEY, uint64_t> m;
        double t0 = now_ms();
        for (auto k : w.keys) m.emplace(k, static_cast<uint64_t>(k));
        best_ins = std::min(best_ins, now_ms() - t0);

        uint64_t cs = 0;
        double t1 = now_ms();
        for (int r = 0; r < fi; ++r)
            for (auto k : w.find_keys) { auto it = m.find(k); cs += (it != m.end()) ? it->second : 0; }
        best_fnd = std::min(best_fnd, (now_ms() - t1) / fi);
        do_not_optimize(cs);

        double t2 = now_ms();
        for (auto k : w.erase_keys) m.erase(k);
        best_ers = std::min(best_ers, now_ms() - t2);
    }
    return {key_type, pattern, n, "map", best_fnd, best_ins, best_ers, mem};
}

template<typename KEY>
static Row bench_umap(const Workload<KEY>& w, size_t n, int fi,
                      const char* key_type, const std::string& pattern) {
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

    for (int t = 0; t < TRIALS; ++t) {
        std::unordered_map<KEY, uint64_t> m;
        m.reserve(w.keys.size());
        double t0 = now_ms();
        for (auto k : w.keys) m.emplace(k, static_cast<uint64_t>(k));
        best_ins = std::min(best_ins, now_ms() - t0);

        uint64_t cs = 0;
        double t1 = now_ms();
        for (int r = 0; r < fi; ++r)
            for (auto k : w.find_keys) { auto it = m.find(k); cs += (it != m.end()) ? it->second : 0; }
        best_fnd = std::min(best_fnd, (now_ms() - t1) / fi);
        do_not_optimize(cs);

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

    rows.push_back(bench_kntrie(w, n, fi, key_type, pattern));
    if (n <= 1000000)
        rows.push_back(bench_map(w, n, fi, key_type, pattern));
    rows.push_back(bench_umap(w, n, fi, key_type, pattern));
}

// ==========================================================================
// JSX output — embeds data + full React component
// ==========================================================================

static void emit_jsx(const std::vector<Row>& rows) {
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

    // Static JSX template
    std::printf("%s",
R"JSX(import { useState } from "react";
import { LineChart, Line, XAxis, YAxis, CartesianGrid, Tooltip, ResponsiveContainer } from "recharts";

const LINES = [
  { key: "kntrie_int32", color: "#3b82f6", dash: "", label: "kntrie i32" },
  { key: "kntrie_uint64", color: "#93c5fd", dash: "6 3", label: "kntrie u64" },
  { key: "map_int32", color: "#ef4444", dash: "", label: "map i32" },
  { key: "map_uint64", color: "#fca5a5", dash: "6 3", label: "map u64" },
  { key: "umap_int32", color: "#22c55e", dash: "", label: "umap i32" },
  { key: "umap_uint64", color: "#86efac", dash: "6 3", label: "umap u64" },
  { key: "raw_int32", color: "#888", dash: "3 3", label: "raw i32", memOnly: true },
  { key: "raw_uint64", color: "#555", dash: "3 3", label: "raw u64", memOnly: true },
];

const METRICS = [
  { suffix: "find", label: "Find", unit: "ns/entry", convert: (ms, n) => (ms * 1e6) / n },
  { suffix: "insert", label: "Insert", unit: "ns/entry", convert: (ms, n) => (ms * 1e6) / n },
  { suffix: "erase", label: "Erase (N/2)", unit: "ns/entry", convert: (ms, n) => (ms * 1e6) / (n / 2) },
  { suffix: "mem", label: "Memory", unit: "B/entry", convert: (b, n) => b / n },
];

const PATTERNS = ["random", "sequential"];

function buildData(pattern, metric) {
  const lines = LINES.filter((l) => metric.suffix === "mem" || !l.memOnly);
  return RAW_DATA
    .filter((r) => r.pattern === pattern)
    .map((r) => {
      const out = { N: r.N };
      for (const line of lines) {
        if (line.key === "raw_int32") {
          if (metric.suffix === "mem") out[line.key] = metric.convert(r.N * 12, r.N);
        } else if (line.key === "raw_uint64") {
          if (metric.suffix === "mem") out[line.key] = metric.convert(r.N * 16, r.N);
        } else {
          const raw = r[`${line.key}_${metric.suffix}`];
          if (raw != null) out[line.key] = metric.convert(raw, r.N);
        }
      }
      return out;
    });
}

const fmtN = (v) => {
  if (v >= 1e6) return `${(v / 1e6).toFixed(0)}M`;
  if (v >= 1e3) return `${(v / 1e3).toFixed(0)}K`;
  return `${v}`;
};

const fmtVal = (v, unit) => {
  if (v == null) return "";
  if (v < 0.1) return `${v.toFixed(3)} ${unit}`;
  if (v < 10) return `${v.toFixed(2)} ${unit}`;
  if (v < 1000) return `${v.toFixed(1)} ${unit}`;
  return `${v.toFixed(0)} ${unit}`;
};

const Tip = ({ active, payload, label, unit }) => {
  if (!active || !payload?.length) return null;
  return (
    <div style={{ background: "#1a1a2e", border: "1px solid #444", borderRadius: 8, padding: "8px 12px", fontSize: 12 }}>
      <div style={{ color: "#aaa", marginBottom: 4, fontWeight: 600 }}>N = {fmtN(label)}</div>
      {payload.filter((p) => p.value != null).map((p) => (
        <div key={p.dataKey} style={{ color: p.color, marginBottom: 1 }}>
          {LINES.find((l) => l.key === p.dataKey)?.label}: {fmtVal(p.value, unit)}
        </div>
      ))}
    </div>
  );
};

const Chart = ({ title, data, unit }) => {
  const allVals = data.flatMap((d) => LINES.map((l) => d[l.key]).filter((v) => v != null && v > 0));
  if (!allVals.length) return null;
  const minY = Math.pow(10, Math.floor(Math.log10(Math.min(...allVals))));
  const maxY = Math.pow(10, Math.ceil(Math.log10(Math.max(...allVals))));

  return (
    <div style={{ marginBottom: 28 }}>
      <h3 style={{ margin: "0 0 6px 0", fontSize: 14, fontWeight: 600, color: "#ddd", textAlign: "center" }}>{title}</h3>
      <ResponsiveContainer width="100%" height={220}>
        <LineChart data={data} margin={{ top: 4, right: 16, left: 8, bottom: 4 }}>
          <CartesianGrid strokeDasharray="3 3" stroke="#2a2a3e" />
          <XAxis dataKey="N" scale="log" domain={["dataMin", "dataMax"]} type="number"
            tickFormatter={fmtN} tick={{ fill: "#888", fontSize: 11 }} stroke="#444" />
          <YAxis scale="log" domain={[minY, maxY]} type="number"
            tickFormatter={(v) => fmtVal(v, "")} tick={{ fill: "#888", fontSize: 10 }}
            stroke="#444" width={52} allowDataOverflow />
          <Tooltip content={<Tip unit={unit} />} />
          {LINES.map((l) => (
            <Line key={l.key} type="monotone" dataKey={l.key} stroke={l.color}
              strokeWidth={l.dash ? 1.5 : 2.5} dot={{ r: l.dash ? 2 : 3, fill: l.color }}
              strokeDasharray={l.dash || undefined} connectNulls />
          ))}
        </LineChart>
      </ResponsiveContainer>
    </div>
  );
};

export default function App() {
  return (
    <div style={{ background: "#0f0f1a", color: "#ddd", minHeight: "100vh", padding: "16px 12px", fontFamily: "system-ui, sans-serif" }}>
      <h2 style={{ margin: "0 0 4px 0", fontSize: 18, fontWeight: 700, textAlign: "center" }}>kntrie Benchmark</h2>
      <p style={{ textAlign: "center", color: "#777", fontSize: 12, margin: "0 0 12px 0" }}>
        Log-log · Per-entry · Lower is better · Solid = i32, Dashed = u64
      </p>

      <div style={{ display: "flex", justifyContent: "center", gap: 12, marginBottom: 16, flexWrap: "wrap" }}>
        {LINES.map((l) => (
          <div key={l.key} style={{ display: "flex", alignItems: "center", gap: 5, fontSize: 11 }}>
            <div style={{ width: 20, height: l.dash ? 0 : 2.5, background: l.color, borderRadius: 1,
              borderTop: l.dash ? `2px dashed ${l.color}` : "none" }} />
            <span style={{ color: "#bbb" }}>{l.label}</span>
          </div>
        ))}
      </div>

      <div style={{ maxWidth: 580, margin: "0 auto" }}>
        {PATTERNS.map((pat) => (
          <div key={pat}>
            <h3 style={{ fontSize: 16, fontWeight: 700, color: "#aac4ff", textAlign: "center",
              margin: "20px 0 12px 0", borderBottom: "1px solid #333", paddingBottom: 6 }}>
              {pat}
            </h3>
            {METRICS.map((m) => (
              <Chart key={`${pat}-${m.suffix}`} title={m.label}
                data={buildData(pat, m)} unit={m.unit} />
            ))}
          </div>
        ))}
      </div>
    </div>
  );
}
)JSX");
}

int main() {
    std::vector<size_t> sizes;
    for (double n = 100; n < 6000000; n *= 1.5)
        sizes.push_back(static_cast<size_t>(n));

    const char* patterns[] = {"random", "sequential"};
    std::vector<Row> rows;

    for (auto* pat : patterns) {
        for (auto n : sizes) {
            std::fprintf(stderr, "u64 %s N=%zu...\n", pat, n);
            bench_all<uint64_t>(n, pat, "uint64_t", rows);
        }
        for (auto n : sizes) {
            std::fprintf(stderr, "i32 %s N=%zu...\n", pat, n);
            bench_all<int32_t>(n, pat, "int32_t", rows);
        }
    }

    emit_jsx(rows);
    return 0;
}
