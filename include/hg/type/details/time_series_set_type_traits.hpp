#pragma once

#include <bg/time_series/tss/tss_type.hpp>
#include <bg/type/details/time_series_type_traits.hpp>
#include <bg/type/time_series_set_type.hpp>
#include <bg/type/type_registry.hpp>
#include <bg/util/static_storage.hpp>

namespace bg::details {

template <typename Item, auto... Properties> struct time_series_type_traits<TSS<Item, Properties...>> {
    using semantic_type = TimeSeriesSetType;

    static constexpr bool generic = scalar_declaration_traits<Item>::generic;
    static constexpr bool supported =
        scalar_declaration_traits<Item>::generic ||
        (scalar_declaration_traits<Item>::supports_time_series && has_cpp_scalar_type<Item>);

    template <typename Scope, std::size_t Index, TimeSeriesTypeRole Role>
    static const TimeSeriesType &get(GuaranteeParameter guarantee) {
        const auto &item = scalar_type_for<Item>();

        if constexpr (Role == TimeSeriesTypeRole::input || scalar_declaration_traits<Item>::generic) {
            static const auto type = util::static_storage<GenericTimeSeriesSetType>{item, guarantee};
            [[maybe_unused]] static const auto &registration = TypeRegistry::register_static(type.get());
            return type.get();
        }
        if (!guarantee.is_resolved()) {
            static const auto type = util::static_storage<GenericTimeSeriesSetType>{item, guarantee};
            [[maybe_unused]] static const auto &registration = TypeRegistry::register_static(type.get());
            return type.get();
        }

        return time_series_set_type(item, *guarantee.value());
    }
};

template <typename Item, auto... Properties> struct declaration_variable_traits<TSS<Item, Properties...>> {
    template <typename Visitor> static void for_each(Visitor &&visitor) {
        for_each_declaration_variable<Item>(visitor);
        time_series_properties<Properties...>::for_each_variable(std::forward<Visitor>(visitor));
    }
};

} // namespace bg::details
