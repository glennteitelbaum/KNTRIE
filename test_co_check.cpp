#include "kntrie_impl.hpp"
#include <cstdio>

using namespace gteitelbaum;

void walk(const uint64_t* node, int depth, int bits_remaining) {
    auto* h = get_header(node);
    int skip = h->skip();
    int bits_after_skip = bits_remaining - skip * 8;
    
    if (h->is_leaf()) {
        int st = h->suffix_type();
        int entries = h->entries();
        const char* st_name[] = {"bitmap(u8)", "u16", "u32", "u64"};
        int expected_st;
        if (bits_after_skip <= 8) expected_st = 0;
        else if (bits_after_skip <= 16) expected_st = 1;
        else if (bits_after_skip <= 32) expected_st = 2;
        else expected_st = 3;
        
        printf("  LEAF depth=%d skip=%d bits_remaining=%d bits_after_skip=%d "
               "st=%d(%s) expected_st=%d(%s) entries=%d %s\n",
               depth, skip, bits_remaining, bits_after_skip,
               st, st_name[st], expected_st, st_name[expected_st],
               entries, st == expected_st ? "OK" : "*** MISMATCH ***");
        return;
    }
    
    // bitmask
    int entries = h->entries();
    printf("  BITMASK depth=%d skip=%d bits_remaining=%d bits_after_skip=%d children=%d\n",
           depth, skip, bits_remaining, bits_after_skip, entries);
    
    using BO = bitmask_ops<uint64_t, std::allocator<uint64_t>>;
    BO::for_each_child(node, [&](uint8_t idx, int, uint64_t* child) {
        walk(child, depth + 1, bits_after_skip - 8);
    });
}

int main() {
    for (int n : {70000, 333000}) {
        printf("=== u64 sequential n=%d ===\n", n);
        kntrie_impl<uint64_t, uint64_t> t;
        for (int i = 0; i < n; ++i)
            t.insert(static_cast<uint64_t>(i), static_cast<uint64_t>(i));
        
        for (int ri = 0; ri < 256; ++ri) {
            auto* child = t.debug_root_child(ri);
            if (child == SENTINEL_NODE) continue;
            printf("root[%d]: bits_remaining=56\n", ri);
            walk(child, 1, 56);  // 64 - 8 root bits = 56
        }
        printf("\n");
    }
    return 0;
}
