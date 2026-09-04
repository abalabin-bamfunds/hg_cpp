#pragma once

#include <bg/graph/details/wiring_callable.hpp>
#include <bg/graph/wiring_error.hpp>
#include <bg/node/details/aggregate_node.hpp>
#include <bg/node/node_template.hpp>

#include <stdexcept>

namespace bg {

template <typename Definition>
concept AggregateNodeClass = details::aggregate_node_class<Definition>::value;

template <AggregateNodeClass Definition, typename Label = details::default_wiring_label>
class AggregateNodeTemplate final : public NodeTemplate {
  public:
    using definition_type = Definition;
    explicit AggregateNodeTemplate(Label label = {}) : label_(std::move(label)) {
        if constexpr (!std::same_as<Label, details::default_wiring_label>) {
            if (signature().parameter_count() != details::callable<Label>::argument_count) {
                throw std::invalid_argument{"a node label function and its signature have different parameter counts"};
            }
        }
    }

    template <typename... Arguments>
        requires((details::graph_code_argument<Arguments> && ...) && details::aggregate_node_has_output<Definition>())
    [[nodiscard]] auto operator()(Arguments &&...arguments) const & {
        using Output = details::aggregate_output_time_series_t<Definition>;
        return wp<Output>{*NodeTemplate::operator()(std::forward<Arguments>(arguments)...)};
    }

    template <typename... Arguments>
        requires((details::graph_code_argument<Arguments> && ...) && details::aggregate_node_has_output<Definition>())
    [[nodiscard]] auto operator()(const TypeMap &bindings, Arguments &&...arguments) const & {
        using Output = details::aggregate_output_time_series_t<Definition>;
        return wp<Output>{*NodeTemplate::operator()(bindings, std::forward<Arguments>(arguments)...)};
    }

    template <typename... Arguments>
        requires((details::graph_code_argument<Arguments> && ...) && !details::aggregate_node_has_output<Definition>())
    void operator()(Arguments &&...arguments) const & {
        (void)NodeTemplate::operator()(std::forward<Arguments>(arguments)...);
    }

    template <typename... Arguments>
        requires((details::graph_code_argument<Arguments> && ...) && !details::aggregate_node_has_output<Definition>())
    void operator()(const TypeMap &bindings, Arguments &&...arguments) const & {
        (void)NodeTemplate::operator()(bindings, std::forward<Arguments>(arguments)...);
    }

    template <typename... Arguments> void operator()(Arguments &&...) const && = delete;

    [[nodiscard]] const Signature &signature() const override {
        return details::aggregate_node_signature<Definition>();
    }

    [[nodiscard]] EvaluationFunction evaluation_function(const ResolvedSignature &) const noexcept override {
        if constexpr (details::aggregate_node_has_evaluate<Definition>) {
            return &details::evaluate_aggregate_node<Definition>;
        } else {
            return nullptr;
        }
    }

    [[nodiscard]] EvaluationFunction start_function(const ResolvedSignature &) const noexcept override {
        if constexpr (details::aggregate_node_has_start<Definition>) {
            return &details::start_aggregate_node<Definition>;
        } else {
            return nullptr;
        }
    }

    [[nodiscard]] EvaluationFunction stop_function(const ResolvedSignature &) const noexcept override {
        if constexpr (details::aggregate_node_has_stop<Definition>) {
            return &details::stop_aggregate_node<Definition>;
        } else {
            return nullptr;
        }
    }

    [[nodiscard]] InvalidationBehavior invalidation_behavior(const ResolvedSignature &) const noexcept override {
        if constexpr (details::aggregate_node_has_invalidation_function<Definition>) {
            return {InvalidationMode::invoke, &details::invalidate_aggregate_node<Definition>};
        } else if constexpr (details::aggregate_node_preserves_output<Definition>) {
            return {InvalidationMode::preserve_output};
        } else if constexpr (details::aggregate_output_requires_view_invalidation<Definition>(
                                 std::make_index_sequence<util::aggregate_member_count<Definition>>{})) {
            return {InvalidationMode::invoke, &details::invalidate_aggregate_output<Definition>};
        } else {
            return {};
        }
    }

    [[nodiscard]] const TimeSeriesRuntimeType *
    output_runtime_type(const ResolvedSignature &signature,
                        std::span<const ScalarRuntimeType *const> parameter_runtime_types,
                        std::span<const TimeSeriesRuntimeType *const> time_series_input_runtime_types) const override {
        return details::aggregate_output_runtime_type<Definition>(
            signature, parameter_runtime_types, time_series_input_runtime_types,
            std::make_index_sequence<util::aggregate_member_count<Definition>>{});
    }

    [[nodiscard]] bool uses_time_series_input_runtime_type(std::size_t parameter_index) const noexcept override {
        return details::aggregate_uses_time_series_input_runtime_type<Definition>(
            parameter_index, std::make_index_sequence<util::aggregate_member_count<Definition>>{});
    }

    [[nodiscard]] bool uses_scalar_input_runtime_type(std::size_t parameter_index) const noexcept override {
        return details::aggregate_uses_scalar_input_runtime_type<Definition>(
            parameter_index, std::make_index_sequence<util::aggregate_member_count<Definition>>{});
    }

    [[nodiscard]] bool uses_output_runtime_type() const noexcept override {
        return details::aggregate_uses_output_runtime_type<Definition>(
            std::make_index_sequence<util::aggregate_member_count<Definition>>{});
    }

    [[nodiscard]] bool uses_future_scheduling() const noexcept override {
        return details::aggregate_uses_future_scheduling<Definition>();
    }

    [[nodiscard]] NodeStateLayout state_layout(const ResolvedSignature &signature,
                                               std::span<const ScalarRuntimeType *const> parameter_runtime_types,
                                               std::span<const ScalarType *const> state_types) const override {
        return details::aggregate_state_layout<Definition>(
            signature, parameter_runtime_types, state_types,
            std::make_index_sequence<util::aggregate_member_count<Definition>>{});
    }

    void construct_initial_state(const NodeStateLayout &layout, void *storage) const noexcept override {
        details::construct_aggregate_state<Definition>(
            layout, storage, std::make_index_sequence<util::aggregate_member_count<Definition>>{});
    }

    [[nodiscard]] EvaluationFunction state_destruction_function(const ResolvedSignature &) const noexcept override {
        if constexpr (details::aggregate_state_requires_destruction<Definition>(
                          std::make_index_sequence<util::aggregate_member_count<Definition>>{})) {
            return &details::destroy_aggregate_state<Definition>;
        } else {
            return nullptr;
        }
    }

    [[nodiscard]] std::vector<const ScalarType *> resolve_state_types(const TypeMap &bindings) const override {
        return details::aggregate_state_types<Definition>(
            bindings, std::make_index_sequence<util::aggregate_member_count<Definition>>{});
    }

  private:
    [[nodiscard]] util::symbol label(const ResolvedSignature &signature, std::span<details::wiring_argument> arguments,
                                     util::symbol default_label) const override {
        if constexpr (std::same_as<Label, details::default_wiring_label>) {
            return default_label;
        } else {
            if (!details::wiring_parameters_match<Label>(signature)) {
                throw WiringError{"a node label function does not match its resolved signature"};
            }

            return details::wiring_label(details::invoke_graph_function(label_, arguments));
        }
    }

    [[no_unique_address]] Label label_;
};

} // namespace bg
