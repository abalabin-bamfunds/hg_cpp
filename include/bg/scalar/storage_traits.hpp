#pragma once

#include <bg/allocator.hpp>
#include <bg/scalar/builtins.hpp>
#include <bg/scalar/details/value_type.hpp>
#include <bg/scalar/opaque.hpp>
#include <bg/scalar/string.hpp>
#include <bg/scalar/struct/declaration.hpp>

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
    static constexpr bool supports_hash = std::same_as<T, byte> || std::same_as<T, date_time_t> ||
                                          std::same_as<T, timedelta_t> || std::same_as<T, symbol> ||
                                          requires(value_type value) { std::hash<value_type>{}(value); };
    static constexpr bool uses_variable_memory = false;
    static constexpr bool requires_finalization = !std::is_trivially_destructible_v<stored_type>;
    static constexpr bool assign_uses_memory_resource = false;

    static value_type read(const stored_type &value) noexcept { return value; }

    static std::size_t hash(value_type value) noexcept {
        if constexpr (std::same_as<T, byte>) {
            return std::to_integer<std::size_t>(value);
        } else if constexpr (std::same_as<T, date_time_t>) {
            return std::hash<date_time_t::rep>{}(value.time_since_epoch().count());
        } else if constexpr (std::same_as<T, timedelta_t>) {
            return std::hash<timedelta_t::rep>{}(value.count());
        } else if constexpr (std::same_as<T, symbol>) {
            return std::hash<std::string_view>{}(value.text());
        } else {
            return std::hash<value_type>{}(value);
        }
    }

    static bool equals(value_type left, value_type right) noexcept { return left == right; }

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

namespace details {

inline std::size_t combine_scalar_hash(std::size_t result, std::size_t field) noexcept {
    constexpr auto constant = std::size_t{0x9e3779b9};
    return result ^ (field + constant + (result << 6) + (result >> 2));
}

template <native_struct T, std::size_t... Index>
std::size_t hash_native_struct(const T &value, std::index_sequence<Index...>) noexcept {
    const auto members = util::aggregate_members(value);
    auto result = std::size_t{};
    ((result = combine_scalar_hash(
          result,
          scalar_storage_traits<std::remove_cv_t<util::aggregate_member_type<Index, T>>>::hash(std::get<Index>(members)))),
     ...);
    return result;
}

template <native_struct T, std::size_t... Index>
bool equal_native_struct(const T &left, const T &right, std::index_sequence<Index...>) noexcept {
    const auto left_members = util::aggregate_members(left);
    const auto right_members = util::aggregate_members(right);
    return (scalar_storage_traits<std::remove_cv_t<util::aggregate_member_type<Index, T>>>::equals(
                std::get<Index>(left_members), std::get<Index>(right_members)) &&
            ...);
}

} // namespace details

template <native_struct T> struct scalar_storage_traits<T> {
    using stored_type = T;
    using value_type = details::scalar_value_t<T>;

    static constexpr bool supports_equality = true;
    static constexpr bool supports_hash = true;
    static constexpr bool uses_variable_memory = false;
    static constexpr bool requires_finalization = false;
    static constexpr bool assign_uses_memory_resource = false;

    static value_type read(const stored_type &value) noexcept { return value; }

    static std::size_t hash(const stored_type &value) noexcept {
        return details::hash_native_struct(value, std::make_index_sequence<util::aggregate_member_count<T>>{});
    }

    static bool equals(const stored_type &left, const stored_type &right) noexcept {
        return details::equal_native_struct(left, right,
                                            std::make_index_sequence<util::aggregate_member_count<T>>{});
    }

    static void construct(stored_type *destination, const stored_type &value, allocator &) {
        std::construct_at(destination, value);
    }

    static void assign(stored_type &destination, const stored_type &value, allocator &) {
        std::destroy_at(std::addressof(destination));
        std::construct_at(std::addressof(destination), value);
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

    static bool equals(value_type left, value_type right) noexcept { return left == right; }

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
