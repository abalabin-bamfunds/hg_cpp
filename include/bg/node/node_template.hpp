#pragma once

#include <bg/allocator.hpp>
#include <bg/graph/evaluation_function.hpp>
#include <bg/graph/output_notification.hpp>
#include <bg/graph/wiring_template.hpp>
#include <bg/node/invalidation.hpp>
#include <bg/node/state_layout.hpp>
#include <bg/time_series/time_series_runtime_type.hpp>
#include <bg/type/signature.hpp>

#include <cstddef>
#include <optional>
#include <vector>

namespace bg {

class ScalarType;

using CompiledNodeDataDestructionFunction = void (*)(void *) noexcept;

struct CompiledNodeDataLayout {
    std::size_t size{};
    std::size_t alignment{1};
    CompiledNodeDataDestructionFunction destruction_function{};
};

// A TS value output whose value is one scalar parameter and never changes.
struct ConstantOutput {
    std::size_t scalar_parameter;
};

class NodeTemplate : public WiringTemplate {
  public:
    virtual bool deduplicate_wiring() const noexcept { return true; }
    virtual EvaluationFunction evaluation_function(const ResolvedSignature &signature) const noexcept = 0;
    virtual EvaluationFunction start_function(const ResolvedSignature &) const noexcept { return nullptr; }
    virtual EvaluationFunction stop_function(const ResolvedSignature &) const noexcept { return nullptr; }
    virtual InvalidationBehavior invalidation_behavior(const ResolvedSignature &) const noexcept { return {}; }
    virtual bool uses_time_series_input_runtime_type(std::size_t) const noexcept { return false; }
    virtual bool uses_scalar_input_runtime_type(std::size_t) const noexcept { return false; }
    virtual bool uses_output_runtime_type() const noexcept { return false; }
    virtual bool uses_node_invocation() const noexcept { return false; }
    virtual bool uses_future_scheduling() const noexcept { return false; }
    virtual bool uses_cycle_callbacks() const noexcept { return false; }
    virtual InputNotification input_notification(std::size_t) const noexcept { return {}; }
    virtual NodeStateLayout state_layout(const ResolvedSignature &, std::span<const ScalarRuntimeType *const>,
                                         std::span<const ScalarType *const>) const {
        return {};
    }
    virtual void construct_initial_state(const NodeStateLayout &, void *) const noexcept {}
    virtual EvaluationFunction state_destruction_function(const ResolvedSignature &) const noexcept { return nullptr; }

    [[nodiscard]] virtual std::vector<const TimeSeriesType *> dynamic_reference_input_types(const TypeMap &) const {
        return {};
    }

    [[nodiscard]] virtual std::vector<const ScalarType *> resolve_state_types(const TypeMap &) const { return {}; }

    virtual CompiledNodeDataLayout compiled_data_layout(const ResolvedSignature &) const noexcept { return {}; }
    virtual void construct_compiled_data(const ResolvedSignature &, void *, allocator &) const {}
    virtual std::optional<ConstantOutput> constant_output(const ResolvedSignature &) const noexcept {
        return std::nullopt;
    }

  private:
    [[nodiscard]] std::optional<WiringPoint> wire(GraphWiring &wiring, ResolvedSignature signature,
                                                  const TypeMap &type_map,
                                                  std::span<const WiringArgument> arguments) const final;
};

} // namespace bg
