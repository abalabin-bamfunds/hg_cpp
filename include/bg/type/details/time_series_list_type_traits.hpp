#pragma once

#include <bg/time_series/tsl/tsl_type.hpp>
#include <bg/type/details/time_series_type_traits.hpp>
#include <bg/type/size_variable_instance.hpp>
#include <bg/type/time_series_list_type.hpp>

namespace bg::details {

template <typename Item, auto Size, auto... Properties> struct time_series_type_traits<TSL<Item, Size, Properties...>> {
    static constexpr bool generic = generic_time_series<Item> || size_variable_declaration<decltype(Size)>;
    static constexpr bool supported = supported_time_series<Item>;

    template <typename Scope, std::size_t Index, bool Input>
    static const TimeSeriesType &get(GuaranteeParameter guarantee) {
        if constexpr (!size_variable_declaration<decltype(Size)>) {
            static_assert(Size != std::dynamic_extent, "variable-size time series lists are not implemented");
        }
        const auto &item = time_series_type_for<Scope, Index, Input, Item>(guarantee);
        const auto size = size_parameter<Scope, Size>();

        if constexpr (Input) {
            static const auto type = GenericTimeSeriesListType{item, size, guarantee};
            return type;
        }
        if (!item.is_resolved() || !size.is_resolved() || !guarantee.is_resolved()) {
            static const auto type = GenericTimeSeriesListType{item, size, guarantee};
            return type;
        }

        return time_series_list_type(item, *size.value(), *guarantee.value());
    }
};

} // namespace bg::details
