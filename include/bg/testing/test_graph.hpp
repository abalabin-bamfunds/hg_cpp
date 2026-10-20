#pragma once

#include <bg/evaluation_profiler.hpp>
#include <bg/graph_run_options.hpp>
#include <bg/scalar/value.hpp>
#include <bg/testing/details/test_graph.hpp>
#include <bg/testing/test_graph_literal_input.hpp>
#include <bg/testing/test_graph_input.hpp>

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

template <typename T = void> class TestGraphResult {
  public:
    explicit TestGraphResult(details::CapturedValues captured) : captured_{std::move(captured)} {}

    std::size_t size() const noexcept { return captured_.records.size(); }

    // Visit one captured tick without taking ownership of its encoded values. The reader is valid only for the
    // duration of the callback. An invalid tick contains only its header.
    template <typename Visitor> bool visit_tick(std::size_t index, Visitor &&visitor) const {
        const auto &record = captured_.records.at(index);
        if (!record) {
            return false;
        }

        auto reader = util::buffer_reader{*record};
        const auto header = bg::details::TickHeader::read(reader);
        if (header.operation != bg::details::TickOperation::tick) {
            throw std::invalid_argument{"test_graph result tick does not start with a tick header"};
        }

        const auto *type = captured_.type();
        if (type == nullptr) {
            throw std::logic_error{"test_graph result has captured bytes without a time-series type"};
        }

        std::invoke(std::forward<Visitor>(visitor), *type, header.valid, reader);
        return true;
    }

    auto operator[](std::size_t index) const {
        if constexpr (std::same_as<T, void>) {
            auto value = std::optional<ScalarValue<>>{};
            visit_scalar(index, [&](Scalar<> actual) {
                auto memory = allocator{};
                value.emplace(actual, memory);
            });
            return value;
        } else {
            auto value = std::optional<ScalarValue<T>>{};
            visit_scalar(index, [&](Scalar<> actual) {
                auto memory = allocator{};
                value.emplace(scalar_cast<T>(actual), memory);
            });
            return value;
        }
    }

    template <typename Value = T>
        requires(!std::same_as<Value, void>)
    bool equals(
        std::initializer_list<std::optional<std::remove_cvref_t<typename bg::scalar_storage_traits<Value>::value_type>>>
            expected) const {
        if (size() != expected.size()) {
            return false;
        }

        for (auto index = std::size_t{}; index < expected.size(); ++index) {
            const auto &value = *(expected.begin() + static_cast<std::ptrdiff_t>(index));
            auto matched = false;
            const auto present = visit_scalar(index, [&](Scalar<> actual) {
                if (!value) {
                    return;
                }
                const auto actual_scalar = scalar_cast<Value>(actual);
                decltype(auto) actual_value = actual_scalar.value();
                matched = scalar_storage_traits<Value>::equals(actual_value, *value);
            });
            if (present != value.has_value() || (value && !matched)) {
                return false;
            }
        }

        return true;
    }

    template <typename Value = T>
        requires(!std::same_as<Value, void>)
    bool operator==(
        std::initializer_list<std::optional<std::remove_cvref_t<typename bg::scalar_storage_traits<Value>::value_type>>>
            expected) const {
        return equals(expected);
    }

    bool equals(std::initializer_list<literal::TimeSeriesLiteral> expected) const
        requires std::same_as<T, void>
    {
        return details::LiteralMatcher::matches_sequence(captured_.records, captured_.type(),
                                                         std::span{expected.begin(), expected.size()});
    }

    bool operator==(std::initializer_list<literal::TimeSeriesLiteral> expected) const
        requires std::same_as<T, void>
    {
        return equals(expected);
    }

  private:
    template <typename Function> bool visit_scalar(std::size_t index, Function &&function) const {
        auto present = false;
        visit_tick(index, [&](const TimeSeriesType &type, bool valid, util::buffer_reader &reader) {
            if (!valid) {
                return;
            }

            const auto *value_type = type_cast<TimeSeriesValueType>(&type);
            if (value_type == nullptr) {
                throw std::invalid_argument{"test_graph result tick is a collection"};
            }
            present = bg::details::TimeSeriesTickScalarRead::visit(reader, value_type->value_type(),
                                                                    std::forward<Function>(function));
        });
        return present;
    }

    details::CapturedValues captured_;
};

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
