#pragma once

#include <bg/type/time_series_guarantee.hpp>
#include <bg/util/compile_time_string.hpp>

#include <concepts>
#include <optional>
#include <type_traits>

namespace bg {

template <util::compile_time_string Name, auto Minimum = std::nullopt>
    requires(std::same_as<std::remove_cv_t<decltype(Minimum)>, std::nullopt_t> ||
             std::same_as<std::remove_cv_t<decltype(Minimum)>, TimeSeriesGuarantee>)
struct GuaranteeVariableDeclaration {
    static constexpr auto name = Name;
    static constexpr std::optional<TimeSeriesGuarantee> minimum{Minimum};
};

template <util::compile_time_string Name, auto Minimum = std::nullopt>
inline constexpr auto guarantee_variable = GuaranteeVariableDeclaration<Name, Minimum>{};

namespace details {

template <typename T> struct is_guarantee_variable_declaration : std::false_type {};

template <util::compile_time_string Name, auto Minimum>
    requires requires { GuaranteeVariableDeclaration<Name, Minimum>{}; }
struct is_guarantee_variable_declaration<GuaranteeVariableDeclaration<Name, Minimum>> : std::true_type {};

template <typename T>
concept guarantee_variable_declaration = is_guarantee_variable_declaration<std::remove_cv_t<T>>::value;

} // namespace details

} // namespace bg
