#pragma once

#include <bg/graph/details/variadic_wiring_operations.hpp>
#include <bg/graph/wiring_argument.hpp>
#include <bg/graph/wiring_point.hpp>
#include <bg/time_series/tsb/tsb_type.hpp>
#include <bg/util/symbol.hpp>

#include <cstddef>
#include <span>

namespace bg {

class TimeSeriesBundleType;

namespace details {

class VariadicWiringOperations;

[[nodiscard]] WiringPoint make_free_time_series_bundle(std::span<const WiringArgument> fields);
[[nodiscard]] WiringPoint materialize_time_series_bundle(std::span<const WiringArgument> fields,
                                                         const TimeSeriesBundleType &type);
[[nodiscard]] WiringPoint time_series_bundle_field(WiringPoint bundle, std::size_t index);
[[nodiscard]] WiringPoint time_series_bundle_field(WiringPoint bundle, util::symbol name);
[[nodiscard]] WiringArgument time_series_bundle_argument(WiringPoint bundle, std::size_t index);
const VariadicWiringOperations &time_series_bundle_variadic_wiring_operations() noexcept;

template <template <template <typename, auto...> typename> typename Schema, auto... Properties>
struct variadic_wiring_operations_traits<TSB<Schema, Properties...>> {
    static const VariadicWiringOperations *get() noexcept {
        return &time_series_bundle_variadic_wiring_operations();
    }
};

} // namespace details
} // namespace bg
