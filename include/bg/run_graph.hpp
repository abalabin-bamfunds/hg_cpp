#pragma once

#include <bg/graph_run_input.hpp>
#include <bg/graph_run_options.hpp>
#include <bg/graph_run_result.hpp>
#include <bg/details/graph_run.hpp>
#include <bg/graph.hpp>
#include <bg/literal/sentinel.hpp>
#include <bg/scalar/list/scalar_type.hpp>
#include <bg/scalar/list/runtime_type.hpp>
#include <bg/scalar/literal_operations.hpp>
#include <bg/scalar/dict/scalar_type.hpp>
#include <bg/scalar/set/scalar_type.hpp>
#include <bg/scalar/struct/scalar_type.hpp>
#include <bg/scalar/tuple/scalar_type.hpp>
#include <bg/scalar/bundle/scalar_type.hpp>
#include <bg/operators.hpp>
#include <bg/type/time_series_bundle_type.hpp>
#include <bg/type/time_series_dict_type.hpp>
#include <bg/type/time_series_list_type.hpp>
#include <bg/type/time_series_reference_type.hpp>
#include <bg/type/time_series_set_type.hpp>
#include <bg/type/time_series_value_type.hpp>
#include <bg/type/type_variable.hpp>
#include <bg/time_series/literal_operations.hpp>

#include <algorithm>
#include <concepts>
#include <deque>
#include <functional>
#include <initializer_list>
#include <optional>
#include <ranges>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace bg {

namespace details {

struct RawGraphRunLiteralInput {
    std::span<const literal::TimeSeriesLiteral> values;
};

struct GraphRunLiteralStorage {
    std::deque<std::vector<literal::TimeSeriesLiteral>> sequences;
    std::deque<ScalarValue<>> values;
};

template <typename T> struct graph_run_ticks : std::false_type {};

template <std::ranges::view Range> struct graph_run_ticks<GraphRunTicks<Range>> : std::true_type {};

template <typename T> inline constexpr bool graph_run_ticks_v = graph_run_ticks<std::remove_cvref_t<T>>::value;

template <typename Function> auto run_graph_callback(Function &&function, const GraphRunOptions &options) {
    using FunctionInfo = graph_function<Function>;
    using OutputScalar = typename FunctionInfo::output_scalar;

    auto captured = CapturedValues{
        options.start_time.value_or(MIN_DT + EvaluationTime::duration{1})};
    captured.set_cycle_count(1);

    auto wiring = GraphWiring{};
    {
        auto context = WiringContext{wiring};
        if constexpr (std::same_as<typename FunctionInfo::result, void>) {
            std::invoke(std::forward<Function>(function));
        } else {
            const auto output = std::invoke(std::forward<Function>(function));
            if (options.capture_output) {
                capture_graph_output(output, captured);
            }
        }
    }

    return GraphRunResult<OutputScalar>{finish_graph_run(wiring, captured, options)};
}

template <typename T> struct graph_run_optional : std::false_type {};

template <typename T> struct graph_run_optional<std::optional<T>> : std::true_type {
    using value_type = T;
};

template <typename T> inline constexpr bool graph_run_optional_v =
    graph_run_optional<std::remove_cvref_t<T>>::value;

template <typename T> inline constexpr bool graph_run_atomic_range_v = [] {
    if constexpr (!scalar_source<T> || !std::ranges::input_range<std::remove_cvref_t<T>>) {
        return false;
    } else {
        using Scalar = canonical_scalar_t<std::remove_cvref_t<T>>;
        return std::same_as<Scalar, string> || std::same_as<Scalar, bytes>;
    }
}();

template <typename T> inline constexpr bool graph_run_character_pointer_v = [] {
    using Input = std::remove_cvref_t<T>;
    if constexpr (!std::is_pointer_v<Input>) {
        return false;
    } else {
        return std::same_as<std::remove_cv_t<std::remove_pointer_t<Input>>, char>;
    }
}();

template <typename T> inline constexpr bool graph_run_range_input_v =
    std::ranges::input_range<std::remove_cvref_t<T>> && !graph_run_atomic_range_v<T> &&
    !requires(const std::remove_cvref_t<T> &value) { details::scalar_data(value); };

template <typename T> inline constexpr bool graph_run_literal_range_v = [] {
    if constexpr (std::ranges::input_range<std::remove_cvref_t<T>>) {
        return std::same_as<std::ranges::range_value_t<std::remove_cvref_t<T>>, literal::TimeSeriesLiteral>;
    } else {
        return false;
    }
}();

template <typename T> inline constexpr bool graph_run_pair_like_v = [] {
    if constexpr (requires { std::tuple_size<std::remove_cvref_t<T>>::value; }) {
        return std::tuple_size_v<std::remove_cvref_t<T>> == 2;
    } else {
        return false;
    }
}();

template <typename T> struct graph_run_initializer_list : std::false_type {};

template <typename T> struct graph_run_initializer_list<std::initializer_list<T>> : std::true_type {
    using value_type = T;
};

template <typename T> inline constexpr bool graph_run_initializer_list_v =
    graph_run_initializer_list<std::remove_cvref_t<T>>::value;

template <typename T> struct graph_run_input_scalar {
    using type = canonical_scalar_type_t<T>;
};

template <typename T> struct graph_run_input_scalar<std::optional<T>> : graph_run_input_scalar<T> {};

template <std::ranges::input_range Range>
    requires(!graph_run_atomic_range_v<Range>)
struct graph_run_input_scalar<Range> {
    using type = List<typename graph_run_input_scalar<std::ranges::range_value_t<Range>>::type>;
};

template <typename T>
using graph_run_input_scalar_t = typename graph_run_input_scalar<std::remove_cvref_t<T>>::type;

template <typename T> inline constexpr bool graph_run_scalar_input_v = [] {
    using Input = std::remove_cvref_t<T>;
    if constexpr (graph_run_optional_v<Input>) {
        return graph_run_scalar_input_v<typename graph_run_optional<Input>::value_type>;
    } else if constexpr (graph_run_range_input_v<Input>) {
        return graph_run_scalar_input_v<std::ranges::range_value_t<Input>>;
    } else {
        return scalar_source<Input> || scalar_type_mapped<Input> ||
               requires(const Input &value) { details::scalar_data(value); };
    }
}();

template <typename Value> ScalarValue<> graph_run_scalar_value(Value &&value) {
    if constexpr (requires { details::scalar_data(value); }) {
        return ScalarValue<>{Scalar<>::from(details::scalar_data(value))};
    } else if constexpr (scalar_source<Value>) {
        return ScalarValue<>{value};
    } else if constexpr (scalar_type_mapped<std::remove_cvref_t<Value>>) {
        using Scalar = std::remove_cvref_t<Value>;
        return ScalarValue<>{ScalarValue<Scalar>{std::forward<Value>(value)}};
    } else {
        static_assert(sizeof(Value) == 0, "run_graph input must be a scalar");
    }
}

template <typename Value> ScalarValue<> graph_run_owned_scalar_value(Value &&value);

template <std::ranges::input_range Range> ScalarValue<> graph_run_range_scalar_value(Range &&range) {
    using RangeType = std::remove_reference_t<Range>;
    using Item = std::ranges::range_value_t<RangeType>;
    using Scalar = graph_run_input_scalar_t<Range>;

    auto construct = [](void *opaque, const ScalarRuntimeType &runtime_type, void *destination,
                        allocator &memory) {
        const auto &list_type = static_cast<const ScalarListRuntimeType &>(runtime_type);
        list_type.construct_runtime_value(destination, memory);
        auto rollback = util::scope_exit{[&]() noexcept { list_type.destroy(destination, memory); }};
        auto &values = *static_cast<RangeType *>(opaque);
        for (auto &&value : values) {
            if constexpr (graph_run_optional_v<Item>) {
                if (!value) {
                    throw std::invalid_argument{"run_graph list values cannot be absent"};
                }
                auto item = graph_run_owned_scalar_value(*value);
                list_type.append(destination, details::scalar_data(item), memory);
            } else {
                auto item = [&] {
                    if constexpr (std::same_as<std::remove_cvref_t<decltype(value)>, Item>) {
                        return graph_run_owned_scalar_value(value);
                    } else {
                        return graph_run_owned_scalar_value(Item(value));
                    }
                }();
                list_type.append(destination, details::scalar_data(item), memory);
            }
        }
        rollback.release();
    };

    return ScalarValue<>::from_construction(cpp_scalar_runtime_type<Scalar>(),
                                            ScalarConstruction{
                                                const_cast<void *>(static_cast<const void *>(std::addressof(range))),
                                                construct});
}

template <typename Value> ScalarValue<> graph_run_owned_scalar_value(Value &&value) {
    if constexpr (graph_run_range_input_v<Value>) {
        return graph_run_range_scalar_value(std::forward<Value>(value));
    } else {
        return graph_run_scalar_value(std::forward<Value>(value));
    }
}

inline bool graph_run_explicit_input(const GraphRunInput &) noexcept { return true; }

inline bool graph_run_explicit_input(const GraphRunLiteralInput &input) noexcept {
    return input.runtime_type != nullptr;
}

inline bool graph_run_explicit_input(const RawGraphRunLiteralInput &) noexcept { return false; }

template <typename T> bool graph_run_explicit_input(const T &) noexcept { return false; }

template <typename Value>
literal::TimeSeriesLiteral graph_run_literal_from_value(Value &&value, GraphRunLiteralStorage &storage) {
    if constexpr (graph_run_range_input_v<Value>) {
        using Item = std::ranges::range_value_t<std::remove_cvref_t<Value>>;
        auto &elements = storage.sequences.emplace_back();
        for (auto &&element : std::forward<Value>(value)) {
            if constexpr (std::same_as<std::remove_cvref_t<decltype(element)>, Item>) {
                elements.emplace_back(graph_run_literal_from_value(std::forward<decltype(element)>(element), storage));
            } else {
                elements.emplace_back(graph_run_literal_from_value(Item(element), storage));
            }
        }
        return literal::TimeSeriesLiteral(std::span<const literal::TimeSeriesLiteral>{elements});
    } else if constexpr (graph_run_pair_like_v<Value>) {
        auto &elements = storage.sequences.emplace_back();
        std::apply([&](const auto &...item) { (elements.emplace_back(graph_run_literal_from_value(item, storage)), ...); },
                   value);
        return literal::TimeSeriesLiteral(std::span<const literal::TimeSeriesLiteral>{elements});
    } else if constexpr (std::same_as<std::remove_cvref_t<Value>, literal::TimeSeriesLiteral>) {
        return value;
    } else {
        auto scalar = graph_run_owned_scalar_value(std::forward<Value>(value));
        storage.values.emplace_back(std::move(scalar));
        return literal::TimeSeriesLiteral(storage.values.back());
    }
}

inline const TimeSeriesType *graph_run_parameter_time_series(const Signature &signature, std::size_t index) {
    const auto parameter_index = index < signature.parameter_count()
                                     ? index
                                     : signature.first_variadic_parameter().value_or(index);
    const auto *parameter_type = signature.parameter(parameter_index).type;
    if (const auto *type = type_cast<TimeSeriesType>(parameter_type)) {
        return type;
    }

    const auto *variable = dynamic_cast<const TypeVariableType<Type> *>(parameter_type);
    if (variable == nullptr) {
        return nullptr;
    }

    const TimeSeriesType *time_series_type{};
    for (const auto *alternative : variable->alternatives()) {
        const auto *candidate = type_cast<TimeSeriesType>(alternative);
        if (candidate == nullptr) {
            continue;
        }
        if (time_series_type != nullptr) {
            return nullptr;
        }
        time_series_type = candidate;
    }
    return time_series_type;
}

inline const TimeSeriesRuntimeType &graph_run_runtime_type(const Signature &signature, std::size_t index);

template <typename Input>
void preflight_graph_run_input(const Signature &signature, std::size_t index, const Input &input) {
    const auto parameter_index = index < signature.parameter_count()
                                     ? index
                                     : signature.first_variadic_parameter().value_or(index);
    if (parameter_index >= signature.parameter_count()) {
        throw std::invalid_argument{"run_graph input count exceeds the graph signature"};
    }
    const auto *time_series_type = graph_run_parameter_time_series(signature, parameter_index);
    if (time_series_type != nullptr && !graph_run_explicit_input(input)) {
        (void)graph_run_runtime_type(signature, parameter_index);
    }
}

template <typename... Inputs, std::size_t... Index>
void preflight_graph_run_inputs(const Signature &signature, std::index_sequence<Index...>,
                                const Inputs &...inputs) {
    (preflight_graph_run_input(signature, Index, inputs), ...);
}

template <typename T> inline constexpr bool graph_run_descriptor_range_v = [] {
    if constexpr (!std::ranges::contiguous_range<std::remove_cvref_t<T>>) {
        return false;
    } else {
        using Input = std::remove_cv_t<std::ranges::range_value_t<std::remove_cvref_t<T>>>;
        return std::same_as<Input, GraphRunInput> || std::same_as<Input, GraphRunLiteralInput>;
    }
}();

inline const TimeSeriesRuntimeType &graph_run_runtime_type(const TimeSeriesType &type) {
    if (type.is_resolved()) {
        return resolve_time_series_runtime_type(type);
    }

    auto type_map = TypeMap{};
    if (const auto *guarantee = type.guarantee().variable()) {
        type_map.set(*guarantee, guarantee->minimum().value_or(TimeSeriesGuarantee::ordinary));
    }
    try {
        const auto &resolved_type = type_cast<TimeSeriesType>(type.resolve(type_map));
        return resolve_time_series_runtime_type(resolved_type);
    } catch (const TypeResolutionError &error) {
        if (error.code() != TypeResolutionErrorCode::unmapped_variable) {
            throw;
        }
        throw std::invalid_argument{"a generic run_graph input requires an explicit time-series type"};
    }
}

inline const TimeSeriesRuntimeType &graph_run_runtime_type(const Signature &signature, std::size_t index) {
    const auto *type = graph_run_parameter_time_series(signature, index);
    if (type == nullptr) {
        throw std::invalid_argument{"run_graph needs a concrete time-series input type"};
    }
    return graph_run_runtime_type(*type);
}

inline const ScalarType *graph_run_value_type(const TimeSeriesType &target) {
    const auto *value_type = type_cast<TimeSeriesValueType>(&target.target_type());
    return value_type == nullptr ? nullptr : &value_type->value_type();
}

template <typename Value>
const TimeSeriesType *graph_run_parameter_time_series_for_value(const Signature &signature, std::size_t index) {
    const auto parameter_index = index < signature.parameter_count()
                                     ? index
                                     : signature.first_variadic_parameter().value_or(index);
    const auto *parameter_type = signature.parameter(parameter_index).type;
    if (const auto *time_series_type = type_cast<TimeSeriesType>(parameter_type)) {
        return time_series_type;
    }

    const auto *variable = dynamic_cast<const TypeVariableType<Type> *>(parameter_type);
    if (variable == nullptr) {
        return nullptr;
    }

    for (const auto *alternative : variable->alternatives()) {
        const auto *time_series_type = type_cast<TimeSeriesType>(alternative);
        const auto *value_type = time_series_type == nullptr ? nullptr : graph_run_value_type(*time_series_type);
        if (value_type != nullptr && value_type->accepts(cpp_scalar_type<Value>())) {
            return time_series_type;
        }
    }
    return nullptr;
}

inline const ScalarRuntimeType &graph_run_value_runtime_type(const TimeSeriesType &target) {
    const auto *value_type = graph_run_value_type(target);
    if (value_type == nullptr) {
        throw std::invalid_argument{"run_graph needs a scalar time-series input type"};
    }
    return resolve_scalar_runtime_type(*value_type, {});
}

inline bool graph_run_literal_matches_scalar(const literal::TimeSeriesLiteral &value, const ScalarType &type);

inline std::optional<std::span<const literal::TimeSeriesLiteral>>
graph_run_literal_sequence(const literal::TimeSeriesLiteral &value) {
    return value.visit(
        [](const literal::ScalarLiteralItem &) -> std::optional<std::span<const literal::TimeSeriesLiteral>> {
            return std::nullopt;
        },
        [](std::span<const literal::TimeSeriesLiteral> elements)
            -> std::optional<std::span<const literal::TimeSeriesLiteral>> { return elements; });
}

inline std::optional<std::string_view> graph_run_literal_name(const literal::TimeSeriesLiteral &value) {
    return value.visit(
        [](const literal::ScalarLiteralItem &item) -> std::optional<std::string_view> {
            return item.visit_scalar(
                [](const auto *source) -> std::optional<std::string_view> {
                    using Scalar = std::remove_cvref_t<decltype(*source)>;
                    if constexpr (std::same_as<typename Scalar::scalar_type, string>) {
                        return source->value();
                    } else if constexpr (std::same_as<typename Scalar::scalar_type, symbol>) {
                        return source->value().text();
                    } else {
                        return std::nullopt;
                    }
                });
        },
        [](std::span<const literal::TimeSeriesLiteral>) -> std::optional<std::string_view> { return std::nullopt; });
}

inline std::optional<std::span<const literal::TimeSeriesLiteral>>
graph_run_literal_pair(const literal::TimeSeriesLiteral &value) {
    const auto elements = graph_run_literal_sequence(value);
    return elements && elements->size() == 2 ? elements : std::nullopt;
}

inline bool graph_run_named_scalar_fields_match(std::span<const literal::TimeSeriesLiteral> entries,
                                                const auto &field_count, const auto &field_name,
                                                const auto &field_type, const auto &field_optional) {
    auto supplied = std::vector<bool>(field_count(), false);
    for (const auto &entry : entries) {
        const auto pair = graph_run_literal_pair(entry);
        if (!pair) {
            return false;
        }
        const auto name = graph_run_literal_name((*pair)[0]);
        if (!name) {
            return false;
        }
        auto found = false;
        for (auto index = std::size_t{}; index < field_count(); ++index) {
            if (*name != field_name(index)) {
                continue;
            }
            if (supplied[index] || !graph_run_literal_matches_scalar((*pair)[1], field_type(index))) {
                return false;
            }
            supplied[index] = true;
            found = true;
            break;
        }
        if (!found) {
            return false;
        }
    }
    for (auto index = std::size_t{}; index < field_count(); ++index) {
        if (!supplied[index] && !field_optional(index)) {
            return false;
        }
    }
    return true;
}

inline bool graph_run_literal_matches_scalar(const literal::TimeSeriesLiteral &value, const ScalarType &type) {
    const auto elements = graph_run_literal_sequence(value);
    if (!elements) {
        const auto *source_type = value.visit(
            [](const literal::ScalarLiteralItem &item) -> const ScalarRuntimeType * {
                return item.visit_scalar([](const auto *source) -> const ScalarRuntimeType * {
                    return &source->runtime_type();
                }).value_or(nullptr);
            },
            [](std::span<const literal::TimeSeriesLiteral>) -> const ScalarRuntimeType * { return nullptr; })
                                .value_or(nullptr);
        return source_type != nullptr && resolve_scalar_runtime_type(type, {}).accepts_input(*source_type);
    }

    if (const auto *list = type_cast<ListScalarType>(&type)) {
        return std::ranges::all_of(*elements, [&](const auto &item) {
            return graph_run_literal_matches_scalar(item, list->item_type());
        });
    }
    if (const auto *set = type_cast<SetScalarType>(&type)) {
        return std::ranges::all_of(*elements, [&](const auto &item) {
            return graph_run_literal_matches_scalar(item, set->item_type());
        });
    }
    if (const auto *dict = type_cast<DictScalarType>(&type)) {
        return std::ranges::all_of(*elements, [&](const auto &entry) {
            const auto pair = graph_run_literal_pair(entry);
            return pair && graph_run_literal_matches_scalar((*pair)[0], dict->key_type()) &&
                   graph_run_literal_matches_scalar((*pair)[1], dict->mapped_type());
        });
    }
    if (const auto *tuple = type_cast<TupleScalarType>(&type)) {
        return tuple->size() == elements->size() && [&] {
            for (auto index = std::size_t{}; index < tuple->size(); ++index) {
                if (!graph_run_literal_matches_scalar((*elements)[index], tuple->item_type(index))) {
                    return false;
                }
            }
            return true;
        }();
    }
    if (const auto *structure = type_cast<StructScalarType>(&type)) {
        return graph_run_named_scalar_fields_match(
            *elements,
            [&] { return structure->field_count(); },
            [&](std::size_t index) { return structure->field(index).name.text(); },
            [&](std::size_t index) -> const ScalarType & { return *structure->field(index).type; },
            [](std::size_t) { return false; });
    }
    const auto *hierarchical = type_cast<HierarchicalScalarType>(&type);
    if (const auto *bundle = dynamic_cast<const ResolvedHierarchicalScalarType *>(hierarchical)) {
        return graph_run_named_scalar_fields_match(
            *elements,
            [&] { return bundle->field_count(); },
            [&](std::size_t index) { return bundle->field(index).name.text(); },
            [&](std::size_t index) -> const ScalarType & {
                return static_cast<const ScalarType &>(*bundle->field(index).type);
            },
            [&](std::size_t index) { return bundle->field(index).optional; });
    }
    if (type_cast<BuiltinScalarType>(&type) != nullptr) {
        return false;
    }
    const auto &runtime_type = resolve_scalar_runtime_type(type, {});
    return ScalarDispatch<ScalarLiteralOperations>::instance().find(runtime_type) != nullptr;
}

inline bool graph_run_literal_matches_event(const literal::TimeSeriesLiteral &value, const TimeSeriesType &type) {
    if (const auto sentinel = literal::Sentinel::from(value);
        sentinel && (*sentinel == literal::SKIP || *sentinel == literal::INVALID)) {
        return true;
    }
    if (const auto *reference = type_cast<TimeSeriesReferenceType>(&type)) {
        return graph_run_literal_matches_event(value, reference->target_type());
    }
    if (const auto *scalar = type_cast<TimeSeriesValueType>(&type)) {
        return graph_run_literal_matches_scalar(value, scalar->value_type());
    }
    const auto elements = graph_run_literal_sequence(value);
    if (!elements) {
        return false;
    }
    if (const auto *dictionary = type_cast<TimeSeriesDictionaryType>(&type)) {
        return std::ranges::all_of(*elements, [&](const auto &entry) {
            const auto pair = graph_run_literal_pair(entry);
            if (!pair || !graph_run_literal_matches_scalar((*pair)[0], dictionary->key_type())) {
                return false;
            }
            const auto value_sentinel = literal::Sentinel::from((*pair)[1]);
            return (value_sentinel && (*value_sentinel == literal::SKIP || *value_sentinel == literal::REMOVE)) ||
                   graph_run_literal_matches_event((*pair)[1], dictionary->value_type());
        });
    }
    if (const auto *list = type_cast<TimeSeriesListType>(&type)) {
        return std::ranges::all_of(*elements, [&](const auto &entry) {
            const auto pair = graph_run_literal_pair(entry);
            return pair && graph_run_literal_matches_event((*pair)[1], list->item_type());
        });
    }
    if (const auto *set = type_cast<TimeSeriesSetType>(&type)) {
        return std::ranges::all_of(*elements, [&](const auto &item) {
            if (const auto tagged = literal::TaggedLiteral::from(item);
                tagged && tagged->sentinel == literal::REMOVED) {
                return graph_run_literal_matches_scalar(tagged->argument, set->item_type());
            }
            return graph_run_literal_matches_scalar(item, set->item_type());
        });
    }
    if (const auto *bundle = type_cast<TimeSeriesBundleType>(&type)) {
        return std::ranges::all_of(*elements, [&](const auto &entry) {
            const auto pair = graph_run_literal_pair(entry);
            if (!pair) {
                return false;
            }
            const auto name = graph_run_literal_name((*pair)[0]);
            if (!name) {
                return false;
            }
            for (auto index = std::size_t{}; index < bundle->item_count(); ++index) {
                if (*name == bundle->item_name(index).text()) {
                    return graph_run_literal_matches_event((*pair)[1], bundle->item_type(index));
                }
            }
            return false;
        });
    }
    return TimeSeriesDispatch<TimeSeriesLiteralOperations>::instance().find(type) != nullptr;
}

inline bool graph_run_raw_literals_are_initial_event(std::span<const literal::TimeSeriesLiteral> values,
                                                      const TimeSeriesType &target) {
    if (const auto *scalar = type_cast<TimeSeriesValueType>(&target)) {
        const auto &value_type = scalar->value_type();
        const auto is_composite = type_cast<ListScalarType>(&value_type) != nullptr ||
                                  type_cast<SetScalarType>(&value_type) != nullptr ||
                                  type_cast<DictScalarType>(&value_type) != nullptr ||
                                  type_cast<TupleScalarType>(&value_type) != nullptr ||
                                  type_cast<StructScalarType>(&value_type) != nullptr ||
                                  type_cast<HierarchicalScalarType>(&value_type) != nullptr;
        auto has_provider_literal = false;
        if (!is_composite && type_cast<BuiltinScalarType>(&value_type) == nullptr) {
            const auto &runtime_type = resolve_scalar_runtime_type(value_type, {});
            has_provider_literal = ScalarDispatch<ScalarLiteralOperations>::instance().find(runtime_type) != nullptr;
        }
        return (is_composite || has_provider_literal) &&
               graph_run_literal_matches_event(literal::TimeSeriesLiteral(values), target);
    }
    return graph_run_literal_matches_event(literal::TimeSeriesLiteral(values), target);
}

struct GraphRunInputShape {
    const TimeSeriesType *time_series_type{};
    std::optional<std::span<const literal::TimeSeriesLiteral>> values;
};

template <typename Input>
GraphRunInputShape graph_run_parameter_time_series_for_input(const Signature &signature, std::size_t index,
                                                             Input &input, GraphRunLiteralStorage &storage) {
    const auto parameter_index = index < signature.parameter_count()
                                     ? index
                                     : signature.first_variadic_parameter().value_or(index);
    const auto *variable = dynamic_cast<const TypeVariableType<Type> *>(signature.parameter(parameter_index).type);
    if (variable == nullptr) {
        return {};
    }

    using Range = std::remove_cvref_t<Input>;
    using Item = std::ranges::range_value_t<Range>;
    if constexpr (scalar_type_mapped<std::remove_cvref_t<Item>> && !scalar_source<Item>) {
        return {graph_run_parameter_time_series_for_value<Item>(signature, index), {}};
    } else if constexpr (std::ranges::forward_range<Range> && graph_run_range_input_v<Input> &&
                         !graph_run_optional_v<std::ranges::range_value_t<Range>> &&
                         !graph_run_literal_range_v<Input>) {
        auto &values = storage.sequences.emplace_back();
        for (auto &&value : input) {
            if constexpr (std::same_as<std::remove_cvref_t<decltype(value)>,
                                       std::ranges::range_value_t<Range>>) {
                values.emplace_back(graph_run_literal_from_value(std::forward<decltype(value)>(value), storage));
            } else {
                using Item = std::ranges::range_value_t<Range>;
                values.emplace_back(graph_run_literal_from_value(Item(value), storage));
            }
        }
        const auto literal_values = std::span<const literal::TimeSeriesLiteral>{values};
        const TimeSeriesType *match{};
        for (const auto *alternative : variable->alternatives()) {
            const auto *candidate = type_cast<TimeSeriesType>(alternative);
            if (candidate == nullptr || !graph_run_raw_literals_are_initial_event(literal_values, *candidate)) {
                continue;
            }
            if (match != nullptr) {
                throw std::invalid_argument{"run_graph input matches multiple generic time-series types"};
            }
            match = candidate;
        }
        if (match != nullptr) {
            return {match, literal_values};
        }
        const auto *fallback = graph_run_parameter_time_series_for_value<
            graph_run_input_scalar_t<std::ranges::range_value_t<Range>>>(signature, index);
        return {fallback, literal_values};
    } else {
        if constexpr (scalar_source<Item> || graph_run_optional_v<Item> || graph_run_range_input_v<Item>) {
            return {graph_run_parameter_time_series_for_value<graph_run_input_scalar_t<Item>>(signature, index), {}};
        } else {
            return {};
        }
    }
}

struct GraphRunArguments {
    const Signature &signature;
    std::vector<WiringArgument> values;
    std::deque<std::vector<std::optional<ScalarValue<>>>> ticks;
    GraphRunLiteralStorage literals;
    std::size_t cycle_count{1};
    std::size_t next_input{};

    template <typename Input> void append(Input &&input);

    template <typename Range> WiringPoint wire_ticks(Range &values, const TimeSeriesType &target);
    WiringPoint wire_literal(const GraphRunLiteralInput &input, std::size_t index) const;
    WiringArgument raw_literals_argument(RawGraphRunLiteralInput input, const TimeSeriesType &target);
    template <typename Input> WiringArgument argument_materialized(std::size_t index, Input &&input);
    template <typename Input> WiringArgument argument(std::size_t index, Input &&input);
};

template <typename Range>
WiringPoint GraphRunArguments::wire_ticks(Range &values, const TimeSeriesType &target) {
    using Item = std::ranges::range_value_t<std::remove_cvref_t<Range>>;
    const auto *value_type = graph_run_value_type(target);
    const auto uses_literal_source = value_type == nullptr || type_cast<SetScalarType>(value_type) != nullptr ||
                                    type_cast<DictScalarType>(value_type) != nullptr;
    if (uses_literal_source) {
        auto &events = literals.sequences.emplace_back();
        for (auto &&value : values) {
            if constexpr (graph_run_optional_v<Item>) {
                if (value) {
                    events.emplace_back(graph_run_literal_from_value(*value, literals));
                } else {
                    events.emplace_back(literal::TimeSeriesLiteral(literal::SKIP));
                }
            } else if constexpr (std::same_as<Item, literal::TimeSeriesLiteral>) {
                events.emplace_back(value);
            } else {
                if constexpr (std::same_as<std::remove_cvref_t<decltype(value)>, Item>) {
                    events.emplace_back(graph_run_literal_from_value(std::forward<decltype(value)>(value), literals));
                } else {
                    events.emplace_back(graph_run_literal_from_value(Item(value), literals));
                }
            }
        }

        cycle_count = std::max(cycle_count, events.size());
        return details::wire_literal_source(
            GraphRunLiteralInput(std::span<const literal::TimeSeriesLiteral>{events}),
            graph_run_runtime_type(target));
    }

    auto &stored_ticks = ticks.emplace_back();
    for (auto &&value : values) {
        if constexpr (graph_run_optional_v<Item>) {
            if (value) {
                if constexpr (graph_run_scalar_input_v<typename graph_run_optional<Item>::value_type>) {
                    stored_ticks.emplace_back(std::in_place, graph_run_owned_scalar_value(*value));
                } else {
                    throw std::invalid_argument{"run_graph requires a literal-compatible collection input"};
                }
            } else {
                stored_ticks.emplace_back(std::nullopt);
            }
        } else {
            if constexpr (graph_run_scalar_input_v<decltype(value)>) {
                auto item = [&] {
                    if constexpr (std::same_as<std::remove_cvref_t<decltype(value)>, Item>) {
                        return graph_run_owned_scalar_value(value);
                    } else {
                        return graph_run_owned_scalar_value(Item(value));
                    }
                }();
                stored_ticks.emplace_back(std::in_place, std::move(item));
            } else {
                throw std::invalid_argument{"run_graph requires a literal-compatible collection input"};
            }
        }
    }

    cycle_count = std::max(cycle_count, stored_ticks.size());

    auto runtime_type = std::addressof(graph_run_value_runtime_type(target));
    for (const auto &tick : stored_ticks) {
        if (tick) {
            runtime_type = std::addressof(tick->runtime_type());
            break;
        }
    }
    return details::wire_runtime_source(GraphRunInput{*runtime_type, stored_ticks});
}

inline WiringPoint GraphRunArguments::wire_literal(const GraphRunLiteralInput &input, std::size_t index) const {
    const auto &runtime_type = input.runtime_type == nullptr ? graph_run_runtime_type(signature, index)
                                                             : *input.runtime_type;
    return details::wire_literal_source(input, runtime_type);
}

inline WiringArgument GraphRunArguments::raw_literals_argument(RawGraphRunLiteralInput input,
                                                               const TimeSeriesType &target) {
    const auto &runtime_type = graph_run_runtime_type(target);
    const auto initial_event = graph_run_raw_literals_are_initial_event(input.values, runtime_type.type());
    cycle_count = std::max(cycle_count, initial_event ? std::size_t{1} : input.values.size());
    if (initial_event && graph_run_value_type(runtime_type.type()) != nullptr) {
        const auto value = literal::TimeSeriesLiteral(input.values);
        return WiringArgument{operators::const_(
            ScalarValue<>::from_literal(graph_run_value_runtime_type(runtime_type.type()), value))};
    }
    if (initial_event) {
        auto &event = literals.sequences.emplace_back();
        event.emplace_back(input.values);
        return WiringArgument{details::wire_literal_source(
            GraphRunLiteralInput(std::span<const literal::TimeSeriesLiteral>{event}), runtime_type)};
    }
    return WiringArgument{details::wire_literal_source(GraphRunLiteralInput(input.values), runtime_type)};
}

template <typename Input>
WiringArgument GraphRunArguments::argument_materialized(std::size_t index, Input &&input) {
    const auto parameter_index = index < signature.parameter_count()
                                     ? index
                                     : signature.first_variadic_parameter().value_or(index);
    auto *time_series_type = graph_run_parameter_time_series(signature, parameter_index);
    auto inferred_values = std::optional<std::span<const literal::TimeSeriesLiteral>>{};
    if (time_series_type == nullptr) {
        if constexpr (graph_run_range_input_v<Input> && !graph_run_literal_range_v<Input>) {
            const auto inferred = graph_run_parameter_time_series_for_input(signature, parameter_index, input, literals);
            time_series_type = inferred.time_series_type;
            inferred_values = inferred.values;
        }
    }
    if (time_series_type == nullptr) {
        if constexpr (std::same_as<std::remove_cvref_t<Input>, GraphRunInput>) {
            cycle_count = std::max(cycle_count, input.ticks.size());
            auto point = WiringPoint{};
            point = details::wire_runtime_source(input);
            return WiringArgument{point};
        } else if constexpr (std::same_as<std::remove_cvref_t<Input>, GraphRunLiteralInput>) {
            cycle_count = std::max(cycle_count, input.ticks.size());
            return WiringArgument{wire_literal(input, index)};
        } else if constexpr (graph_run_ticks_v<Input> ||
                             std::same_as<std::remove_cvref_t<Input>, RawGraphRunLiteralInput>) {
            throw std::invalid_argument{"a time-series input cannot bind to an unresolved graph parameter"};
        } else if constexpr (graph_run_literal_range_v<Input>) {
            throw std::invalid_argument{"a literal time-series input cannot bind to a scalar graph parameter"};
        } else if constexpr (graph_run_range_input_v<Input>) {
            if (inferred_values) {
                if constexpr (graph_run_scalar_input_v<Input>) {
                    using Scalar = graph_run_input_scalar_t<Input>;
                    auto value = ScalarValue<>::from_literal(cpp_scalar_runtime_type<Scalar>(),
                                                             literal::TimeSeriesLiteral(*inferred_values));
                    return WiringArgument{std::move(value)};
                } else {
                    throw std::invalid_argument{"run_graph needs a concrete time-series input type"};
                }
            }
            using Item = std::ranges::range_value_t<std::remove_cvref_t<Input>>;
            if constexpr (graph_run_optional_v<Item> ||
                          !(scalar_source<Item> || graph_run_range_input_v<Item>)) {
                throw std::invalid_argument{"run_graph needs a concrete time-series input type"};
            } else {
                return WiringArgument{graph_run_range_scalar_value(std::forward<Input>(input))};
            }
        } else {
            return WiringArgument{std::forward<Input>(input)};
        }
    }

    if (inferred_values) {
        return raw_literals_argument(RawGraphRunLiteralInput{*inferred_values}, *time_series_type);
    }

    WiringPoint point;
    if constexpr (std::same_as<std::remove_cvref_t<Input>, GraphRunInput>) {
        cycle_count = std::max(cycle_count, input.ticks.size());
        point = details::wire_runtime_source(input);
    } else if constexpr (std::same_as<std::remove_cvref_t<Input>, GraphRunLiteralInput>) {
        cycle_count = std::max(cycle_count, input.ticks.size());
        point = wire_literal(input, index);
    } else if constexpr (std::same_as<std::remove_cvref_t<Input>, RawGraphRunLiteralInput>) {
        return raw_literals_argument(input, *time_series_type);
    } else if constexpr (graph_run_initializer_list_v<Input>) {
        using Item = typename graph_run_initializer_list<std::remove_cvref_t<Input>>::value_type;
        if constexpr (graph_run_optional_v<Item>) {
            point = wire_ticks(input, *time_series_type);
        } else {
            auto &values = literals.sequences.emplace_back();
            for (auto &&value : input) {
                values.emplace_back(graph_run_literal_from_value(std::forward<decltype(value)>(value), literals));
            }
            const auto raw_values = std::span<const literal::TimeSeriesLiteral>{values};
            return raw_literals_argument(RawGraphRunLiteralInput{raw_values}, *time_series_type);
        }
    } else if constexpr (graph_run_ticks_v<Input>) {
        auto &values = input.values;
        point = wire_ticks(values, *time_series_type);
    } else if constexpr (graph_run_range_input_v<Input>) {
        if constexpr (std::same_as<std::ranges::range_value_t<std::remove_cvref_t<Input>>,
                                   literal::TimeSeriesLiteral>) {
            auto &stored_values = literals.sequences.emplace_back();
            for (const auto &value : input) {
                stored_values.emplace_back(value);
            }
            const auto values = std::span<const literal::TimeSeriesLiteral>{stored_values};
            return raw_literals_argument(RawGraphRunLiteralInput{values}, *time_series_type);
        } else if constexpr (graph_run_optional_v<std::ranges::range_value_t<std::remove_cvref_t<Input>>>) {
            point = wire_ticks(input, *time_series_type);
        } else {
            using Item = std::ranges::range_value_t<std::remove_cvref_t<Input>>;
            if constexpr (scalar_type_mapped<std::remove_cvref_t<Item>> && !scalar_source<Item>) {
                point = wire_ticks(input, *time_series_type);
            } else {
                auto &values = literals.sequences.emplace_back();
                for (auto &&value : input) {
                    if constexpr (std::same_as<std::remove_cvref_t<decltype(value)>, Item>) {
                        values.emplace_back(graph_run_literal_from_value(std::forward<decltype(value)>(value), literals));
                    } else {
                        values.emplace_back(graph_run_literal_from_value(Item(value), literals));
                    }
                }
                return raw_literals_argument(
                    RawGraphRunLiteralInput{std::span<const literal::TimeSeriesLiteral>{values}}, *time_series_type);
            }
        }
    } else if constexpr (requires { details::scalar_data(input); } || scalar_source<Input>) {
        cycle_count = std::max(cycle_count, std::size_t{1});
        return WiringArgument{operators::const_(std::forward<Input>(input))};
    } else if constexpr (scalar_type_mapped<std::remove_cvref_t<Input>>) {
        cycle_count = std::max(cycle_count, std::size_t{1});
        auto value = graph_run_scalar_value(std::forward<Input>(input));
        return WiringArgument{operators::const_(std::move(value))};
    } else {
        static_assert(sizeof(Input) == 0, "run_graph input must be a scalar, range, or typed input descriptor");
    }
    return WiringArgument{point};
}

template <typename Input>
WiringArgument GraphRunArguments::argument(std::size_t index, Input &&input) {
    using Range = std::remove_cvref_t<Input>;
    if constexpr (graph_run_range_input_v<Input> && std::ranges::input_range<Range> &&
                  !std::ranges::forward_range<Range> && !graph_run_literal_range_v<Input>) {
        using Item = std::ranges::range_value_t<Range>;
        auto values = std::vector<Item>{};
        for (auto &&value : input) {
            if constexpr (std::same_as<std::remove_cvref_t<decltype(value)>, Item>) {
                values.emplace_back(std::forward<decltype(value)>(value));
            } else {
                values.emplace_back(Item(value));
            }
        }
        return argument(index, values);
    } else {
        return argument_materialized(index, std::forward<Input>(input));
    }
}

template <typename Input> void GraphRunArguments::append(Input &&input) {
    const auto index = next_input++;
    values.emplace_back(argument(index, std::forward<Input>(input)));
}

template <typename OutputScalar, typename Tested>
GraphRunResult<OutputScalar> run_graph_with_arguments(const Tested &tested, const GraphRunOptions &options,
                                                      auto &&populate_arguments) {
    auto arguments = GraphRunArguments{tested.signature()};
    auto captured = details::CapturedValues{
        options.start_time.value_or(options.engine == EvaluationEngine::realtime
                                       ? SystemWallClock::instance().now()
                                       : MIN_DT + EvaluationTime::duration{1})};
    auto wiring = GraphWiring{};
    {
        auto context = WiringContext{wiring};
        std::invoke(std::forward<decltype(populate_arguments)>(populate_arguments), arguments);
        const auto output = static_cast<const WiringTemplate &>(tested)(std::span<const WiringArgument>{arguments.values});
        if (options.capture_output && output) {
            details::capture_graph_output(*output, captured);
        }
    }
    captured.set_cycle_count(arguments.cycle_count);
    return GraphRunResult<OutputScalar>{details::finish_graph_run(wiring, captured, options)};
}

template <typename OutputScalar, typename Tested, typename... Inputs>
GraphRunResult<OutputScalar> run_graph_target(const Tested &tested, const GraphRunOptions &options,
                                              Inputs &&...inputs) {
    const auto &signature = tested.signature();
    if (sizeof...(Inputs) > signature.parameter_count() && !signature.has_variadic_parameters()) {
        throw std::invalid_argument{"run_graph input count exceeds the graph signature"};
    }
    preflight_graph_run_inputs(signature, std::index_sequence_for<Inputs...>{}, inputs...);

    return run_graph_with_arguments<OutputScalar>(tested, options, [&](GraphRunArguments &arguments) {
        arguments.values.reserve(sizeof...(Inputs));
        (arguments.append(std::forward<Inputs>(inputs)), ...);
    });
}

template <typename OutputScalar, typename Tested, std::ranges::contiguous_range InputRange>
    requires graph_run_descriptor_range_v<InputRange>
GraphRunResult<OutputScalar> run_graph_descriptor_range_target(const Tested &tested,
                                                                const GraphRunOptions &options,
                                                                const InputRange &inputs) {
    const auto &signature = tested.signature();
    const auto input_count = std::ranges::size(inputs);
    if (input_count > signature.parameter_count() && !signature.has_variadic_parameters()) {
        throw std::invalid_argument{"run_graph input count exceeds the graph signature"};
    }
    auto index = std::size_t{};
    for (const auto &input : inputs) {
        preflight_graph_run_input(signature, index++, input);
    }

    return run_graph_with_arguments<OutputScalar>(tested, options, [&](GraphRunArguments &arguments) {
        arguments.values.reserve(input_count);
        for (const auto &input : inputs) {
            arguments.append(input);
        }
    });
}

template <typename Tested, typename... Inputs>
auto run_graph_bound_target(const Tested &tested, const GraphRunOptions &options, Inputs &&...inputs) {
    if constexpr (std::derived_from<std::remove_cvref_t<Tested>, WiringTemplate>) {
        return run_graph_target<void>(tested, options, std::forward<Inputs>(inputs)...);
    } else {
        using FunctionInfo = details::graph_function<Tested>;
        auto graph_template = bg::graph(tested);
        return run_graph_target<typename FunctionInfo::output_scalar>(
            graph_template, options, std::forward<Inputs>(inputs)...);
    }
}

template <typename Tested, std::ranges::contiguous_range InputRange>
    requires graph_run_descriptor_range_v<InputRange>
auto run_graph_bound_descriptor_range_target(const Tested &tested, const GraphRunOptions &options,
                                             const InputRange &inputs) {
    if constexpr (std::derived_from<std::remove_cvref_t<Tested>, WiringTemplate>) {
        return run_graph_descriptor_range_target<void>(tested, options, inputs);
    } else {
        using FunctionInfo = details::graph_function<Tested>;
        auto graph_template = bg::graph(tested);
        return run_graph_descriptor_range_target<typename FunctionInfo::output_scalar>(graph_template, options,
                                                                                       inputs);
    }
}

} // namespace details

// Wire an explicitly typed input while a run_graph callback has an active WiringContext.
inline WiringPoint graph_run_input(const GraphRunLiteralInput &input) {
    if (input.runtime_type == nullptr) {
        throw std::invalid_argument{"graph_run_input requires an explicit time-series type"};
    }
    return details::wire_literal_source(input, *input.runtime_type);
}

inline WiringPoint graph_run_input(const GraphRunInput &input) {
    return details::wire_runtime_source(input);
}

template <std::invocable Function>
    requires(!std::derived_from<std::remove_cvref_t<Function>, WiringTemplate>)
auto run_graph(Function &&function, const GraphRunOptions &options = {}) {
    return details::run_graph_callback(std::forward<Function>(function), options);
}

template <typename Tested, std::ranges::contiguous_range Inputs>
    requires details::graph_run_target<Tested> && details::graph_run_descriptor_range_v<Inputs>
auto run_graph(const Tested &tested, const Inputs &inputs) {
    return details::run_graph_bound_descriptor_range_target(tested, GraphRunOptions{}, inputs);
}

template <typename Tested, typename Options, std::ranges::contiguous_range Inputs>
    requires details::graph_run_target<Tested> && std::same_as<std::remove_cvref_t<Options>, GraphRunOptions> &&
             details::graph_run_descriptor_range_v<Inputs>
auto run_graph(const Tested &tested, const Options &options, const Inputs &inputs) {
    return details::run_graph_bound_descriptor_range_target(tested, options, inputs);
}

template <typename Tested, typename... Inputs>
    requires details::graph_run_target<Tested> && (sizeof...(Inputs) > 0) &&
             ((!std::same_as<std::remove_cvref_t<Inputs>, GraphRunOptions>) && ...) &&
             ((!details::graph_run_descriptor_range_v<Inputs>) && ...)
auto run_graph(const Tested &tested, Inputs &&...inputs) {
    return details::run_graph_bound_target(tested, GraphRunOptions{}, std::forward<Inputs>(inputs)...);
}

template <typename Tested, typename Options, typename... Inputs>
    requires details::graph_run_target<Tested> && (sizeof...(Inputs) > 0) &&
             std::same_as<std::remove_cvref_t<Options>, GraphRunOptions> &&
             ((!details::graph_run_descriptor_range_v<Inputs>) && ...)
auto run_graph(const Tested &tested, const Options &options, Inputs &&...inputs) {
    return details::run_graph_bound_target(tested, options, std::forward<Inputs>(inputs)...);
}

template <typename Tested, typename... Values>
    requires details::graph_run_target<Tested> && (sizeof...(Values) > 0) &&
             ((!details::graph_run_character_pointer_v<Values>) && ...) &&
             ((!std::same_as<std::remove_cvref_t<Values>, literal::Sentinel>) && ...)
auto run_graph(const Tested &tested, std::initializer_list<Values>... inputs) {
    return details::run_graph_bound_target(tested, GraphRunOptions{}, inputs...);
}

template <typename Tested, typename... Values>
    requires details::graph_run_target<Tested> && (sizeof...(Values) > 0) &&
             ((!details::graph_run_character_pointer_v<Values>) && ...) &&
             ((!std::same_as<std::remove_cvref_t<Values>, literal::Sentinel>) && ...)
auto run_graph(const Tested &tested, const GraphRunOptions &options,
                             std::initializer_list<Values>... inputs) {
    return details::run_graph_bound_target(tested, options, inputs...);
}

template <typename Tested>
    requires details::graph_run_target<Tested>
auto run_graph(const Tested &tested, std::initializer_list<literal::TimeSeriesLiteral> input) {
    return details::run_graph_bound_target(tested, GraphRunOptions{}, input);
}

template <typename Tested>
    requires details::graph_run_target<Tested>
auto run_graph(const Tested &tested, const GraphRunOptions &options,
                             std::initializer_list<literal::TimeSeriesLiteral> input) {
    return details::run_graph_bound_target(tested, options, input);
}

template <typename Tested>
    requires details::graph_run_target<Tested>
auto run_graph(const Tested &tested, std::initializer_list<literal::TimeSeriesLiteral> first,
                             std::initializer_list<literal::TimeSeriesLiteral> second) {
    return details::run_graph_bound_target(tested, GraphRunOptions{}, first, second);
}

template <typename Tested>
    requires details::graph_run_target<Tested>
auto run_graph(const Tested &tested, const GraphRunOptions &options,
                             std::initializer_list<literal::TimeSeriesLiteral> first,
                             std::initializer_list<literal::TimeSeriesLiteral> second) {
    return details::run_graph_bound_target(tested, options, first, second);
}

template <typename Tested>
    requires details::graph_run_target<Tested>
auto run_graph(const Tested &tested, std::initializer_list<literal::TimeSeriesLiteral> first,
                             std::initializer_list<literal::TimeSeriesLiteral> second,
                             std::initializer_list<literal::TimeSeriesLiteral> third) {
    return details::run_graph_bound_target(tested, GraphRunOptions{}, first, second, third);
}

template <typename Tested>
    requires details::graph_run_target<Tested>
auto run_graph(const Tested &tested, const GraphRunOptions &options,
                             std::initializer_list<literal::TimeSeriesLiteral> first,
                             std::initializer_list<literal::TimeSeriesLiteral> second,
                             std::initializer_list<literal::TimeSeriesLiteral> third) {
    return details::run_graph_bound_target(tested, options, first, second, third);
}

template <typename Tested>
    requires details::graph_run_target<Tested>
auto run_graph(const Tested &tested, std::initializer_list<literal::TimeSeriesLiteral> first,
                             std::initializer_list<literal::TimeSeriesLiteral> second,
                             std::initializer_list<literal::TimeSeriesLiteral> third,
                             std::initializer_list<literal::TimeSeriesLiteral> fourth) {
    return details::run_graph_bound_target(tested, GraphRunOptions{}, first, second, third, fourth);
}

template <typename Tested>
    requires details::graph_run_target<Tested>
auto run_graph(const Tested &tested, const GraphRunOptions &options,
                             std::initializer_list<literal::TimeSeriesLiteral> first,
                             std::initializer_list<literal::TimeSeriesLiteral> second,
                             std::initializer_list<literal::TimeSeriesLiteral> third,
                             std::initializer_list<literal::TimeSeriesLiteral> fourth) {
    return details::run_graph_bound_target(tested, options, first, second, third, fourth);
}

template <typename Tested>
    requires std::derived_from<std::remove_cvref_t<Tested>, WiringTemplate>
auto run_graph(const Tested &tested) {
    return details::run_graph_bound_target(tested, GraphRunOptions{});
}

template <typename Tested>
    requires std::derived_from<std::remove_cvref_t<Tested>, WiringTemplate>
auto run_graph(const Tested &tested, const GraphRunOptions &options) {
    return details::run_graph_bound_target(tested, options);
}

} // namespace bg
