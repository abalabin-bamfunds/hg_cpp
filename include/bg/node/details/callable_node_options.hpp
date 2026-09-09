#pragma once

#include <concepts>
#include <type_traits>
#include <utility>

namespace bg::details {

template <typename Function> struct CallableEvaluation {
    Function function;
};

template <typename Function> struct CallableRequirement {
    Function function;
};

template <typename Declaration, typename Function> struct CallableResolver {
    using declaration_type = Declaration;

    Function function;
};

struct CallableWiringDeduplication {
    bool enabled;
};

template <typename T> struct is_callable_requirement : std::false_type {};

template <typename Function> struct is_callable_requirement<CallableRequirement<Function>> : std::true_type {};

template <typename T>
inline constexpr bool is_callable_requirement_v = is_callable_requirement<std::remove_cvref_t<T>>::value;

template <typename T> struct is_callable_resolver : std::false_type {};

template <typename Declaration, typename Function>
struct is_callable_resolver<CallableResolver<Declaration, Function>> : std::true_type {};

template <typename T>
inline constexpr bool is_callable_resolver_v = is_callable_resolver<std::remove_cvref_t<T>>::value;

template <typename T>
inline constexpr bool is_callable_wiring_deduplication_v =
    std::same_as<std::remove_cvref_t<T>, CallableWiringDeduplication>;

template <typename... Options>
concept callable_node_options =
    ((is_callable_requirement_v<Options> || is_callable_resolver_v<Options> ||
      is_callable_wiring_deduplication_v<Options>) &&
     ...) &&
    ((0U + ... + static_cast<unsigned>(is_callable_requirement_v<Options>)) <= 1U) &&
    ((0U + ... + static_cast<unsigned>(is_callable_wiring_deduplication_v<Options>)) <= 1U) &&
    (std::copy_constructible<Options> && ...);

struct CallableEvaluationName {
    template <typename Function> constexpr auto operator=(Function &&function) const {
        return CallableEvaluation<std::decay_t<Function>>{std::forward<Function>(function)};
    }
};

struct CallableRequirementName {
    template <typename Function> constexpr auto operator=(Function &&function) const {
        return CallableRequirement<std::decay_t<Function>>{std::forward<Function>(function)};
    }
};

struct CallableWiringDeduplicationName {
    constexpr auto operator=(bool enabled) const noexcept { return CallableWiringDeduplication{enabled}; }
};

template <typename Declaration> struct CallableResolverName {
    template <typename Function> constexpr auto operator=(Function &&function) const {
        return CallableResolver<Declaration, std::decay_t<Function>>{std::forward<Function>(function)};
    }
};

} // namespace bg::details

namespace bg {

inline constexpr auto eval = details::CallableEvaluationName{};
inline constexpr auto require = details::CallableRequirementName{};
inline constexpr auto deduplicate_wiring = details::CallableWiringDeduplicationName{};

template <typename Declaration> inline constexpr auto resolve = details::CallableResolverName<Declaration>{};

} // namespace bg
