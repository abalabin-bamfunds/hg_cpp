#pragma once

#include <bg/scalar/storage_traits.hpp>
#include <bg/time_series/details/time_series_properties.hpp>
#include <bg/time_series/details/type_access.hpp>
#include <bg/time_series/time_series_variable.hpp>

#include <type_traits>

namespace bg {

// Parses the guarantee property accepted by a time series declaration.
template <auto... Properties>
using TimeSeriesDeclarationProperties = details::time_series_properties<Properties...>;

// Supplies declaration type() and guarantee metadata for a provider-defined time series declaration.
template <typename Declaration, auto... Properties>
struct TimeSeriesDeclaration : details::time_series_type_access<Declaration> {
  private:
    using ParsedProperties = TimeSeriesDeclarationProperties<Properties...>;
    static_assert(ParsedProperties::valid);

  public:
    static constexpr bool has_explicit_guarantee = ParsedProperties::has_guarantee;
    static constexpr auto guarantee = ParsedProperties::guarantee;
};

template <typename Target, auto... Properties>
struct REF : TimeSeriesDeclaration<REF<Target, Properties...>, Properties...> {
  public:
    using target_type = Target;
};

template <typename T, auto... Properties> struct TS : TimeSeriesDeclaration<TS<T, Properties...>, Properties...> {
  public:
    using value_type = T;
};

namespace details {

template <typename T> struct time_series_value {
    using type = void;
};

template <typename T, auto... Properties> struct time_series_value<TS<T, Properties...>> {
    using type = T;
};

template <typename T> using time_series_value_t = typename time_series_value<std::remove_cv_t<T>>::type;

template <typename TimeSeries>
inline constexpr bool time_series_scalar_supports_alternative_layouts = [] {
    using Value = time_series_value_t<TimeSeries>;
    if constexpr (std::is_void_v<Value>) {
        return false;
    } else {
        return scalar_storage_supports_alternative_layouts<Value>;
    }
}();

template <typename T> struct time_series_reference : std::false_type {};

template <typename Target, auto... Properties>
struct time_series_reference<REF<Target, Properties...>> : std::true_type {
    using target_type = Target;
};

template <typename T>
inline constexpr bool is_time_series_reference = time_series_reference<std::remove_cv_t<T>>::value;

template <typename T> struct dereferenced_time_series {
    using type = std::remove_cv_t<T>;
};

template <typename Target, auto... Properties>
struct dereferenced_time_series<REF<Target, Properties...>> {
    using type = Target;
};

template <typename T>
using dereferenced_time_series_t = typename dereferenced_time_series<std::remove_cv_t<T>>::type;

} // namespace details

} // namespace bg

#include <bg/type/details/time_series_reference_type_traits.hpp>
#include <bg/type/details/time_series_value_type_traits.hpp>
