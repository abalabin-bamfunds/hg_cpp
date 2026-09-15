#pragma once

#include <bg/graph/output_notification.hpp>
#include <bg/time_series/ref/ref_storage.hpp>
#include <bg/time_series/time_series_runtime_type.hpp>
#include <bg/type/time_series_reference_type.hpp>

#include <memory>

namespace bg {

class RuntimeGraph;

class TimeSeriesReferenceRuntimeType : public TimeSeriesRuntimeType {
  public:
    explicit TimeSeriesReferenceRuntimeType(const TimeSeriesReferenceType &type) noexcept
        : TimeSeriesReferenceRuntimeType{type, sizeof(Reference), alignof(Reference)} {}

    void construct_initial_value(void *destination) const noexcept override {
        std::construct_at(static_cast<Reference *>(destination));
    }

    void invalidate_output(const details::OutputState &output) const override;

    bool satisfies_reference_request(const TimeSeriesType &requested_type) const noexcept override;
    Reference reference(const void *value) const override {
        return *static_cast<const Reference *>(value);
    }

    // A derived runtime type owns the opaque address and defines how a dynamic input follows it.
    virtual void *binding_address(const void *) const noexcept { return nullptr; }
    virtual Reference resolve_binding(const void *) const noexcept { return {}; }
    virtual void add_binding_subscriber(void *, RuntimeGraph &, std::uint32_t, std::size_t, std::uint32_t,
                                        InputNotificationFunction) const;
    virtual void remove_binding_subscriber(void *, RuntimeGraph &, std::size_t) const noexcept;

    void destroy_value(void *, allocator &) const noexcept override {}

  protected:
    TimeSeriesReferenceRuntimeType(const TimeSeriesReferenceType &type, std::size_t size,
                                   std::size_t alignment) noexcept
        : TimeSeriesRuntimeType{type, size, alignment, false, false, true} {}
};

const TimeSeriesRuntimeType &time_series_reference_runtime_type(const TimeSeriesReferenceType &type);

} // namespace bg
