#pragma once

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

namespace bg::details {

template <typename T> struct scalar_storage_traits {
    using stored_type = T;
    using value_type = scalar_value_t<T>;

    static constexpr bool supports_equality = std::equality_comparable<value_type>;
    static constexpr bool supports_hash =
        std::same_as<T, EvaluationTime> || requires(value_type value) { std::hash<value_type>{}(value); };
    static constexpr bool uses_variable_memory = false;
    static constexpr bool requires_finalization = !std::is_trivially_destructible_v<stored_type>;

    [[nodiscard]] static value_type read(const stored_type &value) noexcept { return value; }

    [[nodiscard]] static std::size_t hash(value_type value) noexcept {
        if constexpr (std::same_as<T, EvaluationTime>) {
            return std::hash<EvaluationTime::rep>{}(value.time_since_epoch().count());
        } else {
            return std::hash<value_type>{}(value);
        }
    }

    static void construct(stored_type *destination, value_type value, std::pmr::memory_resource &) {
        std::construct_at(destination, std::move(value));
    }

    static void assign(stored_type &destination, value_type value) { destination = std::move(value); }

    static void destroy(stored_type *value, std::pmr::memory_resource &) noexcept { std::destroy_at(value); }
};

template <> struct scalar_storage_traits<string> {
    using stored_type = std::pmr::string;
    using value_type = std::string_view;

    static constexpr bool supports_equality = true;
    static constexpr bool supports_hash = true;
    static constexpr bool uses_variable_memory = true;
    static constexpr bool requires_finalization = false;

    [[nodiscard]] static value_type read(const stored_type &value) noexcept { return value; }

    [[nodiscard]] static std::size_t hash(value_type value) noexcept { return std::hash<value_type>{}(value); }

    static void construct(stored_type *destination, value_type value, std::pmr::memory_resource &memory) {
        std::construct_at(destination, value, &memory);
    }

    static void assign(stored_type &destination, value_type value) { destination.assign(value); }

    static void destroy(stored_type *value, std::pmr::memory_resource &) noexcept { std::destroy_at(value); }
};

template <typename T> struct scalar_storage_traits<Opaque<T>> {
    using stored_type = T;
    using value_type = T;

    static constexpr bool supports_equality = false;
    static constexpr bool supports_hash = false;
    static constexpr bool uses_variable_memory = false;
    static constexpr bool requires_finalization = !std::is_trivially_destructible_v<stored_type>;

    [[nodiscard]] static scalar_value_t<Opaque<T>> read(const stored_type &value) noexcept { return value; }

    template <typename Source>
    static void construct(stored_type *destination, Source &&value, std::pmr::memory_resource &) {
        std::construct_at(destination, std::forward<Source>(value));
    }

    static void destroy(stored_type *value, std::pmr::memory_resource &) noexcept { std::destroy_at(value); }
};

template <typename T> using scalar_stored_t = typename scalar_storage_traits<T>::stored_type;

} // namespace bg::details
