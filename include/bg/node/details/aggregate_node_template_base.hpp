#pragma once

#include <bg/node/node_template.hpp>

#include <span>
#include <vector>

namespace bg::details {

using AggregateStateLayoutFunction = NodeStateLayout (*)(const ResolvedSignature &,
                                                         std::span<const ScalarRuntimeType *const>,
                                                         std::span<const ScalarType *const>);
using AggregateStateConstructionFunction = void (*)(const NodeStateLayout &, void *) noexcept;
using AggregateStateTypesFunction = std::vector<const ScalarType *> (*)(const TypeMap &);
using AggregateDynamicReferenceInputTypesFunction = std::vector<const TimeSeriesType *> (*)(const TypeMap &);
using AggregateRequirementFunction = RequirementResult (*)(const WiringArguments &);

enum class AggregateRuntimeTypeUse : unsigned char {
    none = 0,
    time_series = 1,
    scalar = 2,
};

struct AggregateNodeDescriptor {
    const Signature *signature;
    EvaluationFunction evaluation_function;
    NodeFunction start_function;
    bool schedule_on_start;
    NodeFunction stop_function;
    InvalidationBehavior invalidation_behavior;
    std::span<const AggregateRuntimeTypeUse> parameter_runtime_type_uses;
    AggregateStateLayoutFunction state_layout;
    AggregateStateConstructionFunction construct_initial_state;
    NodeFunction state_destruction_function;
    AggregateStateTypesFunction resolve_state_types;
    AggregateDynamicReferenceInputTypesFunction dynamic_reference_input_types;
    AggregateRequirementFunction requirement;
    bool uses_output_runtime_type;
    bool uses_timed_scheduling;
    bool deduplicate_wiring;
};

class AggregateNodeTemplateBase : public NodeTemplate {
  public:
    explicit AggregateNodeTemplateBase(const AggregateNodeDescriptor &descriptor)
        : NodeTemplate{NodeTemplateIdentity::persistent(&descriptor)}, descriptor_{&descriptor} {}

    const Signature &signature() const final;
    bool deduplicate_wiring() const noexcept final;
    bool schedule_on_start(const ResolvedSignature &) const noexcept final;
    RequirementResult requirement(const WiringArguments &arguments) const final;
    EvaluationFunction evaluation_function(const ResolvedSignature &) const noexcept final;
    NodeFunction start_function(const ResolvedSignature &) const noexcept final;
    NodeFunction stop_function(const ResolvedSignature &) const noexcept final;
    InvalidationBehavior invalidation_behavior(const ResolvedSignature &) const noexcept final;
    bool uses_time_series_input_runtime_type(const ResolvedSignature &,
                                             std::size_t parameter_index) const noexcept final;
    bool uses_scalar_input_runtime_type(const ResolvedSignature &, std::size_t parameter_index) const noexcept final;
    bool uses_output_runtime_type() const noexcept final;
    bool uses_timed_scheduling() const noexcept final;
    NodeStateLayout state_layout(const ResolvedSignature &signature,
                                 std::span<const ScalarRuntimeType *const> parameter_runtime_types,
                                 std::span<const ScalarType *const> state_types) const final;
    void construct_initial_state(const NodeStateLayout &layout, void *storage) const noexcept final;
    NodeFunction state_destruction_function(const ResolvedSignature &) const noexcept final;
    std::vector<const TimeSeriesType *> dynamic_reference_input_types(const TypeMap &) const final;
    std::vector<const ScalarType *> resolve_state_types(const TypeMap &type_map) const final;

  private:
    const AggregateNodeDescriptor *descriptor_;
};

} // namespace bg::details
