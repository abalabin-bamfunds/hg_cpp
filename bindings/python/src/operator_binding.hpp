#pragma once

#include <nanobind/nanobind.h>

namespace bg::python {
class PythonWiringPoint;
}

namespace bg::python::details {

void bind_operators(nanobind::module_ &module);
nanobind::object getitem(const PythonWiringPoint &collection, nanobind::handle key);

} // namespace bg::python::details
