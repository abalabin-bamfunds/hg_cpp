#pragma once

#include <bg/type/guarantee_parameter.hpp>
#include <bg/type/time_series_type.hpp>

namespace bg {

class TimeSeriesReferenceType : public TimeSeriesType {
  public:
    [[nodiscard]] const TimeSeriesType &target_type() const noexcept { return *target_type_; }

  protected:
    TimeSeriesReferenceType(const TimeSeriesType &target_type, GuaranteeParameter guarantee)
        : TimeSeriesType(guarantee), target_type_(&target_type) {}

  private:
    const TimeSeriesType *target_type_;
};

[[nodiscard]] const TimeSeriesReferenceType &time_series_reference_type(const TimeSeriesType &target_type,
                                                                        TimeSeriesGuarantee guarantee);

class GenericTimeSeriesReferenceType final : public TimeSeriesReferenceType {
  public:
    GenericTimeSeriesReferenceType(const TimeSeriesType &target_type, GuaranteeParameter guarantee)
        : TimeSeriesReferenceType(target_type, guarantee) {}

    [[nodiscard]] bool is_resolved() const noexcept override { return false; }
    [[nodiscard]] bool match(const Type &supplied, TypeMap &bindings) const override;
    [[nodiscard]] const Type &resolve(const TypeMap &bindings) const override;
};

namespace details {

[[nodiscard]] inline const Type &unwrap_time_series_reference(const Type &type) noexcept {
    const auto *reference = dynamic_cast<const TimeSeriesReferenceType *>(&type);
    return reference == nullptr ? type : reference->target_type();
}

} // namespace details

} // namespace bg
