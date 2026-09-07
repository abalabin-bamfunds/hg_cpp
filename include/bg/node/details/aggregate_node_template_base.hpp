#pragma once

#include <bg/node/node_template.hpp>

#include <span>
#include <vector>

namespace bg::details {

using AggregateOutputRuntimeTypeFunction = const TimeSeriesRuntimeType *(*)(
    const ResolvedSignature &, std::span<const ScalarRuntimeType *const>,
    std::span<const TimeSeriesRuntimeType *const>);
using AggregateStateLayoutFunction = NodeStateLayout (*)(const ResolvedSignature &,
                                                         std::span<const ScalarRuntimeType *const>,
                                                         std::span<const ScalarType *const>);
using AggregateStateConstructionFunction = void (*)(const NodeStateLayout &, void *) noexcept;
using AggregateStateTypesFunction = std::vector<const ScalarType *> (*)(const TypeMap &);

enum class AggregateRuntimeTypeUse : unsigned char {
    none = 0,
    time_series = 1,
    scalar = 2,
};

struct AggregateNodeDescriptor {
    const Signature *signature;
    EvaluationFunction evaluation_function;
    EvaluationFunction start_function;
    EvaluationFunction stop_function;
    InvalidationBehavior invalidation_behavior;
    AggregateOutputRuntimeTypeFunction output_runtime_type;
    std::span<const AggregateRuntimeTypeUse> parameter_runtime_type_uses;
    AggregateStateLayoutFunction state_layout;
    AggregateStateConstructionFunction construct_initial_state;
    EvaluationFunction state_destruction_function;
    AggregateStateTypesFunction resolve_state_types;
    bool uses_output_runtime_type;
    bool uses_future_scheduling;
};

class AggregateNodeTemplateBase : public NodeTemplate {
  public:
    explicit AggregateNodeTemplateBase(const AggregateNodeDescriptor &descriptor) noexcept : descriptor_{&descriptor} {}

    const Signature &signature() const final;
    EvaluationFunction evaluation_function(const ResolvedSignature &) const noexcept final;
    EvaluationFunction start_function(const ResolvedSignature &) const noexcept final;
    EvaluationFunction stop_function(const ResolvedSignature &) const noexcept final;
    InvalidationBehavior invalidation_behavior(const ResolvedSignature &) const noexcept final;
    const TimeSeriesRuntimeType *
    output_runtime_type(const ResolvedSignature &signature,
                        std::span<const ScalarRuntimeType *const> parameter_runtime_types,
                        std::span<const TimeSeriesRuntimeType *const> time_series_input_runtime_types) const final;
    bool uses_time_series_input_runtime_type(std::size_t parameter_index) const noexcept final;
    bool uses_scalar_input_runtime_type(std::size_t parameter_index) const noexcept final;
    bool uses_output_runtime_type() const noexcept final;
    bool uses_future_scheduling() const noexcept final;
    NodeStateLayout state_layout(const ResolvedSignature &signature,
                                 std::span<const ScalarRuntimeType *const> parameter_runtime_types,
                                 std::span<const ScalarType *const> state_types) const final;
    void construct_initial_state(const NodeStateLayout &layout, void *storage) const noexcept final;
    EvaluationFunction state_destruction_function(const ResolvedSignature &) const noexcept final;
    std::vector<const ScalarType *> resolve_state_types(const TypeMap &bindings) const final;

  private:
    const AggregateNodeDescriptor *descriptor_;
};

} // namespace bg::details
