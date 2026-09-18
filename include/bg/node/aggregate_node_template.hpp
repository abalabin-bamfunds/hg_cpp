#pragma once

#include <bg/graph/details/wiring_callable.hpp>
#include <bg/graph/wiring_error.hpp>
#include <bg/node/details/aggregate_node_descriptor.hpp>

#include <stdexcept>

namespace bg {

template <typename Definition>
concept AggregateNodeClass = details::aggregate_node_class<Definition>::value;

template <AggregateNodeClass Definition, typename Label = details::default_wiring_label>
class AggregateNodeTemplate final : public details::AggregateNodeTemplateBase {
  public:
    using definition_type = Definition;
    explicit AggregateNodeTemplate(Label label = {})
        : AggregateNodeTemplateBase{details::aggregate_node_descriptor<Definition>()}, label_(std::move(label)) {
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

  private:
    util::symbol label(const ResolvedSignature &signature, std::span<const WiringArgument> arguments,
                       util::symbol default_label) const override {
        if constexpr (std::same_as<Label, details::default_wiring_label>) {
            return default_label;
        } else {
            if (!details::wiring_parameters_match<Label>(signature)) {
                throw WiringError{"a node label function does not match its resolved signature"};
            }

            return details::wiring_label(details::invoke_graph_function(label_, arguments));
        }
    }

    [[no_unique_address]] Label label_;
};

} // namespace bg
