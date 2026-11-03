#include <hg/python/graph_definition.hpp>

#include <hg/operators.hpp>
#include <hg/python/scalar_conversion.hpp>
#include <hg/python/wiring_point.hpp>
#include <hg/graph/wiring_context.hpp>
#include <hg/graph/context.hpp>
#include <hg/util/scope_exit.hpp>

#include "python_context.hpp"
#include "wiring_argument.hpp"

#include <array>
#include <memory_resource>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace hg::python {
namespace nb = nanobind;

namespace {

thread_local std::shared_ptr<PythonWiringLifetime> active_wiring_lifetime;

} // namespace

PythonWiringScope::PythonWiringScope(GraphWiring &wiring) {
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

void PythonWiringLifetime::enter_graph(nanobind::handle function, const WiringPath *path) {
    auto code = nb::getattr(function, "__code__", nb::none());
    graph_calls_.push_back({Object{code}, path, {}});
}

void PythonWiringLifetime::leave_graph() noexcept { graph_calls_.pop_back(); }

void PythonWiringLifetime::observe() {
    if (graph_calls_.empty()) {
        return;
    }

    auto &graph_call = graph_calls_.back();
    if (graph_call.frame) {
        return;
    }
    if (!graph_call.code || graph_call.code.get().ptr() == Py_None) {
        return;
    }

    auto *current_frame = PyEval_GetFrame();
    if (current_frame == nullptr) {
        return;
    }

    auto frame = nb::borrow<nb::object>(nb::handle{reinterpret_cast<PyObject *>(current_frame)});
    while (frame && frame.ptr() != Py_None) {
        auto code = nb::getattr(frame, "f_code", nb::none());
        if (code.ptr() == graph_call.code.get().ptr()) {
            graph_call.frame = Object{frame};
            return;
        }

        frame = nb::getattr(frame, "f_back", nb::none());
    }
}

void PythonWiringLifetime::label_graph() {
    auto &graph_call = graph_calls_.back();
    if (!graph_call.frame) {
        return;
    }

    auto locals = nb::steal<nb::object>(PyObject_GetAttrString(graph_call.frame.get().ptr(), "f_locals"));
    if (!locals) {
        nb::raise_python_error();
    }
    auto items = nb::steal<nb::object>(PyMapping_Items(locals.ptr()));
    if (!items) {
        nb::raise_python_error();
    }

    const auto count = PySequence_Size(items.ptr());
    if (count < 0) {
        nb::raise_python_error();
    }
    for (auto index = Py_ssize_t{}; index < count; ++index) {
        auto *item = PySequence_GetItem(items.ptr(), index);
        if (item == nullptr) {
            nb::raise_python_error();
        }
        auto entry = nb::steal<nb::object>(item);
        const auto key = PyTuple_GetItem(entry.ptr(), 0);
        const auto value = PyTuple_GetItem(entry.ptr(), 1);
        if (key == nullptr || value == nullptr || !PyUnicode_Check(key) ||
            !nb::isinstance<PythonWiringPoint>(value)) {
            continue;
        }

        const auto point = nb::cast<const PythonWiringPoint &>(nb::handle{value}).get(*wiring_);
        const auto identity = wiring_->node_call_identity(point);
        if (!identity) {
            continue;
        }

        auto name_size = Py_ssize_t{};
        const auto *name = PyUnicode_AsUTF8AndSize(key, &name_size);
        if (name == nullptr) {
            nb::raise_python_error();
        }
        wiring_->set_default_node_label(
            *identity, graph_call.path, util::symbol{std::string_view{name, static_cast<std::size_t>(name_size)}});
    }
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

    auto gil = nanobind::gil_scoped_acquire{};
    details::activate_contexts();
    auto context_scopes = hg::details::ContextScopes::activate_context_parameters(resolved_signature, arguments);
    auto scope = PythonWiringScope{wiring};
    auto python_arguments = std::vector<nanobind::object>{};
    python_arguments.reserve(arguments.size());
    for (auto index = std::size_t{}; index < arguments.size(); ++index) {
        const auto &argument = arguments[index];
        if (argument.is_time_series()) {
            const auto optional = argument.requires_optional() ||
                                  resolved_signature.parameters[index].validity == InputValidity::optional;
            const auto valid_items = argument.annotations().valid_items ||
                                     resolved_signature.parameters[index].valid_items;
            python_arguments.push_back(nanobind::cast(
                PythonWiringPoint{argument.time_series(), scope.lifetime(), optional, valid_items}));
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
    scope.lifetime()->enter_graph(function.get(), WiringContext::current().path());
    auto leave_graph = util::scope_exit{[&]() noexcept { scope.lifetime()->leave_graph(); }};
    auto *result = PyObject_CallObject(function.get().ptr(), call_arguments.ptr());
    if (result == nullptr) {
        nanobind::raise_python_error();
    }
    auto returned = nanobind::steal<nanobind::object>(result);
    scope.lifetime()->label_graph();

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

} // namespace hg::python
