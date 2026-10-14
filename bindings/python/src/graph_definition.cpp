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
