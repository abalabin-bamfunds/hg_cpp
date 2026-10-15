#include <bg/graph/details/variadic_wiring_operations.hpp>
#include <bg/graph/details/variadic_wiring_record.hpp>
#include <bg/library/operators.hpp>
#include <bg/node/runtime_signature_node_template.hpp>
#include <bg/operators.hpp>
#include <bg/run_graph.hpp>
#include <bg/evaluation_profiler.hpp>
#include <bg/python/graph_definition.hpp>
#include <bg/python/module.hpp>
#include <bg/python/node_definition.hpp>
#include <bg/python/object.hpp>
#include <bg/python/scalar_conversion.hpp>
#include <bg/python/wiring_point.hpp>
#include <bg/service.hpp>
#include <bg/service_catalogue.hpp>
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
#include <bg/type/type_registry.hpp>
#include <bg/type/type.hpp>
#include <bg/type/type_variable.hpp>
#include <bg/type/variable_registry.hpp>
#include <bg/util/scope_exit.hpp>
#include <bg/version.hpp>

#include "delta.hpp"
#include "dict_view.hpp"
#include "higher_order.hpp"
#include "operator_binding.hpp"
#include "wiring_argument.hpp"

#include <nanobind/stl/optional.h>
#include <nanobind/stl/chrono.h>
#include <nanobind/stl/pair.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/tuple.h>
#include <nanobind/stl/vector.h>

#include <Python.h>

#include <cstdint>
#include <exception>
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
using namespace nanobind::literals;

namespace bg::python {
namespace {

std::exception_ptr python_error_cause(std::exception_ptr cause) {
    if (cause == nullptr) {
        return {};
    }

    try {
        std::rethrow_exception(cause);
    } catch (const WiringError &error) {
        return python_error_cause(error.cause());
    } catch (const nb::python_error &) {
        return cause;
    } catch (...) {
        return {};
    }
}

[[noreturn]] void rethrow_python_error(const WiringError &error) {
    const auto cause = python_error_cause(error.cause());
    if (cause == nullptr) {
        throw error;
    }

    try {
        std::rethrow_exception(cause);
    } catch (nb::python_error &python_exception) {
        auto preserved = nb::python_error{python_exception};
        if (!error.contexts().empty()) {
            const auto context = WiringError::from_exception("BG wiring context", error.contexts(), {});
            try {
                const auto add_note = nb::getattr(nb::borrow<nb::object>(PyExc_BaseException), "add_note");
                add_note(preserved.value(), nb::str{context.what()});
            } catch (const nb::python_error &) {
                PyErr_Clear();
            }
        }
        preserved.restore();
        nb::raise_python_error();
    }

    std::terminate();
}

class TypeDescriptor {
  public:
    explicit TypeDescriptor(const Type &type, bool accepts_absent_scalar = false) noexcept
        : type_{&type}, accepts_absent_scalar_{accepts_absent_scalar} {}
    explicit TypeDescriptor(const Schema &schema) noexcept : type_{&schema}, schema_{&schema} {}
    TypeDescriptor(const TypeDescriptor &value, bool accepts_time_series_value) noexcept
        : type_{value.type_}, schema_{value.schema_},
          accepts_absent_scalar_{value.accepts_absent_scalar_},
          accepts_time_series_value_{accepts_time_series_value} {}

    const Type &type() const noexcept { return *type_; }
    const Schema *schema() const noexcept { return schema_; }
    bool accepts_absent_scalar() const noexcept { return accepts_absent_scalar_; }
    bool accepts_time_series_value() const noexcept { return accepts_time_series_value_; }

  private:
    const Type *type_;
    const Schema *schema_{};
    bool accepts_absent_scalar_{};
    bool accepts_time_series_value_{};
};

using PythonParameter = std::tuple<std::string, TypeDescriptor, std::string>;

class PythonServiceDefinition {
  public:
    PythonServiceDefinition(std::unique_ptr<ServiceDefinition> service,
                            std::shared_ptr<const PythonNodeDefinition::SignatureDefinition> signature)
        : service_{std::move(service)}, signature_{std::move(signature)} {}

    const ServiceDefinition &service() const noexcept { return *service_; }
    const PythonNodeDefinition::SignatureDefinition &signature() const noexcept { return *signature_; }

    nb::object wire(nb::sequence arguments) const {
        auto &wiring = WiringContext::current().wiring();
        const auto &service_signature = signature_->signature;
        if (nb::len(arguments) != service_signature.parameter_count()) {
            throw std::invalid_argument{"the Python service received the wrong number of arguments"};
        }

        auto temporary_memory = std::pmr::unsynchronized_pool_resource{};
        auto memory = allocator{temporary_memory};
        auto wiring_arguments = std::vector<WiringArgument>{};
        wiring_arguments.reserve(service_signature.parameter_count());
        for (auto index = std::size_t{}; index < service_signature.parameter_count(); ++index) {
            const auto &parameter_type = *signature_->wiring_parameter_types[index];
            if (parameter_type.is_scalar()) {
                wiring_arguments.push_back(details::wiring_argument(parameter_type, arguments[index], memory, wiring));
                continue;
            }
            if (!nb::isinstance<PythonWiringPoint>(arguments[index])) {
                throw std::invalid_argument{"Python service time-series arguments require wiring points"};
            }
            wiring_arguments.push_back(nb::cast<const PythonWiringPoint &>(arguments[index]).argument(wiring));
        }

        const auto result = service_->operator()(std::span<const WiringArgument>{wiring_arguments});
        if (!result) {
            return nb::none();
        }
        return nb::cast(PythonWiringPoint{*result, PythonWiringScope::current()});
    }

  private:
    std::unique_ptr<ServiceDefinition> service_;
    std::shared_ptr<const PythonNodeDefinition::SignatureDefinition> signature_;
};

class PythonServiceCatalogue {
  public:
    void register_impl(const PythonServiceDefinition &service, const PythonGraphDefinition &implementation,
                       nb::handle service_owner, nb::handle implementation_owner, nb::sequence positional,
                       nb::dict named) {
        auto temporary_memory = std::pmr::unsynchronized_pool_resource{};
        auto memory = allocator{temporary_memory};
        auto arguments = std::vector<WiringArgument>{};
        arguments.reserve(nb::len(positional) + nb::len(named));
        for (auto index = std::size_t{}; index < nb::len(positional); ++index) {
            arguments.push_back(registration_argument(positional[index], memory));
        }
        for (const auto [name, value] : named) {
            arguments.push_back(registration_argument(value, memory, util::symbol{nb::cast<std::string>(name)}));
        }

        auto graph = std::make_shared<PythonGraphDefinition>(implementation);
        keep_alive_.emplace_back(service_owner);
        keep_alive_.emplace_back(implementation_owner);
        catalogue_.register_impl(service.service(), std::move(graph), arguments);
    }

    const ServiceCatalogue &catalogue() const noexcept { return catalogue_; }

  private:
    WiringArgument registration_argument(nb::handle value, allocator &memory,
                                         std::optional<util::symbol> name = std::nullopt) {
        if (nb::isinstance<PythonWiringPoint>(value)) {
            auto wiring_argument =
                nb::cast<const PythonWiringPoint &>(value).argument(WiringContext::current().wiring());
            auto argument = name ? WiringArgument{*name, std::move(wiring_argument)} : std::move(wiring_argument);
            if (name) {
                argument.match_fixed_parameter_by_name();
            }
            return argument;
        }
        if (value.is_none()) {
            throw std::invalid_argument{"None is not a service implementation argument"};
        }

        auto scalar = scalar_conversions().from_python(value, memory);
        auto argument = name ? WiringArgument{*name, std::move(scalar)} : WiringArgument{std::move(scalar)};
        if (name) {
            argument.match_fixed_parameter_by_name();
        }
        return argument;
    }

    std::vector<Object> keep_alive_;
    ServiceCatalogue catalogue_;
};

std::unique_ptr<TimeSeriesType> generic_time_series_type(const TimeSeriesType &type,
                                                         const GuaranteeVariable &guarantee) {
    if (const auto *value = type_cast<TimeSeriesValueType>(&type)) {
        return std::make_unique<GenericTimeSeriesValueType>(value->value_type(), guarantee);
    }
    if (const auto *reference = type_cast<TimeSeriesReferenceType>(&type)) {
        return std::make_unique<GenericTimeSeriesReferenceType>(reference->target_type(), guarantee);
    }
    if (const auto *set = type_cast<TimeSeriesSetType>(&type)) {
        return std::make_unique<GenericTimeSeriesSetType>(set->item_type(), guarantee);
    }
    if (const auto *list = type_cast<TimeSeriesListType>(&type)) {
        return std::make_unique<GenericTimeSeriesListType>(list->item_type(), list->size(), guarantee);
    }
    if (const auto *dictionary = type_cast<TimeSeriesDictionaryType>(&type)) {
        return std::make_unique<GenericTimeSeriesDictionaryType>(dictionary->key_type(), dictionary->value_type(),
                                                                 dictionary->key_set_type(), guarantee);
    }
    if (const auto *bundle = type_cast<TimeSeriesBundleType>(&type)) {
        return std::make_unique<GenericTimeSeriesBundleType>(bundle->schema_type(), guarantee);
    }

    throw std::invalid_argument{"a Python definition uses an unsupported time-series type"};
}

std::shared_ptr<const PythonNodeDefinition::SignatureDefinition>
make_signature(std::string_view name, const std::vector<PythonParameter> &parameters,
               const std::optional<TypeDescriptor> &output) {
    auto resolved_parameters = std::vector<Parameter>{};
    resolved_parameters.reserve(parameters.size());
    auto wiring_parameter_types = std::vector<const Type *>{};
    wiring_parameter_types.reserve(parameters.size());
    for (const auto &[parameter_name, parameter_type, variadic] : parameters) {
        const Type *type = &parameter_type.type();
        const Type *wiring_type = type;
        if (parameter_type.accepts_time_series_value()) {
            const auto *scalar = type_cast<ScalarType>(type);
            if (scalar == nullptr || !variadic.empty()) {
                throw std::invalid_argument{"a Python @node input requires a scalar annotation"};
            }

            const auto &guarantee = VariableRegistry::instance().register_owned(
                std::make_unique<GuaranteeVariable>(util::symbol{parameter_name}));
            const auto &time_series = TypeRegistry::instance().register_owned(
                std::make_unique<GenericTimeSeriesValueType>(*scalar, guarantee));
            const auto &input_type = TypeRegistry::instance().register_owned(std::make_unique<TypeVariableType<Type>>(
                util::symbol{parameter_name}, std::vector<const Type *>{scalar, &time_series}));
            type = &input_type;
            wiring_type = scalar;
        }
        if (const auto *time_series = type_cast<TimeSeriesType>(type); time_series != nullptr && variadic.empty()) {
            const auto &guarantee = VariableRegistry::instance().register_owned(
                std::make_unique<GuaranteeVariable>(util::symbol{parameter_name}));
            type = &TypeRegistry::instance().register_owned(generic_time_series_type(*time_series, guarantee));
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

    return std::make_shared<const PythonNodeDefinition::SignatureDefinition>(std::move(wiring_parameter_types),
                                                                              std::move(signature));
}

PythonServiceDefinition make_service(std::string name, std::string kind,
                                     const std::vector<PythonParameter> &parameters,
                                     const std::optional<TypeDescriptor> &output) {
    auto signature = make_signature(name, parameters, output);
    auto service = [&]() -> std::unique_ptr<ServiceDefinition> {
        if (kind == "reference") {
            return std::make_unique<ReferenceService>(signature->signature);
        }
        if (kind == "request_reply") {
            return std::make_unique<Service>(signature->signature);
        }
        throw std::invalid_argument{"unknown Python service kind"};
    }();
    return {std::move(service), std::move(signature)};
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
        argument.set_variadic_name(util::symbol{std::to_string(index)}, bg::details::VariadicMode::positional);
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
            argument.clear_name();
        } else {
            argument.set_variadic_name(argument_name, exact_parameter ? bg::details::VariadicMode::none
                                                                      : bg::details::VariadicMode::named);
        }
        arguments.push_back(std::move(argument));
    }
    return arguments;
}

nb::object wire_graph(const PythonGraphDefinition &definition, nb::sequence fixed_values,
                     nb::sequence positional_values, nb::dict named_values);

nb::list test_graph(const PythonGraphDefinition &definition, nb::sequence input_sequences,
                    PythonServiceCatalogue *service_catalogue, EvaluationProfiler::Recorder *recorder) {
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
        const auto &input_type = *definition.wiring_parameter_types()[index];
        const auto *scalar_type = type_cast<ScalarType>(&input_type);
        if (scalar_type == nullptr) {
            if (const auto *time_series = type_cast<TimeSeriesValueType>(&input_type)) {
                scalar_type = &time_series->value_type();
            }
        }
        if (scalar_type == nullptr) {
            throw std::invalid_argument{
                "test_graph currently requires every graph parameter to be a scalar or TS[scalar] type"};
        }

        auto ticks = nb::cast<nb::sequence>(input_sequences[index]);
        auto values = std::vector<std::optional<ScalarValue<>>>{};
        values.reserve(nb::len(ticks));
        for (const auto tick : ticks) {
            if (tick.is_none()) {
                values.emplace_back(std::nullopt);
                continue;
            }
            values.emplace_back(scalar_conversions().from_python(*scalar_type, tick, memory));
        }
        const auto &runtime_type = resolve_scalar_runtime_type(*scalar_type, {});
        owned_inputs.push_back(std::move(values));
        input_views.push_back({runtime_type, owned_inputs.back()});
    }

    auto results = [&] {
        try {
            auto release_gil = nb::gil_scoped_release{};
            if (service_catalogue != nullptr && recorder != nullptr) {
                return testing::test_graph(static_cast<const WiringTemplate &>(definition), *recorder,
                                           service_catalogue->catalogue(),
                                           std::span<const testing::TestGraphInput>{input_views});
            }
            if (service_catalogue != nullptr) {
                return testing::test_graph(static_cast<const WiringTemplate &>(definition),
                                           service_catalogue->catalogue(),
                                           std::span<const testing::TestGraphInput>{input_views});
            }
            if (recorder != nullptr) {
                return testing::test_graph(static_cast<const WiringTemplate &>(definition), *recorder,
                                           std::span<const testing::TestGraphInput>{input_views});
            }
            return testing::test_graph(static_cast<const WiringTemplate &>(definition),
                                       std::span<const testing::TestGraphInput>{input_views});
        } catch (const WiringError &error) {
            rethrow_python_error(error);
        }
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

void run_graph(const PythonGraphDefinition &definition, nb::sequence fixed_values, nb::sequence positional_values,
               nb::dict named_values, PythonServiceCatalogue *service_catalogue,
               EvaluationProfiler::Recorder *recorder) {
    Object::drain_deferred_releases();
    auto drain_on_exit = util::scope_exit{[]() noexcept {
        if (Py_IsInitialized() && PyGILState_Check()) {
            Object::drain_deferred_releases();
        }
    }};

    auto run = [&] {
        auto acquire_gil = nb::gil_scoped_acquire{};
        auto scope = PythonWiringScope{WiringContext::current().wiring()};
        (void)wire_graph(definition, fixed_values, positional_values, named_values);
    };

    try {
        auto release_gil = nb::gil_scoped_release{};
        if (service_catalogue != nullptr && recorder != nullptr) {
            bg::run_graph(run, service_catalogue->catalogue(), *recorder);
        } else if (service_catalogue != nullptr) {
            bg::run_graph(run, service_catalogue->catalogue());
        } else if (recorder != nullptr) {
            bg::run_graph(run, *recorder);
        } else {
            bg::run_graph(run);
        }
    } catch (const WiringError &error) {
        rethrow_python_error(error);
    }
}

nb::dict profiler_stats(EvaluationProfilerStats stats) {
    auto result = nb::dict{};
    result["evaluations"] = stats.evaluations;
    result["checkpoint_ticks"] = stats.checkpoint_ticks;
    return result;
}

nb::dict profiler_cycle_stats(EvaluationProfilerCycleStats stats) {
    auto result = nb::dict{};
    result["cycles"] = stats.cycles;
    result["total_nanoseconds"] = stats.total_nanoseconds;
    result["min_nanoseconds"] = stats.min_nanoseconds;
    result["max_nanoseconds"] = stats.max_nanoseconds;
    return result;
}

nb::dict profiler_graph(const EvaluationProfilerGraph &graph) {
    auto result = nb::dict{};
    result["graph_id"] = graph.graph_id.value;
    result["parent_graph_id"] = graph.parent_graph_id.value;
    result["owner_node_index"] = graph.owner_node_index;
    result["node_count"] = graph.node_count;
    result["evaluations"] = graph.totals.evaluations;
    result["checkpoint_ticks"] = graph.totals.checkpoint_ticks;
    result["active"] = graph.active;
    if (graph.label) {
        result["label"] = scalar_conversions().to_python(graph.label->view());
    } else {
        result["label"] = nb::none();
    }
    if (graph.last_updated) {
        result["last_updated"] = nb::cast(*graph.last_updated);
    } else {
        result["last_updated"] = nb::none();
    }
    return result;
}

nb::dict profiler_node_period(EvaluationProfilerNodePeriod period) {
    auto result = nb::dict{};
    result["node_index"] = period.node_index;
    result["evaluations"] = period.totals.evaluations;
    result["checkpoint_ticks"] = period.totals.checkpoint_ticks;
    return result;
}

nb::dict profiler_outer_period(EvaluationProfilerCyclePeriod period) {
    auto result = nb::dict{};
    result["start"] = nb::cast(period.start);
    result["timing"] = profiler_cycle_stats(period.timing);
    return result;
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
    } catch (nb::python_error &) {
        throw;
    } catch (const TypeResolutionError &error) {
        auto message = std::string{"cannot wire Python node: "} + error.what() + " for argument '" +
                       std::string{error.subject().text()} + "'";
        if (error.expected() != nullptr && error.supplied() != nullptr) {
            message += " (expected ";
            error.expected()->format_name(message);
            message += ", received ";
            error.supplied()->format_name(message);
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
    auto arguments = wiring_arguments(definition.signature(), fixed_values, positional_values, named_values, memory,
                                      wiring, definition.wiring_parameter_types());

    try {
        const auto result = static_cast<const WiringTemplate &>(definition)(std::span<const WiringArgument>{arguments});
        return result ? nb::cast(PythonWiringPoint{*result, PythonWiringScope::current()}) : nb::none();
    } catch (nb::python_error &) {
        throw;
    } catch (const TypeResolutionError &error) {
        auto message = std::string{"cannot wire Python graph: "} + error.what() + " for argument '" +
                       std::string{error.subject().text()} + "'";
        if (error.expected() != nullptr && error.supplied() != nullptr) {
            message += " (expected ";
            error.expected()->format_name(message);
            message += ", received ";
            error.supplied()->format_name(message);
            message += ')';
        }
        throw std::invalid_argument{std::move(message)};
    } catch (const WiringError &error) {
        if (python_error_cause(error.cause()) != nullptr) {
            rethrow_python_error(error);
        }
        throw std::invalid_argument{std::string{"cannot wire Python graph: "} + error.what()};
    } catch (const std::exception &error) {
        throw std::invalid_argument{std::string{"cannot wire Python graph: "} + error.what()};
    }
}

} // namespace

const Type &details::python_type_argument(nb::handle value) {
    if (nb::isinstance<TypeDescriptor>(value)) {
        return nb::cast<const TypeDescriptor &>(value).type();
    }
    return scalar_conversions().type_from_python(value);
}

void bind_core(nb::module_ &module) {
    if (!nb::register_module(module)) {
        nb::raise_python_error();
    }

    module.def("version", [] { return std::string{bg::version()}; });
    nb::class_<PythonServiceDefinition>(module, "_PythonServiceDefinition");
    nb::class_<PythonServiceCatalogue>(module, "_ServiceCatalogue").def(nb::init<>());
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
        const auto &size = VariableRegistry::instance().register_owned(
            std::make_unique<SizeVariable>(util::symbol{"SIZE"}));
        const auto &type = TypeRegistry::instance().register_owned(
            std::make_unique<GenericTimeSeriesListType>(*item, size, TimeSeriesGuarantee::ordinary));
        return TypeDescriptor{type};
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
        const Schema *base_schema = nullptr;
        if (base) {
            base_schema = base->schema();
            if (base_schema == nullptr) {
                throw std::invalid_argument{"a Python time-series schema base must be a schema"};
            }
        }

        auto schema_fields = std::vector<SchemaField>{};
        schema_fields.reserve(fields.size());
        for (const auto &[field_name, field_type] : fields) {
            const auto *time_series = type_cast<TimeSeriesType>(&field_type.type());
            if (time_series == nullptr) {
                throw std::invalid_argument{"a Python time-series schema field must have a time-series type"};
            }
            schema_fields.push_back({util::symbol{field_name}, time_series});
        }

        return TypeDescriptor{schema(util::symbol{name}, new_runtime_schema_id(), base_schema, schema_fields)};
    });
    module.def("_time_series_bundle_type", [](const TypeDescriptor &value) {
        const auto *schema = value.schema();
        if (schema == nullptr) {
            throw std::invalid_argument{"TSB[...] requires a TimeSeriesSchema"};
        }
        return TypeDescriptor{time_series_bundle_type(*schema, TimeSeriesGuarantee::ordinary)};
    });
    module.def("_variadic_time_series_bundle_type", [] {
        const auto &schema = TypeRegistry::instance().register_owned(
            std::make_unique<TypeVariableType<SchemaType>>(util::symbol{"TS_SCHEMA"}));
        const auto &type = TypeRegistry::instance().register_owned(
            std::make_unique<GenericTimeSeriesBundleType>(schema, TimeSeriesGuarantee::ordinary));
        return TypeDescriptor{type};
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
                                 std::optional<TypeDescriptor> output, nb::handle function,
                                 bool lift_scalar_output) {
        return PythonGraphDefinition{make_signature(name, parameters, output), function, lift_scalar_output};
    });
    module.def("_make_service", &make_service, "name"_a, "kind"_a, "parameters"_a, "output"_a);
    module.def("_make_service_catalogue", [] { return PythonServiceCatalogue{}; });
    module.def("_wire_service", [](const PythonServiceDefinition &definition, nb::sequence arguments) {
        return definition.wire(arguments);
    });
    module.def("_register_service_impl",
               [](PythonServiceCatalogue &catalogue, nb::handle service, nb::handle implementation,
                  nb::sequence positional, nb::dict named) {
                   const auto &service_definition =
                       nb::cast<const PythonServiceDefinition &>(nb::getattr(service, "_native_definition"));
                   const auto &graph_definition =
                       nb::cast<const PythonGraphDefinition &>(nb::getattr(implementation, "_native_definition"));
                   catalogue.register_impl(service_definition, graph_definition, service, implementation, positional,
                                           named);
               });
    module.def("_wire_node", &wire_node);
    module.def("_wire_graph", &wire_graph);
    nb::class_<EvaluationProfiler::Recorder>(module, "EvaluationProfilerRecorder");

    nb::class_<EvaluationProfiler>(module, "EvaluationProfiler")
        .def(nb::new_([](std::chrono::microseconds node_period, std::size_t retained_node_periods,
                         std::chrono::microseconds outer_period, std::size_t retained_outer_periods) {
                 auto config = EvaluationProfilerConfig{};
                 config.node_period = node_period;
                 config.retained_node_periods = retained_node_periods;
                 config.outer_period = outer_period;
                 config.retained_outer_periods = retained_outer_periods;
                 return new EvaluationProfiler(config);
             }),
             "node_period"_a = std::chrono::minutes{1}, "retained_node_periods"_a = 30,
             "outer_period"_a = std::chrono::seconds{1}, "retained_outer_periods"_a = 24 * 60 * 60)
        .def("recorder", &EvaluationProfiler::recorder, nb::keep_alive<0, 1>())
        .def("root_graph_id", [](const EvaluationProfiler &profiler) -> nb::object {
            const auto id = profiler.root_graph_id();
            if (!id) {
                return nb::none();
            }
            return nb::int_(id->value);
        })
        .def("graph_ids", [](const EvaluationProfiler &profiler) {
            auto result = nb::list{};
            for (const auto id : profiler.graph_ids()) {
                result.append(id.value);
            }
            return result;
        })
        .def("graph", [](const EvaluationProfiler &profiler, std::uint64_t graph_id) -> nb::object {
            const auto *value = profiler.graph(GraphInstanceId{graph_id});
            if (value == nullptr) {
                return nb::none();
            }
            return profiler_graph(*value);
        })
        .def("node_period_starts", [](const EvaluationProfiler &profiler) {
            auto result = nb::list{};
            for (const auto start : profiler.node_period_starts()) {
                result.append(nb::cast(start));
            }
            return result;
        })
        .def("node_periods", [](const EvaluationProfiler &profiler, std::uint64_t graph_id,
                                 EvaluationTime period_start) {
            auto result = nb::list{};
            for (const auto period : profiler.node_periods(GraphInstanceId{graph_id}, period_start)) {
                result.append(profiler_node_period(period));
            }
            return result;
        })
        .def("graph_period", [](const EvaluationProfiler &profiler, std::uint64_t graph_id,
                                EvaluationTime period_start) -> nb::object {
            const auto stats = profiler.graph_period(GraphInstanceId{graph_id}, period_start);
            if (!stats) {
                return nb::none();
            }
            return profiler_stats(*stats);
        })
        .def("node_period", [](const EvaluationProfiler &profiler, std::uint64_t graph_id,
                               std::uint32_t node_index, EvaluationTime period_start) -> nb::object {
            const auto stats = profiler.node_period(GraphInstanceId{graph_id}, node_index, period_start);
            if (!stats) {
                return nb::none();
            }
            return profiler_stats(*stats);
        })
        .def("outer_totals", [](const EvaluationProfiler &profiler) {
            return profiler_cycle_stats(profiler.outer_totals());
        })
        .def("outer_periods", [](const EvaluationProfiler &profiler) {
            auto result = nb::list{};
            for (const auto period : profiler.outer_periods()) {
                result.append(profiler_outer_period(period));
            }
            return result;
        })
        .def("outer_period", [](const EvaluationProfiler &profiler, EvaluationTime period_start) -> nb::object {
            const auto stats = profiler.outer_period(period_start);
            if (!stats) {
                return nb::none();
            }
            return profiler_cycle_stats(*stats);
        }, "period_start"_a)
        .def("report", &EvaluationProfiler::report, "recent_outer_periods"_a = 10);

    module.def("_test_graph", &test_graph, "definition"_a, "input_sequences"_a, "catalogue"_a = nullptr,
               "recorder"_a = nullptr);
    module.def("_run_graph", &run_graph, "definition"_a, "fixed_values"_a, "positional_values"_a,
               "named_values"_a, "catalogue"_a = nullptr, "recorder"_a = nullptr);
    module.def("_register_delta_types", &details::register_delta_types);
    module.def("_scalar_conversions_address", [] { return reinterpret_cast<std::uintptr_t>(&scalar_conversions()); });
    module.def("_register_symbol_type", [](nb::handle python_type) {
        return scalar_conversions().add_python_type(python_type, symbol_type());
    });

    details::bind_operators(module);
    const auto add_operator = details::PythonOperator{operators::add};
    const auto sub_operator = details::PythonOperator{operators::sub};
    const auto mul_operator = details::PythonOperator{operators::mul};
    const auto div_operator = details::PythonOperator{operators::div};
    const auto floordiv_operator = details::PythonOperator{operators::floordiv};
    const auto mod_operator = details::PythonOperator{operators::mod};
    const auto pow_operator = details::PythonOperator{operators::pow};
    const auto round_operator = details::PythonOperator{operators::round};
    const auto pos_operator = details::PythonOperator{operators::pos};
    const auto neg_operator = details::PythonOperator{operators::neg};
    const auto abs_operator = details::PythonOperator{operators::abs};
    const auto eq_operator = details::PythonOperator{operators::eq};
    const auto ne_operator = details::PythonOperator{operators::ne};
    const auto lt_operator = details::PythonOperator{operators::lt};
    const auto le_operator = details::PythonOperator{operators::le};
    const auto gt_operator = details::PythonOperator{operators::gt};
    const auto ge_operator = details::PythonOperator{operators::ge};

    nb::class_<TypeDescriptor>(module, "_Type")
        .def_prop_ro("is_time_series", [](const TypeDescriptor &value) { return value.type().is_time_series(); })
        .def_prop_ro("accepts_absent_scalar", &TypeDescriptor::accepts_absent_scalar);
    nb::class_<PythonWiringPoint>(module, "WiringPoint")
        .def_prop_ro("has_output", &PythonWiringPoint::has_output)
        .def_prop_ro("type_name",
                     [](const PythonWiringPoint &point) {
                         auto result = std::string{};
                         point.type().format_name(result);
                         return result;
                     })
        .def("__bool__", [](const PythonWiringPoint &) -> bool {
            throw nb::type_error("A wiring point has no Python truth value; use 'has_output' for output presence.");
        })
        .def("_as_optional", &PythonWiringPoint::as_optional)
        .def("__getitem__", &details::getitem)
        .def(
            "__add__",
            [add_operator](const PythonWiringPoint &self, nb::handle other) {
                return add_operator.call(nb::cast(self), other);
            },
            nb::is_operator())
        .def("__radd__",
             [add_operator](const PythonWiringPoint &self, nb::handle other) {
                 return add_operator.call(other, nb::cast(self));
             },
             nb::is_operator())
        .def("__sub__",
             [sub_operator](const PythonWiringPoint &self, nb::handle other) {
                 return sub_operator.call(nb::cast(self), other);
             },
             nb::is_operator())
        .def("__rsub__",
             [sub_operator](const PythonWiringPoint &self, nb::handle other) {
                 return sub_operator.call(other, nb::cast(self));
             },
             nb::is_operator())
        .def("__mul__",
             [mul_operator](const PythonWiringPoint &self, nb::handle other) {
                 return mul_operator.call(nb::cast(self), other);
             },
             nb::is_operator())
        .def("__rmul__",
             [mul_operator](const PythonWiringPoint &self, nb::handle other) {
                 return mul_operator.call(other, nb::cast(self));
             },
             nb::is_operator())
        .def("__truediv__",
             [div_operator](const PythonWiringPoint &self, nb::handle other) {
                 return div_operator.call(nb::cast(self), other);
             },
             nb::is_operator())
        .def("__rtruediv__",
             [div_operator](const PythonWiringPoint &self, nb::handle other) {
                 return div_operator.call(other, nb::cast(self));
             },
             nb::is_operator())
        .def("__floordiv__",
             [floordiv_operator](const PythonWiringPoint &self, nb::handle other) {
                 return floordiv_operator.call(nb::cast(self), other);
             },
             nb::is_operator())
        .def("__rfloordiv__",
             [floordiv_operator](const PythonWiringPoint &self, nb::handle other) {
                 return floordiv_operator.call(other, nb::cast(self));
             },
             nb::is_operator())
        .def("__mod__",
             [mod_operator](const PythonWiringPoint &self, nb::handle other) {
                 return mod_operator.call(nb::cast(self), other);
             },
             nb::is_operator())
        .def("__rmod__",
             [mod_operator](const PythonWiringPoint &self, nb::handle other) {
                 return mod_operator.call(other, nb::cast(self));
             },
             nb::is_operator())
        .def("__pow__",
             [pow_operator](const PythonWiringPoint &self, nb::handle other) {
                 return pow_operator.call(nb::cast(self), other);
             },
             nb::is_operator())
        .def("__rpow__",
             [pow_operator](const PythonWiringPoint &self, nb::handle other) {
                 return pow_operator.call(other, nb::cast(self));
             },
             nb::is_operator())
        .def("__pos__", [pos_operator](const PythonWiringPoint &self) { return pos_operator.call(nb::cast(self)); },
             nb::is_operator())
        .def("__neg__", [neg_operator](const PythonWiringPoint &self) { return neg_operator.call(nb::cast(self)); },
             nb::is_operator())
        .def("__abs__", [abs_operator](const PythonWiringPoint &self) { return abs_operator.call(nb::cast(self)); },
             nb::is_operator())
        .def("__round__",
             [round_operator](const PythonWiringPoint &self, nb::object ndigits) {
                 if (ndigits.is_none()) {
                     return round_operator.call(nb::cast(self));
                 }
                 return round_operator.call(nb::cast(self), ndigits);
             },
             "ndigits"_a = nb::none(), nb::is_operator())
        .def("__eq__",
             [eq_operator](const PythonWiringPoint &self, nb::handle other) {
                 return eq_operator.call(nb::cast(self), other);
             },
             nb::is_operator())
        .def("__ne__",
             [ne_operator](const PythonWiringPoint &self, nb::handle other) {
                 return ne_operator.call(nb::cast(self), other);
             },
             nb::is_operator())
        .def("__lt__",
             [lt_operator](const PythonWiringPoint &self, nb::handle other) {
                 return lt_operator.call(nb::cast(self), other);
             },
             nb::is_operator())
        .def("__le__",
             [le_operator](const PythonWiringPoint &self, nb::handle other) {
                 return le_operator.call(nb::cast(self), other);
             },
             nb::is_operator())
        .def("__gt__",
             [gt_operator](const PythonWiringPoint &self, nb::handle other) {
                 return gt_operator.call(nb::cast(self), other);
             },
             nb::is_operator())
        .def("__ge__",
             [ge_operator](const PythonWiringPoint &self, nb::handle other) {
                 return ge_operator.call(nb::cast(self), other);
             },
             nb::is_operator());
    nb::class_<InputView>(module, "InputView")
        .def_prop_ro("valid", &InputView::valid)
        .def_prop_ro("modified", &InputView::modified)
        .def_prop_ro("last_modified_time", &InputView::last_modified_time);
    nb::class_<TSInput, InputView>(module, "TSInput")
        .def_prop_ro("value", &TSInput::value)
        .def_prop_ro("delta_value", &TSInput::delta_value);
    nb::class_<TSSInput, InputView>(module, "TSSInput")
        .def_prop_ro("value", &TSSInput::value)
        .def_prop_ro("delta_value", &TSSInput::delta_value)
        .def("__len__", &TSSInput::size)
        .def("__contains__", &TSSInput::contains)
        .def("__iter__", &TSSInput::iter)
        .def("added", &TSSInput::added)
        .def("removed", &TSSInput::removed)
        .def("was_added", &TSSInput::was_added)
        .def("was_removed", &TSSInput::was_removed);
    nb::class_<TSLInput, InputView>(module, "TSLInput")
        .def_prop_ro("value", &TSLInput::value)
        .def_prop_ro("delta_value", &TSLInput::delta_value)
        .def("__len__", &TSLInput::size)
        .def("__getitem__", &TSLInput::getitem)
        .def("__iter__", &TSLInput::iter)
        .def("keys", &TSLInput::keys)
        .def("values", &TSLInput::values)
        .def("items", &TSLInput::items)
        .def("modified_keys", &TSLInput::modified_keys)
        .def("modified_values", &TSLInput::modified_values)
        .def("modified_items", &TSLInput::modified_items)
        .def("valid_keys", &TSLInput::valid_keys)
        .def("valid_values", &TSLInput::valid_values)
        .def("valid_items", &TSLInput::valid_items);
    nb::class_<TSBInput, InputView>(module, "TSBInput")
        .def_prop_ro("value", &TSBInput::value)
        .def_prop_ro("delta_value", &TSBInput::delta_value)
        .def("__len__", &TSBInput::size)
        .def("__getitem__", &TSBInput::getitem)
        .def("__getattr__", &TSBInput::getattr)
        .def("keys", &TSBInput::keys)
        .def("values", &TSBInput::values)
        .def("items", &TSBInput::items)
        .def("modified_keys", &TSBInput::modified_keys)
        .def("modified_values", &TSBInput::modified_values)
        .def("modified_items", &TSBInput::modified_items)
        .def("valid_keys", &TSBInput::valid_keys)
        .def("valid_values", &TSBInput::valid_values)
        .def("valid_items", &TSBInput::valid_items);
    nb::class_<TSDInput, InputView>(module, "TSDInput")
        .def_prop_ro("value", &TSDInput::value)
        .def_prop_ro("delta_value", &TSDInput::delta_value)
        .def("__len__", &TSDInput::size)
        .def("__getitem__", &TSDInput::getitem)
        .def("__contains__", &TSDInput::contains)
        .def("__iter__", &TSDInput::iter)
        .def("get", &TSDInput::get, "key"_a, "default"_a = nb::none())
        .def_prop_ro("key_set", &TSDInput::key_set)
        .def("keys", &TSDInput::keys)
        .def("values", &TSDInput::values)
        .def("items", &TSDInput::items)
        .def("added_keys", &TSDInput::added_keys)
        .def("added_values", &TSDInput::added_values)
        .def("added_items", &TSDInput::added_items)
        .def("removed_keys", &TSDInput::removed_keys)
        .def("removed_values", &TSDInput::removed_values)
        .def("removed_items", &TSDInput::removed_items)
        .def("modified_keys", &TSDInput::modified_keys)
        .def("modified_values", &TSDInput::modified_values)
        .def("modified_items", &TSDInput::modified_items)
        .def("valid_keys", &TSDInput::valid_keys)
        .def("valid_values", &TSDInput::valid_values)
        .def("valid_items", &TSDInput::valid_items);
    nb::class_<REFInput, InputView>(module, "REFInput")
        .def_prop_ro("value", &REFInput::value)
        .def_prop_ro("delta_value", &REFInput::delta_value);
    bind_input_iterators(module);
    bind_dict_view(module);
    nb::class_<ReferenceValue>(module, "ReferenceValue").def_prop_ro("valid", &ReferenceValue::valid);
    nb::class_<PythonNodeDefinition>(module, "_PythonNodeDefinition");
    nb::class_<PythonGraphDefinition>(module, "_PythonGraphDefinition");

    details::bind_higher_order(module);
}

} // namespace bg::python
