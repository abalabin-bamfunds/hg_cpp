#pragma once

#include <bg/graph/details/wiring_callable.hpp>
#include <bg/graph/operator.hpp>
#include <bg/node/details/callable_signature.hpp>
#include <bg/node/input.hpp>
#include <bg/node/output.hpp>
#include <bg/type/details/time_series_type.hpp>
#include <bg/type/details/time_series_type_traits.hpp>
#include <bg/util/reflection.hpp>

#include <concepts>
#include <cstddef>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace bg::details {

template <typename Function, std::size_t... Index>
Signature make_operator_signature(util::symbol name, std::index_sequence<Index...>) {
    static_assert(std::same_as<typename callable<Function>::result_type, void>,
                  "an operator prototype lambda must return void and declare its output with Out");
    static_assert(callable_output_count<Function>(std::index_sequence<Index...>{}) <= 1,
                  "an operator prototype can have at most one output");
    return make_callable_signature<Function, Function>(name);
}

template <typename Function> Signature make_operator_signature(util::symbol name) {
    return make_operator_signature<Function>(name, std::make_index_sequence<callable<Function>::argument_count>{});
}

template <auto Function> struct declared_operator_scope {};

template <auto Function, std::size_t... Index>
Signature make_declared_operator_signature(std::index_sequence<Index...>) {
    using FunctionType = decltype(Function);
    using Result = std::remove_cvref_t<typename callable<FunctionType>::result_type>;
    static_assert(std::same_as<Result, void> || supported_time_series<Result>,
                  "an operator prototype function must return void or a time series type");

    auto parameters = std::vector<Parameter>{};
    parameters.reserve(sizeof...(Index));
    (add_callable_parameter<declared_operator_scope<Function>, FunctionType, Index>(parameters), ...);

    const auto name = util::symbol{util::function_name<Function>()};
    if constexpr (std::same_as<Result, void>) {
        return Signature{name, std::move(parameters)};
    } else {
        return Signature{name, std::move(parameters),
                         callable_output_type<declared_operator_scope<Function>, Result, sizeof...(Index)>()};
    }
}

template <auto Function> Signature make_declared_operator_signature() {
    using FunctionType = decltype(Function);
    return make_declared_operator_signature<Function>(
        std::make_index_sequence<callable<FunctionType>::argument_count>{});
}

template <typename Definition, std::size_t... Index>
consteval bool aggregate_operator_fields(std::index_sequence<Index...>) {
    return ((input_field<std::remove_cv_t<util::aggregate_member_type<Index, Definition>>>::value ||
             output_field<std::remove_cv_t<util::aggregate_member_type<Index, Definition>>>::value ||
             scalar_field<std::remove_cv_t<util::aggregate_member_type<Index, Definition>>>::value) &&
            ...);
}

template <typename Definition>
concept aggregate_operator_definition =
    std::is_aggregate_v<Definition> &&
    aggregate_operator_fields<Definition>(std::make_index_sequence<util::aggregate_member_count<Definition>>{}) &&
    output_field_count<Definition>(std::make_index_sequence<util::aggregate_member_count<Definition>>{}) <= 1;

} // namespace bg::details

namespace bg {

template <typename Prototype> [[nodiscard]] auto operator_(std::string_view name, Prototype) {
    using Function = std::remove_cvref_t<Prototype>;
    return Operator{details::make_operator_signature<Function>(util::symbol{name})};
}

template <details::aggregate_operator_definition Definition> [[nodiscard]] auto operator_(std::string_view name) {
    auto signature = details::make_signature<Definition>(
        util::symbol{name}, std::make_index_sequence<util::aggregate_member_count<Definition>>{});

    return Operator{std::move(signature)};
}

template <details::aggregate_operator_definition Definition> [[nodiscard]] auto operator_() {
    return operator_<Definition>(util::unqualified_type_name<Definition>());
}

template <auto Prototype> [[nodiscard]] auto operator_() {
    return Operator{details::make_declared_operator_signature<Prototype>()};
}

} // namespace bg
