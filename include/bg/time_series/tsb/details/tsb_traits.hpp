#pragma once

#include <bg/schema/reflection.hpp>

#include <cstddef>
#include <string_view>
#include <type_traits>

namespace bg::details {

template <template <template <typename, auto...> typename> typename Schema>
using bundle_schema_reflection = schema_reflection<Schema, bundle_schema_field>;

template <template <template <typename, auto...> typename> typename Schema>
using bundle_base_t = typename bundle_schema_reflection<Schema>::base_schema;

template <template <template <typename, auto...> typename> typename Schema>
inline constexpr bool bundle_has_base = bundle_schema_reflection<Schema>::has_base;

template <template <template <typename, auto...> typename> typename Schema>
inline constexpr std::size_t direct_bundle_field_count = bundle_schema_reflection<Schema>::direct_field_count;

template <std::size_t Index, template <template <typename, auto...> typename> typename Schema>
using direct_bundle_field = typename bundle_schema_reflection<Schema>::template direct_field<Index>;

template <std::size_t Index, template <template <typename, auto...> typename> typename Schema>
using direct_bundle_field_time_series = typename direct_bundle_field<Index, Schema>::time_series_type;

template <std::size_t Index, template <template <typename, auto...> typename> typename Schema>
consteval std::string_view direct_bundle_field_name() {
    return bundle_schema_reflection<Schema>::template direct_field_name<Index>();
}

template <typename T> struct time_series_bundle : std::false_type {};

template <template <template <typename, auto...> typename> typename Schema, auto... Properties>
struct time_series_bundle<TSB<Schema, Properties...>> : std::true_type {
    template <template <typename, auto...> typename Field> using schema_type = Schema<Field>;
};

template <typename T> inline constexpr bool is_time_series_bundle = time_series_bundle<std::remove_cv_t<T>>::value;

inline constexpr auto missing_bundle_field = missing_schema_field;

template <template <template <typename, auto...> typename> typename Schema>
inline constexpr std::size_t bundle_field_count = bundle_schema_reflection<Schema>::field_count;

template <std::size_t Index, template <template <typename, auto...> typename> typename Schema>
inline constexpr std::size_t direct_bundle_field_logical_index =
    bundle_schema_reflection<Schema>::direct_field_logical_index(Index);

template <util::compile_time_string Name, template <template <typename, auto...> typename> typename Schema>
inline constexpr std::size_t direct_bundle_field_index =
    bundle_schema_reflection<Schema>::direct_field_index(Name.view());

template <util::compile_time_string Name, template <template <typename, auto...> typename> typename Schema>
inline constexpr std::size_t bundle_field_index = bundle_schema_reflection<Schema>::field_index(Name.view());

template <util::compile_time_string Name, template <template <typename, auto...> typename> typename Schema>
using bundle_field_time_series =
    typename bundle_schema_reflection<Schema>::template field_type<Name>::time_series_type;

template <template <template <typename, auto...> typename> typename Schema,
          template <typename, auto...> typename Field, typename Factory>
[[nodiscard]] Schema<Field> make_bundle_fields(Factory &factory) {
    return bundle_schema_reflection<Schema>::template make<Field>(factory);
}

} // namespace bg::details
