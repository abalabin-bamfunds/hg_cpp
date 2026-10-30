#include <bg/switch.hpp>

#include <bg/graph/details/graph_wiring_structure.hpp>
#include <bg/graph/details/nested.hpp>
#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/node/node_template.hpp>
#include <bg/switch/details/default_switch_wiring.hpp>
#include <bg/time_series/tsb/details/wiring.hpp>
#include <bg/type/time_series_reference_type.hpp>
#include <bg/type/time_series_value_type.hpp>
#include <bg/type/time_series_variable.hpp>

#include <algorithm>
#include <ranges>
#include <stdexcept>
#include <string>
#include <utility>

namespace bg::details {
namespace {

struct SelectedArgument {
    std::size_t supplied_index;
    WiringArgument value;
};

class SwitchGraphArgumentBuilder {
  public:
    SwitchGraphArgumentBuilder(WiringPoint key, std::span<const WiringArgument> arguments,
                               std::span<const PreparedSwitchCase> cases)
        : key_{key}, arguments_{arguments}, cases_{cases} {}

    GraphArgument build() {
        validate_key();
        append_outer_arguments();

        if (cases_.empty()) {
            throw WiringError{"switch_ requires at least one case"};
        }

        auto case_structures = std::vector<GraphWiringStructure>{};
        case_structures.reserve(cases_.size());
        node_.cases.reserve(cases_.size());
        for (const auto &item : cases_) {
            append_case(item, case_structures);
        }

        const auto *output_target = resolved_output_target();
        const auto output = output_target == nullptr
                                ? nullptr
                                : &time_series_reference_type(*output_target, TimeSeriesGuarantee::deduplicated);
        auto resolved_parameters = std::vector<ResolvedParameter>{};
        resolved_parameters.reserve(parameters_.size());
        for (const auto &parameter : parameters_) {
            resolved_parameters.push_back({parameter.name, parameter.type, parameter.activity, parameter.validity,
                                           parameter.accepts_absent_scalar, parameter.variadic_mode, false, false,
                                           parameter.valid_items});
        }
        auto outer_signature = ResolvedSignature{util::symbol{"switch"}, std::move(resolved_parameters), output};

        auto payload = std::make_shared<DefaultSwitchWiring>(DefaultSwitchWiring{
            .node = std::move(node_),
            .outer_signature = std::move(outer_signature),
            .outer_arguments = std::move(outer_arguments_),
        });

        return GraphArgument{std::move(case_structures), std::move(payload)};
    }

  private:
    void validate_key() {
        const auto &target = key_.type().target_type();
        const auto *value = type_cast<TimeSeriesValueType>(&target);
        if (value == nullptr || !value->value_type().supports_equality() || !value->value_type().supports_hash()) {
            throw WiringError{"switch_ key must be a hashable time-series value"};
        }
        key_type_ = &value->value_type();

        parameters_.push_back({util::symbol{"key"}, &target, active, optional, false, VariadicMode::none});
        outer_arguments_.emplace_back(key_);
    }

    void append_outer_arguments() {
        for (auto index = std::size_t{}; index < arguments_.size(); ++index) {
            const auto &argument = arguments_[index];
            if (!argument.is_time_series()) {
                continue;
            }

            const auto &source_type = static_cast<const TimeSeriesType &>(argument.type());
            const auto *source_reference = type_cast<TimeSeriesReferenceType>(&source_type);
            const auto &reference = source_reference != nullptr
                                        ? *source_reference
                                        : time_series_reference_type(source_type, TimeSeriesGuarantee::const_);
            const auto parameter_index = parameters_.size();
            outer_parameter_indices_.push_back({index, parameter_index});
            parameters_.push_back({.name = argument.name().value_or(util::symbol{std::to_string(index)}),
                                   .type = &reference,
                                   .activity = active,
                                   .validity = optional,
                                   .valid_items = argument.annotations().valid_items});
            auto stored = WiringArgument::borrow(argument);
            stored.clear_name();
            outer_arguments_.push_back(std::move(stored));
        }
    }

    std::size_t outer_parameter_index(std::size_t supplied_index) const {
        const auto found =
            std::ranges::find(outer_parameter_indices_, supplied_index, &std::pair<std::size_t, std::size_t>::first);
        if (found == outer_parameter_indices_.end()) {
            throw std::logic_error{"a switch boundary does not have an outer parameter"};
        }
        return found->second;
    }

    std::vector<SelectedArgument> select_arguments(const PreparedSwitchCase &item) const {
        if (!item.preserve_argument_names) {
            auto selected = std::vector<SelectedArgument>{};
            selected.reserve(arguments_.size());
            for (auto index = std::size_t{}; index < arguments_.size(); ++index) {
                auto value = WiringArgument::borrow(arguments_[index]);
                value.clear_name();
                selected.push_back({index, std::move(value)});
            }
            return selected;
        }

        const auto &signature = item.function->signature();
        const auto first_variadic = signature.first_variadic_parameter().value_or(signature.parameter_count());
        auto used = std::vector<bool>(arguments_.size());
        auto selected = std::vector<SelectedArgument>{};
        selected.reserve(arguments_.size());
        auto next_positional = std::size_t{};

        for (auto parameter_index = std::size_t{}; parameter_index < first_variadic; ++parameter_index) {
            const auto name = signature.parameter(parameter_index).name;
            auto supplied_index = arguments_.size();
            for (auto index = std::size_t{}; index < arguments_.size(); ++index) {
                if (!used[index] && arguments_[index].name() == std::optional{name}) {
                    supplied_index = index;
                    break;
                }
            }
            if (supplied_index == arguments_.size()) {
                while (next_positional < arguments_.size() &&
                       (used[next_positional] || arguments_[next_positional].name())) {
                    ++next_positional;
                }
                if (next_positional == arguments_.size()) {
                    if (signature.default_argument(parameter_index) != nullptr) {
                        continue;
                    }
                    throw WiringError{"switch case graph is missing argument '" + std::string{name.text()} + "'"};
                }
                supplied_index = next_positional++;
            }

            used[supplied_index] = true;
            auto value = WiringArgument::borrow(arguments_[supplied_index]);
            value.set_name(name);
            value.match_fixed_parameter_by_name();
            selected.push_back({supplied_index, std::move(value)});
        }

        const auto append_remaining = [&](VariadicMode mode) {
            if (!signature.variadic_parameter(mode)) {
                return;
            }
            for (auto index = std::size_t{}; index < arguments_.size(); ++index) {
                if (used[index] || (mode == VariadicMode::named) != arguments_[index].name().has_value()) {
                    continue;
                }
                used[index] = true;
                selected.push_back({index, WiringArgument::borrow(arguments_[index])});
            }
        };
        append_remaining(VariadicMode::positional);
        append_remaining(VariadicMode::named);

        for (auto index = std::size_t{}; index < arguments_.size(); ++index) {
            if (!used[index] && !arguments_[index].name()) {
                throw WiringError{"switch case graph does not accept every positional argument"};
            }
        }
        return selected;
    }

    void record_output_type(bool default_case, const TimeSeriesType *target) {
        if (default_case) {
            default_output_target_ = target;
            return;
        }
        if (!keyed_case_seen_) {
            keyed_case_seen_ = true;
            output_target_ = target;
            return;
        }
        if ((output_target_ == nullptr) != (target == nullptr)) {
            throw WiringError{"switch_ cannot mix sink and output cases"};
        }
        if (target == nullptr) {
            return;
        }

        output_target_ = output_target_->common_type(*target);
        if (output_target_ == nullptr) {
            throw WiringError{"switch case output types are incompatible"};
        }
    }

    const TimeSeriesType *resolved_output_target() const {
        if (!keyed_case_seen_) {
            return default_output_target_;
        }
        if (output_target_ == nullptr) {
            if (default_output_target_ != nullptr) {
                throw WiringError{"switch_ cannot mix sink and output cases"};
            }
            return nullptr;
        }
        if (default_output_target_ == nullptr) {
            return output_target_;
        }

        const auto *common = output_target_->common_type(*default_output_target_);
        if (common == nullptr) {
            throw WiringError{"switch case output types are incompatible"};
        }
        return common;
    }

    void append_case(const PreparedSwitchCase &item, std::vector<GraphWiringStructure> &structures) {
        auto stored_keys = std::vector<std::shared_ptr<const ScalarValue<>>>{};
        stored_keys.reserve(item.keys.size());
        if (item.default_case) {
            if (!item.keys.empty()) {
                throw WiringError{"a DEFAULT switch case cannot also have keys"};
            }
            if (default_seen_) {
                throw WiringError{"switch_ accepts one DEFAULT case"};
            }
            default_seen_ = true;
        } else {
            if (item.keys.empty()) {
                throw WiringError{"a switch case key tuple cannot be empty"};
            }

            for (const auto &key : item.keys) {
                if (!key.is_scalar() || !key.present() || &key.scalar().type() != key_type_) {
                    throw WiringError{"switch case keys must match the switch key type"};
                }
                for (const auto &existing : node_.cases) {
                    for (const auto &existing_key : existing.keys) {
                        if (existing_key->equals(key.scalar())) {
                            throw WiringError{"switch_ has duplicate case keys"};
                        }
                    }
                }
                for (const auto &existing_key : stored_keys) {
                    if (existing_key->equals(key.scalar())) {
                        throw WiringError{"switch_ has duplicate case keys"};
                    }
                }
                stored_keys.push_back(std::make_shared<const ScalarValue<>>(key.scalar()));
            }
        }

        const auto parent_diagnostics =
            details::WiringDiagnosticContext::current(WiringContext::current().wiring());
        auto wiring = std::make_shared<GraphWiring>(WiringContext::current().wiring());
        auto diagnostics = details::WiringDiagnosticContext{*wiring, parent_diagnostics};
        auto boundaries = std::vector<WiringPoint>{};
        auto boundary_inputs = std::vector<SwitchInputBoundary>{};
        auto boundary_input_points = std::vector<WiringPoint>{};
        auto selected = select_arguments(item);
        auto graph_arguments = std::vector<WiringArgument>{};
        graph_arguments.reserve(selected.size());

        auto context = WiringContext{*wiring};
        for (auto &argument : selected) {
            if (!argument.value.is_time_series()) {
                graph_arguments.push_back(std::move(argument.value));
                continue;
            }

            const auto &source_type = static_cast<const TimeSeriesType &>(argument.value.type());
            const auto *source_reference = type_cast<TimeSeriesReferenceType>(&source_type);
            const auto &reference = source_reference != nullptr
                                        ? *source_reference
                                        : time_series_reference_type(source_type, TimeSeriesGuarantee::const_);
            const auto boundary = wire_nested_input(reference);
            boundaries.push_back(boundary);
            boundary_inputs.push_back({outer_parameter_index(argument.supplied_index), 0});
            boundary_input_points.push_back(boundary);
            auto inner = WiringArgument{boundary};
            if (argument.value.requires_optional()) {
                inner.require_optional();
            }
            if (argument.value.annotations().valid_items) {
                inner.require_valid_items();
            }
            if (const auto name = argument.value.name()) {
                inner.set_variadic_name(*name, argument.value.variadic_capture_mode());
            }
            graph_arguments.push_back(std::move(inner));
        }

        const auto result = (*item.function)(std::span<const WiringArgument>{graph_arguments});
        auto output_node = std::optional<std::size_t>{};
        auto structure_result = std::optional<WiringPoint>{};
        const TimeSeriesType *target{};
        if (result) {
            structure_result = wiring->complete_pending_wiring(*result);
            target = &structure_result->type().target_type().reference_free_type();
        } else {
            (void)wiring->complete_pending_wiring();
        }
        for (std::size_t index = 0; index < boundary_inputs.size(); ++index) {
            boundary_inputs[index].input_node = wiring->output_node_index(boundary_input_points[index]);
        }

        record_output_type(item.default_case, target);

        auto inherited_parameter_start = std::optional<std::size_t>{};
        if (!wiring->inherited_requirements().empty()) {
            inherited_parameter_start = parameters_.size();
        }
        for (const auto &requirement : wiring->inherited_requirements()) {
            parameters_.push_back({requirement.name, &requirement.boundary_input_type(), passive, optional});
            outer_arguments_.emplace_back(requirement.provider);
        }

        structures.push_back(graph_wiring_structure(*wiring, boundaries, structure_result));
        if (result) {
            const auto output = wire_nested_output(*structure_result, structure_result->type());
            wiring->finalize();
            output_node = wiring->output_node_index(output);
        } else {
            wiring->finalize();
        }
        node_.cases.push_back({
            .keys = std::move(stored_keys),
            .default_case = item.default_case,
            .branch = {.function_owner = item.function_owner,
                       .wiring = std::move(wiring),
                       .inputs = std::move(boundary_inputs),
                       .inherited_parameter_start = inherited_parameter_start,
                       .output_node = output_node},
        });
    }

    WiringPoint key_;
    std::span<const WiringArgument> arguments_;
    std::span<const PreparedSwitchCase> cases_;
    const ScalarType *key_type_{};
    const TimeSeriesType *output_target_{};
    const TimeSeriesType *default_output_target_{};
    bool default_seen_{};
    bool keyed_case_seen_{};
    std::vector<Parameter> parameters_;
    std::vector<WiringArgument> outer_arguments_;
    std::vector<std::pair<std::size_t, std::size_t>> outer_parameter_indices_;
    SwitchNodeWiring node_;
};

Signature default_switch_signature(bool output) {
    auto parameters = std::vector<Parameter>{
        {util::symbol{"key"}, &TS<SCALAR>::type(), active, optional},
        {util::symbol{"cases"}, &GraphArgument::type()},
        {util::symbol{"args"}, &TSB<SWITCH_POSITIONAL>::type(), active, optional, false, VariadicMode::positional,
         variadic_wiring_operations<TSB<SWITCH_POSITIONAL>>()},
        {util::symbol{"kwargs"}, &TSB<SWITCH_NAMED>::type(), active, optional, false, VariadicMode::named,
         variadic_wiring_operations<TSB<SWITCH_NAMED>>()},
    };
    if (!output) {
        return {util::symbol{"default_switch"}, std::move(parameters)};
    }

    const auto &result = TimeSeriesVariableInstance<TS_T>::get();
    auto resolvers = std::vector<VariableResolver>{
        VariableResolver{&result, [](const TypeMap &, const WiringArguments &arguments) -> TypeMapValue {
                             const auto *cases = arguments.graph(1);
                             if (cases == nullptr) {
                                 return static_cast<const Type *>(nullptr);
                             }
                             return static_cast<const Type *>(DefaultSwitchWiring::from(*cases).outer_signature.output);
                         }}};
    return {util::symbol{"default_switch"}, std::move(parameters), result, std::move(resolvers)};
}

class DefaultValueSwitchTemplate final : public WiringTemplate {
  public:
    DefaultValueSwitchTemplate() : signature_{default_switch_signature(true)} {}

    const Signature &default_signature() const noexcept final { return signature_; }

  private:
    std::optional<WiringPoint> wire(GraphWiring &, ResolvedSignature, const WiringArguments &,
                                    std::span<const WiringArgument> arguments) const final {
        return DefaultSwitchWiring::from(arguments[1].graph()).wire();
    }

    Signature signature_;
};

class DefaultSinkSwitchTemplate final : public WiringTemplate {
  public:
    DefaultSinkSwitchTemplate() : signature_{default_switch_signature(false)} {}

    const Signature &default_signature() const noexcept final { return signature_; }

    RequirementResult requirement(const WiringArguments &arguments) const final {
        const auto *cases = arguments.graph(1);
        return cases != nullptr && DefaultSwitchWiring::from(*cases).outer_signature.output == nullptr
                   ? accept
                   : reject("the switch case graphs produce an output");
    }

  private:
    std::optional<WiringPoint> wire(GraphWiring &, ResolvedSignature, const WiringArguments &,
                                    std::span<const WiringArgument> arguments) const final {
        return DefaultSwitchWiring::from(arguments[1].graph()).wire();
    }

    Signature signature_;
};

} // namespace

GraphArgument build_switch_graph_argument(WiringPoint key, std::span<const WiringArgument> arguments,
                                          std::span<const PreparedSwitchCase> cases) {
    return SwitchGraphArgumentBuilder{key, arguments, cases}.build();
}

const DefaultSwitchWiring &DefaultSwitchWiring::from(const GraphArgument &argument) {
    const auto wiring = argument.payload<DefaultSwitchWiring>();
    if (!wiring) {
        throw WiringError{"the default switch overload requires prepared switch cases"};
    }
    return *wiring;
}

std::optional<WiringPoint> DefaultSwitchWiring::wire() const {
    auto node_template = make_switch_node(node, outer_signature);
    auto &outer = WiringContext::current().wiring();
    const auto &owned = outer.own_node_template(std::move(node_template));
    auto call = outer.invoke_node(owned, outer_arguments);
    for (const auto &branch : node.cases) {
        outer.register_nested_service_consumers(*branch.branch.wiring, call.identity);
    }
    return call.output;
}

} // namespace bg::details

namespace bg {

operator_<auto(TS<SCALAR>, GRAPH_T, TSB<details::SWITCH_POSITIONAL>, TSB<details::SWITCH_NAMED>)->TS_T,
          details::switch_operator_implementation>
    switch_{"switch", {"key", "cases", "args", "kwargs"}};

namespace {

const auto register_default_switch_overloads = [] {
    switch_.overload(details::DefaultValueSwitchTemplate{});
    switch_.overload(details::DefaultSinkSwitchTemplate{});
    return true;
}();

} // namespace
} // namespace bg
