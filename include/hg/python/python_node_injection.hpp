#pragma once

#include <hg/evaluation_context.hpp>
#include <hg/python/callback_scope.hpp>
#include <hg/python/export.hpp>
#include <hg/schedule_handle.hpp>

#include <nanobind/nanobind.h>

#include <cstdint>
#include <memory>
#include <utility>

namespace hg::python {

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

class HG_PYTHON_API PythonEvaluationClock {
  public:
    explicit PythonEvaluationClock(NodeCallbackAccess access) noexcept : access_{std::move(access)} {}

    EvaluationTime evaluation_time() const;
    EvaluationTime now() const;

  private:
    NodeCallbackAccess access_;
};

class HG_PYTHON_API PythonScheduler {
  public:
    explicit PythonScheduler(NodeCallbackAccess access) noexcept : access_{std::move(access)} {}

    ScheduleHandle schedule_at(EvaluationTime time) const;
    void cancel(ScheduleHandle handle) const;
    ScheduleHandle schedule_wall_clock_at(EvaluationTime time) const;
    bool timer_fired(ScheduleHandle handle) const;

  private:
    NodeCallbackAccess access_;
};

HG_PYTHON_API void bind_node_injectables(nanobind::module_ &module);

} // namespace hg::python
