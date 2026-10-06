#pragma once

#include <bg/allocator.hpp>
#include <bg/graph/evaluation_function.hpp>
#include <bg/graph/output_notification.hpp>
#include <bg/graph/wiring_template.hpp>
#include <bg/node/invalidation.hpp>
#include <bg/node/node_template_identity.hpp>
#include <bg/node/state_layout.hpp>
#include <bg/time_series/time_series_runtime_type.hpp>
#include <bg/type/signature.hpp>

#include <cstddef>
#include <optional>
#include <utility>
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

// A TS value output that the compiler can lower to a source and constant fallback input binding.
struct DefaultOutput {
    std::size_t source_parameter;
    std::size_t fallback_parameter;
};

class NodeTemplate : public WiringTemplate {
  public:
    NodeTemplate() : definition_identity_{NodeTemplateIdentity::unique()} {}

    // The returned value remains valid after this template object is destroyed.
    virtual NodeTemplateIdentity definition_identity() const { return definition_identity_; }
    virtual bool deduplicate_wiring() const noexcept { return true; }
    virtual bool schedule_on_start(const ResolvedSignature &) const noexcept { return false; }
    virtual EvaluationFunction evaluation_function(const ResolvedSignature &signature) const noexcept = 0;
    virtual NodeFunction start_function(const ResolvedSignature &) const noexcept { return nullptr; }
    virtual NodeFunction stop_function(const ResolvedSignature &) const noexcept { return nullptr; }
    virtual InvalidationBehavior invalidation_behavior(const ResolvedSignature &) const noexcept { return {}; }
    virtual bool uses_time_series_input_runtime_type(const ResolvedSignature &, std::size_t) const noexcept {
        return false;
    }
    virtual bool uses_scalar_input_runtime_type(const ResolvedSignature &, std::size_t) const noexcept { return false; }
    virtual bool uses_output_runtime_type() const noexcept { return false; }
    virtual const TimeSeriesRuntimeType *output_runtime_type(const ResolvedSignature &) const { return nullptr; }
    virtual bool uses_node_invocation() const noexcept { return false; }
    // Enables CompiledNode::input_node_index and stores one index per parameter on this compiled node.
    virtual bool uses_input_node_indices() const noexcept { return false; }
    virtual bool uses_timed_scheduling() const noexcept { return false; }
    virtual bool uses_cycle_callbacks() const noexcept { return false; }
    virtual InputNotification input_notification(std::size_t) const noexcept { return {}; }
    virtual NodeStateLayout state_layout(const ResolvedSignature &, std::span<const ScalarRuntimeType *const>,
                                         std::span<const ScalarType *const>) const {
        return {};
    }
    virtual void construct_initial_state(const NodeStateLayout &, void *) const noexcept {}
    virtual NodeFunction state_destruction_function(const ResolvedSignature &) const noexcept { return nullptr; }

    [[nodiscard]] virtual std::vector<const TimeSeriesType *> dynamic_reference_input_types(const TypeMap &) const {
        return {};
    }

    [[nodiscard]] virtual std::vector<const ScalarType *> resolve_state_types(const TypeMap &) const { return {}; }

    void resolve_additional_types(const TypeMap &type_map) const override {
        (void)resolve_state_types(type_map);
        (void)dynamic_reference_input_types(type_map);
    }

    virtual CompiledNodeDataLayout compiled_data_layout(const ResolvedSignature &) const noexcept { return {}; }
    virtual void construct_compiled_data(const ResolvedSignature &, void *, allocator &) const {}
    virtual std::optional<ConstantOutput> constant_output(const ResolvedSignature &) const noexcept {
        return std::nullopt;
    }
    virtual std::optional<DefaultOutput> default_output(const ResolvedSignature &) const noexcept {
        return std::nullopt;
    }

    // Engine-facing concrete wiring path for a node call retained by GraphWiring.
    [[nodiscard]] std::optional<WiringPoint> bind(GraphWiring &wiring, ResolvedSignature signature,
                                                  const TypeMap &type_map,
                                                  std::span<const WiringArgument> arguments,
                                                  const TimeSeriesRuntimeType *output_runtime_type) const;

  protected:
    explicit NodeTemplate(NodeTemplateIdentity identity) : definition_identity_{std::move(identity)} {}

  private:
    [[nodiscard]] std::optional<WiringPoint> wire(GraphWiring &wiring, ResolvedSignature signature,
                                                  const TypeMap &type_map,
                                                  std::span<const WiringArgument> arguments) const final;

    NodeTemplateIdentity definition_identity_;
};

} // namespace bg
