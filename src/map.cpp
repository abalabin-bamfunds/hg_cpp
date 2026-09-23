#include <bg/map.hpp>

#include <bg/graph/details/map_node.hpp>
#include <bg/graph/details/nested.hpp>
#include <bg/graph/details/variadic_wiring_operations.hpp>
#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/wiring_arguments.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/map/details/default_map_wiring.hpp>
#include <bg/node/node_template.hpp>
#include <bg/scalar/type.hpp>
#include <bg/scalar/value.hpp>
#include <bg/time_series/time_series_type.hpp>
#include <bg/time_series/tsb/details/wiring.hpp>
#include <bg/time_series/tsb/tsb_type.hpp>
#include <bg/type/time_series_bundle_type.hpp>
#include <bg/type/time_series_dict_type.hpp>
#include <bg/type/time_series_reference_type.hpp>
#include <bg/type/time_series_set_type.hpp>
#include <bg/type/time_series_variable.hpp>

#include <algorithm>
#include <ranges>
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

class MapCallResolver {
  public:
    explicit MapCallResolver(std::span<const MapArgument> arguments) : arguments_{arguments} {}

    ResolvedMapCall resolve() {
        split_arguments();
        resolve_key_type();
        resolve_key_set();
        return std::move(result_);
    }

  private:
    static const TimeSeriesType *collection_type(const MapArgument &argument) {
        if (argument.pass_through || !argument.value.is_time_series()) {
            return nullptr;
        }

        return &static_cast<const TimeSeriesType &>(argument.value.type()).target_type();
    }

    static MapArgumentSelector selector(const WiringArgument &argument) {
        if (!argument.is_scalar() || !argument.present()) {
            throw WiringError{"a map argument selector must be a present scalar"};
        }

        const auto value = argument.scalar();
        if (&value.type() == &int64_type()) {
            return MapArgumentSelector{scalar_cast<std::int64_t>(value).value()};
        }
        if (&value.type() == &string_type()) {
            return MapArgumentSelector{scalar_cast<string>(value).value()};
        }
        if (&value.type() == &symbol_type()) {
            return MapArgumentSelector{scalar_cast<util::symbol>(value).value()};
        }

        throw WiringError{"a map argument selector must be an integer or a name"};
    }

    void split_arguments() {
        result_.positional.reserve(arguments_.size());
        result_.named.reserve(arguments_.size());

        const auto key_argument_name = util::symbol{map_key_argument_name.view()};
        const auto key_set_argument_name = util::symbol{map_key_set_argument_name.view()};
        for (const auto &argument : arguments_) {
            const auto name = argument.value.name();
            const auto is_key_argument = name == key_argument_name;
            const auto is_key_set_argument = name == key_set_argument_name;
            if (is_key_argument || is_key_set_argument) {
                add_option(argument, is_key_argument);
            } else if (name) {
                result_.named.push_back(argument);
            } else {
                result_.positional.push_back(argument);
            }
        }
    }

    void add_option(const MapArgument &argument, bool is_key_argument) {
        if (argument.pass_through) {
            throw WiringError{"a map option cannot be pass-through"};
        }

        auto &destination = is_key_argument ? result_.key_argument : key_set_argument_;
        if (destination) {
            throw WiringError{is_key_argument ? "a map call accepts one __key_arg__ option"
                                              : "a map call accepts one __key_set_arg__ option"};
        }

        destination = selector(argument.value);
        result_.options.push_back(argument.value);
    }

    void resolve_key_type() {
        const auto inspect = [&](const MapArgument &argument) {
            const auto *type = collection_type(argument);
            if (type == nullptr) {
                return;
            }

            const ScalarType *candidate{};
            if (const auto *dictionary = type_cast<TimeSeriesDictionaryType>(type)) {
                candidate = &dictionary->key_type();
            } else if (const auto *set = type_cast<TimeSeriesSetType>(type)) {
                candidate = &set->item_type();
            } else {
                return;
            }

            if (result_.key_type != nullptr && result_.key_type != candidate) {
                throw WiringError{"all map TSD and TSS arguments must have the same key type"};
            }
            result_.key_type = candidate;
        };

        for (const auto &argument : result_.positional) {
            inspect(argument);
        }
        for (const auto &argument : result_.named) {
            inspect(argument);
        }

        if (result_.key_type == nullptr) {
            throw WiringError{"a map call requires a TSD or TSS argument"};
        }
        if (!result_.key_type->is_resolved()) {
            throw WiringError{"a map call requires a concrete key type"};
        }
    }

    void resolve_key_set() {
        if (key_set_argument_) {
            result_.selected_key_set = selected_argument(*key_set_argument_);
        } else {
            infer_key_set();
        }
        if (!result_.selected_key_set) {
            return;
        }

        const auto &argument = supplied(*result_.selected_key_set);
        const auto *type = collection_type(argument);
        const auto *dictionary = type == nullptr ? nullptr : type_cast<TimeSeriesDictionaryType>(type);
        const auto *set = type == nullptr ? nullptr : type_cast<TimeSeriesSetType>(type);
        if (dictionary == nullptr && set == nullptr) {
            throw WiringError{"__key_set_arg__ must select a mapped TSD or a TSS"};
        }

        const auto &selected_key = dictionary != nullptr ? dictionary->key_type() : set->item_type();
        if (&selected_key != result_.key_type) {
            throw WiringError{"the selected map key set has a different key type"};
        }
        result_.omit_selected_key_set_argument = set != nullptr;
    }

    void infer_key_set() {
        const auto inspect = [&](const MapArgument &argument, std::size_t index) {
            const auto *type = collection_type(argument);
            if (type == nullptr || type_cast<TimeSeriesSetType>(type) == nullptr) {
                return;
            }
            if (result_.selected_key_set) {
                throw WiringError{"a map call with multiple TSS arguments requires __key_set_arg__"};
            }
            result_.selected_key_set = index;
        };

        for (auto index = std::size_t{}; index < result_.positional.size(); ++index) {
            inspect(result_.positional[index], index);
        }
        for (auto index = std::size_t{}; index < result_.named.size(); ++index) {
            inspect(result_.named[index], result_.positional.size() + index);
        }
    }

    std::size_t selected_argument(const MapArgumentSelector &selected) const {
        if (const auto index = selected.index()) {
            if (*index >= result_.positional.size() + result_.named.size()) {
                throw WiringError{"a map argument selector index is out of range"};
            }
            return *index;
        }

        const auto name = *selected.name();
        const auto found = std::ranges::find_if(result_.named, [&](const auto &argument) {
            return argument.value.name() == name;
        });
        if (found == result_.named.end()) {
            throw WiringError{"a map argument selector names an unknown argument"};
        }
        return result_.positional.size() + static_cast<std::size_t>(found - result_.named.begin());
    }

    const MapArgument &supplied(std::size_t index) const {
        return index < result_.positional.size() ? result_.positional[index]
                                                 : result_.named[index - result_.positional.size()];
    }

    std::span<const MapArgument> arguments_;
    ResolvedMapCall result_;
    std::optional<MapArgumentSelector> key_set_argument_;
};

struct MapGraphArgumentBuilder {
    MapGraphArgumentBuilder(const WiringTemplate &function, ResolvedMapCall call,
                            bool preserve_named_arguments)
        : function{function}, call{std::move(call)},
          preserve_named_arguments{preserve_named_arguments}, wiring{std::make_shared<GraphWiring>()} {}

    GraphArgument build() {
        resolve_key_argument();

        auto graph_arguments = std::vector<WiringArgument>{};
        auto boundaries = std::vector<WiringPoint>{};
        graph_arguments.reserve(call.positional.size() + call.named.size() +
                                static_cast<std::size_t>(key_parameter.has_value()));

        auto context = WiringContext{*wiring};
        auto graph_parameter = std::size_t{};
        const auto append_key = [&] {
            if (!key_parameter || graph_parameter != *key_parameter) {
                return;
            }

            const auto &key = time_series_value_type(*call.key_type, TimeSeriesGuarantee::const_);
            const auto boundary = wire_nested_constant_input(key);
            key_input_node = wired_output_node_index(*wiring, boundary);
            boundaries.push_back(boundary);
            graph_arguments.emplace_back(boundary);
            ++graph_parameter;
        };

        const auto append = [&](MapArgument &argument, std::size_t user_index) {
            append_key();
            if (call.selected_key_set == user_index && call.omit_selected_key_set_argument) {
                return;
            }

            graph_arguments.push_back(graph_argument(argument, boundaries));
            ++graph_parameter;
        };

        for (auto index = std::size_t{}; index < call.positional.size(); ++index) {
            append(call.positional[index], index);
        }
        for (auto index = std::size_t{}; index < call.named.size(); ++index) {
            append(call.named[index], call.positional.size() + index);
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
                                      *call.key_type,
                                      time_series_reference_type(*output_target,
                                                                 TimeSeriesGuarantee::deduplicated),
                                      TimeSeriesGuarantee::ordinary);
        auto signature = ResolvedSignature{util::symbol{"map"}, std::move(parameters), output};
        auto graph = MapGraphWiring{.wiring = wiring,
                                    .inputs = std::move(inputs),
                                    .key_input_node = key_input_node,
                                    .output_node = output_node,
                                    .key_set_parameter = key_set_parameter};
        auto default_wiring = std::make_shared<DefaultMapWiring>(DefaultMapWiring{
            .graph = std::move(graph),
            .outer_signature = std::move(signature),
            .outer_arguments = std::move(outer_arguments),
            .dictionary_key_set_parameter = dictionary_key_set_parameter,
        });
        return GraphArgument{structure, std::move(default_wiring)};
    }

  private:
    static const TimeSeriesType &argument_time_series(const WiringArgument &argument) {
        if (!argument.is_time_series()) {
            throw WiringError{"a map collection argument must be a time series"};
        }
        return static_cast<const TimeSeriesType &>(argument.type()).target_type();
    }

    void resolve_key_argument() {
        if (call.key_argument) {
            if (const auto index = call.key_argument->index()) {
                key_parameter = *index;
                return;
            }

            const auto name = *call.key_argument->name();
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

    MapArgument &supplied(std::size_t index) {
        return index < call.positional.size() ? call.positional[index]
                                              : call.named[index - call.positional.size()];
    }

    WiringArgument graph_argument(MapArgument &supplied_argument,
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
        if (!call.selected_key_set) {
            return;
        }

        auto &selected = supplied(*call.selected_key_set);
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
    ResolvedMapCall call;
    bool preserve_named_arguments;
    std::shared_ptr<GraphWiring> wiring;
    std::vector<ResolvedParameter> parameters;
    std::vector<WiringArgument> outer_arguments;
    std::vector<MapInputBoundary> inputs;
    std::optional<std::size_t> key_parameter;
    std::optional<std::size_t> key_input_node;
    std::optional<std::size_t> output_node;
    std::optional<std::size_t> key_set_parameter;
    std::optional<std::size_t> dictionary_key_set_parameter;
    const TimeSeriesType *output_target{};
};

} // namespace

ResolvedMapCall resolve_map_call(std::span<const MapArgument> arguments) {
    return MapCallResolver{arguments}.resolve();
}

GraphArgument build_map_graph_argument(const WiringTemplate &function, ResolvedMapCall call,
                                       bool preserve_named_arguments) {
    return MapGraphArgumentBuilder{function, std::move(call), preserve_named_arguments}
        .build();
}

const DefaultMapWiring &DefaultMapWiring::from(const GraphArgument &argument) {
    const auto wiring = graph_argument_access::payload<DefaultMapWiring>(argument);
    if (!wiring) {
        throw WiringError{"the default map overload requires a map graph argument"};
    }
    return *wiring;
}

std::optional<WiringPoint> DefaultMapWiring::wire() const {
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
            return static_cast<const Type *>(
                details::DefaultMapWiring::from(*function).outer_signature.output);
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
        return details::DefaultMapWiring::from(arguments.front().graph()).wire();
    }

    Signature signature_;
};

class DefaultSinkMapTemplate final : public WiringTemplate {
  public:
    DefaultSinkMapTemplate() : signature_{default_map_signature(false)} {}

    const Signature &signature() const noexcept final { return signature_; }

    RequirementResult requirement(const WiringArguments &arguments) const final {
        const auto *function = arguments.graph(0);
        return function != nullptr &&
                       details::DefaultMapWiring::from(*function).outer_signature.output == nullptr
                   ? accept
                   : reject("the mapped graph produces an output");
    }

  private:
    std::optional<WiringPoint> wire(GraphWiring &, ResolvedSignature, const TypeMap &,
                                    std::span<const WiringArgument> arguments) const final {
        return details::DefaultMapWiring::from(arguments.front().graph()).wire();
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
