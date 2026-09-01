#pragma once

#include <bg/time_series/tsl/tsl_type.hpp>
#include <bg/type/details/time_series_type.hpp>
#include <bg/type/time_series_list_type.hpp>

namespace bg::details {

template <typename Item, std::size_t Size, auto... Properties>
struct time_series_type_traits<TSL<Item, Size, Properties...>> {
    static constexpr bool generic = generic_time_series<Item>::value;
    static constexpr bool supported = supported_time_series<Item>::value;

    template <typename Scope, std::size_t Index, bool Input>
    [[nodiscard]] static const TimeSeriesType &get(GuaranteeParameter guarantee) {
        static_assert(Size != std::dynamic_extent, "variable-size time series lists are not implemented");
        const auto &item = time_series_type_for<Scope, Index, Input, Item>(guarantee);

        if constexpr (Input) {
            static const auto type = GenericTimeSeriesListType{item, Size, guarantee};
            return type;
        }
        if (!item.is_resolved() || !guarantee.is_resolved()) {
            static const auto type = GenericTimeSeriesListType{item, Size, guarantee};
            return type;
        }

        return time_series_list_type(item, Size, *guarantee.value());
    }
};

} // namespace bg::details
