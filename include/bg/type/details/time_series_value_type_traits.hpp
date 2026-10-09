#pragma once

#include <bg/time_series/time_series_type.hpp>
#include <bg/type/details/time_series_type_traits.hpp>
#include <bg/type/time_series_value_type.hpp>
#include <bg/type/type_registry.hpp>

namespace bg::details {

template <typename T, auto... Properties> struct time_series_type_traits<TS<T, Properties...>> {
    using semantic_type = TimeSeriesValueType;

    static constexpr bool generic = scalar_declaration_traits<T>::generic;
    static constexpr bool supported =
        scalar_declaration_traits<T>::generic ||
        (scalar_declaration_traits<T>::supports_time_series && has_cpp_scalar_type<T>);

    template <typename Scope, std::size_t Index, TimeSeriesTypeRole Role>
    static const TimeSeriesType &get(GuaranteeParameter guarantee) {
        const auto &value = scalar_type_for<T>();

        if constexpr (Role == TimeSeriesTypeRole::input || scalar_declaration_traits<T>::generic) {
            static const auto type = GenericTimeSeriesValueType{value, guarantee};
            [[maybe_unused]] static const auto &registration = TypeRegistry::register_static(type);
            return type;
        }
        if (!guarantee.is_resolved()) {
            static const auto type = GenericTimeSeriesValueType{value, guarantee};
            [[maybe_unused]] static const auto &registration = TypeRegistry::register_static(type);
            return type;
        }

        return time_series_value_type(value, *guarantee.value());
    }
};

template <typename T, auto... Properties> struct declaration_variable_traits<TS<T, Properties...>> {
    template <typename Visitor> static void for_each(Visitor &&visitor) {
        for_each_declaration_variable<T>(visitor);
        time_series_properties<Properties...>::for_each_variable(std::forward<Visitor>(visitor));
    }
};

} // namespace bg::details
