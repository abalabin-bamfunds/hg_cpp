#pragma once

#include <hg/time_series/time_series_type.hpp>
#include <hg/type/details/time_series_type_traits.hpp>
#include <hg/type/time_series_value_type.hpp>
#include <hg/type/type_registry.hpp>
#include <hg/util/static_storage.hpp>

namespace hg::details {

template <typename T, auto... Properties> struct time_series_type_traits<TS<T, Properties...>> {
    using semantic_type = TimeSeriesValueType;

    static constexpr bool generic = scalar_declaration_traits<T>::generic;
    static constexpr bool supported =
        scalar_declaration_traits<T>::generic ||
        (scalar_declaration_traits<T>::supports_time_series && has_cpp_scalar_type<T>);

    template <typename Scope, std::size_t Index, TimeSeriesTypeRole Role>
    static const TimeSeriesType &get(GuaranteeParameter guarantee) {
        using Declaration = time_series_declaration_identity<time_series_type_traits, Scope, Index, Role>;
        const auto &value = scalar_type_for<T>();

        if constexpr (Role == TimeSeriesTypeRole::input || scalar_declaration_traits<T>::generic) {
            static const auto &type = TypeRegistry::instance().get_or_create_static<
                Declaration, GenericTimeSeriesValueType>([&]() -> const auto & {
                    static const auto type = util::static_storage<GenericTimeSeriesValueType>{value, guarantee};
                    return type.get();
                });
            return type;
        }
        if (!guarantee.is_resolved()) {
            static const auto &type = TypeRegistry::instance().get_or_create_static<
                Declaration, GenericTimeSeriesValueType>([&]() -> const auto & {
                    static const auto type = util::static_storage<GenericTimeSeriesValueType>{value, guarantee};
                    return type.get();
                });
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

} // namespace hg::details
