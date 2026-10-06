#include <bg/reduce.hpp>

#include <bg/graph/details/variadic_wiring_operations.hpp>
#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/graph/wiring_template.hpp>
#include <bg/node/node_template.hpp>
#include <bg/reduce/details/default_reduce_wiring.hpp>
#include <bg/reduce/details/reduce_node.hpp>
#include <bg/time_series/time_series_type.hpp>
#include <bg/time_series/tsb/details/wiring.hpp>
#include <bg/type/time_series_dict_type.hpp>
#include <bg/type/time_series_reference_type.hpp>
#include <bg/type/time_series_value_type.hpp>

#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

namespace bg::details {
namespace {

const TimeSeriesType &dictionary_type(const WiringArgument &argument) {
    if (!argument.is_time_series()) {
        throw WiringError{"reduce requires a TSD time-series argument"};
    }
    return static_cast<const TimeSeriesType &>(argument.type()).target_type();
}

const TimeSeriesType &mapped_dictionary_type(const TimeSeriesDictionaryType &dictionary) {
    if (type_cast<TimeSeriesReferenceType>(&dictionary.value_type()) != nullptr) {
        return dictionary;
    }

    const auto guarantee = *dictionary.guarantee().value();
    const auto &item_reference =
        time_series_reference_type(dictionary.value_type(), strongest(TimeSeriesGuarantee::final, guarantee));
    return time_series_dictionary_type(dictionary.key_type(), item_reference, guarantee);
}

} // namespace

GraphArgument build_reduce_graph_argument(const WiringTemplate &function, const WiringArgument &dictionary,
                                          const std::optional<WiringArgument> &default_value,
                                          const TypeMap &type_map) {
    const auto *dictionary_type_value = type_cast<TimeSeriesDictionaryType>(&dictionary_type(dictionary));
    if (dictionary_type_value == nullptr) {
        throw WiringError{"reduce requires a TSD time-series argument"};
    }

    const auto &item_type = dictionary_type_value->value_type().target_type();
    auto built = build_reduce_graph_argument_wiring(function, item_type, type_map);
    const auto *output_value_type = type_cast<TimeSeriesValueType>(&item_type);
    if (output_value_type == nullptr) {
        throw WiringError{"reduce graph output must have a scalar-valued time series type"};
    }

    auto parameters = std::vector<ResolvedParameter>{};
    auto outer_arguments = std::vector<WiringArgument>{};
    const auto value_dictionary_parameter = std::uint32_t{};
    const auto reference_dictionary_parameter = std::uint32_t{1};
    const auto &value_dictionary_type = time_series_dictionary_type(
        dictionary_type_value->key_type(), item_type, *dictionary_type_value->guarantee().value());
    parameters.push_back({util::symbol{"values"}, &value_dictionary_type, active, optional});
    parameters.push_back({util::symbol{"references"}, &mapped_dictionary_type(*dictionary_type_value), active,
                          optional});
    outer_arguments.push_back(dictionary);
    outer_arguments.back().clear_name();
    outer_arguments.push_back(dictionary);
    outer_arguments.back().clear_name();

    auto default_parameter = std::optional<std::uint32_t>{};
    if (default_value) {
        if (!default_value->is_scalar() || !output_value_type->value_type().accepts(default_value->type())) {
            throw WiringError{"reduce default must be a scalar value matching the TSD item type"};
        }
        default_parameter = static_cast<std::uint32_t>(parameters.size());
        parameters.push_back({util::symbol{"default"}, &output_value_type->value_type(), active, optional, true});
        outer_arguments.push_back(*default_value);
        outer_arguments.back().clear_name();
    }

    for (const auto &requirement : built.graph.wiring->inherited_requirements()) {
        parameters.push_back({requirement.name, &requirement.boundary_input_type(), passive, optional});
        outer_arguments.emplace_back(requirement.provider);
    }

    const auto &output_type = time_series_value_type(output_value_type->value_type(), TimeSeriesGuarantee::ordinary);
    auto outer_signature = ResolvedSignature{util::symbol{"reduce"}, std::move(parameters), &output_type};
    auto structure = built.structure;
    auto payload = std::make_shared<DefaultReduceWiring>(DefaultReduceWiring{
        .graph = std::move(built.graph),
        .outer_signature = std::move(outer_signature),
        .outer_arguments = std::move(outer_arguments),
        .value_dictionary_parameter = value_dictionary_parameter,
        .reference_dictionary_parameter = reference_dictionary_parameter,
        .default_parameter = default_parameter,
    });
    return GraphArgument{std::move(structure), std::move(payload)};
}

const DefaultReduceWiring &DefaultReduceWiring::from(const GraphArgument &argument) {
    const auto wiring = argument.payload<DefaultReduceWiring>();
    if (wiring == nullptr) {
        throw WiringError{"the default reduce overload requires a reduce graph argument"};
    }
    return *wiring;
}

std::optional<WiringPoint> DefaultReduceWiring::wire() const {
    auto node_template = make_reduce_node(graph, outer_signature, value_dictionary_parameter,
                                          reference_dictionary_parameter, default_parameter);
    auto &outer = WiringContext::current().wiring();
    const auto &owned = outer.own_node_template(std::move(node_template));
    const auto invocation = outer.invoke_node(owned, std::span<const WiringArgument>{outer_arguments});
    outer.register_nested_service_consumers(*graph.wiring, invocation.identity);
    return invocation.output;
}

} // namespace bg::details

namespace bg {
namespace {

Signature default_reduce_signature() {
    auto parameters = std::vector<Parameter>{
        {util::symbol{"function"}, &GraphArgument::type()},
        {util::symbol{"args"}, &TSB<details::REDUCE_POSITIONAL>::type(), active, optional, false,
         details::VariadicMode::positional, details::variadic_wiring_operations<TSB<details::REDUCE_POSITIONAL>>()},
        {util::symbol{"kwargs"}, &TSB<details::REDUCE_NAMED>::type(), active, optional, false,
         details::VariadicMode::named, details::variadic_wiring_operations<TSB<details::REDUCE_NAMED>>()},
    };

    const auto &result = details::TimeSeriesVariableInstance<TS_T>::get();
    auto resolvers = std::vector<VariableResolver>{VariableResolver{
        &result, [](const TypeMap &, const WiringArguments &arguments) -> TypeMapValue {
            const auto *function = arguments.graph(0);
            if (function == nullptr) {
                return static_cast<const Type *>(nullptr);
            }
            return static_cast<const Type *>(details::DefaultReduceWiring::from(*function).outer_signature.output);
        }}};
    return {util::symbol{"default_reduce"}, std::move(parameters), result, std::move(resolvers)};
}

class DefaultReduceTemplate final : public WiringTemplate {
  public:
    DefaultReduceTemplate() : signature_{default_reduce_signature()} {}

    const Signature &signature() const noexcept final { return signature_; }

    RequirementResult requirement(const WiringArguments &arguments) const final {
        const auto *function = arguments.graph(0);
        if (function == nullptr) {
            return reject("a reduce call requires a reducer graph");
        }
        return function->payload<details::DefaultReduceWiring>() != nullptr
                   ? accept
                   : reject("the graph was not prepared for reduce");
    }

  private:
    std::optional<WiringPoint> wire(GraphWiring &, ResolvedSignature, const TypeMap &,
                                    std::span<const WiringArgument> arguments) const final {
        return details::DefaultReduceWiring::from(arguments.front().graph()).wire();
    }

    Signature signature_;
};

} // namespace

operator_<auto(GRAPH_T, TSB<details::REDUCE_POSITIONAL>, TSB<details::REDUCE_NAMED>) -> TS_T,
          details::reduce_operator_implementation>
    reduce{"reduce", {"graph", "args", "kwargs"}};

namespace {

const auto register_default_reduce = [] {
    reduce.overload(DefaultReduceTemplate{});
    return true;
}();

} // namespace
} // namespace bg
