#pragma once

#include <bg/graph/details/wiring_callable.hpp>
#include <bg/graph/type_mapping.hpp>
#include <bg/graph/wiring_arguments.hpp>
#include <bg/graph/wiring_error.hpp>
#include <bg/node/details/aggregate_node_descriptor.hpp>

#include <stdexcept>
#include <memory>

namespace bg {

template <typename Definition>
concept AggregateNodeClass = details::aggregate_node_class<Definition>::value;

template <AggregateNodeClass Definition, typename Label = details::default_wiring_label, typename... TypeParameters>
class AggregateNodeTemplate final
    : public details::AggregateNodeTemplateBase,
      public details::TypeParameterizedWiring<AggregateNodeTemplate<Definition, Label, TypeParameters...>,
                                              TypeParameters...> {
    using TypeMapping = details::TypeParameterizedWiring<
        AggregateNodeTemplate<Definition, Label, TypeParameters...>, TypeParameters...>;

  public:
    using definition_type = Definition;
    using TypeMapping::operator[];

    explicit AggregateNodeTemplate(Label label = {})
        : AggregateNodeTemplate{details::aggregate_node_descriptor<Definition>(), std::move(label)} {}

    AggregateNodeTemplate(const details::AggregateNodeDescriptor &descriptor, Label label = {})
        : AggregateNodeTemplateBase{descriptor}, label_(std::move(label)) {
        if constexpr (!std::same_as<Label, details::default_wiring_label>) {
            if (signature().parameter_count() != details::callable<Label>::argument_count) {
                throw std::invalid_argument{"a node label function and its signature have different parameter counts"};
            }
        }
    }

    template <typename... Arguments>
        requires((details::graph_code_argument<Arguments> && ...) && details::aggregate_node_has_output<Definition>())
    [[nodiscard]] auto operator()(Arguments &&...arguments) const & {
        using Output = details::aggregate_output_time_series_t<Definition>;
        return wp<Output>{*NodeTemplate::operator()(std::forward<Arguments>(arguments)...)};
    }

    template <typename... Arguments>
        requires((details::graph_code_argument<Arguments> && ...) && details::aggregate_node_has_output<Definition>())
    [[nodiscard]] auto operator()(const TypeMap &type_map, Arguments &&...arguments) const & {
        using Output = details::aggregate_output_time_series_t<Definition>;
        return wp<Output>{*NodeTemplate::operator()(type_map, std::forward<Arguments>(arguments)...)};
    }

    template <typename... Arguments>
        requires((details::graph_code_argument<Arguments> && ...) && !details::aggregate_node_has_output<Definition>())
    void operator()(Arguments &&...arguments) const & {
        (void)NodeTemplate::operator()(std::forward<Arguments>(arguments)...);
    }

    template <typename... Arguments>
        requires((details::graph_code_argument<Arguments> && ...) && !details::aggregate_node_has_output<Definition>())
    void operator()(const TypeMap &type_map, Arguments &&...arguments) const & {
        (void)NodeTemplate::operator()(type_map, std::forward<Arguments>(arguments)...);
    }

    template <typename... Arguments> void operator()(Arguments &&...) const && = delete;
    template <typename... Mappings> auto operator[](Mappings...) && = delete;

  private:
    const TimeSeriesRuntimeType *output_runtime_type(const WiringArguments &arguments) const override {
        if constexpr (details::aggregate_node_has_output<Definition>()) {
            if constexpr (requires(const WiringArguments &wiring_arguments) {
                              { Definition::output_runtime_type(wiring_arguments) } ->
                                  std::same_as<const TimeSeriesRuntimeType &>;
                          }) {
                return std::addressof(Definition::output_runtime_type(arguments));
            } else {
                using Output = details::aggregate_output_declaration_t<Definition>;
                return &output_declaration_traits<Output>::runtime(*arguments.resolved_output_type());
            }
        }
        return nullptr;
    }

    util::symbol label(const ResolvedSignature &signature, std::span<const WiringArgument> arguments,
                       util::symbol default_label) const override {
        if constexpr (std::same_as<Label, details::default_wiring_label>) {
            return default_label;
        } else {
            if (!details::wiring_parameters_match<Label>(this->signature(), signature)) {
                throw WiringError{"a node label function does not match its resolved signature"};
            }

            return details::wiring_label(details::invoke_graph_function(label_, arguments));
        }
    }

    [[no_unique_address]] Label label_;
};

} // namespace bg
