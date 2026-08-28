#pragma once

#include <bg/scalar/details/allocation_context.hpp>
#include <bg/scalar/details/storage_traits.hpp>
#include <bg/scalar/dict/type.hpp>
#include <bg/scalar/storage_representation.hpp>
#include <bg/scalar/value.hpp>

namespace bg {

template <typename K, typename V> class MutableDict : public Dict<K, V> {
    using Base = Dict<K, V>;
    using Storage = details::scalar_dict_storage<K, V>;
    using KeyTraits = details::scalar_storage_traits<K>;
    using ValueTraits = details::scalar_storage_traits<V>;

  public:
    explicit MutableDict(Storage &storage) noexcept : Base{storage} {}

    [[nodiscard]] std::size_t size() const noexcept { return Base::size(); }
    [[nodiscard]] bool empty() const noexcept { return Base::empty(); }
    [[nodiscard]] bool contains(typename KeyTraits::value_type key) const { return Base::contains(key); }
    [[nodiscard]] decltype(auto) at(typename KeyTraits::value_type key) const;

    void insert_or_assign(typename KeyTraits::value_type key, typename ValueTraits::value_type value) const {
        auto &memory = details::current_scalar_memory_resource();
        auto [stored, inserted] = mutable_storage().items.try_emplace_with(
            key, memory, [&](auto *destination) { KeyTraits::construct(destination, key, memory); },
            [&](auto *destination) noexcept { KeyTraits::destroy(destination, memory); },
            [&](auto *destination) { ValueTraits::construct(destination, value, memory); });
        if (!inserted) {
            if constexpr (requires { ValueTraits::assign(*stored, value, memory); }) {
                ValueTraits::assign(*stored, value, memory);
            } else if constexpr (requires { ValueTraits::assign(*stored, value); }) {
                ValueTraits::assign(*stored, value);
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
        const auto &representation = cpp_scalar_storage_representation<V>();
        auto [stored, inserted] = mutable_storage().items.try_emplace_with(
            key, memory, [&](auto *destination) { KeyTraits::construct(destination, key, memory); },
            [&](auto *destination) noexcept { KeyTraits::destroy(destination, memory); },
            [&](auto *destination) { value.input_view().copy_construct(destination, representation, memory); });
        if (!inserted) {
            value.input_view().assign(stored, representation);
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
    [[nodiscard]] Storage &mutable_storage() const noexcept { return const_cast<Storage &>(Base::storage()); }
};

} // namespace bg
