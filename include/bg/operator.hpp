#pragma once

#include <bg/graph/operator.hpp>
#include <bg/graph/type_mapping.hpp>
#include <bg/type/details/scalar_type.hpp>
#include <bg/type/details/time_series_type.hpp>

#include <array>
#include <concepts>
#include <cstddef>
#include <functional>
#include <initializer_list>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace bg::details {

struct default_operator_implementation {
    template <typename SelectOverload, typename... Arguments>
        requires(graph_code_argument<Arguments> && ...)
    static decltype(auto) invoke(const TypeMap &, SelectOverload &&select_overload, Arguments &&...arguments) {
        return std::invoke(std::forward<SelectOverload>(select_overload),
                           std::forward<Arguments>(arguments)...);
    }
};

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

template <typename Prototype, typename Implementation = details::default_operator_implementation,
          typename... TypeParameters>
class OperatorTemplate final
    : public Operator,
      public details::TypeParameterizedWiring<OperatorTemplate<Prototype, Implementation, TypeParameters...>,
                                              TypeParameters...> {
    using TypeMapping = details::TypeParameterizedWiring<
        OperatorTemplate<Prototype, Implementation, TypeParameters...>, TypeParameters...>;

  public:
    using TypeMapping::operator[];

    explicit OperatorTemplate(std::string_view name, std::span<const std::string_view> parameter_names = {})
        : Operator(details::operator_prototype<Prototype>::signature(util::symbol{name}, parameter_names)) {}

    OperatorTemplate(std::string_view name, std::initializer_list<std::string_view> parameter_names)
        : OperatorTemplate(name, std::span<const std::string_view>{parameter_names}) {}

    template <typename... Arguments>
    auto operator()(Arguments &&...arguments) const & {
        const auto type_map = TypeMap{};
        const auto select_overload = [this]<typename... Prepared>(Prepared &&...prepared) {
            return public_result(WiringTemplate::operator()(std::forward<Prepared>(prepared)...));
        };

        return Implementation::invoke(type_map, select_overload, std::forward<Arguments>(arguments)...);
    }

    auto operator()(std::span<const WiringPoint> arguments) const & {
        return public_result(WiringTemplate::operator()(arguments));
    }

    template <typename... Arguments>
    auto operator()(const TypeMap &type_map, Arguments &&...arguments) const & {
        const auto select_overload = [this, &type_map]<typename... Prepared>(Prepared &&...prepared) {
            return public_result(
                WiringTemplate::operator()(type_map, std::forward<Prepared>(prepared)...));
        };

        return Implementation::invoke(type_map, select_overload, std::forward<Arguments>(arguments)...);
    }

    auto operator()(const TypeMap &type_map, std::span<const WiringPoint> arguments) const & {
        return public_result(WiringTemplate::operator()(type_map, arguments));
    }

    template <typename... Mappings> auto operator[](Mappings...) && = delete;

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

namespace details {

template <typename PartsTuple, std::size_t PrototypeIndex, bool HasImplementation>
struct operator_implementation {
    using type = default_operator_implementation;
};

template <typename PartsTuple, std::size_t PrototypeIndex>
struct operator_implementation<PartsTuple, PrototypeIndex, true> {
    using type = std::tuple_element_t<PrototypeIndex + 1, PartsTuple>;
};

template <typename... Parts> struct operator_declaration {
    static constexpr auto part_count = sizeof...(Parts);
    static_assert(part_count != 0, "operator_ requires a prototype");

    using PartsTuple = std::tuple<Parts...>;

    static constexpr auto prototype_count = [] {
        return (std::size_t{} + ... + static_cast<std::size_t>(std::is_function_v<Parts>));
    }();
    static_assert(prototype_count == 1, "operator_ requires exactly one function prototype");

    static constexpr auto prototype_index = [] {
        constexpr auto functions = std::array{std::is_function_v<Parts>...};
        for (auto index = std::size_t{}; index < functions.size(); ++index) {
            if (functions[index]) {
                return index;
            }
        }
        return functions.size();
    }();

    static_assert(part_count - prototype_index <= 2,
                  "operator_ accepts only an optional implementation after its prototype");

    using Prototype = std::tuple_element_t<prototype_index, PartsTuple>;
    using Implementation = typename operator_implementation<PartsTuple, prototype_index,
                                                            prototype_index + 1 < part_count>::type;

    template <std::size_t... Index> static consteval bool valid_type_parameters(std::index_sequence<Index...>) {
        return (type_parameter_declaration<std::tuple_element_t<Index, PartsTuple>> && ...);
    }

    static_assert(valid_type_parameters(std::make_index_sequence<prototype_index>{}),
                  "types before an operator prototype must be type variables");

    template <std::size_t... Index>
    static auto make(std::index_sequence<Index...>)
        -> OperatorTemplate<Prototype, Implementation, std::tuple_element_t<Index, PartsTuple>...>;

    using type = decltype(make(std::make_index_sequence<prototype_index>{}));
};

} // namespace details

template <typename... Declaration>
using operator_ = typename details::operator_declaration<Declaration...>::type;

} // namespace bg
