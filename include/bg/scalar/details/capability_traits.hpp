#pragma once

#include <bg/scalar/details/storage_traits_base.hpp>
#include <bg/scalar/type.hpp>

namespace bg::details {

// Defines the operations exposed by generic scalar handles. Collections specialize this to propagate item capabilities.
template <typename T> struct scalar_capability_traits {
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
