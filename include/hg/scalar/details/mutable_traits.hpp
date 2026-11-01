#pragma once

#include <concepts>

namespace bg::details {

// Maps a mutable scalar type to the view of its stored value.
template <typename T> struct scalar_mutable_traits;

template <typename T>
concept mutable_scalar =
    requires {
        typename scalar_mutable_traits<T>::storage_type;
        typename scalar_mutable_traits<T>::view_type;
    } && std::constructible_from<typename scalar_mutable_traits<T>::view_type,
                                 typename scalar_mutable_traits<T>::storage_type &>;

template <mutable_scalar T>
auto mutable_scalar_view(typename scalar_mutable_traits<T>::storage_type &storage) noexcept {
    return typename scalar_mutable_traits<T>::view_type{storage};
}

} // namespace bg::details
