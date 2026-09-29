#pragma once

#include <bg/python/export.hpp>

#include <nanobind/nanobind.h>

#include <string_view>

namespace bg {
class Operator;
}

namespace bg::python {

// Exposes a process-lifetime C++ operator through the normal Python wiring interface.
BG_PYTHON_API void export_operator(nanobind::module_ &module, std::string_view python_name, const Operator &operation);

} // namespace bg::python
