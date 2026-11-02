#pragma once

#include <hg/python/chrono.hpp>
#include <hg/python/export.hpp>

#include <nanobind/nanobind.h>

namespace hg::python {

HG_PYTHON_API void bind_core(nanobind::module_ &module);

} // namespace hg::python
