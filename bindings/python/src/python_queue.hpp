#pragma once

#include <bg/queue.hpp>
#include <bg/queue_mode.hpp>
#include <bg/type/time_series_type.hpp>

#include <nanobind/nanobind.h>

#include <utility>

namespace bg::python {

class PythonQueue {
  public:
    PythonQueue(bg::Queue queue, const TimeSeriesType &output_type, QueueMode mode) noexcept
        : queue_{std::move(queue)}, output_type_{&output_type}, mode_{mode} {}

    PythonQueue(const PythonQueue &other);
    PythonQueue(PythonQueue &&other) noexcept = default;
    PythonQueue &operator=(PythonQueue &&other) noexcept = default;

    bool write(nanobind::handle value);

  private:
    bg::Queue queue_;
    const TimeSeriesType *output_type_;
    QueueMode mode_;
    // Python calls serialize under the GIL; this flag spans the GIL-released native write.
    bool writing_{};
};

void bind_python_queue(nanobind::module_ &module);

} // namespace bg::python
