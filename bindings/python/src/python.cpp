#include <bg/graph/details/variadic_wiring_operations.hpp>
#include <bg/graph/details/variadic_wiring_record.hpp>
#include <bg/node/runtime_signature_node_template.hpp>
#include <bg/operators.hpp>
#include <bg/python/graph_definition.hpp>
#include <bg/python/module.hpp>
#include <bg/python/node_definition.hpp>
#include <bg/python/object.hpp>
#include <bg/python/scalar_conversion.hpp>
#include <bg/python/wiring_point.hpp>
#include <bg/scalar/dict/scalar_type.hpp>
#include <bg/scalar/runtime_type.hpp>
#include <bg/testing/test_graph.hpp>
#include <bg/time_series/tsb/details/wiring.hpp>
#include <bg/time_series/tsl/details/wiring.hpp>
#include <bg/type/size_variable.hpp>
#include <bg/type/time_series_bundle_type.hpp>
#include <bg/type/time_series_dict_type.hpp>
#include <bg/type/time_series_list_type.hpp>
#include <bg/type/time_series_reference_type.hpp>
#include <bg/type/time_series_set_type.hpp>
#include <bg/type/type_resolution_error.hpp>
#include <bg/type/type_text.hpp>
#include <bg/type/type_variable.hpp>
#include <bg/util/scope_exit.hpp>
#include <bg/version.hpp>

#include "delta.hpp"
#include "dict_view.hpp"
#include "higher_order.hpp"
#include "operator_binding.hpp"
#include "wiring_argument.hpp"

#include <nanobind/stl/optional.h>
#include <nanobind/stl/pair.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/tuple.h>
#include <nanobind/stl/vector.h>

#include <Python.h>

#include <cstdint>
#include <memory_resource>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

namespace nb = nanobind;

namespace bg::python {
namespace {

class TypeDescriptor {
  public:
    explicit TypeDescriptor(const Type &type, bool accepts_absent_scalar = false) noexcept
        : type_{&type}, accepts_absent_scalar_{accepts_absent_scalar} {}
    explicit TypeDescriptor(const BundleSchema &schema) noexcept : type_{&schema}, bundle_schema_{&schema} {}
    TypeDescriptor(const Type &type, std::shared_ptr<const void> owner) noexcept
        : type_{&type}, owner_{std::move(owner)} {}
    TypeDescriptor(const TypeDescriptor &value, bool accepts_time_series_value) noexcept
        : type_{value.type_}, bundle_schema_{value.bundle_schema_},
          accepts_absent_scalar_{value.accepts_absent_scalar_}, owner_{value.owner_},
          accepts_time_series_value_{accepts_time_series_value} {}

    const Type &type() const noexcept { return *type_; }
    const BundleSchema *bundle_schema() const noexcept { return bundle_schema_; }
    bool accepts_absent_scalar() const noexcept { return accepts_absent_scalar_; }
    bool accepts_time_series_value() const noexcept { return accepts_time_series_value_; }
    const std::shared_ptr<const void> &owner() const noexcept { return owner_; }

  private:
    const Type *type_;
    const BundleSchema *bundle_schema_{};
    bool accepts_absent_scalar_{};
    std::shared_ptr<const void> owner_;
    bool accepts_time_series_value_{};
};

struct VariadicBundleAnnotation {
    VariadicBundleAnnotation() : schema{util::symbol{"TS_SCHEMA"}}, type{schema, TimeSeriesGuarantee::ordinary} {}

    TypeVariableType<BundleSchemaType> schema;
    GenericTimeSeriesBundleType type;
};

struct VariadicListAnnotation {
    VariadicListAnnotation(const TimeSeriesType &item, std::shared_ptr<const void> item_owner)
        : item_owner{std::move(item_owner)}, size{util::symbol{"SIZE"}},
          type{item, size, TimeSeriesGuarantee::ordinary} {}

    std::shared_ptr<const void> item_owner;
    SizeVariable size;
    GenericTimeSeriesListType type;
};

using PythonParameter = std::tuple<std::string, TypeDescriptor, std::string>;

struct PythonValueInputType {
    PythonValueInputType(std::string_view name, const ScalarType &scalar_type)
        : guarantee{util::symbol{name}}, time_series{scalar_type, guarantee},
          type{util::symbol{name}, std::vector<const Type *>{&scalar_type, &time_series}} {}

    GuaranteeVariable guarantee;
    GenericTimeSeriesValueType time_series;
    TypeVariableType<Type> type;
};

class PythonSchemaRegistry {
  public:
    TypeDescriptor make(std::string_view name, const std::optional<TypeDescriptor> &base,
                        const std::vector<std::pair<std::string, TypeDescriptor>> &fields) {
        const BundleSchema *base_schema = nullptr;
        if (base) {
            base_schema = base->bundle_schema();
            if (base_schema == nullptr) {
                throw std::invalid_argument{"a Python time-series schema base must be a bundle schema"};
            }
        }

        auto bundle_fields = std::vector<BundleField>{};
        bundle_fields.reserve(fields.size());
        for (const auto &[field_name, field_type] : fields) {
            const auto *time_series = type_cast<TimeSeriesType>(&field_type.type());
            if (time_series == nullptr) {
                throw std::invalid_argument{"a Python time-series schema field must have a time-series type"};
            }
            bundle_fields.push_back({util::symbol{field_name}, time_series});
        }

        auto identity = std::make_shared<const char>();
        const auto *identity_address = identity.get();
        identities_.push_back(std::move(identity));
        return TypeDescriptor{bundle_schema(util::symbol{name}, identity_address, base_schema, bundle_fields)};
    }

  private:
    std::vector<std::shared_ptr<const char>> identities_;
};

PythonSchemaRegistry &schema_registry() {
    static auto registry = PythonSchemaRegistry{};
    return registry;
}

PythonNodeDefinition::SignatureDefinition::GenericTimeSeriesType
generic_time_series_type(const TimeSeriesType &type, const GuaranteeVariable &guarantee) {
    if (const auto *value = type_cast<TimeSeriesValueType>(&type)) {
        return GenericTimeSeriesValueType{value->value_type(), guarantee};
    }
    if (const auto *reference = type_cast<TimeSeriesReferenceType>(&type)) {
        return GenericTimeSeriesReferenceType{reference->target_type(), guarantee};
    }
    if (const auto *set = type_cast<TimeSeriesSetType>(&type)) {
        return GenericTimeSeriesSetType{set->item_type(), guarantee};
    }
    if (const auto *list = type_cast<TimeSeriesListType>(&type)) {
        return GenericTimeSeriesListType{list->item_type(), list->size(), guarantee};
    }
    if (const auto *dictionary = type_cast<TimeSeriesDictionaryType>(&type)) {
        return GenericTimeSeriesDictionaryType{dictionary->key_type(), dictionary->value_type(),
                                               dictionary->key_set_type(), guarantee};
    }
    if (const auto *bundle = type_cast<TimeSeriesBundleType>(&type)) {
        return GenericTimeSeriesBundleType{bundle->schema_type(), guarantee};
    }

    throw std::invalid_argument{"a Python definition uses an unsupported time-series type"};
}

std::shared_ptr<const PythonNodeDefinition::SignatureDefinition>
make_signature(std::string_view name, const std::vector<PythonParameter> &parameters,
               const std::optional<TypeDescriptor> &output) {
    const auto time_series_parameter_count = static_cast<std::size_t>(std::ranges::count_if(
        parameters, [](const auto &parameter) { return std::get<1>(parameter).type().is_time_series(); }));
    auto type_owners = std::vector<std::shared_ptr<const void>>{};
    type_owners.reserve(parameters.size() + static_cast<std::size_t>(output.has_value()));
    auto guarantees = std::vector<GuaranteeVariable>{};
    guarantees.reserve(time_series_parameter_count);
    auto time_series_types = std::vector<PythonNodeDefinition::SignatureDefinition::GenericTimeSeriesType>{};
    time_series_types.reserve(time_series_parameter_count);
    auto resolved_parameters = std::vector<Parameter>{};
    resolved_parameters.reserve(parameters.size());
    auto wiring_parameter_types = std::vector<const Type *>{};
    wiring_parameter_types.reserve(parameters.size());
    for (const auto &[parameter_name, parameter_type, variadic] : parameters) {
        if (parameter_type.owner()) {
            type_owners.push_back(parameter_type.owner());
        }

        const Type *type = &parameter_type.type();
        const Type *wiring_type = type;
        if (parameter_type.accepts_time_series_value()) {
            const auto *scalar = type_cast<ScalarType>(type);
            if (scalar == nullptr || !variadic.empty()) {
                throw std::invalid_argument{"a Python @node input requires a scalar annotation"};
            }

            auto owner = std::make_shared<PythonValueInputType>(parameter_name, *scalar);
            type = &owner->type;
            wiring_type = scalar;
            type_owners.push_back(std::move(owner));
        }
        if (const auto *time_series = type_cast<TimeSeriesType>(type); time_series != nullptr && variadic.empty()) {
            guarantees.emplace_back(util::symbol{parameter_name});
            time_series_types.push_back(generic_time_series_type(*time_series, guarantees.back()));
            type = std::visit([](const auto &value) -> const Type * { return &value; }, time_series_types.back());
        }

        auto parameter = Parameter{util::symbol{parameter_name}, type};
        parameter.accepts_absent_scalar = parameter_type.accepts_absent_scalar();
        if (parameter_type.accepts_time_series_value() && parameter_type.accepts_absent_scalar()) {
            parameter.validity = optional;
        }
        if (!variadic.empty()) {
            auto mode = bg::details::VariadicMode::none;
            if (variadic == "positional") {
                mode = bg::details::VariadicMode::positional;
            } else if (variadic == "named") {
                mode = bg::details::VariadicMode::named;
            } else {
                throw std::invalid_argument{"a Python variadic parameter has an invalid mode"};
            }

            const auto *time_series = type_cast<TimeSeriesType>(type);
            if (time_series == nullptr) {
                throw std::invalid_argument{"a Python variadic parameter requires a time-series collection"};
            }

            if (type_cast<TimeSeriesListType>(time_series) != nullptr) {
                parameter.variadic_operations = &bg::details::time_series_list_variadic_wiring_operations();
            } else if (type_cast<TimeSeriesBundleType>(time_series) != nullptr) {
                parameter.variadic_operations = &bg::details::time_series_bundle_variadic_wiring_operations();
            } else {
                throw std::invalid_argument{"a Python variadic parameter requires a TSL or TSB annotation"};
            }
            parameter.variadic_mode = mode;
            parameter.variadic_operations->validate_parameter(*time_series, mode);
        }
        resolved_parameters.push_back(parameter);
        wiring_parameter_types.push_back(wiring_type);
    }

    if (output && output->owner()) {
        type_owners.push_back(output->owner());
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
        std::move(type_owners), std::move(guarantees), std::move(time_series_types), std::move(wiring_parameter_types),
        std::move(signature));
}

std::vector<WiringArgument> wiring_arguments(const Signature &signature, nb::sequence fixed_values,
                                             nb::sequence positional_values, nb::dict named_values, allocator &memory,
                                             GraphWiring &wiring, std::span<const Type *const> parameter_types = {}) {
    const auto fixed_count = signature.first_variadic_parameter().value_or(signature.parameter_count());
    if (nb::len(fixed_values) != fixed_count) {
        throw std::invalid_argument{"the Python definition received the wrong number of fixed arguments"};
    }

    auto arguments = std::vector<WiringArgument>{};
    arguments.reserve(fixed_count + nb::len(positional_values) + nb::len(named_values));
    for (auto index = std::size_t{}; index < fixed_count; ++index) {
        const auto &expected = parameter_types.empty() ? *signature.parameter(index).type : *parameter_types[index];
        arguments.push_back(details::wiring_argument(expected, fixed_values[index], memory, wiring));
    }

    for (auto index = std::size_t{}; index < nb::len(positional_values); ++index) {
        auto argument = details::wiring_argument(positional_values[index], memory, wiring);
        bg::details::wiring_argument_access::set_variadic_name(argument, util::symbol{std::to_string(index)},
                                                               bg::details::VariadicMode::positional);
        arguments.push_back(std::move(argument));
    }

    const auto positional_parameter = signature.variadic_parameter(bg::details::VariadicMode::positional);
    const auto named_parameter = signature.variadic_parameter(bg::details::VariadicMode::named);
    for (const auto [name, value] : named_values) {
        auto argument = details::wiring_argument(value, memory, wiring);
        const auto argument_name = util::symbol{nb::cast<std::string>(name)};
        const auto exact_parameter =
            (positional_parameter && signature.parameter(*positional_parameter).name == argument_name) ||
            (named_parameter && signature.parameter(*named_parameter).name == argument_name);
        if (exact_parameter && argument.is_time_series() &&
            bg::details::variadic_wiring_record(argument.time_series()) != nullptr) {
            bg::details::wiring_argument_access::clear_name(argument);
        } else {
            bg::details::wiring_argument_access::set_variadic_name(argument, argument_name,
                                                                   exact_parameter ? bg::details::VariadicMode::none
                                                                                   : bg::details::VariadicMode::named);
        }
        arguments.push_back(std::move(argument));
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

nb::object wire_node(const PythonNodeDefinition &definition, nb::sequence fixed_values, nb::sequence positional_values,
                     nb::dict named_values) {
    auto &wiring = WiringContext::current().wiring();
    auto temporary_memory = std::pmr::unsynchronized_pool_resource{};
    auto memory = allocator{temporary_memory};
    auto arguments = wiring_arguments(definition.signature(), fixed_values, positional_values, named_values, memory,
                                      wiring, definition.wiring_parameter_types());

    const auto scope = PythonWiringScope::current();
    auto *node_template = scope->node_template(definition.identity());
    if (node_template == nullptr) {
        auto retained =
            std::make_unique<RuntimeSignatureNodeTemplate<PythonNodeDefinition>>(definition.retain_function());
        node_template = &wiring.own_node_template(std::move(retained));
        scope->add_node_template(definition.identity(), *node_template);
    }

    try {
        const auto result =
            static_cast<const WiringTemplate &>(*node_template)(std::span<const WiringArgument>{arguments});
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

nb::object wire_graph(const PythonGraphDefinition &definition, nb::sequence fixed_values,
                      nb::sequence positional_values, nb::dict named_values) {
    auto &wiring = WiringContext::current().wiring();
    auto temporary_memory = std::pmr::unsynchronized_pool_resource{};
    auto memory = allocator{temporary_memory};
    auto arguments =
        wiring_arguments(definition.signature(), fixed_values, positional_values, named_values, memory, wiring);

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
    module.def("_scalar_type",
               [](nb::handle value) { return TypeDescriptor{scalar_conversions().type_from_python(value)}; });
    module.def("_dict_scalar_type", [](const TypeDescriptor &key_value, const TypeDescriptor &mapped_value) {
        const auto *key = type_cast<ScalarType>(&key_value.type());
        const auto *mapped = type_cast<ScalarType>(&mapped_value.type());
        if (key == nullptr || mapped == nullptr) {
            throw std::invalid_argument{"dict[K, V] requires scalar key and value annotations"};
        }
        return TypeDescriptor{dict_scalar_type(*key, *mapped)};
    });
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
    module.def("_reference_type", [](const TypeDescriptor &value) {
        const auto *target = type_cast<TimeSeriesType>(&value.type());
        if (target == nullptr) {
            throw std::invalid_argument{"REF[...] requires a time-series annotation"};
        }
        return TypeDescriptor{time_series_reference_type(*target, TimeSeriesGuarantee::ordinary)};
    });
    module.def("_time_series_set_type", [](const TypeDescriptor &value) {
        const auto *item = type_cast<ScalarType>(&value.type());
        if (item == nullptr) {
            throw std::invalid_argument{"TSS[...] requires a scalar annotation"};
        }
        return TypeDescriptor{time_series_set_type(*item, TimeSeriesGuarantee::ordinary)};
    });
    module.def("_time_series_list_type", [](const TypeDescriptor &value, std::size_t size) {
        const auto *item = type_cast<TimeSeriesType>(&value.type());
        if (item == nullptr) {
            throw std::invalid_argument{"TSL[...] requires a time-series item annotation"};
        }
        return TypeDescriptor{time_series_list_type(*item, size, TimeSeriesGuarantee::ordinary)};
    });
    module.def("_variadic_time_series_list_type", [](const TypeDescriptor &value) {
        const auto *item = type_cast<TimeSeriesType>(&value.type());
        if (item == nullptr) {
            throw std::invalid_argument{"TSL[..., SIZE] requires a time-series item annotation"};
        }
        auto annotation = std::make_shared<VariadicListAnnotation>(*item, value.owner());
        const auto &type = annotation->type;
        return TypeDescriptor{type, std::move(annotation)};
    });
    module.def("_time_series_dict_type", [](const TypeDescriptor &key_value, const TypeDescriptor &item_value) {
        const auto *key = type_cast<ScalarType>(&key_value.type());
        const auto *item = type_cast<TimeSeriesType>(&item_value.type());
        if (key == nullptr || item == nullptr) {
            throw std::invalid_argument{"TSD[...] requires a scalar key and a time-series item annotation"};
        }
        return TypeDescriptor{time_series_dictionary_type(*key, *item, TimeSeriesGuarantee::ordinary)};
    });
    module.def("_make_time_series_schema", [](std::string name, nb::handle identity, std::optional<TypeDescriptor> base,
                                              const std::vector<std::pair<std::string, TypeDescriptor>> &fields) {
        (void)identity;
        return schema_registry().make(name, base, fields);
    });
    module.def("_time_series_bundle_type", [](const TypeDescriptor &value) {
        const auto *schema = value.bundle_schema();
        if (schema == nullptr) {
            throw std::invalid_argument{"TSB[...] requires a TimeSeriesSchema"};
        }
        return TypeDescriptor{time_series_bundle_type(*schema, TimeSeriesGuarantee::ordinary)};
    });
    module.def("_variadic_time_series_bundle_type", [] {
        auto annotation = std::make_shared<VariadicBundleAnnotation>();
        const auto &type = annotation->type;
        return TypeDescriptor{type, std::move(annotation)};
    });
    module.def("_optional_type", [](const TypeDescriptor &value) {
        if (value.type().is_time_series() || !value.type().is_scalar()) {
            throw std::invalid_argument{"Optional[...] currently applies only to scalar node inputs"};
        }
        return TypeDescriptor{value.type(), true};
    });
    module.def("_node_input_type", [](const TypeDescriptor &value) {
        if (type_cast<ScalarType>(&value.type()) == nullptr) {
            throw std::invalid_argument{"Python @node inputs require scalar annotations"};
        }
        return TypeDescriptor{value, true};
    });
    module.def("_make_node", [](std::string name, const std::vector<PythonParameter> &parameters,
                                std::optional<TypeDescriptor> output, nb::handle function, bool schedule_on_start,
                                bool pass_time_series_values) {
        return PythonNodeDefinition{make_signature(name, parameters, output), function, schedule_on_start,
                                    pass_time_series_values};
    });
    module.def("_make_graph", [](std::string name, const std::vector<PythonParameter> &parameters,
                                 std::optional<TypeDescriptor> output, nb::handle function) {
        return PythonGraphDefinition{make_signature(name, parameters, output), function};
    });
    module.def("_wire_node", &wire_node);
    module.def("_wire_graph", &wire_graph);
    module.def("_test_graph", &test_graph);
    module.def("_register_delta_types", &details::register_delta_types);
    module.def("_scalar_conversions_address", [] { return reinterpret_cast<std::uintptr_t>(&scalar_conversions()); });

    nb::class_<TypeDescriptor>(module, "_Type")
        .def_prop_ro("is_time_series", [](const TypeDescriptor &value) { return value.type().is_time_series(); })
        .def_prop_ro("accepts_absent_scalar", &TypeDescriptor::accepts_absent_scalar);
    nb::class_<PythonWiringPoint>(module, "WiringPoint")
        .def_prop_ro("has_output", &PythonWiringPoint::has_output)
        .def_prop_ro("type_name",
                     [](const PythonWiringPoint &point) {
                         auto result = std::string{};
                         append_type_text(result, point.type());
                         return result;
                     })
        .def("__getitem__", &details::getitem);
    nb::class_<InputView>(module, "InputView")
        .def_prop_ro("valid", &InputView::valid)
        .def_prop_ro("modified", &InputView::modified)
        .def_prop_ro("value", &InputView::value)
        .def_prop_ro("delta_value", &InputView::delta_value)
        .def("__getitem__", &InputView::getitem);
    bind_dict_view(module);
    nb::class_<ReferenceValue>(module, "ReferenceValue").def_prop_ro("valid", &ReferenceValue::valid);
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

    (void)scalar_conversions().add_python_type(nb::type<util::symbol>(), symbol_type());

    module.attr("symbol") = module.attr("Symbol");
    details::bind_higher_order(module);
    details::bind_operators(module);
}

} // namespace bg::python
