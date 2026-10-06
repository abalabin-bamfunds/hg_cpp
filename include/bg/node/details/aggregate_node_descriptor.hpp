#pragma once

#include <bg/node/details/aggregate_node.hpp>
#include <bg/node/details/aggregate_node_template_base.hpp>

#include <array>

namespace bg::details {

template <typename Definition, std::size_t... Index>
consteval auto aggregate_parameter_runtime_type_uses(std::index_sequence<Index...>) {
    constexpr auto ParameterCount = parameter_index<Definition>(std::index_sequence<Index...>{});
    auto result = std::array<AggregateRuntimeTypeUse, ParameterCount>{};
    [[maybe_unused]] auto parameter = std::size_t{};

    (
        [&] {
            using Field = std::remove_cv_t<util::aggregate_member_type<Index, Definition>>;

            if constexpr (input_field<Field>::value) {
                if constexpr (input_field<Field>::dynamic_binding) {
                    return;
                }
                if constexpr (input_runtime_type_field<Field>::value) {
                    result[parameter] = AggregateRuntimeTypeUse::time_series;
                }
                ++parameter;
            } else if constexpr (scalar_field<Field>::value) {
                if constexpr (generic_scalar_input_field<Field>::value) {
                    result[parameter] = AggregateRuntimeTypeUse::scalar;
                }
                ++parameter;
            }
        }(),
        ...);

    return result;
}

template <typename Definition, std::size_t... Index>
consteval bool aggregate_node_has_state(std::index_sequence<Index...>) {
    return (false || ... || state_field<std::remove_cv_t<util::aggregate_member_type<Index, Definition>>>::value);
}

template <typename Definition> const AggregateNodeDescriptor &aggregate_node_descriptor() {
    constexpr auto MemberCount = util::aggregate_member_count<Definition>;
    using Index = std::make_index_sequence<MemberCount>;
    constexpr auto HasDynamicReferenceInputs = aggregate_has_dynamic_reference_inputs<Definition>(Index{});
    constexpr auto HasStoredState = aggregate_node_has_state<Definition>(Index{}) || HasDynamicReferenceInputs;
    static_assert(!aggregate_node_has_finalize<Definition> || HasStoredState,
                  "an aggregate node with finalize() must contain state");
    static constexpr auto parameter_runtime_type_uses = aggregate_parameter_runtime_type_uses<Definition>(Index{});

    static const auto descriptor = AggregateNodeDescriptor{
        .signature = &aggregate_node_signature<Definition>(),
        .evaluation_function =
            [] {
                if constexpr (aggregate_node_has_evaluate<Definition>) {
                    return &evaluate_aggregate_node<Definition>;
                } else {
                    return EvaluationFunction{};
                }
            }(),
        .start_function =
            [] {
                if constexpr (aggregate_node_has_start<Definition>) {
                    return &start_aggregate_node<Definition>;
                } else {
                    return NodeFunction{};
                }
            }(),
        .schedule_on_start = aggregate_schedules_on_start<Definition>(),
        .stop_function =
            [] {
                if constexpr (aggregate_node_has_stop<Definition> || HasDynamicReferenceInputs) {
                    return &stop_aggregate_node<Definition>;
                } else {
                    return NodeFunction{};
                }
            }(),
        .invalidation_behavior =
            [] {
                if constexpr (aggregate_node_has_invalidation_function<Definition>) {
                    return InvalidationBehavior{InvalidationMode::invoke, &invalidate_aggregate_node<Definition>};
                } else if constexpr (aggregate_node_preserves_output<Definition>) {
                    return InvalidationBehavior{InvalidationMode::preserve_output};
                } else if constexpr (aggregate_output_requires_view_invalidation<Definition>(Index{})) {
                    return InvalidationBehavior{InvalidationMode::invoke, &invalidate_aggregate_output<Definition>};
                } else {
                    return InvalidationBehavior{};
                }
            }(),
        .parameter_runtime_type_uses = parameter_runtime_type_uses,
        .state_layout =
            [] {
                if constexpr (HasStoredState) {
                    return AggregateStateLayoutFunction{[](const ResolvedSignature &signature,
                                                           std::span<const ScalarRuntimeType *const> parameter_types,
                                                           std::span<const ScalarType *const> state_types) {
                        return aggregate_state_layout<Definition>(signature, parameter_types, state_types, Index{});
                    }};
                } else {
                    return AggregateStateLayoutFunction{};
                }
            }(),
        .construct_initial_state =
            [] {
                if constexpr (HasStoredState) {
                    return AggregateStateConstructionFunction{
                        [](const NodeStateLayout &layout, void *storage) noexcept {
                            construct_aggregate_state<Definition>(layout, storage, Index{});
                        }};
                } else {
                    return AggregateStateConstructionFunction{};
                }
            }(),
        .state_destruction_function =
            [] {
                if constexpr (aggregate_node_has_finalize<Definition> ||
                              aggregate_state_requires_destruction<Definition>(Index{}) ||
                              HasDynamicReferenceInputs) {
                    return static_cast<NodeFunction>(&finalize_aggregate_node_state<Definition>);
                } else {
                    return NodeFunction{};
                }
            }(),
        .resolve_state_types =
            [] {
                if constexpr (aggregate_node_has_state<Definition>(Index{})) {
                    return AggregateStateTypesFunction{
                        [](const TypeMap &type_map) { return aggregate_state_types<Definition>(type_map, Index{}); }};
                } else {
                    return AggregateStateTypesFunction{};
                }
            }(),
        .dynamic_reference_input_types =
            [] {
                if constexpr (HasDynamicReferenceInputs) {
                    return AggregateDynamicReferenceInputTypesFunction{[](const TypeMap &type_map) {
                        return aggregate_dynamic_reference_input_types<Definition>(type_map, Index{});
                    }};
                } else {
                    return AggregateDynamicReferenceInputTypesFunction{};
                }
            }(),
        .requirement =
            [] {
                if constexpr (requires(const WiringArguments &arguments) {
                                  requirement_result(Definition::require(arguments));
                              }) {
                    return AggregateRequirementFunction{[](const WiringArguments &arguments) {
                        return requirement_result(Definition::require(arguments));
                    }};
                } else {
                    return AggregateRequirementFunction{};
                }
            }(),
        .uses_output_runtime_type = aggregate_uses_output_runtime_type<Definition>(Index{}),
        .uses_input_node_indices = [] {
            if constexpr (requires { Definition::uses_input_node_indices; }) {
                return static_cast<bool>(Definition::uses_input_node_indices);
            }
            return false;
        }(),
        .uses_timed_scheduling = aggregate_uses_timed_scheduling<Definition>(),
        .deduplicate_wiring =
            [] {
                if constexpr (requires { Definition::deduplicate_wiring; }) {
                    return static_cast<bool>(Definition::deduplicate_wiring);
                }
                return true;
            }(),
    };

    return descriptor;
}

} // namespace bg::details
