#pragma once

#include <bg/time_series/time_series_type.hpp>
#include <bg/type/details/scalar_type.hpp>
#include <bg/type/details/time_series_type_traits.hpp>

namespace bg::details {

template <typename Declaration, bool TimeSeries = supported_time_series<Declaration>,
          bool Scalar = !scalar_declaration_traits<Declaration>::generic && has_cpp_scalar_type<Declaration> &&
                        scalar_declaration_traits<Declaration>::supports_time_series>
struct bundle_field_projection {};

template <typename Declaration, bool Scalar>
struct bundle_field_projection<Declaration, true, Scalar> {
    using time_series_type = Declaration;
};

template <typename Declaration>
struct bundle_field_projection<Declaration, false, true> {
    using time_series_type = TS<Declaration>;
};

template <typename Declaration, auto... Properties>
struct bundle_schema_field : bundle_field_projection<Declaration> {
    using declared_type = Declaration;

    template <template <typename, auto...> typename Field>
    struct rebound_field {
        using type = Field<typename bundle_field_projection<Declaration>::time_series_type, Properties...>;
    };

    template <template <typename, auto...> typename Field>
    using rebind = typename rebound_field<Field>::type;
};

template <> inline constexpr bool is_schema_reflection_field<bundle_schema_field> = true;

} // namespace bg::details
