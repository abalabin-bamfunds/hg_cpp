#pragma once

#include <bg/type/guarantee_parameter.hpp>
#include <bg/type/type.hpp>

namespace bg {

class TimeSeriesType : public Type {
  public:
    [[nodiscard]] bool is_scalar() const noexcept final { return false; }
    [[nodiscard]] bool is_time_series() const noexcept final { return true; }

    [[nodiscard]] const GuaranteeParameter &guarantee() const noexcept { return guarantee_; }

  protected:
    TimeSeriesType() = default;
    explicit TimeSeriesType(GuaranteeParameter guarantee) : guarantee_(guarantee) {}

  private:
    GuaranteeParameter guarantee_;
};

} // namespace bg
