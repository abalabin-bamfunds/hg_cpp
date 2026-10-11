#pragma once

#include <bg/allocator.hpp>
#include <bg/python/export.hpp>
#include <bg/scalar/dispatch.hpp>
#include <bg/scalar/scalar.hpp>
#include <bg/scalar/value.hpp>
#include <bg/util/symbol.hpp>

#include <nanobind/nanobind.h>

#include <cstdint>
#include <memory>

namespace bg::python {

class CallbackLifetime;
class PythonScalarConversions;

using ScalarToPython = nanobind::object (*)(Scalar<>, const PythonScalarConversions &);
using ScalarToPythonView = nanobind::object (*)(Scalar<>, const PythonScalarConversions &,
                                                const std::shared_ptr<CallbackLifetime> &, std::uint64_t);
using PythonToScalar = ScalarValue<> (*)(nanobind::handle, allocator &, const PythonScalarConversions &);

// Maps Python classes and scalar types to their Python conversion handlers.
class BG_PYTHON_API PythonScalarConversions {
  public:
    PythonScalarConversions();
    ~PythonScalarConversions();

    PythonScalarConversions(const PythonScalarConversions &) = delete;
    PythonScalarConversions &operator=(const PythonScalarConversions &) = delete;

    [[nodiscard]] bool add(const ScalarType &type, PythonToScalar conversion);
    [[nodiscard]] bool add(const ScalarType &type, ScalarToPython conversion);
    [[nodiscard]] bool add(const ScalarRuntimeType &runtime_type, ScalarToPython conversion);
    [[nodiscard]] bool add(const ScalarType &type, ScalarToPythonView conversion);
    [[nodiscard]] bool add(const ScalarRuntimeType &runtime_type, ScalarToPythonView conversion);
    [[nodiscard]] const ScalarType &add_python_type(nanobind::handle python_type);
    [[nodiscard]] bool add_python_type(nanobind::handle python_type, const ScalarType &type);
    // Selects an exact runtime handler before a semantic-type handler. Other values use structural conversion.
    ScalarToPython to_python_handler(const ScalarRuntimeType &runtime_type) const noexcept;
    nanobind::object to_python(Scalar<> value) const;
    nanobind::object symbol_to_python(util::symbol value) const;
    // Produces a direct node argument. A borrowed result must use the supplied callback lifetime.
    nanobind::object to_python_view(Scalar<> value, const std::shared_ptr<CallbackLifetime> &lifetime,
                                    std::uint64_t generation) const;
    const ScalarType &type_from_python(nanobind::handle python_type) const;
    [[nodiscard]] ScalarValue<> from_python(nanobind::handle value, allocator &memory) const;
    [[nodiscard]] ScalarValue<> from_python(const ScalarType &type, nanobind::handle value, allocator &memory) const;
    bool can_from_python(const ScalarType &type) const noexcept;
    PythonToScalar from_python_handler(const ScalarType &type) const noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

BG_PYTHON_API PythonScalarConversions &scalar_conversions();

} // namespace bg::python
