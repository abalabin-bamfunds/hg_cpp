#pragma once

#include <bg/scalar/canonical.hpp>
#include <bg/scalar/cpp_runtime_type.hpp>
#include <bg/graph/wiring_parameter.hpp>
#include <bg/type/details/scalar_type.hpp>
#include <bg/type/signature.hpp>

#include <array>
#include <cstddef>
#include <initializer_list>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace bg::details {

template <typename Declaration> const Type &signature_declaration_type() {
    using Value = std::remove_cv_t<Declaration>;

    if constexpr (requires { Value::type(); }) {
        return Value::type();
    } else {
        return cpp_scalar_type<canonical_scalar_type_t<Value>>();
    }
}

template <typename Result, typename... Arguments>
Signature make_typed_signature(util::symbol name, std::span<const std::string_view> parameter_names,
                               Result (*)(Arguments...), bool names_are_explicit = true) {
    if (parameter_names.size() != sizeof...(Arguments)) {
        throw std::invalid_argument{"a typed signature requires one name for every parameter"};
    }

    auto parameters = std::vector<Parameter>{};
    parameters.reserve(sizeof...(Arguments));
    if constexpr (sizeof...(Arguments) != 0) {
        auto index = std::size_t{};
        ([&] {
            auto parameter = Parameter{util::symbol{parameter_names[index]},
                                       &signature_declaration_type<Arguments>()};
            parameter.name_is_explicit = names_are_explicit;
            parameters.push_back(std::move(parameter));
            ++index;
        }(), ...);
    }

    if constexpr (std::same_as<Result, void>) {
        return {name, std::move(parameters)};
    } else {
        const auto &output = signature_declaration_type<Result>();
        const auto *time_series = type_cast<TimeSeriesType>(&output);
        if (time_series == nullptr) {
            throw std::invalid_argument{"a typed signature output must be a time series type"};
        }
        return {name, std::move(parameters), *time_series};
    }
}

template <typename Result, typename... Arguments>
Signature make_typed_signature(util::symbol name, Result (*)(Arguments...)) {
    auto names = std::array<std::string, sizeof...(Arguments)>{};
    for (auto index = std::size_t{}; index < names.size(); ++index) {
        names[index] = std::to_string(index);
    }
    auto views = std::array<std::string_view, sizeof...(Arguments)>{};
    for (auto index = std::size_t{}; index < names.size(); ++index) {
        views[index] = names[index];
    }
    return make_typed_signature(name, std::span<const std::string_view>{views},
                                static_cast<Result (*)(Arguments...)>(nullptr), false);
}

template <typename Prototype> struct typed_prototype;

template <typename Result, typename... Arguments> struct typed_prototype<Result(Arguments...)> {
    static constexpr bool has_output = !std::same_as<Result, void>;
    static constexpr std::size_t argument_count = sizeof...(Arguments);

    static Signature signature(util::symbol name, std::span<const std::string_view> parameter_names) {
        return make_typed_signature(name, parameter_names, static_cast<Result (*)(Arguments...)>(nullptr));
    }

    static Signature signature(util::symbol name, std::initializer_list<WiringParameter> parameters) {
        auto result = make_typed_signature(name, static_cast<Result (*)(Arguments...)>(nullptr));
        return result.with_parameters(std::span<const WiringParameter>{parameters});
    }
};

} // namespace bg::details
