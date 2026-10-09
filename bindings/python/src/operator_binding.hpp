#pragma once

#include <bg/graph/operator.hpp>
#include <bg/graph/wiring_argument.hpp>

#include <nanobind/nanobind.h>

#include <span>
#include <string>

namespace bg::python {
class PythonWiringPoint;
}

namespace bg::python::details {

class PythonOperator {
  public:
    explicit PythonOperator(const Operator &operation) noexcept;

    nanobind::object call(nanobind::args positional, nanobind::kwargs named) const;
    nanobind::object call(nanobind::handle lhs, nanobind::handle rhs) const;
    nanobind::object invoke(std::span<const WiringArgument> arguments) const;
    std::string name() const;

  private:
    const Operator *operation_;
};

void bind_operators(nanobind::module_ &module);
nanobind::object getitem(const PythonWiringPoint &collection, nanobind::handle key);

} // namespace bg::python::details
