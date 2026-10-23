#pragma once

#include <bg/evaluation_context.hpp>
#include <bg/graph/details/variadic_wiring_operations.hpp>
#include <bg/graph/wiring_parameter.hpp>
#include <bg/graph/wiring_arguments.hpp>
#include <bg/graph/node.hpp>
#include <bg/node/details/aggregate_resolver.hpp>
#include <bg/node/input.hpp>
#include <bg/node/output.hpp>
#include <bg/node/state.hpp>
#include <bg/node/state_layout.hpp>
#include <bg/scalar/cpp_runtime_type.hpp>
#include <bg/time_series/ref/dynamic_reference_input.hpp>
#include <bg/time_series/dynamic_value_input.hpp>
#include <bg/time_series/ref/ref_runtime_type.hpp>
#include <bg/time_series/input_data.hpp>
#include <bg/time_series/cpp_runtime_type.hpp>
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
#include <bg/type/variable_registry.hpp>
#include <bg/type/time_series_list_type.hpp>
#include <bg/type/time_series_reference_type.hpp>
#include <bg/type/time_series_set_type.hpp>
#include <bg/type/time_series_value_type.hpp>
#include <bg/type/type_registry.hpp>
#include <bg/type/type_variable.hpp>
#include <bg/type/time_series_variable.hpp>
#include <bg/util/static_storage.hpp>
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

template <typename T> struct dynamic_value_binding_time_series : std::false_type {};

template <typename T, auto... Properties>
    requires(!scalar_variable<T>)
struct dynamic_value_binding_time_series<TS<T, Properties...>> : std::true_type {};

template <typename T>
inline constexpr bool is_dynamic_value_binding_time_series = dynamic_value_binding_time_series<T>::value;

template <typename T> struct input_field {
    static constexpr bool value = false;
    static constexpr bool reflected_name = true;
    static constexpr bool dynamic_activity = false;
    static constexpr bool dynamic_binding = false;
    static constexpr bool context = false;
    static constexpr bool valid_items = false;
};

template <typename TimeSeries, auto... Properties> struct input_field<In<TimeSeries, Properties...>> {
    using Input = In<TimeSeries, Properties...>;

    static constexpr bool value = true;
    static constexpr bool reflected_name = Input::name.empty();
    static constexpr InputActivity activity = Input::activity;
    static constexpr InputValidity validity = Input::validity;
    static constexpr bool dynamic_activity = Input::dynamic_activity;
    static constexpr bool dynamic_binding = Input::dynamic_binding;
    static constexpr bool context = (false || ... || is_context<decltype(Properties)>);
    static constexpr bool valid_items = basic_input_properties<true, Properties...>::valid_items;
    static constexpr auto variadic_mode = [] {
        if constexpr (requires { Input::variadic_mode; }) {
            return Input::variadic_mode;
        }
        return VariadicMode::none;
    }();

    static_assert(!dynamic_binding || is_time_series_reference<TimeSeries> ||
                      is_dynamic_value_binding_time_series<TimeSeries>,
                  "dynamic_binding requires a reference or TS<T> input");
    static_assert(!dynamic_binding || is_time_series_reference<TimeSeries> || validity == InputValidity::optional,
                  "a dynamic value input must be optional");
    static_assert(!dynamic_binding || !is_dynamic_value_binding_time_series<TimeSeries> ||
                      activity == InputActivity::active,
                  "a dynamic value input must be active");

    using time_series_type = TimeSeries;
};

template <typename T> struct output_field {
    static constexpr bool value = false;

    using time_series_type = void;
    using output_type = void;
};

template <typename TimeSeries, auto... Options>
struct output_field<Out<TimeSeries, Options...>> {
    static constexpr bool value = true;

    using time_series_type = typename output_declaration_traits<Out<TimeSeries, Options...>>::time_series_type;
    using output_type = Out<TimeSeries, Options...>;
};

template <typename... Outputs> struct first_output;

template <typename First, typename... Rest> struct first_output<First, Rest...> {
    using type = std::conditional_t<std::same_as<First, void>, typename first_output<Rest...>::type, First>;
};

template <> struct first_output<> {
    using type = void;
};

template <typename Definition, std::size_t... Index>
auto aggregate_output_declaration_type(std::index_sequence<Index...>) -> typename first_output<
    typename output_field<std::remove_cv_t<util::aggregate_member_type<Index, Definition>>>::output_type...>::type;

template <typename Definition>
using aggregate_output_declaration_t = decltype(aggregate_output_declaration_type<Definition>(
    std::make_index_sequence<util::aggregate_member_count<Definition>>{}));

template <typename Definition>
using aggregate_output_time_series_t = typename output_field<aggregate_output_declaration_t<Definition>>::time_series_type;

template <typename T> inline constexpr bool generic_scalar_state = scalar_declaration_traits<T>::generic;

template <typename T> struct scalar_field {
    static constexpr bool value = scalar_declaration_traits<T>::supports_implicit_argument &&
                                  requires { cpp_scalar_type<canonical_scalar_type_t<T>>(); };
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

template <typename T> struct scalar_view : std::false_type {};

template <typename T> struct scalar_view<Scalar<T>> : std::true_type {};

template <typename T> struct promoted_field {
    using parameter_type = typename scalar_field<T>::parameter_type;

    static constexpr bool value = scalar_field<T>::value && !scalar_view<T>::value &&
                                  scalar_declaration_traits<parameter_type>::supports_implicit_argument;
    static constexpr bool optional = scalar_field<T>::optional;

    using value_type = typename scalar_field<T>::value_type;
};

template <typename T> struct state_field {
    static constexpr bool value = false;
    static constexpr bool scalar_collection = false;
    static constexpr bool runtime_typed = false;
};

template <typename T> struct state_field<State<T>> {
    static constexpr bool value = true;
    static constexpr bool scalar_collection = scalar_declaration_traits<T>::is_collection;
    static constexpr bool runtime_typed = generic_scalar_state<T>;

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
                if constexpr (state_field<Field>::runtime_typed) {
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
std::vector<const TimeSeriesType *> aggregate_dynamic_input_types(const TypeMap &type_map,
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

template <typename T> struct input_runtime_type_field : std::false_type {};

template <typename TimeSeries, auto... Properties>
struct input_runtime_type_field<In<TimeSeries, Properties...>>
    : std::bool_constant<input_requires_runtime_type<In<TimeSeries, Properties...>>> {};

template <typename Field, bool = promoted_field<Field>::value>
struct promoted_input_runtime_type_field : std::false_type {};

template <typename Field>
struct promoted_input_runtime_type_field<Field, true>
    : std::bool_constant<input_requires_runtime_type<In<TS<typename promoted_field<Field>::parameter_type>>>> {};

template <typename T> struct generic_output_field : std::false_type {};

template <typename TimeSeries, auto... Options>
struct generic_output_field<Out<TimeSeries, Options...>>
    : std::bool_constant<generic_time_series<TimeSeries>> {};

template <typename T> struct output_runtime_type_field : std::false_type {};

template <typename TimeSeries, auto... Options>
struct output_runtime_type_field<Out<TimeSeries, Options...>>
    : std::bool_constant<output_requires_runtime_type<Out<TimeSeries, Options...>>> {};

template <typename Definition, std::size_t... Index>
consteval bool aggregate_uses_output_runtime_type(std::index_sequence<Index...>) {
    constexpr auto output_field_uses_runtime_type =
        (output_runtime_type_field<std::remove_cv_t<util::aggregate_member_type<Index, Definition>>>::value || ...);
    constexpr auto definition_selects_runtime_type =
        !std::same_as<aggregate_output_declaration_t<Definition>, void> &&
        requires(const WiringArguments &arguments) {
            { Definition::output_runtime_type(arguments) } -> std::same_as<const TimeSeriesRuntimeType &>;
        };
    return output_field_uses_runtime_type || definition_selects_runtime_type;
}

template <typename T> consteval bool scalar_input_requires_runtime_type() {
    if constexpr (generic_scalar_state<T> || hierarchical_scalar_declaration<T>) {
        return true;
    }
    return scalar_storage_supports_alternative_layouts<T>;
}

template <typename T> struct scalar_input_runtime_type_field : std::false_type {};

template <typename T>
struct scalar_input_runtime_type_field<Scalar<T>>
    : std::bool_constant<scalar_input_requires_runtime_type<T>()> {};

template <typename T>
    requires(scalar_field<T>::value && !scalar_view<T>::value)
struct scalar_input_runtime_type_field<T>
    : std::bool_constant<scalar_input_requires_runtime_type<typename scalar_field<T>::parameter_type>()> {};

template <typename T> struct scalar_variable_view : std::false_type {};

template <scalar_variable Variable> struct scalar_variable_view<Scalar<Variable>> : std::true_type {};

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
                                             promoted_field<Field>::value || scalar_field<Field>::value ||
                                             state_field<Field>::value;

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
template <typename Definition, bool HasOutput> class aggregate_output_state {
  public:
    aggregate_output_state(const CompiledNode &, void *, EvaluationContext *) noexcept {}

    OutputState *output_state() noexcept { return nullptr; }
};

template <typename Definition> class aggregate_output_state<Definition, true> {
  public:
    aggregate_output_state(const CompiledNode &node, void *runtime_storage, EvaluationContext *context)
        : output_state_{output_runtime_type(node)} {
        const auto runtime_node = Node{node, runtime_storage};
        output_state_.bind(runtime_node.output_last_updated(), runtime_node.output_value(), *context);
    }

    OutputState *output_state() noexcept { return &output_state_; }

  private:
    static const TimeSeriesRuntimeType &output_runtime_type(const CompiledNode &node) {
        if constexpr (aggregate_uses_output_runtime_type<Definition>(
                          std::make_index_sequence<util::aggregate_member_count<Definition>>{})) {
            return node.output_runtime_type();
        } else {
            return cpp_time_series_runtime_type<aggregate_output_time_series_t<Definition>,
                                                TimeSeriesTypeRole::output>();
        }
    }

    OutputState output_state_;
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

template <typename Definition> [[nodiscard]] consteval bool aggregate_uses_cycle_callbacks() {
    if constexpr (requires { Definition::uses_cycle_callbacks; }) {
        return Definition::uses_cycle_callbacks;
    }

    return false;
}

template <typename Definition> [[nodiscard]] consteval bool aggregate_schedules_on_start() {
    if constexpr (requires { Definition::schedule_on_start; }) {
        return Definition::schedule_on_start;
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
    } else if constexpr (TimeSeries::has_explicit_guarantee || input_field<Field>::dynamic_binding) {
        return time_series_type_for<Definition, Index, TimeSeriesTypeRole::input, TimeSeries>(
            guarantee_parameter<TimeSeries::guarantee>());
    } else {
        static const auto guarantee = util::static_storage<GuaranteeVariable>{
            util::symbol{util::aggregate_member_names<Definition>[Index]}};
        [[maybe_unused]] static const auto &registration = VariableRegistry::register_static(guarantee.get());
        return time_series_type_for<Definition, Index, TimeSeriesTypeRole::input, TimeSeries>(guarantee.get());
    }
}

template <typename Scope, std::size_t Index, typename Value>
const Type &promoted_type(util::symbol name) {
    using TimeSeries = TS<Value>;

    static const auto guarantee = util::static_storage<GuaranteeVariable>{name};
    [[maybe_unused]] static const auto &guarantee_registration = VariableRegistry::register_static(guarantee.get());
    const auto &time_series = time_series_type_for<Scope, Index, TimeSeriesTypeRole::input, TimeSeries>(guarantee.get());
    static const auto variable = util::static_storage<TypeVariableType<Type>>{
        name, std::vector<const Type *>{&scalar_type_for<Value>(), &time_series}};
    [[maybe_unused]] static const auto &type_registration = TypeRegistry::register_static(variable.get());

    return variable.get();
}

template <typename Definition, std::size_t Index> const Type &aggregate_promoted_type() {
    using Field = std::remove_cv_t<util::aggregate_member_type<Index, Definition>>;
    using Value = typename promoted_field<Field>::parameter_type;
    return promoted_type<Definition, Index, Value>(util::symbol{util::aggregate_member_names<Definition>[Index]});
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
        return time_series_type_for<Definition, Index, TimeSeriesTypeRole::output, TimeSeries>(
            TimeSeriesGuarantee::ordinary);
    }
}

template <typename Definition, std::size_t Target, std::size_t... Index>
consteval std::size_t aggregate_dynamic_activity_input_index(std::index_sequence<Index...>) {
    return (std::size_t{} + ... +
            static_cast<std::size_t>(
                Index < Target &&
                input_field<std::remove_cv_t<util::aggregate_member_type<Index, Definition>>>::dynamic_activity));
}

template <typename Definition, std::size_t... Index>
[[nodiscard]] consteval bool aggregate_output_requires_view_invalidation(std::index_sequence<Index...>) {
    return (false || ... || [] {
        using Field = std::remove_cv_t<util::aggregate_member_type<Index, Definition>>;
        return output_field<Field>::value && requires(Field output) { output.invalidate(); };
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
                                  input_field<Field>::validity, false, mode, nullptr, input_field<Field>::context,
                                  input_field<Field>::dynamic_activity, nullptr, input_field<Field>::valid_items});
        } else {
            parameters.push_back({util::symbol{util::aggregate_member_names<Definition>[Index]},
                                  &input_type<Definition, Index>(), input_field<Field>::activity,
                                  input_field<Field>::validity, false, mode,
                                  variadic_wiring_operations<typename input_field<Field>::time_series_type>(), false,
                                  input_field<Field>::dynamic_activity, nullptr, input_field<Field>::valid_items});
        }
    } else if constexpr (promoted_field<Field>::value) {
        constexpr auto validity = promoted_field<Field>::optional ? InputValidity::optional : InputValidity::required;
        parameters.push_back({util::symbol{util::aggregate_member_names<Definition>[Index]},
                              &aggregate_promoted_type<Definition, Index>(), active, validity,
                              promoted_field<Field>::optional});
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
    } else if constexpr (promoted_field<Field>::value) {
        append_aggregate_resolvers<Definition, typename promoted_field<Field>::value_type>(resolvers);
        append_aggregate_resolvers<Definition, TS<typename promoted_field<Field>::parameter_type>>(resolvers);
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
    static const Signature signature = [] {
        auto result = make_signature<Definition>(
            util::symbol{util::unqualified_type_name<Definition>()},
            std::make_index_sequence<util::aggregate_member_count<Definition>>{});
        if constexpr (requires { typename Definition::defaults; }) {
            const auto defaults = typename Definition::defaults{};
            const auto parameters = aggregate_wiring_parameters(defaults);
            return result.with_defaults(parameters);
        }
        return result;
    }();

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
    return (false || ... ||
            state_field<std::remove_cv_t<util::aggregate_member_type<Index, Definition>>>::runtime_typed);
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
    return (
        std::size_t{} + ... +
        static_cast<std::size_t>(state_field<std::remove_cv_t<util::aggregate_member_type<Index, Definition>>>::value));
}

template <typename Definition, std::size_t Target, std::size_t... Index>
consteval std::size_t dynamic_input_index(std::index_sequence<Index...>) {
    return (std::size_t{} + ... + static_cast<std::size_t>(Index < Target && [] {
                using Field = std::remove_cv_t<util::aggregate_member_type<Index, Definition>>;
                return input_field<Field>::value && input_field<Field>::dynamic_binding;
            }()));
}

template <typename Definition, std::size_t Target, std::size_t... Index>
consteval std::size_t generic_state_index(std::index_sequence<Index...>) {
    return (std::size_t{} + ... +
            static_cast<std::size_t>(Index < Target &&
                                     state_field<std::remove_cv_t<util::aggregate_member_type<Index, Definition>>>::
                                         runtime_typed));
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

template <typename Field>
using dynamic_input_storage_type = std::conditional_t<
    is_time_series_reference<typename Field::time_series_type>, DynamicReferenceInputStorage, DynamicValueInputStorage>;

template <typename Definition, std::size_t Target, std::size_t... Index>
consteval std::size_t aggregate_dynamic_input_offset(std::index_sequence<Index...>) {
    auto offset = aggregate_static_state_size<Definition>(
        std::make_index_sequence<util::aggregate_member_count<Definition>>{});
    auto result = std::size_t{};
    (
        [&] {
            using Field = std::remove_cv_t<util::aggregate_member_type<Index, Definition>>;
            if constexpr (input_field<Field>::value && input_field<Field>::dynamic_binding) {
                using Storage = dynamic_input_storage_type<Field>;
                offset = align_up(offset, alignof(Storage));
                if constexpr (Index == Target) {
                    result = offset;
                }
                offset += sizeof(Storage);
            }
        }(),
        ...);
    return result;
}

template <typename Definition, std::size_t Index>
std::size_t dynamic_input_offset(const CompiledNode &node) noexcept {
    constexpr auto MemberCount = util::aggregate_member_count<Definition>;
    constexpr auto DynamicIndex =
        dynamic_input_index<Definition, Index>(std::make_index_sequence<MemberCount>{});

    if constexpr (aggregate_has_generic_state<Definition>(std::make_index_sequence<MemberCount>{})) {
        constexpr auto StateFieldCount = state_field_count<Definition>(std::make_index_sequence<MemberCount>{});
        return node.state_field_offset(StateFieldCount + DynamicIndex);
    } else {
        return aggregate_dynamic_input_offset<Definition, Index>(std::make_index_sequence<MemberCount>{});
    }
}

template <typename Definition, std::size_t Index>
[[nodiscard]] auto make_runtime_field(const CompiledNode &node, std::byte *runtime_storage,
                                      EvaluationContext *context, OutputState *output_state,
                                      bool invalidation = false) {
    using Field = std::remove_cv_t<util::aggregate_member_type<Index, Definition>>;
    const auto runtime_node = Node{node, runtime_storage};

    if constexpr (input_field<Field>::value) {
        if constexpr (input_field<Field>::dynamic_binding) {
            constexpr auto MemberCount = util::aggregate_member_count<Definition>;
            constexpr auto DynamicIndex =
                dynamic_input_index<Definition, Index>(std::make_index_sequence<MemberCount>{});
            const auto offset = dynamic_input_offset<Definition, Index>(node);
            auto *state = static_cast<std::byte *>(runtime_node.state());
            const auto &runtime_type = node.dynamic_input_runtime_type(DynamicIndex);
            using Storage = dynamic_input_storage_type<Field>;
            auto &input = *reinterpret_cast<Storage *>(state + offset);
            return Field{input, runtime_type, *context, node.state_offset() + offset};
        }

        constexpr auto ParameterIndex = parameter_index<Definition>(std::make_index_sequence<Index>{});
        constexpr auto ControlledInputIndex =
            aggregate_dynamic_activity_input_index<Definition, Index>(std::make_index_sequence<Index>{});
        const auto input = runtime_node.input_data(ParameterIndex, *context);
        const auto modified = input.last_updated.time() == context->evaluation_time();

        if constexpr (input_runtime_type_field<Field>::value) {
            const auto &runtime_type = runtime_node.time_series_input_runtime_type(ParameterIndex, *context);
            const auto data = TimeSeriesInputData{runtime_type, input.last_updated, input.value, modified};
            if constexpr (input_field<Field>::dynamic_activity) {
                return Field{data, runtime_node.input_activity(ControlledInputIndex)};
            } else {
                return Field{data};
            }
        } else {
            if constexpr (input_field<Field>::dynamic_activity) {
                return Field{input.last_updated, static_cast<const typename Field::storage_type *>(input.value),
                             modified,
                             runtime_node.input_activity(ControlledInputIndex)};
            } else {
                return Field{input.last_updated, static_cast<const typename Field::storage_type *>(input.value),
                             modified};
            }
        }
    } else if constexpr (promoted_field<Field>::value) {
        constexpr auto ParameterIndex = parameter_index<Definition>(std::make_index_sequence<Index>{});
        using Value = typename promoted_field<Field>::value_type;
        using Parameter = typename promoted_field<Field>::parameter_type;
        const auto &binding = node.parameters()[ParameterIndex];

        if (binding.mode() == ParameterBindingMode::scalar) {
            const auto *data = binding.scalar_data();
            if constexpr (promoted_field<Field>::optional) {
                if (data == nullptr) {
                    return Field{};
                }
            }

            auto value = [&] {
                if constexpr (scalar_input_runtime_type_field<Field>::value) {
                    const auto scalar = Scalar<>::from(node.scalar_input_runtime_type(ParameterIndex), data);
                    return scalar_api_value<Value>(scalar_cast<Parameter>(scalar).value());
                } else {
                    using Traits = scalar_storage_traits<Parameter>;
                    return scalar_api_value<Value>(
                        Traits::read(*static_cast<const typename Traits::stored_type *>(data)));
                }
            }();
            if constexpr (promoted_field<Field>::optional) {
                return Field{std::move(value)};
            } else {
                return value;
            }
        }

        if (invalidation) {
            if constexpr (promoted_field<Field>::optional) {
                return Field{};
            } else if constexpr (std::default_initializable<Value>) {
                return Value{};
            } else {
                throw std::logic_error{"invalidated promoted input requires a default-initializable value"};
            }
        }

        const auto input = runtime_node.input_data(ParameterIndex, *context);
        const auto modified = input.last_updated.time() == context->evaluation_time();
        using Input = In<TS<Parameter>>;
        const auto value = [&] {
            if constexpr (input_requires_runtime_type<Input>) {
                const auto &runtime_type =
                    runtime_node.time_series_input_runtime_type(ParameterIndex, *context);
                return Input{TimeSeriesInputData{runtime_type, input.last_updated, input.value, modified}};
            } else {
                return Input{input.last_updated,
                             static_cast<const typename Input::storage_type *>(input.value), modified};
            }
        }();

        if constexpr (promoted_field<Field>::optional) {
            if (!value.valid()) {
                return Field{};
            }
        }

        auto result = scalar_api_value<Value>(value.value());
        if constexpr (promoted_field<Field>::optional) {
            return Field{std::move(result)};
        } else {
            return result;
        }
    } else if constexpr (scalar_field<Field>::value) {
        constexpr auto ParameterIndex = parameter_index<Definition>(std::make_index_sequence<Index>{});
        const auto *data = node.parameters()[ParameterIndex].scalar_data();
        using Value = typename scalar_field<Field>::value_type;
        using Parameter = typename scalar_field<Field>::parameter_type;

        if constexpr (scalar_field<Field>::optional) {
            if (data == nullptr) {
                return Field{};
            }
        }

        if constexpr (scalar_input_runtime_type_field<Field>::value) {
            const auto &runtime_type = node.scalar_input_runtime_type(ParameterIndex);

            if constexpr (scalar_view<Field>::value) {
                return Field::from(runtime_type, data);
            } else if constexpr (scalar_field<Field>::optional) {
                const auto scalar = Scalar<>::from(runtime_type, data);
                return Field{scalar_api_value<Value>(scalar_cast<Parameter>(scalar).value())};
            } else {
                const auto scalar = Scalar<>::from(runtime_type, data);
                return scalar_api_value<Value>(scalar_cast<Parameter>(scalar).value());
            }
        } else {
            using Traits = scalar_storage_traits<Parameter>;

            if constexpr (scalar_view<Field>::value) {
                return Field::from(*static_cast<const typename Traits::stored_type *>(data));
            } else if constexpr (scalar_field<Field>::optional) {
                return Field{scalar_api_value<Value>(
                    Traits::read(*static_cast<const typename Traits::stored_type *>(data)))};
            } else {
                return scalar_api_value<Value>(Traits::read(*static_cast<const typename Traits::stored_type *>(data)));
            }
        }
    } else if constexpr (state_field<Field>::value) {
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

        if constexpr (state_field<Field>::runtime_typed) {
            constexpr auto GenericStateIndex =
                generic_state_index<Definition, Index>(std::make_index_sequence<MemberCount>{});
            const auto &runtime_type = node.generic_state_runtime_type(GenericStateIndex);
            return Field{runtime_type, value};
        } else {
            using Value = typename state_field<Field>::storage_type;
            return Field{*reinterpret_cast<Value *>(value)};
        }
    } else {
        return output_factory::make<Field>(TimeSeriesOutputBinding{*output_state});
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
                auto size = std::size_t{};
                auto alignment = std::size_t{};

                if constexpr (state_field<Field>::runtime_typed) {
                    const auto *type = state_types[state];
                    const auto *runtime_type = &resolve_scalar_runtime_type(*type, parameter_runtime_types);
                    if (!runtime_type->supports_byte_copy_initialization()) {
                        throw std::invalid_argument{"aggregate scalar state requires byte-copy initialization"};
                    }
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
                auto *address = static_cast<std::byte *>(storage) + layout.field_offsets[state++];

                if constexpr (state_field<Field>::runtime_typed) {
                    layout.generic_runtime_types[generic++]->construct_initial_value(address);
                } else {
                    using Value = typename state_field<Field>::storage_type;
                    std::construct_at(reinterpret_cast<Value *>(address));
                }
            }
        }(),
        ...);

    constexpr auto StateFieldCount = state_field_count<Definition>(std::index_sequence<Index...>{});
    auto dynamic_input = std::size_t{};
    (
        [&] {
            using Field = std::remove_cv_t<util::aggregate_member_type<Index, Definition>>;
            if constexpr (input_field<Field>::value && input_field<Field>::dynamic_binding) {
                using Storage = dynamic_input_storage_type<Field>;
                const auto offset = layout.field_offsets[StateFieldCount + dynamic_input++];
                std::construct_at(reinterpret_cast<Storage *>(static_cast<std::byte *>(storage) + offset));
            }
        }(),
        ...);
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

            if constexpr (state_field<Field>::value &&
                          (state_field<Field>::scalar_collection || state_field<Field>::runtime_typed)) {
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

                if constexpr (state_field<Field>::runtime_typed) {
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
    return (... || (state_field<std::remove_cv_t<util::aggregate_member_type<Index, Definition>>>::scalar_collection ||
                    state_field<std::remove_cv_t<util::aggregate_member_type<Index, Definition>>>::runtime_typed));
}

template <typename Definition, std::size_t... Index>
[[nodiscard]] consteval bool aggregate_has_dynamic_inputs(std::index_sequence<Index...>) noexcept {
    return (false || ... || [] {
        using Field = std::remove_cv_t<util::aggregate_member_type<Index, Definition>>;
        return input_field<Field>::value && input_field<Field>::dynamic_binding;
    }());
}

template <typename Definition, std::size_t Index>
void unbind_dynamic_input(const CompiledNode &node, void *runtime_storage, EvaluationContext &context) {
    using Field = std::remove_cv_t<util::aggregate_member_type<Index, Definition>>;
    if constexpr (input_field<Field>::value && input_field<Field>::dynamic_binding) {
        const auto offset = dynamic_input_offset<Definition, Index>(node);
        auto *state = static_cast<std::byte *>(Node{node, runtime_storage}.state());
        using Storage = dynamic_input_storage_type<Field>;
        auto &input = *reinterpret_cast<Storage *>(state + offset);
        input.unbind(context.runtime_graph(), node.state_offset() + offset);
    }
}

template <typename Definition, std::size_t... Index>
void unbind_dynamic_inputs(const CompiledNode &node, void *runtime_storage, EvaluationContext &context,
                                     std::index_sequence<Index...>) noexcept {
    if constexpr (sizeof...(Index) == 0) {
        (void)node;
        (void)runtime_storage;
        (void)context;
    } else {
        (unbind_dynamic_input<Definition, Index>(node, runtime_storage, context), ...);
    }
}

template <typename Definition>
void unbind_dynamic_inputs(const CompiledNode &node, void *runtime_storage,
                                     EvaluationContext &context) noexcept {
    unbind_dynamic_inputs<Definition>(node, runtime_storage, context,
                                                std::make_index_sequence<util::aggregate_member_count<Definition>>{});
}

template <typename Definition, auto Function, std::size_t... Index>
decltype(auto) invoke_aggregate_node(const CompiledNode &node, void *runtime_storage, EvaluationContext *context,
                                     std::index_sequence<Index...>, bool invalidation = false) {
    [[maybe_unused]] auto *storage = static_cast<std::byte *>(runtime_storage);
    auto output_state = aggregate_output_state<Definition, aggregate_node_has_output<Definition>()>{
        node, runtime_storage, context};
    auto definition = Definition{make_runtime_field<Definition, Index>(
        node, storage, context, output_state.output_state(), invalidation)...};

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

    unbind_dynamic_inputs<Definition>(node, runtime_storage, *context);
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

    unbind_dynamic_inputs<Definition>(node, runtime_storage, *context);
}

template <typename Definition>
void invalidate_aggregate_node(const CompiledNode &node, void *runtime_storage, EvaluationContext *context) {
    invoke_aggregate_node<Definition, &Definition::on_invalidation>(
        node, runtime_storage, context, std::make_index_sequence<util::aggregate_member_count<Definition>>{}, true);
}

template <typename Definition, std::size_t... Index>
void invalidate_aggregate_output(const CompiledNode &node, void *runtime_storage, EvaluationContext *context,
                                 std::index_sequence<Index...>) {
    auto *storage = static_cast<std::byte *>(runtime_storage);
    auto output_state = aggregate_output_state<Definition, aggregate_node_has_output<Definition>()>{
        node, runtime_storage, context};

    (
        [&] {
            using Field = std::remove_cv_t<util::aggregate_member_type<Index, Definition>>;
            if constexpr (output_field<Field>::value && requires(Field output) { output.invalidate(); }) {
                make_runtime_field<Definition, Index>(node, storage, context, output_state.output_state()).invalidate();
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
