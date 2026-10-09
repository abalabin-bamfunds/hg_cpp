#pragma once

#include <bg/time_series/ref/ref_storage.hpp>
#include <bg/time_series/time_series_runtime_type.hpp>
#include <bg/type/time_series_reference_type.hpp>

#include <memory>

namespace bg {

class TimeSeriesReferenceRuntimeType : public TimeSeriesRuntimeType {
  public:
    explicit TimeSeriesReferenceRuntimeType(const TimeSeriesReferenceType &type) noexcept
        : TimeSeriesReferenceRuntimeType{type, sizeof(Reference), alignof(Reference)} {}

    void construct_initial_value(void *destination) const noexcept override {
        std::construct_at(static_cast<Reference *>(destination));
    }

    void invalidate_output(const OutputState &output) const override;
    void copy(const TimeSeriesInputData &input, OutputState &output) const override;

    Reference reference(const void *value) const override { return *static_cast<const Reference *>(value); }

    void destroy_value(void *, allocator &) const noexcept override {}

  protected:
    TimeSeriesReferenceRuntimeType(const TimeSeriesReferenceType &type, std::size_t size,
                                   std::size_t alignment) noexcept
        : TimeSeriesRuntimeType{type, size, alignment, false, false, true, true} {}
};

const TimeSeriesRuntimeType &time_series_reference_runtime_type(const TimeSeriesReferenceType &type);

} // namespace bg
