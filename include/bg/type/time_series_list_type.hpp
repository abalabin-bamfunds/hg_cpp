#pragma once

#include <bg/type/fixed_item_time_series_type.hpp>
#include <bg/type/guarantee_parameter.hpp>

#include <cstddef>

namespace bg {

class TimeSeriesListType : public FixedItemTimeSeriesType {
  public:
    [[nodiscard]] const TimeSeriesType &item_type() const noexcept { return *item_type_; }
    [[nodiscard]] std::size_t item_count() const noexcept final { return size_; }
    [[nodiscard]] const TimeSeriesType &item_type(std::size_t index) const final;
    [[nodiscard]] const TimeSeriesType &with_guarantee(TimeSeriesGuarantee guarantee) const override;

  protected:
    TimeSeriesListType(const TimeSeriesType &item_type, std::size_t size, GuaranteeParameter guarantee)
        : FixedItemTimeSeriesType(guarantee), item_type_(&item_type), size_(size) {}

    [[nodiscard]] bool do_match(const Type &supplied, TypeMap &bindings) const override;

  private:
    const TimeSeriesType *item_type_;
    std::size_t size_;
};

class GenericTimeSeriesListType final : public TimeSeriesListType {
  public:
    GenericTimeSeriesListType(const TimeSeriesType &item_type, std::size_t size, GuaranteeParameter guarantee)
        : TimeSeriesListType(item_type, size, guarantee) {}

    [[nodiscard]] bool is_resolved() const noexcept override { return false; }
    [[nodiscard]] const Type &resolve(const TypeMap &bindings) const override;

};

[[nodiscard]] const TimeSeriesListType &time_series_list_type(const TimeSeriesType &item_type, std::size_t size,
                                                              TimeSeriesGuarantee guarantee);

} // namespace bg
