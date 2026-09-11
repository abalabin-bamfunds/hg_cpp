#pragma once

#include <bg/scalar/details/allocation_context.hpp>
#include <bg/scalar/details/mutable_traits.hpp>
#include <bg/scalar/details/storage_traits.hpp>
#include <bg/scalar/dict/type.hpp>
#include <bg/scalar/runtime_type.hpp>
#include <bg/scalar/value.hpp>

namespace bg {

template <typename K, typename V> class MutableDict : public Dict<K, V> {
    using Base = Dict<K, V>;
    using Storage = details::scalar_dict_storage<K, V>;
    using KeyTraits = details::scalar_storage_traits<K>;
    using ValueTraits = details::scalar_storage_traits<V>;

  public:
    explicit MutableDict(Storage &storage) noexcept : Base{storage} {}

    std::size_t size() const noexcept { return Base::size(); }
    bool empty() const noexcept { return Base::empty(); }
    bool contains(typename KeyTraits::value_type key) const { return Base::contains(key); }
    decltype(auto) at(typename KeyTraits::value_type key) const;

    void insert_or_assign(typename KeyTraits::value_type key, typename ValueTraits::value_type value) const {
        auto &memory = details::current_scalar_memory_resource();
        auto [stored, inserted] = mutable_storage().items.try_emplace_with(
            key, memory, [&](auto *destination) { KeyTraits::construct(destination, key, memory); },
            [&](auto *destination) noexcept { KeyTraits::destroy(destination, memory); },
            [&](auto *destination) { ValueTraits::construct(destination, value, memory); });
        if (!inserted) {
            if constexpr (requires { ValueTraits::assign(*stored, value, memory); }) {
                ValueTraits::assign(*stored, value, memory);
            } else {
                (void)erase(key);
                (void)mutable_storage().items.try_emplace_with(
                    key, memory, [&](auto *destination) { KeyTraits::construct(destination, key, memory); },
                    [&](auto *destination) noexcept { KeyTraits::destroy(destination, memory); },
                    [&](auto *destination) { ValueTraits::construct(destination, value, memory); });
            }
        }
    }

    void insert_or_assign(typename KeyTraits::value_type key, const Scalar<V> &value) const {
        auto &memory = details::current_scalar_memory_resource();
        const auto &runtime_type = cpp_scalar_runtime_type<V>();
        auto [stored, inserted] = mutable_storage().items.try_emplace_with(
            key, memory, [&](auto *destination) { KeyTraits::construct(destination, key, memory); },
            [&](auto *destination) noexcept { KeyTraits::destroy(destination, memory); },
            [&](auto *destination) { details::scalar_data(value).copy_construct(destination, runtime_type, memory); });
        if (!inserted) {
            details::scalar_data(value).assign(stored, runtime_type);
        }
    }

    [[nodiscard]] bool erase(typename KeyTraits::value_type key) const {
        auto &memory = details::current_scalar_memory_resource();
        return mutable_storage().items.erase_with(
            key, memory, [&](auto *stored) noexcept { KeyTraits::destroy(stored, memory); },
            [&](auto *stored) noexcept { ValueTraits::destroy(stored, memory); });
    }

    void clear() const {
        auto &memory = details::current_scalar_memory_resource();
        mutable_storage().items.release_with(
            memory, [&](auto *stored) noexcept { KeyTraits::destroy(stored, memory); },
            [&](auto *stored) noexcept { ValueTraits::destroy(stored, memory); });
    }

  private:
    // MutableDict is constructed from mutable storage; Dict retains the same pointer for read-only access.
    Storage &mutable_storage() const noexcept { return const_cast<Storage &>(Base::storage()); }
};

} // namespace bg

namespace bg::details {

template <typename K, typename V>
    requires(!generic_scalar<Dict<K, V>>)
struct scalar_mutable_traits<Dict<K, V>> {
    using storage_type = scalar_dict_storage<K, V>;
    using view_type = MutableDict<K, V>;
};

} // namespace bg::details
