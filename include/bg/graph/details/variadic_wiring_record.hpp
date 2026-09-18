#pragma once

#include <bg/allocator.hpp>
#include <bg/graph/details/wiring_record.hpp>
#include <bg/graph/variadic_parameter.hpp>
#include <bg/graph/wiring_argument.hpp>

#include <memory>
#include <span>
#include <vector>

namespace bg {
class TimeSeriesBundleType;
}

namespace bg::details {

class VariadicWiringRecord final : public WiringRecord {
    struct CapturedArguments {
        const TimeSeriesBundleType *type;
        std::vector<WiringArgument> arguments;
    };

  public:
    VariadicWiringRecord(CapturedArguments captured, VariadicMode mode);

    std::span<const WiringArgument> arguments() const noexcept { return arguments_; }
    VariadicMode mode() const noexcept { return mode_; }
    WiringPoint probe_point() const noexcept { return point(*this); }

    static CapturedArguments capture(VariadicMode mode, std::vector<WiringArgument> arguments,
                                     allocator *memory);

  private:
    WiringPoint materialize(const TimeSeriesType &target_type) const override;

    std::vector<WiringArgument> arguments_;
    VariadicMode mode_;
};

[[nodiscard]] const VariadicWiringRecord *variadic_wiring_record(WiringPoint point) noexcept;
[[nodiscard]] WiringPoint make_variadic_wiring_record(VariadicMode mode, std::vector<WiringArgument> arguments,
                                                      allocator &memory);
[[nodiscard]] std::unique_ptr<VariadicWiringRecord>
make_unregistered_variadic_wiring_record(VariadicMode mode, std::vector<WiringArgument> arguments);

} // namespace bg::details
