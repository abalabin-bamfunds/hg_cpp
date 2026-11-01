#pragma once

#include <bg/python/export.hpp>

#include <nanobind/nanobind.h>

namespace bg::python {

BG_PYTHON_API void bind_core(nanobind::module_ &module);

} // namespace bg::python
