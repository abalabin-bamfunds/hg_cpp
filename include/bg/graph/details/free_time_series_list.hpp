#pragma once

#include <bg/graph/wiring_point.hpp>

#include <concepts>
#include <span>

namespace bg {

class TimeSeriesListType;

namespace details {

struct FreeTimeSeriesListArgument {
    const TimeSeriesListType *type;
    std::span<const WiringPoint> items;
    WiringPoint (*materialize)(std::span<const WiringPoint>, const TimeSeriesListType &);
};

template <typename T>
concept free_time_series_list = requires(const T &value) {
    { value.wiring_argument() } -> std::same_as<FreeTimeSeriesListArgument>;
};

} // namespace details
} // namespace bg
