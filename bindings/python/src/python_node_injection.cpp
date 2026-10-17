#include <bg/python/python_node_injection.hpp>

#include <nanobind/stl/chrono.h>

#include <stdexcept>

namespace nb = nanobind;
using namespace nanobind::literals;

namespace bg::python {

EvaluationContext &NodeCallbackAccess::get() const {
    if (generator_clock != nullptr) {
        if (generator_clock->current == nullptr) {
            throw std::runtime_error{"a Python generator clock was used outside generator advancement"};
        }
        return *generator_clock->current;
    }
    if (!lifetime->active(generation)) {
        throw std::runtime_error{"a Python node injectable was used outside its callback"};
    }
    return context;
}

EvaluationTime PythonEvaluationClock::evaluation_time() const { return access_.get().evaluation_time(); }

EvaluationTime PythonEvaluationClock::now() const { return access_.get().wall_clock().now(); }

ScheduleHandle PythonScheduler::schedule_at(EvaluationTime time) const { return access_.get().schedule_at(time); }

void PythonScheduler::cancel(ScheduleHandle handle) const { access_.get().cancel(handle); }

ScheduleHandle PythonScheduler::schedule_wall_clock_at(EvaluationTime time) const {
    return access_.get().schedule_wall_clock_at(time);
}

bool PythonScheduler::timer_fired(ScheduleHandle handle) const { return access_.get().timer_fired(handle); }

void bind_node_injectables(nb::module_ &module) {
    nb::class_<ScheduleHandle>(module, "ScheduleHandle")
        .def("__bool__", [](const ScheduleHandle &handle) { return static_cast<bool>(handle); });

    nb::class_<PythonEvaluationClock>(module, "EvaluationClock")
        .def_prop_ro("evaluation_time", &PythonEvaluationClock::evaluation_time)
        .def_prop_ro("now", &PythonEvaluationClock::now);

    nb::class_<PythonScheduler>(module, "Scheduler")
        .def("schedule_at", &PythonScheduler::schedule_at, "time"_a)
        .def("cancel", &PythonScheduler::cancel, "handle"_a)
        .def("schedule_wall_clock_at", &PythonScheduler::schedule_wall_clock_at, "time"_a)
        .def("timer_fired", &PythonScheduler::timer_fired, "handle"_a);
}

} // namespace bg::python
