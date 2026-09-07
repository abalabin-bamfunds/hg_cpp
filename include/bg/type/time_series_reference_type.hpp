#pragma once

#include <bg/type/guarantee_parameter.hpp>
#include <bg/type/time_series_type.hpp>

namespace bg {

class TimeSeriesReferenceType : public TimeSeriesType {
  public:
    VISITOR_SUPPORT()

    const TimeSeriesType &target_type() const noexcept { return *target_type_; }
    [[nodiscard]] const TimeSeriesType &with_guarantee(TimeSeriesGuarantee guarantee) const override;

  protected:
    TimeSeriesReferenceType(const TimeSeriesType &target_type, GuaranteeParameter guarantee)
        : TimeSeriesType(guarantee), target_type_(&target_type) {}

  private:
    const TimeSeriesType *target_type_;
};

const TimeSeriesReferenceType &time_series_reference_type(const TimeSeriesType &target_type,
                                                                        TimeSeriesGuarantee guarantee);

class GenericTimeSeriesReferenceType final : public TimeSeriesReferenceType {
  public:
    GenericTimeSeriesReferenceType(const TimeSeriesType &target_type, GuaranteeParameter guarantee)
        : TimeSeriesReferenceType(target_type, guarantee) {}

    bool is_resolved() const noexcept override { return false; }
    [[nodiscard]] const Type &resolve(const TypeMap &bindings) const override;
};

} // namespace bg
