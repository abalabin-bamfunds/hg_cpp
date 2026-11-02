#pragma once

#include <hg/time_series/details/type_access.hpp>
#include <hg/time_series/guarantee_variable.hpp>
#include <hg/type/time_series_guarantee.hpp>
#include <hg/util/compile_time_string.hpp>

#include <concepts>
#include <type_traits>

namespace hg {

template <typename Variable, auto Guarantee>
struct TimeSeriesVariableWithGuarantee
    : details::time_series_type_access<TimeSeriesVariableWithGuarantee<Variable, Guarantee>> {};

template <util::compile_time_string Name>
struct TimeSeriesVariable : details::time_series_type_access<TimeSeriesVariable<Name>> {
    static constexpr auto name = Name;

    template <auto Guarantee>
        requires std::same_as<std::remove_cv_t<decltype(Guarantee)>, TimeSeriesGuarantee> ||
                     details::guarantee_variable_declaration<decltype(Guarantee)>
    using with = TimeSeriesVariableWithGuarantee<TimeSeriesVariable, Guarantee>;
};

using TS_T = TimeSeriesVariable<"TS_T">;
using OUT = TimeSeriesVariable<"OUT">;

namespace details {

template <typename T> struct is_time_series_variable : std::false_type {};

template <util::compile_time_string Name> struct is_time_series_variable<TimeSeriesVariable<Name>> : std::true_type {};

template <typename Variable, auto Guarantee>
struct is_time_series_variable<TimeSeriesVariableWithGuarantee<Variable, Guarantee>> : std::true_type {};

template <typename T>
concept time_series_variable = is_time_series_variable<std::remove_cv_t<T>>::value;

} // namespace details

} // namespace hg
