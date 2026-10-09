#pragma once

#include <bg/type/guarantee_parameter.hpp>
#include <bg/type/time_series_type.hpp>

namespace bg {

class TimeSeriesReferenceType : public TimeSeriesType {
  public:
    VISITOR_SUPPORT()

    bool contains_reference() const final { return true; }
    const TimeSeriesType &target_type() const noexcept override { return *target_type_; }
    const TimeSeriesType &dereferenced_type() const override;
    TimeSeriesGuarantee reference_limit() const;
    [[nodiscard]] const TimeSeriesType &with_guarantee(TimeSeriesGuarantee guarantee) const override;

  protected:
    TimeSeriesReferenceType(const TimeSeriesType &target_type, GuaranteeParameter guarantee)
        : TimeSeriesType(guarantee), target_type_(&target_type) {}

  private:
    const TimeSeriesType *target_type_;
};

class GenericTimeSeriesReferenceType;

const TimeSeriesReferenceType &time_series_reference_type(const TimeSeriesType &target_type,
                                                          TimeSeriesGuarantee guarantee);
// The returned type has process lifetime. The target type must also have process lifetime.
const GenericTimeSeriesReferenceType &generic_time_series_reference_type(
    const TimeSeriesType &target_type, TimeSeriesGuarantee guarantee);

class GenericTimeSeriesReferenceType final : public TimeSeriesReferenceType {
  public:
    GenericTimeSeriesReferenceType(const TimeSeriesType &target_type, GuaranteeParameter guarantee)
        : TimeSeriesReferenceType(target_type, guarantee) {}

    bool is_resolved() const noexcept override { return false; }
    [[nodiscard]] const Type &resolve(const TypeMap &type_map) const override;
};

} // namespace bg
