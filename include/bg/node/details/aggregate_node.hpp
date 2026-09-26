#pragma once

#include <bg/evaluation_context.hpp>
#include <bg/graph/node.hpp>
#include <bg/graph/details/variadic_wiring_operations.hpp>
#include <bg/node/details/aggregate_resolver.hpp>
#include <bg/node/input.hpp>
#include <bg/node/output.hpp>
#include <bg/node/state.hpp>
#include <bg/node/state_layout.hpp>
#include <bg/scalar/cpp_runtime_type.hpp>
#include <bg/time_series/ref/dynamic_reference_input.hpp>
#include <bg/time_series/ref/ref_runtime_type.hpp>
#include <bg/time_series/time_series_runtime_type.hpp>
#include <bg/time_series/tsb/tsb_runtime_type.hpp>
#include <bg/time_series/tsb/tsb_type.hpp>
#include <bg/time_series/tsd/tsd_runtime_type.hpp>
#include <bg/time_series/tsd/tsd_type.hpp>
#include <bg/time_series/tsl/tsl_runtime_type.hpp>
#include <bg/time_series/tsl/tsl_type.hpp>
#include <bg/time_series/tss/tss_runtime_type.hpp>
#include <bg/time_series/tss/tss_type.hpp>
#include <bg/type/details/time_series_type.hpp>
#include <bg/type/scalar_type.hpp>
#include <bg/type/scalar_variable.hpp>
#include <bg/type/signature.hpp>
#include <bg/type/time_series_dict_type.hpp>
#include <bg/type/time_series_list_type.hpp>
#include <bg/type/time_series_reference_type.hpp>
#include <bg/type/time_series_set_type.hpp>
#include <bg/type/time_series_value_type.hpp>
#include <bg/type/time_series_variable.hpp>
#include <bg/type/type_variable.hpp>
#include <bg/util/reflection.hpp>

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

namespace bg::details {

template <typename T> struct input_field {
    static constexpr bool value = false;
    static constexpr bool reflected_name = true;
    static constexpr bool dynamic_binding = false;
};

template <typename TimeSeries, auto... Properties> struct input_field<In<TimeSeries, Properties...>> {
    using Input = In<TimeSeries, Properties...>;

    static constexpr bool value = true;
    static constexpr bool reflected_name = Input::name.empty();
    static constexpr InputActivity activity = Input::activity;
    static constexpr InputValidity validity = Input::validity;
    static constexpr bool dynamic_binding = Input::dynamic_binding;
    static constexpr auto variadic_mode = [] {
        if constexpr (requires { Input::variadic_mode; }) {
            return Input::variadic_mode;
        }
        return VariadicMode::none;
    }();

    static_assert(!dynamic_binding || is_time_series_reference<TimeSeries>,
                  "dynamic_binding requires a time series reference input");

    using time_series_type = TimeSeries;
};

template <typename T> struct output_field {
    static constexpr bool value = false;

    using time_series_type = void;
};

template <typename TimeSeries> struct output_field<Out<TimeSeries>> {
    static constexpr bool value = true;

    using time_series_type = TimeSeries;
};

template <typename... TimeSeries> struct first_output_time_series;

template <typename First, typename... Rest> struct first_output_time_series<First, Rest...> {
    using type = std::conditional_t<std::same_as<First, void>, typename first_output_time_series<Rest...>::type, First>;
};

template <> struct first_output_time_series<> {
    using type = void;
};

template <typename Definition, std::size_t... Index>
auto aggregate_output_time_series_type(std::index_sequence<Index...>) -> typename first_output_time_series<
    typename output_field<std::remove_cv_t<util::aggregate_member_type<Index, Definition>>>::time_series_type...>::type;

template <typename Definition>
using aggregate_output_time_series_t = decltype(aggregate_output_time_series_type<Definition>(
    std::make_index_sequence<util::aggregate_member_count<Definition>>{}));

template <typename T> inline constexpr bool generic_scalar_state = generic_scalar<T>;

template <typename T> struct scalar_field {
    static constexpr bool value = scalar_declaration_traits<T>::supports_implicit_argument && requires {
        cpp_scalar_type<canonical_scalar_type_t<T>>();
    };
    static constexpr bool optional = false;

    using value_type = T;
    using parameter_type = canonical_scalar_type_t<T>;
};

template <typename T> struct scalar_field<std::optional<T>> {
    static constexpr bool value = requires { cpp_scalar_type<canonical_scalar_type_t<T>>(); };
    static constexpr bool optional = true;

    using value_type = T;
    using parameter_type = canonical_scalar_type_t<T>;
};

template <typename T> struct scalar_field<Scalar<T>> {
    static constexpr bool value = generic_scalar_state<T> || requires { cpp_scalar_type<T>(); };
    static constexpr bool optional = false;

    using value_type = T;
    using parameter_type = T;
};

template <typename T> struct state_field {
    static constexpr bool value = false;
    static constexpr bool scalar_collection = false;
};

template <typename T> struct state_field<State<T>> {
    static constexpr bool value = true;
    static constexpr bool scalar_collection = scalar_declaration_traits<T>::is_collection;

    using value_type = T;
    using storage_type = std::conditional_t<scalar_collection, scalar_stored_t<T>, T>;
};

template <typename Definition, std::size_t Index> const TimeSeriesType &input_type();

template <typename Definition, std::size_t... Index>
std::vector<const ScalarType *> aggregate_state_types(const TypeMap &type_map, std::index_sequence<Index...>) {
    auto result = std::vector<const ScalarType *>{};
    result.reserve((std::size_t{} + ... +
                    static_cast<std::size_t>(
                        state_field<std::remove_cv_t<util::aggregate_member_type<Index, Definition>>>::value)));

    (
        [&] {
            using Field = std::remove_cv_t<util::aggregate_member_type<Index, Definition>>;

            if constexpr (state_field<Field>::value) {
                using Value = typename state_field<Field>::value_type;
                if constexpr (state_field<Field>::scalar_collection && generic_scalar_state<Value>) {
                    result.push_back(&static_cast<const ScalarType &>(scalar_type_for<Value>().resolve(type_map)));
                } else {
                    result.push_back(nullptr);
                }
            }
        }(),
        ...);

    return result;
}

template <typename Definition, std::size_t... Index>
std::vector<const TimeSeriesType *> aggregate_dynamic_reference_input_types(const TypeMap &type_map,
                                                                            std::index_sequence<Index...>) {
    auto result = std::vector<const TimeSeriesType *>{};
    result.reserve((std::size_t{} + ... + static_cast<std::size_t>([] {
                        using Field = std::remove_cv_t<util::aggregate_member_type<Index, Definition>>;
                        return input_field<Field>::value && input_field<Field>::dynamic_binding;
                    }())));

    (
        [&] {
            using Field = std::remove_cv_t<util::aggregate_member_type<Index, Definition>>;
            if constexpr (input_field<Field>::value && input_field<Field>::dynamic_binding) {
                const auto &resolved = input_type<Definition, Index>().resolve(type_map);
                result.push_back(&static_cast<const TimeSeriesType &>(resolved));
            }
        }(),
        ...);

    return result;
}

template <typename T> struct generic_input_field : std::false_type {};

template <typename TimeSeries, auto... Properties>
struct generic_input_field<In<TimeSeries, Properties...>> : std::bool_constant<generic_time_series<TimeSeries>> {};

template <typename T> struct input_view_field : std::false_type {};

template <typename T> struct time_series_value_has_runtime_layout : std::false_type {};

template <typename T, auto... Properties>
struct time_series_value_has_runtime_layout<TS<T, Properties...>>
    : std::bool_constant<scalar_declaration_traits<T>::is_collection> {};

template <typename TimeSeries, auto... Properties>
struct input_view_field<In<TimeSeries, Properties...>>
    : std::bool_constant<generic_time_series<TimeSeries> || is_time_series_list<TimeSeries> ||
                         is_time_series_bundle<TimeSeries> || is_time_series_set<TimeSeries> ||
                         is_time_series_dict<TimeSeries> || time_series_value_has_runtime_layout<TimeSeries>::value> {};

template <typename T> struct generic_output_field : std::false_type {};

template <typename TimeSeries>
struct generic_output_field<Out<TimeSeries>> : std::bool_constant<generic_time_series<TimeSeries>> {};

template <typename T> struct output_view_field : std::false_type {};

template <typename TimeSeries>
struct output_view_field<Out<TimeSeries>>
    : std::bool_constant<generic_time_series<TimeSeries> || is_time_series_bundle<TimeSeries>> {};

template <typename T> struct generic_scalar_input_field : std::false_type {};

template <typename T> struct generic_scalar_input_field<Scalar<T>> : std::bool_constant<generic_scalar_state<T>> {};

template <typename T> struct scalar_variable_view : std::false_type {};

template <scalar_variable Variable> struct scalar_variable_view<Scalar<Variable>> : std::true_type {};

template <typename T> struct scalar_view : std::false_type {};

template <typename T> struct scalar_view<Scalar<T>> : std::true_type {};

template <typename Field, bool = input_field<Field>::value> struct supported_input : std::false_type {};

template <typename Field>
struct supported_input<Field, true>
    : std::bool_constant<supported_time_series<typename input_field<Field>::time_series_type>> {};

template <typename Field, bool = output_field<Field>::value> struct supported_output : std::false_type {};

template <typename Field>
struct supported_output<Field, true>
    : std::bool_constant<supported_time_series<typename output_field<Field>::time_series_type>> {};

template <typename Field>
inline constexpr bool supported_node_field = supported_input<Field>::value || supported_output<Field>::value ||
                                             scalar_field<Field>::value || state_field<Field>::value;

template <typename Definition, std::size_t... Index>
consteval bool supported_node_fields(std::index_sequence<Index...>) {
    return (supported_node_field<std::remove_cv_t<util::aggregate_member_type<Index, Definition>>> && ...);
}

template <typename Definition, std::size_t... Index>
consteval std::size_t output_field_count(std::index_sequence<Index...>) {
    return (std::size_t{} + ... +
            static_cast<std::size_t>(
                output_field<std::remove_cv_t<util::aggregate_member_type<Index, Definition>>>::value));
}

template <typename Definition> consteval bool aggregate_node_has_output() {
    using Index = std::make_index_sequence<util::aggregate_member_count<Definition>>;

    return output_field_count<Definition>(Index{}) != 0;
}

// Holds the root output view in the aggregate invocation; item views continue to use evaluation scratch storage.
template <bool HasOutput> class aggregate_output_view_storage {
  public:
    explicit aggregate_output_view_storage(EvaluationContext *) noexcept {}
};

template <> class aggregate_output_view_storage<true> {
  public:
    explicit aggregate_output_view_storage(EvaluationContext *context) noexcept
        : local_storage_{context->scratch().use_local_storage(storage_)} {}

  private:
    alignas(OutputState) std::byte storage_[sizeof(OutputState)];
    ScratchPad::LocalStorageScope local_storage_;
};

template <typename Result>
concept aggregate_evaluation_result = std::same_as<Result, void> || std::same_as<Result, bool>;

template <typename Definition>
inline constexpr bool aggregate_node_has_evaluate = requires(const Definition &definition) {
    { definition.evaluate() } -> aggregate_evaluation_result;
} || requires(const Definition &definition, EvaluationContext *context) {
    { definition.evaluate(context) } -> aggregate_evaluation_result;
};

template <typename Definition>
inline constexpr bool aggregate_node_evaluate_returns_bool = [] {
    if constexpr (requires(const Definition &definition) {
                      { definition.evaluate() } -> std::same_as<bool>;
                  }) {
        return true;
    } else {
        return requires(const Definition &definition, EvaluationContext *context) {
            { definition.evaluate(context) } -> std::same_as<bool>;
        };
    }
}();

template <typename Definition>
inline constexpr bool aggregate_node_has_start = requires(const Definition &definition) {
    { definition.start() } -> std::same_as<void>;
} || requires(const Definition &definition, EvaluationContext *context) {
    { definition.start(context) } -> std::same_as<void>;
};

template <typename Definition>
inline constexpr bool aggregate_node_has_stop = requires(const Definition &definition) {
    { definition.stop() } noexcept -> std::same_as<void>;
} || requires(const Definition &definition, EvaluationContext *context) {
    { definition.stop(context) } noexcept -> std::same_as<void>;
};

template <typename Definition>
inline constexpr bool aggregate_node_has_invalidation_function = requires(const Definition &definition) {
    { definition.on_invalidation() } -> std::same_as<void>;
} || requires(const Definition &definition, EvaluationContext *context) {
    { definition.on_invalidation(context) } -> std::same_as<void>;
};

template <typename Definition>
inline constexpr bool aggregate_node_has_finalize = requires(const Definition &definition) {
    { definition.finalize() } noexcept -> std::same_as<void>;
} || requires(const Definition &definition, EvaluationContext *context) {
    { definition.finalize(context) } noexcept -> std::same_as<void>;
};

template <typename Definition>
inline constexpr bool aggregate_node_preserves_output =
    requires { requires std::same_as<std::remove_cv_t<decltype(Definition::on_invalidation)>, PreserveOutput>; };

template <typename Definition> [[nodiscard]] consteval bool aggregate_uses_timed_scheduling() {
    if constexpr (requires { Definition::uses_timed_scheduling; }) {
        return Definition::uses_timed_scheduling;
    }

    return false;
}

template <typename Definition, std::size_t... Index>
consteval bool input_fields_use_reflected_names(std::index_sequence<Index...>) {
    return ((!input_field<std::remove_cv_t<util::aggregate_member_type<Index, Definition>>>::value ||
             input_field<std::remove_cv_t<util::aggregate_member_type<Index, Definition>>>::dynamic_binding ||
             input_field<std::remove_cv_t<util::aggregate_member_type<Index, Definition>>>::reflected_name) &&
            ...);
}

template <typename Definition, bool = std::is_aggregate_v<Definition>> struct aggregate_node_class {
    static constexpr bool value = false;
};

template <typename Definition> struct aggregate_node_class<Definition, true> {
  private:
    using Index = std::make_index_sequence<util::aggregate_member_count<Definition>>;

  public:
    static constexpr bool value = supported_node_fields<Definition>(Index{}) &&
                                  output_field_count<Definition>(Index{}) <= 1 &&
                                  input_fields_use_reflected_names<Definition>(Index{}) &&
                                  (aggregate_node_has_evaluate<Definition> || aggregate_node_has_start<Definition>);
};

template <typename Definition, std::size_t Index> const TimeSeriesType &input_type() {
    using Field = std::remove_cv_t<util::aggregate_member_type<Index, Definition>>;
    using TimeSeries = typename input_field<Field>::time_series_type;

    if constexpr (time_series_variable<TimeSeries>) {
        return time_series_variable_instance<TimeSeries>();
    } else if constexpr (TimeSeries::has_explicit_guarantee) {
        return time_series_type_for<Definition, Index, true, TimeSeries>(guarantee_parameter<TimeSeries::guarantee>());
    } else {
        static const auto guarantee = GuaranteeVariable{util::symbol{util::aggregate_member_names<Definition>[Index]}};
        return time_series_type_for<Definition, Index, true, TimeSeries>(guarantee);
    }
}

template <typename Definition, std::size_t Index> const ScalarType &scalar_type() {
    using Field = std::remove_cv_t<util::aggregate_member_type<Index, Definition>>;
    using Value = typename scalar_field<Field>::parameter_type;

    return scalar_type_for<Value>();
}

template <typename Definition, std::size_t Index> const TimeSeriesType &output_type() {
    using Field = std::remove_cv_t<util::aggregate_member_type<Index, Definition>>;
    using TimeSeries = typename output_field<Field>::time_series_type;

    if constexpr (time_series_variable<TimeSeries>) {
        return time_series_variable_instance<TimeSeries>();
    } else {
        return time_series_type_for<Definition, Index, false, TimeSeries>(TimeSeriesGuarantee::ordinary);
    }
}

template <typename Definition, std::size_t... Index>
consteval std::size_t aggregate_time_series_runtime_type_count(std::index_sequence<Index...>) {
    return (std::size_t{} + ... +
            static_cast<std::size_t>(
                input_view_field<std::remove_cv_t<util::aggregate_member_type<Index, Definition>>>::value &&
                !input_field<std::remove_cv_t<util::aggregate_member_type<Index, Definition>>>::dynamic_binding));
}

template <typename Definition, std::size_t Target, std::size_t... Index>
consteval std::size_t aggregate_time_series_runtime_type_index(std::index_sequence<Index...>) {
    return (std::size_t{} + ... +
            static_cast<std::size_t>(
                Index < Target &&
                input_view_field<std::remove_cv_t<util::aggregate_member_type<Index, Definition>>>::value &&
                !input_field<std::remove_cv_t<util::aggregate_member_type<Index, Definition>>>::dynamic_binding));
}

template <typename Definition, std::size_t... Index>
consteval std::size_t aggregate_generic_scalar_input_count(std::index_sequence<Index...>) {
    return (std::size_t{} + ... +
            static_cast<std::size_t>(
                generic_scalar_input_field<std::remove_cv_t<util::aggregate_member_type<Index, Definition>>>::value));
}

template <typename Definition, std::size_t Target, std::size_t... Index>
consteval std::size_t aggregate_generic_scalar_input_index(std::index_sequence<Index...>) {
    return (std::size_t{} + ... +
            static_cast<std::size_t>(
                Index < Target &&
                generic_scalar_input_field<std::remove_cv_t<util::aggregate_member_type<Index, Definition>>>::value));
}

template <typename Definition, std::size_t... Index>
consteval bool aggregate_uses_output_runtime_type(std::index_sequence<Index...>) {
    return (false || ... || output_view_field<std::remove_cv_t<util::aggregate_member_type<Index, Definition>>>::value);
}

template <typename Definition, std::size_t... Index>
[[nodiscard]] consteval bool aggregate_output_requires_view_invalidation(std::index_sequence<Index...>) {
    return (false || ... || [] {
        using Field = std::remove_cv_t<util::aggregate_member_type<Index, Definition>>;
        if constexpr (!output_field<Field>::value) {
            return false;
        } else {
            using TimeSeries = typename output_field<Field>::time_series_type;
            return time_series_variable<TimeSeries> || is_time_series_reference<TimeSeries> ||
                   is_time_series_list<TimeSeries>;
        }
    }());
}

template <typename Definition, std::size_t Index>
void add_signature_field(std::vector<Parameter> &parameters, const TimeSeriesType *&output) {
    using Field = std::remove_cv_t<util::aggregate_member_type<Index, Definition>>;

    if constexpr (input_field<Field>::value && !input_field<Field>::dynamic_binding) {
        constexpr auto mode = input_field<Field>::variadic_mode;
        if constexpr (mode == VariadicMode::none) {
            parameters.push_back({util::symbol{util::aggregate_member_names<Definition>[Index]},
                                  &input_type<Definition, Index>(), input_field<Field>::activity,
                                  input_field<Field>::validity});
        } else {
            parameters.push_back({util::symbol{util::aggregate_member_names<Definition>[Index]},
                                  &input_type<Definition, Index>(), input_field<Field>::activity,
                                  input_field<Field>::validity, false, mode,
                                  variadic_wiring_operations<typename input_field<Field>::time_series_type>()});
        }
    } else if constexpr (scalar_field<Field>::value) {
        parameters.push_back({util::symbol{util::aggregate_member_names<Definition>[Index]},
                              &scalar_type<Definition, Index>(), active, required, scalar_field<Field>::optional});
    } else if constexpr (output_field<Field>::value) {
        output = &output_type<Definition, Index>();
    }
}

template <typename Definition, std::size_t Index>
void add_signature_field_resolvers(std::vector<VariableResolver> &resolvers) {
    using Field = std::remove_cv_t<util::aggregate_member_type<Index, Definition>>;

    if constexpr (input_field<Field>::value && !input_field<Field>::dynamic_binding) {
        append_aggregate_resolvers<Definition, typename input_field<Field>::time_series_type>(resolvers);
    } else if constexpr (scalar_field<Field>::value) {
        append_aggregate_resolvers<Definition, typename scalar_field<Field>::value_type>(resolvers);
    } else if constexpr (state_field<Field>::value) {
        append_aggregate_resolvers<Definition, typename state_field<Field>::value_type>(resolvers);
    } else if constexpr (output_field<Field>::value) {
        append_aggregate_resolvers<Definition, typename output_field<Field>::time_series_type>(resolvers);
    }
}

template <typename Definition, std::size_t... Index>
[[nodiscard]] Signature make_signature(util::symbol name, std::index_sequence<Index...>) {
    std::vector<Parameter> parameters;
    parameters.reserve(sizeof...(Index));
    const TimeSeriesType *output = nullptr;
    auto resolvers = std::vector<VariableResolver>{};

    (add_signature_field<Definition, Index>(parameters, output), ...);
    (add_signature_field_resolvers<Definition, Index>(resolvers), ...);

    if (output == nullptr) {
        return {name, std::move(parameters), std::move(resolvers)};
    }

    return {name, std::move(parameters), *output, std::move(resolvers)};
}

template <typename Definition> [[nodiscard]] const Signature &aggregate_node_signature() {
    static const Signature signature =
        make_signature<Definition>(util::symbol{util::unqualified_type_name<Definition>()},
                                   std::make_index_sequence<util::aggregate_member_count<Definition>>{});

    return signature;
}

template <typename Definition, std::size_t... Index>
consteval std::size_t parameter_index(std::index_sequence<Index...>) {
    return (std::size_t{} + ... + static_cast<std::size_t>([] {
                using Field = std::remove_cv_t<util::aggregate_member_type<Index, Definition>>;
                return (input_field<Field>::value && !input_field<Field>::dynamic_binding) ||
                       scalar_field<Field>::value;
            }()));
}

template <typename Definition, std::size_t... Index>
[[nodiscard]] consteval bool aggregate_has_generic_state(std::index_sequence<Index...>) {
    return (false || ... || [] {
        using Field = std::remove_cv_t<util::aggregate_member_type<Index, Definition>>;

        if constexpr (state_field<Field>::value && state_field<Field>::scalar_collection) {
            return generic_scalar_state<typename state_field<Field>::value_type>;
        }

        return false;
    }());
}

template <typename Definition, std::size_t Target, std::size_t... Index>
consteval std::size_t state_index(std::index_sequence<Index...>) {
    return (
        std::size_t{} + ... +
        static_cast<std::size_t>(Index < Target &&
                                 state_field<std::remove_cv_t<util::aggregate_member_type<Index, Definition>>>::value));
}

template <typename Definition, std::size_t... Index>
consteval std::size_t state_field_count(std::index_sequence<Index...>) {
    return (std::size_t{} + ... +
            static_cast<std::size_t>(
                state_field<std::remove_cv_t<util::aggregate_member_type<Index, Definition>>>::value));
}

template <typename Definition, std::size_t Target, std::size_t... Index>
consteval std::size_t dynamic_reference_input_index(std::index_sequence<Index...>) {
    return (std::size_t{} + ... + static_cast<std::size_t>(Index < Target && [] {
                using Field = std::remove_cv_t<util::aggregate_member_type<Index, Definition>>;
                return input_field<Field>::value && input_field<Field>::dynamic_binding;
            }()));
}

template <typename Definition, std::size_t Target, std::size_t... Index>
consteval std::size_t generic_state_index(std::index_sequence<Index...>) {
    return (std::size_t{} + ... + static_cast<std::size_t>(Index < Target && [] {
                using Field = std::remove_cv_t<util::aggregate_member_type<Index, Definition>>;

                if constexpr (state_field<Field>::value && state_field<Field>::scalar_collection) {
                    return generic_scalar_state<typename state_field<Field>::value_type>;
                }

                return false;
            }()));
}

[[nodiscard]] constexpr std::size_t align_up(std::size_t offset, std::size_t alignment) noexcept {
    const auto remainder = offset % alignment;

    return remainder == 0 ? offset : offset + alignment - remainder;
}

template <typename Definition, std::size_t Target, std::size_t... Index>
consteval std::size_t aggregate_state_offset(std::index_sequence<Index...>) {
    auto offset = std::size_t{};
    auto result = std::size_t{};

    (
        [&] {
            using Field = std::remove_cv_t<util::aggregate_member_type<Index, Definition>>;

            if constexpr (state_field<Field>::value) {
                using Value = typename state_field<Field>::storage_type;
                offset = align_up(offset, alignof(Value));

                if constexpr (Index == Target) {
                    result = offset;
                }

                offset += sizeof(Value);
            }
        }(),
        ...);

    return result;
}

template <typename Definition, std::size_t... Index>
consteval std::size_t aggregate_static_state_size(std::index_sequence<Index...>) {
    auto offset = std::size_t{};
    (
        [&] {
            using Field = std::remove_cv_t<util::aggregate_member_type<Index, Definition>>;
            if constexpr (state_field<Field>::value) {
                using Value = typename state_field<Field>::storage_type;
                offset = align_up(offset, alignof(Value));
                offset += sizeof(Value);
            }
        }(),
        ...);
    return offset;
}

template <typename Definition, std::size_t Index>
consteval std::size_t aggregate_dynamic_reference_input_offset() {
    constexpr auto MemberCount = util::aggregate_member_count<Definition>;
    constexpr auto DynamicIndex =
        dynamic_reference_input_index<Definition, Index>(std::make_index_sequence<MemberCount>{});
    auto offset = aggregate_static_state_size<Definition>(std::make_index_sequence<MemberCount>{});
    offset = align_up(offset, alignof(DynamicReferenceInputStorage));
    return offset + DynamicIndex * sizeof(DynamicReferenceInputStorage);
}

template <typename Definition, std::size_t Index>
std::size_t dynamic_reference_input_offset(const CompiledNode &node) noexcept {
    constexpr auto MemberCount = util::aggregate_member_count<Definition>;
    constexpr auto DynamicIndex =
        dynamic_reference_input_index<Definition, Index>(std::make_index_sequence<MemberCount>{});

    if constexpr (aggregate_has_generic_state<Definition>(std::make_index_sequence<MemberCount>{})) {
        constexpr auto StateFieldCount = state_field_count<Definition>(std::make_index_sequence<MemberCount>{});
        return node.state_field_offset(StateFieldCount + DynamicIndex);
    } else {
        return aggregate_dynamic_reference_input_offset<Definition, Index>();
    }
}

template <typename Definition, std::size_t Index>
[[nodiscard]] auto make_runtime_field(const CompiledNode &node, std::byte *runtime_storage,
                                      EvaluationContext *context) {
    using Field = std::remove_cv_t<util::aggregate_member_type<Index, Definition>>;
    const auto runtime_node = Node{node, runtime_storage};

    if constexpr (input_field<Field>::value) {
        if constexpr (input_field<Field>::dynamic_binding) {
            constexpr auto MemberCount = util::aggregate_member_count<Definition>;
            constexpr auto TimeSeriesRuntimeTypeCount =
                aggregate_time_series_runtime_type_count<Definition>(std::make_index_sequence<MemberCount>{});
            constexpr auto GenericScalarInputCount =
                aggregate_generic_scalar_input_count<Definition>(std::make_index_sequence<MemberCount>{});
            constexpr auto UsesOutputRuntimeType =
                aggregate_uses_output_runtime_type<Definition>(std::make_index_sequence<MemberCount>{});
            constexpr auto DynamicIndex =
                dynamic_reference_input_index<Definition, Index>(std::make_index_sequence<MemberCount>{});
            const auto offset = dynamic_reference_input_offset<Definition, Index>(node);
            auto *state = static_cast<std::byte *>(runtime_node.state());
            auto &input = *reinterpret_cast<DynamicReferenceInputStorage *>(state + offset);
            const auto &runtime_type = node.dynamic_reference_input_runtime_type(
                TimeSeriesRuntimeTypeCount, GenericScalarInputCount, UsesOutputRuntimeType, DynamicIndex);
            return Field{input, runtime_type, *context, node.state_offset() + offset};
        }

        constexpr auto ParameterIndex = parameter_index<Definition>(std::make_index_sequence<Index>{});
        const auto &last_updated = runtime_node.input_last_updated(ParameterIndex);
        const auto *value = runtime_node.input_value(ParameterIndex);
        const auto modified = runtime_node.input_modified(ParameterIndex, context->evaluation_time());

        if constexpr (input_view_field<Field>::value) {
            constexpr auto RuntimeTypeIndex = aggregate_time_series_runtime_type_index<Definition, Index>(
                std::make_index_sequence<util::aggregate_member_count<Definition>>{});
            const auto &runtime_type = runtime_node.time_series_input_runtime_type(ParameterIndex, RuntimeTypeIndex);
            return Field{last_updated, value, runtime_type, modified};
        } else {
            return Field{last_updated, static_cast<const typename Field::storage_type *>(value), modified};
        }
    } else if constexpr (scalar_field<Field>::value) {
        constexpr auto ParameterIndex = parameter_index<Definition>(std::make_index_sequence<Index>{});
        const auto *data = node.parameters()[ParameterIndex].scalar_data();
        using Value = typename scalar_field<Field>::value_type;
        using Parameter = typename scalar_field<Field>::parameter_type;

        if constexpr (generic_scalar_input_field<Field>::value) {
            constexpr auto TimeSeriesRuntimeTypeCount = aggregate_time_series_runtime_type_count<Definition>(
                std::make_index_sequence<util::aggregate_member_count<Definition>>{});
            constexpr auto GenericScalarInputIndex = aggregate_generic_scalar_input_index<Definition, Index>(
                std::make_index_sequence<util::aggregate_member_count<Definition>>{});

            const auto &runtime_type =
                node.scalar_input_runtime_type(TimeSeriesRuntimeTypeCount, GenericScalarInputIndex);
            return Field::from(runtime_type, data);
        } else {
            using Traits = scalar_storage_traits<Parameter>;

            if constexpr (scalar_view<Field>::value) {
                return Field::from(*static_cast<const typename Traits::stored_type *>(data));
            } else if constexpr (scalar_field<Field>::optional) {
                return data == nullptr ? Field{}
                                       : Field{scalar_api_value<Value>(
                                             Traits::read(*static_cast<const typename Traits::stored_type *>(data)))};
            } else {
                return scalar_api_value<Value>(
                    Traits::read(*static_cast<const typename Traits::stored_type *>(data)));
            }
        }
    } else if constexpr (state_field<Field>::value) {
        using Declared = typename state_field<Field>::value_type;
        constexpr auto MemberCount = util::aggregate_member_count<Definition>;
        constexpr auto StateIndex = state_index<Definition, Index>(std::make_index_sequence<MemberCount>{});
        const auto offset = [&] {
            if constexpr (aggregate_has_generic_state<Definition>(std::make_index_sequence<MemberCount>{})) {
                return node.state_field_offset(StateIndex);
            } else {
                return aggregate_state_offset<Definition, Index>(std::make_index_sequence<MemberCount>{});
            }
        }();
        auto *value = static_cast<std::byte *>(runtime_node.state()) + offset;

        if constexpr (state_field<Field>::scalar_collection && generic_scalar_state<Declared>) {
            constexpr auto GenericStateIndex =
                generic_state_index<Definition, Index>(std::make_index_sequence<MemberCount>{});
            const auto &runtime_type = node.generic_state_runtime_type(GenericStateIndex);
            return Field{runtime_type, value};
        } else {
            using Value = typename state_field<Field>::storage_type;
            return Field{*reinterpret_cast<Value *>(value)};
        }
    } else {
        auto &last_updated = runtime_node.output_last_updated();

        if constexpr (output_view_field<Field>::value) {
            constexpr auto TimeSeriesRuntimeTypeCount = aggregate_time_series_runtime_type_count<Definition>(
                std::make_index_sequence<util::aggregate_member_count<Definition>>{});
            constexpr auto GenericScalarInputCount = aggregate_generic_scalar_input_count<Definition>(
                std::make_index_sequence<util::aggregate_member_count<Definition>>{});
            return output_factory::make<typename Field::time_series_type>(
                last_updated, runtime_node.output_value(),
                node.output_runtime_type(TimeSeriesRuntimeTypeCount, GenericScalarInputCount), *context);
        } else {
            auto &value = *static_cast<typename Field::storage_type *>(runtime_node.output_value());

            return output_factory::make<typename Field::time_series_type>(last_updated, value, *context);
        }
    }
}

template <typename Definition, std::size_t... Index>
NodeStateLayout
aggregate_state_layout(const ResolvedSignature &,
                       [[maybe_unused]] std::span<const ScalarRuntimeType *const> parameter_runtime_types,
                       [[maybe_unused]] std::span<const ScalarType *const> state_types, std::index_sequence<Index...>) {
    auto result = NodeStateLayout{};
    result.field_offsets.reserve(state_types.size());
    [[maybe_unused]] auto state = std::size_t{};

    (
        [&] {
            using Field = std::remove_cv_t<util::aggregate_member_type<Index, Definition>>;

            if constexpr (state_field<Field>::value) {
                using Declared = typename state_field<Field>::value_type;
                auto size = std::size_t{};
                auto alignment = std::size_t{};

                if constexpr (state_field<Field>::scalar_collection && generic_scalar_state<Declared>) {
                    const auto *type = state_types[state];
                    const auto *runtime_type = &resolve_scalar_runtime_type(*type, parameter_runtime_types);
                    size = runtime_type->size();
                    alignment = runtime_type->alignment();
                    result.generic_runtime_types.push_back(runtime_type);
                } else {
                    using Stored = typename state_field<Field>::storage_type;
                    size = sizeof(Stored);
                    alignment = alignof(Stored);
                }

                result.size = align_up(result.size, alignment);
                result.field_offsets.push_back(result.size);
                result.size += size;
                result.alignment = std::max(result.alignment, alignment);
                ++state;
            }
        }(),
        ...);

    return result;
}

template <typename Definition, std::size_t... Index>
void construct_aggregate_state(const NodeStateLayout &layout, [[maybe_unused]] void *storage,
                               std::index_sequence<Index...>) noexcept {
    [[maybe_unused]] auto state = std::size_t{};
    [[maybe_unused]] auto generic = std::size_t{};

    (
        [&] {
            using Field = std::remove_cv_t<util::aggregate_member_type<Index, Definition>>;

            if constexpr (state_field<Field>::value) {
                using Declared = typename state_field<Field>::value_type;
                auto *address = static_cast<std::byte *>(storage) + layout.field_offsets[state++];

                if constexpr (state_field<Field>::scalar_collection && generic_scalar_state<Declared>) {
                    layout.generic_runtime_types[generic++]->construct_initial_value(address);
                } else {
                    using Value = typename state_field<Field>::storage_type;
                    std::construct_at(reinterpret_cast<Value *>(address));
                }
            }
        }(),
        ...);

    constexpr auto StateFieldCount = state_field_count<Definition>(std::index_sequence<Index...>{});
    constexpr auto DynamicReferenceInputCount =
        (std::size_t{} + ... + static_cast<std::size_t>([] {
             using Field = std::remove_cv_t<util::aggregate_member_type<Index, Definition>>;
             return input_field<Field>::value && input_field<Field>::dynamic_binding;
         }()));
    for (auto index = std::size_t{}; index < DynamicReferenceInputCount; ++index) {
        const auto offset = layout.field_offsets[StateFieldCount + index];
        std::construct_at(reinterpret_cast<DynamicReferenceInputStorage *>(static_cast<std::byte *>(storage) + offset));
    }
}

template <typename Definition, std::size_t... Index>
void destroy_aggregate_state(const CompiledNode &node, void *runtime_storage, EvaluationContext *,
                             std::index_sequence<Index...>) noexcept {
    const auto runtime_node = Node{node, runtime_storage};
    auto memory = current_scalar_allocator();
    constexpr auto MemberCount = util::aggregate_member_count<Definition>;

    (
        [&] {
            using Field = std::remove_cv_t<util::aggregate_member_type<Index, Definition>>;

            if constexpr (state_field<Field>::value && state_field<Field>::scalar_collection) {
                using Declared = typename state_field<Field>::value_type;
                constexpr auto StateIndex = state_index<Definition, Index>(std::make_index_sequence<MemberCount>{});
                const auto offset = [&] {
                    if constexpr (aggregate_has_generic_state<Definition>(std::make_index_sequence<MemberCount>{})) {
                        return node.state_field_offset(StateIndex);
                    } else {
                        return aggregate_state_offset<Definition, Index>(std::make_index_sequence<MemberCount>{});
                    }
                }();
                auto *value = static_cast<std::byte *>(runtime_node.state()) + offset;

                if constexpr (generic_scalar_state<Declared>) {
                    constexpr auto GenericStateIndex =
                        generic_state_index<Definition, Index>(std::make_index_sequence<MemberCount>{});
                    node.generic_state_runtime_type(GenericStateIndex).destroy(value, memory);
                } else {
                    using Stored = typename state_field<Field>::storage_type;
                    scalar_storage_traits<Declared>::destroy(reinterpret_cast<Stored *>(value), memory);
                }
            }
        }(),
        ...);
}

template <typename Definition>
void destroy_aggregate_state(const CompiledNode &node, void *runtime_storage, EvaluationContext *context) noexcept {
    destroy_aggregate_state<Definition>(node, runtime_storage, context,
                                        std::make_index_sequence<util::aggregate_member_count<Definition>>{});
}

template <typename Definition, std::size_t... Index>
[[nodiscard]] consteval bool aggregate_state_requires_destruction(std::index_sequence<Index...>) noexcept {
    return (... || state_field<std::remove_cv_t<util::aggregate_member_type<Index, Definition>>>::scalar_collection);
}

template <typename Definition, std::size_t... Index>
[[nodiscard]] consteval bool aggregate_has_dynamic_reference_inputs(std::index_sequence<Index...>) noexcept {
    return (false || ... || [] {
        using Field = std::remove_cv_t<util::aggregate_member_type<Index, Definition>>;
        return input_field<Field>::value && input_field<Field>::dynamic_binding;
    }());
}

template <typename Definition, std::size_t Index>
void unbind_dynamic_reference_input(const CompiledNode &node, void *runtime_storage, EvaluationContext &context) {
    using Field = std::remove_cv_t<util::aggregate_member_type<Index, Definition>>;
    if constexpr (input_field<Field>::value && input_field<Field>::dynamic_binding) {
        const auto offset = dynamic_reference_input_offset<Definition, Index>(node);
        auto *state = static_cast<std::byte *>(Node{node, runtime_storage}.state());
        auto &input = *reinterpret_cast<DynamicReferenceInputStorage *>(state + offset);
        input.unbind(context.runtime_graph(), node.state_offset() + offset);
    }
}

template <typename Definition, std::size_t... Index>
void unbind_dynamic_reference_inputs(const CompiledNode &node, void *runtime_storage, EvaluationContext &context,
                                     std::index_sequence<Index...>) noexcept {
    (unbind_dynamic_reference_input<Definition, Index>(node, runtime_storage, context), ...);
}

template <typename Definition>
void unbind_dynamic_reference_inputs(const CompiledNode &node, void *runtime_storage,
                                     EvaluationContext &context) noexcept {
    unbind_dynamic_reference_inputs<Definition>(
        node, runtime_storage, context, std::make_index_sequence<util::aggregate_member_count<Definition>>{});
}

template <typename Definition, auto Function, std::size_t... Index>
decltype(auto) invoke_aggregate_node(const CompiledNode &node, void *runtime_storage, EvaluationContext *context,
                                     std::index_sequence<Index...>) {
    [[maybe_unused]] auto *storage = static_cast<std::byte *>(runtime_storage);
    [[maybe_unused]] auto output_view_storage =
        aggregate_output_view_storage<aggregate_node_has_output<Definition>()>{context};
    auto definition = Definition{make_runtime_field<Definition, Index>(node, storage, context)...};

    if constexpr (std::invocable<decltype(Function), const Definition &, EvaluationContext *>) {
        return std::invoke(Function, std::as_const(definition), context);
    } else {
        return std::invoke(Function, std::as_const(definition));
    }
}

template <typename Definition, auto Function>
decltype(auto) invoke_aggregate_node(const CompiledNode &node, void *runtime_storage, EvaluationContext *context) {
    return invoke_aggregate_node<Definition, Function>(
        node, runtime_storage, context, std::make_index_sequence<util::aggregate_member_count<Definition>>{});
}

template <typename Definition>
bool evaluate_aggregate_node(const CompiledNode &node, void *runtime_storage, EvaluationContext *context) {
    if constexpr (aggregate_node_evaluate_returns_bool<Definition>) {
        return invoke_aggregate_node<Definition, &Definition::evaluate>(node, runtime_storage, context);
    } else {
        invoke_aggregate_node<Definition, &Definition::evaluate>(node, runtime_storage, context);
        return true;
    }
}

template <typename Definition>
void finalize_aggregate_node_state(const CompiledNode &node, void *runtime_storage,
                                   EvaluationContext *context) noexcept {
    if constexpr (aggregate_node_has_finalize<Definition>) {
        invoke_aggregate_node<Definition, &Definition::finalize>(node, runtime_storage, context);
    }

    unbind_dynamic_reference_inputs<Definition>(node, runtime_storage, *context);
    destroy_aggregate_state<Definition>(node, runtime_storage, context);
}

template <typename Definition>
void start_aggregate_node(const CompiledNode &node, void *runtime_storage, EvaluationContext *context) {
    invoke_aggregate_node<Definition, &Definition::start>(node, runtime_storage, context);
}

template <typename Definition>
void stop_aggregate_node(const CompiledNode &node, void *runtime_storage, EvaluationContext *context) {
    if constexpr (aggregate_node_has_stop<Definition>) {
        invoke_aggregate_node<Definition, &Definition::stop>(node, runtime_storage, context);
    }

    unbind_dynamic_reference_inputs<Definition>(node, runtime_storage, *context);
}

template <typename Definition>
void invalidate_aggregate_node(const CompiledNode &node, void *runtime_storage, EvaluationContext *context) {
    invoke_aggregate_node<Definition, &Definition::on_invalidation>(node, runtime_storage, context);
}

template <typename Definition, std::size_t... Index>
void invalidate_aggregate_output(const CompiledNode &node, void *runtime_storage, EvaluationContext *context,
                                 std::index_sequence<Index...>) {
    auto *storage = static_cast<std::byte *>(runtime_storage);
    [[maybe_unused]] auto output_view_storage =
        aggregate_output_view_storage<aggregate_node_has_output<Definition>()>{context};
    auto definition = Definition{make_runtime_field<Definition, Index>(node, storage, context)...};
    auto fields = util::detail::aggregate_tie<sizeof...(Index)>::apply(definition);

    (
        [&] {
            using Field = std::remove_cv_t<util::aggregate_member_type<Index, Definition>>;
            if constexpr (output_field<Field>::value) {
                std::get<Index>(fields).invalidate();
            }
        }(),
        ...);
}

template <typename Definition>
void invalidate_aggregate_output(const CompiledNode &node, void *runtime_storage, EvaluationContext *context) {
    invalidate_aggregate_output<Definition>(node, runtime_storage, context,
                                            std::make_index_sequence<util::aggregate_member_count<Definition>>{});
}

} // namespace bg::details
