#pragma once

#include <bg/evaluation_profiler.hpp>
#include <bg/graph_run_options.hpp>
#include <bg/scalar/value.hpp>
#include <bg/testing/details/test_graph.hpp>
#include <bg/testing/test_graph_literal_input.hpp>
#include <bg/testing/test_graph_input.hpp>
#include <bg/graph_run_result.hpp>

#include <array>
#include <cstddef>
#include <functional>
#include <initializer_list>
#include <optional>
#include <ranges>
#include <span>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace bg::testing {

namespace details {

template <typename Input>
concept literal_input_range = std::ranges::contiguous_range<Input> &&
                              std::same_as<std::remove_cv_t<std::ranges::range_value_t<Input>>,
                                           TestGraphLiteralInput>;

template <typename Input>
concept runtime_input_range = std::ranges::contiguous_range<Input> &&
                              std::same_as<std::remove_cv_t<std::ranges::range_value_t<Input>>, TestGraphInput>;

template <typename TimeSeries> struct simple_scalar_test_input : std::false_type {};

template <typename Scalar, auto... Properties>
struct simple_scalar_test_input<TS<Scalar, Properties...>>
    : std::bool_constant<!scalar_declaration_traits<Scalar>::is_collection> {};

template <typename Tested>
concept test_graph_target = std::derived_from<std::remove_cvref_t<Tested>, WiringTemplate> ||
                            requires { typename test_function<Tested>::callable; };

template <typename Function>
inline constexpr bool legacy_scalar_test_input = [] {
    using FunctionInfo = test_function<Function>;
    if constexpr (FunctionInfo::callable::argument_count == 1) {
        return simple_scalar_test_input<typename FunctionInfo::template input_time_series<0>>::value;
    } else {
        return false;
    }
}();

} // namespace details

template <typename T = void> using TestGraphResult = GraphRunResult<T>;

namespace details {

template <typename Tested>
[[nodiscard]] auto run_literal_inputs(const Tested &tested, const GraphRunOptions &options,
                                     std::span<const TestGraphLiteralInput> inputs) {
    if constexpr (std::derived_from<std::remove_cvref_t<Tested>, WiringTemplate>) {
        return TestGraphResult<>{run_template_literals(tested, options, inputs)};
    } else {
        using FunctionInfo = test_function<Tested>;
        static_assert(bg::details::is_typed_wiring_point<typename FunctionInfo::result>);
        if (inputs.size() != FunctionInfo::callable::argument_count) {
            throw std::invalid_argument{"test_graph literal input count does not match the graph function"};
        }

        auto values = run_function_literals(tested, options, inputs,
                                            std::make_index_sequence<FunctionInfo::callable::argument_count>{});
        return TestGraphResult<typename FunctionInfo::output_scalar>{std::move(values)};
    }
}

template <typename Tested, typename... Inputs>
[[nodiscard]] auto run_literal_inputs(const Tested &tested, const GraphRunOptions &options,
                                     const Inputs &...inputs) {
    static_assert((std::same_as<std::remove_cvref_t<Inputs>, TestGraphLiteralInput> && ...));
    const auto values = std::array<TestGraphLiteralInput, sizeof...(Inputs)>{inputs...};
    return run_literal_inputs(tested, options,
                              std::span<const TestGraphLiteralInput>{values});
}

template <test_graph_target Tested, std::ranges::contiguous_range... Inputs>
[[nodiscard]] auto run_range_inputs(const Tested &tested, const GraphRunOptions &options, const Inputs &...inputs) {
    if constexpr (std::derived_from<std::remove_cvref_t<Tested>, WiringTemplate>) {
        return TestGraphResult<>{run_template(tested, options, inputs...)};
    } else {
        using FunctionInfo = test_function<Tested>;
        static_assert(FunctionInfo::callable::argument_count == sizeof...(Inputs));
        static_assert(bg::details::is_typed_wiring_point<typename FunctionInfo::result>);

        const auto ranges = std::tie(inputs...);
        auto values = run_function(tested, options, ranges, std::make_index_sequence<sizeof...(Inputs)>{});
        return TestGraphResult<typename FunctionInfo::output_scalar>{std::move(values)};
    }
}

template <typename Function>
    requires(!std::derived_from<std::remove_cvref_t<Function>, WiringTemplate> &&
             legacy_scalar_test_input<Function>)
[[nodiscard]] auto run_legacy_scalar_input(const Function &function, const GraphRunOptions &options,
                                           std::initializer_list<
                                               std::optional<typename test_function<Function>::template input_scalar<0>>>
                                               input) {
    using FunctionInfo = test_function<Function>;

    const auto inputs = std::tie(input);
    auto values = run_function(function, options, inputs, std::index_sequence<0>{});
    return TestGraphResult<typename FunctionInfo::output_scalar>{std::move(values)};
}

template <typename Options>
concept graph_run_options = std::same_as<std::remove_cvref_t<Options>, GraphRunOptions>;

} // namespace details

template <details::test_graph_target Tested, std::ranges::contiguous_range... Inputs>
    requires((!details::literal_input_range<Inputs>) && ...) &&
            ((!details::runtime_input_range<Inputs>) && ...)
[[nodiscard]] auto test_graph(const Tested &tested, const Inputs &...inputs) {
    return details::run_range_inputs(tested, GraphRunOptions{}, inputs...);
}

template <details::test_graph_target Tested, details::graph_run_options Options,
          std::ranges::contiguous_range... Inputs>
    requires((!details::literal_input_range<Inputs>) && ...) &&
            ((!details::runtime_input_range<Inputs>) && ...)
[[nodiscard]] auto test_graph(const Tested &tested, const Options &options, const Inputs &...inputs) {
    return details::run_range_inputs(tested, options, inputs...);
}

template <details::test_graph_target Tested, details::runtime_input_range Inputs>
[[nodiscard]] auto test_graph(const Tested &tested, const Inputs &inputs) {
    static_assert(std::derived_from<std::remove_cvref_t<Tested>, WiringTemplate>);
    const auto options = GraphRunOptions{};
    return TestGraphResult<>{details::run_runtime_template(
        tested, options,
        std::span<const TestGraphInput>{std::ranges::data(inputs), std::ranges::size(inputs)})};
}

template <details::test_graph_target Tested, details::graph_run_options Options,
          details::runtime_input_range Inputs>
[[nodiscard]] auto test_graph(const Tested &tested, const Options &options, const Inputs &inputs) {
    static_assert(std::derived_from<std::remove_cvref_t<Tested>, WiringTemplate>);
    return TestGraphResult<>{details::run_runtime_template(
        tested, options,
        std::span<const TestGraphInput>{std::ranges::data(inputs), std::ranges::size(inputs)})};
}

template <details::test_graph_target Tested, details::literal_input_range Inputs>
[[nodiscard]] auto test_graph(const Tested &tested, const Inputs &inputs) {
    return details::run_literal_inputs(
        tested, GraphRunOptions{},
        std::span<const TestGraphLiteralInput>{std::ranges::data(inputs), std::ranges::size(inputs)});
}

template <details::test_graph_target Tested, details::graph_run_options Options,
          details::literal_input_range Inputs>
[[nodiscard]] auto test_graph(const Tested &tested, const Options &options, const Inputs &inputs) {
    return details::run_literal_inputs(
        tested, options,
        std::span<const TestGraphLiteralInput>{std::ranges::data(inputs), std::ranges::size(inputs)});
}

template <details::test_graph_target Tested>
[[nodiscard]] auto test_graph(const Tested &tested, TestGraphLiteralInput input) {
    return details::run_literal_inputs(tested, GraphRunOptions{}, input);
}

template <details::test_graph_target Tested, details::graph_run_options Options>
[[nodiscard]] auto test_graph(const Tested &tested, const Options &options, TestGraphLiteralInput input) {
    return details::run_literal_inputs(tested, options, input);
}

template <details::test_graph_target Tested>
[[nodiscard]] auto test_graph(const Tested &tested, TestGraphLiteralInput first, TestGraphLiteralInput second) {
    return details::run_literal_inputs(tested, GraphRunOptions{}, first, second);
}

template <details::test_graph_target Tested, details::graph_run_options Options>
[[nodiscard]] auto test_graph(const Tested &tested, const Options &options, TestGraphLiteralInput first,
                              TestGraphLiteralInput second) {
    return details::run_literal_inputs(tested, options, first, second);
}

template <details::test_graph_target Tested>
[[nodiscard]] auto test_graph(const Tested &tested, TestGraphLiteralInput first, TestGraphLiteralInput second,
                              TestGraphLiteralInput third) {
    return details::run_literal_inputs(tested, GraphRunOptions{}, first, second, third);
}

template <details::test_graph_target Tested, details::graph_run_options Options>
[[nodiscard]] auto test_graph(const Tested &tested, const Options &options, TestGraphLiteralInput first,
                              TestGraphLiteralInput second, TestGraphLiteralInput third) {
    return details::run_literal_inputs(tested, options, first, second, third);
}

template <details::test_graph_target Tested>
[[nodiscard]] auto test_graph(const Tested &tested, TestGraphLiteralInput first, TestGraphLiteralInput second,
                              TestGraphLiteralInput third, TestGraphLiteralInput fourth) {
    return details::run_literal_inputs(tested, GraphRunOptions{}, first, second, third, fourth);
}

template <details::test_graph_target Tested, details::graph_run_options Options>
[[nodiscard]] auto test_graph(const Tested &tested, const Options &options, TestGraphLiteralInput first,
                              TestGraphLiteralInput second, TestGraphLiteralInput third,
                              TestGraphLiteralInput fourth) {
    return details::run_literal_inputs(tested, options, first, second, third, fourth);
}

template <details::test_graph_target Tested, typename... Inputs>
    requires(sizeof...(Inputs) > 0 &&
             (std::same_as<std::remove_cvref_t<Inputs>, TestGraphLiteralInput> && ...))
[[nodiscard]] auto test_graph(const Tested &tested, const Inputs &...inputs) {
    return details::run_literal_inputs(tested, GraphRunOptions{}, inputs...);
}

template <details::test_graph_target Tested, details::graph_run_options Options, typename... Inputs>
    requires(sizeof...(Inputs) > 0 &&
             (std::same_as<std::remove_cvref_t<Inputs>, TestGraphLiteralInput> && ...))
[[nodiscard]] auto test_graph(const Tested &tested, const Options &options, const Inputs &...inputs) {
    return details::run_literal_inputs(tested, options, inputs...);
}

template <typename Function>
    requires(!std::derived_from<std::remove_cvref_t<Function>, WiringTemplate> &&
             details::legacy_scalar_test_input<Function>)
[[nodiscard]] auto test_graph(
    const Function &function,
    std::initializer_list<std::optional<typename details::test_function<Function>::template input_scalar<0>>> input) {
    return details::run_legacy_scalar_input(function, GraphRunOptions{}, input);
}

template <typename Function, details::graph_run_options Options>
    requires(!std::derived_from<std::remove_cvref_t<Function>, WiringTemplate> &&
             details::legacy_scalar_test_input<Function>)
[[nodiscard]] auto test_graph(
    const Function &function, const Options &options,
    std::initializer_list<std::optional<typename details::test_function<Function>::template input_scalar<0>>> input) {
    return details::run_legacy_scalar_input(function, options, input);
}

} // namespace bg::testing

namespace bg {

template <typename T>
bool GraphRunResult<T>::equals(std::initializer_list<literal::TimeSeriesLiteral> expected) const
    requires std::same_as<T, void>
{
    return testing::details::LiteralMatcher::matches_sequence(
        captured_, captured_.type(), std::span{expected.begin(), expected.size()});
}

} // namespace bg
