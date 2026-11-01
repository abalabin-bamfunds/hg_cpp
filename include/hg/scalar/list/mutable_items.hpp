#pragma once

#include <hg/scalar/list/mutable.hpp>

namespace hg {

template <typename T> decltype(auto) MutableList<T>::operator[](std::size_t index) const noexcept {
    auto &stored = mutable_storage().items[static_cast<std::uint32_t>(index)];
    if constexpr (details::mutable_scalar<T>) {
        return details::mutable_scalar_view<T>(stored);
    } else {
        return Traits::read(stored);
    }
}

} // namespace hg
