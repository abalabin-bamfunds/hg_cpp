#pragma once

#include <bg/graph/context.hpp>
#include <bg/graph/details/wiring_callable.hpp>
#include <bg/graph/wiring_arguments.hpp>
#include <bg/graph/type_mapping.hpp>
#include <bg/graph/wiring_error.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/graph/wiring_template.hpp>

#include <functional>
#include <stdexcept>
#include <type_traits>

namespace bg {

namespace details {

template <typename Label, typename Requirement> struct CallableGraphOptions {
    Label label;
    CallableRequirement<Requirement> requirement;

    const Label &label_function() const noexcept { return label; }

    RequirementResult check_requirement(const WiringArguments &arguments) const {
        return requirement_result(std::invoke(requirement.function, arguments));
    }
};

template <typename T> struct callable_graph_label {
    using type = T;
};

template <typename Label, typename Requirement>
struct callable_graph_label<CallableGraphOptions<Label, Requirement>> {
    using type = Label;
};

template <typename T> using callable_graph_label_t = typename callable_graph_label<T>::type;

} // namespace details

template <typename Function, typename Options = details::default_wiring_label, typename... TypeParameters>
class GraphTemplate final
    : public WiringTemplate,
      public details::TypeParameterizedWiring<GraphTemplate<Function, Options, TypeParameters...>, TypeParameters...> {
    using FunctionInfo = details::graph_callable<Function>;
    using Result = typename FunctionInfo::result_type;
    using LabelFunction = details::callable_graph_label_t<Options>;

    static_assert(std::same_as<Result, void> || details::is_typed_wiring_point<Result>);

  public:
    using result_type = Result;
    GraphTemplate(Signature signature, Function function, Options options = {})
        : signature_(std::move(signature)), function_(std::move(function)), options_(std::move(options)) {
        if (signature_.parameter_count() != FunctionInfo::argument_count) {
            throw std::invalid_argument{"a graph function and its signature have different parameter counts"};
        }

        if constexpr (!std::same_as<LabelFunction, details::default_wiring_label>) {
            if (signature_.parameter_count() != details::callable<LabelFunction>::argument_count) {
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

    const Signature &default_signature() const override { return signature_; }

    RequirementResult requirement(const WiringArguments &arguments) const override {
        if constexpr (requires { options_.check_requirement(arguments); }) {
            return options_.check_requirement(arguments);
        } else {
            return accept;
        }
    }

    bool allows_output_inference() const noexcept override { return true; }

    bool preserves_input_point(std::size_t index) const noexcept override {
        return signature_.is_union_parameter(index);
    }

    bool same_binding_type_map(const TypeMap &left, const TypeMap &right) const override {
        if constexpr (FunctionInfo::receives_wiring_arguments) {
            return left.has_same_mappings(right);
        }
        return true;
    }

  private:
    util::symbol label(const ResolvedSignature &signature, std::span<const WiringArgument> arguments,
                       util::symbol default_label) const override {
        if constexpr (std::same_as<LabelFunction, details::default_wiring_label>) {
            return default_label;
        } else {
            const auto &label_function = [&]() -> const LabelFunction & {
                if constexpr (requires { options_.label_function(); }) {
                    return options_.label_function();
                } else {
                    return options_;
                }
            }();
            if (!details::wiring_parameters_match<LabelFunction>(signature_, signature)) {
                throw WiringError{"a graph label function does not match its resolved signature"};
            }

            return details::wiring_label(details::invoke_graph_function(label_function, arguments));
        }
    }

    [[nodiscard]] std::optional<WiringPoint> wire(GraphWiring &, ResolvedSignature signature,
                                                  const WiringArguments &argument_view,
                                                  std::span<const WiringArgument> arguments) const override {
        if (!details::wiring_parameters_match<Function>(signature_, signature)) {
            throw WiringError{"a graph function does not match its resolved signature"};
        }

        // Make resolved graph context parameters available as inner contexts while wiring the graph body.
        auto context_scopes = details::ContextScopes::activate_context_parameters(signature, arguments);

        if constexpr (std::same_as<Result, void>) {
            details::invoke_graph_function(function_, argument_view, arguments);

            return std::nullopt;
        } else {
            return static_cast<WiringPoint>(details::invoke_graph_function(function_, argument_view, arguments));
        }
    }

    Signature signature_;
    [[no_unique_address]] Function function_;
    [[no_unique_address]] Options options_;
};

template <typename Function> GraphTemplate(Signature, Function) -> GraphTemplate<std::decay_t<Function>>;

template <typename Function, typename Label>
GraphTemplate(Signature, Function, Label) -> GraphTemplate<std::decay_t<Function>, std::decay_t<Label>>;

} // namespace bg
