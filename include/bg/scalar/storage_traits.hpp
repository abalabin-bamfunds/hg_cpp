#pragma once

#include <bg/allocator.hpp>
#include <bg/evaluation_time.hpp>
#include <bg/scalar/details/value_type.hpp>
#include <bg/scalar/opaque.hpp>
#include <bg/scalar/string.hpp>

#include <concepts>
#include <cstddef>
#include <functional>
#include <memory>
#include <memory_resource>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace bg {

// Defines the canonical C++ storage, read view, intrinsic operations, and lifecycle for a scalar declaration.
// Scalar providers specialize this trait in their public interface.
template <typename T> struct scalar_storage_traits {
    using stored_type = T;
    using value_type = details::scalar_value_t<T>;

    static constexpr bool supports_equality = std::equality_comparable<value_type>;
    static constexpr bool supports_hash =
        std::same_as<T, EvaluationTime> || requires(value_type value) { std::hash<value_type>{}(value); };
    static constexpr bool uses_variable_memory = false;
    static constexpr bool requires_finalization = !std::is_trivially_destructible_v<stored_type>;
    static constexpr bool assign_uses_memory_resource = false;

    static value_type read(const stored_type &value) noexcept { return value; }

    static std::size_t hash(value_type value) noexcept {
        if constexpr (std::same_as<T, EvaluationTime>) {
            return std::hash<EvaluationTime::rep>{}(value.time_since_epoch().count());
        } else {
            return std::hash<value_type>{}(value);
        }
    }

    static void construct(stored_type *destination, value_type value, allocator &) {
        std::construct_at(destination, std::move(value));
    }

    static void assign(stored_type &destination, value_type value, allocator &)
        requires std::assignable_from<stored_type &, value_type>
    {
        destination = std::move(value);
    }

    static void destroy(stored_type *value, allocator &) noexcept { std::destroy_at(value); }
};

template <> struct scalar_storage_traits<string> {
    using stored_type = std::pmr::string;
    using value_type = std::string_view;

    static constexpr bool supports_equality = true;
    static constexpr bool supports_hash = true;
    static constexpr bool uses_variable_memory = true;
    static constexpr bool requires_finalization = false;
    static constexpr bool assign_uses_memory_resource = false;

    static value_type read(const stored_type &value) noexcept { return value; }

    static std::size_t hash(value_type value) noexcept { return std::hash<value_type>{}(value); }

    static void construct(stored_type *destination, value_type value, allocator &memory) {
        std::construct_at(destination, value, &memory.memory_resource());
    }

    static void assign(stored_type &destination, value_type value, allocator &) { destination.assign(value); }

    static void destroy(stored_type *value, allocator &) noexcept { std::destroy_at(value); }
};

template <typename T> struct scalar_storage_traits<Opaque<T>> {
    using stored_type = T;
    using value_type = T;

    static constexpr bool supports_equality = false;
    static constexpr bool supports_hash = false;
    static constexpr bool uses_variable_memory = false;
    static constexpr bool requires_finalization = !std::is_trivially_destructible_v<stored_type>;
    static constexpr bool assign_uses_memory_resource = false;

    static details::scalar_value_t<Opaque<T>> read(const stored_type &value) noexcept { return value; }

    template <typename Source>
    static void construct(stored_type *destination, Source &&value, allocator &) {
        std::construct_at(destination, std::forward<Source>(value));
    }

    static void assign(stored_type &destination, const stored_type &value, allocator &)
        requires std::is_copy_assignable_v<stored_type>
    {
        destination = value;
    }

    static void destroy(stored_type *value, allocator &) noexcept { std::destroy_at(value); }
};

namespace details {

template <typename T> using scalar_stored_t = typename scalar_storage_traits<T>::stored_type;

} // namespace details

} // namespace bg
