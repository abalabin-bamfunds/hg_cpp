#pragma once

#include <bg/graph/details/wiring_callable.hpp>
#include <bg/graph/graph_argument.hpp>
#include <bg/graph/wiring_template.hpp>
#include <bg/scalar/type.hpp>
#include <bg/type/details/declaration_variables.hpp>
#include <bg/type/details/scalar_type.hpp>
#include <bg/type/details/time_series_type.hpp>
#include <bg/type/guarantee_variable.hpp>
#include <bg/type/type_variable.hpp>

#include <array>
#include <concepts>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace bg {

namespace details {

struct no_graph_pattern_t {
    constexpr bool operator==(const no_graph_pattern_t &) const = default;
};

inline constexpr auto no_graph_pattern = no_graph_pattern_t{};

template <typename Function> struct graph_pattern_value {
    Function function;

    constexpr bool operator==(const graph_pattern_value &) const = default;
};

template <typename T> struct is_graph_declaration : std::false_type {};

template <typename T>
concept graph_declaration = is_graph_declaration<std::remove_cv_t<T>>::value;

template <typename Scope, typename Declaration, std::size_t Index>
const Type &declared_graph_parameter_type() {
    using Value = std::remove_cv_t<Declaration>;

    if constexpr (supported_time_series<Value>) {
        if constexpr (time_series_variable<Value>) {
            return time_series_variable_instance<Value>();
        } else if constexpr (Value::has_explicit_guarantee) {
            return time_series_type_for<Scope, Index, true, Value>(guarantee_parameter<Value::guarantee>());
        } else {
            static const auto guarantee = GuaranteeVariable{util::symbol{std::to_string(Index)}};
            return time_series_type_for<Scope, Index, true, Value>(guarantee);
        }
    } else if constexpr (generic_scalar<Value>) {
        return scalar_variable_instance<Value>();
    } else if constexpr (type_variable_declaration<Value> || graph_declaration<Value>) {
        return Value::type();
    } else {
        return cpp_scalar_type<canonical_scalar_type_t<Value>>();
    }
}

template <typename Scope, typename Declaration, std::size_t Index>
const TimeSeriesType &declared_graph_output_type() {
    using Value = std::remove_cv_t<Declaration>;
    static_assert(supported_time_series<Value>, "a graph type output must be a time series type or void");

    if constexpr (time_series_variable<Value>) {
        return time_series_variable_instance<Value>();
    } else {
        return time_series_type_for<Scope, Index, false, Value>(TimeSeriesGuarantee::ordinary);
    }
}

template <typename Declaration> struct graph_api_argument {
    using Value = std::remove_cv_t<Declaration>;
    using type = std::conditional_t<supported_time_series<Value>, wp<Value>, Value>;
};

template <typename Declaration>
using graph_api_argument_t = typename graph_api_argument<Declaration>::type;

template <typename Declaration> decltype(auto) declared_graph_argument(const WiringArgument &argument) {
    using Value = std::remove_cv_t<Declaration>;

    if constexpr (supported_time_series<Value>) {
        return wp<Value>::from_validated(argument.time_series());
    } else if constexpr (graph_declaration<Value>) {
        return Value{argument.graph()};
    } else {
        return graph_argument<Value>(argument);
    }
}

template <typename Prototype, auto Pattern> class GraphPatternTemplate;

template <auto Pattern> inline constexpr auto graph_pattern_identity = 0;

template <typename Result, typename... Arguments, auto Pattern>
class GraphPatternTemplate<Result(Arguments...), Pattern> final : public WiringTemplate {
    using Function = decltype(Pattern.function);

    static_assert(std::invocable<const Function &, graph_api_argument_t<Arguments>...>,
                  "a graph pattern must accept the declared graph parameters");

  public:
    GraphPatternTemplate() : signature_{make_signature(std::index_sequence_for<Arguments...>{})} {}

    const Signature &signature() const final { return signature_; }

  private:
    template <std::size_t... Index>
    static Signature make_signature(std::index_sequence<Index...>) {
        auto parameters = std::vector<Parameter>{};
        parameters.reserve(sizeof...(Arguments));
        (parameters.push_back(
             {util::symbol{std::to_string(Index)},
              &declared_graph_parameter_type<GraphPatternTemplate, Arguments, Index>()}),
         ...);

        if constexpr (std::same_as<Result, void>) {
            static_assert(std::same_as<std::invoke_result_t<const Function &, graph_api_argument_t<Arguments>...>,
                                       void>,
                          "an outputless graph pattern must return void");
            return {util::symbol{"graph_pattern"}, std::move(parameters)};
        } else {
            static_assert(is_typed_wiring_point<
                              std::invoke_result_t<const Function &, graph_api_argument_t<Arguments>...>>,
                          "an output graph pattern must return a wiring point");
            const auto &output =
                declared_graph_output_type<GraphPatternTemplate, Result, sizeof...(Arguments)>();
            return {util::symbol{"graph_pattern"}, std::move(parameters), output};
        }
    }

    template <std::size_t... Index>
    static std::optional<WiringPoint> invoke(std::span<const WiringArgument> arguments,
                                             std::index_sequence<Index...>) {
        if constexpr (std::same_as<Result, void>) {
            std::invoke(Pattern.function, declared_graph_argument<Arguments>(arguments[Index])...);
            return std::nullopt;
        } else {
            return static_cast<WiringPoint>(
                std::invoke(Pattern.function, declared_graph_argument<Arguments>(arguments[Index])...));
        }
    }

    std::optional<WiringPoint> wire(GraphWiring &, ResolvedSignature, const TypeMap &,
                                    std::span<const WiringArgument> arguments) const final {
        return invoke(arguments, std::index_sequence_for<Arguments...>{});
    }

    Signature signature_;
};

template <typename Prototype> struct graph_type_declaration;

template <typename Result, typename... Arguments>
struct graph_type_declaration<Result(Arguments...)> {
    template <typename Scope, std::size_t... Index>
    static std::vector<const Type *> parameters(std::index_sequence<Index...>) {
        return {&declared_graph_parameter_type<Scope, Arguments, Index>()...};
    }

    template <typename Scope> static const TimeSeriesType *output() {
        if constexpr (std::same_as<Result, void>) {
            return nullptr;
        } else {
            return &declared_graph_output_type<Scope, Result, sizeof...(Arguments)>();
        }
    }

    template <typename Visitor> static void for_each_variable(Visitor &&visitor) {
        (for_each_declaration_variable<Arguments>(visitor), ...);
        if constexpr (!std::same_as<Result, void>) {
            for_each_declaration_variable<Result>(visitor);
        }
    }

    static constexpr auto argument_count = sizeof...(Arguments);
};

} // namespace details

template <typename Function>
constexpr auto graph_pattern(Function function) {
    return details::graph_pattern_value<Function>{std::move(function)};
}

template <typename Prototype, auto Pattern = details::no_graph_pattern> class GRAPH {
    using Declaration = details::graph_type_declaration<Prototype>;

  public:
    explicit GRAPH(const GraphArgument &argument) noexcept : argument_{&argument} {}

    const GraphArgument &graph_argument() const noexcept { return *argument_; }

    static const GraphArgumentType &type() {
        static const auto value = [] {
            auto parameters = Declaration::template parameters<GRAPH>(
                std::make_index_sequence<Declaration::argument_count>{});
            auto pattern = std::shared_ptr<const WiringTemplate>{};
            const auto *pattern_identity = static_cast<const void *>(nullptr);
            if constexpr (!std::same_as<std::remove_cv_t<decltype(Pattern)>, details::no_graph_pattern_t>) {
                pattern = std::make_shared<const details::GraphPatternTemplate<Prototype, Pattern>>();
                pattern_identity = &details::graph_pattern_identity<Pattern>;
            }
            return GraphArgumentType{
                std::move(parameters), Declaration::template output<GRAPH>(), std::move(pattern),
                pattern_identity};
        }();
        return value;
    }

  private:
    const GraphArgument *argument_;
};

class GRAPH_T {
  public:
    explicit GRAPH_T(const GraphArgument &argument) noexcept : argument_{&argument} {}

    const GraphArgument &graph_argument() const noexcept { return *argument_; }
    static const GraphArgumentType &type() noexcept { return GraphArgument::type(); }

  private:
    const GraphArgument *argument_;
};

namespace details {

template <typename Prototype, auto Pattern>
struct is_graph_declaration<GRAPH<Prototype, Pattern>> : std::true_type {};

template <> struct is_graph_declaration<GRAPH_T> : std::true_type {};

template <typename Prototype, auto Pattern>
struct declaration_variable_traits<GRAPH<Prototype, Pattern>> {
    template <typename Visitor> static void for_each(Visitor &&visitor) {
        graph_type_declaration<Prototype>::for_each_variable(std::forward<Visitor>(visitor));
    }
};

} // namespace details

} // namespace bg
