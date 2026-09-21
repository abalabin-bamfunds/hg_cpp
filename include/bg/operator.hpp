#pragma once

#include <bg/graph/operator.hpp>
#include <bg/type/details/scalar_type.hpp>
#include <bg/type/details/time_series_type.hpp>

#include <concepts>
#include <cstddef>
#include <initializer_list>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace bg::details {

struct direct_operator_invocation {};

template <typename Declaration> const Type &operator_declaration_type() {
    using Value = std::remove_cv_t<Declaration>;

    if constexpr (requires { Value::type(); }) {
        return Value::type();
    } else {
        return cpp_scalar_type<canonical_scalar_type_t<Value>>();
    }
}

template <typename Result, typename... Arguments>
Signature make_typed_operator_signature(util::symbol name, std::span<const std::string_view> parameter_names,
                                        Result (*)(Arguments...)) {
    if (parameter_names.size() != sizeof...(Arguments)) {
        throw std::invalid_argument{"an operator prototype requires one name for every parameter"};
    }

    auto parameters = std::vector<Parameter>{};
    parameters.reserve(sizeof...(Arguments));
    if constexpr (sizeof...(Arguments) != 0) {
        auto index = std::size_t{};
        (
            [&] {
                parameters.push_back({util::symbol{parameter_names[index]}, &operator_declaration_type<Arguments>()});
                ++index;
            }(),
            ...);
    }

    if constexpr (std::same_as<Result, void>) {
        return {name, std::move(parameters)};
    } else {
        const auto &output = operator_declaration_type<Result>();
        const auto *time_series = type_cast<TimeSeriesType>(&output);
        if (time_series == nullptr) {
            throw std::invalid_argument{"an operator prototype output must be a time series type"};
        }
        return {name, std::move(parameters), *time_series};
    }
}

template <typename Prototype> struct operator_prototype;

template <typename Result, typename... Arguments> struct operator_prototype<Result(Arguments...)> {
    static constexpr bool has_output = !std::same_as<Result, void>;

    static Signature signature(util::symbol name, std::span<const std::string_view> parameter_names) {
        return make_typed_operator_signature(name, parameter_names, static_cast<Result (*)(Arguments...)>(nullptr));
    }
};

} // namespace bg::details

namespace bg {

template <typename Prototype, typename Invocation = details::direct_operator_invocation>
class operator_ : public Operator {
  public:
    explicit operator_(std::string_view name, std::span<const std::string_view> parameter_names = {})
        : Operator(details::operator_prototype<Prototype>::signature(util::symbol{name}, parameter_names)) {}

    operator_(std::string_view name, std::initializer_list<std::string_view> parameter_names)
        : operator_(name, std::span<const std::string_view>{parameter_names}) {}

    template <typename... Arguments>
        requires((std::same_as<Invocation, details::direct_operator_invocation> &&
                  (details::graph_code_argument<Arguments> && ...)) ||
                 (!std::same_as<Invocation, details::direct_operator_invocation> &&
                  requires(const operator_ &operation, Arguments &&...arguments) {
                      Invocation::invoke(operation, std::forward<Arguments>(arguments)...);
                  }))
    auto operator()(Arguments &&...arguments) const & {
        if constexpr (std::same_as<Invocation, details::direct_operator_invocation>) {
            return public_result(WiringTemplate::operator()(std::forward<Arguments>(arguments)...));
        } else {
            return Invocation::invoke(*this, std::forward<Arguments>(arguments)...);
        }
    }

    auto operator()(std::span<const WiringPoint> arguments) const & {
        return public_result(WiringTemplate::operator()(arguments));
    }

    template <typename... Arguments>
        requires(details::graph_code_argument<Arguments> && ...)
    auto operator()(const TypeMap &type_map, Arguments &&...arguments) const & {
        return public_result(WiringTemplate::operator()(type_map, std::forward<Arguments>(arguments)...));
    }

    auto operator()(const TypeMap &type_map, std::span<const WiringPoint> arguments) const & {
        return public_result(WiringTemplate::operator()(type_map, arguments));
    }

    template <typename... Arguments> auto operator()(Arguments &&...) const && = delete;

  private:
    static auto public_result(std::optional<WiringPoint> result) {
        if constexpr (details::operator_prototype<Prototype>::has_output) {
            return result ? wp<>{*result} : wp<>{};
        } else if (result) {
            throw std::logic_error{"an outputless operator overload produced an output"};
        }
    }
};

} // namespace bg
