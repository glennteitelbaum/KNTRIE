#ifndef KNTRIE_HPP
#define KNTRIE_HPP

#include "kntrie_impl.hpp"
#include <stdexcept>
#include <iterator>

namespace gteitelbaum {

template<typename KEY, typename VALUE, typename ALLOC = std::allocator<uint64_t>>
class kntrie {
    static_assert(std::is_integral_v<KEY>, "KEY must be integral");

public:
    using key_type       = KEY;
    using mapped_type    = VALUE;
    using value_type     = std::pair<const KEY, VALUE>;
    using size_type      = std::size_t;
    using difference_type = std::ptrdiff_t;
    using allocator_type = ALLOC;
    using reference       = value_type&;
    using const_reference = const value_type&;

    struct iterator {
        using iterator_category = std::bidirectional_iterator_tag;
        using value_type        = std::pair<const KEY, VALUE>;
        using difference_type   = std::ptrdiff_t;
        using pointer           = value_type*;
        using reference         = value_type&;
        iterator() = default;
        reference operator*()  const { throw std::logic_error("iterator not implemented"); }
        pointer   operator->() const { throw std::logic_error("iterator not implemented"); }
        iterator& operator++()    { throw std::logic_error("iterator not implemented"); }
        iterator  operator++(int) { throw std::logic_error("iterator not implemented"); }
        iterator& operator--()    { throw std::logic_error("iterator not implemented"); }
        iterator  operator--(int) { throw std::logic_error("iterator not implemented"); }
        bool operator==(const iterator&) const noexcept { return true; }
        bool operator!=(const iterator&) const noexcept { return false; }
    };

    struct const_iterator {
        using iterator_category = std::bidirectional_iterator_tag;
        using value_type        = std::pair<const KEY, VALUE>;
        using difference_type   = std::ptrdiff_t;
        using pointer           = const value_type*;
        using reference         = const value_type&;
        const_iterator() = default;
        const_iterator(const iterator&) {}
        reference operator*()  const { throw std::logic_error("const_iterator not implemented"); }
        pointer   operator->() const { throw std::logic_error("const_iterator not implemented"); }
        const_iterator& operator++()    { throw std::logic_error("const_iterator not implemented"); }
        const_iterator  operator++(int) { throw std::logic_error("const_iterator not implemented"); }
        const_iterator& operator--()    { throw std::logic_error("const_iterator not implemented"); }
        const_iterator  operator--(int) { throw std::logic_error("const_iterator not implemented"); }
        bool operator==(const const_iterator&) const noexcept { return true; }
        bool operator!=(const const_iterator&) const noexcept { return false; }
    };

    kntrie() = default;
    ~kntrie() = default;
    kntrie(const kntrie&) = delete;
    kntrie& operator=(const kntrie&) = delete;

    [[nodiscard]] bool      empty() const noexcept { return impl_.empty(); }
    [[nodiscard]] size_type size()  const noexcept { return impl_.size(); }

    std::pair<iterator, bool> insert(const value_type& kv) {
        auto [ok, ins] = impl_.insert(kv.first, kv.second);
        return {iterator{}, ins};
    }
    std::pair<bool, bool> insert(const KEY& key, const VALUE& value) {
        return impl_.insert(key, value);
    }
    std::pair<bool, bool> insert_or_assign(const KEY& key, const VALUE& value) {
        return impl_.insert_or_assign(key, value);
    }
    std::pair<bool, bool> assign(const KEY& key, const VALUE& value) {
        return impl_.assign(key, value);
    }
    template<typename... Args>
    std::pair<iterator, bool> emplace(Args&&... args) {
        value_type kv(std::forward<Args>(args)...);
        return insert(kv);
    }

    void clear() noexcept { impl_.clear(); }
    size_type erase(const KEY& key) { return impl_.erase(key) ? 1 : 0; }

    const VALUE* find_value(const KEY& key) const noexcept { return impl_.find_value(key); }
    bool contains(const KEY& key) const noexcept { return impl_.contains(key); }
    size_type count(const KEY& key) const noexcept { return contains(key) ? 1 : 0; }

    VALUE& operator[](const KEY& key) {
        const VALUE* v = impl_.find_value(key);
        if (v) return const_cast<VALUE&>(*v);
        impl_.insert(key, VALUE{});
        return const_cast<VALUE&>(*impl_.find_value(key));
    }
    const VALUE& at(const KEY& key) const {
        const VALUE* v = impl_.find_value(key);
        if (!v) throw std::out_of_range("kntrie::at: key not found");
        return *v;
    }
    VALUE& at(const KEY& key) {
        const VALUE* v = impl_.find_value(key);
        if (!v) throw std::out_of_range("kntrie::at: key not found");
        return const_cast<VALUE&>(*v);
    }

    iterator       find(const KEY&)       { return iterator{}; }
    const_iterator find(const KEY&) const { return const_iterator{}; }

    iterator       begin()        noexcept { return iterator{}; }
    iterator       end()          noexcept { return iterator{}; }
    const_iterator begin()  const noexcept { return const_iterator{}; }
    const_iterator end()    const noexcept { return const_iterator{}; }
    const_iterator cbegin() const noexcept { return const_iterator{}; }
    const_iterator cend()   const noexcept { return const_iterator{}; }

    iterator       lower_bound(const KEY&)       { return iterator{}; }
    const_iterator lower_bound(const KEY&) const { return const_iterator{}; }
    iterator       upper_bound(const KEY&)       { return iterator{}; }
    const_iterator upper_bound(const KEY&) const { return const_iterator{}; }

    std::pair<iterator, iterator>             equal_range(const KEY& k)       { return {lower_bound(k), upper_bound(k)}; }
    std::pair<const_iterator, const_iterator> equal_range(const KEY& k) const { return {lower_bound(k), upper_bound(k)}; }

    size_t memory_usage() const noexcept { return impl_.memory_usage(); }
    auto   debug_stats() const noexcept  { return impl_.debug_stats(); }
    auto   debug_root_info() const       { return impl_.debug_root_info(); }
    const uint64_t* debug_root() const noexcept { return impl_.debug_root(); }

private:
    kntrie_impl<KEY, VALUE, ALLOC> impl_;
};

} // namespace gteitelbaum

#endif // KNTRIE_HPP
