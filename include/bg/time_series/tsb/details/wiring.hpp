#pragma once

#include <bg/graph/details/variadic_wiring_operations.hpp>
#include <bg/graph/wiring_point.hpp>
#include <bg/time_series/tsb/tsb_type.hpp>
#include <bg/util/symbol.hpp>

#include <cstddef>
#include <span>

namespace bg {

class TimeSeriesBundleType;

namespace details {

class VariadicWiringOperations;

struct NamedBundleWiringPoint {
    util::symbol name;
    WiringPoint point;
};

[[nodiscard]] WiringPoint make_free_time_series_bundle(std::span<const NamedBundleWiringPoint> fields);
[[nodiscard]] WiringPoint materialize_time_series_bundle(std::span<const NamedBundleWiringPoint> fields,
                                                         const TimeSeriesBundleType &type);
[[nodiscard]] WiringPoint time_series_bundle_field(WiringPoint bundle, std::size_t index);
[[nodiscard]] WiringPoint time_series_bundle_field(WiringPoint bundle, util::symbol name);
const VariadicWiringOperations &time_series_bundle_variadic_wiring_operations() noexcept;

template <template <template <typename> typename> typename Schema, auto... Properties>
struct variadic_wiring_operations_traits<TSB<Schema, Properties...>> {
    static const VariadicWiringOperations *get() noexcept {
        return &time_series_bundle_variadic_wiring_operations();
    }
};

} // namespace details
} // namespace bg
