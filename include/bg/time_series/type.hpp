#pragma once

#include <bg/time_series/details/type_properties.hpp>

namespace bg {

struct TS_T {};

template <typename T, auto... Properties> struct TS {
  private:
    using ParsedProperties = details::time_series_properties<Properties...>;
    static_assert(ParsedProperties::valid);

  public:
    using value_type = T;

    static constexpr bool has_explicit_guarantee = ParsedProperties::has_guarantee;
    static constexpr TimeSeriesGuarantee guarantee = ParsedProperties::guarantee;
};

} // namespace bg
