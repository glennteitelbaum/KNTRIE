#include "kntrie_impl.hpp"
#include <cstdio>

using namespace gteitelbaum;
using BO = bitmask_ops<uint64_t, std::allocator<uint64_t>>;

void dump_node(const uint64_t* node, int depth, int max_depth) {
    auto* h = get_header(node);
    int skip = h->skip();
    int entries = h->entries();
    int st = h->suffix_type();
    int alloc = h->alloc_u64();
    
    if (h->is_leaf()) {
        printf("  %*s LEAF skip=%d st=%d entries=%d alloc=%d\n",
               depth*2, "", skip, st, entries, alloc);
        return;
    }
    
    printf("  %*s BITMASK skip=%d children=%d alloc=%d",
           depth*2, "", skip, entries, alloc);
    if (skip > 0) {
        printf(" prefix=");
        const uint8_t* p = h->prefix_bytes();
        for (int i = 0; i < skip; ++i) printf("%02x", p[i]);
    }
    printf("\n");
    
    if (depth < max_depth) {
        int shown = 0;
        BO::for_each_child(node, [&](uint8_t idx, int, uint64_t* child) {
            if (shown < 3) {
                printf("  %*s  [%02x]:\n", depth*2, "", idx);
                dump_node(child, depth + 1, max_depth);
                shown++;
            } else if (shown == 3) {
                printf("  %*s  ... (%d more)\n", depth*2, "", entries - 3);
                shown++;
            }
        });
    }
}

template<typename KEY>
void analyze(const char* label) {
    printf("=== %s sequential n=4000000 ===\n", label);
    kntrie_impl<KEY, uint64_t> t;
    for (int i = 0; i < 4000000; ++i)
        t.insert(static_cast<KEY>(i), static_cast<uint64_t>(i));
    
    for (int ri = 0; ri < 256; ++ri) {
        auto* child = t.debug_root_child(ri);
        if (child == SENTINEL_NODE) continue;
        printf("root[%d]:\n", ri);
        dump_node(child, 0, 3);
    }
    printf("\n");
}

int main() {
    analyze<uint64_t>("u64");
    analyze<int32_t>("i32");
    return 0;
}
