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

    void format_name(std::string &result) const override;
    static const TimeSeriesBundleType &from_fields(
        std::span<const SchemaField> fields,
        TimeSeriesGuarantee guarantee = TimeSeriesGuarantee::ordinary);

    const SchemaType &schema_type() const noexcept { return *schema_; }
    const Schema &schema() const;
    bool contains_reference() const override;
    const TimeSeriesType &reference_free_type() const override;
    std::size_t item_count() const noexcept final;
    const TimeSeriesType &item_type(std::size_t index) const final;
    util::symbol item_name(std::size_t index) const;
    std::optional<std::size_t> find_item_index(util::symbol name) const;
    [[nodiscard]] const TimeSeriesType &with_guarantee(TimeSeriesGuarantee guarantee) const override;
    const TimeSeriesType &with_guarantee_limit(TimeSeriesGuarantee limit) const override;
    const TimeSeriesType *common_type(const TimeSeriesType &other) const override;
    [[nodiscard]] bool do_match(const Type &supplied, TypeMap &type_map) const override;
    [[nodiscard]] bool do_match(const Type &supplied, TypeMap &type_map,
                                std::optional<TimeSeriesGuarantee> reference_limit) const override;

  protected:
    TimeSeriesBundleType(const SchemaType &schema, GuaranteeParameter guarantee);

  private:
    static const Schema *common_schema(const Schema &left, const Schema &right);
    static const Schema &limit_schema_guarantees(const Schema &schema, TimeSeriesGuarantee limit);

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
