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

class MapGraphArgumentBuilder {
  public:
    MapGraphArgumentBuilder(util::symbol name, const WiringTemplate &function, ResolvedMapCall call,
                            bool preserve_named_arguments, const TypeMap &type_map)
        : name_{name}, function_{function}, call_{std::move(call)}, preserve_named_arguments_{preserve_named_arguments},
          type_map_{type_map}, wiring_{std::make_shared<GraphWiring>(WiringContext::current().wiring())} {
        const auto parent_diagnostics = details::WiringDiagnosticContext::current(WiringContext::current().wiring());
        diagnostic_contexts_.assign(parent_diagnostics.begin(), parent_diagnostics.end());
    }

    MapGraphArgumentWiring build() {
        resolve_key_argument();

        auto supplied_graph_arguments = std::vector<WiringArgument>{};
        auto boundaries = std::vector<WiringPoint>{};
        supplied_graph_arguments.reserve(call_.positional.size() + call_.named.size());

        auto diagnostics = details::WiringDiagnosticContext{*wiring_, diagnostic_contexts_};
        auto context = WiringContext{*wiring_};
        const auto append = [&](MapArgument &argument, std::size_t user_index) {
            if (call_.selected_key_set == user_index && call_.omit_selected_key_set_argument) {
                return;
            }

            supplied_graph_arguments.push_back(graph_argument(argument, user_index, boundaries));
        };

        for (auto index = std::size_t{}; index < call_.positional.size(); ++index) {
            append(call_.positional[index], index);
        }
        for (auto index = std::size_t{}; index < call_.named.size(); ++index) {
            append(call_.named[index], call_.positional.size() + index);
        }

        auto graph_arguments = arrange_graph_arguments(std::move(supplied_graph_arguments), boundaries);
        const auto result = function_(type_map_, std::span<const WiringArgument>{graph_arguments});
        auto structure_result = result;
        if (result) {
            structure_result = wiring_->complete_pending_wiring(*result);
        } else {
            (void)wiring_->complete_pending_wiring();
        }
        for (std::size_t index = 0; index < inputs_.size(); ++index) {
            inputs_[index].input_node = wiring_->output_node_index(input_boundaries_[index]);
        }
        if (key_input_boundary_) {
            key_input_node_ = wiring_->output_node_index(*key_input_boundary_);
        }
        const auto structure = graph_wiring_structure(*wiring_, boundaries, structure_result);
        auto output_boundary = std::optional<WiringPoint>{};
        if (result) {
            output_boundary = call_.valid_items
                                  ? wire_filtered_nested_output(*structure_result, structure_result->type())
                                  : wire_nested_output(*structure_result, structure_result->type());
        } else if (call_.valid_items) {
            throw WiringError{"__valid_items__ requires a mapped graph that produces an output"};
        }

        append_key_set_parameter();
        append_inherited_time_series_parameters();
        wiring_->finalize();
        const TimeSeriesType *output = nullptr;
        if (output_boundary) {
            output_node_ = wiring_->output_node_index(*output_boundary);
            const auto &output_reference = type_cast<TimeSeriesReferenceType>(output_boundary->type());
            output = &time_series_dictionary_type(*call_.key_type, output_reference,
                                                  TimeSeriesGuarantee::ordinary);
        }
        auto signature = ResolvedSignature{name_, std::move(parameters_), output};
        auto graph = MapGraphWiring{
            .wiring = wiring_,
            .inputs = std::move(inputs_),
            .key_input_node = key_input_node_,
            .output_node = output_node_,
            .key_set_parameter = key_set_parameter_,
            .valid_items = call_.valid_items,
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
    static const TimeSeriesType &mapped_dictionary_type(const TimeSeriesDictionaryType &dictionary,
                                                        TimeSeriesGuarantee reference_guarantee) {
        const auto &item = dictionary.value_type();
        if (type_cast<TimeSeriesReferenceType>(&item) != nullptr) {
            return dictionary.with_guarantee_limit(reference_guarantee);
        }

        const auto dictionary_guarantee = *dictionary.guarantee().value();
        const auto &reference = time_series_reference_type(item, reference_guarantee);
        return time_series_dictionary_type(dictionary.key_type(), reference, dictionary_guarantee);
    }

    std::size_t mapped_dictionary_count() const {
        const auto is_mapped_dictionary = [](const MapArgument &argument) {
            if (argument.pass_through || !argument.value.is_time_series()) {
                return false;
            }
            return type_cast<TimeSeriesDictionaryType>(&argument_time_series(argument.value)) != nullptr;
        };

        auto count = std::size_t{};
        for (const auto &argument : call_.positional) {
            count += static_cast<std::size_t>(is_mapped_dictionary(argument));
        }
        for (const auto &argument : call_.named) {
            count += static_cast<std::size_t>(is_mapped_dictionary(argument));
        }
        return count;
    }

    bool mapped_source_controls_lifetime(std::size_t user_index) const {
        if (call_.selected_key_set) {
            return *call_.selected_key_set == user_index;
        }
        return mapped_dictionary_count() == 1;
    }

    static const TimeSeriesType &argument_time_series(const WiringArgument &argument) {
        if (!argument.is_time_series()) {
            throw WiringError{"a map or mesh collection argument must be a time series"};
        }
        return static_cast<const TimeSeriesType &>(argument.type()).dereferenced_type();
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

    std::vector<WiringArgument> arrange_graph_arguments(std::vector<WiringArgument> supplied_arguments,
                                                        std::vector<WiringPoint> &boundaries) {
        const auto &signature = function_.signature();
        const auto fixed_count = signature.first_variadic_parameter().value_or(signature.parameter_count());
        auto fixed = std::vector<std::optional<WiringArgument>>(fixed_count);

        if (key_parameter_) {
            if (*key_parameter_ >= signature.parameter_count()) {
                throw WiringError{"__key_arg__ selects a graph parameter that is out of range"};
            }
            if (*key_parameter_ >= fixed_count || signature.parameter(*key_parameter_).context) {
                throw WiringError{"__key_arg__ must select a fixed graph parameter"};
            }

            const auto &key = time_series_value_type(*call_.key_type, TimeSeriesGuarantee::const_);
            const auto boundary = wire_nested_constant_input(key);
            key_input_boundary_ = boundary;
            boundaries.push_back(boundary);
            fixed[*key_parameter_].emplace(boundary);
        }

        auto variadic = std::vector<WiringArgument>{};
        variadic.reserve(supplied_arguments.size());
        auto next_positional = std::size_t{};
        for (auto &argument : supplied_arguments) {
            if (preserve_named_arguments_ && argument.name()) {
                auto named_parameter = std::optional<std::size_t>{};
                for (auto index = std::size_t{}; index < fixed_count; ++index) {
                    const auto &candidate = signature.parameter(index);
                    if (!candidate.context && candidate.name == *argument.name()) {
                        named_parameter = index;
                        break;
                    }
                }
                if (named_parameter) {
                    const auto index = *named_parameter;
                    if (fixed[index]) {
                        throw WiringError{"a graph parameter received more than one " + std::string{name_.text()} +
                                          " argument"};
                    }

                    argument.clear_name();
                    fixed[index].emplace(std::move(argument));
                    continue;
                }

                variadic.push_back(std::move(argument));
                continue;
            }

            while (next_positional < fixed_count &&
                   (fixed[next_positional] || signature.parameter(next_positional).context)) {
                ++next_positional;
            }
            if (next_positional == fixed_count) {
                variadic.push_back(std::move(argument));
                continue;
            }

            fixed[next_positional++].emplace(std::move(argument));
        }

        auto result = std::vector<WiringArgument>{};
        result.reserve(supplied_arguments.size() + static_cast<std::size_t>(key_parameter_.has_value()));
        for (auto index = std::size_t{}; index < fixed_count; ++index) {
            if (signature.parameter(index).context) {
                continue;
            }
            if (!fixed[index]) {
                if (signature.default_argument(index) != nullptr) {
                    continue;
                }
                throw WiringError{"a " + std::string{name_.text()} +
                                  " call does not provide a fixed graph parameter"};
            }
            auto argument = std::move(*fixed[index]);
            argument.set_name(signature.parameter(index).name);
            argument.match_fixed_parameter_by_name();
            result.push_back(std::move(argument));
        }
        for (auto &argument : variadic) {
            result.push_back(std::move(argument));
        }
        return result;
    }

    MapArgument &supplied(std::size_t index) {
        return index < call_.positional.size() ? call_.positional[index] : call_.named[index - call_.positional.size()];
    }

    WiringArgument graph_argument(MapArgument &supplied_argument, std::size_t user_index,
                                  std::vector<WiringPoint> &boundaries) {
        auto argument = WiringArgument::borrow(supplied_argument.value);
        if (!preserve_named_arguments_) {
            argument.clear_name();
        }
        if (!argument.is_time_series()) {
            return argument;
        }

        const auto &supplied_type = static_cast<const TimeSeriesType &>(argument.type());
        const auto &supplied_target = supplied_type.target_type();
        const auto *supplied_dictionary = type_cast<TimeSeriesDictionaryType>(&supplied_target);
        const auto mapped = supplied_dictionary != nullptr && !supplied_argument.pass_through;
        const auto controls_lifetime = mapped && mapped_source_controls_lifetime(user_index);
        const auto *item_reference = mapped
                                         ? type_cast<TimeSeriesReferenceType>(&supplied_dictionary->value_type())
                                         : nullptr;
        const auto &inner_type = mapped
                                     ? item_reference != nullptr ? item_reference->target_type()
                                                                 : supplied_dictionary->value_type().target_type()
                                     : supplied_target;
        const auto *supplied_reference = type_cast<TimeSeriesReferenceType>(&supplied_type);
        auto input_reference_guarantee = controls_lifetime ? TimeSeriesGuarantee::const_
                                                          : TimeSeriesGuarantee::ordinary;
        if (item_reference != nullptr) {
            input_reference_guarantee =
                weakest(input_reference_guarantee, *item_reference->guarantee().value());
        }
        if (supplied_reference != nullptr) {
            input_reference_guarantee = weakest(input_reference_guarantee, supplied_reference->reference_limit());
        }
        const auto &input_reference = mapped
                                          ? time_series_reference_type(inner_type, input_reference_guarantee)
                                          : supplied_reference != nullptr
                                                ? *supplied_reference
                                                : time_series_reference_type(supplied_type,
                                                                             TimeSeriesGuarantee::const_);
        const auto boundary = wire_nested_input(input_reference);
        boundaries.push_back(boundary);

        auto graph_argument = WiringArgument{boundary};
        if (argument.requires_optional()) {
            graph_argument.require_optional();
        }
        if (argument.annotations().valid_items) {
            graph_argument.require_valid_items();
        }
        if (const auto name = argument.name()) {
            graph_argument.set_variadic_name(*name, VariadicMode::none);
        }

        const auto parameter_index = parameters_.size();
        const auto &mapped_type = mapped ? mapped_dictionary_type(*supplied_dictionary, input_reference_guarantee)
                                         : input_reference;
        const auto &outer_type = mapped && supplied_reference != nullptr
                                     ? mapped_type.with_guarantee_limit(supplied_reference->reference_limit())
                                     : mapped_type;
        parameters_.push_back({.name = argument_name(supplied_argument.value, parameter_index),
                               .type = &outer_type,
                               .activity = active,
                               .validity = optional,
                               .valid_items = supplied_argument.value.annotations().valid_items});
        outer_arguments_.push_back(WiringArgument::borrow(supplied_argument.value));
        outer_arguments_.back().clear_name();
        inputs_.push_back({mapped ? MapInputKind::mapped : MapInputKind::pass_through, parameter_index, 0});
        input_boundaries_.push_back(boundary);
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
        parameters_.push_back(
            {util::symbol{"__key_set__"}, key_set_type, active, optional, false, VariadicMode::none});
        outer_arguments_.emplace_back(key_set_argument);
    }

    void append_inherited_time_series_parameters() {
        for (const auto &requirement : wiring_->inherited_requirements()) {
            parameters_.push_back({requirement.name, &requirement.boundary_input_type(), passive, optional});
            outer_arguments_.emplace_back(requirement.provider);
        }
    }

    util::symbol name_;
    const WiringTemplate &function_;
    ResolvedMapCall call_;
    bool preserve_named_arguments_;
    const TypeMap &type_map_;
    std::shared_ptr<GraphWiring> wiring_;
    std::vector<WiringErrorContext> diagnostic_contexts_;
    std::vector<ResolvedParameter> parameters_;
    std::vector<WiringArgument> outer_arguments_;
    std::vector<MapInputBoundary> inputs_;
    std::vector<WiringPoint> input_boundaries_;
    std::optional<std::size_t> key_parameter_;
    std::optional<std::size_t> key_input_node_;
    std::optional<WiringPoint> key_input_boundary_;
    std::optional<std::size_t> output_node_;
    std::optional<std::size_t> key_set_parameter_;
    std::optional<std::size_t> dictionary_key_set_parameter_;
};

} // namespace

MapGraphArgumentWiring build_map_graph_argument_wiring(util::symbol name, const WiringTemplate &function,
                                                       ResolvedMapCall call, bool preserve_named_arguments,
                                                       const TypeMap &type_map) {
    return MapGraphArgumentBuilder{name, function, std::move(call), preserve_named_arguments, type_map}.build();
}

} // namespace bg::details
