#pragma once

#include <bg/allocator.hpp>
#include <bg/graph/details/variadic_wiring_operations.hpp>
#include <bg/graph/details/wiring_argument_descriptor.hpp>
#include <bg/graph/details/wiring_record.hpp>
#include <bg/graph/wiring_argument.hpp>

#include <memory>
#include <optional>
#include <vector>

namespace bg::details {

class VariadicWiringRecord final : public WiringRecord {
  public:
    VariadicWiringRecord(const TimeSeriesType &parameter_type, VariadicMode mode,
                         const VariadicWiringOperations &operations, std::vector<WiringArgument> arguments,
                         allocator *memory);
    VariadicWiringRecord(WiringPoint collection, VariadicMode mode,
                         const VariadicWiringOperations &operations, allocator *memory = nullptr);

    std::size_t size() const noexcept;
    util::symbol item_name(std::size_t index) const;
    WiringArgument argument(std::size_t index) const;
    std::vector<WiringArgumentDescriptor> argument_descriptors() const;
    VariadicMode mode() const noexcept { return mode_; }
    std::optional<WiringPoint> direct_collection() const noexcept { return direct_; }
    bool materialisation_ready_impl() const noexcept override;

  private:
    VariadicWiringRecord(VariadicCapture captured, VariadicMode mode,
                         const VariadicWiringOperations &operations);

    MaterialisationPreparation prepare_materialisation(const TimeSeriesType &target_type) const override;
    WiringPoint materialize(const TimeSeriesType &target_type) const override;

    const VariadicWiringOperations *operations_;
    std::vector<WiringArgument> arguments_;
    std::optional<WiringPoint> direct_;
    VariadicMode mode_;
};

const VariadicWiringRecord *variadic_wiring_record(WiringPoint point) noexcept;

} // namespace bg::details
