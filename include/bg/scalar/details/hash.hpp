#pragma once

#include <bg/scalar/details/storage_traits_base.hpp>

#include <concepts>
#include <cstddef>

namespace bg::details {

template <typename T> struct scalar_hash {
    using is_transparent = void;
    using Traits = scalar_storage_traits<T>;
    using Stored = typename Traits::stored_type;
    using Value = typename Traits::value_type;

    [[nodiscard]] std::size_t operator()(const Stored &value) const noexcept {
        return Traits::hash(Traits::read(value));
    }

    [[nodiscard]] std::size_t operator()(Value value) const noexcept
        requires(!std::same_as<Stored, Value>)
    {
        return Traits::hash(value);
    }
};

template <typename T> struct scalar_equal {
    using is_transparent = void;
    using Traits = scalar_storage_traits<T>;
    using Stored = typename Traits::stored_type;
    using Value = typename Traits::value_type;

    [[nodiscard]] bool operator()(const Stored &left, const Stored &right) const noexcept {
        return Traits::read(left) == Traits::read(right);
    }

    [[nodiscard]] bool operator()(const Stored &left, Value right) const noexcept
        requires(!std::same_as<Stored, Value>)
    {
        return Traits::read(left) == right;
    }
};

} // namespace bg::details
