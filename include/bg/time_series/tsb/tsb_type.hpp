#pragma once

#include <bg/schema/reflection.hpp>
#include <bg/time_series/details/time_series_properties.hpp>
#include <bg/time_series/details/type_access.hpp>
#include <bg/time_series/schema_variable.hpp>
#include <bg/time_series/tsb/schema_field.hpp>

namespace bg {

template <template <template <typename, auto...> typename> typename Base,
          template <typename, auto...> typename Field,
          bool Schema = details::is_bundle_schema_field<Field>>
struct bundle_extends;

template <template <template <typename, auto...> typename> typename Base,
          template <typename, auto...> typename Field>
struct bundle_extends<Base, Field, false> : Base<Field> {};

template <template <template <typename, auto...> typename> typename Base,
          template <typename, auto...> typename Field>
struct bundle_extends<Base, Field, true> : details::schema_base_marker<Base> {};

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

} // namespace bg

#include <bg/time_series/tsb/details/tsb_traits.hpp>
#include <bg/type/details/time_series_bundle_type_traits.hpp>
