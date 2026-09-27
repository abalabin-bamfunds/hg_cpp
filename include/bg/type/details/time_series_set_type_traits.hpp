#pragma once

#include <bg/time_series/tss/tss_type.hpp>
#include <bg/type/details/time_series_type_traits.hpp>
#include <bg/type/time_series_set_type.hpp>

namespace bg::details {

template <typename Item, auto... Properties> struct time_series_type_traits<TSS<Item, Properties...>> {
    using semantic_type = TimeSeriesSetType;

    static constexpr bool generic = generic_scalar<Item>;
    static constexpr bool supported =
        generic_scalar<Item> || (scalar_declaration_traits<Item>::supports_time_series && has_cpp_scalar_type<Item>);

    template <typename Scope, std::size_t Index, bool Input>
    static const TimeSeriesType &get(GuaranteeParameter guarantee) {
        const auto &item = scalar_type_for<Item>();

        if constexpr (Input || generic_scalar<Item>) {
            static const auto type = GenericTimeSeriesSetType{item, guarantee};
            return type;
        }
        if (!guarantee.is_resolved()) {
            static const auto type = GenericTimeSeriesSetType{item, guarantee};
            return type;
        }

        return time_series_set_type(item, *guarantee.value());
    }
};

template <typename Item, auto... Properties> struct declaration_variable_traits<TSS<Item, Properties...>> {
    template <typename Visitor> static void for_each(Visitor &&visitor) {
        for_each_declaration_variable<Item>(visitor);
        (for_each_property_variable<Properties>(visitor), ...);
    }
};

} // namespace bg::details
