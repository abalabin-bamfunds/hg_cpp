#pragma once

#include <bg/python/export.hpp>

#include <nanobind/nanobind.h>

namespace bg {
class ServiceCatalogue;
}

namespace bg::python {

// Return the catalogue owned by a Python ServiceCatalogue wrapper. The caller holds the GIL, and the returned
// reference remains valid while the wrapper lives.
BG_PYTHON_API ServiceCatalogue &service_catalogue(nanobind::handle catalogue);

} // namespace bg::python
