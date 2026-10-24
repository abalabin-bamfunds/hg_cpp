#pragma once

#include <bg/literal/time_series_literal.hpp>
#include <bg/scalar/runtime_type.hpp>
#include <bg/scalar/value.hpp>
#include <bg/time_series/cpp_runtime_type.hpp>
#include <bg/time_series/time_series_runtime_type.hpp>

#include <initializer_list>
#include <concepts>
#include <optional>
#include <ranges>
#include <span>
#include <type_traits>
#include <utility>

namespace bg {

struct GraphRunInput {
    const ScalarRuntimeType &runtime_type;
    std::span<const std::optional<ScalarValue<>>> ticks;
};

// Borrows tick literals for a synchronous graph run.
class GraphRunLiteralInput {
  public:
    GraphRunLiteralInput(std::initializer_list<literal::TimeSeriesLiteral> ticks) noexcept
        : ticks{ticks.begin(), ticks.size()} {}

    GraphRunLiteralInput(std::span<const literal::TimeSeriesLiteral> ticks) noexcept : ticks{ticks} {}

    GraphRunLiteralInput(const TimeSeriesRuntimeType &runtime_type,
                         std::initializer_list<literal::TimeSeriesLiteral> ticks) noexcept
        : ticks{ticks.begin(), ticks.size()}, runtime_type{&runtime_type} {}

    GraphRunLiteralInput(const TimeSeriesRuntimeType &runtime_type,
                         std::span<const literal::TimeSeriesLiteral> ticks) noexcept
        : ticks{ticks}, runtime_type{&runtime_type} {}

    std::span<const literal::TimeSeriesLiteral> ticks;
    const TimeSeriesRuntimeType *runtime_type{};
};

template <std::ranges::view Range> struct GraphRunTicks {
    Range values;
};

template <std::ranges::viewable_range Range>
    requires(!std::same_as<std::remove_cvref_t<Range>, GraphRunLiteralInput>)
[[nodiscard]] auto ticks(Range &&values) {
    return GraphRunTicks<std::views::all_t<Range>>{std::views::all(std::forward<Range>(values))};
}

[[nodiscard]] inline GraphRunLiteralInput ticks(std::initializer_list<literal::TimeSeriesLiteral> values) noexcept {
    return GraphRunLiteralInput(values);
}

template <typename TimeSeries>
[[nodiscard]] GraphRunLiteralInput input(GraphRunLiteralInput source) {
    source.runtime_type = &cpp_time_series_runtime_type<TimeSeries>();
    return source;
}

[[nodiscard]] inline GraphRunLiteralInput input(const TimeSeriesRuntimeType &runtime_type,
                                               GraphRunLiteralInput source) {
    source.runtime_type = &runtime_type;
    return source;
}

[[nodiscard]] inline GraphRunLiteralInput input(const TimeSeriesType &time_series_type,
                                               GraphRunLiteralInput source) {
    source.runtime_type = &resolve_time_series_runtime_type(time_series_type);
    return source;
}

} // namespace bg
