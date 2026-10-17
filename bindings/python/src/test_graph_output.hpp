#pragma once

#include <bg/testing/test_graph.hpp>

#include <nanobind/nanobind.h>

namespace bg::python::details {

class TestGraphOutputs {
  public:
    explicit TestGraphOutputs(const testing::TestGraphResult<> &results) noexcept : results_{results} {}

    nanobind::list to_python() const;

  private:
    nanobind::object scalar(util::buffer_reader &reader, const ScalarType &type) const;
    nanobind::object delta(const TimeSeriesType &type, bool valid, util::buffer_reader &reader) const;

    const testing::TestGraphResult<> &results_;
};

} // namespace bg::python::details
