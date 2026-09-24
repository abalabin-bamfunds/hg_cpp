#include <bg/map/details/map_graph_argument.hpp>

#include <bg/graph/details/nested.hpp>
#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/graph/wiring_template.hpp>
#include <bg/time_series/time_series_type.hpp>
#include <bg/type/time_series_dict_type.hpp>
#include <bg/type/time_series_reference_type.hpp>
#include <bg/type/time_series_value_type.hpp>

#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace bg::details {
namespace {

const TimeSeriesType &mapped_dictionary_type(const TimeSeriesDictionaryType &dictionary) {
    const auto &item = dictionary.value_type();
    if (type_cast<TimeSeriesReferenceType>(&item) != nullptr) {
        return dictionary;
    }

    const auto dictionary_guarantee = *dictionary.guarantee().value();
    const auto &reference =
        time_series_reference_type(item, strongest(TimeSeriesGuarantee::final, dictionary_guarantee));
    return time_series_dictionary_type(dictionary.key_type(), reference, dictionary_guarantee);
}

class MapGraphArgumentBuilder {
  public:
    MapGraphArgumentBuilder(util::symbol name, const WiringTemplate &function, ResolvedMapCall call,
                            bool preserve_named_arguments)
        : name_{name}, function_{function}, call_{std::move(call)}, preserve_named_arguments_{preserve_named_arguments},
          wiring_{std::make_shared<GraphWiring>()} {}

    MapGraphArgumentWiring build() {
        resolve_key_argument();

        auto graph_arguments = std::vector<WiringArgument>{};
        auto boundaries = std::vector<WiringPoint>{};
        graph_arguments.reserve(call_.positional.size() + call_.named.size() +
                                static_cast<std::size_t>(key_parameter_.has_value()));

        auto context = WiringContext{*wiring_};
        auto graph_parameter = std::size_t{};
        const auto append_key = [&] {
            if (!key_parameter_ || graph_parameter != *key_parameter_) {
                return;
            }

            const auto &key = time_series_value_type(*call_.key_type, TimeSeriesGuarantee::const_);
            const auto boundary = wire_nested_constant_input(key);
            key_input_node_ = wired_output_node_index(*wiring_, boundary);
            boundaries.push_back(boundary);
            graph_arguments.emplace_back(boundary);
            ++graph_parameter;
        };

        const auto append = [&](MapArgument &argument, std::size_t user_index) {
            append_key();
            if (call_.selected_key_set == user_index && call_.omit_selected_key_set_argument) {
                return;
            }

            graph_arguments.push_back(graph_argument(argument, boundaries));
            ++graph_parameter;
        };

        for (auto index = std::size_t{}; index < call_.positional.size(); ++index) {
            append(call_.positional[index], index);
        }
        for (auto index = std::size_t{}; index < call_.named.size(); ++index) {
            append(call_.named[index], call_.positional.size() + index);
        }
        append_key();
        if (key_parameter_ && graph_parameter <= *key_parameter_) {
            throw WiringError{"__key_arg__ selects a graph parameter that is out of range"};
        }

        const auto result = function_(std::span<const WiringArgument>{graph_arguments});
        const auto structure = graph_wiring_structure(*wiring_, boundaries, result);
        if (result) {
            const auto boundary = wire_nested_output(*result, result->type());
            output_node_ = wired_output_node_index(*wiring_, boundary);
            output_target_ = &type_cast<TimeSeriesReferenceType>(boundary.type()).target_type();
        }

        append_key_set_parameter();
        const auto output =
            output_target_ == nullptr
                ? nullptr
                : &time_series_dictionary_type(
                      *call_.key_type, time_series_reference_type(*output_target_, TimeSeriesGuarantee::deduplicated),
                      TimeSeriesGuarantee::ordinary);
        auto signature = ResolvedSignature{name_, std::move(parameters_), output};
        auto graph = MapGraphWiring{
            .wiring = wiring_,
            .inputs = std::move(inputs_),
            .key_input_node = key_input_node_,
            .output_node = output_node_,
            .key_set_parameter = key_set_parameter_,
        };
        return {
            .structure = structure,
            .graph = std::move(graph),
            .outer_signature = std::move(signature),
            .outer_arguments = std::move(outer_arguments_),
            .dictionary_key_set_parameter = dictionary_key_set_parameter_,
        };
    }

  private:
    static const TimeSeriesType &argument_time_series(const WiringArgument &argument) {
        if (!argument.is_time_series()) {
            throw WiringError{"a map or mesh collection argument must be a time series"};
        }
        return static_cast<const TimeSeriesType &>(argument.type()).target_type();
    }

    void resolve_key_argument() {
        if (call_.key_argument) {
            if (const auto index = call_.key_argument->index()) {
                key_parameter_ = *index;
                return;
            }

            const auto name = *call_.key_argument->name();
            for (auto index = std::size_t{}; index < function_.signature().parameter_count(); ++index) {
                if (function_.signature().parameter(index).name == name) {
                    key_parameter_ = index;
                    return;
                }
            }
            throw WiringError{"__key_arg__ names an unknown graph parameter"};
        }

        if (function_.signature().parameter_count() != 0 && function_.signature().parameter(0).name.text() == "key") {
            key_parameter_ = 0;
        }
    }

    MapArgument &supplied(std::size_t index) {
        return index < call_.positional.size() ? call_.positional[index] : call_.named[index - call_.positional.size()];
    }

    WiringArgument graph_argument(MapArgument &supplied_argument, std::vector<WiringPoint> &boundaries) {
        auto argument = wiring_argument_access::borrow(supplied_argument.value);
        if (!preserve_named_arguments_) {
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

        const auto parameter_index = parameters_.size();
        const auto &outer_type =
            mapped
                ? mapped_dictionary_type(*dictionary)
                : static_cast<const TimeSeriesType &>(time_series_reference_type(type, TimeSeriesGuarantee::ordinary));
        parameters_.push_back({argument_name(supplied_argument.value, parameter_index), &outer_type, active,
                               mapped ? required : optional, false, VariadicMode::none});
        outer_arguments_.push_back(wiring_argument_access::borrow(supplied_argument.value));
        wiring_argument_access::clear_name(outer_arguments_.back());
        inputs_.push_back({mapped ? MapInputKind::mapped : MapInputKind::pass_through, parameter_index,
                           wired_output_node_index(*wiring_, boundary)});
        return graph_argument;
    }

    static util::symbol argument_name(const WiringArgument &argument, std::size_t index) {
        return argument.name().value_or(util::symbol{std::to_string(index)});
    }

    void append_key_set_parameter() {
        if (!call_.selected_key_set) {
            return;
        }

        auto &selected = supplied(*call_.selected_key_set);
        const auto &type = argument_time_series(selected.value);
        auto key_set_argument = selected.value.time_series();
        const TimeSeriesType *key_set_type = &type;
        if (const auto *dictionary = type_cast<TimeSeriesDictionaryType>(&type)) {
            key_set_type = &dictionary->key_set_type();
            dictionary_key_set_parameter_ = parameters_.size();
        }

        key_set_parameter_ = parameters_.size();
        parameters_.push_back({util::symbol{"__key_set__"}, key_set_type, active, required, false, VariadicMode::none});
        outer_arguments_.emplace_back(key_set_argument);
    }

    util::symbol name_;
    const WiringTemplate &function_;
    ResolvedMapCall call_;
    bool preserve_named_arguments_;
    std::shared_ptr<GraphWiring> wiring_;
    std::vector<ResolvedParameter> parameters_;
    std::vector<WiringArgument> outer_arguments_;
    std::vector<MapInputBoundary> inputs_;
    std::optional<std::size_t> key_parameter_;
    std::optional<std::size_t> key_input_node_;
    std::optional<std::size_t> output_node_;
    std::optional<std::size_t> key_set_parameter_;
    std::optional<std::size_t> dictionary_key_set_parameter_;
    const TimeSeriesType *output_target_{};
};

} // namespace

MapGraphArgumentWiring build_map_graph_argument_wiring(util::symbol name, const WiringTemplate &function,
                                                       ResolvedMapCall call, bool preserve_named_arguments) {
    return MapGraphArgumentBuilder{name, function, std::move(call), preserve_named_arguments}.build();
}

} // namespace bg::details
