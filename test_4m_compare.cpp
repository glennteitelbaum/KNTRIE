#include "kntrie_impl.hpp"
#include <cstdio>
#include <map>
#include <vector>
#include <algorithm>

using namespace gteitelbaum;

struct LevelInfo {
    std::map<int,int> leaf_sizes;   // entries -> count
    int bitmask_count = 0;
    int leaf_count = 0;
    int total_entries = 0;
};

template<typename KEY>
void walk(const kntrie_impl<KEY, uint64_t>& t, const uint64_t* node, 
          int depth, std::vector<LevelInfo>& levels) {
    if (depth >= (int)levels.size()) levels.resize(depth + 1);
    auto* h = get_header(node);
    int skip = h->skip();
    
    if (h->is_leaf()) {
        int entries = h->entries();
        levels[depth].leaf_count++;
        levels[depth].leaf_sizes[entries]++;
        levels[depth].total_entries += entries;
        return;
    }
    
    levels[depth].bitmask_count++;
    using BO = bitmask_ops<uint64_t, std::allocator<uint64_t>>;
    int children = h->entries();
    BO::for_each_child(node, [&](uint8_t idx, int, uint64_t* child) {
        walk(t, child, depth + 1, levels);
    });
}

template<typename KEY>
void analyze(const char* label, int n) {
    printf("=== %s sequential n=%d ===\n", label, n);
    kntrie_impl<KEY, uint64_t> t;
    for (int i = 0; i < n; ++i)
        t.insert(static_cast<KEY>(i), static_cast<uint64_t>(i));
    
    auto s = t.debug_stats();
    printf("mem=%zu (%.1f B/e) bm=%zu cl=%zu bl=%zu\n",
           s.total_bytes, double(s.total_bytes)/n,
           s.bitmask_nodes, s.compact_leaves, s.bitmap_leaves);
    
    std::vector<LevelInfo> levels;
    for (int ri = 0; ri < 256; ++ri) {
        auto* child = t.debug_root_child(ri);
        if (child == SENTINEL_NODE) continue;
        walk(t, child, 0, levels);
    }
    
    for (int d = 0; d < (int)levels.size(); ++d) {
        auto& L = levels[d];
        if (L.bitmask_count == 0 && L.leaf_count == 0) continue;
        printf("  depth %d: %d bitmask, %d leaves (%d entries)\n",
               d, L.bitmask_count, L.leaf_count, L.total_entries);
        // Show top-5 leaf size buckets
        std::vector<std::pair<int,int>> sz(L.leaf_sizes.begin(), L.leaf_sizes.end());
        std::sort(sz.begin(), sz.end(), [](auto&a, auto&b){ return a.second > b.second; });
        for (int i = 0; i < std::min(5, (int)sz.size()); ++i)
            printf("    entries=%d: %d leaves\n", sz[i].first, sz[i].second);
    }
    printf("\n");
}

int main() {
    analyze<uint64_t>("u64", 4000000);
    analyze<int32_t>("i32", 4000000);
    return 0;
}
