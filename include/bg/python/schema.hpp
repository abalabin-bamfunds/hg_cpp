#pragma once

#include <bg/python/export.hpp>

#include <nanobind/nanobind.h>

namespace bg {
class Schema;
}

namespace bg::python {

// Associate native schema projections and their metadata with a Python class. The optional scalar projection
// enables Bundle conversion; omitting it keeps time-series-only schemas independent of hierarchical scalars.
// The caller holds the GIL. Both schema descriptors must remain valid for the process lifetime.
BG_PYTHON_API void export_schema(nanobind::handle python_type, const Schema &time_series_schema,
                                 const Schema *scalar_schema = nullptr);

} // namespace bg::python
