#pragma once

#include <bg/allocator.hpp>
#include <bg/python/export.hpp>
#include <bg/scalar/dispatch.hpp>
#include <bg/scalar/scalar.hpp>
#include <bg/scalar/value.hpp>

#include <nanobind/nanobind.h>

#include <memory>

namespace bg::python {

class PythonScalarConversions;

using ScalarToPython = nanobind::object (*)(Scalar<>, const PythonScalarConversions &);
using PythonToScalar = ScalarValue<> (*)(nanobind::handle, allocator &, const PythonScalarConversions &);

// Extensions register Python-owned runtime representations here. Built-in and structural values use Scalar::visit().
class BG_PYTHON_API PythonScalarConversions {
  public:
    PythonScalarConversions();
    ~PythonScalarConversions();

    PythonScalarConversions(const PythonScalarConversions &) = delete;
    PythonScalarConversions &operator=(const PythonScalarConversions &) = delete;

    [[nodiscard]] bool add(const ScalarType &type, PythonToScalar conversion);
    [[nodiscard]] bool add(const ScalarType &type, ScalarToPython conversion);
    [[nodiscard]] bool add(const ScalarRuntimeType &runtime_type, ScalarToPython conversion);
    nanobind::object to_python(Scalar<> value) const;
    [[nodiscard]] ScalarValue<> from_python(const ScalarType &type, nanobind::handle value, allocator &memory) const;
    bool can_from_python(const ScalarType &type) const noexcept;
    PythonToScalar from_python_handler(const ScalarType &type) const noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

BG_PYTHON_API PythonScalarConversions &scalar_conversions();

} // namespace bg::python
