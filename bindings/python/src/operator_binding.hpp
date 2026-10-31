#pragma once

#include <bg/operator.hpp>
#include <bg/graph/wiring_argument.hpp>

#include <nanobind/nanobind.h>

#include <span>
#include <string>
#include <vector>

namespace bg::python {
class PythonWiringPoint;
}

namespace bg::python::details {

class PythonOperator {
  public:
    explicit PythonOperator(const Operator &operation);
    PythonOperator(const Operator &operation, std::span<const Type *const> type_parameters);
    PythonOperator(const PythonOperator &other);
    PythonOperator &operator=(const PythonOperator &other);

    nanobind::object call(nanobind::args positional, nanobind::kwargs named) const;
    nanobind::object call(nanobind::handle value) const;
    nanobind::object call(nanobind::handle lhs, nanobind::handle rhs) const;
    nanobind::object invoke(std::span<const WiringArgument> arguments) const;
    PythonOperator map_type_arguments(nanobind::handle arguments) const;
    std::string name() const;

  private:
    const Operator *operation_;
    std::vector<const Type *> type_parameters_;
    TypeMap type_map_;
};

const Type &python_type_argument(nanobind::handle value);

void bind_operators(nanobind::module_ &module);
void register_python_object_operators();
nanobind::object getitem(const PythonWiringPoint &collection, nanobind::handle key);

} // namespace bg::python::details
