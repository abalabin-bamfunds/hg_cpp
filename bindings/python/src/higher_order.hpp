#pragma once

#include <nanobind/nanobind.h>

namespace hg::python::details {

void bind_higher_order(nanobind::module_ &module);

} // namespace hg::python::details
