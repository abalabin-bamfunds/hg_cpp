#include <bg/node/details/aggregate_node_template_base.hpp>

#include <bg/graph/wiring_arguments.hpp>

namespace bg::details {

const Signature &AggregateNodeTemplateBase::signature() const {
    return *descriptor_->signature;
}

bool AggregateNodeTemplateBase::deduplicate_wiring() const noexcept {
    return descriptor_->deduplicate_wiring;
}

RequirementResult AggregateNodeTemplateBase::requirement(const WiringArguments &arguments) const {
    return descriptor_->requirement == nullptr ? accept : descriptor_->requirement(arguments);
}

EvaluationFunction AggregateNodeTemplateBase::evaluation_function(const ResolvedSignature &) const noexcept {
    return descriptor_->evaluation_function;
}

EvaluationFunction AggregateNodeTemplateBase::start_function(const ResolvedSignature &) const noexcept {
    return descriptor_->start_function;
}

EvaluationFunction AggregateNodeTemplateBase::stop_function(const ResolvedSignature &) const noexcept {
    return descriptor_->stop_function;
}

InvalidationBehavior AggregateNodeTemplateBase::invalidation_behavior(const ResolvedSignature &) const noexcept {
    return descriptor_->invalidation_behavior;
}

bool AggregateNodeTemplateBase::uses_time_series_input_runtime_type(std::size_t parameter_index) const noexcept {
    return descriptor_->parameter_runtime_type_uses[parameter_index] == AggregateRuntimeTypeUse::time_series;
}

bool AggregateNodeTemplateBase::uses_scalar_input_runtime_type(std::size_t parameter_index) const noexcept {
    return descriptor_->parameter_runtime_type_uses[parameter_index] == AggregateRuntimeTypeUse::scalar;
}

bool AggregateNodeTemplateBase::uses_output_runtime_type() const noexcept {
    return descriptor_->uses_output_runtime_type;
}

bool AggregateNodeTemplateBase::uses_future_scheduling() const noexcept {
    return descriptor_->uses_future_scheduling;
}

NodeStateLayout
AggregateNodeTemplateBase::state_layout(const ResolvedSignature &signature,
                                        std::span<const ScalarRuntimeType *const> parameter_runtime_types,
                                        std::span<const ScalarType *const> state_types) const {
    if (descriptor_->state_layout == nullptr) {
        return {};
    }
    return descriptor_->state_layout(signature, parameter_runtime_types, state_types);
}

void AggregateNodeTemplateBase::construct_initial_state(const NodeStateLayout &layout, void *storage) const noexcept {
    if (descriptor_->construct_initial_state != nullptr) {
        descriptor_->construct_initial_state(layout, storage);
    }
}

EvaluationFunction AggregateNodeTemplateBase::state_destruction_function(const ResolvedSignature &) const noexcept {
    return descriptor_->state_destruction_function;
}

std::vector<const TimeSeriesType *>
AggregateNodeTemplateBase::dynamic_reference_input_types(const TypeMap &type_map) const {
    if (descriptor_->dynamic_reference_input_types == nullptr) {
        return {};
    }
    return descriptor_->dynamic_reference_input_types(type_map);
}

std::vector<const ScalarType *> AggregateNodeTemplateBase::resolve_state_types(const TypeMap &type_map) const {
    if (descriptor_->resolve_state_types == nullptr) {
        return {};
    }
    return descriptor_->resolve_state_types(type_map);
}

} // namespace bg::details
