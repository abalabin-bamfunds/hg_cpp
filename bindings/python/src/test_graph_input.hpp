#pragma once

#include <bg/testing/test_graph_literal_input.hpp>
#include <bg/type/signature.hpp>

#include "python_literal_builder.hpp"

#include <nanobind/nanobind.h>

#include <span>
#include <vector>

namespace bg::python::details {

class TestGraphInputs {
  public:
    TestGraphInputs(const Signature &signature, std::span<const Type *const> wiring_parameter_types,
                    nanobind::sequence inputs);

    std::span<const testing::TestGraphLiteralInput> inputs() const noexcept { return inputs_; }

  private:
    const TimeSeriesType &input_type(const Type &parameter_type, const Type &wiring_type) const;

    PythonLiteralBuilder literals_;
    std::vector<testing::TestGraphLiteralInput> inputs_;
};

} // namespace bg::python::details
