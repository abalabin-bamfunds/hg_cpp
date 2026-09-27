#pragma once

namespace bg::details {

template <typename TimeSeries, auto... Properties> struct bundle_schema_field {
    using time_series_type = TimeSeries;

    template <template <typename, auto...> typename Field>
    using rebind = Field<TimeSeries, Properties...>;
};

template <template <typename, auto...> typename Field> inline constexpr bool is_bundle_schema_field = false;

template <> inline constexpr bool is_bundle_schema_field<bundle_schema_field> = true;

} // namespace bg::details
