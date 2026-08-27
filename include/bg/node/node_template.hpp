#pragma once

#include <bg/graph/evaluation_function.hpp>
#include <bg/graph/wiring_template.hpp>
#include <bg/node/invalidation.hpp>
#include <bg/type/signature.hpp>

#include <cstddef>

namespace bg {

class ScalarStorageRepresentation;

class NodeTemplate : public WiringTemplate {
  public:
    [[nodiscard]] virtual EvaluationFunction evaluation_function(const ResolvedSignature &signature) const noexcept = 0;
    [[nodiscard]] virtual EvaluationFunction start_function(const ResolvedSignature &) const noexcept {
        return nullptr;
    }
    [[nodiscard]] virtual EvaluationFunction stop_function(const ResolvedSignature &) const noexcept { return nullptr; }
    [[nodiscard]] virtual InvalidationBehavior invalidation_behavior(const ResolvedSignature &) const noexcept {
        return {};
    }
    [[nodiscard]] virtual const ScalarStorageRepresentation *output_storage_representation(
        const ResolvedSignature &signature,
        std::span<const ScalarStorageRepresentation *const> parameter_storage_representations) const noexcept = 0;
    [[nodiscard]] virtual bool uses_generic_time_series_input_view(std::size_t) const noexcept { return false; }
    [[nodiscard]] virtual bool uses_generic_scalar_input_view(std::size_t) const noexcept { return false; }
    [[nodiscard]] virtual bool uses_generic_output_view() const noexcept { return false; }
    [[nodiscard]] virtual bool uses_future_scheduling() const noexcept { return false; }
    [[nodiscard]] virtual std::size_t state_size(const ResolvedSignature &) const noexcept { return 0; }
    [[nodiscard]] virtual std::size_t state_alignment(const ResolvedSignature &) const noexcept { return 1; }
    virtual void construct_initial_state(const ResolvedSignature &, void *) const noexcept {}

  private:
    [[nodiscard]] std::optional<WiringPoint> wire(GraphWiring &wiring, ResolvedSignature signature,
                                                  std::span<details::wiring_argument> arguments) const final;
};

} // namespace bg
