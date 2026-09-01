#pragma once

#include <bg/type/fixed_item_time_series_type.hpp>
#include <bg/type/guarantee_parameter.hpp>
#include <bg/type/scalar_type.hpp>

namespace bg {

class TimeSeriesDictionaryType : public FixedItemTimeSeriesType {
  public:
    [[nodiscard]] const ScalarType &key_type() const noexcept { return *key_type_; }
    [[nodiscard]] const TimeSeriesType &value_type() const noexcept { return *value_type_; }
    [[nodiscard]] const TimeSeriesType &key_set_type() const noexcept { return *key_set_type_; }
    [[nodiscard]] const TimeSeriesType &with_guarantee(TimeSeriesGuarantee guarantee) const override;

    [[nodiscard]] std::size_t item_count() const noexcept final { return 1; }
    [[nodiscard]] const TimeSeriesType &item_type(std::size_t index) const final;
    [[nodiscard]] bool supports_invalidation() const noexcept final { return false; }

  protected:
    TimeSeriesDictionaryType(const ScalarType &key_type, const TimeSeriesType &value_type,
                             const TimeSeriesType &key_set_type, GuaranteeParameter guarantee)
        : FixedItemTimeSeriesType(guarantee), key_type_(&key_type), value_type_(&value_type),
          key_set_type_(&key_set_type) {}

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

    [[nodiscard]] bool is_resolved() const noexcept override { return false; }
    [[nodiscard]] const Type &resolve(const TypeMap &bindings) const override;

  protected:
    [[nodiscard]] bool do_match(const Type &supplied, TypeMap &bindings) const override;
};

[[nodiscard]] const TimeSeriesDictionaryType &time_series_dictionary_type(const ScalarType &key_type,
                                                                          const TimeSeriesType &value_type,
                                                                          TimeSeriesGuarantee guarantee);

} // namespace bg
