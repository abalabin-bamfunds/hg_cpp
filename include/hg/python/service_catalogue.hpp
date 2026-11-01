#pragma once

#include <hg/python/export.hpp>

#include <nanobind/nanobind.h>

namespace hg {
class ServiceCatalogue;
}

namespace hg::python {

// Return the catalogue owned by a Python ServiceCatalogue wrapper. The caller holds the GIL, and the returned
// reference remains valid while the wrapper lives.
HG_PYTHON_API ServiceCatalogue &service_catalogue(nanobind::handle catalogue);

} // namespace hg::python
