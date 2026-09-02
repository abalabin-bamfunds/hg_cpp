#pragma once

#include <bg/graph/wiring_point.hpp>
#include <bg/util/symbol.hpp>

#include <concepts>
#include <span>

namespace bg {

class TimeSeriesBundleType;

namespace details {

struct NamedBundleWiringPoint {
    util::symbol name;
    WiringPoint point;
};

struct FreeTimeSeriesBundleArgument {
    const TimeSeriesBundleType *type;
    std::span<const NamedBundleWiringPoint> fields;
    WiringPoint (*materialize)(std::span<const NamedBundleWiringPoint>, const TimeSeriesBundleType &);
};

template <typename T>
concept free_time_series_bundle = requires(const T &value) {
    { value.wiring_argument() } -> std::same_as<FreeTimeSeriesBundleArgument>;
};

} // namespace details
} // namespace bg
