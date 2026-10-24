#pragma once

#include <bg/graph_run_input.hpp>
#include <bg/type/signature.hpp>

#include "python_literal_builder.hpp"

#include <nanobind/nanobind.h>

#include <optional>
#include <span>
#include <vector>

namespace bg::python::details {

class GraphRunInputs {
  public:
    GraphRunInputs(const Signature &signature, std::span<const Type *const> wiring_parameter_types,
                   nanobind::sequence fixed_values, nanobind::sequence positional_values,
                   nanobind::dict named_values);

    std::size_t input_length() const noexcept { return input_length_; }
    nanobind::list fixed_values() const;
    nanobind::list positional_values() const;
    nanobind::dict named_values() const;

  private:
    struct Argument {
        nanobind::object value;
        std::optional<std::size_t> input_index;
    };

    const TimeSeriesType &input_type(const Type &parameter_type, const Type &wiring_type) const;
    Argument prepare(nanobind::handle value, const Type &parameter_type, const Type &wiring_type);
    nanobind::object argument_value(const Argument &argument) const;

    PythonLiteralBuilder literals_;
    nanobind::object ticks_type_;
    std::vector<GraphRunLiteralInput> inputs_;
    std::vector<Argument> fixed_values_;
    std::vector<Argument> positional_values_;
    std::vector<std::pair<nanobind::object, Argument>> named_values_;
    std::size_t input_length_{};
};

} // namespace bg::python::details
