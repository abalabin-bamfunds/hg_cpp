#pragma once

#include <bg/type/fixed_item_time_series_type.hpp>
#include <bg/type/guarantee_parameter.hpp>
#include <bg/type/size_parameter.hpp>

#include <cstddef>
#include <optional>
#include <span>

namespace bg {

class TimeSeriesListType : public FixedItemTimeSeriesType {
  public:
    VISITOR_SUPPORT()

    static const TimeSeriesListType &from_items(
        std::span<const TimeSeriesType *const> items,
        TimeSeriesGuarantee guarantee = TimeSeriesGuarantee::ordinary);

    const TimeSeriesType &item_type() const noexcept { return *item_type_; }
    bool contains_reference() const override { return item_type().contains_reference(); }
    const SizeParameter &size() const noexcept { return size_; }
    std::size_t item_count() const noexcept final { return size_.value().value_or(std::dynamic_extent); }
    const TimeSeriesType &item_type(std::size_t index) const final;
    [[nodiscard]] const TimeSeriesType &with_guarantee(TimeSeriesGuarantee guarantee) const override;
    const TimeSeriesType &with_guarantee_limit(TimeSeriesGuarantee limit) const override;
    [[nodiscard]] bool do_match(const Type &supplied, TypeMap &type_map) const override;
    [[nodiscard]] bool do_match(const Type &supplied, TypeMap &type_map,
                                std::optional<TimeSeriesGuarantee> reference_limit) const override;

  protected:
    TimeSeriesListType(const TimeSeriesType &item_type, SizeParameter size, GuaranteeParameter guarantee)
        : FixedItemTimeSeriesType(guarantee), item_type_(&item_type), size_(size) {}

  private:
    const TimeSeriesType *item_type_;
    SizeParameter size_;
};

class GenericTimeSeriesListType final : public TimeSeriesListType {
  public:
    GenericTimeSeriesListType(const TimeSeriesType &item_type, SizeParameter size, GuaranteeParameter guarantee)
        : TimeSeriesListType(item_type, size, guarantee) {}

    bool is_resolved() const noexcept override { return false; }
    [[nodiscard]] const Type &resolve(const TypeMap &type_map) const override;
};

const TimeSeriesListType &time_series_list_type(const TimeSeriesType &item_type, std::size_t size,
                                                TimeSeriesGuarantee guarantee);

} // namespace bg
