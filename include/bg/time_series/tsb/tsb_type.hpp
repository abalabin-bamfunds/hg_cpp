#pragma once

#include <bg/time_series/details/time_series_properties.hpp>
#include <bg/time_series/schema_variable.hpp>

namespace bg {

namespace details {

template <typename TimeSeries> struct bundle_schema_field {
    using time_series_type = TimeSeries;
};

template <template <typename> typename Field> inline constexpr bool is_bundle_schema_field = false;

template <> inline constexpr bool is_bundle_schema_field<bundle_schema_field> = true;

template <template <template <typename> typename> typename Base> struct bundle_base_marker {};

} // namespace details

template <template <template <typename> typename> typename Base, template <typename> typename Field,
          bool Schema = details::is_bundle_schema_field<Field>>
struct bundle_extends;

template <template <template <typename> typename> typename Base, template <typename> typename Field>
struct bundle_extends<Base, Field, false> : Base<Field> {};

template <template <template <typename> typename> typename Base, template <typename> typename Field>
struct bundle_extends<Base, Field, true> : details::bundle_base_marker<Base> {};

template <template <template <typename> typename> typename Schema, auto... Properties> struct TSB {
  private:
    using ParsedProperties = details::time_series_properties<Properties...>;
    static_assert(ParsedProperties::valid);

  public:
    template <template <typename> typename Field> using schema_type = Schema<Field>;

    static constexpr bool has_explicit_guarantee = ParsedProperties::has_guarantee;
    static constexpr auto guarantee = ParsedProperties::guarantee;
};

} // namespace bg

#include <bg/time_series/tsb/details/tsb_traits.hpp>
