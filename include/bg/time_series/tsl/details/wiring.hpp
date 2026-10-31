#pragma once

#include <bg/graph/details/variadic_wiring_operations.hpp>
#include <bg/graph/wiring_point.hpp>
#include <bg/time_series/tsl/tsl_type.hpp>

#include <span>

namespace bg {

class TimeSeriesListType;
class TimeSeriesType;

namespace details {

class VariadicWiringOperations;

WiringPoint make_free_fixed_time_series_list(std::span<const WiringPoint> items);
WiringPoint make_empty_fixed_time_series_list(const TimeSeriesType &item_type);
WiringPoint time_series_list_item(WiringPoint list, std::size_t index);
const VariadicWiringOperations &time_series_list_variadic_wiring_operations() noexcept;

template <typename Item, auto Size, auto... Properties>
struct variadic_wiring_operations_traits<TSL<Item, Size, Properties...>> {
    static const VariadicWiringOperations *get() noexcept {
        return &time_series_list_variadic_wiring_operations();
    }
};

} // namespace details
} // namespace bg
