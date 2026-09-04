#pragma once

#include <bg/type/fixed_item_time_series_type.hpp>
#include <bg/type/guarantee_parameter.hpp>
#include <bg/util/symbol.hpp>

#include <cstddef>
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

    [[nodiscard]] util::symbol name() const noexcept { return name_; }
    [[nodiscard]] bool named() const noexcept { return identity_ != nullptr; }
    [[nodiscard]] const void *identity() const noexcept { return identity_; }
    [[nodiscard]] const BundleSchema *base() const noexcept { return base_; }
    [[nodiscard]] std::span<const BundleField> direct_fields() const noexcept { return fields_; }
    [[nodiscard]] std::size_t field_count() const noexcept;
    [[nodiscard]] const BundleField &field(std::size_t index) const;
    [[nodiscard]] const BundleField *find(util::symbol name) const noexcept;
    [[nodiscard]] bool derives_from(const BundleSchema &schema) const noexcept;
    [[nodiscard]] bool is_resolved() const noexcept override;
    [[nodiscard]] const Type &resolve(const TypeMap &bindings) const override;

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

    [[nodiscard]] const BundleSchemaType &schema_type() const noexcept { return *schema_; }
    [[nodiscard]] const BundleSchema &schema() const;
    [[nodiscard]] std::size_t item_count() const noexcept final;
    [[nodiscard]] const TimeSeriesType &item_type(std::size_t index) const final;
    [[nodiscard]] util::symbol item_name(std::size_t index) const;
    [[nodiscard]] const TimeSeriesType &with_guarantee(TimeSeriesGuarantee guarantee) const override;

  protected:
    TimeSeriesBundleType(const BundleSchemaType &schema, GuaranteeParameter guarantee)
        : FixedItemTimeSeriesType(guarantee), schema_{&schema} {}

    [[nodiscard]] bool do_match(const Type &supplied, TypeMap &bindings) const override;

  private:
    const BundleSchemaType *schema_;
};

class GenericTimeSeriesBundleType final : public TimeSeriesBundleType {
  public:
    GenericTimeSeriesBundleType(const BundleSchemaType &schema, GuaranteeParameter guarantee)
        : TimeSeriesBundleType(schema, guarantee) {}

    [[nodiscard]] bool is_resolved() const noexcept override { return false; }
    [[nodiscard]] const Type &resolve(const TypeMap &bindings) const override;
};

[[nodiscard]] const BundleSchema &bundle_schema(util::symbol name, const void *identity, const BundleSchema *base,
                                                std::span<const BundleField> fields);
[[nodiscard]] const BundleSchema &unnamed_bundle_schema(std::span<const BundleField> fields);
[[nodiscard]] const TimeSeriesBundleType &time_series_bundle_type(const BundleSchema &schema,
                                                                  TimeSeriesGuarantee guarantee);

} // namespace bg
