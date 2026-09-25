#pragma once

#include <bg/time_series/tsb/tsb_type.hpp>
#include <bg/type/details/time_series_type_traits.hpp>
#include <bg/type/schema_variable_instance.hpp>
#include <bg/type/time_series_bundle_type.hpp>

#include <vector>

namespace bg::details {

template <typename Schema> inline constexpr int bundle_schema_identity = 0;

template <typename Scope, bool Input, template <template <typename> typename> typename Schema, std::size_t... Index>
std::vector<BundleField> direct_bundle_fields(GuaranteeParameter guarantee, std::index_sequence<Index...>) {
    auto fields = std::vector<BundleField>{};
    fields.reserve(sizeof...(Index));
    (fields.push_back({util::symbol{direct_bundle_field_name<Index, Schema>()},
                       &time_series_type_for<Scope, direct_bundle_field_logical_index<Index, Schema>, Input,
                                             direct_bundle_field_time_series<Index, Schema>>(guarantee)}),
     ...);
    return fields;
}

template <typename Scope, bool Input, template <template <typename> typename> typename Schema>
const BundleSchema &cpp_bundle_schema(GuaranteeParameter guarantee) {
    const BundleSchema *base = nullptr;
    if constexpr (bundle_has_base<Schema>) {
        using Base = bundle_base_t<Schema>;
        base = &cpp_bundle_schema<Scope, Input, Base::template apply>(guarantee);
    }

    const auto fields = direct_bundle_fields<Scope, Input, Schema>(
        guarantee, std::make_index_sequence<direct_bundle_field_count<Schema>>{});
    return bundle_schema(util::symbol{util::unqualified_type_name<Schema<bundle_schema_field>>()},
                         &bundle_schema_identity<Schema<bundle_schema_field>>, base, fields);
}

template <template <template <typename> typename> typename Schema, bool HasBase> struct bundle_base_type_traits {
    static constexpr bool generic = false;
    static constexpr bool supported = true;
};

template <template <template <typename> typename> typename Schema> struct bundle_base_type_traits<Schema, true> {
    using Base = bundle_base_t<Schema>;
    using Traits = time_series_type_traits<TSB<Base::template apply>>;

    static constexpr bool generic = Traits::generic;
    static constexpr bool supported = Traits::supported;
};

template <template <template <typename> typename> typename Schema, auto... Properties>
struct time_series_type_traits<TSB<Schema, Properties...>> {
    using semantic_type = TimeSeriesBundleType;

    static constexpr bool schema_variable = bundle_schema_variable_declaration<Schema<bundle_schema_field>>;
    using BaseTraits = bundle_base_type_traits<Schema, !schema_variable && bundle_has_base<Schema>>;

    template <std::size_t... Index> static consteval bool fields_generic(std::index_sequence<Index...>) {
        return (false || ... || generic_time_series<direct_bundle_field_time_series<Index, Schema>>);
    }

    template <std::size_t... Index> static consteval bool fields_supported(std::index_sequence<Index...>) {
        return (true && ... && supported_time_series<direct_bundle_field_time_series<Index, Schema>>);
    }

    static constexpr bool generic = [] {
        if constexpr (schema_variable) {
            return true;
        } else {
            return BaseTraits::generic || fields_generic(std::make_index_sequence<direct_bundle_field_count<Schema>>{});
        }
    }();
    static constexpr bool supported = [] {
        if constexpr (schema_variable) {
            return true;
        } else {
            return BaseTraits::supported &&
                   fields_supported(std::make_index_sequence<direct_bundle_field_count<Schema>>{});
        }
    }();

    template <typename Scope, std::size_t, bool Input> static const TimeSeriesType &get(GuaranteeParameter guarantee) {
        if constexpr (schema_variable) {
            static const auto type = GenericTimeSeriesBundleType{schema_variable_instance<Schema>(), guarantee};
            return type;
        } else {
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
    }
};

template <template <template <typename> typename> typename Schema, std::size_t... Index, typename Visitor>
void for_each_bundle_field_variable(std::index_sequence<Index...>, Visitor &&visitor) {
    (for_each_declaration_variable<direct_bundle_field_time_series<Index, Schema>>(visitor), ...);
}

template <template <template <typename> typename> typename Schema, auto... Properties>
struct declaration_variable_traits<TSB<Schema, Properties...>> {
    template <typename Visitor> static void for_each(Visitor &&visitor) {
        if constexpr (bundle_schema_variable_declaration<Schema<bundle_schema_field>>) {
            using Declaration = schema_type<Schema>;
            visitor.template operator()<Declaration>(schema_variable_instance<Schema>());
        } else {
            if constexpr (bundle_has_base<Schema>) {
                using Base = bundle_base_t<Schema>;
                for_each_declaration_variable<TSB<Base::template apply>>(visitor);
            }
            for_each_bundle_field_variable<Schema>(std::make_index_sequence<direct_bundle_field_count<Schema>>{},
                                                   visitor);
        }
        (for_each_property_variable<Properties>(visitor), ...);
    }
};

} // namespace bg::details

namespace bg {

template <template <template <typename> typename> typename Schema>
const BundleSchemaType &schema_type<Schema>::type() {
    return TSB<Schema>::type().schema_type();
}

} // namespace bg
