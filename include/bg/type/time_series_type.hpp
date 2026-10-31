#pragma once

#include <bg/type/guarantee_parameter.hpp>
#include <bg/type/type.hpp>

#include <cassert>
#include <cstddef>
#include <string>

namespace bg {

enum class TimeSeriesTypeRole { declaration, input, output };

class TimeSeriesRuntimeType;

class TimeSeriesType : public Type {
  public:
    using RuntimeTypeFactory = const TimeSeriesRuntimeType &(*)(const TimeSeriesType &);

    VISITOR_SUPPORT()

    bool is_scalar() const noexcept final { return false; }
    bool is_time_series() const noexcept final { return true; }

    const GuaranteeParameter &guarantee() const noexcept { return guarantee_; }
    virtual const TimeSeriesType &target_type() const noexcept { return *this; }
    // Unlike target_type(), this returns what a non-reference consumer observes. A resolved reference caps its
    // target guarantees recursively; a non-reference type returns itself.
    virtual const TimeSeriesType &dereferenced_type() const;
    // Return the resolved type observed by a value input, removing contained references recursively.
    virtual const TimeSeriesType &reference_free_type() const;
    [[nodiscard]] virtual const TimeSeriesType &with_guarantee(TimeSeriesGuarantee guarantee) const;
    // Returns a canonical resolved type capped at limit, including contained time series. Extension types with
    // contained time series must override this operation to cap them. A reference caps only its own guarantee and
    // preserves its target. The receiver and its contained time series must be resolved.
    virtual const TimeSeriesType &with_guarantee_limit(TimeSeriesGuarantee limit) const;
    // Returns the canonical resolved type with weakest matching guarantees, or nullptr for incompatible or
    // unresolved types.
    virtual const TimeSeriesType *common_type(const TimeSeriesType &other) const;
    virtual bool supports_invalidation() const noexcept { return true; }
    virtual bool contains_reference() const;
    virtual bool reference_changes_require_cycle_cleanup() const noexcept { return false; }
    // Returns whether reference representation differs at this type or at a contained time series.
    virtual bool requires_reference_representation_for(const TimeSeriesType &supplied) const noexcept;

    RuntimeTypeFactory default_runtime_type_factory() const noexcept { return default_runtime_type_factory_; }
    void set_default_runtime_type_factory(RuntimeTypeFactory factory) const noexcept {
        assert(factory != nullptr);
        if (default_runtime_type_factory_ == nullptr) {
            default_runtime_type_factory_ = factory;
        }
    }

  protected:
    void append_guarantee(std::string &result) const;
    TimeSeriesType() = default;
    explicit TimeSeriesType(GuaranteeParameter guarantee) : guarantee_(guarantee) {}

  private:
    GuaranteeParameter guarantee_;
    mutable RuntimeTypeFactory default_runtime_type_factory_{};
};

} // namespace bg
