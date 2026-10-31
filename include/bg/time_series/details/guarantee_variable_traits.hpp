#pragma once

#include <bg/time_series/guarantee_variable.hpp>
#include <bg/time_series/time_series_type.hpp>
#include <bg/time_series/tsb/tsb_type.hpp>
#include <bg/time_series/tsd/tsd_type.hpp>
#include <bg/time_series/tsl/tsl_type.hpp>
#include <bg/time_series/tss/tss_type.hpp>
#include <bg/type/details/time_series_bundle_type_traits.hpp>

#include <type_traits>
#include <utility>

namespace bg::details {

template <auto... Properties>
inline constexpr bool has_guarantee_variable_property =
    (false || ... || guarantee_variable_declaration<decltype(Properties)>);

template <typename TimeSeries> struct contains_guarantee_variable : std::false_type {};

template <typename T, auto... Properties>
struct contains_guarantee_variable<TS<T, Properties...>>
    : std::bool_constant<has_guarantee_variable_property<Properties...>> {};

template <typename K, auto... Properties>
struct contains_guarantee_variable<TSS<K, Properties...>>
    : std::bool_constant<has_guarantee_variable_property<Properties...>> {};

template <typename K, typename Item, auto... Properties>
struct contains_guarantee_variable<TSD<K, Item, Properties...>>
    : std::bool_constant<has_guarantee_variable_property<Properties...> ||
                         contains_guarantee_variable<Item>::value> {};

template <typename Item, auto Size, auto... Properties>
struct contains_guarantee_variable<TSL<Item, Size, Properties...>>
    : std::bool_constant<has_guarantee_variable_property<Properties...> ||
                         contains_guarantee_variable<Item>::value> {};

template <typename Target, auto... Properties>
struct contains_guarantee_variable<REF<Target, Properties...>>
    : std::bool_constant<has_guarantee_variable_property<Properties...> ||
                         contains_guarantee_variable<Target>::value> {};

template <template <template <typename, auto...> typename> typename Schema, std::size_t... Index>
consteval bool bundle_fields_contain_guarantee_variable(std::index_sequence<Index...>) {
    return (false || ... ||
            contains_guarantee_variable<direct_bundle_field_time_series<Index, Schema>>::value);
}

template <template <template <typename, auto...> typename> typename Schema>
consteval bool bundle_schema_contains_guarantee_variable() {
    if constexpr (bundle_schema_variable_declaration<Schema<bundle_schema_field>>) {
        return false;
    } else {
        constexpr auto fields_contain_guarantee_variable = bundle_fields_contain_guarantee_variable<Schema>(
            std::make_index_sequence<direct_bundle_field_count<Schema>>{});
        constexpr auto base_contains_guarantee_variable = [] {
            if constexpr (bundle_has_base<Schema>) {
                using Base = bundle_base_t<Schema>;
                return contains_guarantee_variable<TSB<Base::template apply>>::value;
            }
            return false;
        }();
        return fields_contain_guarantee_variable || base_contains_guarantee_variable;
    }
}

template <template <template <typename, auto...> typename> typename Schema, auto... Properties>
struct contains_guarantee_variable<TSB<Schema, Properties...>>
    : std::bool_constant<has_guarantee_variable_property<Properties...> ||
                         bundle_schema_contains_guarantee_variable<Schema>()> {};

template <typename TimeSeries>
inline constexpr bool contains_guarantee_variable_v =
    contains_guarantee_variable<std::remove_cv_t<TimeSeries>>::value;

} // namespace bg::details
