#ifndef KNTRIE_HPP
#define KNTRIE_HPP

#include "kntrie_impl.hpp"
#include <stdexcept>
#include <iterator>

namespace gteitelbaum {

template<typename KEY, typename VALUE, typename ALLOC = std::allocator<uint64_t>>
class kntrie {
    static_assert(std::is_integral_v<KEY>, "KEY must be integral");

    // --- Normalization: A types → normalized slot, deduplicates templates ---
    using VT_orig = value_traits<VALUE, ALLOC>;
    static constexpr bool NORMALIZE = VT_orig::IS_TRIVIAL;
    using IMPL_V = std::conditional_t<NORMALIZE, typename VT_orig::slot_type, VALUE>;
    using impl_t = kntrie_impl<KEY, IMPL_V, ALLOC>;

    // --- Boundary conversion helpers ---

    static IMPL_V to_impl(const VALUE& v) noexcept {
        if constexpr (NORMALIZE) {
            IMPL_V iv{};
            std::memcpy(&iv, &v, sizeof(VALUE));
            return iv;
        } else {
            return v;
        }
    }

    static VALUE to_value(const IMPL_V& iv) noexcept {
        if constexpr (NORMALIZE) {
            VALUE v{};
            std::memcpy(&v, &iv, sizeof(VALUE));
            return v;
        } else {
            return iv;
        }
    }

    static const VALUE* to_value_ptr(const IMPL_V* p) noexcept {
        if constexpr (NORMALIZE)
            return std::launder(reinterpret_cast<const VALUE*>(p));
        else
            return p;
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
        const impl_t* parent_ = nullptr;
        KEY   key_{};
        VALUE value_{};
        bool  valid_ = false;

        const_iterator(const impl_t* p, KEY k, VALUE v, bool valid)
            : parent_(p), key_(k), value_(v), valid_(valid) {}

        static const_iterator from_impl_result(const impl_t* p,
                                               const typename impl_t::iter_result_t& r) {
            return const_iterator(p, r.key, to_value(r.value), r.found);
        }

    public:
        using iterator_category = std::bidirectional_iterator_tag;
        using value_type        = std::pair<const KEY, VALUE>;
        using difference_type   = std::ptrdiff_t;
        using pointer           = const std::pair<const KEY, VALUE>*;
        using reference         = const std::pair<const KEY, VALUE>&;

        const_iterator() = default;

        const KEY&   key()   const noexcept { return key_; }
        const VALUE& value() const noexcept { return value_; }

        std::pair<const KEY, const VALUE&> operator*() const noexcept {
            return {key_, value_};
        }

        const_iterator& operator++() {
            auto r = parent_->iter_next_(key_);
            key_   = r.key;
            value_ = to_value(r.value);
            valid_ = r.found;
            return *this;
        }

        const_iterator operator++(int) {
            auto tmp = *this;
            ++(*this);
            return tmp;
        }

        const_iterator& operator--() {
            auto r = parent_->iter_prev_(key_);
            key_   = r.key;
            value_ = to_value(r.value);
            valid_ = r.found;
            return *this;
        }

        const_iterator operator--(int) {
            auto tmp = *this;
            --(*this);
            return tmp;
        }

        bool operator==(const const_iterator& o) const noexcept {
            if (!valid_ && !o.valid_) return true;
            if (valid_ != o.valid_) return false;
            return key_ == o.key_;
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
        auto [ok, ins] = impl_.insert(kv.first, to_impl(kv.second));
        return {iterator{}, ins};
    }
    std::pair<bool, bool> insert(const KEY& key, const VALUE& value) {
        return impl_.insert(key, to_impl(value));
    }
    std::pair<bool, bool> insert_or_assign(const KEY& key, const VALUE& value) {
        return impl_.insert_or_assign(key, to_impl(value));
    }
    std::pair<bool, bool> assign(const KEY& key, const VALUE& value) {
        return impl_.assign(key, to_impl(value));
    }
    template<typename... Args>
    std::pair<iterator, bool> emplace(Args&&... args) {
        value_type kv(std::forward<Args>(args)...);
        return insert(kv);
    }

    void clear() noexcept { impl_.clear(); }
    size_type erase(const KEY& key) { return impl_.erase(key) ? 1 : 0; }

    // ==================================================================
    // Lookup
    // ==================================================================

    const VALUE* find_value(const KEY& key) const noexcept {
        return to_value_ptr(impl_.find_value(key));
    }
    bool contains(const KEY& key) const noexcept { return impl_.contains(key); }
    size_type count(const KEY& key) const noexcept { return contains(key) ? 1 : 0; }

    VALUE& operator[](const KEY& key) {
        auto* v = impl_.find_value(key);
        if (v) return const_cast<VALUE&>(*to_value_ptr(v));
        impl_.insert(key, to_impl(VALUE{}));
        return const_cast<VALUE&>(*to_value_ptr(impl_.find_value(key)));
    }
    const VALUE& at(const KEY& key) const {
        auto* v = impl_.find_value(key);
        if (!v) throw std::out_of_range("kntrie::at: key not found");
        return *to_value_ptr(v);
    }
    VALUE& at(const KEY& key) {
        auto* v = impl_.find_value(key);
        if (!v) throw std::out_of_range("kntrie::at: key not found");
        return const_cast<VALUE&>(*to_value_ptr(v));
    }

    // ==================================================================
    // Iterators
    // ==================================================================

    const_iterator begin() const noexcept {
        return const_iterator::from_impl_result(&impl_, impl_.iter_first_());
    }
    const_iterator end() const noexcept {
        return const_iterator{};
    }
    const_iterator cbegin() const noexcept { return begin(); }
    const_iterator cend()   const noexcept { return end(); }

    const_iterator rbegin() const noexcept {
        return const_iterator::from_impl_result(&impl_, impl_.iter_last_());
    }
    const_iterator rend() const noexcept {
        return const_iterator{};
    }

    const_iterator find(const KEY& key) const noexcept {
        auto* v = impl_.find_value(key);
        if (!v) return end();
        return const_iterator(&impl_, key, to_value(*v), true);
    }

    const_iterator lower_bound(const KEY& k) const noexcept {
        auto* v = impl_.find_value(k);
        if (v) return const_iterator(&impl_, k, to_value(*v), true);
        auto r = impl_.iter_next_(k);
        return const_iterator::from_impl_result(&impl_, r);
    }

    const_iterator upper_bound(const KEY& k) const noexcept {
        auto r = impl_.iter_next_(k);
        return const_iterator::from_impl_result(&impl_, r);
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
