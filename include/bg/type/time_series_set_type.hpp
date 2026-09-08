#pragma once

#include <bg/type/guarantee_parameter.hpp>
#include <bg/type/scalar_type.hpp>
#include <bg/type/time_series_type.hpp>

namespace bg {

class TimeSeriesSetType : public TimeSeriesType {
  public:
    VISITOR_SUPPORT()

    const ScalarType &item_type() const noexcept { return *item_type_; }
    [[nodiscard]] const TimeSeriesType &with_guarantee(TimeSeriesGuarantee guarantee) const override;
    bool supports_invalidation() const noexcept final { return false; }

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

    bool is_resolved() const noexcept override { return false; }
    [[nodiscard]] const Type &resolve(const TypeMap &type_map) const override;

  protected:
    [[nodiscard]] bool do_match(const Type &supplied, TypeMap &type_map) const override;
};

const TimeSeriesSetType &time_series_set_type(const ScalarType &item_type, TimeSeriesGuarantee guarantee);

} // namespace bg
