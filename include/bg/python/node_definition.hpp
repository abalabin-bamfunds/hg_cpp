#pragma once

#include <bg/node/node_invocation.hpp>
#include <bg/node/node_template_identity.hpp>
#include <bg/python/export.hpp>
#include <bg/python/input_view.hpp>
#include <bg/python/python_node_injection.hpp>
#include <bg/python/object.hpp>
#include <bg/python/scalar_conversion.hpp>
#include <bg/type/signature.hpp>

#include <nanobind/nanobind.h>

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
    using state_type = PyObject *;

    enum class ParameterMode : std::uint8_t { scalar, input_view, time_series_value };
    enum class CallbackParameter : std::uint8_t { input, evaluation_clock, scheduler, state };

    struct ParameterConversion {
        ParameterMode mode;
    };

    struct CompiledData {
        Object function;
        Object state_factory;
        std::vector<ParameterConversion> parameters;
        std::vector<CallbackParameter> callback_parameters;
        bool has_output{};
        std::shared_ptr<CallbackLifetime> lifetime{std::make_shared<CallbackLifetime>()};

        nanobind::object argument(NodeInvocation<CompiledData, state_type> invocation, std::size_t index,
                                  CallbackScope &callback) const;
        nanobind::object call(NodeInvocation<CompiledData, state_type> invocation, CallbackScope &callback) const;
        void apply_delta(NodeInvocation<CompiledData, state_type> invocation, nanobind::handle delta) const;
        void invoke(NodeInvocation<CompiledData, state_type> invocation) const;
    };

    struct Identity {
        std::shared_ptr<const char> value;

        std::size_t hash() const noexcept { return std::hash<const void *>{}(value.get()); }
        bool operator==(const Identity &) const noexcept = default;
    };

    PythonNodeDefinition(std::shared_ptr<const SignatureDefinition> signature, nanobind::handle function,
                         bool schedule_on_start, bool pass_time_series_values,
                         std::vector<CallbackParameter> callback_parameters, nanobind::handle state_factory);

    const Signature &signature() const noexcept { return signature_->signature; }
    std::span<const Type *const> wiring_parameter_types() const noexcept { return signature_->wiring_parameter_types; }
    const void *identity() const noexcept { return identity_.get(); }
    PythonNodeDefinition retain_function() const;
    NodeTemplateIdentity definition_identity() const;
    CompiledData compile(const ResolvedSignature &signature, allocator &) const;
    bool schedule_on_start(const ResolvedSignature &signature) const noexcept;
    bool deduplicate_wiring() const noexcept;
    bool uses_timed_scheduling() const noexcept;

    static void evaluate(NodeInvocation<compiled_data_type, state_type> invocation);
    static void destroy_state(NodeInvocation<compiled_data_type, state_type> invocation) noexcept;

  private:
    PythonNodeDefinition(std::shared_ptr<const SignatureDefinition> signature, Object function,
                         std::shared_ptr<const char> identity, bool schedule_on_start,
                         bool pass_time_series_values, std::vector<CallbackParameter> callback_parameters,
                         Object state_factory) noexcept;

    std::shared_ptr<const SignatureDefinition> signature_;
    WeakObject weak_function_;
    Object retained_function_;
    Object state_factory_;
    std::shared_ptr<const char> identity_;
    bool schedule_on_start_;
    bool pass_time_series_values_;
    std::vector<CallbackParameter> callback_parameters_;
};

} // namespace bg::python
