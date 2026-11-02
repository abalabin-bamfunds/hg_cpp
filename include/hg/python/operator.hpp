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

// Registers a WiringPoint method for receiver types matched by `receiver_type`. Call with the Python GIL held.
// `receiver_type` and `operation` must remain alive for the process lifetime. Repeated registrations of the same
// name, receiver type, and operator are idempotent. A method name cannot refer to different operators.
// Calling the bound method prepends the receiver and passes remaining positional and keyword arguments to the operator.
HG_PYTHON_API void register_wiring_method(const Type &receiver_type, std::string_view name,
                                         const Operator &operation);

template <typename Operation>
    requires requires(const Operation &operation) { operation.explicit_type_parameters(); }
void export_operator(nanobind::module_ &module, std::string_view python_name, const Operation &operation) {
    const auto type_parameters = operation.explicit_type_parameters();
    export_operator(module, python_name, operation, type_parameters);
}

} // namespace hg::python
