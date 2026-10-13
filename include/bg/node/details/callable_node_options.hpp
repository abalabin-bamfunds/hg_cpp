#pragma once

#include <bg/graph/requirement.hpp>

#include <concepts>
#include <type_traits>
#include <utility>

namespace bg::details {

template <typename Function> struct CallableEvaluation {
    Function function;
};

template <typename Declaration, typename Function> struct CallableResolver {
    using declaration_type = Declaration;

    Function function;
};

struct CallableWiringDeduplication {
    bool enabled;
};

struct CallableStartScheduling {
    bool enabled;
};

template <typename T> struct is_callable_resolver : std::false_type {};

template <typename Declaration, typename Function>
struct is_callable_resolver<CallableResolver<Declaration, Function>> : std::true_type {};

template <typename T>
inline constexpr bool is_callable_resolver_v = is_callable_resolver<std::remove_cvref_t<T>>::value;

template <typename T>
inline constexpr bool is_callable_wiring_deduplication_v =
    std::same_as<std::remove_cvref_t<T>, CallableWiringDeduplication>;

template <typename T>
inline constexpr bool is_callable_start_scheduling_v = std::same_as<std::remove_cvref_t<T>, CallableStartScheduling>;

template <typename... Options>
concept callable_node_options =
    ((is_callable_requirement_v<Options> || is_callable_resolver_v<Options> ||
      is_callable_wiring_deduplication_v<Options> || is_callable_start_scheduling_v<Options>) &&
     ...) &&
    ((0U + ... + static_cast<unsigned>(is_callable_requirement_v<Options>)) <= 1U) &&
    ((0U + ... + static_cast<unsigned>(is_callable_wiring_deduplication_v<Options>)) <= 1U) &&
    ((0U + ... + static_cast<unsigned>(is_callable_start_scheduling_v<Options>)) <= 1U) &&
    (std::copy_constructible<Options> && ...);

struct CallableEvaluationName {
    template <typename Function> constexpr auto operator=(Function &&function) const {
        return CallableEvaluation<std::decay_t<Function>>{std::forward<Function>(function)};
    }
};

struct CallableWiringDeduplicationName {
    constexpr auto operator=(bool enabled) const noexcept { return CallableWiringDeduplication{enabled}; }
};

struct CallableStartSchedulingName {
    constexpr auto operator=(bool enabled) const noexcept { return CallableStartScheduling{enabled}; }
};

template <typename Declaration> struct CallableResolverName {
    template <typename Function> constexpr auto operator=(Function &&function) const {
        return CallableResolver<Declaration, std::decay_t<Function>>{std::forward<Function>(function)};
    }
};

} // namespace bg::details

namespace bg {

inline constexpr auto eval = details::CallableEvaluationName{};
inline constexpr auto deduplicate_wiring = details::CallableWiringDeduplicationName{};
inline constexpr auto schedule_on_start = details::CallableStartSchedulingName{};

template <typename Declaration> inline constexpr auto resolve = details::CallableResolverName<Declaration>{};

} // namespace bg
