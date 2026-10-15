#include <bg/python/graph_definition.hpp>

#include <bg/operators.hpp>
#include <bg/python/scalar_conversion.hpp>
#include <bg/python/wiring_point.hpp>
#include <bg/graph/wiring_context.hpp>

#include "wiring_argument.hpp"

#include <array>
#include <memory_resource>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

namespace bg::python {
namespace {

thread_local std::shared_ptr<PythonWiringLifetime> active_wiring_lifetime;

} // namespace

PythonWiringScope::PythonWiringScope(const GraphWiring &wiring) {
    if (active_wiring_lifetime != nullptr && active_wiring_lifetime->is_active_for(wiring)) {
        lifetime_ = active_wiring_lifetime;
        return;
    }

    previous_lifetime_ = active_wiring_lifetime;
    lifetime_ = std::make_shared<PythonWiringLifetime>(wiring);
    active_wiring_lifetime = lifetime_;
    owner_ = true;
}

PythonWiringScope::~PythonWiringScope() {
    if (!owner_) {
        return;
    }

    lifetime_->end();
    active_wiring_lifetime = std::move(previous_lifetime_);
}

std::shared_ptr<PythonWiringLifetime> PythonWiringScope::current() {
    if (active_wiring_lifetime == nullptr) {
        throw std::logic_error{"Python wiring points can only be created during graph construction"};
    }
    return active_wiring_lifetime;
}

PythonGraphDefinition::PythonGraphDefinition(
    std::shared_ptr<const PythonNodeDefinition::SignatureDefinition> signature, nanobind::handle function,
    bool lift_scalar_output)
    : signature_{std::move(signature)}, function_{function}, lift_scalar_output_{lift_scalar_output} {
    if (signature_ == nullptr) {
        throw std::invalid_argument{"a Python graph requires a signature"};
    }
    if (!PyCallable_Check(function.ptr())) {
        throw std::invalid_argument{"a Python graph definition requires a callable"};
    }
}

PythonGraphDefinition PythonGraphDefinition::specialize(std::span<const Type *const> input_types) const {
    auto resolution = signature_->signature.resolve(input_types);
    auto parameters = std::vector<Parameter>{};
    parameters.reserve(signature_->signature.parameter_count());
    auto wiring_parameter_types = std::vector<const Type *>{};
    wiring_parameter_types.reserve(signature_->signature.parameter_count());

    for (auto index = std::size_t{}; index < signature_->signature.parameter_count(); ++index) {
        auto parameter = signature_->signature.parameter(index);
        parameter.type = resolution.parameters[index].type;
        wiring_parameter_types.push_back(parameter.type);
        parameters.push_back(std::move(parameter));
    }

    auto specialized_signature = resolution.output == nullptr
                                     ? Signature{resolution.name, std::move(parameters)}
                                     : Signature{resolution.name, std::move(parameters), *resolution.output};
    auto definition = std::make_shared<PythonNodeDefinition::SignatureDefinition>(
        std::move(wiring_parameter_types), std::move(specialized_signature));
    auto function = function_.lock();
    return {std::move(definition), function.get(), lift_scalar_output_};
}

std::optional<WiringPoint> PythonGraphDefinition::wire(GraphWiring &wiring, ResolvedSignature resolved_signature,
                                                       const WiringArguments &,
                                                       std::span<const WiringArgument> arguments) const {
    if (&WiringContext::current().wiring() != &wiring) {
        throw std::logic_error{"a Python graph must wire in its active graph context"};
    }

    auto scope = PythonWiringScope{wiring};
    auto gil = nanobind::gil_scoped_acquire{};
    auto python_arguments = std::vector<nanobind::object>{};
    python_arguments.reserve(arguments.size());
    for (auto index = std::size_t{}; index < arguments.size(); ++index) {
        const auto &argument = arguments[index];
        if (argument.is_time_series()) {
            const auto optional = argument.requires_optional() ||
                                  resolved_signature.parameters[index].validity == InputValidity::optional;
            python_arguments.push_back(
                nanobind::cast(PythonWiringPoint{argument.time_series(), scope.lifetime(), optional}));
            continue;
        }
        if (argument.is_scalar()) {
            if (!argument.present()) {
                python_arguments.emplace_back(nanobind::none());
            } else {
                python_arguments.push_back(scalar_conversions().to_python(argument.scalar()));
            }
            continue;
        }

        throw std::invalid_argument{"Python graph parameters do not support graph arguments yet"};
    }

    auto tuple = nanobind::tuple_builder{python_arguments.size()};
    for (auto &argument : python_arguments) {
        tuple.put(argument);
    }
    auto call_arguments = tuple.commit();
    const auto function = function_.lock();
    auto *result = PyObject_CallObject(function.get().ptr(), call_arguments.ptr());
    if (result == nullptr) {
        nanobind::raise_python_error();
    }
    auto returned = nanobind::steal<nanobind::object>(result);

    const auto *expected_output = signature_->signature.output();
    if (returned.is_none()) {
        if (expected_output != nullptr) {
            throw std::invalid_argument{"a Python graph with an output annotation returned None"};
        }
        return std::nullopt;
    }

    if (expected_output == nullptr) {
        throw std::invalid_argument{"a Python sink graph must return None"};
    }
    if (nanobind::isinstance<PythonWiringPoint>(returned)) {
        return nanobind::cast<PythonWiringPoint>(returned).get(wiring);
    }

    if (!lift_scalar_output_) {
        throw std::invalid_argument{"a Python graph with a time-series return annotation must return a wiring point"};
    }

    const auto *output_type = type_cast<TimeSeriesValueType>(expected_output);
    if (output_type == nullptr) {
        throw std::logic_error{"a scalar-returning Python graph requires a time-series value output"};
    }

    auto temporary_memory = std::pmr::unsynchronized_pool_resource{};
    auto memory = allocator{temporary_memory};
    auto scalar_argument = details::wiring_argument(output_type->value_type(), returned, memory, wiring);
    const auto scalar_arguments = std::array{std::move(scalar_argument)};
    const auto point = static_cast<const WiringTemplate &>(operators::const_)(
        std::span<const WiringArgument>{scalar_arguments});
    if (!point) {
        throw WiringError{"a Python graph returned a scalar that cannot be lifted to its declared time series"};
    }

    return *point;
}

} // namespace bg::python
