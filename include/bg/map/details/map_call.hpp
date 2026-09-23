#pragma once

#include <bg/graph/wiring_argument.hpp>
#include <bg/graph/wiring_error.hpp>
#include <bg/graph/wiring_point.hpp>
#include <bg/map_argument.hpp>
#include <bg/time_series/time_series_type.hpp>
#include <bg/time_series/tsd/tsd_type.hpp>
#include <bg/time_series/tss/tss_type.hpp>
#include <bg/util/named_argument.hpp>

#include <concepts>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace bg {

class ScalarType;

namespace details {

inline constexpr auto map_key_argument_name = util::compile_time_string{"__key_arg__"};
inline constexpr auto map_key_set_argument_name = util::compile_time_string{"__key_set_arg__"};

class MapArgumentSelector {
  public:
    template <std::integral Index>
        requires(!std::same_as<std::remove_cv_t<Index>, bool>)
    explicit MapArgumentSelector(Index index) {
        if (!std::in_range<std::size_t>(index)) {
            throw WiringError{"a map argument selector index must be non-negative"};
        }

        value_ = static_cast<std::size_t>(index);
    }

    explicit MapArgumentSelector(util::symbol name) : value_{name} {}
    explicit MapArgumentSelector(std::string_view name) : value_{util::symbol{name}} {}
    explicit MapArgumentSelector(const std::string &name) : MapArgumentSelector{std::string_view{name}} {}
    explicit MapArgumentSelector(const char *name) {
        if (name == nullptr) {
            throw WiringError{"a map argument selector name cannot be null"};
        }

        value_ = util::symbol{name};
    }

    std::optional<std::size_t> index() const noexcept {
        if (const auto *index = std::get_if<std::size_t>(&value_)) {
            return *index;
        }
        return std::nullopt;
    }

    std::optional<util::symbol> name() const noexcept {
        if (const auto *name = std::get_if<util::symbol>(&value_)) {
            return *name;
        }
        return std::nullopt;
    }

  private:
    std::variant<std::size_t, util::symbol> value_;
};

struct ResolvedMapCall {
    std::vector<MapArgument> positional;
    std::vector<MapArgument> named;
    std::vector<WiringArgument> options;
    const ScalarType *key_type{};
    std::optional<MapArgumentSelector> key_argument;
    std::optional<std::size_t> selected_key_set;
    bool omit_selected_key_set_argument{};
};

ResolvedMapCall resolve_map_call(std::span<const MapArgument> arguments);

template <typename T>
inline constexpr bool is_map_option =
    named_argument_is<map_key_argument_name, T> || named_argument_is<map_key_set_argument_name, T>;

// These traits describe only the C++ parameter needed to instantiate a graph function.
// Map's semantic decisions use WiringArgument types at runtime.
template <typename Argument> struct map_callable_argument {
    using key_type = void;
    using parameter_type = std::remove_cvref_t<Argument>;

    static constexpr bool key_set = false;
};

template <typename Target, auto... Properties>
struct map_callable_argument<wp<REF<Target, Properties...>>>
    : map_callable_argument<wp<Target>> {};

template <typename K, typename Item, auto... Properties>
struct map_callable_argument<wp<TSD<K, Item, Properties...>>> {
    using key_type = K;
    using parameter_type = wp<dereferenced_time_series_t<Item>>;

    static constexpr bool key_set = false;
};

template <typename K, auto... Properties>
struct map_callable_argument<wp<TSS<K, Properties...>>> {
    using key_type = K;
    using parameter_type = wp<TSS<K, Properties...>>;

    static constexpr bool key_set = true;
};

template <typename T> struct map_callable_argument<pass_through_argument<T>> {
    using key_type = void;
    using parameter_type = T;

    static constexpr bool key_set = false;
};

template <util::compile_time_string Name, typename Value>
struct map_callable_argument<named_argument<Name, Value>> : map_callable_argument<Value> {};

template <typename Argument>
using map_callable_argument_t = map_callable_argument<std::remove_cvref_t<Argument>>;

template <typename Argument>
inline constexpr bool map_callable_key_set = map_callable_argument_t<Argument>::key_set;

template <typename... Arguments> struct first_map_callable_key {
    using type = void;
};

template <typename First, typename... Rest>
struct first_map_callable_key<First, Rest...> {
    using Candidate = typename map_callable_argument_t<First>::key_type;
    using type = std::conditional_t<std::same_as<Candidate, void>,
                                    typename first_map_callable_key<Rest...>::type,
                                    Candidate>;
};

template <typename K>
using map_key_parameter_t = wp<TS<K, TimeSeriesGuarantee::const_>>;

template <typename PositionalTuple, typename NamedTuple, typename OptionsTuple>
struct MapCallArguments {
    PositionalTuple positional;
    NamedTuple named;
    OptionsTuple options;
};

template <typename Argument>
auto positional_map_argument(Argument &&argument) {
    if constexpr (!is_map_option<Argument> && !is_named_argument_v<Argument>) {
        return std::tuple<std::decay_t<Argument>>{std::forward<Argument>(argument)};
    } else {
        return std::tuple<>{};
    }
}

template <typename Argument>
auto named_map_argument(Argument &&argument) {
    if constexpr (!is_map_option<Argument> && is_named_argument_v<Argument>) {
        return std::tuple<std::decay_t<Argument>>{std::forward<Argument>(argument)};
    } else {
        return std::tuple<>{};
    }
}

template <util::compile_time_string Name, typename Value>
auto map_option_argument(named_argument<Name, Value> argument) {
    if constexpr (!is_map_option<named_argument<Name, Value>>) {
        return std::tuple<>{};
    } else if constexpr (std::same_as<std::remove_cv_t<Value>, const char *> ||
                         std::same_as<std::remove_cv_t<Value>, char *>) {
        return std::tuple{named_argument<Name, std::string>{std::string{argument.value}}};
    } else {
        return std::tuple<named_argument<Name, Value>>{std::move(argument)};
    }
}

template <typename Argument>
    requires(!is_named_argument_v<Argument>)
auto map_option_argument(Argument &&) {
    return std::tuple<>{};
}

template <typename... Arguments>
auto group_map_call_arguments(Arguments &&...arguments) {
    auto positional = std::tuple_cat(positional_map_argument(std::forward<Arguments>(arguments))...);
    auto named = std::tuple_cat(named_map_argument(std::forward<Arguments>(arguments))...);
    auto options = std::tuple_cat(map_option_argument(std::forward<Arguments>(arguments))...);

    return MapCallArguments<decltype(positional), decltype(named), decltype(options)>{
        .positional = std::move(positional),
        .named = std::move(named),
        .options = std::move(options),
    };
}

} // namespace details
} // namespace bg
