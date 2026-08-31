#pragma once

#include <bg/graph/evaluation_function.hpp>
#include <bg/graph/wiring_template.hpp>
#include <bg/node/invalidation.hpp>
#include <bg/node/state_layout.hpp>
#include <bg/time_series/time_series_storage_representation.hpp>
#include <bg/type/signature.hpp>

#include <cstddef>
#include <optional>
#include <vector>

namespace bg {

class ScalarType;

// Routes changes from one time series input to a dynamic subscriber list stored in node state.
struct DynamicSubscribersLayout {
    std::size_t source_input;
    std::size_t state_offset;
};

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
    [[nodiscard]] virtual const TimeSeriesStorageRepresentation *
    output_storage_representation(const ResolvedSignature &signature,
                                  std::span<const ScalarStorageRepresentation *const> parameter_storage_representations,
                                  std::span<const TimeSeriesStorageRepresentation *const>
                                      time_series_input_storage_representations) const noexcept = 0;
    [[nodiscard]] virtual bool uses_generic_time_series_input_view(std::size_t) const noexcept { return false; }
    [[nodiscard]] virtual bool uses_generic_scalar_input_view(std::size_t) const noexcept { return false; }
    [[nodiscard]] virtual bool uses_generic_output_view() const noexcept { return false; }
    [[nodiscard]] virtual bool uses_future_scheduling() const noexcept { return false; }
    [[nodiscard]] virtual std::optional<DynamicSubscribersLayout>
    dynamic_subscribers_layout(const ResolvedSignature &) const noexcept {
        return std::nullopt;
    }
    [[nodiscard]] virtual NodeStateLayout state_layout(const ResolvedSignature &,
                                                       std::span<const ScalarStorageRepresentation *const>,
                                                       std::span<const ScalarType *const>) const {
        return {};
    }
    virtual void construct_initial_state(const NodeStateLayout &, void *) const noexcept {}
    [[nodiscard]] virtual EvaluationFunction state_destruction_function(const ResolvedSignature &) const noexcept {
        return nullptr;
    }

    [[nodiscard]] virtual std::vector<const ScalarType *> resolve_state_types(const TypeMap &) const { return {}; }

  private:
    [[nodiscard]] std::optional<WiringPoint> wire(GraphWiring &wiring, ResolvedSignature signature,
                                                  const TypeMap &bindings,
                                                  std::span<details::wiring_argument> arguments) const final;
};

} // namespace bg
