#pragma once

#include <bg/time_series/ref/ref_storage.hpp>
#include <bg/time_series/time_series_runtime_type.hpp>
#include <bg/type/time_series_reference_type.hpp>

#include <memory>

namespace bg {

class TimeSeriesReferenceRuntimeType final : public TimeSeriesRuntimeType {
  public:
    TimeSeriesReferenceRuntimeType(const TimeSeriesReferenceType &type, const TimeSeriesRuntimeType &target) noexcept
        : TimeSeriesRuntimeType{
              type, sizeof(details::ReferenceAddress), alignof(details::ReferenceAddress), false, false, true},
          target_{&target} {}
    const TimeSeriesRuntimeType &target_runtime_type() const noexcept { return *target_; }

    void construct_initial_value(void *destination) const noexcept override {
        std::construct_at(static_cast<details::ReferenceAddress *>(destination));
    }

    void invalidate_output(const details::OutputState &output) const override;

    void destroy_value(void *, std::pmr::memory_resource &) const noexcept override {}

  private:
    const TimeSeriesRuntimeType *target_;
};

const TimeSeriesRuntimeType &
time_series_reference_runtime_type(const TimeSeriesReferenceType &type, const TimeSeriesRuntimeType &target);

} // namespace bg
