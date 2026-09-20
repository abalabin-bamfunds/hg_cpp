#pragma once

#include <bg/graph/wiring_error.hpp>
#include <bg/graph/wiring_point.hpp>
#include <bg/scalar/type.hpp>
#include <bg/time_series/time_series_type.hpp>
#include <bg/time_series/tsd/tsd_type.hpp>
#include <bg/time_series/tss/tss_type.hpp>
#include <bg/type/time_series_guarantee.hpp>
#include <bg/util/named_argument.hpp>
#include <bg/util/symbol.hpp>

#include <concepts>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>

namespace bg {

template <typename T> struct pass_through_argument {
    T value;
};

template <typename T> auto pass_through(T &&value) {
    return pass_through_argument<std::decay_t<T>>{std::forward<T>(value)};
}

namespace details {

inline constexpr auto map_key_argument_name = util::compile_time_string{"__key_arg__"};
inline constexpr auto map_key_set_argument_name = util::compile_time_string{"__key_set_arg__"};

template <typename T>
inline constexpr bool is_map_option =
    named_argument_is<map_key_argument_name, T> || named_argument_is<map_key_set_argument_name, T>;

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

template <typename TimeSeries> struct map_reference_target {
    using type = std::remove_cv_t<TimeSeries>;
};

template <typename Target, auto... Properties> struct map_reference_target<REF<Target, Properties...>> {
    using type = Target;
};

template <typename TimeSeries>
using map_reference_target_t = typename map_reference_target<std::remove_cv_t<TimeSeries>>::type;

template <typename Argument> struct map_argument_traits {
    static constexpr bool key_set_candidate = false;

    using key_type = void;
    using per_key_type = std::remove_cvref_t<Argument>;
};

template <typename Target, auto... Properties>
struct map_argument_traits<wp<REF<Target, Properties...>>>
    : map_argument_traits<wp<Target>> {};

template <typename K, typename Item, auto... Properties> struct map_argument_traits<wp<TSD<K, Item, Properties...>>> {
    static constexpr bool key_set_candidate = false;

    using key_type = K;
    using per_key_type = wp<map_reference_target_t<Item>>;
};

template <typename K, auto... Properties> struct map_argument_traits<wp<TSS<K, Properties...>>> {
    static constexpr bool key_set_candidate = true;

    using key_type = K;
    using per_key_type = wp<TSS<K, Properties...>>;
};

template <typename T> struct map_argument_traits<pass_through_argument<T>> {
    static constexpr bool key_set_candidate = false;

    using key_type = void;
    using per_key_type = T;
};

template <util::compile_time_string Name, typename PerKeyType> struct named_map_per_key_type {
    using type = named_argument<Name, PerKeyType>;
};

template <util::compile_time_string Name> struct named_map_per_key_type<Name, void> {
    using type = void;
};

template <util::compile_time_string Name, typename Value>
struct map_argument_traits<named_argument<Name, Value>> : map_argument_traits<Value> {
    using ValueTraits = map_argument_traits<Value>;
    using per_key_type = typename named_map_per_key_type<Name, typename ValueTraits::per_key_type>::type;
};

template <typename Argument> using map_argument_traits_t = map_argument_traits<std::remove_cvref_t<Argument>>;

template <typename Argument> struct map_argument_value {
    using type = std::remove_cvref_t<Argument>;
};

template <util::compile_time_string Name, typename Value>
struct map_argument_value<named_argument<Name, Value>> : map_argument_value<Value> {};

template <typename Value>
struct map_argument_value<pass_through_argument<Value>> : map_argument_value<Value> {};

template <typename Argument>
inline constexpr bool untyped_map_wiring_point =
    std::same_as<typename map_argument_value<std::remove_cvref_t<Argument>>::type, wp<>>;

template <typename Argument>
inline constexpr bool map_key_candidate = !std::same_as<typename map_argument_traits_t<Argument>::key_type, void>;

template <typename Argument>
inline constexpr bool map_key_set_candidate =
    map_argument_traits_t<Argument>::key_set_candidate;

template <typename... Arguments> struct first_map_key {
    using type = void;
};

template <typename First, typename... Rest> struct first_map_key<First, Rest...> {
    using Candidate = typename map_argument_traits_t<First>::key_type;
    using type = std::conditional_t<std::same_as<Candidate, void>, typename first_map_key<Rest...>::type, Candidate>;
};

enum class MapCallError {
    none,
    duplicate_key_argument,
    duplicate_key_set_argument,
    no_key_type,
    generic_key_type,
    inconsistent_key_types,
    ambiguous_key_set,
};

template <typename... Arguments> struct map_call_traits {
    using key_type = typename first_map_key<Arguments...>::type;

    static constexpr auto key_argument_count = named_argument_count<map_key_argument_name, Arguments...>;
    static constexpr auto key_set_argument_count = named_argument_count<map_key_set_argument_name, Arguments...>;
    static constexpr auto key_candidate_count =
        (std::size_t{} + ... + static_cast<std::size_t>(map_key_candidate<Arguments>));
    static constexpr auto key_set_candidate_count =
        (std::size_t{} + ... + static_cast<std::size_t>(map_key_set_candidate<Arguments>));
    static constexpr bool key_types_match =
        ((!map_key_candidate<Arguments> ||
          std::same_as<typename map_argument_traits_t<Arguments>::key_type, key_type>) &&
         ...);

    static constexpr auto error = [] {
        if constexpr (key_argument_count > 1) {
            return MapCallError::duplicate_key_argument;
        } else if constexpr (key_set_argument_count > 1) {
            return MapCallError::duplicate_key_set_argument;
        } else if constexpr (key_candidate_count == 0) {
            return MapCallError::no_key_type;
        } else if constexpr (scalar_variable<key_type>) {
            return MapCallError::generic_key_type;
        } else if constexpr (!key_types_match) {
            return MapCallError::inconsistent_key_types;
        } else if constexpr (key_set_candidate_count > 1 && key_set_argument_count == 0) {
            return MapCallError::ambiguous_key_set;
        } else {
            return MapCallError::none;
        }
    }();

    static constexpr bool valid = error == MapCallError::none;
};

template <typename K> using map_key_argument_t = wp<TS<K, TimeSeriesGuarantee::const_>>;

template <typename PositionalTuple, typename NamedTuple, typename Key> struct MapCallPreprocessing {
    using key_type = Key;
    using inserted_key_type = map_key_argument_t<Key>;

    PositionalTuple positional;
    NamedTuple named;
    std::optional<MapArgumentSelector> key_arg;
    std::optional<MapArgumentSelector> key_set_arg;
};

template <typename PositionalTuple, typename NamedTuple> struct MapGraphCallPreprocessing {
    PositionalTuple positional;
    NamedTuple named;
    std::optional<MapArgumentSelector> key_arg;
    std::optional<MapArgumentSelector> key_set_arg;
};

template <typename Argument> auto positional_map_argument(Argument &&argument) {
    if constexpr (!is_map_option<Argument> && !is_named_argument_v<Argument>) {
        return std::tuple<std::decay_t<Argument>>{std::forward<Argument>(argument)};
    } else {
        return std::tuple<>{};
    }
}

template <typename Argument> auto named_map_argument(Argument &&argument) {
    if constexpr (!is_map_option<Argument> && is_named_argument_v<Argument>) {
        return std::tuple<std::decay_t<Argument>>{std::forward<Argument>(argument)};
    } else {
        return std::tuple<>{};
    }
}

template <util::compile_time_string Name, typename... Arguments>
std::optional<MapArgumentSelector> map_option_selector(const Arguments &...arguments) {
    if constexpr (named_argument_count<Name, Arguments...> == 0) {
        return std::nullopt;
    } else {
        return MapArgumentSelector{named_argument_value<Name>(arguments...)};
    }
}

template <std::size_t UserIndex = 0> std::optional<MapArgumentSelector> inferred_key_set_selector() {
    return std::nullopt;
}

template <std::size_t UserIndex = 0, typename First, typename... Rest>
std::optional<MapArgumentSelector> inferred_key_set_selector() {
    if constexpr (is_map_option<First>) {
        return inferred_key_set_selector<UserIndex, Rest...>();
    } else if constexpr (map_key_set_candidate<First>) {
        if constexpr (is_named_argument_v<First>) {
            return MapArgumentSelector{util::symbol{std::remove_cvref_t<First>::name.view()}};
        } else {
            return MapArgumentSelector{UserIndex};
        }
    } else if constexpr (is_named_argument_v<First>) {
        return inferred_key_set_selector<UserIndex, Rest...>();
    } else {
        return inferred_key_set_selector<UserIndex + 1, Rest...>();
    }
}

template <typename... Arguments> void validate_map_call() {
    constexpr auto error = map_call_traits<Arguments...>::error;

    if constexpr (error == MapCallError::duplicate_key_argument) {
        throw WiringError{"a map call accepts one __key_arg__ option"};
    } else if constexpr (error == MapCallError::duplicate_key_set_argument) {
        throw WiringError{"a map call accepts one __key_set_arg__ option"};
    } else if constexpr (error == MapCallError::no_key_type) {
        if constexpr ((untyped_map_wiring_point<Arguments> || ...)) {
            throw WiringError{
                "a map graph function requires typed TSD or TSS arguments; use a graph template with wp<>"};
        } else {
            throw WiringError{"a map call requires a TSD or TSS argument"};
        }
    } else if constexpr (error == MapCallError::generic_key_type) {
        throw WiringError{"a map call requires a concrete key type"};
    } else if constexpr (error == MapCallError::inconsistent_key_types) {
        throw WiringError{"all map TSD and TSS arguments must have the same key type"};
    } else if constexpr (error == MapCallError::ambiguous_key_set) {
        throw WiringError{"a map call with multiple TSS arguments requires __key_set_arg__"};
    }
}

template <typename... Arguments> void validate_map_options() {
    constexpr auto key_argument_count = named_argument_count<map_key_argument_name, Arguments...>;
    constexpr auto key_set_argument_count = named_argument_count<map_key_set_argument_name, Arguments...>;

    if constexpr (key_argument_count > 1) {
        throw WiringError{"a map call accepts one __key_arg__ option"};
    } else if constexpr (key_set_argument_count > 1) {
        throw WiringError{"a map call accepts one __key_set_arg__ option"};
    }
}

template <typename... Arguments> auto preprocess_map_call(Arguments &&...arguments) {
    validate_map_call<Arguments...>();

    auto key_arg = map_option_selector<map_key_argument_name>(arguments...);
    auto key_set_arg = map_option_selector<map_key_set_argument_name>(arguments...);
    if (!key_set_arg && map_call_traits<Arguments...>::key_set_candidate_count == 1) {
        key_set_arg = inferred_key_set_selector<0, Arguments...>();
    }

    auto positional = std::tuple_cat(positional_map_argument(std::forward<Arguments>(arguments))...);
    auto named = std::tuple_cat(named_map_argument(std::forward<Arguments>(arguments))...);

    using Key = typename map_call_traits<Arguments...>::key_type;
    return MapCallPreprocessing<decltype(positional), decltype(named), Key>{
        .positional = std::move(positional),
        .named = std::move(named),
        .key_arg = std::move(key_arg),
        .key_set_arg = std::move(key_set_arg),
    };
}

template <typename... Arguments> auto preprocess_map_graph_call(Arguments &&...arguments) {
    validate_map_options<Arguments...>();

    auto key_arg = map_option_selector<map_key_argument_name>(arguments...);
    auto key_set_arg = map_option_selector<map_key_set_argument_name>(arguments...);
    auto positional = std::tuple_cat(positional_map_argument(std::forward<Arguments>(arguments))...);
    auto named = std::tuple_cat(named_map_argument(std::forward<Arguments>(arguments))...);

    return MapGraphCallPreprocessing<decltype(positional), decltype(named)>{
        .positional = std::move(positional),
        .named = std::move(named),
        .key_arg = std::move(key_arg),
        .key_set_arg = std::move(key_set_arg),
    };
}

} // namespace details

} // namespace bg
