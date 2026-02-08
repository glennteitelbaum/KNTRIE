#ifndef KNTRIE3_HPP
#define KNTRIE3_HPP

#include "kntrie.hpp"

namespace kn3 {
    template<typename K, typename V, typename A = std::allocator<uint64_t>>
    using kntrie3 = gteitelbaum::kntrie<K, V, A>;
}

#endif
