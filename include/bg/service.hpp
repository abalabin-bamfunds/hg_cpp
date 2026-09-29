#pragma once

#include <bg/graph/details/typed_signature.hpp>
#include <bg/graph/service.hpp>
#include <bg/graph/type_mapping.hpp>
#include <bg/type/type_parameter.hpp>

#include <initializer_list>
#include <span>
#include <stdexcept>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

namespace bg {

template <ServiceKind Kind, typename Prototype, typename... TypeParameters>
class ServiceTemplate final
    : public Service,
      public details::TypeParameterizedWiring<ServiceTemplate<Kind, Prototype, TypeParameters...>,
                                              TypeParameters...> {
    using TypeMapping = details::TypeParameterizedWiring<ServiceTemplate<Kind, Prototype, TypeParameters...>,
                                                         TypeParameters...>;

  public:
    using TypeMapping::operator[];

    explicit ServiceTemplate(std::string_view name, std::span<const std::string_view> parameter_names = {})
        : Service{Kind, details::typed_prototype<Prototype>::signature(util::symbol{name}, parameter_names)} {}

    ServiceTemplate(std::string_view name, std::initializer_list<std::string_view> parameter_names)
        : ServiceTemplate{name, std::span<const std::string_view>{parameter_names}} {}

    template <typename... Arguments> auto operator()(Arguments &&...arguments) const & {
        return public_result(WiringTemplate::operator()(std::forward<Arguments>(arguments)...));
    }

    auto operator()(std::span<const WiringPoint> arguments) const & {
        return public_result(WiringTemplate::operator()(arguments));
    }

    template <typename... Arguments>
    auto operator()(const TypeMap &type_map, Arguments &&...arguments) const & {
        return public_result(WiringTemplate::operator()(type_map, std::forward<Arguments>(arguments)...));
    }

    auto operator()(const TypeMap &type_map, std::span<const WiringPoint> arguments) const & {
        return public_result(WiringTemplate::operator()(type_map, arguments));
    }

    template <typename... Mappings> auto operator[](Mappings...) && = delete;
    template <typename... Arguments> auto operator()(Arguments &&...) const && = delete;

  private:
    static auto public_result(std::optional<WiringPoint> result) {
        if constexpr (details::typed_prototype<Prototype>::has_output) {
            if (!result) {
                throw std::logic_error{"a service with an output did not produce a wiring point"};
            }
            return wp<>{*result};
        } else if (result) {
            throw std::logic_error{"a service without an output produced a wiring point"};
        }
    }
};

namespace details {

template <ServiceKind Kind, typename... Declaration> struct service_declaration {
    using Parsed = type_parameterized_declaration<Declaration...>;
    using Types = typename Parsed::Types;
    using Prototype = typename Parsed::Definition;

    static_assert(std::is_function_v<Prototype>, "a service declaration must end with a function prototype");
    static_assert(Parsed::has_valid_parameters,
                  "types before a service prototype must be type variables");

    template <std::size_t... Index>
    static auto make(std::index_sequence<Index...>)
        -> ServiceTemplate<Kind, Prototype, std::tuple_element_t<Index, Types>...>;

    using type = decltype(make(std::make_index_sequence<sizeof...(Declaration) - 1>{}));
};

} // namespace details

template <typename... Declaration>
using reference_service = typename details::service_declaration<ServiceKind::reference, Declaration...>::type;

template <typename... Declaration>
using subscription_service = typename details::service_declaration<ServiceKind::subscription, Declaration...>::type;

template <typename... Declaration>
using compute_service = typename details::service_declaration<ServiceKind::compute, Declaration...>::type;

} // namespace bg
