#pragma once

#include <bg/graph/details/graph_signature.hpp>
#include <bg/util/reflection.hpp>

#include <concepts>
#include <string_view>
#include <type_traits>
#include <utility>

namespace bg::details {

template <typename T>
inline constexpr bool supported_graph_field =
    is_typed_wiring_point<T> || std::same_as<std::remove_cv_t<T>, GraphArgument> ||
    supported_scalar_argument<T> || supported_optional_scalar_argument<T>;

template <typename Definition, std::size_t... Index>
consteval bool supported_graph_fields(std::index_sequence<Index...>) {
    return (supported_graph_field<std::remove_cv_t<util::aggregate_member_type<Index, Definition>>> && ...);
}

template <typename Definition, typename Index> class aggregate_graph_function;

template <typename Definition, std::size_t... Index>
class aggregate_graph_function<Definition, std::index_sequence<Index...>> {
    using Result = decltype(std::declval<const Definition &>().wire());

  public:
    Result operator()(std::remove_cv_t<util::aggregate_member_type<Index, Definition>>... arguments) const {
        Definition definition{std::move(arguments)...};

        if constexpr (std::same_as<Result, void>) {
            std::as_const(definition).wire();
        } else {
            return std::as_const(definition).wire();
        }
    }
};

template <typename Definition>
using aggregate_graph_function_t =
    aggregate_graph_function<Definition, std::make_index_sequence<util::aggregate_member_count<Definition>>>;

template <typename Definition, bool = std::is_aggregate_v<Definition>>
struct aggregate_graph_definition : std::false_type {};

template <typename Definition>
        struct aggregate_graph_definition<Definition, true>
    : std::bool_constant <
      supported_graph_fields<Definition>(std::make_index_sequence<util::aggregate_member_count<Definition>>{}) &&
    requires(const Definition &definition) {
    definition.wire();
}>{};

template <typename Definition> [[nodiscard]] Signature aggregate_graph_signature() {
    using Function = aggregate_graph_function_t<Definition>;
    constexpr auto &member_names = util::aggregate_member_names<Definition>;

    return make_graph_signature<Function>(util::symbol{util::unqualified_type_name<Definition>()},
                                          std::span<const std::string_view>{member_names});
}

} // namespace bg::details
