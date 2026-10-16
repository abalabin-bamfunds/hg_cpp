#pragma once

#include <bg/node/node_invocation.hpp>
#include <bg/node/node_template.hpp>
#include <bg/node/output_declaration_traits.hpp>
#include <bg/graph/wiring_arguments.hpp>
#include <bg/graph/type_mapping.hpp>
#include <bg/util/byte_copy.hpp>

#include <concepts>
#include <memory>
#include <type_traits>

namespace bg::details {

template <typename Definition, typename = void> struct node_compiled_data {
    using type = void;
};

template <typename Definition>
struct node_compiled_data<Definition, std::void_t<typename Definition::compiled_data_type>> {
    using type = typename Definition::compiled_data_type;
};

template <typename Definition, typename = void> struct node_state {
    using type = void;
};

template <typename Definition> struct node_state<Definition, std::void_t<typename Definition::state_type>> {
    using type = typename Definition::state_type;
};

template <typename Definition> using node_compiled_data_t = typename node_compiled_data<Definition>::type;
template <typename Definition> using node_state_t = typename node_state<Definition>::type;

template <typename Definition>
concept runtime_signature_node = requires(const Definition &definition) {
    { definition.signature() } -> std::same_as<const Signature &>;
} && (requires(NodeInvocation<node_compiled_data_t<Definition>, node_state_t<Definition>> invocation) {
    { Definition::evaluate(invocation) } -> std::same_as<void>;
} || requires(NodeInvocation<node_compiled_data_t<Definition>, node_state_t<Definition>> invocation) {
    { Definition::evaluate(invocation) } -> std::same_as<bool>;
} || requires(NodeInvocation<node_compiled_data_t<Definition>, node_state_t<Definition>> invocation) {
    { Definition::start(invocation) } -> std::same_as<void>;
});

} // namespace bg::details

namespace bg {

template <details::runtime_signature_node Definition, typename... TypeParameters>
class RuntimeSignatureNodeTemplate final
    : public NodeTemplate,
      public details::TypeParameterizedWiring<RuntimeSignatureNodeTemplate<Definition, TypeParameters...>,
                                              TypeParameters...> {
    using CompiledData = details::node_compiled_data_t<Definition>;
    using State = details::node_state_t<Definition>;
    using Invocation = NodeInvocation<CompiledData, State>;

  public:
    explicit RuntimeSignatureNodeTemplate(Definition definition) : definition_{std::move(definition)} {
        if constexpr (!std::is_void_v<CompiledData>) {
            static_assert(std::is_nothrow_destructible_v<CompiledData>);
        }
        if constexpr (!std::is_void_v<State>) {
            static_assert(util::byte_copy_initializable<State>);
        }
    }

    template <typename... Arguments>
        requires(requires { typename Definition::output_time_series_type; } &&
                 !std::same_as<typename Definition::output_time_series_type, void> &&
                 (details::graph_code_argument<Arguments> && ...))
    [[nodiscard]] auto operator()(Arguments &&...arguments) const & {
        using Output = typename Definition::output_time_series_type;
        return wp<Output>{*NodeTemplate::operator()(std::forward<Arguments>(arguments)...)};
    }

    template <typename... Arguments>
        requires(requires { typename Definition::output_time_series_type; } &&
                 !std::same_as<typename Definition::output_time_series_type, void> &&
                 (details::graph_code_argument<Arguments> && ...))
    [[nodiscard]] auto operator()(const TypeMap &type_map, Arguments &&...arguments) const & {
        using Output = typename Definition::output_time_series_type;
        return wp<Output>{*NodeTemplate::operator()(type_map, std::forward<Arguments>(arguments)...)};
    }

    template <typename... Arguments>
        requires(requires { typename Definition::output_time_series_type; } &&
                 std::same_as<typename Definition::output_time_series_type, void> &&
                 (details::graph_code_argument<Arguments> && ...))
    void operator()(Arguments &&...arguments) const & {
        (void)NodeTemplate::operator()(std::forward<Arguments>(arguments)...);
    }

    template <typename... Arguments>
        requires(requires { typename Definition::output_time_series_type; } &&
                 std::same_as<typename Definition::output_time_series_type, void> &&
                 (details::graph_code_argument<Arguments> && ...))
    void operator()(const TypeMap &type_map, Arguments &&...arguments) const & {
        (void)NodeTemplate::operator()(type_map, std::forward<Arguments>(arguments)...);
    }

    template <typename... Arguments>
        requires(!requires { typename Definition::output_time_series_type; } &&
                 (details::graph_code_argument<Arguments> && ...))
    [[nodiscard]] std::optional<WiringPoint> operator()(Arguments &&...arguments) const & {
        return NodeTemplate::operator()(std::forward<Arguments>(arguments)...);
    }

    template <typename... Arguments>
        requires(!requires { typename Definition::output_time_series_type; } &&
                 (details::graph_code_argument<Arguments> && ...))
    [[nodiscard]] std::optional<WiringPoint> operator()(const TypeMap &type_map, Arguments &&...arguments) const & {
        return NodeTemplate::operator()(type_map, std::forward<Arguments>(arguments)...);
    }

    [[nodiscard]] std::optional<WiringPoint> operator()(std::span<const WiringPoint> arguments) const & {
        return NodeTemplate::operator()(arguments);
    }

    [[nodiscard]] std::optional<WiringPoint> operator()(const TypeMap &type_map,
                                                         std::span<const WiringPoint> arguments) const & {
        return NodeTemplate::operator()(type_map, arguments);
    }

    template <typename... Arguments>
        requires(details::graph_code_argument<Arguments> && ...)
    [[nodiscard]] std::optional<WiringPoint> operator()(Arguments &&...arguments) && {
        auto node_template = std::make_unique<RuntimeSignatureNodeTemplate>(std::move(*this));
        const auto &owned = WiringContext::current().wiring().own_node_template(std::move(node_template));
        return owned(std::forward<Arguments>(arguments)...);
    }

    template <typename... Arguments>
        requires(details::graph_code_argument<Arguments> && ...)
    [[nodiscard]] std::optional<WiringPoint> operator()(const TypeMap &type_map, Arguments &&...arguments) && {
        auto node_template = std::make_unique<RuntimeSignatureNodeTemplate>(std::move(*this));
        const auto &owned = WiringContext::current().wiring().own_node_template(std::move(node_template));
        return owned(type_map, std::forward<Arguments>(arguments)...);
    }

    const Signature &signature() const override { return definition_.signature(); }

    NodeTemplateIdentity definition_identity() const override {
        if constexpr (requires { { definition_.definition_identity() } -> std::same_as<NodeTemplateIdentity>; }) {
            return definition_.definition_identity();
        } else {
            return NodeTemplate::definition_identity();
        }
    }

    bool deduplicate_wiring() const noexcept override {
        if constexpr (requires { definition_.deduplicate_wiring(); }) {
            return definition_.deduplicate_wiring();
        } else if constexpr (requires { Definition::deduplicate_wiring; }) {
            return static_cast<bool>(Definition::deduplicate_wiring);
        }
        return true;
    }

    bool may_pend_evaluation(const ResolvedSignature &) const noexcept override {
        if constexpr (requires(Invocation invocation) {
                          { Definition::evaluate(invocation) } -> std::same_as<bool>;
                      }) {
            return true;
        }
        return false;
    }

    bool schedule_on_start(const ResolvedSignature &signature) const noexcept override {
        if constexpr (requires { definition_.schedule_on_start(signature); }) {
            return definition_.schedule_on_start(signature);
        } else if constexpr (requires { definition_.schedule_on_start(); }) {
            return definition_.schedule_on_start();
        } else if constexpr (requires { Definition::schedule_on_start; }) {
            return static_cast<bool>(Definition::schedule_on_start);
        }
        return false;
    }

    RequirementResult requirement(const WiringArguments &arguments) const override {
        if constexpr (requires { requirement_result(definition_.requirement(arguments)); }) {
            return requirement_result(definition_.requirement(arguments));
        }

        return accept;
    }

    EvaluationFunction evaluation_function(const ResolvedSignature &) const noexcept override {
        if constexpr (requires(Invocation invocation) { Definition::evaluate(invocation); }) {
            return &evaluate;
        }
        return nullptr;
    }

    NodeFunction start_function(const ResolvedSignature &) const noexcept override {
        if constexpr (requires(Invocation invocation) { Definition::start(invocation); }) {
            return &start;
        }
        return nullptr;
    }

    NodeFunction stop_function(const ResolvedSignature &) const noexcept override {
        if constexpr (requires(Invocation invocation) { Definition::stop(invocation); }) {
            static_assert(noexcept(Definition::stop(std::declval<Invocation>())));
            return &stop;
        }
        return nullptr;
    }

    InvalidationBehavior invalidation_behavior(const ResolvedSignature &) const noexcept override {
        if constexpr (requires(Invocation invocation) { Definition::on_invalidation(invocation); }) {
            return {InvalidationMode::invoke, &invalidate};
        }
        return {};
    }

    bool uses_time_series_input_runtime_type(const ResolvedSignature &signature,
                                             std::size_t parameter_index) const noexcept override {
        if constexpr (requires { definition_.uses_time_series_input_runtime_type(signature, parameter_index); }) {
            return definition_.uses_time_series_input_runtime_type(signature, parameter_index);
        }
        return signature.parameters[parameter_index].type->is_time_series();
    }

    bool uses_scalar_input_runtime_type(const ResolvedSignature &signature,
                                        std::size_t parameter_index) const noexcept override {
        if constexpr (requires { definition_.uses_scalar_input_runtime_type(signature, parameter_index); }) {
            return definition_.uses_scalar_input_runtime_type(signature, parameter_index);
        }
        return signature.parameters[parameter_index].type->is_scalar();
    }

    bool uses_output_runtime_type() const noexcept override {
        if (!definition_.signature().has_output()) {
            return false;
        }
        if constexpr (requires(const Definition &definition, const WiringArguments &arguments) {
                          { definition.output_runtime_type(arguments) } ->
                              std::same_as<const TimeSeriesRuntimeType &>;
                      }) {
            return true;
        }
        if constexpr (requires { definition_.uses_output_runtime_type(); }) {
            return definition_.uses_output_runtime_type();
        }
        return definition_.signature().has_output();
    }

    const TimeSeriesRuntimeType *output_runtime_type(const WiringArguments &arguments) const override {
        if (arguments.resolved_output_type() == nullptr) {
            return nullptr;
        }
        if constexpr (requires(const Definition &definition, const WiringArguments &wiring_arguments) {
                          { definition.output_runtime_type(wiring_arguments) } ->
                              std::same_as<const TimeSeriesRuntimeType &>;
                      }) {
            return std::addressof(definition_.output_runtime_type(arguments));
        } else if constexpr (requires { typename Definition::output_declaration_type; }) {
            using Output = typename Definition::output_declaration_type;
            if constexpr (!std::same_as<Output, void>) {
                return std::addressof(
                    output_declaration_traits<Output>::runtime(*arguments.resolved_output_type()));
            }
        }
        return nullptr;
    }

    bool uses_node_invocation_parameters() const noexcept override {
        if constexpr (requires { definition_.uses_node_invocation_parameters(); }) {
            return definition_.uses_node_invocation_parameters();
        }
        return true;
    }

    bool uses_input_node_indices() const noexcept override {
        if constexpr (requires { Definition::uses_input_node_indices; }) {
            return Definition::uses_input_node_indices;
        }
        return false;
    }

    bool uses_timed_scheduling() const noexcept override {
        if constexpr (requires { { definition_.uses_timed_scheduling() } -> std::convertible_to<bool>; }) {
            return static_cast<bool>(definition_.uses_timed_scheduling());
        } else if constexpr (requires { Definition::uses_timed_scheduling; }) {
            return Definition::uses_timed_scheduling;
        }
        return false;
    }

    bool uses_cycle_callbacks() const noexcept override {
        if constexpr (requires { Definition::uses_cycle_callbacks; }) {
            return Definition::uses_cycle_callbacks;
        }
        return false;
    }

    NodeStateLayout state_layout(const ResolvedSignature &, std::span<const ScalarRuntimeType *const>,
                                 std::span<const ScalarType *const>) const override {
        if constexpr (std::is_void_v<State>) {
            return {};
        } else {
            return {
                .size = sizeof(State),
                .alignment = alignof(State),
                .field_offsets = {},
                .generic_runtime_types = {},
            };
        }
    }

    void construct_initial_state(const NodeStateLayout &, void *storage) const noexcept override {
        if constexpr (!std::is_void_v<State>) {
            std::construct_at(static_cast<State *>(storage));
        }
    }

    NodeFunction state_destruction_function(const ResolvedSignature &) const noexcept override {
        if constexpr (requires(Invocation invocation) { Definition::destroy_state(invocation); }) {
            static_assert(noexcept(Definition::destroy_state(std::declval<Invocation>())));
            return &destroy_state;
        }
        return nullptr;
    }

    CompiledNodeDataLayout compiled_data_layout(const ResolvedSignature &) const noexcept override {
        if constexpr (std::is_void_v<CompiledData>) {
            return {};
        } else {
            auto result = CompiledNodeDataLayout{
                .size = sizeof(CompiledData),
                .alignment = alignof(CompiledData),
            };
            if constexpr (!std::is_trivially_destructible_v<CompiledData>) {
                result.destruction_function = [](void *data) noexcept {
                    std::destroy_at(static_cast<CompiledData *>(data));
                };
            }
            return result;
        }
    }

    void construct_compiled_data(const ResolvedSignature &signature, void *storage, allocator &memory) const override {
        if constexpr (!std::is_void_v<CompiledData>) {
            std::construct_at(static_cast<CompiledData *>(storage), definition_.compile(signature, memory));
        }
    }

  private:
    static Invocation invocation(const CompiledNode &node, void *runtime_storage, EvaluationContext *context) noexcept {
        return Invocation{node, runtime_storage, *context};
    }

    static bool evaluate(const CompiledNode &node, void *runtime_storage, EvaluationContext *context) {
        if constexpr (std::same_as<decltype(Definition::evaluate(invocation(node, runtime_storage, context))), bool>) {
            return Definition::evaluate(invocation(node, runtime_storage, context));
        } else {
            Definition::evaluate(invocation(node, runtime_storage, context));
            return true;
        }
    }

    static void start(const CompiledNode &node, void *runtime_storage, EvaluationContext *context) {
        Definition::start(invocation(node, runtime_storage, context));
    }

    static void stop(const CompiledNode &node, void *runtime_storage, EvaluationContext *context) noexcept {
        Definition::stop(invocation(node, runtime_storage, context));
    }

    static void invalidate(const CompiledNode &node, void *runtime_storage, EvaluationContext *context) {
        Definition::on_invalidation(invocation(node, runtime_storage, context));
    }

    static void destroy_state(const CompiledNode &node, void *runtime_storage, EvaluationContext *context) noexcept {
        Definition::destroy_state(invocation(node, runtime_storage, context));
    }

    Definition definition_;
};

} // namespace bg
