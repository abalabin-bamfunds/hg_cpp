#pragma once

#include <hg/python/export.hpp>

#include <nanobind/nanobind.h>

namespace hg {
class Schema;
}

namespace hg::python {

// Associate native schema projections and their metadata with a Python class. The optional scalar projection
// enables Bundle conversion; omitting it keeps time-series-only schemas independent of hierarchical scalars.
// The caller holds the GIL. Both schema descriptors must remain valid for the process lifetime.
HG_PYTHON_API void export_schema(nanobind::handle python_type, const Schema &time_series_schema,
                                 const Schema *scalar_schema = nullptr);

} // namespace hg::python
