#pragma once

#include <bg/time_series/tsb/tsb_type.hpp>
#include <bg/type/details/time_series_type_traits.hpp>
#include <bg/type/schema_variable_instance.hpp>
#include <bg/type/time_series_bundle_type.hpp>
#include <bg/type/type_registry.hpp>

#include <type_traits>
#include <vector>

namespace bg::details {

template <typename Scope, TimeSeriesTypeRole Role,
          template <template <typename, auto...> typename> typename Schema,
          std::size_t... Index>
std::vector<SchemaField> direct_bundle_fields(GuaranteeParameter guarantee, std::index_sequence<Index...>) {
    auto fields = std::vector<SchemaField>{};
    fields.reserve(sizeof...(Index));
    (fields.push_back({util::symbol{direct_bundle_field_name<Index, Schema>()},
                       &time_series_type_for<Scope, direct_bundle_field_logical_index<Index, Schema>, Role,
                                             direct_bundle_field_time_series<Index, Schema>>(guarantee)}),
     ...);
    return fields;
}

template <typename Scope, TimeSeriesTypeRole Role,
          template <template <typename, auto...> typename> typename Schema>
const bg::Schema &cpp_bundle_schema(GuaranteeParameter guarantee) {
    const bg::Schema *base = nullptr;
    if constexpr (bundle_has_base<Schema>) {
        using Base = bundle_base_t<Schema>;
        base = &cpp_bundle_schema<Scope, Role, Base::template apply>(guarantee);
    }

    const auto fields = direct_bundle_fields<Scope, Role, Schema>(
        guarantee, std::make_index_sequence<direct_bundle_field_count<Schema>>{});
    return bg::schema(util::symbol{util::unqualified_type_name<Schema<bundle_schema_field>>()},
                      std::type_index{typeid(schema_template<Schema>)}, base, fields);
}

template <template <template <typename, auto...> typename> typename Schema, bool HasBase>
struct bundle_base_type_traits {
    static constexpr bool generic = false;
    static constexpr bool supported = true;
};

template <template <template <typename, auto...> typename> typename Schema>
struct bundle_base_type_traits<Schema, true> {
    using Base = bundle_base_t<Schema>;
    using Traits = time_series_type_traits<TSB<Base::template apply>>;

    static constexpr bool generic = Traits::generic;
    static constexpr bool supported = Traits::supported;
};

template <typename Field, typename = void> struct bundle_field_type_traits {
    static constexpr bool generic = false;
    static constexpr bool supported = false;
};

template <typename Field>
struct bundle_field_type_traits<Field, std::void_t<typename Field::time_series_type>> {
    using TimeSeries = typename Field::time_series_type;

    static constexpr bool generic = generic_time_series<TimeSeries>;
    static constexpr bool supported = supported_time_series<TimeSeries>;
};

template <template <template <typename, auto...> typename> typename Schema, auto... Properties>
struct time_series_type_traits<TSB<Schema, Properties...>> {
    using semantic_type = TimeSeriesBundleType;

    static constexpr bool schema_variable = bundle_schema_variable_declaration<Schema<bundle_schema_field>>;
    using BaseTraits = bundle_base_type_traits<Schema, !schema_variable && bundle_has_base<Schema>>;

    template <std::size_t... Index> static consteval bool fields_generic(std::index_sequence<Index...>) {
        return (false || ... || bundle_field_type_traits<direct_bundle_field<Index, Schema>>::generic);
    }

    template <std::size_t... Index> static consteval bool fields_supported(std::index_sequence<Index...>) {
        return (true && ... && bundle_field_type_traits<direct_bundle_field<Index, Schema>>::supported);
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

    template <typename Scope, std::size_t, TimeSeriesTypeRole Role>
    static const TimeSeriesType &get(GuaranteeParameter guarantee) {
        if constexpr (schema_variable) {
            static const auto type = GenericTimeSeriesBundleType{schema_variable_instance<Schema>(), guarantee};
            [[maybe_unused]] static const auto &registration = TypeRegistry::register_static(type);
            return type;
        } else {
            const auto &schema = cpp_bundle_schema<Scope, Role, Schema>(guarantee);
            if constexpr (Role == TimeSeriesTypeRole::input || generic) {
                static const auto type = GenericTimeSeriesBundleType{schema, guarantee};
                [[maybe_unused]] static const auto &registration = TypeRegistry::register_static(type);
                return type;
            }
            if (!schema.is_resolved() || !guarantee.is_resolved()) {
                static const auto type = GenericTimeSeriesBundleType{schema, guarantee};
                [[maybe_unused]] static const auto &registration = TypeRegistry::register_static(type);
                return type;
            }
            return time_series_bundle_type(schema, *guarantee.value());
        }
    }
};

template <template <template <typename, auto...> typename> typename Schema, std::size_t... Index,
          typename Visitor>
void for_each_bundle_field_variable(std::index_sequence<Index...>, Visitor &&visitor) {
    (for_each_declaration_variable<direct_bundle_field_time_series<Index, Schema>>(visitor), ...);
}

template <template <template <typename, auto...> typename> typename Schema, auto... Properties>
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
        time_series_properties<Properties...>::for_each_variable(std::forward<Visitor>(visitor));
    }
};

} // namespace bg::details

namespace bg::details {

template <template <template <typename, auto...> typename> typename Schema>
const SchemaType &time_series_schema_type() {
    return TSB<Schema>::type().schema_type();
}

} // namespace bg::details
