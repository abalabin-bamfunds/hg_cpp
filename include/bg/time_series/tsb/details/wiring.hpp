#pragma once

#include <bg/graph/wiring_point.hpp>
#include <bg/util/symbol.hpp>

#include <cstddef>
#include <optional>
#include <span>

namespace bg {

class TimeSeriesBundleType;

namespace details {

class WiringRecord;

struct NamedBundleWiringPoint {
    util::symbol name;
    WiringPoint point;
};

[[nodiscard]] WiringPoint make_free_time_series_bundle(std::span<const NamedBundleWiringPoint> fields);
[[nodiscard]] WiringPoint materialize_time_series_bundle(std::span<const NamedBundleWiringPoint> fields,
                                                         const TimeSeriesBundleType &type);
[[nodiscard]] std::optional<WiringPoint> free_time_series_bundle_field(const WiringRecord &record,
                                                                      std::size_t index);

} // namespace details
} // namespace bg
