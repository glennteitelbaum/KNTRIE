#include "kntrie.hpp"
#include <iostream>
#include <cassert>
#include <string>
#include <random>

using namespace kn;

void test_basic_insert_find() {
    std::cout << "test_basic_insert_find... ";
    
    kntrie<int, int> trie;
    
    assert(trie.empty());
    assert(trie.size() == 0);
    
    auto [it, inserted] = trie.insert(42, 100);
    assert(inserted);
    assert(trie.size() == 1);
    assert(!trie.empty());
    
    auto found = trie.find(42);
    assert(found != trie.end());
    assert(found.key() == 42);
    assert(found.value() == 100);
    
    // Insert duplicate
    auto [it2, inserted2] = trie.insert(42, 200);
    assert(!inserted2);
    assert(trie.size() == 1);
    
    // Key not found
    auto not_found = trie.find(999);
    assert(not_found == trie.end());
    
    std::cout << "PASSED\n";
}

void test_signed_keys() {
    std::cout << "test_signed_keys... ";
    
    kntrie<int32_t, int> trie;
    
    trie.insert(-100, 1);
    trie.insert(-1, 2);
    trie.insert(0, 3);
    trie.insert(1, 4);
    trie.insert(100, 5);
    
    assert(trie.size() == 5);
    
    // Check ordering via iteration
    auto it = trie.begin();
    assert(it.key() == -100); ++it;
    assert(it.key() == -1); ++it;
    assert(it.key() == 0); ++it;
    assert(it.key() == 1); ++it;
    assert(it.key() == 100); ++it;
    assert(it == trie.end());
    
    std::cout << "PASSED\n";
}

void test_various_key_sizes() {
    std::cout << "test_various_key_sizes... ";
    
    // uint8_t
    {
        kntrie<uint8_t, int> trie;
        for (int i = 0; i < 256; ++i) {
            trie.insert(static_cast<uint8_t>(i), i * 10);
        }
        assert(trie.size() == 256);
        
        for (int i = 0; i < 256; ++i) {
            auto it = trie.find(static_cast<uint8_t>(i));
            assert(it != trie.end());
            assert(it.value() == i * 10);
        }
    }
    
    // uint16_t
    {
        kntrie<uint16_t, int> trie;
        for (int i = 0; i < 1000; ++i) {
            trie.insert(static_cast<uint16_t>(i), i);
        }
        assert(trie.size() == 1000);
    }
    
    // uint64_t
    {
        kntrie<uint64_t, int> trie;
        trie.insert(0ULL, 0);
        trie.insert(1ULL, 1);
        trie.insert(0xFFFFFFFFFFFFFFFFULL, 999);
        assert(trie.size() == 3);
        
        auto it = trie.begin();
        assert(it.key() == 0ULL);
    }
    
    std::cout << "PASSED\n";
}

void test_erase() {
    std::cout << "test_erase... ";
    
    kntrie<int, int> trie;
    
    for (int i = 0; i < 100; ++i) {
        trie.insert(i, i * 10);
    }
    assert(trie.size() == 100);
    
    // Erase by key
    size_t erased = trie.erase(50);
    assert(erased == 1);
    assert(trie.size() == 99);
    assert(!trie.contains(50));
    
    // Erase non-existent
    erased = trie.erase(50);
    assert(erased == 0);
    assert(trie.size() == 99);
    
    // Erase all
    for (int i = 0; i < 100; ++i) {
        trie.erase(i);
    }
    assert(trie.empty());
    
    std::cout << "PASSED\n";
}

void test_iteration() {
    std::cout << "test_iteration... ";
    
    kntrie<int, int> trie;
    
    // Insert in random order
    int keys[] = {50, 25, 75, 10, 30, 60, 90, 5, 15, 100};
    for (int k : keys) {
        trie.insert(k, k * 2);
    }
    
    // Forward iteration should be sorted
    int prev = -1;
    int count = 0;
    for (auto it = trie.begin(); it != trie.end(); ++it) {
        assert(it.key() > prev);
        assert(it.value() == it.key() * 2);
        prev = it.key();
        ++count;
    }
    assert(count == 10);
    
    // Reverse iteration
    prev = 1000;
    count = 0;
    for (auto it = trie.rbegin(); it != trie.rend(); ++it) {
        auto [k, v] = *it;
        assert(k < prev);
        prev = k;
        ++count;
    }
    assert(count == 10);
    
    std::cout << "PASSED\n";
}

void test_lower_upper_bound() {
    std::cout << "test_lower_upper_bound... ";
    
    kntrie<int, int> trie;
    
    trie.insert(10, 1);
    trie.insert(20, 2);
    trie.insert(30, 3);
    trie.insert(40, 4);
    
    // lower_bound exact match
    auto it = trie.lower_bound(20);
    assert(it != trie.end());
    assert(it.key() == 20);
    
    // lower_bound between values
    it = trie.lower_bound(25);
    assert(it != trie.end());
    assert(it.key() == 30);
    
    // lower_bound before all
    it = trie.lower_bound(5);
    assert(it != trie.end());
    assert(it.key() == 10);
    
    // lower_bound after all
    it = trie.lower_bound(50);
    assert(it == trie.end());
    
    // upper_bound
    it = trie.upper_bound(20);
    assert(it != trie.end());
    assert(it.key() == 30);
    
    it = trie.upper_bound(40);
    assert(it == trie.end());
    
    std::cout << "PASSED\n";
}

void test_large_value_type() {
    std::cout << "test_large_value_type... ";
    
    struct LargeValue {
        int data[10];
        std::string str;
        
        LargeValue() : str("default") {
            for (int i = 0; i < 10; ++i) data[i] = i;
        }
        LargeValue(int x, const std::string& s) : str(s) {
            for (int i = 0; i < 10; ++i) data[i] = x + i;
        }
    };
    
    kntrie<int, LargeValue> trie;
    
    trie.insert(1, LargeValue(100, "hello"));
    trie.insert(2, LargeValue(200, "world"));
    
    auto it = trie.find(1);
    assert(it != trie.end());
    assert(it.value().data[0] == 100);
    assert(it.value().str == "hello");
    
    // Erase should properly cleanup
    trie.erase(1);
    assert(trie.size() == 1);
    
    trie.clear();
    assert(trie.empty());
    
    std::cout << "PASSED\n";
}

void test_leaf_split() {
    std::cout << "test_leaf_split... ";
    
    kntrie<uint64_t, int> trie;
    
    // Insert more than 64 entries to force leaf split
    for (int i = 0; i < 100; ++i) {
        trie.insert(static_cast<uint64_t>(i), i);
    }
    assert(trie.size() == 100);
    
    // Verify all entries
    for (int i = 0; i < 100; ++i) {
        assert(trie.contains(static_cast<uint64_t>(i)));
        auto it = trie.find(static_cast<uint64_t>(i));
        assert(it.value() == i);
    }
    
    std::cout << "PASSED\n";
}

void test_leaf_merge() {
    std::cout << "test_leaf_merge... ";
    
    kntrie<uint64_t, int> trie;
    
    // Insert then erase to test merging
    for (int i = 0; i < 100; ++i) {
        trie.insert(static_cast<uint64_t>(i), i);
    }
    
    // Erase most entries
    for (int i = 10; i < 100; ++i) {
        trie.erase(static_cast<uint64_t>(i));
    }
    assert(trie.size() == 10);
    
    // Verify remaining
    for (int i = 0; i < 10; ++i) {
        assert(trie.contains(static_cast<uint64_t>(i)));
    }
    
    std::cout << "PASSED\n";
}

void test_copy_move() {
    std::cout << "test_copy_move... ";
    
    kntrie<int, int> trie1;
    for (int i = 0; i < 50; ++i) {
        trie1.insert(i, i * 10);
    }
    
    // Copy constructor
    kntrie<int, int> trie2(trie1);
    assert(trie2.size() == trie1.size());
    for (int i = 0; i < 50; ++i) {
        assert(trie2.contains(i));
    }
    
    // Move constructor
    kntrie<int, int> trie3(std::move(trie1));
    assert(trie3.size() == 50);
    assert(trie1.empty()); // Moved from
    
    // Copy assignment
    kntrie<int, int> trie4;
    trie4 = trie2;
    assert(trie4.size() == 50);
    
    // Move assignment
    kntrie<int, int> trie5;
    trie5 = std::move(trie2);
    assert(trie5.size() == 50);
    assert(trie2.empty());
    
    std::cout << "PASSED\n";
}

void test_stress() {
    std::cout << "test_stress... ";
    
    kntrie<uint32_t, uint32_t> trie;
    std::mt19937 rng(12345);
    std::uniform_int_distribution<uint32_t> dist;
    
    const int N = 10000;
    std::vector<uint32_t> keys;
    
    // Insert random keys
    for (int i = 0; i < N; ++i) {
        uint32_t k = dist(rng);
        auto [it, inserted] = trie.insert(k, k);
        if (inserted) {
            keys.push_back(k);
        }
    }
    
    assert(trie.size() == keys.size());
    
    // Verify all keys exist
    for (uint32_t k : keys) {
        assert(trie.contains(k));
    }
    
    // Verify sorted order
    uint32_t prev = 0;
    bool first = true;
    for (auto it = trie.begin(); it != trie.end(); ++it) {
        if (!first) {
            assert(it.key() > prev);
        }
        prev = it.key();
        first = false;
    }
    
    // Erase half
    for (size_t i = 0; i < keys.size() / 2; ++i) {
        trie.erase(keys[i]);
    }
    
    assert(trie.size() == keys.size() - keys.size() / 2);
    
    std::cout << "PASSED\n";
}

int main() {
    std::cout << "=== kntrie tests ===\n\n";
    
    test_basic_insert_find();
    test_signed_keys();
    test_various_key_sizes();
    test_erase();
    test_iteration();
    test_lower_upper_bound();
    test_large_value_type();
    test_leaf_split();
    test_leaf_merge();
    test_copy_move();
    test_stress();
    
    std::cout << "\n=== All tests passed! ===\n";
    return 0;
}
