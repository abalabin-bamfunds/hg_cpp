#pragma once

#include <bg/scalar/storage_traits.hpp>
#include <bg/scalar/declaration.hpp>

namespace bg::details {

// Defines the intrinsic operations exposed by a scalar handle. Collections propagate the item requirements.
template <typename T> struct scalar_handle_traits {
    static constexpr bool supports_equality = [] {
        if constexpr (scalar_variable<T>) {
            return scalar_variable_supports_equality<T>;
        } else {
            return scalar_storage_traits<T>::supports_equality;
        }
    }();

    static constexpr bool supports_hash = [] {
        if constexpr (scalar_variable<T>) {
            return scalar_variable_supports_hash<T>;
        } else {
            return scalar_storage_traits<T>::supports_hash;
        }
    }();
};

} // namespace bg::details
