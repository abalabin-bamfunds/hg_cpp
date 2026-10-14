#include <bg/map.hpp>

#include <bg/graph/details/nested.hpp>
#include <bg/graph/details/variadic_wiring_operations.hpp>
#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/wiring_arguments.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/map/details/default_map_wiring.hpp>
#include <bg/map/details/map_node.hpp>
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

class MapCallResolver {
  public:
    MapCallResolver(std::string_view operation, std::span<const MapArgument> arguments)
        : operation_{operation}, arguments_{arguments} {}

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

    MapArgumentSelector selector(const WiringArgument &argument) const {
        if (!argument.is_scalar() || !argument.present()) {
            throw WiringError{std::string{"a "} + operation_ + " argument selector must be a present scalar"};
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

        throw WiringError{std::string{"a "} + operation_ + " argument selector must be an integer or a name"};
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
            throw WiringError{std::string{"a "} + operation_ + " option cannot be pass-through"};
        }

        auto &destination = is_key_argument ? result_.key_argument : key_set_argument_;
        if (destination) {
            throw WiringError{std::string{"a "} + operation_ +
                              (is_key_argument ? " call accepts one __key_arg__ option"
                                               : " call accepts one __key_set_arg__ option")};
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
                throw WiringError{std::string{"all "} + operation_ +
                                  " TSD and TSS arguments must have the same key type"};
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
            throw WiringError{std::string{"a "} + operation_ + " call requires a TSD or TSS argument"};
        }
        if (!result_.key_type->is_resolved()) {
            throw WiringError{std::string{"a "} + operation_ + " call requires a concrete key type"};
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
            throw WiringError{std::string{"the selected "} + operation_ + " key set has a different key type"};
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
                throw WiringError{std::string{"a "} + operation_ +
                                  " call with multiple TSS arguments requires __key_set_arg__"};
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
                throw WiringError{std::string{"a "} + operation_ + " argument selector index is out of range"};
            }
            return *index;
        }

        const auto name = *selected.name();
        const auto found =
            std::ranges::find_if(result_.named, [&](const auto &argument) { return argument.value.name() == name; });
        if (found == result_.named.end()) {
            throw WiringError{std::string{"a "} + operation_ + " argument selector names an unknown argument"};
        }
        return result_.positional.size() + static_cast<std::size_t>(found - result_.named.begin());
    }

    const MapArgument &supplied(std::size_t index) const {
        return index < result_.positional.size() ? result_.positional[index]
                                                 : result_.named[index - result_.positional.size()];
    }

    std::string operation_;
    std::span<const MapArgument> arguments_;
    ResolvedMapCall result_;
    std::optional<MapArgumentSelector> key_set_argument_;
};

} // namespace

ResolvedMapCall resolve_map_call(std::span<const MapArgument> arguments) {
    return resolve_map_call("map", arguments);
}

ResolvedMapCall resolve_map_call(std::string_view operation, std::span<const MapArgument> arguments) {
    return MapCallResolver{operation, arguments}.resolve();
}

GraphArgument build_map_graph_argument(const WiringTemplate &function, ResolvedMapCall call,
                                       bool preserve_named_arguments, const TypeMap &type_map) {
    auto built = build_map_graph_argument_wiring(util::symbol{"map"}, function, std::move(call),
                                                 preserve_named_arguments, type_map);
    auto structure = built.structure;
    auto default_wiring = std::make_shared<DefaultMapWiring>(DefaultMapWiring{
        .graph = std::move(built.graph),
        .outer_signature = std::move(built.outer_signature),
        .outer_arguments = std::move(built.outer_arguments),
        .dictionary_key_set_parameter = built.dictionary_key_set_parameter,
    });
    return GraphArgument{std::move(structure), std::move(default_wiring)};
}

const DefaultMapWiring &DefaultMapWiring::from(const GraphArgument &argument) {
    const auto wiring = argument.payload<DefaultMapWiring>();
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
        const auto &key_set_type =
            static_cast<const TimeSeriesType &>(*outer_signature.parameters[*dictionary_key_set_parameter].type);
        key_set = WiringArgument{outer.output_item(key_set.time_series(), key_set_item_id, key_set_type)};
    }

    auto call = outer.invoke_node(owned, arguments);
    outer.register_nested_service_consumers(*graph.wiring, call.identity);
    return call.output;
}

} // namespace bg::details

namespace bg {
namespace {

Signature default_map_signature(bool output) {
    auto parameters = std::vector<Parameter>{
        {util::symbol{"function"}, &GraphArgument::type()},
        {util::symbol{"args"}, &TSB<details::MAP_POSITIONAL>::type(), active, optional, false,
         details::VariadicMode::positional, details::variadic_wiring_operations<TSB<details::MAP_POSITIONAL>>()},
        {util::symbol{"kwargs"}, &TSB<details::MAP_NAMED>::type(), active, optional, false,
         details::VariadicMode::named, details::variadic_wiring_operations<TSB<details::MAP_NAMED>>()},
    };
    if (!output) {
        return {util::symbol{"default_map"}, std::move(parameters)};
    }

    const auto &result = details::TimeSeriesVariableInstance<TS_T>::get();
    auto resolvers = std::vector<VariableResolver>{VariableResolver{
        &result, [](const TypeMap &, const WiringArguments &arguments) -> TypeMapValue {
            const auto *function = arguments.graph(0);
            if (function == nullptr) {
                return static_cast<const Type *>(nullptr);
            }
            return static_cast<const Type *>(details::DefaultMapWiring::from(*function).outer_signature.output);
        }}};
    return {util::symbol{"default_map"}, std::move(parameters), result, std::move(resolvers)};
}

class DefaultValueMapTemplate final : public WiringTemplate {
  public:
    DefaultValueMapTemplate() : signature_{default_map_signature(true)} {}

    const Signature &signature() const noexcept final { return signature_; }

  private:
    std::optional<WiringPoint> wire(GraphWiring &, ResolvedSignature, const WiringArguments &,
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
        return function != nullptr && details::DefaultMapWiring::from(*function).outer_signature.output == nullptr
                   ? accept
                   : reject("the mapped graph produces an output");
    }

  private:
    std::optional<WiringPoint> wire(GraphWiring &, ResolvedSignature, const WiringArguments &,
                                    std::span<const WiringArgument> arguments) const final {
        return details::DefaultMapWiring::from(arguments.front().graph()).wire();
    }

    Signature signature_;
};

} // namespace

operator_<auto(GRAPH_T, TSB<details::MAP_POSITIONAL>, TSB<details::MAP_NAMED>)->TS_T,
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
