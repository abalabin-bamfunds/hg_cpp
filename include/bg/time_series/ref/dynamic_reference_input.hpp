#pragma once

#include <bg/evaluation_time.hpp>
#include <bg/time_series/ref/ref_storage.hpp>

#include <cstddef>
#include <cstdint>

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
    explicit DynamicReferenceInputStorage(const TimeSeriesRuntimeType &runtime_type) noexcept
        : runtime_type_{&runtime_type} {}

    const TimeSeriesRuntimeType &runtime_type() const noexcept { return *runtime_type_; }
    const EvaluationTime &last_updated() const noexcept { return last_updated_; }
    const Reference &value() const noexcept { return value_; }
    bool modified(EvaluationTime evaluation_time) const noexcept { return change_time_ == evaluation_time; }

    void bind(Reference reference, const TimeSeriesReferenceRuntimeType &runtime_type, EvaluationContext &context,
              std::size_t storage_offset);
    void unbind(RuntimeGraph &graph, std::size_t storage_offset) noexcept;

  private:
    static void notify(OutputChangeContext &context, std::size_t node_index, std::size_t storage_offset,
                       OutputChange change);

    bool refresh(EvaluationTime evaluation_time) noexcept;

    const TimeSeriesRuntimeType *runtime_type_;
    const TimeSeriesReferenceRuntimeType *binding_runtime_type_{};
    Reference selected_;
    Reference value_;
    EvaluationTime last_updated_{MIN_DT};
    EvaluationTime change_time_{MIN_DT};
};

} // namespace details
} // namespace bg
