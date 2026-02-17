#ifndef KNTRIE_HPP
#define KNTRIE_HPP

#include "kntrie_impl.hpp"
#include <stdexcept>
#include <iterator>

namespace gteitelbaum {

template<typename KEY, typename VALUE, typename ALLOC = std::allocator<uint64_t>>
class kntrie {
    static_assert(std::is_integral_v<KEY>, "KEY must be integral");

    using UK     = std::make_unsigned_t<KEY>;
    using impl_t = kntrie_impl<UK, VALUE, ALLOC>;

    static constexpr UK SIGN_BIT = std::is_signed_v<KEY>
        ? (UK(1) << (sizeof(KEY) * 8 - 1)) : UK(0);

    static UK to_unsigned(KEY k) noexcept {
        return static_cast<UK>(k) ^ SIGN_BIT;
    }
    static KEY from_unsigned(UK u) noexcept {
        return static_cast<KEY>(u ^ SIGN_BIT);
    }

public:
    using key_type        = KEY;
    using mapped_type     = VALUE;
    using value_type      = std::pair<const KEY, VALUE>;
    using size_type       = std::size_t;
    using difference_type = std::ptrdiff_t;
    using allocator_type  = ALLOC;

    // ==================================================================
    // Iterator — snapshot-based, bidirectional
    // ==================================================================

    class const_iterator {
        friend class kntrie;
        const impl_t* parent_v = nullptr;
        UK    ukey_v{};
        VALUE value_v{};
        bool  is_valid_v = false;

        const_iterator(const impl_t* p, UK uk, VALUE v, bool valid)
            : parent_v(p), ukey_v(uk), value_v(v), is_valid_v(valid) {}

        static const_iterator from_result(const impl_t* p,
                                           const typename impl_t::iter_result_t& r) {
            return const_iterator(p, r.key, r.value, r.found);
        }

    public:
        using iterator_category = std::bidirectional_iterator_tag;
        using value_type        = std::pair<const KEY, VALUE>;
        using difference_type   = std::ptrdiff_t;
        using pointer           = const std::pair<const KEY, VALUE>*;
        using reference         = const std::pair<const KEY, VALUE>&;

        const_iterator() = default;

        KEY          key()   const noexcept { return from_unsigned(ukey_v); }
        const VALUE& value() const noexcept { return value_v; }

        std::pair<const KEY, const VALUE&> operator*() const noexcept {
            return {from_unsigned(ukey_v), value_v};
        }

        const_iterator& operator++() {
            auto r = parent_v->iter_next(ukey_v);
            ukey_v  = r.key;
            value_v = r.value;
            is_valid_v = r.found;
            return *this;
        }

        const_iterator operator++(int) {
            auto tmp = *this;
            ++(*this);
            return tmp;
        }

        const_iterator& operator--() {
            auto r = parent_v->iter_prev(ukey_v);
            ukey_v  = r.key;
            value_v = r.value;
            is_valid_v = r.found;
            return *this;
        }

        const_iterator operator--(int) {
            auto tmp = *this;
            --(*this);
            return tmp;
        }

        bool operator==(const const_iterator& o) const noexcept {
            if (!is_valid_v && !o.is_valid_v) return true;
            if (is_valid_v != o.is_valid_v) return false;
            return ukey_v == o.ukey_v;
        }

        bool operator!=(const const_iterator& o) const noexcept {
            return !(*this == o);
        }
    };

    using iterator = const_iterator;

    // ==================================================================
    // Construction / Destruction
    // ==================================================================

    kntrie() = default;
    ~kntrie() = default;
    kntrie(const kntrie&) = delete;
    kntrie& operator=(const kntrie&) = delete;

    // ==================================================================
    // Size
    // ==================================================================

    [[nodiscard]] bool      empty() const noexcept { return impl_.empty(); }
    [[nodiscard]] size_type size()  const noexcept { return impl_.size(); }

    // ==================================================================
    // Modifiers
    // ==================================================================

    std::pair<iterator, bool> insert(const value_type& kv) {
        auto [ok, ins] = impl_.insert(to_unsigned(kv.first), kv.second);
        return {iterator{}, ins};
    }
    std::pair<bool, bool> insert(const KEY& key, const VALUE& value) {
        return impl_.insert(to_unsigned(key), value);
    }
    std::pair<bool, bool> insert_or_assign(const KEY& key, const VALUE& value) {
        return impl_.insert_or_assign(to_unsigned(key), value);
    }
    std::pair<bool, bool> assign(const KEY& key, const VALUE& value) {
        return impl_.assign(to_unsigned(key), value);
    }
    template<typename... Args>
    std::pair<iterator, bool> emplace(Args&&... args) {
        value_type kv(std::forward<Args>(args)...);
        return insert(kv);
    }

    void clear() noexcept { impl_.clear(); }
    size_type erase(const KEY& key) { return impl_.erase(to_unsigned(key)) ? 1 : 0; }

    // ==================================================================
    // Lookup
    // ==================================================================

    const VALUE* find_value(const KEY& key) const noexcept { return impl_.find_value(to_unsigned(key)); }
    bool contains(const KEY& key) const noexcept { return impl_.contains(to_unsigned(key)); }
    size_type count(const KEY& key) const noexcept { return contains(key) ? 1 : 0; }

    VALUE& operator[](const KEY& key) {
        UK uk = to_unsigned(key);
        const VALUE* v = impl_.find_value(uk);
        if (v) return const_cast<VALUE&>(*v);
        impl_.insert(uk, VALUE{});
        return const_cast<VALUE&>(*impl_.find_value(uk));
    }
    const VALUE& at(const KEY& key) const {
        const VALUE* v = impl_.find_value(to_unsigned(key));
        if (!v) throw std::out_of_range("kntrie::at: key not found");
        return *v;
    }
    VALUE& at(const KEY& key) {
        const VALUE* v = impl_.find_value(to_unsigned(key));
        if (!v) throw std::out_of_range("kntrie::at: key not found");
        return const_cast<VALUE&>(*v);
    }

    // ==================================================================
    // Iterators
    // ==================================================================

    const_iterator begin() const noexcept {
        return const_iterator::from_result(&impl_, impl_.iter_first());
    }
    const_iterator end() const noexcept {
        return const_iterator{};
    }
    const_iterator cbegin() const noexcept { return begin(); }
    const_iterator cend()   const noexcept { return end(); }

    const_iterator rbegin() const noexcept {
        return const_iterator::from_result(&impl_, impl_.iter_last());
    }
    const_iterator rend() const noexcept {
        return const_iterator{};
    }

    const_iterator find(const KEY& key) const noexcept {
        UK uk = to_unsigned(key);
        const VALUE* v = impl_.find_value(uk);
        if (!v) return end();
        return const_iterator(&impl_, uk, *v, true);
    }

    const_iterator lower_bound(const KEY& k) const noexcept {
        UK uk = to_unsigned(k);
        const VALUE* v = impl_.find_value(uk);
        if (v) return const_iterator(&impl_, uk, *v, true);
        auto r = impl_.iter_next(uk);
        return const_iterator::from_result(&impl_, r);
    }

    const_iterator upper_bound(const KEY& k) const noexcept {
        auto r = impl_.iter_next(to_unsigned(k));
        return const_iterator::from_result(&impl_, r);
    }

    std::pair<const_iterator, const_iterator> equal_range(const KEY& k) const noexcept {
        return {lower_bound(k), upper_bound(k)};
    }

    // ==================================================================
    // Debug / Stats
    // ==================================================================

    size_t memory_usage() const noexcept { return impl_.memory_usage(); }
    auto   debug_stats() const noexcept  { return impl_.debug_stats(); }
    auto   debug_root_info() const       { return impl_.debug_root_info(); }
    const uint64_t* debug_root() const noexcept { return impl_.debug_root(); }

    const impl_t& impl() const noexcept { return impl_; }

private:
    impl_t impl_;
};

} // namespace gteitelbaum

#endif // KNTRIE_HPP
