#pragma once

#include <bg/graph/context.hpp>
#include <bg/graph/details/wiring_callable.hpp>
#include <bg/graph/type_mapping.hpp>
#include <bg/graph/wiring_error.hpp>
#include <bg/graph/wiring_template.hpp>

#include <stdexcept>
#include <type_traits>

namespace bg {

template <typename Function, typename Label = details::default_wiring_label, typename... TypeParameters>
class GraphTemplate final
    : public WiringTemplate,
      public details::TypeParameterizedWiring<GraphTemplate<Function, Label, TypeParameters...>, TypeParameters...> {
    using FunctionInfo = details::callable<Function>;
    using Result = typename FunctionInfo::result_type;

    static_assert(std::same_as<Result, void> || details::is_typed_wiring_point<Result>);

  public:
    using result_type = Result;
    GraphTemplate(Signature signature, Function function, Label label = {})
        : signature_(std::move(signature)), function_(std::move(function)), label_(std::move(label)) {
        if (signature_.parameter_count() != FunctionInfo::argument_count) {
            throw std::invalid_argument{"a graph function and its signature have different parameter counts"};
        }

        if constexpr (!std::same_as<Label, details::default_wiring_label>) {
            if (signature_.parameter_count() != details::callable<Label>::argument_count) {
                throw std::invalid_argument{"a graph label function and its signature have different parameter counts"};
            }
        }
    }

    template <typename... Arguments>
        requires(details::graph_code_argument<Arguments> && ...)
    [[nodiscard]] auto operator()(Arguments &&...arguments) const {
        if constexpr (std::same_as<Result, void>) {
            (void)WiringTemplate::operator()(std::forward<Arguments>(arguments)...);
        } else {
            return std::remove_cvref_t<Result>{*WiringTemplate::operator()(std::forward<Arguments>(arguments)...)};
        }
    }

    template <typename... Arguments>
        requires(details::graph_code_argument<Arguments> && ...)
    [[nodiscard]] auto operator()(const TypeMap &type_map, Arguments &&...arguments) const {
        if constexpr (std::same_as<Result, void>) {
            (void)WiringTemplate::operator()(type_map, std::forward<Arguments>(arguments)...);
        } else {
            return std::remove_cvref_t<Result>{
                *WiringTemplate::operator()(type_map, std::forward<Arguments>(arguments)...)};
        }
    }

    const Signature &signature() const override { return signature_; }

  private:
    util::symbol label(const ResolvedSignature &signature, std::span<const WiringArgument> arguments,
                       util::symbol default_label) const override {
        if constexpr (std::same_as<Label, details::default_wiring_label>) {
            return default_label;
        } else {
            if (!details::wiring_parameters_match<Label>(signature_, signature)) {
                throw WiringError{"a graph label function does not match its resolved signature"};
            }

            return details::wiring_label(details::invoke_graph_function(label_, arguments));
        }
    }

    [[nodiscard]] std::optional<WiringPoint> wire(GraphWiring &, ResolvedSignature signature, const TypeMap &,
                                                  std::span<const WiringArgument> arguments) const override {
        if (!details::wiring_parameters_match<Function>(signature_, signature)) {
            throw WiringError{"a graph function does not match its resolved signature"};
        }

        // Make resolved graph context parameters available as inner contexts while wiring the graph body.
        auto context_scopes = details::ContextScopes::activate_context_parameters(signature, arguments);

        if constexpr (std::same_as<Result, void>) {
            details::invoke_graph_function(function_, arguments);

            return std::nullopt;
        } else {
            return static_cast<WiringPoint>(details::invoke_graph_function(function_, arguments));
        }
    }

    Signature signature_;
    [[no_unique_address]] Function function_;
    [[no_unique_address]] Label label_;
};

template <typename Function> GraphTemplate(Signature, Function) -> GraphTemplate<std::decay_t<Function>>;

template <typename Function, typename Label>
GraphTemplate(Signature, Function, Label) -> GraphTemplate<std::decay_t<Function>, std::decay_t<Label>>;

} // namespace bg
