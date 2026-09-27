#pragma once

#include <bg/scalar/value.hpp>
#include <bg/testing/details/test_graph.hpp>

#include <cstddef>
#include <initializer_list>
#include <optional>
#include <ranges>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace bg::testing {

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
        std::initializer_list<std::optional<std::remove_cvref_t<bg::details::scalar_value_t<Value>>>> expected) const {
        if (values_.size() != expected.size()) {
            return false;
        }

        auto actual = values_.begin();
        for (const auto &value : expected) {
            if (actual->has_value() != value.has_value()) {
                return false;
            }
            if (value) {
                decltype(auto) actual_value = scalar_cast<Value>(**actual).value();
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
        std::initializer_list<std::optional<std::remove_cvref_t<bg::details::scalar_value_t<Value>>>> expected) const {
        return equals(expected);
    }

  private:
    std::vector<std::optional<ScalarValue<>>> values_;
};

// Each input range defines updates by outer cycle. An empty optional skips its cycle.
// Each result position owns the output update from the corresponding cycle, if any.
template <std::ranges::contiguous_range... Inputs>
[[nodiscard]] TestGraphResult<> test_graph(const WiringTemplate &tested, const Inputs &...inputs) {
    return TestGraphResult<>{details::run_template(tested, inputs...)};
}

template <typename Function, std::ranges::contiguous_range... Inputs>
    requires(!std::derived_from<std::remove_cvref_t<Function>, WiringTemplate>)
[[nodiscard]] auto test_graph(const Function &function, const Inputs &...inputs) {
    using FunctionInfo = details::test_function<Function>;
    static_assert(FunctionInfo::callable::argument_count == sizeof...(Inputs));
    static_assert(bg::details::is_typed_wiring_point<typename FunctionInfo::result>);

    const auto ranges = std::tie(inputs...);
    auto values = details::run_function(function, ranges, std::index_sequence_for<Inputs...>{});

    return TestGraphResult<typename FunctionInfo::output_scalar>{std::move(values)};
}

template <typename Function>
    requires(!std::derived_from<std::remove_cvref_t<Function>, WiringTemplate>)
[[nodiscard]] auto test_graph(
    const Function &function,
    std::initializer_list<std::optional<typename details::test_function<Function>::template input_scalar<0>>> input) {
    using FunctionInfo = details::test_function<Function>;
    static_assert(FunctionInfo::callable::argument_count == 1);

    const auto inputs = std::tie(input);
    auto values = details::run_function(function, inputs, std::index_sequence<0>{});

    return TestGraphResult<typename FunctionInfo::output_scalar>{std::move(values)};
}

} // namespace bg::testing
