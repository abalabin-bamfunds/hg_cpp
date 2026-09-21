#include <bg/map.hpp>

#include <bg/graph/details/map_node.hpp>
#include <bg/graph/details/nested.hpp>
#include <bg/graph/details/variadic_wiring_operations.hpp>
#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/wiring_arguments.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/map/details/map_plan.hpp>
#include <bg/node/node_template.hpp>
#include <bg/time_series/time_series_type.hpp>
#include <bg/time_series/tsb/details/wiring.hpp>
#include <bg/time_series/tsb/tsb_type.hpp>
#include <bg/type/time_series_bundle_type.hpp>
#include <bg/type/time_series_dict_type.hpp>
#include <bg/type/time_series_reference_type.hpp>
#include <bg/type/time_series_set_type.hpp>
#include <bg/type/time_series_variable.hpp>

#include <algorithm>
#include <stdexcept>
#include <string>
#include <utility>

namespace bg::details {
namespace {

const TimeSeriesType &mapped_dictionary_type(const TimeSeriesDictionaryType &dictionary) {
    const auto &item = dictionary.value_type();
    if (type_cast<TimeSeriesReferenceType>(&item) != nullptr) {
        return dictionary;
    }

    const auto dictionary_guarantee = *dictionary.guarantee().value();
    const auto &reference = time_series_reference_type(
        item, strongest(TimeSeriesGuarantee::final, dictionary_guarantee));
    return time_series_dictionary_type(dictionary.key_type(), reference, dictionary_guarantee);
}

struct PlanBuilder {
    PlanBuilder(const WiringTemplate &function, std::vector<MapSuppliedArgument> positional,
                std::vector<MapSuppliedArgument> named,
                const std::optional<MapArgumentSelector> &key_argument,
                const std::optional<MapArgumentSelector> &key_set_argument,
                bool preserve_named_arguments)
        : function{function}, positional{std::move(positional)}, named{std::move(named)},
          key_argument{key_argument}, key_set_argument{key_set_argument},
          preserve_named_arguments{preserve_named_arguments}, wiring{std::make_shared<GraphWiring>()} {}

    GraphArgument build() {
        resolve_key_type();
        resolve_key_set_argument();
        resolve_key_argument();

        auto graph_arguments = std::vector<WiringArgument>{};
        auto boundaries = std::vector<WiringPoint>{};
        graph_arguments.reserve(positional.size() + named.size() +
                                static_cast<std::size_t>(key_parameter.has_value()));

        auto context = WiringContext{*wiring};
        auto graph_parameter = std::size_t{};
        const auto append_key = [&] {
            if (!key_parameter || graph_parameter != *key_parameter) {
                return;
            }

            const auto &key = time_series_value_type(*key_type, TimeSeriesGuarantee::const_);
            const auto boundary = wire_nested_constant_input(key);
            key_input_node = wired_output_node_index(*wiring, boundary);
            boundaries.push_back(boundary);
            graph_arguments.emplace_back(boundary);
            ++graph_parameter;
        };

        const auto append = [&](MapSuppliedArgument &argument, std::size_t user_index) {
            append_key();
            if (selected_key_set == user_index && is_set(argument.value)) {
                return;
            }

            graph_arguments.push_back(graph_argument(argument, boundaries));
            ++graph_parameter;
        };

        for (auto index = std::size_t{}; index < positional.size(); ++index) {
            append(positional[index], index);
        }
        for (auto index = std::size_t{}; index < named.size(); ++index) {
            append(named[index], positional.size() + index);
        }
        append_key();
        if (key_parameter && graph_parameter <= *key_parameter) {
            throw WiringError{"__key_arg__ selects a graph parameter that is out of range"};
        }

        const auto result = function(std::span<const WiringArgument>{graph_arguments});
        const auto structure = graph_wiring_structure(*wiring, boundaries, result);
        if (result) {
            const auto boundary = wire_nested_output(*result, result->type());
            output_node = wired_output_node_index(*wiring, boundary);
            output_target = &type_cast<TimeSeriesReferenceType>(boundary.type()).target_type();
        }

        append_key_set_parameter();
        const auto output = output_target == nullptr
                                ? nullptr
                                : &time_series_dictionary_type(
                                      *key_type,
                                      time_series_reference_type(*output_target,
                                                                 TimeSeriesGuarantee::deduplicated),
                                      TimeSeriesGuarantee::ordinary);
        auto signature = ResolvedSignature{util::symbol{"map"}, std::move(parameters), output};
        auto graph = MapGraphWiring{.wiring = wiring,
                                    .inputs = std::move(inputs),
                                    .key_input_node = key_input_node,
                                    .output_node = output_node,
                                    .key_set_parameter = key_set_parameter};
        auto plan = std::make_shared<MapPlan>(MapPlan{
            .graph = std::move(graph),
            .outer_signature = std::move(signature),
            .outer_arguments = std::move(outer_arguments),
            .dictionary_key_set_parameter = dictionary_key_set_parameter,
        });
        return GraphArgument{structure, std::move(plan)};
    }

  private:
    static const TimeSeriesType &argument_time_series(const WiringArgument &argument) {
        if (!argument.is_time_series()) {
            throw WiringError{"a map collection argument must be a time series"};
        }
        return static_cast<const TimeSeriesType &>(argument.type()).target_type();
    }

    static bool is_set(const WiringArgument &argument) {
        return type_cast<TimeSeriesSetType>(&argument_time_series(argument)) != nullptr;
    }

    static bool is_dictionary(const WiringArgument &argument) {
        return type_cast<TimeSeriesDictionaryType>(&argument_time_series(argument)) != nullptr;
    }

    void resolve_key_type() {
        const auto inspect = [&](const MapSuppliedArgument &argument) {
            if (argument.pass_through || !argument.value.is_time_series()) {
                return;
            }

            const auto &type = argument_time_series(argument.value);
            const ScalarType *candidate{};
            if (const auto *dictionary = type_cast<TimeSeriesDictionaryType>(&type)) {
                candidate = &dictionary->key_type();
            } else if (const auto *set = type_cast<TimeSeriesSetType>(&type)) {
                candidate = &set->item_type();
            } else {
                return;
            }

            if (key_type != nullptr && key_type != candidate) {
                throw WiringError{"all map TSD and TSS arguments must have the same key type"};
            }
            key_type = candidate;
        };

        for (const auto &argument : positional) {
            inspect(argument);
        }
        for (const auto &argument : named) {
            inspect(argument);
        }
        if (key_type == nullptr) {
            throw WiringError{"a map call requires a TSD or TSS argument"};
        }
        if (!key_type->is_resolved()) {
            throw WiringError{"a map call requires a concrete key type"};
        }
    }

    void resolve_key_set_argument() {
        if (key_set_argument) {
            selected_key_set = *selected_map_argument_index(key_set_argument, positional, named);
        } else {
            const auto inspect = [&](const MapSuppliedArgument &argument, std::size_t index) {
                if (argument.pass_through || !argument.value.is_time_series() || !is_set(argument.value)) {
                    return;
                }
                if (selected_key_set) {
                    throw WiringError{"a map call with multiple TSS arguments requires __key_set_arg__"};
                }
                selected_key_set = index;
            };

            for (auto index = std::size_t{}; index < positional.size(); ++index) {
                inspect(positional[index], index);
            }
            for (auto index = std::size_t{}; index < named.size(); ++index) {
                inspect(named[index], positional.size() + index);
            }
        }
        if (!selected_key_set) {
            return;
        }

        auto &argument = supplied(*selected_key_set);
        if (argument.pass_through || (!is_set(argument.value) && !is_dictionary(argument.value))) {
            throw WiringError{"__key_set_arg__ must select a mapped TSD or a TSS"};
        }

        const auto &selected_type = argument_time_series(argument.value);
        const auto &selected_key = type_cast<TimeSeriesDictionaryType>(&selected_type) != nullptr
                                       ? type_cast<TimeSeriesDictionaryType>(selected_type).key_type()
                                       : type_cast<TimeSeriesSetType>(selected_type).item_type();
        if (&selected_key != key_type) {
            throw WiringError{"the selected map key set has a different key type"};
        }
    }

    void resolve_key_argument() {
        if (key_argument) {
            if (const auto index = key_argument->index()) {
                key_parameter = *index;
                return;
            }

            const auto name = *key_argument->name();
            for (auto index = std::size_t{}; index < function.signature().parameter_count(); ++index) {
                if (function.signature().parameter(index).name == name) {
                    key_parameter = index;
                    return;
                }
            }
            throw WiringError{"__key_arg__ names an unknown graph parameter"};
        }

        if (function.signature().parameter_count() != 0 &&
            function.signature().parameter(0).name.text() == "key") {
            key_parameter = 0;
        }
    }

    MapSuppliedArgument &supplied(std::size_t index) {
        return index < positional.size() ? positional[index] : named[index - positional.size()];
    }

    WiringArgument graph_argument(MapSuppliedArgument &supplied_argument,
                                  std::vector<WiringPoint> &boundaries) {
        auto argument = wiring_argument_access::borrow(supplied_argument.value);
        if (!preserve_named_arguments) {
            wiring_argument_access::clear_name(argument);
        }
        if (!argument.is_time_series()) {
            return argument;
        }

        const auto &type = argument_time_series(argument);
        const auto *dictionary = type_cast<TimeSeriesDictionaryType>(&type);
        const auto mapped = dictionary != nullptr && !supplied_argument.pass_through;
        const auto &inner_type = mapped ? dictionary->value_type().target_type() : type;
        const auto boundary = wire_nested_input(inner_type);
        boundaries.push_back(boundary);

        auto graph_argument = WiringArgument{boundary};
        if (const auto name = argument.name()) {
            wiring_argument_access::set_variadic_name(graph_argument, *name, VariadicMode::none);
        }

        const auto parameter_index = parameters.size();
        const auto &outer_type = mapped
                                     ? mapped_dictionary_type(*dictionary)
                                     : static_cast<const TimeSeriesType &>(time_series_reference_type(
                                           type, TimeSeriesGuarantee::ordinary));
        parameters.push_back({argument_name(supplied_argument.value, parameter_index), &outer_type, active,
                              mapped ? required : optional, false, VariadicMode::none});
        outer_arguments.push_back(wiring_argument_access::borrow(supplied_argument.value));
        wiring_argument_access::clear_name(outer_arguments.back());
        inputs.push_back({mapped ? MapInputKind::mapped : MapInputKind::pass_through,
                          parameter_index, wired_output_node_index(*wiring, boundary)});
        return graph_argument;
    }

    util::symbol argument_name(const WiringArgument &argument, std::size_t index) const {
        return argument.name().value_or(util::symbol{std::to_string(index)});
    }

    void append_key_set_parameter() {
        if (!selected_key_set) {
            return;
        }

        auto &selected = supplied(*selected_key_set);
        const auto &type = argument_time_series(selected.value);
        auto key_set_argument = selected.value.time_series();
        const TimeSeriesType *key_set_type = &type;
        if (const auto *dictionary = type_cast<TimeSeriesDictionaryType>(&type)) {
            key_set_type = &dictionary->key_set_type();
            dictionary_key_set_parameter = parameters.size();
        }

        key_set_parameter = parameters.size();
        parameters.push_back({util::symbol{"__key_set__"}, key_set_type, active, required, false,
                              VariadicMode::none});
        outer_arguments.emplace_back(key_set_argument);
    }

    const WiringTemplate &function;
    std::vector<MapSuppliedArgument> positional;
    std::vector<MapSuppliedArgument> named;
    const ScalarType *key_type{};
    const std::optional<MapArgumentSelector> &key_argument;
    const std::optional<MapArgumentSelector> &key_set_argument;
    bool preserve_named_arguments;
    std::shared_ptr<GraphWiring> wiring;
    std::vector<ResolvedParameter> parameters;
    std::vector<WiringArgument> outer_arguments;
    std::vector<MapInputBoundary> inputs;
    std::optional<std::size_t> selected_key_set;
    std::optional<std::size_t> key_parameter;
    std::optional<std::size_t> key_input_node;
    std::optional<std::size_t> output_node;
    std::optional<std::size_t> key_set_parameter;
    std::optional<std::size_t> dictionary_key_set_parameter;
    const TimeSeriesType *output_target{};
};

} // namespace

std::optional<std::size_t>
selected_map_argument_index(const std::optional<MapArgumentSelector> &selector,
                            const std::vector<MapSuppliedArgument> &positional,
                            const std::vector<MapSuppliedArgument> &named) {
    if (!selector) {
        return std::nullopt;
    }
    if (const auto index = selector->index()) {
        if (*index >= positional.size() + named.size()) {
            throw WiringError{"a map argument selector index is out of range"};
        }
        return *index;
    }

    const auto name = *selector->name();
    const auto found = std::ranges::find_if(named, [&](const auto &argument) {
        return argument.value.name() == name;
    });
    if (found == named.end()) {
        throw WiringError{"a map argument selector names an unknown argument"};
    }
    return positional.size() + static_cast<std::size_t>(found - named.begin());
}

GraphArgument build_map_plan(const WiringTemplate &function,
                             std::vector<MapSuppliedArgument> positional,
                             std::vector<MapSuppliedArgument> named,
                             const std::optional<MapArgumentSelector> &key_argument,
                             const std::optional<MapArgumentSelector> &key_set_argument,
                             bool preserve_named_arguments) {
    return PlanBuilder{function, std::move(positional), std::move(named), key_argument,
                       key_set_argument, preserve_named_arguments}
        .build();
}

const MapPlan &MapPlan::from(const GraphArgument &argument) {
    const auto plan = graph_argument_access::payload<MapPlan>(argument);
    if (!plan) {
        throw WiringError{"the default map overload requires a map graph argument"};
    }
    return *plan;
}

std::optional<WiringPoint> MapPlan::wire() const {
    auto node_template = make_map_node(graph, outer_signature);
    auto &outer = WiringContext::current().wiring();
    const auto &owned = outer.own_node_template(std::move(node_template));

    auto arguments = outer_arguments;
    if (dictionary_key_set_parameter) {
        auto &key_set = arguments[*dictionary_key_set_parameter];
        const auto materialized = materialize_wiring_point(
            key_set.time_series(), key_set.time_series().type());
        const auto &key_set_type = static_cast<const TimeSeriesType &>(
            *outer_signature.parameters[*dictionary_key_set_parameter].type);
        key_set = WiringArgument{
            wiring_point_for_item(materialized, key_set_item_id, key_set_type)};
    }

    return owned(std::span<const WiringArgument>{arguments});
}

} // namespace bg::details

namespace bg {
namespace {

Signature default_map_signature(bool output) {
    auto parameters = std::vector<Parameter>{
        {util::symbol{"function"}, &GraphArgument::type()},
        {util::symbol{"args"}, &TSB<details::MAP_POSITIONAL>::type(), active, required, false,
         details::VariadicMode::positional,
         details::variadic_wiring_operations<TSB<details::MAP_POSITIONAL>>()},
        {util::symbol{"kwargs"}, &TSB<details::MAP_NAMED>::type(), active, required, false,
         details::VariadicMode::named,
         details::variadic_wiring_operations<TSB<details::MAP_NAMED>>()},
    };
    if (!output) {
        return {util::symbol{"default_map"}, std::move(parameters)};
    }

    const auto &result = details::TimeSeriesVariableInstance<TS_T>::get();
    auto resolvers = std::vector<VariableResolver>{VariableResolver{
        &result,
        [](const TypeMap &, const WiringArguments &arguments) -> TypeMapValue {
            const auto *function = arguments.graph(0);
            if (function == nullptr) {
                return static_cast<const Type *>(nullptr);
            }
            return static_cast<const Type *>(details::MapPlan::from(*function).outer_signature.output);
        }}};
    return {util::symbol{"default_map"}, std::move(parameters), result, std::move(resolvers)};
}

class DefaultValueMapTemplate final : public WiringTemplate {
  public:
    DefaultValueMapTemplate() : signature_{default_map_signature(true)} {}

    const Signature &signature() const noexcept final { return signature_; }

  private:
    std::optional<WiringPoint> wire(GraphWiring &, ResolvedSignature, const TypeMap &,
                                    std::span<const WiringArgument> arguments) const final {
        return details::MapPlan::from(arguments.front().graph()).wire();
    }

    Signature signature_;
};

class DefaultSinkMapTemplate final : public WiringTemplate {
  public:
    DefaultSinkMapTemplate() : signature_{default_map_signature(false)} {}

    const Signature &signature() const noexcept final { return signature_; }

    RequirementResult requirement(const WiringArguments &arguments) const final {
        const auto *function = arguments.graph(0);
        return function != nullptr && details::MapPlan::from(*function).outer_signature.output == nullptr
                   ? accept
                   : reject("the mapped graph produces an output");
    }

  private:
    std::optional<WiringPoint> wire(GraphWiring &, ResolvedSignature, const TypeMap &,
                                    std::span<const WiringArgument> arguments) const final {
        return details::MapPlan::from(arguments.front().graph()).wire();
    }

    Signature signature_;
};

} // namespace

operator_<auto(GRAPH_T, TSB<details::MAP_POSITIONAL>, TSB<details::MAP_NAMED>) -> TS_T,
          details::map_operator_implementation>
    map{"map", {"graph", "args", "kwargs"}};

namespace {

const auto register_default_map_overloads = [] {
    map.overload(DefaultValueMapTemplate{});
    map.overload(DefaultSinkMapTemplate{});
    return true;
}();

} // namespace

} // namespace bg
