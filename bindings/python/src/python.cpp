#include <bg/operators.hpp>
#include <bg/node/runtime_signature_node_template.hpp>
#include <bg/python/graph_definition.hpp>
#include <bg/python/module.hpp>
#include <bg/python/node_definition.hpp>
#include <bg/python/object.hpp>
#include <bg/python/scalar_conversion.hpp>
#include <bg/python/wiring_point.hpp>
#include <bg/scalar/runtime_type.hpp>
#include <bg/type/type_resolution_error.hpp>
#include <bg/testing/test_graph.hpp>
#include <bg/type/type_text.hpp>
#include <bg/util/scope_exit.hpp>
#include <bg/version.hpp>

#include <nanobind/stl/optional.h>
#include <nanobind/stl/pair.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/vector.h>

#include <Python.h>

#include <array>
#include <cstdint>
#include <memory_resource>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace nb = nanobind;

namespace bg::python {
namespace {

class TypeDescriptor {
  public:
    explicit TypeDescriptor(const Type &type, bool accepts_absent_scalar = false) noexcept
        : type_{&type}, accepts_absent_scalar_{accepts_absent_scalar} {}

    const Type &type() const noexcept { return *type_; }
    bool accepts_absent_scalar() const noexcept { return accepts_absent_scalar_; }

  private:
    const Type *type_;
    bool accepts_absent_scalar_;
};

const ScalarType &scalar_type_from_python_type(nb::handle value) {
    if (value.ptr() == reinterpret_cast<PyObject *>(&PyBool_Type)) {
        return bool_type();
    }
    if (value.ptr() == reinterpret_cast<PyObject *>(&PyLong_Type)) {
        return int64_type();
    }
    if (value.ptr() == reinterpret_cast<PyObject *>(&PyFloat_Type)) {
        return double_type();
    }
    if (value.ptr() == reinterpret_cast<PyObject *>(&PyUnicode_Type)) {
        return string_type();
    }
    if (value.ptr() == nb::type<util::symbol>().ptr()) {
        return symbol_type();
    }

    throw std::invalid_argument{"Python annotations must use bool, int, float, str, or bg.Symbol"};
}

std::shared_ptr<const PythonNodeDefinition::SignatureDefinition>
make_signature(std::string_view name, const std::vector<std::pair<std::string, TypeDescriptor>> &parameters,
               const std::optional<TypeDescriptor> &output) {
    const auto time_series_parameter_count = static_cast<std::size_t>(std::ranges::count_if(
        parameters, [](const auto &parameter) { return parameter.second.type().is_time_series(); }));
    auto guarantees = std::vector<GuaranteeVariable>{};
    guarantees.reserve(time_series_parameter_count);
    auto time_series_types = std::vector<GenericTimeSeriesValueType>{};
    time_series_types.reserve(time_series_parameter_count);
    auto resolved_parameters = std::vector<Parameter>{};
    resolved_parameters.reserve(parameters.size());
    for (const auto &[parameter_name, parameter_type] : parameters) {
        const auto *type = &parameter_type.type();
        if (const auto *time_series = type_cast<TimeSeriesValueType>(type)) {
            guarantees.emplace_back(util::symbol{parameter_name});
            time_series_types.emplace_back(time_series->value_type(), guarantees.back());
            type = &time_series_types.back();
        }

        auto parameter = Parameter{util::symbol{parameter_name}, type};
        parameter.accepts_absent_scalar = parameter_type.accepts_absent_scalar();
        resolved_parameters.push_back(parameter);
    }

    auto signature = [&]() -> Signature {
        if (!output) {
            return {util::symbol{name}, std::move(resolved_parameters)};
        }
        const auto *time_series_type = type_cast<TimeSeriesType>(&output->type());
        if (time_series_type == nullptr) {
            throw std::invalid_argument{"a Python definition output annotation must be a time-series type"};
        }
        return {util::symbol{name}, std::move(resolved_parameters), *time_series_type};
    }();

    return std::make_shared<const PythonNodeDefinition::SignatureDefinition>(
        std::move(guarantees), std::move(time_series_types), std::move(signature));
}

WiringArgument wiring_argument(const Type &expected, nb::handle value, allocator &memory, GraphWiring &wiring) {
    if (nb::isinstance<PythonWiringPoint>(value)) {
        const auto point = nb::cast<PythonWiringPoint>(value).get(wiring);
        if (!point.has_output()) {
            throw std::invalid_argument{"a Python wiring point must have an output"};
        }
        return WiringArgument{point};
    }

    const auto *scalar_type = type_cast<ScalarType>(&expected);
    const auto *time_series_type = type_cast<TimeSeriesValueType>(&expected);
    if (scalar_type == nullptr && time_series_type == nullptr) {
        throw std::invalid_argument{"Python node and graph arguments require scalar or TS[T] types"};
    }

    const auto expects_scalar = scalar_type != nullptr;
    if (time_series_type != nullptr) {
        scalar_type = &time_series_type->value_type();
    }

    if (value.is_none() && expects_scalar) {
        return WiringArgument::absent_scalar(*scalar_type, resolve_scalar_runtime_type(*scalar_type, {}));
    }
    if (value.is_none()) {
        throw std::invalid_argument{"None cannot be wired to a time-series input"};
    }

    auto scalar = [&] {
        try {
            return scalar_conversions().from_python(*scalar_type, value, memory);
        } catch (const std::exception &error) {
            auto message = std::string{"cannot convert Python value to scalar type "};
            append_type_text(message, *scalar_type);
            message += ": ";
            message += error.what();
            throw std::invalid_argument{std::move(message)};
        }
    }();
    if (expects_scalar) {
        return WiringArgument{std::move(scalar)};
    }

    auto arguments = std::array{WiringArgument{std::move(scalar)}};
    const auto point = static_cast<const WiringTemplate &>(operators::const_)(
        std::span<const WiringArgument>{arguments});
    if (!point) {
        throw std::logic_error{"the const operator did not produce a time series"};
    }
    return WiringArgument{*point};
}

std::vector<WiringArgument> wiring_arguments(const Signature &signature, nb::sequence values, allocator &memory,
                                             GraphWiring &wiring) {
    if (nb::len(values) != signature.parameter_count()) {
        throw std::invalid_argument{"the Python definition received the wrong number of arguments"};
    }

    auto arguments = std::vector<WiringArgument>{};
    arguments.reserve(signature.parameter_count());
    for (auto index = std::size_t{}; index < signature.parameter_count(); ++index) {
        arguments.push_back(wiring_argument(*signature.parameter(index).type, values[index], memory, wiring));
    }
    return arguments;
}

nb::list test_graph(const PythonGraphDefinition &definition, nb::sequence input_sequences) {
    Object::drain_deferred_releases();
    auto drain_on_exit = util::scope_exit{[]() noexcept {
        if (Py_IsInitialized() && PyGILState_Check()) {
            Object::drain_deferred_releases();
        }
    }};
    const auto &signature = definition.signature();
    if (nb::len(input_sequences) != signature.parameter_count()) {
        throw std::invalid_argument{"test_graph requires one update sequence per graph parameter"};
    }

    auto input_memory = std::pmr::unsynchronized_pool_resource{};
    auto memory = allocator{input_memory};
    auto owned_inputs = std::vector<std::vector<std::optional<ScalarValue<>>>>{};
    owned_inputs.reserve(signature.parameter_count());
    auto input_views = std::vector<testing::TestGraphInput>{};
    input_views.reserve(signature.parameter_count());

    for (auto index = std::size_t{}; index < signature.parameter_count(); ++index) {
        const auto *time_series = type_cast<TimeSeriesValueType>(signature.parameter(index).type);
        if (time_series == nullptr) {
            throw std::invalid_argument{"test_graph currently requires every graph parameter to be TS[T]"};
        }

        const auto &scalar_type = time_series->value_type();
        auto ticks = nb::cast<nb::sequence>(input_sequences[index]);
        auto values = std::vector<std::optional<ScalarValue<>>>{};
        values.reserve(nb::len(ticks));
        for (const auto tick : ticks) {
            if (tick.is_none()) {
                values.emplace_back(std::nullopt);
                continue;
            }
            values.emplace_back(scalar_conversions().from_python(scalar_type, tick, memory));
        }
        const auto &runtime_type = resolve_scalar_runtime_type(scalar_type, {});
        owned_inputs.push_back(std::move(values));
        input_views.push_back({runtime_type, owned_inputs.back()});
    }

    auto results = [&] {
        auto release_gil = nb::gil_scoped_release{};
        return testing::test_graph(static_cast<const WiringTemplate &>(definition),
                                   std::span<const testing::TestGraphInput>{input_views});
    }();
    auto output = nb::list{};
    for (auto index = std::size_t{}; index < results.size(); ++index) {
        auto tick = results[index];
        if (!tick) {
            output.append(nb::none());
            continue;
        }
        output.append(scalar_conversions().to_python(*tick));
    }
    return output;
}

nb::object wire_node(const PythonNodeDefinition &definition, nb::sequence values) {
    auto &wiring = WiringContext::current().wiring();
    auto temporary_memory = std::pmr::unsynchronized_pool_resource{};
    auto memory = allocator{temporary_memory};
    auto arguments = wiring_arguments(definition.signature(), values, memory, wiring);

    const auto scope = PythonWiringScope::current();
    auto *node_template = scope->node_template(definition.identity());
    if (node_template == nullptr) {
        auto retained = std::make_unique<RuntimeSignatureNodeTemplate<PythonNodeDefinition>>(
            definition.retain_function());
        node_template = &wiring.own_node_template(std::move(retained));
        scope->add_node_template(definition.identity(), *node_template);
    }

    try {
        const auto result = static_cast<const WiringTemplate &>(*node_template)(
            std::span<const WiringArgument>{arguments});
        return result ? nb::cast(PythonWiringPoint{*result, PythonWiringScope::current()}) : nb::none();
    } catch (const TypeResolutionError &error) {
        auto message = std::string{"cannot wire Python node: "} + error.what() + " for argument '" +
                       std::string{error.subject().text()} + "'";
        if (error.expected() != nullptr && error.supplied() != nullptr) {
            message += " (expected ";
            append_type_text(message, *error.expected());
            message += ", received ";
            append_type_text(message, *error.supplied());
            message += ')';
        }
        throw std::invalid_argument{std::move(message)};
    } catch (const std::exception &error) {
        throw std::invalid_argument{std::string{"cannot wire Python node: "} + error.what()};
    }
}

nb::object wire_graph(const PythonGraphDefinition &definition, nb::sequence values) {
    auto &wiring = WiringContext::current().wiring();
    auto temporary_memory = std::pmr::unsynchronized_pool_resource{};
    auto memory = allocator{temporary_memory};
    auto arguments = wiring_arguments(definition.signature(), values, memory, wiring);

    try {
        const auto result = static_cast<const WiringTemplate &>(definition)(std::span<const WiringArgument>{arguments});
        return result ? nb::cast(PythonWiringPoint{*result, PythonWiringScope::current()}) : nb::none();
    } catch (const TypeResolutionError &error) {
        auto message = std::string{"cannot wire Python graph: "} + error.what() + " for argument '" +
                       std::string{error.subject().text()} + "'";
        if (error.expected() != nullptr && error.supplied() != nullptr) {
            message += " (expected ";
            append_type_text(message, *error.expected());
            message += ", received ";
            append_type_text(message, *error.supplied());
            message += ')';
        }
        throw std::invalid_argument{std::move(message)};
    } catch (const std::exception &error) {
        throw std::invalid_argument{std::string{"cannot wire Python graph: "} + error.what()};
    }
}

} // namespace

void bind_core(nb::module_ &module) {
    if (!nb::register_module(module)) {
        nb::raise_python_error();
    }

    module.def("version", [] { return std::string{bg::version()}; });
    module.def("_scalar_type", [](nb::handle value) { return TypeDescriptor{scalar_type_from_python_type(value)}; });
    module.def("_time_series_type", [](const TypeDescriptor &value) {
        if (value.accepts_absent_scalar()) {
            throw std::invalid_argument{"Optional[...] cannot be used inside TS[...]"};
        }
        const auto *scalar = type_cast<ScalarType>(&value.type());
        if (scalar == nullptr) {
            throw std::invalid_argument{"TS[...] requires a scalar annotation"};
        }
        return TypeDescriptor{time_series_value_type(*scalar, TimeSeriesGuarantee::ordinary)};
    });
    module.def("_optional_type", [](const TypeDescriptor &value) {
        if (value.type().is_time_series() || !value.type().is_scalar()) {
            throw std::invalid_argument{"Optional[...] currently applies only to scalar node inputs"};
        }
        return TypeDescriptor{value.type(), true};
    });
    module.def("_make_node", [](std::string name, const std::vector<std::pair<std::string, TypeDescriptor>> &parameters,
                                std::optional<TypeDescriptor> output, nb::handle function,
                                bool schedule_on_start) {
        return PythonNodeDefinition{make_signature(name, parameters, output), function, schedule_on_start};
    });
    module.def(
        "_make_graph",
        [](std::string name, const std::vector<std::pair<std::string, TypeDescriptor>> &parameters,
           std::optional<TypeDescriptor> output, nb::handle function) {
            return PythonGraphDefinition{make_signature(name, parameters, output), function};
        });
    module.def("_wire_node", &wire_node);
    module.def("_wire_graph", &wire_graph);
    module.def("_test_graph", &test_graph);
    module.def("_scalar_conversions_address",
               [] { return reinterpret_cast<std::uintptr_t>(&scalar_conversions()); });

    nb::class_<TypeDescriptor>(module, "_Type")
        .def_prop_ro("is_time_series", [](const TypeDescriptor &value) { return value.type().is_time_series(); })
        .def_prop_ro("accepts_absent_scalar", &TypeDescriptor::accepts_absent_scalar);
    nb::class_<PythonWiringPoint>(module, "WiringPoint")
        .def_prop_ro("has_output", &PythonWiringPoint::has_output)
        .def_prop_ro("type_name", [](const PythonWiringPoint &point) {
            auto result = std::string{};
            append_type_text(result, point.type());
            return result;
        });
    nb::class_<InputView>(module, "InputView")
        .def_prop_ro("valid", &InputView::valid)
        .def_prop_ro("modified", &InputView::modified)
        .def_prop_ro("value", &InputView::value);
    nb::class_<PythonNodeDefinition>(module, "_PythonNodeDefinition");
    nb::class_<PythonGraphDefinition>(module, "_PythonGraphDefinition");

    nb::class_<util::symbol>(module, "Symbol")
        .def(nb::init<>())
        .def(nb::init<std::string>())
        .def("__str__", [](const util::symbol &value) { return std::string{value.text()}; })
        .def("__repr__",
             [](const util::symbol &value) {
                 if (!value.has_value()) {
                     return std::string{"symbol()"};
                 }

                 const auto symbol_text = value.text();
                 const auto text = nb::str{symbol_text.data(), symbol_text.size()};
                 return "symbol(" + nb::cast<std::string>(nb::repr(text)) + ")";
             })
        .def("__hash__",
             [](const util::symbol &value) -> Py_hash_t {
                 const auto symbol_text = value.text();
                 const auto text = nb::str{symbol_text.data(), symbol_text.size()};
                 return PyObject_Hash(text.ptr());
             })
        .def(
            "__eq__", [](const util::symbol &left, const util::symbol &right) { return left == right; },
            nb::is_operator());

    module.attr("symbol") = module.attr("Symbol");
}

} // namespace bg::python
