#pragma once

#include <hg/node.hpp>
#include <hg/node/details/aggregate_node.hpp>
#include <hg/node/details/aggregate_node_descriptor.hpp>
#include <hg/node/details/callable_node.hpp>
#include <hg/python/gil.hpp>

#include <tuple>
#include <type_traits>
#include <utility>

namespace hg::python::details {

template <typename Definition> class NodeDefinitionAdapter : public Definition {
    static_assert(!std::is_final_v<Definition>, "Python-aware runtime-signature node definitions must not be final");

    using CompiledData = hg::details::node_compiled_data_t<Definition>;
    using State = hg::details::node_state_t<Definition>;

  public:
    using Invocation = NodeInvocation<CompiledData, State>;

    explicit NodeDefinitionAdapter(Definition definition) : Definition{std::move(definition)} {}

    static auto evaluate(Invocation invocation)
        requires requires(Invocation value) { Definition::evaluate(value); }
    {
        auto gil = GilScope{};
        if constexpr (std::same_as<decltype(Definition::evaluate(invocation)), bool>) {
            const auto complete = Definition::evaluate(invocation);
            gil.check_error();
            return complete;
        } else {
            Definition::evaluate(invocation);
            gil.check_error();
        }
    }

    static void start(Invocation invocation)
        requires requires(Invocation value) { Definition::start(value); }
    {
        auto gil = GilScope{};
        Definition::start(invocation);
        gil.check_error();
    }

    static void stop(Invocation invocation) noexcept
        requires requires(Invocation value) { Definition::stop(value); }
    {
        try {
            auto gil = GilScope{};
            if (!gil) {
                return;
            }
            Definition::stop(invocation);
            gil.report_error();
        } catch (...) {
            report_unraisable(std::current_exception());
        }
    }

    static void on_invalidation(Invocation invocation)
        requires requires(Invocation value) { Definition::on_invalidation(value); }
    {
        auto gil = GilScope{};
        Definition::on_invalidation(invocation);
        gil.check_error();
    }

    static void destroy_state(Invocation invocation) noexcept
        requires requires(Invocation value) { Definition::destroy_state(value); }
    {
        try {
            auto gil = GilScope{};
            if (!gil) {
                return;
            }
            Definition::destroy_state(invocation);
            gil.report_error();
        } catch (...) {
            report_unraisable(std::current_exception());
        }
    }
};

template <typename Definition> const hg::details::AggregateNodeDescriptor &aggregate_node_descriptor() {
    static const auto descriptor = [] {
        auto result = hg::details::aggregate_node_descriptor<Definition>();
        if constexpr (hg::details::aggregate_node_has_evaluate<Definition>) {
            result.evaluation_function = [](const CompiledNode &node, void *storage, EvaluationContext *context) {
                auto gil = GilScope{};
                const auto complete = hg::details::evaluate_aggregate_node<Definition>(node, storage, context);
                gil.check_error();
                return complete;
            };
        }
        if constexpr (hg::details::aggregate_node_has_start<Definition>) {
            result.start_function = [](const CompiledNode &node, void *storage, EvaluationContext *context) {
                auto gil = GilScope{};
                hg::details::start_aggregate_node<Definition>(node, storage, context);
                gil.check_error();
            };
        }
        if (result.stop_function != nullptr) {
            result.stop_function = [](const CompiledNode &node, void *storage, EvaluationContext *context) {
                try {
                    auto gil = GilScope{};
                    if (!gil) {
                        hg::details::unbind_dynamic_inputs<Definition>(node, storage, *context);
                        return;
                    }
                    hg::details::stop_aggregate_node<Definition>(node, storage, context);
                    gil.report_error();
                } catch (...) {
                    report_unraisable(std::current_exception());
                }
            };
        }
        if (result.invalidation_behavior.function != nullptr) {
            result.invalidation_behavior.function = [](const CompiledNode &node, void *storage,
                                                       EvaluationContext *context) {
                auto gil = GilScope{};
                if constexpr (hg::details::aggregate_node_has_invalidation_function<Definition>) {
                    hg::details::invalidate_aggregate_node<Definition>(node, storage, context);
                } else {
                    hg::details::invalidate_aggregate_output<Definition>(node, storage, context);
                }
                gil.check_error();
            };
        }
        if (result.state_destruction_function != nullptr) {
            result.state_destruction_function = [](const CompiledNode &node, void *storage,
                                                   EvaluationContext *context) noexcept {
                auto gil = GilScope{};
                if (!gil) {
                    hg::details::unbind_dynamic_inputs<Definition>(node, storage, *context);
                    hg::details::destroy_aggregate_state<Definition>(node, storage, context);
                    return;
                }
                hg::details::finalize_aggregate_node_state<Definition>(node, storage, context);
                gil.report_error();
            };
        }
        return result;
    }();
    return descriptor;
}

template <typename Definition, typename Label, typename Types, std::size_t... Index>
auto make_aggregate_node(const hg::details::AggregateNodeDescriptor &descriptor, Label &&label,
                         std::type_identity<Types>, std::index_sequence<Index...>) {
    return AggregateNodeTemplate<Definition, std::decay_t<Label>, std::tuple_element_t<Index, Types>...>{
        descriptor, std::forward<Label>(label)};
}

} // namespace hg::python::details

namespace hg::python {

template <typename... Declaration>
    requires(sizeof...(Declaration) != 0 && hg::details::aggregate_node_declaration<Declaration...>::valid)
[[nodiscard]] auto node() {
    using Parts = hg::details::aggregate_node_declaration<Declaration...>;
    return details::make_aggregate_node<typename Parts::Definition>(
        details::aggregate_node_descriptor<typename Parts::Definition>(), hg::details::default_wiring_label{},
        std::type_identity<typename Parts::Types>{}, std::make_index_sequence<sizeof...(Declaration) - 1>{});
}

template <typename... Declaration, typename Label>
    requires(sizeof...(Declaration) != 0 && hg::details::aggregate_node_declaration<Declaration...>::valid)
[[nodiscard]] auto node(Label &&label) {
    using Parts = hg::details::aggregate_node_declaration<Declaration...>;
    return details::make_aggregate_node<typename Parts::Definition>(
        details::aggregate_node_descriptor<typename Parts::Definition>(), std::forward<Label>(label),
        std::type_identity<typename Parts::Types>{}, std::make_index_sequence<sizeof...(Declaration) - 1>{});
}

template <typename... TypeParameters, hg::details::callable_node Function>
    requires(hg::details::type_parameter_declaration<TypeParameters> && ...)
[[nodiscard]] auto node(Function function) {
    using Definition = hg::details::CallableNodeDefinition<Function>;
    auto definition = Definition{std::move(function)};
    return hg::node<TypeParameters...>(details::NodeDefinitionAdapter<Definition>{std::move(definition)});
}

template <typename... TypeParameters, hg::details::callable_node Function, typename... Options>
    requires((hg::details::type_parameter_declaration<TypeParameters> && ...) &&
             hg::details::callable_node_options<Options...>)
[[nodiscard]] auto node(hg::details::CallableEvaluation<Function> evaluation, Options... options) {
    using Definition = hg::details::CallableNodeDefinition<Function, Options...>;
    auto definition = Definition{std::move(evaluation.function), std::move(options)...};
    return hg::node<TypeParameters...>(details::NodeDefinitionAdapter<Definition>{std::move(definition)});
}

template <typename... TypeParameters, hg::details::runtime_signature_node Definition>
    requires(hg::details::type_parameter_declaration<TypeParameters> && ...)
[[nodiscard]] auto node(Definition definition) {
    return hg::node<TypeParameters...>(details::NodeDefinitionAdapter<Definition>{std::move(definition)});
}

} // namespace hg::python
