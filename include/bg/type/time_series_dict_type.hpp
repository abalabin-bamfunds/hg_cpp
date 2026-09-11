#pragma once

#include <bg/type/fixed_item_time_series_type.hpp>
#include <bg/type/guarantee_parameter.hpp>
#include <bg/type/scalar_type.hpp>

namespace bg {

class TimeSeriesDictionaryType : public FixedItemTimeSeriesType {
  public:
    VISITOR_SUPPORT()

    const ScalarType &key_type() const noexcept { return *key_type_; }
    const TimeSeriesType &value_type() const noexcept { return *value_type_; }
    const TimeSeriesType &key_set_type() const noexcept { return *key_set_type_; }
    [[nodiscard]] const TimeSeriesType &with_guarantee(TimeSeriesGuarantee guarantee) const override;

    std::size_t item_count() const noexcept final { return 1; }
    const TimeSeriesType &item_type(std::size_t index) const final;
    TimeSeriesItemId item_id(std::size_t) const noexcept final { return key_set_item_id; }
    bool supports_invalidation() const noexcept final { return false; }
    bool reference_changes_require_cycle_cleanup() const noexcept final { return true; }
    bool requires_reference_representation_for(const TimeSeriesType &supplied) const noexcept final;

  protected:
    TimeSeriesDictionaryType(const ScalarType &key_type, const TimeSeriesType &value_type,
                             const TimeSeriesType &key_set_type, GuaranteeParameter guarantee)
        : FixedItemTimeSeriesType(guarantee), key_type_(&key_type), value_type_(&value_type),
          key_set_type_(&key_set_type) {}

    [[nodiscard]] bool do_match(const Type &supplied, TypeMap &type_map) const override;

  private:
    const ScalarType *key_type_;
    const TimeSeriesType *value_type_;
    const TimeSeriesType *key_set_type_;
};

class GenericTimeSeriesDictionaryType final : public TimeSeriesDictionaryType {
  public:
    GenericTimeSeriesDictionaryType(const ScalarType &key_type, const TimeSeriesType &value_type,
                                    const TimeSeriesType &key_set_type, GuaranteeParameter guarantee)
        : TimeSeriesDictionaryType(key_type, value_type, key_set_type, guarantee) {}

    bool is_resolved() const noexcept override { return false; }
    [[nodiscard]] const Type &resolve(const TypeMap &type_map) const override;
};

const TimeSeriesDictionaryType &time_series_dictionary_type(const ScalarType &key_type,
                                                            const TimeSeriesType &value_type,
                                                            TimeSeriesGuarantee guarantee);

} // namespace bg
