#pragma once

#include <bg/graph/wiring_template.hpp>
#include <bg/python/export.hpp>
#include <bg/python/node_definition.hpp>

#include <nanobind/nanobind.h>

#include <cstddef>
#include <memory>
#include <span>

namespace bg::python {

class BG_PYTHON_API PythonGraphDefinition final : public WiringTemplate {
  public:
    PythonGraphDefinition(std::shared_ptr<const PythonNodeDefinition::SignatureDefinition> signature,
                          nanobind::handle function, bool lift_scalar_output);

    const Signature &signature() const override { return signature_->signature; }
    std::span<const Type *const> wiring_parameter_types() const noexcept {
        return signature_->wiring_parameter_types;
    }
    PythonGraphDefinition specialize(std::span<const Type *const> input_types) const;

  private:
    std::optional<WiringPoint> wire(GraphWiring &, ResolvedSignature, const WiringArguments &,
                                    std::span<const WiringArgument> arguments) const override;

    std::shared_ptr<const PythonNodeDefinition::SignatureDefinition> signature_;
    WeakObject function_;
    bool lift_scalar_output_;
};

} // namespace bg::python
