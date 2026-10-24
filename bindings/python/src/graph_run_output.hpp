#pragma once

#include <bg/graph_run_result.hpp>

#include <nanobind/nanobind.h>

namespace bg::python::details {

class GraphRunOutputs {
  public:
    explicit GraphRunOutputs(const GraphRunResult<> &results) noexcept : results_{results} {}

    nanobind::tuple to_python(std::size_t minimum_size = 0) const;

  private:
    nanobind::object scalar(util::buffer_reader &reader, const ScalarType &type) const;
    nanobind::object delta(const TimeSeriesType &type, bool valid, util::buffer_reader &reader) const;

    const GraphRunResult<> &results_;
};

} // namespace bg::python::details
