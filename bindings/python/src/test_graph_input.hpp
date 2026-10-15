#pragma once

#include <bg/testing/test_graph_literal_input.hpp>
#include <bg/type/signature.hpp>

#include <nanobind/nanobind.h>

#include <memory>
#include <memory_resource>
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
    literal::TimeSeriesLiteral delta(const TimeSeriesType &type, nanobind::handle value);
    literal::TimeSeriesLiteral scalar(const ScalarType &type, nanobind::handle value);
    literal::ScalarLiteral &removed_value(const ScalarType &type, nanobind::handle value);
    literal::TimeSeriesLiteral sequence(std::vector<literal::TimeSeriesLiteral> values);

    std::pmr::unsynchronized_pool_resource pool_;
    allocator memory_;
    std::vector<std::unique_ptr<ScalarValue<>>> scalars_;
    std::vector<std::unique_ptr<literal::ScalarLiteral>> removed_values_;
    std::vector<std::unique_ptr<std::vector<literal::TimeSeriesLiteral>>> sequences_;
    std::vector<testing::TestGraphLiteralInput> inputs_;
};

} // namespace bg::python::details
