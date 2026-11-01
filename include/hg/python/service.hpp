#pragma once

#include <hg/python/export.hpp>

#include <nanobind/nanobind.h>

namespace hg {
class ServiceDefinition;
}

namespace hg::python {

// Associate a Python service declaration with a native service definition before wiring or catalogue use.
// The caller holds the GIL. The native service definition must outlive the Python declaration.
HG_PYTHON_API void export_service(nanobind::handle python_service, const ServiceDefinition &service);

} // namespace hg::python
