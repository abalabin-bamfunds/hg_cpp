#pragma once

#include <bg/scalar/builtins.hpp>
#include <bg/util/reflection.hpp>

#include <concepts>
#include <type_traits>
#include <utility>

namespace bg {

namespace details {

template <typename T>
concept native_struct_atomic = std::same_as<std::remove_cv_t<T>, byte> ||
                               std::same_as<std::remove_cv_t<T>, bool> ||
                               std::same_as<std::remove_cv_t<T>, i64> ||
                               std::same_as<std::remove_cv_t<T>, double> ||
                               std::same_as<std::remove_cv_t<T>, date_time_t> ||
                               std::same_as<std::remove_cv_t<T>, timedelta_t> ||
                               std::same_as<std::remove_cv_t<T>, symbol>;

template <typename T> consteval bool valid_native_struct();

template <typename T>
consteval bool valid_native_struct_member() {
    using Member = std::remove_cv_t<T>;

    if constexpr (native_struct_atomic<Member>) {
        return true;
    } else {
        return valid_native_struct<Member>();
    }
}

template <typename T, std::size_t... Index>
consteval bool valid_native_struct_members(std::index_sequence<Index...>) {
    return (valid_native_struct_member<util::aggregate_member_type<Index, T>>() && ...);
}

template <typename T>
consteval bool valid_native_struct() {
    using Struct = std::remove_cv_t<T>;

    if constexpr (!std::is_class_v<Struct> || std::is_empty_v<Struct> || !std::is_aggregate_v<Struct> ||
                  !std::is_standard_layout_v<Struct> || !std::is_trivially_copyable_v<Struct>) {
        return false;
    } else if constexpr (util::detail::aggregate_initializable<Struct>(
                             std::make_index_sequence<util::detail::maximum_aggregate_members + 1>{})) {
        return false;
    } else {
        constexpr auto field_count = util::aggregate_member_count<Struct>;
        return field_count > 0 && valid_native_struct_members<Struct>(std::make_index_sequence<field_count>{});
    }
}

} // namespace details

template <typename T>
concept native_struct = details::valid_native_struct<std::remove_cvref_t<T>>();

} // namespace bg
