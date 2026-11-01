#pragma once

#include <hg/operator.hpp>
#include <hg/python/export.hpp>

#include <nanobind/nanobind.h>

#include <span>
#include <string_view>

namespace hg::python {

// Exposes a process-lifetime C++ operator through the normal Python wiring interface.
HG_PYTHON_API void export_operator(nanobind::module_ &module, std::string_view python_name, const Operator &operation);

HG_PYTHON_API void export_operator(
    nanobind::module_ &module, std::string_view python_name, const Operator &operation,
    std::span<const Type *const> type_parameters);

template <typename Operation>
    requires requires(const Operation &operation) { operation.explicit_type_parameters(); }
void export_operator(nanobind::module_ &module, std::string_view python_name, const Operation &operation) {
    const auto type_parameters = operation.explicit_type_parameters();
    export_operator(module, python_name, operation, type_parameters);
}

} // namespace hg::python
