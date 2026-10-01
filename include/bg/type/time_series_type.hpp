#pragma once

#include <bg/type/guarantee_parameter.hpp>
#include <bg/type/type.hpp>

namespace bg {

class TimeSeriesType : public Type {
  public:
    VISITOR_SUPPORT()

    bool is_scalar() const noexcept final { return false; }
    bool is_time_series() const noexcept final { return true; }

    const GuaranteeParameter &guarantee() const noexcept { return guarantee_; }
    virtual const TimeSeriesType &target_type() const noexcept { return *this; }
    [[nodiscard]] virtual const TimeSeriesType &with_guarantee(TimeSeriesGuarantee guarantee) const;
    const TimeSeriesType *common_type(const TimeSeriesType &other) const;
    virtual bool supports_invalidation() const noexcept { return true; }
    virtual bool contains_reference() const;
    virtual bool reference_changes_require_cycle_cleanup() const noexcept { return false; }
    virtual bool requires_reference_representation_for(const TimeSeriesType &) const noexcept { return false; }

  protected:
    TimeSeriesType() = default;
    explicit TimeSeriesType(GuaranteeParameter guarantee) : guarantee_(guarantee) {}

  private:
    GuaranteeParameter guarantee_;
};

} // namespace bg
