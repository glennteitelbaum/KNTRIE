#include "kntrie_impl.hpp"
#include <cstdio>
#include <map>
#include <vector>
#include <algorithm>

using namespace gteitelbaum;
using BO = bitmask_ops<uint64_t, std::allocator<uint64_t>>;

struct TreeInfo {
    std::map<int,int> leaf_depth_count;  // depth -> count
    std::map<int,int> leaf_size_hist;    // entries -> count
    int max_depth = 0;
    int bitmask_nodes = 0;
};

void walk(const uint64_t* node, int depth, TreeInfo& info, bool verbose) {
    auto* h = get_header(node);
    int skip = h->skip();
    int effective_depth = depth + skip;
    
    if (h->is_leaf()) {
        int st = h->suffix_type();
        int entries = h->entries();
        info.leaf_depth_count[effective_depth]++;
        info.leaf_size_hist[entries]++;
        if (effective_depth > info.max_depth) info.max_depth = effective_depth;
        if (verbose && entries > 100)
            printf("    leaf depth=%d(+%d skip) st=%d entries=%d alloc=%d\n",
                   depth, skip, st, entries, h->alloc_u64());
        return;
    }
    
    // bitmask
    info.bitmask_nodes++;
    int entries = h->entries();
    if (verbose)
        printf("    bitmask depth=%d(+%d skip) children=%d alloc=%d\n",
               depth, skip, entries, h->alloc_u64());
    
    BO::for_each_child(node, [&](uint8_t idx, int, uint64_t* child) {
        walk(child, effective_depth + 1, info, verbose);
    });
}

void analyze(int n) {
    printf("=== u64 sequential n=%d ===\n", n);
    kntrie_impl<uint64_t, uint64_t> t;
    for (int i = 0; i < n; ++i)
        t.insert(static_cast<uint64_t>(i), static_cast<uint64_t>(i));
    
    auto s = t.debug_stats();
    printf("mem=%zu (%.1f B/e) bm=%zu cl=%zu bl=%zu\n",
           s.total_bytes, double(s.total_bytes)/n,
           s.bitmask_nodes, s.compact_leaves, s.bitmap_leaves);
    
    TreeInfo info;
    bool verbose = (n <= 100000);
    
    for (int ri = 0; ri < 256; ++ri) {
        auto* child = t.debug_root_child(ri);
        if (child == SENTINEL_NODE) continue;
        printf("  root[%d]:\n", ri);
        walk(child, 1, info, verbose);
    }
    
    printf("  max_depth=%d bitmask_nodes=%d\n", info.max_depth, info.bitmask_nodes);
    printf("  leaf depth distribution:\n");
    for (auto& [d, c] : info.leaf_depth_count)
        printf("    depth %d: %d leaves\n", d, c);
    printf("  leaf size histogram (top 10):\n");
    std::vector<std::pair<int,int>> sizes(info.leaf_size_hist.begin(), info.leaf_size_hist.end());
    std::sort(sizes.begin(), sizes.end(), [](auto& a, auto& b){ return a.second > b.second; });
    for (int i = 0; i < std::min(10, (int)sizes.size()); ++i)
        printf("    entries=%d: %d leaves\n", sizes[i].first, sizes[i].second);
    printf("\n");
}

int main() {
    analyze(70000);
    analyze(333000);
    return 0;
}
