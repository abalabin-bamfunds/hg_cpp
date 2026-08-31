#pragma once

#include <bg/evaluation_context.hpp>
#include <bg/graph/compiled_node.hpp>
#include <bg/node/input.hpp>
#include <bg/node/output.hpp>
#include <bg/node/state.hpp>
#include <bg/node/state_layout.hpp>
#include <bg/scalar/cpp_storage_representation.hpp>
#include <bg/time_series/ref/ref_storage_representation.hpp>
#include <bg/time_series/time_series_storage_representation.hpp>
#include <bg/time_series/tsd/tsd_storage_representation.hpp>
#include <bg/time_series/tsd/tsd_type.hpp>
#include <bg/time_series/tsl/tsl_storage_representation.hpp>
#include <bg/time_series/tsl/tsl_type.hpp>
#include <bg/time_series/tss/tss_storage_representation.hpp>
#include <bg/time_series/tss/tss_type.hpp>
#include <bg/type/scalar_type.hpp>
#include <bg/type/scalar_variable.hpp>
#include <bg/type/signature.hpp>
#include <bg/type/time_series_dict_type.hpp>
#include <bg/type/time_series_list_type.hpp>
#include <bg/type/time_series_reference_type.hpp>
#include <bg/type/time_series_set_type.hpp>
#include <bg/type/time_series_value_type.hpp>
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
};

template <typename TimeSeries, auto... Properties> struct input_field<In<TimeSeries, Properties...>> {
    using Input = In<TimeSeries, Properties...>;

    static constexpr bool value = true;
    static constexpr bool reflected_name = Input::name.empty();
    static constexpr InputActivity activity = Input::activity;
    static constexpr InputValidity validity = Input::validity;

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
[[nodiscard]] auto aggregate_output_time_series_type(std::index_sequence<Index...>) ->
    typename first_output_time_series<typename output_field<
        std::remove_cv_t<util::aggregate_member_type<Index, Definition>>>::time_series_type...>::type;

template <typename Definition>
using aggregate_output_time_series_t = decltype(aggregate_output_time_series_type<Definition>(
    std::make_index_sequence<util::aggregate_member_count<Definition>>{}));

template <typename T> inline constexpr bool generic_scalar_state = generic_scalar<T>;

template <typename T> struct scalar_field {
    static constexpr bool value =
        scalar_type_traits<T>::supports_implicit_argument && requires { cpp_scalar_type<T>(); };
    static constexpr bool optional = false;

    using value_type = T;
};

template <typename T> struct scalar_field<std::optional<T>> {
    static constexpr bool value = requires { cpp_scalar_type<T>(); };
    static constexpr bool optional = true;

    using value_type = T;
};

template <typename T> struct scalar_field<Scalar<T>> {
    static constexpr bool value = generic_scalar_state<T> || requires { cpp_scalar_type<T>(); };
    static constexpr bool optional = false;

    using value_type = T;
};

template <typename T> struct state_field {
    static constexpr bool value = false;
    static constexpr bool scalar_collection = false;
};

template <typename T> struct state_field<State<T>> {
    static constexpr bool value = true;
    static constexpr bool scalar_collection = scalar_type_traits<T>::is_collection;

    using value_type = T;
    using storage_type = std::conditional_t<scalar_collection, scalar_stored_t<T>, T>;
};

template <typename Definition, std::size_t... Index>
[[nodiscard]] std::vector<const ScalarType *> aggregate_state_types(const TypeMap &bindings,
                                                                    std::index_sequence<Index...>) {
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
                    result.push_back(
                        &static_cast<const ScalarType &>(scalar_type_for<Definition, Value>().resolve(bindings)));
                } else {
                    result.push_back(nullptr);
                }
            }
        }(),
        ...);

    return result;
}

template <typename T> struct generic_time_series : std::false_type {};

template <> struct generic_time_series<TS_T> : std::true_type {};

template <typename T, auto... Properties>
struct generic_time_series<TS<T, Properties...>> : std::bool_constant<generic_scalar<T>> {};

template <typename Target, auto... Properties>
struct generic_time_series<REF<Target, Properties...>> : generic_time_series<Target> {};

template <typename Item, std::size_t Size, auto... Properties>
struct generic_time_series<TSL<Item, Size, Properties...>> : generic_time_series<Item> {};

template <typename Item, auto... Properties>
struct generic_time_series<TSS<Item, Properties...>> : std::bool_constant<generic_scalar<Item>> {};

template <typename K, typename Item, auto... Properties>
struct generic_time_series<TSD<K, Item, Properties...>>
    : std::bool_constant<generic_scalar<K> || generic_time_series<Item>::value> {};

template <typename T> struct generic_input_field : std::false_type {};

template <typename TimeSeries, auto... Properties>
struct generic_input_field<In<TimeSeries, Properties...>> : generic_time_series<TimeSeries> {};

template <typename T> struct generic_output_field : std::false_type {};

template <typename TimeSeries> struct generic_output_field<Out<TimeSeries>> : generic_time_series<TimeSeries> {};

template <typename T> struct generic_scalar_input_field : std::false_type {};

template <typename T> struct generic_scalar_input_field<Scalar<T>> : std::bool_constant<generic_scalar_state<T>> {};

template <typename T> struct scalar_variable_view : std::false_type {};

template <scalar_variable Variable> struct scalar_variable_view<Scalar<Variable>> : std::true_type {};

template <typename T> struct scalar_view : std::false_type {};

template <typename T> struct scalar_view<Scalar<T>> : std::true_type {};

template <typename TimeSeries> struct supported_time_series : std::false_type {};

template <typename T>
concept has_cpp_scalar_type = requires { cpp_scalar_type<T>(); };

template <> struct supported_time_series<TS_T> : std::true_type {};

template <typename T, auto... Properties>
struct supported_time_series<TS<T, Properties...>>
    : std::bool_constant<generic_scalar<T> || (scalar_type_traits<T>::supports_time_series && has_cpp_scalar_type<T>)> {
};

template <typename Target, auto... Properties>
struct supported_time_series<REF<Target, Properties...>> : supported_time_series<Target> {};

template <typename Item, std::size_t Size, auto... Properties>
struct supported_time_series<TSL<Item, Size, Properties...>> : supported_time_series<Item> {};

template <typename Item, auto... Properties>
struct supported_time_series<TSS<Item, Properties...>>
    : std::bool_constant<generic_scalar<Item> || has_cpp_scalar_type<Item>> {};

template <typename K, typename Item, auto... Properties>
struct supported_time_series<TSD<K, Item, Properties...>>
    : std::bool_constant<(generic_scalar<K> || has_cpp_scalar_type<K>) && supported_time_series<Item>::value> {};

template <typename Field, bool = input_field<Field>::value> struct supported_input : std::false_type {};

template <typename Field>
struct supported_input<Field, true> : supported_time_series<typename input_field<Field>::time_series_type> {};

template <typename Field, bool = output_field<Field>::value> struct supported_output : std::false_type {};

template <typename Field>
struct supported_output<Field, true> : supported_time_series<typename output_field<Field>::time_series_type> {};

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

template <typename Definition> [[nodiscard]] const TypeVariable<TimeSeriesType> &aggregate_time_series_variable() {
    static const TypeVariable<TimeSeriesType> variable{util::symbol{"TS_T"}};

    return variable;
}

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
inline constexpr bool aggregate_node_preserves_output =
    requires { requires std::same_as<std::remove_cv_t<decltype(Definition::on_invalidation)>, PreserveOutput>; };

template <typename Definition> [[nodiscard]] consteval bool aggregate_uses_future_scheduling() {
    if constexpr (requires { Definition::uses_future_scheduling; }) {
        return Definition::uses_future_scheduling;
    }

    return false;
}

template <typename Definition, std::size_t... Index>
consteval bool input_fields_use_reflected_names(std::index_sequence<Index...>) {
    return ((!input_field<std::remove_cv_t<util::aggregate_member_type<Index, Definition>>>::value ||
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
    static constexpr bool value =
        supported_node_fields<Definition>(Index{}) && output_field_count<Definition>(Index{}) <= 1 &&
        input_fields_use_reflected_names<Definition>(Index{}) &&
        (requires(const Definition &definition) {
            { definition.evaluate() } -> std::same_as<void>;
        } || requires(const Definition &definition, EvaluationContext *context) {
            { definition.evaluate(context) } -> std::same_as<void>;
        });
};

template <typename Definition, std::size_t Index, bool Input, typename TimeSeries>
[[nodiscard]] const TimeSeriesType &time_series_type(GuaranteeParameter inherited_guarantee) {
    if constexpr (std::same_as<TimeSeries, TS_T>) {
        return aggregate_time_series_variable<Definition>();
    } else {
        const auto guarantee =
            TimeSeries::has_explicit_guarantee ? GuaranteeParameter{TimeSeries::guarantee} : inherited_guarantee;

        if constexpr (is_time_series_reference<TimeSeries>) {
            using Target = typename time_series_reference<TimeSeries>::target_type;
            const auto &target = time_series_type<Definition, Index, Input, Target>(TimeSeriesGuarantee::ordinary);

            if constexpr (Input || generic_time_series<Target>::value) {
                static const auto type = GenericTimeSeriesReferenceType{target, guarantee};
                return type;
            }

            if (!guarantee.is_resolved()) {
                static const auto type = GenericTimeSeriesReferenceType{target, guarantee};
                return type;
            }

            return time_series_reference_type(target, *guarantee.value());
        } else if constexpr (is_time_series_dict<TimeSeries>) {
            using DictionaryTraits = time_series_dict<TimeSeries>;
            using Key = typename DictionaryTraits::key_type;
            const auto &key = scalar_type_for<Definition, Key>();
            const auto &value =
                time_series_type<Definition, Index, Input, typename DictionaryTraits::item_type>(guarantee);
            static const auto key_set = GenericTimeSeriesSetType{key, guarantee};

            if constexpr (Input || generic_scalar<Key>) {
                static const auto type = GenericTimeSeriesDictionaryType{key, value, key_set, guarantee};
                return type;
            } else if (!value.is_resolved() || !guarantee.is_resolved()) {
                static const auto type = GenericTimeSeriesDictionaryType{key, value, key_set, guarantee};
                return type;
            }

            return time_series_dictionary_type(key, value, *guarantee.value());
        } else if constexpr (is_time_series_set<TimeSeries>) {
            using SetTraits = time_series_set<TimeSeries>;
            using Item = typename SetTraits::item_type;
            const auto &item = scalar_type_for<Definition, Item>();

            if constexpr (Input || generic_scalar<Item>) {
                static const auto type = GenericTimeSeriesSetType{item, guarantee};
                return type;
            } else if (!guarantee.is_resolved()) {
                static const auto type = GenericTimeSeriesSetType{item, guarantee};
                return type;
            }

            return time_series_set_type(item, *guarantee.value());
        } else if constexpr (is_time_series_list<TimeSeries>) {
            using ListTraits = time_series_list<TimeSeries>;
            static_assert(ListTraits::size != std::dynamic_extent,
                          "variable-size time series lists are not implemented");

            const auto &item = time_series_type<Definition, Index, Input, typename ListTraits::item_type>(guarantee);
            if constexpr (Input) {
                static const auto type = GenericTimeSeriesListType{item, ListTraits::size, guarantee};
                return type;
            } else if (!item.is_resolved() || !guarantee.is_resolved()) {
                static const auto type = GenericTimeSeriesListType{item, ListTraits::size, guarantee};
                return type;
            }

            return time_series_list_type(item, ListTraits::size, *guarantee.value());
        } else {
            using Value = typename time_series_value<TimeSeries>::type;
            const auto &value_type = scalar_type_for<Definition, Value>();
            if constexpr (Input || generic_scalar<Value>) {
                static const auto type = GenericTimeSeriesValueType{value_type, guarantee};
                return type;
            } else {
                if (!guarantee.is_resolved()) {
                    static const auto type = GenericTimeSeriesValueType{value_type, guarantee};
                    return type;
                }

                return time_series_value_type(value_type, *guarantee.value());
            }
        }
    }
}

template <typename Definition, std::size_t Index> [[nodiscard]] const TimeSeriesType &input_type() {
    using Field = std::remove_cv_t<util::aggregate_member_type<Index, Definition>>;
    using TimeSeries = typename input_field<Field>::time_series_type;

    if constexpr (std::same_as<TimeSeries, TS_T>) {
        return aggregate_time_series_variable<Definition>();
    } else if constexpr (TimeSeries::has_explicit_guarantee) {
        return time_series_type<Definition, Index, true, TimeSeries>(TimeSeries::guarantee);
    } else {
        static const auto guarantee = GuaranteeVariable{util::symbol{util::aggregate_member_names<Definition>[Index]}};
        return time_series_type<Definition, Index, true, TimeSeries>(guarantee);
    }
}

template <typename Definition, std::size_t Index> [[nodiscard]] const ScalarType &scalar_type() {
    using Field = std::remove_cv_t<util::aggregate_member_type<Index, Definition>>;
    using Value = typename scalar_field<Field>::value_type;

    return scalar_type_for<Definition, Value>();
}

template <typename Definition, std::size_t Index> [[nodiscard]] const TimeSeriesType &output_type() {
    using Field = std::remove_cv_t<util::aggregate_member_type<Index, Definition>>;
    using TimeSeries = typename output_field<Field>::time_series_type;

    if constexpr (std::same_as<TimeSeries, TS_T>) {
        return aggregate_time_series_variable<Definition>();
    } else {
        return time_series_type<Definition, Index, false, TimeSeries>(TimeSeriesGuarantee::ordinary);
    }
}

template <typename Definition, std::size_t... Index>
[[nodiscard]] const TimeSeriesStorageRepresentation *aggregate_output_storage_representation(
    const ResolvedSignature &signature,
    [[maybe_unused]] std::span<const ScalarStorageRepresentation *const> parameter_storage_representations,
    [[maybe_unused]] std::span<const TimeSeriesStorageRepresentation *const> time_series_input_storage_representations,
    std::index_sequence<Index...>) {
    const ScalarStorageRepresentation *value_representation = nullptr;
    const TimeSeriesStorageRepresentation *time_series_representation = nullptr;

    (
        [&] {
            using Field = std::remove_cv_t<util::aggregate_member_type<Index, Definition>>;

            if constexpr (output_field<Field>::value && !generic_output_field<Field>::value) {
                using TimeSeries = typename output_field<Field>::time_series_type;
                if constexpr (is_time_series_reference<TimeSeries> || is_time_series_list<TimeSeries> ||
                              is_time_series_set<TimeSeries> || is_time_series_dict<TimeSeries>) {
                    time_series_representation = &cpp_time_series_storage_representation<TimeSeries>();
                } else {
                    using Value = typename time_series_value<TimeSeries>::type;
                    value_representation = &cpp_scalar_storage_representation<Value>();
                }
            }
        }(),
        ...);

    if (signature.output == nullptr) {
        return nullptr;
    }
    if (time_series_representation != nullptr) {
        return time_series_representation;
    }
    if (value_representation != nullptr) {
        return &time_series_value_storage_representation(*signature.output, *value_representation);
    }

    if (const auto *reference = dynamic_cast<const TimeSeriesReferenceType *>(signature.output)) {
        auto input = std::size_t{};
        for (const auto &parameter : signature.parameters) {
            if (!parameter.type->is_time_series()) {
                continue;
            }

            const auto *representation = time_series_input_storage_representations[input++];
            const auto *target = representation->reference_target_representation();
            if (&representation->type() == &reference->target_type()) {
                return &time_series_reference_storage_representation(*reference, *representation);
            }
            if (target != nullptr && &target->type() == &reference->target_type()) {
                return &time_series_reference_storage_representation(*reference, *target);
            }
        }

        return nullptr;
    }

    if (dynamic_cast<const TimeSeriesValueType *>(signature.output) == nullptr) {
        auto time_series_input = std::size_t{};
        for (const auto &parameter : signature.parameters) {
            if (!parameter.type->is_time_series()) {
                continue;
            }
            if (parameter.type == signature.output) {
                return time_series_input_storage_representations[time_series_input];
            }
            ++time_series_input;
        }

        return nullptr;
    }

    const auto &output_type = static_cast<const TimeSeriesValueType &>(*signature.output);
    [[maybe_unused]] auto parameter_index = std::size_t{};
    (
        [&] {
            using Field = std::remove_cv_t<util::aggregate_member_type<Index, Definition>>;

            if constexpr (input_field<Field>::value) {
                const auto &input_type =
                    static_cast<const TimeSeriesValueType &>(*signature.parameters[parameter_index].type);
                if (&input_type.value_type() == &output_type.value_type()) {
                    value_representation = parameter_storage_representations[parameter_index];
                }
                ++parameter_index;
            } else if constexpr (scalar_field<Field>::value) {
                using Value = typename scalar_field<Field>::value_type;
                const auto *representation = parameter_storage_representations[parameter_index];

                if (representation != nullptr && &representation->type() == &output_type.value_type()) {
                    value_representation = representation;
                } else if constexpr (scalar_variable_list<Value>) {
                    if (representation != nullptr) {
                        const auto &list = static_cast<const ScalarListStorageRepresentation &>(*representation);
                        if (&list.item_storage_representation().type() == &output_type.value_type()) {
                            value_representation = &list.item_storage_representation();
                        }
                    }
                }
                ++parameter_index;
            }
        }(),
        ...);

    return value_representation == nullptr
               ? nullptr
               : &time_series_value_storage_representation(*signature.output, *value_representation);
}

template <typename Definition, std::size_t... Index>
[[nodiscard]] constexpr bool aggregate_uses_generic_time_series_input_view([[maybe_unused]] std::size_t parameter_index,
                                                                           std::index_sequence<Index...>) noexcept {
    [[maybe_unused]] auto current_parameter = std::size_t{};
    auto result = false;

    (
        [&] {
            using Field = std::remove_cv_t<util::aggregate_member_type<Index, Definition>>;

            if constexpr (input_field<Field>::value || scalar_field<Field>::value) {
                if (current_parameter == parameter_index) {
                    result = generic_input_field<Field>::value;
                }
                ++current_parameter;
            }
        }(),
        ...);

    return result;
}

template <typename Definition, std::size_t... Index>
[[nodiscard]] constexpr bool aggregate_uses_generic_scalar_input_view([[maybe_unused]] std::size_t parameter_index,
                                                                      std::index_sequence<Index...>) noexcept {
    [[maybe_unused]] auto current_parameter = std::size_t{};
    auto result = false;

    (
        [&] {
            using Field = std::remove_cv_t<util::aggregate_member_type<Index, Definition>>;

            if constexpr (input_field<Field>::value || scalar_field<Field>::value) {
                if (current_parameter == parameter_index) {
                    result = generic_scalar_input_field<Field>::value;
                }
                ++current_parameter;
            }
        }(),
        ...);

    return result;
}

template <typename Definition, std::size_t... Index>
[[nodiscard]] consteval std::size_t aggregate_generic_input_count(std::index_sequence<Index...>) {
    return (std::size_t{} + ... +
            static_cast<std::size_t>(
                generic_input_field<std::remove_cv_t<util::aggregate_member_type<Index, Definition>>>::value));
}

template <typename Definition, std::size_t Target, std::size_t... Index>
[[nodiscard]] consteval std::size_t aggregate_generic_input_index(std::index_sequence<Index...>) {
    return (std::size_t{} + ... +
            static_cast<std::size_t>(
                Index < Target &&
                generic_input_field<std::remove_cv_t<util::aggregate_member_type<Index, Definition>>>::value));
}

template <typename Definition, std::size_t... Index>
[[nodiscard]] consteval std::size_t aggregate_generic_scalar_input_count(std::index_sequence<Index...>) {
    return (std::size_t{} + ... +
            static_cast<std::size_t>(
                generic_scalar_input_field<std::remove_cv_t<util::aggregate_member_type<Index, Definition>>>::value));
}

template <typename Definition, std::size_t Target, std::size_t... Index>
[[nodiscard]] consteval std::size_t aggregate_generic_scalar_input_index(std::index_sequence<Index...>) {
    return (std::size_t{} + ... +
            static_cast<std::size_t>(
                Index < Target &&
                generic_scalar_input_field<std::remove_cv_t<util::aggregate_member_type<Index, Definition>>>::value));
}

template <typename Definition, std::size_t... Index>
[[nodiscard]] consteval bool aggregate_uses_generic_output_view(std::index_sequence<Index...>) {
    return (false || ... ||
            generic_output_field<std::remove_cv_t<util::aggregate_member_type<Index, Definition>>>::value);
}

template <typename Definition, std::size_t... Index>
[[nodiscard]] consteval bool aggregate_output_requires_view_invalidation(std::index_sequence<Index...>) {
    return (false || ... || [] {
        using Field = std::remove_cv_t<util::aggregate_member_type<Index, Definition>>;
        if constexpr (!output_field<Field>::value) {
            return false;
        } else {
            using TimeSeries = typename output_field<Field>::time_series_type;
            return std::same_as<TimeSeries, TS_T> || is_time_series_reference<TimeSeries> ||
                   is_time_series_list<TimeSeries>;
        }
    }());
}

template <typename Definition, std::size_t Index>
void add_signature_field(std::vector<Parameter> &parameters, const TimeSeriesType *&output) {
    using Field = std::remove_cv_t<util::aggregate_member_type<Index, Definition>>;

    if constexpr (input_field<Field>::value) {
        parameters.push_back({util::symbol{util::aggregate_member_names<Definition>[Index]},
                              &input_type<Definition, Index>(), input_field<Field>::activity,
                              input_field<Field>::validity});
    } else if constexpr (scalar_field<Field>::value) {
        parameters.push_back({util::symbol{util::aggregate_member_names<Definition>[Index]},
                              &scalar_type<Definition, Index>(), active, required, scalar_field<Field>::optional});
    } else if constexpr (output_field<Field>::value) {
        output = &output_type<Definition, Index>();
    }
}

template <typename Definition, std::size_t... Index>
[[nodiscard]] Signature make_signature(std::index_sequence<Index...>) {
    std::vector<Parameter> parameters;
    parameters.reserve(sizeof...(Index));
    const TimeSeriesType *output = nullptr;

    (add_signature_field<Definition, Index>(parameters, output), ...);

    if (output == nullptr) {
        return {util::symbol{util::unqualified_type_name<Definition>()}, std::move(parameters)};
    }

    return {util::symbol{util::unqualified_type_name<Definition>()}, std::move(parameters), *output};
}

template <typename Definition> [[nodiscard]] const Signature &aggregate_node_signature() {
    static const Signature signature =
        make_signature<Definition>(std::make_index_sequence<util::aggregate_member_count<Definition>>{});

    return signature;
}

template <typename Definition, std::size_t... Index>
consteval std::size_t parameter_index(std::index_sequence<Index...>) {
    return (std::size_t{} + ... +
            static_cast<std::size_t>(
                input_field<std::remove_cv_t<util::aggregate_member_type<Index, Definition>>>::value ||
                scalar_field<std::remove_cv_t<util::aggregate_member_type<Index, Definition>>>::value));
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
[[nodiscard]] consteval std::size_t state_index(std::index_sequence<Index...>) {
    return (
        std::size_t{} + ... +
        static_cast<std::size_t>(Index < Target &&
                                 state_field<std::remove_cv_t<util::aggregate_member_type<Index, Definition>>>::value));
}

template <typename Definition, std::size_t Target, std::size_t... Index>
[[nodiscard]] consteval std::size_t generic_state_index(std::index_sequence<Index...>) {
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
[[nodiscard]] consteval std::size_t aggregate_state_offset(std::index_sequence<Index...>) {
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

template <typename Definition, std::size_t Index>
[[nodiscard]] auto make_runtime_field(const CompiledNode &node, std::byte *runtime_storage,
                                      EvaluationContext *context) {
    using Field = std::remove_cv_t<util::aggregate_member_type<Index, Definition>>;

    if constexpr (input_field<Field>::value) {
        constexpr auto ParameterIndex = parameter_index<Definition>(std::make_index_sequence<Index>{});
        const auto &binding = node.parameters()[ParameterIndex];

        const auto &last_updated = binding.last_updated(runtime_storage);
        const auto *value = binding.value(runtime_storage);

        if constexpr (generic_input_field<Field>::value) {
            constexpr auto GenericInputIndex = aggregate_generic_input_index<Definition, Index>(
                std::make_index_sequence<util::aggregate_member_count<Definition>>{});

            if constexpr (std::same_as<typename input_field<Field>::time_series_type, TS_T>) {
                return Field{last_updated, value, node.time_series_input_view_prototype(GenericInputIndex)};
            } else if constexpr (is_time_series_reference<typename input_field<Field>::time_series_type>) {
                return Field{last_updated, value,
                             static_cast<const TimeSeriesReferenceInputView &>(
                                 node.time_series_input_view_prototype(GenericInputIndex))};
            } else if constexpr (is_time_series_dict<typename input_field<Field>::time_series_type>) {
                return Field{last_updated, value,
                             static_cast<const TimeSeriesDictionaryInputView &>(
                                 node.time_series_input_view_prototype(GenericInputIndex))};
            } else if constexpr (is_time_series_set<typename input_field<Field>::time_series_type>) {
                return Field{last_updated, value,
                             static_cast<const TimeSeriesSetInputView &>(
                                 node.time_series_input_view_prototype(GenericInputIndex))};
            } else if constexpr (is_time_series_list<typename input_field<Field>::time_series_type>) {
                return Field{last_updated, value,
                             static_cast<const TimeSeriesListInputView &>(
                                 node.time_series_input_view_prototype(GenericInputIndex))};
            } else {
                return Field{last_updated, value, node.time_series_value_input_view_prototype(GenericInputIndex)};
            }
        } else {
            return Field{last_updated, static_cast<const typename Field::storage_type *>(value)};
        }
    } else if constexpr (scalar_field<Field>::value) {
        constexpr auto ParameterIndex = parameter_index<Definition>(std::make_index_sequence<Index>{});
        const auto *data = node.parameters()[ParameterIndex].scalar_data();
        using Value = typename scalar_field<Field>::value_type;

        if constexpr (generic_scalar_input_field<Field>::value) {
            constexpr auto GenericTimeSeriesInputCount = aggregate_generic_input_count<Definition>(
                std::make_index_sequence<util::aggregate_member_count<Definition>>{});
            constexpr auto GenericScalarInputIndex = aggregate_generic_scalar_input_index<Definition, Index>(
                std::make_index_sequence<util::aggregate_member_count<Definition>>{});

            const auto &prototype =
                node.scalar_input_view_prototype(GenericTimeSeriesInputCount, GenericScalarInputIndex);

            if constexpr (scalar_variable_view<Field>::value) {
                auto view = ErasedScalarView::copy(static_cast<const ScalarValueInputView &>(prototype));
                view->bind(data);

                return Field::from(view);
            } else if constexpr (is_list_v<Value>) {
                return Field{static_cast<const ScalarListInputView &>(prototype), data};
            } else if constexpr (is_set_v<Value>) {
                return Field{static_cast<const ScalarSetInputView &>(prototype), data};
            } else {
                return Field{static_cast<const ScalarDictInputView &>(prototype), data};
            }
        } else {
            using Traits = scalar_storage_traits<Value>;

            if constexpr (scalar_view<Field>::value) {
                return Field::from(*static_cast<const typename Traits::stored_type *>(data));
            } else if constexpr (scalar_field<Field>::optional) {
                return data == nullptr ? Field{}
                                       : Field{Traits::read(*static_cast<const typename Traits::stored_type *>(data))};
            } else {
                return Traits::read(*static_cast<const typename Traits::stored_type *>(data));
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
        auto *value = runtime_storage + node.state_offset() + offset;

        if constexpr (state_field<Field>::scalar_collection && generic_scalar_state<Declared>) {
            constexpr auto GenericStateIndex =
                generic_state_index<Definition, Index>(std::make_index_sequence<MemberCount>{});
            const auto &representation = node.generic_state_storage_representation(GenericStateIndex);
            return Field{representation, value};
        } else {
            using Value = typename state_field<Field>::storage_type;
            return Field{*reinterpret_cast<Value *>(value)};
        }
    } else {
        auto &last_updated = node.output_last_updated(runtime_storage);

        if constexpr (generic_output_field<Field>::value) {
            constexpr auto GenericTimeSeriesInputCount = aggregate_generic_input_count<Definition>(
                std::make_index_sequence<util::aggregate_member_count<Definition>>{});
            constexpr auto GenericScalarInputCount = aggregate_generic_scalar_input_count<Definition>(
                std::make_index_sequence<util::aggregate_member_count<Definition>>{});
            auto *value = static_cast<void *>(runtime_storage + node.output_value_offset());

            return output_factory::make<typename Field::time_series_type>(
                last_updated, value, node.output_view_prototype(GenericTimeSeriesInputCount, GenericScalarInputCount),
                *context);
        } else {
            auto &value =
                *reinterpret_cast<typename Field::storage_type *>(runtime_storage + node.output_value_offset());

            return output_factory::make<typename Field::time_series_type>(last_updated, value, *context);
        }
    }
}

template <typename Definition, std::size_t... Index>
[[nodiscard]] NodeStateLayout
aggregate_state_layout(const ResolvedSignature &,
                       [[maybe_unused]] std::span<const ScalarStorageRepresentation *const> parameter_representations,
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
                    const auto *representation =
                        &resolve_scalar_storage_representation(*type, parameter_representations);
                    size = representation->size();
                    alignment = representation->alignment();
                    result.generic_storage_representations.push_back(representation);
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
                    layout.generic_storage_representations[generic++]->construct_initial_value(address);
                } else {
                    using Value = typename state_field<Field>::storage_type;
                    std::construct_at(reinterpret_cast<Value *>(address));
                }
            }
        }(),
        ...);
}

template <typename Definition, std::size_t... Index>
void destroy_aggregate_state(const CompiledNode &node, void *runtime_storage, EvaluationContext *,
                             std::index_sequence<Index...>) noexcept {
    [[maybe_unused]] auto *storage = static_cast<std::byte *>(runtime_storage);
    auto &memory = current_scalar_memory_resource();
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
                auto *value = storage + node.state_offset() + offset;

                if constexpr (generic_scalar_state<Declared>) {
                    constexpr auto GenericStateIndex =
                        generic_state_index<Definition, Index>(std::make_index_sequence<MemberCount>{});
                    node.generic_state_storage_representation(GenericStateIndex).destroy(value, memory);
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

template <typename Definition, auto Function, std::size_t... Index>
void invoke_aggregate_node(const CompiledNode &node, void *runtime_storage, EvaluationContext *context,
                           std::index_sequence<Index...>) {
    [[maybe_unused]] auto *storage = static_cast<std::byte *>(runtime_storage);
    auto definition = Definition{make_runtime_field<Definition, Index>(node, storage, context)...};

    if constexpr (std::invocable<decltype(Function), const Definition &, EvaluationContext *>) {
        std::invoke(Function, std::as_const(definition), context);
    } else {
        std::invoke(Function, std::as_const(definition));
    }
}

template <typename Definition, auto Function>
void invoke_aggregate_node(const CompiledNode &node, void *runtime_storage, EvaluationContext *context) {
    invoke_aggregate_node<Definition, Function>(node, runtime_storage, context,
                                                std::make_index_sequence<util::aggregate_member_count<Definition>>{});
}

template <typename Definition>
void evaluate_aggregate_node(const CompiledNode &node, void *runtime_storage, EvaluationContext *context) {
    invoke_aggregate_node<Definition, &Definition::evaluate>(node, runtime_storage, context);
}

template <typename Definition>
void start_aggregate_node(const CompiledNode &node, void *runtime_storage, EvaluationContext *context) {
    invoke_aggregate_node<Definition, &Definition::start>(node, runtime_storage, context);
}

template <typename Definition>
void stop_aggregate_node(const CompiledNode &node, void *runtime_storage, EvaluationContext *context) {
    invoke_aggregate_node<Definition, &Definition::stop>(node, runtime_storage, context);
}

template <typename Definition>
void invalidate_aggregate_node(const CompiledNode &node, void *runtime_storage, EvaluationContext *context) {
    invoke_aggregate_node<Definition, &Definition::on_invalidation>(node, runtime_storage, context);
}

template <typename Definition, std::size_t... Index>
void invalidate_aggregate_output(const CompiledNode &node, void *runtime_storage, EvaluationContext *context,
                                 std::index_sequence<Index...>) {
    auto *storage = static_cast<std::byte *>(runtime_storage);
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
