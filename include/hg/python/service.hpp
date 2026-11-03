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

// Return the definition owned by the Python service declaration. The caller holds the GIL and keeps the Python
// declaration alive for as long as the returned reference is used.
HG_PYTHON_API const ServiceDefinition &service_definition(nanobind::handle python_service);

} // namespace hg::python
