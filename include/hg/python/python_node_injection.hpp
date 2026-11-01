#pragma once

#include <bg/evaluation_context.hpp>
#include <bg/python/callback_scope.hpp>
#include <bg/python/export.hpp>
#include <bg/schedule_handle.hpp>

#include <nanobind/nanobind.h>

#include <cstdint>
#include <memory>
#include <utility>

namespace bg::python {

struct GeneratorClockLease {
    EvaluationContext *current{};
};

struct NodeCallbackAccess {
    EvaluationContext &context;
    std::shared_ptr<CallbackLifetime> lifetime;
    std::uint64_t generation;
    std::shared_ptr<GeneratorClockLease> generator_clock;

    EvaluationContext &get() const;
};

class BG_PYTHON_API PythonEvaluationClock {
  public:
    explicit PythonEvaluationClock(NodeCallbackAccess access) noexcept : access_{std::move(access)} {}

    EvaluationTime evaluation_time() const;
    EvaluationTime now() const;

  private:
    NodeCallbackAccess access_;
};

class BG_PYTHON_API PythonScheduler {
  public:
    explicit PythonScheduler(NodeCallbackAccess access) noexcept : access_{std::move(access)} {}

    ScheduleHandle schedule_at(EvaluationTime time) const;
    void cancel(ScheduleHandle handle) const;
    ScheduleHandle schedule_wall_clock_at(EvaluationTime time) const;
    bool timer_fired(ScheduleHandle handle) const;

  private:
    NodeCallbackAccess access_;
};

BG_PYTHON_API void bind_node_injectables(nanobind::module_ &module);

} // namespace bg::python
