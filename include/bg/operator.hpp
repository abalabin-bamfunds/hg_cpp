#pragma once

#include <bg/graph/details/typed_signature.hpp>
#include <bg/graph/operator.hpp>
#include <bg/graph/type_mapping.hpp>
#include <bg/time_series/time_series_type.hpp>
#include <bg/type/type_parameter.hpp>

#include <array>
#include <concepts>
#include <cstddef>
#include <functional>
#include <initializer_list>
#include <optional>
#include <span>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace bg::details {

template <typename Prototype> struct operator_typed_prototype;

template <typename Result, typename... Arguments>
struct operator_typed_prototype<Result(Arguments...)>
    : typed_prototype<std::conditional_t<
          scalar_type_mapped<canonical_scalar_type_t<Result>> &&
              scalar_declaration_traits<canonical_scalar_type_t<Result>>::supports_time_series,
          TS<canonical_scalar_type_t<Result>>, Result>(Arguments...)> {};

struct default_operator_implementation {
    template <typename SelectOverload, typename... Arguments>
        requires(graph_code_argument<Arguments> && ...)
    static decltype(auto) invoke(const TypeMap &, SelectOverload &&select_overload, Arguments &&...arguments) {
        return std::invoke(std::forward<SelectOverload>(select_overload),
                           std::forward<Arguments>(arguments)...);
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

    auto explicit_type_parameters() const {
        return std::array<const Type *, sizeof...(TypeParameters)>{&details::type_parameter<TypeParameters>()...};
    }

    explicit OperatorTemplate(std::string_view name, std::span<const std::string_view> parameter_names = {})
        : Operator(details::operator_typed_prototype<Prototype>::signature(util::symbol{name}, parameter_names),
                   declared_type_parameters()) {}

    OperatorTemplate(std::string_view name, std::initializer_list<WiringParameter> parameters)
        : Operator(details::operator_typed_prototype<Prototype>::signature(util::symbol{name}, parameters),
                   declared_type_parameters()) {}

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
    static std::span<const Variable *const> declared_type_parameters() {
        static const auto parameters = std::array<const Variable *, sizeof...(TypeParameters)>{
            &details::type_parameter<TypeParameters>()...};
        return parameters;
    }

    static auto public_result(std::optional<WiringPoint> result) {
        if constexpr (details::operator_typed_prototype<Prototype>::has_output) {
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
