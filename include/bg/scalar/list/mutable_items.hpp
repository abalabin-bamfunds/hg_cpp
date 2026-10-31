#pragma once

#include <bg/scalar/list/mutable.hpp>

namespace bg {

template <typename T> decltype(auto) MutableList<T>::operator[](std::size_t index) const noexcept {
    auto &stored = mutable_storage().items[static_cast<std::uint32_t>(index)];
    if constexpr (details::mutable_scalar<T>) {
        return details::mutable_scalar_view<T>(stored);
    } else {
        return Traits::read(stored);
    }
}

} // namespace bg
