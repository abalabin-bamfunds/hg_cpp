#pragma once

#include <bg/graph/variadic_parameter.hpp>
#include <bg/util/symbol.hpp>

#include <cstddef>
#include <memory>
#include <span>
#include <vector>

namespace bg {

class allocator;
class TimeSeriesType;
class WiringArgument;
class WiringPoint;

namespace details {

struct VariadicCapture {
    const TimeSeriesType *type;
    std::vector<WiringArgument> arguments;
};

class VariadicWiringOperations {
  public:
    virtual ~VariadicWiringOperations() = default;

    virtual void validate_parameter(const TimeSeriesType &type, VariadicMode mode) const = 0;
    virtual VariadicCapture capture(const TimeSeriesType &parameter_type, VariadicMode mode,
                                    std::vector<WiringArgument> arguments, allocator *memory) const = 0;
    virtual bool accepts_direct(const TimeSeriesType &type) const noexcept = 0;
    virtual std::size_t item_count(const TimeSeriesType &type) const noexcept = 0;
    virtual util::symbol item_name(const TimeSeriesType &type, std::size_t index) const = 0;
    virtual const TimeSeriesType &item_type(const TimeSeriesType &type, std::size_t index) const = 0;
    virtual WiringPoint item(WiringPoint collection, std::size_t index) const = 0;
    virtual WiringPoint materialize(std::span<const WiringArgument> arguments,
                                    const TimeSeriesType &target_type) const = 0;
};

template <typename TimeSeries> struct variadic_wiring_operations_traits {
    static const VariadicWiringOperations *get() noexcept { return nullptr; }
};

template <typename TimeSeries>
const VariadicWiringOperations *variadic_wiring_operations() noexcept {
    return variadic_wiring_operations_traits<TimeSeries>::get();
}

} // namespace details
} // namespace bg
