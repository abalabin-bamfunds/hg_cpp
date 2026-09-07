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
    [[nodiscard]] virtual const TimeSeriesType &with_guarantee(TimeSeriesGuarantee guarantee) const;
    virtual bool supports_invalidation() const noexcept { return true; }

  protected:
    TimeSeriesType() = default;
    explicit TimeSeriesType(GuaranteeParameter guarantee) : guarantee_(guarantee) {}

  private:
    GuaranteeParameter guarantee_;
};

} // namespace bg
