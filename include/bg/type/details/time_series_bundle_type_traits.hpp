#pragma once

#include <bg/time_series/tsb/tsb_type.hpp>
#include <bg/type/details/time_series_type.hpp>
#include <bg/type/time_series_bundle_type.hpp>

#include <vector>

namespace bg::details {

template <template <template <typename> typename> typename Schema> inline constexpr int bundle_schema_identity = 0;

template <typename Scope, bool Input, template <template <typename> typename> typename Schema, std::size_t... Index>
[[nodiscard]] std::vector<BundleField> direct_bundle_fields(GuaranteeParameter guarantee,
                                                            std::index_sequence<Index...>) {
    auto fields = std::vector<BundleField>{};
    fields.reserve(sizeof...(Index));
    (fields.push_back({util::symbol{direct_bundle_field_name<Index, Schema>()},
                       &time_series_type_for<Scope, direct_bundle_field_logical_index<Index, Schema>, Input,
                                             direct_bundle_field_time_series<Index, Schema>>(guarantee)}),
     ...);
    return fields;
}

template <typename Scope, bool Input, template <template <typename> typename> typename Schema>
[[nodiscard]] const BundleSchema &cpp_bundle_schema(GuaranteeParameter guarantee) {
    const BundleSchema *base = nullptr;
    if constexpr (bundle_has_base<Schema>) {
        using Base = bundle_base_t<Schema>;
        base = &cpp_bundle_schema<Scope, Input, Base::template apply>(guarantee);
    }

    const auto fields = direct_bundle_fields<Scope, Input, Schema>(
        guarantee, std::make_index_sequence<direct_bundle_field_count<Schema>>{});
    return bundle_schema(util::symbol{util::unqualified_type_name<Schema<bundle_schema_field>>()},
                         &bundle_schema_identity<Schema>, base, fields);
}

template <template <template <typename> typename> typename Schema, auto... Properties>
struct time_series_type_traits<TSB<Schema, Properties...>> {
    template <std::size_t... Index> [[nodiscard]] static consteval bool fields_generic(std::index_sequence<Index...>) {
        return (false || ... || generic_time_series<direct_bundle_field_time_series<Index, Schema>>::value);
    }

    template <std::size_t... Index>
    [[nodiscard]] static consteval bool fields_supported(std::index_sequence<Index...>) {
        return (true && ... && supported_time_series<direct_bundle_field_time_series<Index, Schema>>::value);
    }

    static constexpr bool base_generic = [] {
        if constexpr (bundle_has_base<Schema>) {
            using Base = bundle_base_t<Schema>;
            return time_series_type_traits<TSB<Base::template apply>>::generic;
        }
        return false;
    }();

    static constexpr bool base_supported = [] {
        if constexpr (bundle_has_base<Schema>) {
            using Base = bundle_base_t<Schema>;
            return time_series_type_traits<TSB<Base::template apply>>::supported;
        }
        return true;
    }();

    static constexpr bool generic =
        base_generic || fields_generic(std::make_index_sequence<direct_bundle_field_count<Schema>>{});
    static constexpr bool supported =
        base_supported && fields_supported(std::make_index_sequence<direct_bundle_field_count<Schema>>{});

    template <typename Scope, std::size_t, bool Input>
    [[nodiscard]] static const TimeSeriesType &get(GuaranteeParameter guarantee) {
        const auto &schema = cpp_bundle_schema<Scope, Input, Schema>(guarantee);
        if constexpr (Input || generic) {
            static const auto type = GenericTimeSeriesBundleType{schema, guarantee};
            return type;
        }
        if (!guarantee.is_resolved()) {
            static const auto type = GenericTimeSeriesBundleType{schema, guarantee};
            return type;
        }
        return time_series_bundle_type(schema, *guarantee.value());
    }
};

} // namespace bg::details
