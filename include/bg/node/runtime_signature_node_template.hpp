#pragma once

#include <bg/node/node_invocation.hpp>
#include <bg/node/node_template.hpp>

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
    Definition::evaluate(invocation);
} || requires(NodeInvocation<node_compiled_data_t<Definition>, node_state_t<Definition>> invocation) {
    Definition::start(invocation);
});

} // namespace bg::details

namespace bg {

template <details::runtime_signature_node Definition> class RuntimeSignatureNodeTemplate final : public NodeTemplate {
    using CompiledData = details::node_compiled_data_t<Definition>;
    using State = details::node_state_t<Definition>;
    using Invocation = NodeInvocation<CompiledData, State>;

  public:
    using NodeTemplate::operator();

    explicit RuntimeSignatureNodeTemplate(Definition definition) : definition_{std::move(definition)} {
        if constexpr (!std::is_void_v<CompiledData>) {
            static_assert(std::is_nothrow_destructible_v<CompiledData>);
        }
        if constexpr (!std::is_void_v<State>) {
            static_assert(std::is_trivially_copyable_v<State>);
            static_assert(std::is_trivially_destructible_v<State>);
            static_assert(std::is_nothrow_default_constructible_v<State>);
        }
    }

    template <typename... Arguments>
        requires(details::graph_code_argument<Arguments> && ...)
    [[nodiscard]] std::optional<WiringPoint> operator()(Arguments &&...arguments) && {
        auto node_template = std::make_unique<RuntimeSignatureNodeTemplate>(std::move(*this));
        const auto &owned = WiringContext::current().wiring().own_node_template(std::move(node_template));
        return owned(std::forward<Arguments>(arguments)...);
    }

    [[nodiscard]] const Signature &signature() const override { return definition_.signature(); }

    [[nodiscard]] EvaluationFunction evaluation_function(const ResolvedSignature &) const noexcept override {
        if constexpr (requires(Invocation invocation) { Definition::evaluate(invocation); }) {
            return &evaluate;
        }
        return nullptr;
    }

    [[nodiscard]] EvaluationFunction start_function(const ResolvedSignature &) const noexcept override {
        if constexpr (requires(Invocation invocation) { Definition::start(invocation); }) {
            return &start;
        }
        return nullptr;
    }

    [[nodiscard]] EvaluationFunction stop_function(const ResolvedSignature &) const noexcept override {
        if constexpr (requires(Invocation invocation) { Definition::stop(invocation); }) {
            static_assert(noexcept(Definition::stop(std::declval<Invocation>())));
            return &stop;
        }
        return nullptr;
    }

    [[nodiscard]] InvalidationBehavior invalidation_behavior(const ResolvedSignature &) const noexcept override {
        if constexpr (requires(Invocation invocation) { Definition::on_invalidation(invocation); }) {
            return {InvalidationMode::invoke, &invalidate};
        }
        return {};
    }

    [[nodiscard]] bool uses_generic_time_series_input_view(std::size_t parameter_index) const noexcept override {
        return signature_parameter(parameter_index).type->is_time_series();
    }

    [[nodiscard]] bool uses_generic_scalar_input_view(std::size_t parameter_index) const noexcept override {
        return signature_parameter(parameter_index).type->is_scalar();
    }

    [[nodiscard]] bool uses_generic_output_view() const noexcept override {
        return definition_.signature().has_output();
    }
    [[nodiscard]] bool uses_node_invocation() const noexcept override { return true; }

    [[nodiscard]] bool uses_future_scheduling() const noexcept override {
        if constexpr (requires { Definition::uses_future_scheduling; }) {
            return Definition::uses_future_scheduling;
        }
        return false;
    }

    [[nodiscard]] bool uses_cycle_callbacks() const noexcept override {
        if constexpr (requires { Definition::uses_cycle_callbacks; }) {
            return Definition::uses_cycle_callbacks;
        }
        return false;
    }

    [[nodiscard]] const TimeSeriesStorageRepresentation *output_storage_representation(
        const ResolvedSignature &signature,
        std::span<const ScalarStorageRepresentation *const> parameter_storage_representations,
        std::span<const TimeSeriesStorageRepresentation *const> input_storage_representations) const noexcept override {
        return definition_.output_storage_representation(signature, parameter_storage_representations,
                                                         input_storage_representations);
    }

    [[nodiscard]] NodeStateLayout state_layout(const ResolvedSignature &,
                                               std::span<const ScalarStorageRepresentation *const>,
                                               std::span<const ScalarType *const>) const override {
        if constexpr (std::is_void_v<State>) {
            return {};
        } else {
            return {
                .size = sizeof(State),
                .alignment = alignof(State),
                .field_offsets = {},
                .generic_storage_representations = {},
            };
        }
    }

    void construct_initial_state(const NodeStateLayout &, void *storage) const noexcept override {
        if constexpr (!std::is_void_v<State>) {
            std::construct_at(static_cast<State *>(storage));
        }
    }

    [[nodiscard]] EvaluationFunction state_destruction_function(const ResolvedSignature &) const noexcept override {
        if constexpr (requires(Invocation invocation) { Definition::destroy_state(invocation); }) {
            static_assert(noexcept(Definition::destroy_state(std::declval<Invocation>())));
            return &destroy_state;
        }
        return nullptr;
    }

    [[nodiscard]] CompiledNodeDataLayout compiled_data_layout(const ResolvedSignature &) const noexcept override {
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

    void construct_compiled_data(const ResolvedSignature &signature, void *storage,
                                 std::pmr::memory_resource &memory) const override {
        if constexpr (!std::is_void_v<CompiledData>) {
            std::construct_at(static_cast<CompiledData *>(storage), definition_.compile(signature, memory));
        }
    }

  private:
    [[nodiscard]] const Parameter &signature_parameter(std::size_t index) const noexcept {
        return definition_.signature().parameter(index);
    }

    static Invocation invocation(const CompiledNode &node, void *runtime_storage, EvaluationContext *context) noexcept {
        return Invocation{node, runtime_storage, *context};
    }

    static void evaluate(const CompiledNode &node, void *runtime_storage, EvaluationContext *context) {
        Definition::evaluate(invocation(node, runtime_storage, context));
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
