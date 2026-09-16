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

// A ReferenceBinding identifies selected-reference state and the operations
// required to subscribe to its changes. Implementations share the operations,
// so the handle itself is two words.
struct ReferenceBindingOperations {
    const Reference &(*value)(const void *) noexcept;
    void (*subscribe)(void *, RuntimeGraph &, std::uint32_t input_offset, std::size_t notification_data,
                      std::uint32_t node_index, InputNotificationFunction notification);
    void (*unsubscribe)(void *, RuntimeGraph &, std::size_t input_offset) noexcept;
};

class ReferenceBinding {
  public:
    ReferenceBinding() = default;
    ReferenceBinding(void *value, const ReferenceBindingOperations &operations) noexcept
        : value_{value}, operations_{&operations} {}

    explicit operator bool() const noexcept { return value_ != nullptr; }
    const Reference &value() const noexcept { return operations_->value(value_); }
    void subscribe(RuntimeGraph &graph, std::uint32_t input_offset, std::size_t notification_data,
                   std::uint32_t node_index, InputNotificationFunction notification) const {
        operations_->subscribe(value_, graph, input_offset, notification_data, node_index, notification);
    }
    void unsubscribe(RuntimeGraph &graph, std::size_t input_offset) const noexcept {
        operations_->unsubscribe(value_, graph, input_offset);
    }

    friend bool operator==(ReferenceBinding, ReferenceBinding) = default;

  private:
    void *value_{};
    const ReferenceBindingOperations *operations_{};
};

class DynamicReferenceInputStorage {
  public:
    const EvaluationTime &last_updated() const noexcept { return value_ ? change_time_ : MIN_DT; }
    const Reference &value() const noexcept { return value_; }
    bool modified(EvaluationTime evaluation_time) const noexcept { return change_time_ == evaluation_time; }

    void bind(ReferenceBinding binding, EvaluationContext &context, std::size_t storage_offset);
    void unbind(RuntimeGraph &graph, std::size_t storage_offset) noexcept;

  private:
    static void notify(OutputChangeContext &context, std::size_t node_index, std::size_t storage_offset,
                       OutputChange change);

    bool refresh(EvaluationTime evaluation_time) noexcept;

    ReferenceBinding binding_{};
    Reference value_;
    EvaluationTime change_time_{MIN_DT};
};

static_assert(sizeof(ReferenceBinding) == 2 * sizeof(void *));
static_assert(sizeof(DynamicReferenceInputStorage) == 4 * sizeof(void *));
static_assert(std::is_trivially_copyable_v<DynamicReferenceInputStorage>);

} // namespace details
} // namespace bg
