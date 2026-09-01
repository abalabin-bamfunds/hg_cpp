#pragma once

#include <bg/type/guarantee_parameter.hpp>
#include <bg/type/scalar_type.hpp>
#include <bg/type/time_series_type.hpp>

namespace bg {

class TimeSeriesValueType : public TimeSeriesType {
  public:
    [[nodiscard]] const ScalarType &value_type() const noexcept { return *value_type_; }
    [[nodiscard]] const TimeSeriesType &with_guarantee(TimeSeriesGuarantee guarantee) const override;

  protected:
    TimeSeriesValueType(const ScalarType &value_type, GuaranteeParameter guarantee)
        : TimeSeriesType(guarantee), value_type_(&value_type) {}

  private:
    const ScalarType *value_type_;
};

[[nodiscard]] const TimeSeriesValueType &time_series_value_type(const ScalarType &value_type,
                                                                TimeSeriesGuarantee guarantee);

class GenericTimeSeriesValueType final : public TimeSeriesValueType {
  public:
    GenericTimeSeriesValueType(const ScalarType &value_type, GuaranteeParameter guarantee)
        : TimeSeriesValueType(value_type, guarantee) {}

    [[nodiscard]] bool is_resolved() const noexcept override { return false; }
    [[nodiscard]] const Type &resolve(const TypeMap &bindings) const override;

  protected:
    [[nodiscard]] bool do_match(const Type &supplied, TypeMap &bindings) const override;
};

} // namespace bg
