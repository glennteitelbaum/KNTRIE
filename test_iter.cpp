#include "kntrie.hpp"
#include <cstdio>
#include <cstdlib>
#include <set>

int fails = 0;
void check(bool c, const char* msg, int line) {
    if (!c) { std::printf("  FAIL line %d: %s\n", line, msg); ++fails; }
}
#define CHECK(c) check((c), #c, __LINE__)

template<typename K>
void test_iter_basic(const char* label) {
    std::printf("%s...\n", label);
    gteitelbaum::kntrie<K, uint64_t> t;

    // Empty
    auto r = t.impl().iter_first_();
    CHECK(!r.found);
    r = t.impl().iter_last_();
    CHECK(!r.found);

    // Single entry
    t.insert(K(42), 100ULL);
    r = t.impl().iter_first_();
    CHECK(r.found && r.key == K(42) && r.value == 100);
    r = t.impl().iter_last_();
    CHECK(r.found && r.key == K(42) && r.value == 100);
    r = t.impl().iter_next_(K(42));
    CHECK(!r.found);
    r = t.impl().iter_prev_(K(42));
    CHECK(!r.found);
    r = t.impl().iter_next_(K(41));
    CHECK(r.found && r.key == K(42));
    r = t.impl().iter_prev_(K(43));
    CHECK(r.found && r.key == K(42));
}

template<typename K>
void test_iter_sequential(const char* label, int N) {
    std::printf("%s (N=%d)...\n", label, N);
    gteitelbaum::kntrie<K, uint64_t> t;
    std::set<K> ref;

    for (int i = 0; i < N; ++i) {
        K k = static_cast<K>(i);
        t.insert(k, uint64_t(i));
        ref.insert(k);
    }

    // Forward iteration
    auto r = t.impl().iter_first_();
    auto it = ref.begin();
    int count = 0;
    while (r.found && it != ref.end()) {
        if (r.key != *it) {
            std::printf("  MISMATCH at %d: got %llu expected %llu\n",
                count, (unsigned long long)r.key, (unsigned long long)*it);
            ++fails;
            return;
        }
        r = t.impl().iter_next_(r.key);
        ++it;
        ++count;
    }
    CHECK(count == N);
    CHECK(!r.found);
    CHECK(it == ref.end());

    // Backward iteration
    r = t.impl().iter_last_();
    auto rit = ref.rbegin();
    count = 0;
    while (r.found && rit != ref.rend()) {
        if (r.key != *rit) {
            std::printf("  MISMATCH at %d: got %llu expected %llu\n",
                count, (unsigned long long)r.key, (unsigned long long)*rit);
            ++fails;
            return;
        }
        r = t.impl().iter_prev_(r.key);
        ++rit;
        ++count;
    }
    CHECK(count == N);
}

template<typename K>
void test_iter_random(const char* label, int N) {
    std::printf("%s (N=%d)...\n", label, N);
    gteitelbaum::kntrie<K, uint64_t> t;
    std::set<K> ref;

    srand(12345);
    for (int i = 0; i < N; ++i) {
        K k;
        if constexpr (sizeof(K) <= 4)
            k = static_cast<K>(rand());
        else
            k = static_cast<K>((uint64_t(rand()) << 32) | rand());
        t.insert(k, uint64_t(i));
        ref.insert(k);
    }

    // Forward
    auto r = t.impl().iter_first_();
    auto it = ref.begin();
    int count = 0;
    while (r.found && it != ref.end()) {
        if (r.key != *it) {
            std::printf("  FWD MISMATCH at %d: got %llu expected %llu\n",
                count, (unsigned long long)r.key, (unsigned long long)*it);
            ++fails;
            return;
        }
        r = t.impl().iter_next_(r.key);
        ++it;
        ++count;
    }
    CHECK(!r.found && it == ref.end());
    std::printf("  forward: %d entries OK\n", count);

    // Backward
    r = t.impl().iter_last_();
    auto rit = ref.rbegin();
    count = 0;
    while (r.found && rit != ref.rend()) {
        if (r.key != *rit) {
            std::printf("  REV MISMATCH at %d: got %llu expected %llu\n",
                count, (unsigned long long)r.key, (unsigned long long)*rit);
            ++fails;
            return;
        }
        r = t.impl().iter_prev_(r.key);
        ++rit;
        ++count;
    }
    CHECK(!r.found && rit == ref.rend());
    std::printf("  backward: %d entries OK\n", count);
}

int main() {
    test_iter_basic<uint16_t>("u16 basic");
    test_iter_basic<uint32_t>("u32 basic");
    test_iter_basic<uint64_t>("u64 basic");
    test_iter_basic<int32_t>("i32 basic");

    test_iter_sequential<uint16_t>("u16 seq", 1000);
    test_iter_sequential<uint32_t>("u32 seq", 5000);
    test_iter_sequential<uint64_t>("u64 seq", 5000);
    test_iter_sequential<int32_t>("i32 seq", 5000);

    test_iter_random<uint32_t>("u32 random", 10000);
    test_iter_random<uint64_t>("u64 random", 10000);

    std::printf("\nIterator tests: %s (%d fails)\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
