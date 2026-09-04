#pragma once

#include <bg/scalar/dict/mutable.hpp>

namespace bg {

template <typename K, typename V> decltype(auto) MutableDict<K, V>::at(typename KeyTraits::value_type key) const {
    auto &stored = mutable_storage().items.at(key);
    if constexpr (details::mutable_scalar<V>) {
        return details::mutable_scalar_view<V>(stored);
    } else {
        return ValueTraits::read(stored);
    }
}

} // namespace bg
