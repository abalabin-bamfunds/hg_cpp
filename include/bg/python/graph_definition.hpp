#pragma once

#include <bg/graph/wiring_template.hpp>
#include <bg/python/export.hpp>
#include <bg/python/node_definition.hpp>

#include <nanobind/nanobind.h>

#include <memory>

namespace bg::python {

class BG_PYTHON_API PythonGraphDefinition final : public WiringTemplate {
  public:
    PythonGraphDefinition(std::shared_ptr<const PythonNodeDefinition::SignatureDefinition> signature,
                          nanobind::handle function);

    const Signature &signature() const override { return signature_->signature; }

  private:
    std::optional<WiringPoint> wire(GraphWiring &, ResolvedSignature, const TypeMap &,
                                    std::span<const WiringArgument> arguments) const override;

    std::shared_ptr<const PythonNodeDefinition::SignatureDefinition> signature_;
    WeakObject function_;
};

} // namespace bg::python
