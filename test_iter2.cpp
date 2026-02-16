#include "kntrie.hpp"
#include <cstdio>
#include <cstdlib>
#include <set>

int fails = 0;
void check(bool c, const char* msg, int line) {
    if (!c) { std::printf("  FAIL line %d: %s\n", line, msg); ++fails; }
}
#define CHECK(c) check((c), #c, __LINE__)

void test_api() {
    std::printf("api (find/lower/upper/rbegin)...\n");
    gteitelbaum::kntrie<uint64_t, uint64_t> t;
    CHECK(t.begin() == t.end());

    t.insert(10, 1); t.insert(20, 2); t.insert(30, 3);

    // begin/end forward
    auto it = t.begin();
    CHECK(it.key() == 10); ++it;
    CHECK(it.key() == 20); ++it;
    CHECK(it.key() == 30); ++it;
    CHECK(it == t.end());

    // rbegin backward
    it = t.rbegin();
    CHECK(it.key() == 30);
    it = t.begin();
    // -- from begin should go invalid
    // (prev of min = not found)

    // find
    CHECK(t.find(20) != t.end() && t.find(20).key() == 20);
    CHECK(t.find(15) == t.end());

    // lower_bound
    CHECK(t.lower_bound(10).key() == 10);
    CHECK(t.lower_bound(15).key() == 20);
    CHECK(t.lower_bound(20).key() == 20);
    CHECK(t.lower_bound(31) == t.end());

    // upper_bound
    CHECK(t.upper_bound(9).key() == 10);
    CHECK(t.upper_bound(10).key() == 20);
    CHECK(t.upper_bound(20).key() == 30);
    CHECK(t.upper_bound(30) == t.end());
}

void test_erase_iterate() {
    std::printf("erase+iterate...\n");
    gteitelbaum::kntrie<uint64_t, uint64_t> t;
    std::set<uint64_t> ref;
    srand(99999);
    for (int i = 0; i < 10000; ++i) {
        uint64_t k = (uint64_t(rand()) << 32) | rand();
        t.insert(k, uint64_t(i));
        ref.insert(k);
    }
    // Erase half
    auto ri = ref.begin();
    int count = 0;
    while (ri != ref.end()) {
        if (count++ % 2 == 0) {
            t.erase(*ri);
            ri = ref.erase(ri);
        } else { ++ri; }
    }
    CHECK(t.size() == ref.size());

    // Forward compare
    auto it = t.begin();
    ri = ref.begin();
    int n = 0;
    while (it != t.end() && ri != ref.end()) {
        if (it.key() != *ri) {
            std::printf("  MISMATCH at %d: got %llu exp %llu\n",
                n, (unsigned long long)it.key(), (unsigned long long)*ri);
            ++fails; return;
        }
        ++it; ++ri; ++n;
    }
    CHECK(it == t.end() && ri == ref.end());
    std::printf("  %d entries match\n", n);
}

template<typename K>
void test_large(const char* label, int N) {
    std::printf("%s (N=%d)...\n", label, N);
    gteitelbaum::kntrie<K, uint64_t> t;
    std::set<K> ref;
    srand(77777);
    for (int i = 0; i < N; ++i) {
        K k;
        if constexpr (sizeof(K) <= 4) k = static_cast<K>(rand());
        else k = static_cast<K>((uint64_t(rand()) << 32) | rand());
        t.insert(k, uint64_t(i));
        ref.insert(k);
    }
    auto it = t.begin();
    auto ri = ref.begin();
    int n = 0;
    while (it != t.end() && ri != ref.end()) {
        if (it.key() != *ri) {
            std::printf("  MISMATCH at %d\n", n);
            ++fails; return;
        }
        ++it; ++ri; ++n;
    }
    CHECK(it == t.end() && ri == ref.end());
    std::printf("  %d entries OK\n", n);
}

int main() {
    test_api();
    test_erase_iterate();
    test_large<uint32_t>("u32 large", 50000);
    test_large<uint64_t>("u64 large", 50000);
    std::printf("\nIterator tests2: %s (%d fails)\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
