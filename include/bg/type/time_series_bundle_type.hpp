#pragma once

#include <bg/type/fixed_item_time_series_type.hpp>
#include <bg/type/guarantee_parameter.hpp>
#include <bg/util/symbol.hpp>

#include <cstddef>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace bg {

struct BundleField {
    util::symbol name;
    const TimeSeriesType *type;
};

class BundleSchemaType : public Type {};

class BundleSchema : public BundleSchemaType {
  public:
    BundleSchema(util::symbol name, const void *identity, const BundleSchema *base, std::vector<BundleField> fields);

    util::symbol name() const noexcept { return name_; }
    bool named() const noexcept { return identity_ != nullptr; }
    const void *identity() const noexcept { return identity_; }
    const BundleSchema *base() const noexcept { return base_; }
    std::span<const BundleField> direct_fields() const noexcept { return fields_; }
    std::size_t field_count() const noexcept;
    const BundleField &field(std::size_t index) const;
    const BundleField *find(util::symbol name) const noexcept;
    bool derives_from(const BundleSchema &schema) const noexcept;
    bool is_resolved() const noexcept override;
    [[nodiscard]] const Type &resolve(const TypeMap &type_map) const override;

  private:
    util::symbol name_;
    const void *identity_;
    const BundleSchema *base_;
    std::vector<BundleField> fields_;
    std::vector<BundleField> all_fields_;
};

class TimeSeriesBundleType : public FixedItemTimeSeriesType {
  public:
    VISITOR_SUPPORT()

    static const TimeSeriesBundleType &from_fields(
        std::span<const BundleField> fields,
        TimeSeriesGuarantee guarantee = TimeSeriesGuarantee::ordinary);

    const BundleSchemaType &schema_type() const noexcept { return *schema_; }
    const BundleSchema &schema() const;
    bool contains_reference() const override;
    std::size_t item_count() const noexcept final;
    const TimeSeriesType &item_type(std::size_t index) const final;
    util::symbol item_name(std::size_t index) const;
    std::optional<std::size_t> find_item_index(util::symbol name) const;
    [[nodiscard]] const TimeSeriesType &with_guarantee(TimeSeriesGuarantee guarantee) const override;

  protected:
    TimeSeriesBundleType(const BundleSchemaType &schema, GuaranteeParameter guarantee)
        : FixedItemTimeSeriesType(guarantee), schema_{&schema} {}

    [[nodiscard]] bool do_match(const Type &supplied, TypeMap &type_map) const override;

  private:
    const BundleSchemaType *schema_;
};

class GenericTimeSeriesBundleType final : public TimeSeriesBundleType {
  public:
    GenericTimeSeriesBundleType(const BundleSchemaType &schema, GuaranteeParameter guarantee)
        : TimeSeriesBundleType(schema, guarantee) {}

    bool is_resolved() const noexcept override { return false; }
    [[nodiscard]] const Type &resolve(const TypeMap &type_map) const override;
};

[[nodiscard]] const BundleSchema &bundle_schema(util::symbol name, const void *identity, const BundleSchema *base,
                                                std::span<const BundleField> fields);
[[nodiscard]] const BundleSchema &unnamed_bundle_schema(std::span<const BundleField> fields);
const TimeSeriesBundleType &time_series_bundle_type(const BundleSchema &schema, TimeSeriesGuarantee guarantee);

} // namespace bg
