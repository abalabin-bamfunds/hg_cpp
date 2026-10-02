#pragma once

#include <bg/evaluation_time.hpp>
#include <bg/graph/output_notification.hpp>
#include <bg/time_series/ref/ref_storage.hpp>

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace bg {

class EvaluationContext;
class RuntimeGraph;

namespace details {

class ReferenceBindingType {
  public:
    virtual const Reference &value(const void *data) const noexcept = 0;
    virtual void subscribe(void *data, RuntimeGraph &graph, std::uint32_t input_offset,
                           std::size_t notification_data, std::uint32_t node_index,
                           InputNotificationFunction notification) const = 0;
    virtual void unsubscribe(void *data, RuntimeGraph &graph, std::size_t input_offset) const noexcept = 0;

  protected:
    ReferenceBindingType() = default;
    ~ReferenceBindingType() = default;
};

// Identifies selected-reference state and the type that interprets it.
class ReferenceBinding {
  public:
    ReferenceBinding() = default;
    ReferenceBinding(void *data, const ReferenceBindingType &type) noexcept : data_{data}, type_{&type} {}

    explicit operator bool() const noexcept { return data_ != nullptr; }
    const Reference &value() const noexcept { return type_->value(data_); }
    void subscribe(RuntimeGraph &graph, std::uint32_t input_offset, std::size_t notification_data,
                   std::uint32_t node_index, InputNotificationFunction notification) const {
        type_->subscribe(data_, graph, input_offset, notification_data, node_index, notification);
    }
    void unsubscribe(RuntimeGraph &graph, std::size_t input_offset) const noexcept {
        type_->unsubscribe(data_, graph, input_offset);
    }

    friend bool operator==(ReferenceBinding, ReferenceBinding) = default;

  private:
    void *data_{};
    const ReferenceBindingType *type_{};
};

class DynamicReferenceInputStorage {
  public:
    const TimeSeriesState &last_updated() const noexcept { return last_updated_; }
    const Reference &value() const noexcept { return value_; }
    bool modified(EvaluationTime evaluation_time) const noexcept {
        return last_updated_.valid() && last_updated_.time() == evaluation_time;
    }

    void bind(ReferenceBinding binding, EvaluationContext &context, std::size_t storage_offset);
    void unbind(RuntimeGraph &graph, std::size_t storage_offset) noexcept;

  private:
    static void notify(OutputChangeContext &context, std::size_t node_index, std::size_t storage_offset,
                       OutputChange change);

    bool refresh(EvaluationTime evaluation_time) noexcept;

    ReferenceBinding binding_{};
    Reference value_;
    TimeSeriesState last_updated_{};
};

static_assert(sizeof(ReferenceBindingType) == sizeof(void *));
static_assert(sizeof(ReferenceBinding) == 2 * sizeof(void *));
static_assert(sizeof(DynamicReferenceInputStorage) == 4 * sizeof(void *));
static_assert(std::is_trivially_copyable_v<DynamicReferenceInputStorage>);

} // namespace details
} // namespace bg
