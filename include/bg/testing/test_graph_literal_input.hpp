#pragma once

#include <bg/literal/time_series_literal.hpp>
#include <bg/time_series/cpp_runtime_type.hpp>
#include <bg/time_series/time_series_runtime_type.hpp>

#include <initializer_list>
#include <span>

namespace bg::testing {

// Borrows tick literals and their sources for a synchronous test_graph call.
class TestGraphLiteralInput {
  public:
    TestGraphLiteralInput(std::initializer_list<literal::TimeSeriesLiteral> ticks) noexcept
        : ticks{ticks.begin(), ticks.size()} {}

    TestGraphLiteralInput(std::span<const literal::TimeSeriesLiteral> ticks) noexcept : ticks{ticks} {}

    TestGraphLiteralInput(const TimeSeriesRuntimeType &runtime_type,
                          std::initializer_list<literal::TimeSeriesLiteral> ticks) noexcept
        : ticks{ticks.begin(), ticks.size()}, runtime_type{&runtime_type} {}

    TestGraphLiteralInput(const TimeSeriesRuntimeType &runtime_type,
                          std::span<const literal::TimeSeriesLiteral> ticks) noexcept
        : ticks{ticks}, runtime_type{&runtime_type} {}

    std::span<const literal::TimeSeriesLiteral> ticks;
    const TimeSeriesRuntimeType *runtime_type{};
};

template <typename TimeSeries>
[[nodiscard]] TestGraphLiteralInput input(TestGraphLiteralInput source) {
    source.runtime_type = &cpp_time_series_runtime_type<TimeSeries>();
    return source;
}

[[nodiscard]] inline TestGraphLiteralInput input(const TimeSeriesRuntimeType &runtime_type,
                                                TestGraphLiteralInput source) {
    source.runtime_type = &runtime_type;
    return source;
}

[[nodiscard]] inline TestGraphLiteralInput input(const TimeSeriesType &time_series_type,
                                                TestGraphLiteralInput source) {
    source.runtime_type = &resolve_time_series_runtime_type(time_series_type);
    return source;
}

} // namespace bg::testing
