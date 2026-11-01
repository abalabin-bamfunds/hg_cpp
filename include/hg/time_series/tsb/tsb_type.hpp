#pragma once

#include <hg/schema/reflection.hpp>
#include <hg/time_series/details/time_series_properties.hpp>
#include <hg/time_series/details/type_access.hpp>
#include <hg/time_series/schema_variable.hpp>
#include <hg/time_series/tsb/schema_field.hpp>

namespace hg {

template <template <template <typename, auto...> typename> typename Schema, auto... Properties>
struct TSB : details::time_series_type_access<TSB<Schema, Properties...>> {
  private:
    using ParsedProperties = details::time_series_properties<Properties...>;
    static_assert(ParsedProperties::valid);

  public:
    template <template <typename, auto...> typename Field> using schema_type = Schema<Field>;

    static constexpr bool has_explicit_guarantee = ParsedProperties::has_guarantee;
    static constexpr auto guarantee = ParsedProperties::guarantee;
};

} // namespace hg

#include <hg/time_series/tsb/details/tsb_traits.hpp>
#include <hg/type/details/time_series_bundle_type_traits.hpp>
