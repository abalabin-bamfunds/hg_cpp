#pragma once

#include <bg/evaluation_time.hpp>
#include <bg/node/node_invocation.hpp>
#include <bg/node/node_template_identity.hpp>
#include <bg/queue_mode.hpp>
#include <bg/python/export.hpp>
#include <bg/python/input_view.hpp>
#include <bg/python/python_node_injection.hpp>
#include <bg/python/object.hpp>
#include <bg/python/scalar_conversion.hpp>
#include <bg/type/signature.hpp>

#include <nanobind/nanobind.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <variant>
#include <vector>

namespace bg::python {

class BG_PYTHON_API PythonNodeDefinition {
  public:
    struct SignatureDefinition {
        SignatureDefinition(std::vector<const Type *> wiring_parameter_types, Signature signature)
            : wiring_parameter_types{std::move(wiring_parameter_types)}, signature{std::move(signature)} {}

        SignatureDefinition(const SignatureDefinition &) = delete;
        SignatureDefinition &operator=(const SignatureDefinition &) = delete;

        std::vector<const Type *> wiring_parameter_types;
        Signature signature;
    };

    struct CompiledData;
    using compiled_data_type = CompiledData;
    struct RuntimeState {
        PyObject *user_state{};
        PyObject *generator{};
        PyObject *pending_value{};
        std::shared_ptr<GeneratorClockLease> generator_clock{std::make_shared<GeneratorClockLease>()};
        EvaluationTime last_generator_time{MIN_DT};
        bool has_generator_time{};
    };
    using state_type = void *;

    enum class ParameterMode : std::uint8_t { scalar, input_view, input_view_valid_items, time_series_value };
    enum class CallbackParameter : std::uint8_t { input, output, evaluation_clock, scheduler, state, queue };

    struct CallbackArgument {
        CallbackParameter parameter;
        std::uint32_t index{};
    };

    using CallbackArguments = std::vector<CallbackArgument>;

    struct CallbackDefinition {
        nanobind::handle function{};
        CallbackArguments arguments;
    };

    struct ParameterConversion {
        ParameterMode mode;
    };

    struct CompiledData {
        Object function;
        Object start_function;
        Object stop_function;
        Object invalidation_function;
        Object state_factory;
        std::vector<ParameterConversion> parameters;
        CallbackArguments evaluation_arguments;
        CallbackArguments start_arguments;
        CallbackArguments stop_arguments;
        CallbackArguments invalidation_arguments;
        bool has_output{};
        bool is_generator{};
        std::shared_ptr<CallbackLifetime> lifetime{std::make_shared<CallbackLifetime>()};

        nanobind::object argument(NodeInvocation<CompiledData, state_type> invocation, std::size_t index,
                                  CallbackScope &callback, bool retain_scalar) const;
        void apply_delta(NodeInvocation<CompiledData, state_type> invocation, nanobind::handle delta) const;
        nanobind::object call(NodeInvocation<CompiledData, state_type> invocation, CallbackScope &callback,
                              const Object &callback_function, const CallbackArguments &arguments,
                              bool generator_call = false) const;
        RuntimeState &runtime_state(NodeInvocation<CompiledData, state_type> invocation) const;
        void start(NodeInvocation<CompiledData, state_type> invocation) const;
        void evaluate(NodeInvocation<CompiledData, state_type> invocation) const;
        void stop(NodeInvocation<CompiledData, state_type> invocation) const noexcept;
        void on_invalidation(NodeInvocation<CompiledData, state_type> invocation) const;
        void advance_generator(NodeInvocation<CompiledData, state_type> invocation) const;
    };

    struct Identity {
        std::shared_ptr<const char> value;

        std::size_t hash() const noexcept { return std::hash<const void *>{}(value.get()); }
        bool operator==(const Identity &) const noexcept = default;
    };

    PythonNodeDefinition(std::shared_ptr<const SignatureDefinition> signature, bool schedule_on_start,
                         bool pass_time_series_values, CallbackDefinition evaluation, CallbackDefinition start,
                         CallbackDefinition stop,
                         CallbackDefinition invalidation, nanobind::handle state_factory, bool generator,
                         QueueMode queue_mode);

    const Signature &signature() const noexcept { return signature_->signature; }
    std::span<const Type *const> wiring_parameter_types() const noexcept { return signature_->wiring_parameter_types; }
    const void *identity() const noexcept { return identity_.get(); }
    PythonNodeDefinition retain_function() const;
    NodeTemplateIdentity definition_identity() const;
    CompiledData compile(const ResolvedSignature &signature, allocator &) const;
    bool schedule_on_start(const ResolvedSignature &signature) const noexcept;
    bool deduplicate_wiring() const noexcept;
    bool uses_timed_scheduling() const noexcept;
    bool uses_queue() const noexcept;
    QueueMode queue_mode() const noexcept { return queue_mode_; }
    bool has_evaluation(const ResolvedSignature &) const noexcept;
    bool has_start(const ResolvedSignature &) const noexcept;
    bool has_stop(const ResolvedSignature &) const noexcept;
    bool has_on_invalidation(const ResolvedSignature &) const noexcept;
    bool is_generator() const noexcept { return generator_; }

    static void evaluate(NodeInvocation<compiled_data_type, state_type> invocation);
    static void start(NodeInvocation<compiled_data_type, state_type> invocation);
    static void stop(NodeInvocation<compiled_data_type, state_type> invocation) noexcept;
    static void on_invalidation(NodeInvocation<compiled_data_type, state_type> invocation);
    static void destroy_state(NodeInvocation<compiled_data_type, state_type> invocation) noexcept;

  private:
    PythonNodeDefinition(std::shared_ptr<const SignatureDefinition> signature, std::shared_ptr<const char> identity,
                         bool schedule_on_start, Object function, CallbackArguments evaluation_arguments,
                         bool pass_time_series_values, Object start_function, CallbackArguments start_arguments,
                         Object stop_function, CallbackArguments stop_arguments, Object invalidation_function,
                         CallbackArguments invalidation_arguments, Object state_factory, bool generator,
                         QueueMode queue_mode) noexcept;

    static std::optional<WeakObject> weak_callback(const CallbackDefinition &callback);
    static Object retain_callback(const std::optional<WeakObject> &callback);

    std::shared_ptr<const SignatureDefinition> signature_;
    std::optional<WeakObject> weak_function_;
    Object retained_function_;
    CallbackArguments evaluation_arguments_;
    std::optional<WeakObject> weak_start_function_;
    Object retained_start_function_;
    CallbackArguments start_arguments_;
    std::optional<WeakObject> weak_stop_function_;
    Object retained_stop_function_;
    CallbackArguments stop_arguments_;
    std::optional<WeakObject> weak_invalidation_function_;
    Object retained_invalidation_function_;
    CallbackArguments invalidation_arguments_;
    Object state_factory_;
    std::shared_ptr<const char> identity_;
    bool schedule_on_start_;
    bool pass_time_series_values_;
    bool generator_;
    QueueMode queue_mode_{QueueMode::standard};
};

} // namespace bg::python
