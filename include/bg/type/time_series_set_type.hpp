#pragma once

#include <bg/type/guarantee_parameter.hpp>
#include <bg/type/scalar_type.hpp>
#include <bg/type/time_series_type.hpp>

namespace bg {

class TimeSeriesSetType : public TimeSeriesType {
  public:
    [[nodiscard]] const ScalarType &item_type() const noexcept { return *item_type_; }

  protected:
    TimeSeriesSetType(const ScalarType &item_type, GuaranteeParameter guarantee)
        : TimeSeriesType(guarantee), item_type_(&item_type) {}

  private:
    const ScalarType *item_type_;
};

class GenericTimeSeriesSetType final : public TimeSeriesSetType {
  public:
    GenericTimeSeriesSetType(const ScalarType &item_type, GuaranteeParameter guarantee)
        : TimeSeriesSetType(item_type, guarantee) {}

    [[nodiscard]] bool is_resolved() const noexcept override { return false; }
    [[nodiscard]] bool match(const Type &supplied, TypeMap &bindings) const override;
    [[nodiscard]] const Type &resolve(const TypeMap &bindings) const override;
};

[[nodiscard]] const TimeSeriesSetType &time_series_set_type(const ScalarType &item_type, TimeSeriesGuarantee guarantee);

} // namespace bg
