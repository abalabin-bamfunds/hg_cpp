#pragma once

#include <bg/scalar/type.hpp>
#include <bg/type/type_map.hpp>
#include <bg/type/type_parameter.hpp>
#include <bg/type/type_resolution_error.hpp>

#include <cstddef>
#include <tuple>
#include <type_traits>
#include <utility>

namespace bg {

template <typename Declaration> struct type_token;

namespace details {

struct mapped_wiring_access;

template <typename Parameter, typename Value> struct type_mapping {};

template <typename T> struct is_type_mapping : std::false_type {};

template <typename Parameter, typename Value>
struct is_type_mapping<type_mapping<Parameter, Value>> : std::true_type {};

template <typename T> inline constexpr bool is_type_mapping_v = is_type_mapping<std::remove_cv_t<T>>::value;

template <typename T> struct is_type_token : std::false_type {};

template <typename Declaration> struct is_type_token<type_token<Declaration>> : std::true_type {};

template <typename T> inline constexpr bool is_type_token_v = is_type_token<std::remove_cv_t<T>>::value;

template <typename Declaration> const Type &mapping_value_type() {
    using Value = std::remove_cv_t<Declaration>;

    if constexpr (requires { Value::type(); }) {
        return Value::type();
    } else {
        return cpp_scalar_type<Value>();
    }
}

template <type_parameter_declaration Parameter, typename Value>
void add_type_mapping(TypeMap &type_map) {
    const auto &supplied = mapping_value_type<Value>();
    const auto &parameter = type_parameter<Parameter>();

    if (!supplied.is_resolved()) {
        throw TypeResolutionError{TypeResolutionErrorCode::invalid_type_map, parameter.name(),
                                  "an explicit type argument must be resolved", &parameter, &supplied};
    }
    if (!parameter.match(supplied, type_map)) {
        throw TypeResolutionError{TypeResolutionErrorCode::mismatch, parameter.name(),
                                  "an explicit type argument does not satisfy its type parameter", &parameter,
                                  &supplied};
    }
}

template <typename Parameters, std::size_t Position> void add_type_mappings(TypeMap &) {}

template <typename Parameters, std::size_t Position, typename Parameter, typename Value, typename... Rest>
void add_type_mappings(TypeMap &type_map, type_mapping<Parameter, Value>, Rest... rest) {
    static_assert(type_parameter_declaration<Parameter>,
                  "the left side of an explicit type mapping must be a type variable");
    add_type_mapping<Parameter, Value>(type_map);
    add_type_mappings<Parameters, Position>(type_map, rest...);
}

template <typename Parameters, std::size_t Position, typename Value, typename... Rest>
void add_type_mappings(TypeMap &type_map, type_token<Value>, Rest... rest) {
    static_assert(Position < std::tuple_size_v<Parameters>,
                  "too many positional type arguments for this wiring construct");
    using Parameter = std::tuple_element_t<Position, Parameters>;
    add_type_mapping<Parameter, Value>(type_map);
    add_type_mappings<Parameters, Position + 1>(type_map, rest...);
}

template <typename... Parameters, typename... Mappings>
TypeMap make_type_map(Mappings... mappings) {
    static_assert((type_parameter_declaration<Parameters> && ...),
                  "positional type parameters must be type variables");
    static_assert(((is_type_token_v<Mappings> || is_type_mapping_v<Mappings>) && ...),
                  "a mapped wiring call accepts only type arguments and type mappings");

    auto result = TypeMap{};
    add_type_mappings<std::tuple<Parameters...>, 0>(result, mappings...);
    return result;
}

template <typename... Parameters, typename... Mappings>
TypeMap extend_type_map(const TypeMap &base, Mappings... mappings) {
    auto result = base.copy();
    add_type_mappings<std::tuple<Parameters...>, 0>(result, mappings...);
    return result;
}

} // namespace details

template <typename Declaration> struct type_token {
    template <typename Value>
    constexpr auto operator=(type_token<Value>) const noexcept
        -> details::type_mapping<Declaration, Value> {
        return {};
    }
};

template <typename Declaration> inline constexpr auto type = type_token<Declaration>{};

namespace details {

template <typename Target, typename... Parameters> class MappedWiring {
  public:
    MappedWiring(const Target &target, TypeMap type_map) : target_{&target}, type_map_{std::move(type_map)} {}

    template <typename... Arguments> decltype(auto) operator()(Arguments &&...arguments) const {
        return (*target_)(type_map_, std::forward<Arguments>(arguments)...);
    }

    template <typename... Mappings> auto operator[](Mappings... mappings) const & {
        return MappedWiring{*target_, extend_type_map<Parameters...>(type_map_, mappings...)};
    }

    template <typename... Arguments>
    decltype(auto) implementation_requests(Arguments &&...arguments) const
        requires requires(const Target &target) {
            target.implementation_requests(std::declval<const TypeMap &>(),
                                          std::declval<Arguments>()...);
        }
    {
        return target_->implementation_requests(type_map_, std::forward<Arguments>(arguments)...);
    }

    template <typename... Arguments>
    decltype(auto) implementation_replies(Arguments &&...arguments) const
        requires requires(const Target &target) {
            target.implementation_replies(std::declval<const TypeMap &>(),
                                         std::declval<Arguments>()...);
        }
    {
        return target_->implementation_replies(type_map_, std::forward<Arguments>(arguments)...);
    }

    template <typename... Mappings> auto operator[](Mappings...) const && = delete;

  private:
    friend struct mapped_wiring_access;

    const Target *target_;
    TypeMap type_map_;
};

struct mapped_wiring_access {
    template <typename Target, typename... Parameters>
    static const Target &target(const MappedWiring<Target, Parameters...> &wiring) noexcept {
        return *wiring.target_;
    }

    template <typename Target, typename... Parameters>
    static const TypeMap &type_map(const MappedWiring<Target, Parameters...> &wiring) noexcept {
        return wiring.type_map_;
    }
};

template <typename Target, typename... Parameters> class OwningMappedWiring {
  public:
    OwningMappedWiring(Target target, TypeMap type_map)
        : target_{std::move(target)}, type_map_{std::move(type_map)} {}

    template <typename... Arguments> decltype(auto) operator()(Arguments &&...arguments) & = delete;

    template <typename... Arguments> decltype(auto) operator()(Arguments &&...arguments) && {
        return std::move(target_)(type_map_, std::forward<Arguments>(arguments)...);
    }

    template <typename... Mappings> auto operator[](Mappings...) const & = delete;

    template <typename... Mappings> auto operator[](Mappings... mappings) && {
        return OwningMappedWiring<Target, Parameters...>{
            std::move(target_), extend_type_map<Parameters...>(type_map_, mappings...)};
    }

  private:
    Target target_;
    TypeMap type_map_;
};

template <typename Target, typename... Parameters> class TypeParameterizedWiring {
  public:
    template <typename... Mappings> auto operator[](Mappings... mappings) const & {
        return MappedWiring<Target, Parameters...>{
            static_cast<const Target &>(*this), make_type_map<Parameters...>(mappings...)};
    }

    template <typename... Mappings> auto operator[](Mappings... mappings) && {
        return OwningMappedWiring<Target, Parameters...>{
            static_cast<Target &&>(*this), make_type_map<Parameters...>(mappings...)};
    }
};

} // namespace details

} // namespace bg
