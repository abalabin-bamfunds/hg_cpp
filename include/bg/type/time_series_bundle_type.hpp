#pragma once

#include <bg/type/fixed_item_time_series_type.hpp>
#include <bg/type/guarantee_parameter.hpp>
#include <bg/type/schema.hpp>
#include <bg/util/symbol.hpp>

#include <cstddef>
#include <optional>
#include <span>

namespace bg {

class TimeSeriesBundleType : public FixedItemTimeSeriesType {
  public:
    VISITOR_SUPPORT()

    static const TimeSeriesBundleType &from_fields(
        std::span<const SchemaField> fields,
        TimeSeriesGuarantee guarantee = TimeSeriesGuarantee::ordinary);

    const SchemaType &schema_type() const noexcept { return *schema_; }
    const Schema &schema() const;
    bool contains_reference() const override;
    std::size_t item_count() const noexcept final;
    const TimeSeriesType &item_type(std::size_t index) const final;
    util::symbol item_name(std::size_t index) const;
    std::optional<std::size_t> find_item_index(util::symbol name) const;
    [[nodiscard]] const TimeSeriesType &with_guarantee(TimeSeriesGuarantee guarantee) const override;

  protected:
    TimeSeriesBundleType(const SchemaType &schema, GuaranteeParameter guarantee);

    [[nodiscard]] bool do_match(const Type &supplied, TypeMap &type_map) const override;

  private:
    const SchemaType *schema_;
};

class GenericTimeSeriesBundleType final : public TimeSeriesBundleType {
  public:
    GenericTimeSeriesBundleType(const SchemaType &schema, GuaranteeParameter guarantee)
        : TimeSeriesBundleType(schema, guarantee) {}

    bool is_resolved() const noexcept override { return false; }
    [[nodiscard]] const Type &resolve(const TypeMap &type_map) const override;
};

const TimeSeriesBundleType &time_series_bundle_type(const Schema &schema, TimeSeriesGuarantee guarantee);

} // namespace bg
