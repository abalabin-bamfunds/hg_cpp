#pragma once

#include <nanobind/nanobind.h>

namespace bg::python::details {

void bind_higher_order(nanobind::module_ &module);

} // namespace bg::python::details
