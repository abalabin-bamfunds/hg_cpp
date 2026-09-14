#pragma once

#include <bg/evaluation_time.hpp>
#include <bg/time_series/ref/ref_storage.hpp>

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace bg {

class EvaluationContext;
class OutputChangeContext;
class RuntimeGraph;
class TimeSeriesReferenceRuntimeType;
class TimeSeriesRuntimeType;
enum class OutputChange : std::uint8_t;

namespace details {

class DynamicReferenceInputStorage {
  public:
    const EvaluationTime &last_updated() const noexcept { return value_ ? change_time_ : MIN_DT; }
    const Reference &value() const noexcept { return value_; }
    bool modified(EvaluationTime evaluation_time) const noexcept { return change_time_ == evaluation_time; }

    void bind(Reference reference, const TimeSeriesReferenceRuntimeType &runtime_type, EvaluationContext &context,
              std::size_t storage_offset);
    void unbind(RuntimeGraph &graph, std::size_t storage_offset) noexcept;

  private:
    static void notify(OutputChangeContext &context, std::size_t node_index, std::size_t storage_offset,
                       OutputChange change);

    bool refresh(EvaluationTime evaluation_time) noexcept;

    const TimeSeriesReferenceRuntimeType *binding_runtime_type_{};
    Reference selected_;
    Reference value_;
    EvaluationTime change_time_{MIN_DT};
};

static_assert(sizeof(DynamicReferenceInputStorage) == 4 * sizeof(void *));
static_assert(std::is_trivially_copyable_v<DynamicReferenceInputStorage>);

} // namespace details
} // namespace bg
