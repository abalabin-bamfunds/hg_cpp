#pragma once

#include <bg/scalar/storage_traits.hpp>

#include <concepts>
#include <cstddef>

namespace bg::details {

template <typename T> struct scalar_hash {
    using is_transparent = void;
    using Traits = scalar_storage_traits<T>;
    using Stored = typename Traits::stored_type;
    using Value = typename Traits::value_type;

    std::size_t operator()(const Stored &value) const {
        return Traits::hash(Traits::read(value));
    }

    std::size_t operator()(Value value) const
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

    bool operator()(const Stored &left, const Stored &right) const {
        return Traits::equals(Traits::read(left), Traits::read(right));
    }

    bool operator()(const Stored &left, Value right) const
        requires(!std::same_as<Stored, Value>)
    {
        return Traits::equals(Traits::read(left), right);
    }
};

} // namespace bg::details
