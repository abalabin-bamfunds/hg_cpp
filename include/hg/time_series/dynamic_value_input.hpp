#pragma once

#include <bg/graph/output_notification.hpp>
#include <bg/time_series/details/input_data.hpp>
#include <bg/time_series/time_series_state.hpp>

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace bg {

class EvaluationContext;
class RuntimeGraph;
class TimeSeriesRuntimeType;

namespace details {

class DynamicValueBindingType {
  public:
    virtual TimeSeriesInputData input_data(const void *data, EvaluationTime evaluation_time) const noexcept = 0;
    virtual void subscribe(void *data, RuntimeGraph &graph, std::size_t storage_offset,
                           std::uint32_t node_index) const = 0;
    virtual void unsubscribe(void *data, RuntimeGraph &graph, std::size_t storage_offset) const noexcept = 0;

  protected:
    DynamicValueBindingType() = default;
    ~DynamicValueBindingType() = default;
};

template <typename T> struct DynamicValueBinding {
    void *data{};
    const DynamicValueBindingType *type{};

    friend bool operator==(DynamicValueBinding, DynamicValueBinding) = default;
};

class DynamicValueInputStorage {
  public:
    template <typename T>
    void bind(DynamicValueBinding<T> binding, EvaluationContext &context, std::size_t storage_offset) {
        bind_raw(binding.data, binding.type, context, storage_offset);
    }
    void unbind(RuntimeGraph &graph, std::size_t storage_offset) noexcept;
    TimeSeriesInputData input_data(const TimeSeriesRuntimeType &runtime_type,
                                   EvaluationTime evaluation_time) const noexcept;

  private:
    void bind_raw(void *data, const DynamicValueBindingType *type, EvaluationContext &context,
                  std::size_t storage_offset);

    void *data_{};
    const DynamicValueBindingType *type_{};
};

static_assert(sizeof(DynamicValueBindingType) == sizeof(void *));
static_assert(sizeof(DynamicValueBinding<bool>) == 2 * sizeof(void *));
static_assert(sizeof(DynamicValueInputStorage) == 2 * sizeof(void *));
static_assert(std::is_trivially_copyable_v<DynamicValueInputStorage>);

} // namespace details
} // namespace bg
