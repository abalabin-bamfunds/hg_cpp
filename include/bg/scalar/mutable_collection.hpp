#pragma once

#include <bg/scalar/dict/mutable.hpp>
#include <bg/scalar/list/mutable.hpp>
#include <bg/scalar/set/mutable.hpp>

namespace bg {

template <typename T> decltype(auto) MutableList<T>::operator[](std::size_t index) const noexcept {
    auto &stored = mutable_storage().items[static_cast<std::uint32_t>(index)];

    if constexpr (is_list_v<T>) {
        return MutableList<typename T::item_type>{stored};
    } else if constexpr (is_set_v<T>) {
        return MutableSet<typename T::item_type>{stored};
    } else if constexpr (is_dict_v<T>) {
        return MutableDict<typename T::key_type, typename T::mapped_type>{stored};
    } else {
        return Traits::read(stored);
    }
}

template <typename K, typename V> decltype(auto) MutableDict<K, V>::at(typename KeyTraits::value_type key) const {
    auto &stored = mutable_storage().items.at(key);

    if constexpr (is_list_v<V>) {
        return MutableList<typename V::item_type>{stored};
    } else if constexpr (is_set_v<V>) {
        return MutableSet<typename V::item_type>{stored};
    } else if constexpr (is_dict_v<V>) {
        return MutableDict<typename V::key_type, typename V::mapped_type>{stored};
    } else {
        return ValueTraits::read(stored);
    }
}

} // namespace bg
