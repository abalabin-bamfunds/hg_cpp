#include "python_queue.hpp"

#include "python_literal_builder.hpp"

#include <bg/util/scope_exit.hpp>

#include <stdexcept>

namespace nb = nanobind;
using namespace nanobind::literals;

namespace bg::python {

PythonQueue::PythonQueue(const PythonQueue &other)
    : output_type_{other.output_type_}, mode_{other.mode_} {
    if (other.writing_) {
        throw std::runtime_error{"cannot copy a Python queue while it is writing"};
    }
    queue_ = other.queue_;
}

bool PythonQueue::write(nb::handle value) {
    if (!queue_.is_open()) {
        return false;
    }
    if (writing_) {
        throw std::runtime_error{"a Python queue cannot write concurrently through the same handle"};
    }

    writing_ = true;
    auto reset_writing = util::scope_exit{[&]() noexcept { writing_ = false; }};
    auto literals = details::PythonLiteralBuilder{};
    const auto delta = literals.delta(*output_type_, value, mode_);
    auto release_gil = nb::gil_scoped_release{};
    return queue_.write(delta);
}

void bind_python_queue(nb::module_ &module) {
    nb::enum_<QueueMode>(module, "QueueMode")
        .value("standard", QueueMode::standard)
        .value("elide", QueueMode::elide)
        .value("batch", QueueMode::batch);

    nb::class_<PythonQueue>(module, "Queue")
        .def("write", &PythonQueue::write, "delta"_a.none())
        .def("__copy__", [](const PythonQueue &queue) { return PythonQueue{queue}; });
}

} // namespace bg::python
