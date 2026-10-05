#pragma once

#include <bg/time_series/cpp_runtime_type.hpp>
#include <bg/time_series/output_binding.hpp>

#include <concepts>
#include <utility>

namespace bg {

template <typename TimeSeries, OutputSpecification Specification = OutputSpecification::default_> class Out;

template <typename Output> struct output_declaration_traits;

template <typename TimeSeries, OutputSpecification Specification>
struct output_declaration_traits<Out<TimeSeries, Specification>> {
    static constexpr auto specification = Specification;

    using time_series_type = TimeSeries;
};

namespace details {

struct output_factory {
    template <typename TimeSeries, OutputSpecification Specification = OutputSpecification::default_, typename... Arguments>
    [[nodiscard]] static Out<TimeSeries, Specification> make(Arguments &&...arguments);
};

template <typename TimeSeries, OutputSpecification Specification, typename... Arguments>
Out<TimeSeries, Specification> output_factory::make(Arguments &&...arguments) {
    if constexpr (std::constructible_from<Out<TimeSeries, Specification>, Arguments...>) {
        return Out<TimeSeries, Specification>{std::forward<Arguments>(arguments)...};
    } else {
        return Out<TimeSeries, Specification>{TimeSeriesOutputBinding{std::forward<Arguments>(arguments)...}};
    }
}

} // namespace details

} // namespace bg
