#pragma once

#include <bg/evaluation_profiler.hpp>
#include <bg/scalar/value.hpp>
#include <bg/testing/details/test_graph.hpp>
#include <bg/testing/test_graph_literal_input.hpp>
#include <bg/testing/test_graph_input.hpp>
#include <bg/testing/test_graph_options.hpp>

#include <array>
#include <cstddef>
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
    explicit TestGraphResult(std::vector<std::optional<ScalarValue<>>> values) : values_{std::move(values)} {}

    std::size_t size() const noexcept { return values_.size(); }

    auto operator[](std::size_t index) const {
        if (!values_[index]) {
            if constexpr (std::same_as<T, void>) {
                return std::optional<Scalar<>>{};
            } else {
                return std::optional<Scalar<T>>{};
            }
        }

        if constexpr (std::same_as<T, void>) {
            return std::optional{values_[index]->view()};
        } else {
            return std::optional{scalar_cast<T>(*values_[index])};
        }
    }

    template <typename Value = T>
        requires(!std::same_as<Value, void>)
    bool equals(
        std::initializer_list<std::optional<std::remove_cvref_t<typename bg::scalar_storage_traits<Value>::value_type>>>
            expected) const {
        if (values_.size() != expected.size()) {
            return false;
        }

        auto actual = values_.begin();
        for (const auto &value : expected) {
            if (actual->has_value() != value.has_value()) {
                return false;
            }
            if (value) {
                const auto actual_scalar = scalar_cast<Value>(**actual);
                decltype(auto) actual_value = actual_scalar.value();
                if (!scalar_storage_traits<Value>::equals(actual_value, *value)) {
                    return false;
                }
            }

            ++actual;
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

  private:
    std::vector<std::optional<ScalarValue<>>> values_;
};

namespace details {

template <typename Tested>
[[nodiscard]] auto run_literal_inputs(const Tested &tested, const TestGraphOptions &options,
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
[[nodiscard]] auto run_literal_inputs(const Tested &tested, const TestGraphOptions &options,
                                     const Inputs &...inputs) {
    static_assert((std::same_as<std::remove_cvref_t<Inputs>, TestGraphLiteralInput> && ...));
    const auto values = std::array<TestGraphLiteralInput, sizeof...(Inputs)>{inputs...};
    return run_literal_inputs(tested, options,
                              std::span<const TestGraphLiteralInput>{values});
}

} // namespace details

struct TestGraph {
    TestGraphOptions options{};

    [[nodiscard]] constexpr auto operator[](TestGraphOptions selected) const noexcept -> TestGraph {
        return TestGraph{selected};
    }

    template <details::runtime_input_range Inputs>
    [[nodiscard]] TestGraphResult<> operator()(const WiringTemplate &tested, const Inputs &inputs) const {
        return TestGraphResult<>{details::run_runtime_template(
            tested, options,
            std::span<const TestGraphInput>{std::ranges::data(inputs), std::ranges::size(inputs)})};
    }

    template <details::test_graph_target Tested, details::literal_input_range Inputs>
    [[nodiscard]] auto operator()(const Tested &tested, const Inputs &inputs) const {
        return details::run_literal_inputs(
            tested, options,
            std::span<const TestGraphLiteralInput>{std::ranges::data(inputs), std::ranges::size(inputs)});
    }

    template <details::test_graph_target Tested, typename... Inputs>
        requires(sizeof...(Inputs) > 0 &&
                 (std::same_as<std::remove_cvref_t<Inputs>, TestGraphLiteralInput> && ...))
    [[nodiscard]] auto operator()(const Tested &tested, const Inputs &...inputs) const {
        return details::run_literal_inputs(tested, options, inputs...);
    }

    template <details::test_graph_target Tested>
    [[nodiscard]] auto operator()(const Tested &tested, TestGraphLiteralInput input) const {
        return details::run_literal_inputs(tested, options, input);
    }

    template <details::test_graph_target Tested>
    [[nodiscard]] auto operator()(const Tested &tested, TestGraphLiteralInput first,
                                  TestGraphLiteralInput second) const {
        return details::run_literal_inputs(tested, options, first, second);
    }

    template <details::test_graph_target Tested>
    [[nodiscard]] auto operator()(const Tested &tested, TestGraphLiteralInput first,
                                  TestGraphLiteralInput second, TestGraphLiteralInput third) const {
        return details::run_literal_inputs(tested, options, first, second, third);
    }

    template <details::test_graph_target Tested>
    [[nodiscard]] auto operator()(const Tested &tested, TestGraphLiteralInput first,
                                  TestGraphLiteralInput second, TestGraphLiteralInput third,
                                  TestGraphLiteralInput fourth) const {
        return details::run_literal_inputs(tested, options, first, second, third, fourth);
    }

    template <details::test_graph_target Tested, std::ranges::contiguous_range... Inputs>
        requires((!details::literal_input_range<Inputs>) && ...) &&
                ((!details::runtime_input_range<Inputs>) && ...)
    [[nodiscard]] auto operator()(const Tested &tested, const Inputs &...inputs) const {
        if constexpr (std::derived_from<std::remove_cvref_t<Tested>, WiringTemplate>) {
            return TestGraphResult<>{details::run_template(tested, options, inputs...)};
        } else {
            using FunctionInfo = details::test_function<Tested>;
            static_assert(FunctionInfo::callable::argument_count == sizeof...(Inputs));
            static_assert(bg::details::is_typed_wiring_point<typename FunctionInfo::result>);

            const auto ranges = std::tie(inputs...);
            auto values = details::run_function(tested, options, ranges,
                                                std::make_index_sequence<sizeof...(Inputs)>{});
            return TestGraphResult<typename FunctionInfo::output_scalar>{std::move(values)};
        }
    }

    template <typename Function>
        requires(!std::derived_from<std::remove_cvref_t<Function>, WiringTemplate> &&
                 details::legacy_scalar_test_input<Function>)
    [[nodiscard]] auto operator()(
        const Function &function,
        std::initializer_list<std::optional<typename details::test_function<Function>::template input_scalar<0>>>
            input) const {
        using FunctionInfo = details::test_function<Function>;

        const auto inputs = std::tie(input);
        auto values = details::run_function(function, options, inputs, std::index_sequence<0>{});
        return TestGraphResult<typename FunctionInfo::output_scalar>{std::move(values)};
    }
};

inline constexpr TestGraph test_graph{};

} // namespace bg::testing
