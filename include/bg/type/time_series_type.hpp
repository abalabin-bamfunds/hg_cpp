#pragma once

#include <bg/type/guarantee_parameter.hpp>
#include <bg/type/type.hpp>

namespace bg {

enum class TimeSeriesTypeRole { declaration, input, output };

class TimeSeriesType : public Type {
  public:
    VISITOR_SUPPORT()

    bool is_scalar() const noexcept final { return false; }
    bool is_time_series() const noexcept final { return true; }

    const GuaranteeParameter &guarantee() const noexcept { return guarantee_; }
    virtual const TimeSeriesType &target_type() const noexcept { return *this; }
    // Unlike target_type(), this returns what a non-reference consumer observes. A resolved reference caps its
    // target guarantees recursively; a non-reference type returns itself.
    virtual const TimeSeriesType &dereferenced_type() const;
    [[nodiscard]] virtual const TimeSeriesType &with_guarantee(TimeSeriesGuarantee guarantee) const;
    // Returns a canonical resolved type capped at limit, including contained time series. A reference caps only
    // its own guarantee and preserves its target. The receiver and its contained time series must be resolved.
    virtual const TimeSeriesType &with_guarantee_limit(TimeSeriesGuarantee limit) const;
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
