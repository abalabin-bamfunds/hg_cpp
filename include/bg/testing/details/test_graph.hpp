#pragma once

#include <bg/evaluation_time.hpp>
#include <bg/evaluation_profiler.hpp>
#include <bg/graph/compiled_graph.hpp>
#include <bg/graph/details/wiring_callable.hpp>
#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/runtime_graph.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/graph/wiring_template.hpp>
#include <bg/node.hpp>
#include <bg/node/runtime_signature_node_template.hpp>
#include <bg/scalar/literal.hpp>
#include <bg/scalar/canonical.hpp>
#include <bg/scalar/list/type.hpp>
#include <bg/scalar/runtime_type.hpp>
#include <bg/scalar/value.hpp>
#include <bg/service_catalogue.hpp>
#include <bg/simulation_evaluation_engine.hpp>
#include <bg/graph_run_options.hpp>
#include <bg/testing/test_graph_input.hpp>
#include <bg/testing/test_graph_literal_input.hpp>
#include <bg/type/type_map.hpp>
#include <bg/type/type_resolution_error.hpp>
#include <bg/time_series/details/scalar_literal.hpp>
#include <bg/time_series/tsd/tsd_runtime_type.hpp>
#include <bg/time_series/tss/tss_runtime_type.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace bg::testing::details {

struct Source {
    static constexpr bool uses_timed_scheduling = true;

    Scalar<List<SCALAR>> values;
    Scalar<List<EvaluationTime>> times;
    State<std::size_t> next;
    Out<TS<SCALAR>> output;

    void start(EvaluationContext *context) const { publish(context); }
    void evaluate(EvaluationContext *context) const { publish(context); }

  private:
    void publish(EvaluationContext *context) const {
        if (*next < times.size() && times[*next].value() == context->evaluation_time()) {
            output.set_value(values[*next]);
            ++*next;
        }

        if (*next < times.size()) {
            context->schedule_at(times[*next].value());
        }
    }
};

struct CapturedValues {
    struct Entry;

    struct Delta {
        enum class Operation { update, add, remove };

        const TimeSeriesRuntimeType *runtime_type{};
        struct Invalid {};
        std::variant<Invalid, ScalarValue<>, std::vector<Entry>> payload;
    };

    struct Entry {
        Delta::Operation operation{Delta::Operation::update};
        std::optional<ScalarValue<>> key;
        std::size_t index{};
        Delta value;
    };

    EvaluationTime start_time;
    std::vector<std::optional<Delta>> records;

    void set(EvaluationTime time, Delta delta) {
        const auto index = position(time);
        records[index].emplace(std::move(delta));
    }

    void reset(EvaluationTime time, const TimeSeriesRuntimeType &runtime_type) {
        auto delta = Delta{};
        delta.runtime_type = &runtime_type;
        set(time, std::move(delta));
    }

  private:
    std::size_t position(EvaluationTime time) {
        const auto index = static_cast<std::size_t>((time - start_time).count());
        if (index >= records.size()) {
            records.resize(index + 1);
        }

        return index;
    }
};

inline const auto source = node<Source>();

using CapturedDelta = CapturedValues::Delta;

struct CaptureTimeSeries {
    CaptureTimeSeries(CapturedDelta &delta, allocator &memory) : delta{&delta}, memory{&memory} {}

    template <typename SCALAR> void operator()(const In<TS<SCALAR>> *input) const {
        delta->runtime_type = &input->runtime_type();
        if (!input->valid()) {
            return;
        }
        delta->payload.template emplace<ScalarValue<>>(input->value_view(), *memory);
    }

    template <typename ITEM, auto SIZE> void operator()(const In<TSL<ITEM, SIZE>> *input) const {
        delta->runtime_type = &input->runtime_type();
        if (!input->valid()) {
            return;
        }
        auto &changes = delta->payload.template emplace<std::vector<CapturedValues::Entry>>();
        input->for_each_updated([&](item_id_t id) {
            auto change = child(CapturedDelta::Operation::update, input->runtime_type());
            change.index = id;
            auto item = input->operator[](id);
            item.visit(CaptureTimeSeries{change.value, *memory}, ddv::noop);
            changes.push_back(std::move(change));
        });
    }

    template <typename KEY> void operator()(const In<TSS<KEY>> *input) const {
        delta->runtime_type = &input->runtime_type();
        if (!input->valid()) {
            return;
        }
        auto &changes = delta->payload.template emplace<std::vector<CapturedValues::Entry>>();
        input->for_each_added([&](const auto &key) {
            auto change = child(CapturedDelta::Operation::add, input->runtime_type());
            change.key.emplace(key, *memory);
            changes.push_back(std::move(change));
        });
        input->for_each_removed([&](const auto &key) {
            auto change = child(CapturedDelta::Operation::remove, input->runtime_type());
            change.key.emplace(key, *memory);
            changes.push_back(std::move(change));
        });
    }

    template <typename KEY, typename ITEM> void operator()(const In<TSD<KEY, ITEM>> *input) const {
        delta->runtime_type = &input->runtime_type();
        if (!input->valid()) {
            return;
        }
        delta->payload.template emplace<std::vector<CapturedValues::Entry>>();
        input->for_each_added([&](const auto &key) { capture_item(*input, key, CapturedDelta::Operation::add); });
        input->for_each_modified(
            [&](const auto &key) { capture_item(*input, key, CapturedDelta::Operation::update); });
        input->for_each_removed([&](const auto &key) {
            auto change = child(CapturedDelta::Operation::remove, input->runtime_type());
            change.key.emplace(key, *memory);
            std::get<std::vector<CapturedValues::Entry>>(delta->payload).push_back(std::move(change));
        });
    }

    template <template <template <typename, auto...> typename> typename SCHEMA>
    void operator()(const In<TSB<SCHEMA>> *input) const {
        delta->runtime_type = &input->runtime_type();
        if (!input->valid()) {
            return;
        }
        auto &changes = delta->payload.template emplace<std::vector<CapturedValues::Entry>>();
        input->for_each_updated([&](item_id_t id) {
            auto change = child(CapturedDelta::Operation::update, input->runtime_type());
            change.index = id;
            const auto item = input->runtime_type().item(input->input_data().data(), id);
            const auto modified = input->modified() && item.last_updated.valid() &&
                                  item.last_updated.time() == input->last_updated();
            const auto child_input = In<TS_T>{TimeSeriesInputData{item.runtime_type, item.last_updated, item.value,
                                                                  modified}};
            child_input.visit(CaptureTimeSeries{change.value, *memory}, ddv::noop);
            changes.push_back(std::move(change));
        });
    }

    template <typename TARGET> void operator()(const In<REF<TARGET>> *) const {
        throw std::invalid_argument{"test_graph cannot capture nested time-series references"};
    }

    void operator()(const In<TS_T> *input) const {
        static_cast<void>(input);
        throw std::invalid_argument{"test_graph cannot capture an external time-series type"};
    }

  private:
    static CapturedValues::Entry child(CapturedDelta::Operation operation,
                                       const TimeSeriesRuntimeType &runtime_type) {
        auto result = CapturedValues::Entry{};
        result.operation = operation;
        result.value.runtime_type = &runtime_type;
        return result;
    }

    template <typename KEY, typename Input>
    void capture_item(const Input &input, const KEY &key, CapturedDelta::Operation operation) const {
        auto change = child(operation, input.runtime_type());
        change.key.emplace(key, *memory);
        auto item = input[key];
        item.visit(CaptureTimeSeries{change.value, *memory}, ddv::noop);
        std::get<std::vector<CapturedValues::Entry>>(delta->payload).push_back(std::move(change));
    }

    CapturedDelta *delta;
    allocator *memory;
};

class RuntimeSink {
  public:
    struct CompiledData {
        CapturedValues *captured;
    };

    using compiled_data_type = CompiledData;
    using Invocation = NodeInvocation<CompiledData>;

    RuntimeSink(const TimeSeriesType &input_type, CapturedValues &captured)
        : captured_{&captured},
          signature_{util::symbol{"test_sink"}, {{.name = util::symbol{"input"}, .type = &input_type}}} {}

    const Signature &signature() const noexcept { return signature_; }
    CompiledData compile(const ResolvedSignature &, allocator &) const noexcept { return {captured_}; }

    static void evaluate(Invocation invocation) {
        auto delta = CapturedDelta{};
        auto memory = allocator{};
        invocation.input<In<TS_T>>(0).visit(CaptureTimeSeries{delta, memory}, ddv::noop);
        invocation.compiled_data().captured->set(invocation.context().evaluation_time(), std::move(delta));
    }

    static void on_invalidation(Invocation invocation) {
        const auto input = invocation.input<In<TS_T>>(0);
        invocation.compiled_data().captured->reset(invocation.context().evaluation_time(), input.runtime_type());
    }

  private:
    CapturedValues *captured_;
    Signature signature_;
};

inline void capture_output(WiringPoint output, CapturedValues &captured) {
    const auto &input_type = output.type().dereferenced_type();
    auto definition = RuntimeSink{input_type, captured};
    auto &wiring = WiringContext::current().wiring();
    const auto &sink_template = wiring.own_node_template(
        std::make_unique<RuntimeSignatureNodeTemplate<RuntimeSink>>(std::move(definition)));
    static_cast<const WiringTemplate &>(sink_template)(output);
}

class RuntimeSource {
  public:
    struct CompiledData {
        std::span<const std::optional<ScalarValue<>>> ticks;
        EvaluationTime start_time;
    };

    using compiled_data_type = CompiledData;
    using state_type = std::size_t;
    using Invocation = NodeInvocation<CompiledData, state_type>;

    RuntimeSource(const TestGraphInput &input, EvaluationTime start_time)
        : input_{input}, start_time_{start_time},
          signature_{util::symbol{"test_source"},
                     {{.name = util::symbol{"runtime_type"},
                       .type = &input.runtime_type.type(),
                       .accepts_absent_scalar = true}},
                     time_series_value_type(input.runtime_type.type(), TimeSeriesGuarantee::ordinary)} {}

    const Signature &signature() const noexcept { return signature_; }
    CompiledData compile(const ResolvedSignature &, allocator &) const noexcept { return {input_.ticks, start_time_}; }

    static void start(Invocation invocation) { publish(invocation); }
    static void evaluate(Invocation invocation) { publish(invocation); }

    static constexpr bool uses_timed_scheduling = true;

  private:
    static void publish(Invocation invocation) {
        const auto &data = invocation.compiled_data();
        auto &next = invocation.state();

        if (next < data.ticks.size() && data.ticks[next]) {
            invocation.output().visit([&](Out<TS<SCALAR>> *output) { output->set_value(data.ticks[next]->view()); },
                                      ddv::noop);
        }

        ++next;
        while (next < data.ticks.size() && !data.ticks[next]) {
            ++next;
        }
        if (next < data.ticks.size()) {
            invocation.context().schedule_at(data.start_time + EvaluationTime::duration{next});
        }
    }

    TestGraphInput input_;
    EvaluationTime start_time_;
    Signature signature_;
};

class LiteralSource {
  public:
    struct CompiledData {
        std::span<const literal::TimeSeriesLiteral> ticks;
        EvaluationTime start_time;
    };

    using compiled_data_type = CompiledData;
    using state_type = std::size_t;
    using Invocation = NodeInvocation<CompiledData, state_type>;
    static constexpr bool uses_timed_scheduling = true;
    static constexpr bool deduplicate_wiring = false;

    LiteralSource(const TestGraphLiteralInput &input, const TimeSeriesRuntimeType &runtime_type,
                  EvaluationTime start_time)
        : ticks_{input.ticks}, runtime_type_{&runtime_type}, start_time_{start_time},
          signature_{util::symbol{"test_literal_source"}, {}, runtime_type.type()} {}

    const Signature &signature() const noexcept { return signature_; }
    CompiledData compile(const ResolvedSignature &, allocator &) const noexcept { return {ticks_, start_time_}; }
    const TimeSeriesRuntimeType &output_runtime_type(const WiringArguments &) const noexcept { return *runtime_type_; }

    static void start(Invocation invocation) { publish(invocation); }
    static void evaluate(Invocation invocation) { publish(invocation); }

  private:
    static bool is_skip(const literal::TimeSeriesLiteral &tick) {
        const auto sentinel = literal::Sentinel::from(tick);
        return sentinel != nullptr && *sentinel == literal::SKIP;
    }

    static void publish(Invocation invocation) {
        const auto &data = invocation.compiled_data();
        auto &next = invocation.state();

        if (next < data.ticks.size() && invocation.context().evaluation_time() ==
                                             data.start_time + EvaluationTime::duration{next}) {
            invocation.output().apply(data.ticks[next]);
            ++next;
        }

        while (next < data.ticks.size() && is_skip(data.ticks[next])) {
            ++next;
        }
        if (next < data.ticks.size()) {
            invocation.context().schedule_at(data.start_time + EvaluationTime::duration{next});
        }
    }

    std::span<const literal::TimeSeriesLiteral> ticks_;
    const TimeSeriesRuntimeType *runtime_type_;
    EvaluationTime start_time_;
    Signature signature_;
};

template <typename T> struct optional_value : std::false_type {};
template <typename T> struct optional_value<std::optional<T>> : std::true_type {};

template <typename T> struct canonical_tick {
    using type = canonical_scalar_t<T>;
};

template <typename T> struct canonical_tick<std::optional<T>> {
    using type = canonical_scalar_t<T>;
};

template <typename T> using canonical_tick_t = typename canonical_tick<T>::type;

template <typename TimeSeries> struct test_output_scalar {
    using type = void;
};

template <typename T, auto... Properties> struct test_output_scalar<TS<T, Properties...>> {
    using type = T;
};

template <typename Target, auto... Properties>
struct test_output_scalar<REF<Target, Properties...>> : test_output_scalar<Target> {};

template <typename Scalar, std::ranges::input_range Range>
[[nodiscard]] WiringPoint wire_source(const Range &ticks, EvaluationTime start_time) {
    using Tick = std::remove_cvref_t<std::ranges::range_value_t<Range>>;
    using Value = typename bg::scalar_storage_traits<Scalar>::value_type;
    using StoredValue =
        std::conditional_t<std::same_as<std::remove_cvref_t<Value>, bool>, std::uint8_t, std::remove_cvref_t<Value>>;

    auto values = std::vector<StoredValue>{};
    auto times = std::vector<EvaluationTime>{};
    values.reserve(std::ranges::size(ticks));
    times.reserve(std::ranges::size(ticks));

    auto index = std::size_t{};
    for (const auto &tick : ticks) {
        if constexpr (optional_value<Tick>::value) {
            if (tick) {
                values.emplace_back(static_cast<StoredValue>(*tick));
                times.push_back(start_time + std::chrono::microseconds{index});
            }
        } else {
            values.emplace_back(static_cast<StoredValue>(tick));
            times.push_back(start_time + std::chrono::microseconds{index});
        }

        ++index;
    }

    return source(ScalarValue<List<Scalar>>{values}, ScalarValue<List<EvaluationTime>>{times});
}

template <typename Function> struct test_function {
    using callable = bg::details::callable<std::remove_cvref_t<Function>>;
    using result = std::remove_cvref_t<typename callable::result_type>;
    using output_time_series = typename bg::details::typed_wiring_point<result>::time_series_type;
    using output_scalar = typename test_output_scalar<output_time_series>::type;

    template <std::size_t Index> using argument = std::remove_cvref_t<typename callable::template argument_type<Index>>;

    template <std::size_t Index>
    using input_time_series = typename bg::details::typed_wiring_point<argument<Index>>::time_series_type;

    template <std::size_t Index> using input_scalar = typename input_time_series<Index>::value_type;
};

inline auto finish_test_graph(GraphWiring &wiring, CapturedValues &captured, EvaluationTime start_time,
                              const GraphRunOptions &options) {
    if (options.services != nullptr) {
        wiring.finalize(*options.services);
    } else {
        wiring.finalize();
    }

    const auto compiled = CompiledGraph{wiring};
    auto run = [&](GlobalState &global_state) {
        auto runtime = RuntimeGraph{compiled, global_state};
        if (options.recorder) {
            options.recorder->run(runtime, start_time);
        } else {
            SimulationEvaluationEngine{}.run(runtime, start_time);
        }
    };

    if (options.global_state != nullptr) {
        run(*options.global_state);
    } else {
        auto global_state = GlobalState{};
        run(global_state);
    }

    return std::move(captured);
}

class LiteralMatcher {
  public:
    static bool matches_tick(const std::optional<CapturedDelta> &actual,
                             const literal::TimeSeriesLiteral &expected) {
        if (marker(expected) == Marker::skip) {
            return !actual;
        }
        if (!actual) {
            return false;
        }
        try {
            return matches_delta(*actual, expected);
        } catch (const std::invalid_argument &) {
            return false;
        }
    }

  private:
    enum class Marker { value, skip, invalid, remove, removed };

    static Marker marker(const literal::TimeSeriesLiteral &value) {
        auto result = Marker::value;
        value.visit(
            [&](const literal::TimeSeriesLiteralItem &item) {
                item.visit_item(
                    [](const literal::ScalarLiteralItem *) {},
                    [&](const literal::SkipLiteral *) { result = Marker::skip; },
                    [&](const literal::InvalidLiteral *) { result = Marker::invalid; },
                    [&](const literal::RemoveLiteral *) { result = Marker::remove; },
                    [&](const literal::RemovedLiteral *) { result = Marker::removed; });
            },
            [](std::span<const literal::TimeSeriesLiteral>) {});
        return result;
    }

    template <typename Predicate>
    static bool consume_entry(const std::vector<CapturedValues::Entry> &entries, std::vector<bool> &consumed,
                              Predicate &&predicate) {
        for (auto index = std::size_t{}; index < entries.size(); ++index) {
            if (!consumed[index] && predicate(entries[index])) {
                consumed[index] = true;
                return true;
            }
        }
        return false;
    }

    static bool all_consumed(const std::vector<bool> &consumed) {
        return std::ranges::all_of(consumed, [](bool used) { return used; });
    }

    static bool matches_delta(const CapturedDelta &actual, const literal::TimeSeriesLiteral &expected) {
        const auto expected_marker = marker(expected);
        if (expected_marker == Marker::invalid) {
            return std::holds_alternative<CapturedDelta::Invalid>(actual.payload);
        }
        if (expected_marker != Marker::value || actual.runtime_type == nullptr) {
            return false;
        }

        if (const auto *scalar = std::get_if<ScalarValue<>>(&actual.payload)) {
            const auto &runtime_type = static_cast<const bg::TimeSeriesValueRuntimeType &>(*actual.runtime_type);
            auto memory = allocator{};
            const auto expected_value =
                ScalarValue<>::from_literal(runtime_type.scalar_runtime_type(), expected, memory);
            return scalar->equals(expected_value);
        }
        if (std::holds_alternative<CapturedDelta::Invalid>(actual.payload)) {
            return false;
        }

        return expected.visit(
            [&](const literal::TimeSeriesLiteralItem &) { return false; },
            [&](std::span<const literal::TimeSeriesLiteral> values) {
                return actual.runtime_type->type().visit(
                    [&](const TimeSeriesListType *) { return matches_list(actual, values); },
                    [&](const TimeSeriesSetType *) { return matches_set(actual, values); },
                    [&](const TimeSeriesDictionaryType *) { return matches_dictionary(actual, values); },
                    [&](const TimeSeriesBundleType *type) { return matches_bundle(actual, values, *type); },
                    [&] { return false; });
            }).value();
    }

    static bool matches_list(const CapturedDelta &actual, std::span<const literal::TimeSeriesLiteral> expected) {
        const auto &entries = std::get<std::vector<CapturedValues::Entry>>(actual.payload);
        auto consumed = std::vector<bool>(entries.size());
        for (const auto &value : expected) {
            const auto pair = bg::details::scalar_literal::pair(
                value, "a TSL update entry requires an index and a value");
            if (marker(*pair.second) == Marker::skip) {
                continue;
            }
            const auto index = bg::details::scalar_literal::list_index(*pair.first);
            if (!consume_entry(entries, consumed, [&](const auto &entry) {
                    return entry.operation == CapturedDelta::Operation::update && !entry.key &&
                           entry.index == index && matches_delta(entry.value, *pair.second);
                })) {
                return false;
            }
        }
        return all_consumed(consumed);
    }

    static bool matches_set(const CapturedDelta &actual, std::span<const literal::TimeSeriesLiteral> expected) {
        const auto &entries = std::get<std::vector<CapturedValues::Entry>>(actual.payload);
        const auto &runtime_type = static_cast<const bg::details::TimeSeriesSetRuntimeType &>(*actual.runtime_type);
        auto memory = allocator{};
        auto consumed = std::vector<bool>(entries.size());
        for (const auto &value : expected) {
            const auto value_marker = marker(value);
            if (value_marker == Marker::skip) {
                continue;
            }
            auto key = std::optional<ScalarValue<>>{};
            auto operation = CapturedDelta::Operation::add;
            if (value_marker == Marker::value) {
                key.emplace(ScalarValue<>::from_literal(runtime_type.key_runtime_type(), value, memory));
            } else if (value_marker == Marker::removed) {
                value.visit(
                    [&](const literal::TimeSeriesLiteralItem &item) {
                        item.visit_item(
                            [&](const literal::RemovedLiteral *removed) {
                                key.emplace(ScalarValue<>::from_literal(runtime_type.key_runtime_type(),
                                                                        *removed->value, memory));
                            },
                            [](const auto *) {});
                    },
                    [](std::span<const literal::TimeSeriesLiteral>) {});
                operation = CapturedDelta::Operation::remove;
            } else {
                return false;
            }

            if (!key || !consume_entry(entries, consumed, [&](const auto &entry) {
                    return entry.operation == operation && entry.key && entry.key->equals(*key);
                })) {
                return false;
            }
        }
        return all_consumed(consumed);
    }

    static bool matches_dictionary(const CapturedDelta &actual,
                                   std::span<const literal::TimeSeriesLiteral> expected) {
        const auto &entries = std::get<std::vector<CapturedValues::Entry>>(actual.payload);
        const auto &runtime_type =
            static_cast<const bg::details::TimeSeriesDictionaryRuntimeType &>(*actual.runtime_type);
        auto memory = allocator{};
        auto consumed = std::vector<bool>(entries.size());
        for (const auto &value : expected) {
            const auto pair = bg::details::scalar_literal::pair(
                value, "a TSD update entry requires a key and a value");
            const auto value_marker = marker(*pair.second);
            if (value_marker == Marker::skip) {
                continue;
            }
            const auto key = ScalarValue<>::from_literal(runtime_type.key_runtime_type(), *pair.first, memory);
            if (value_marker == Marker::removed) {
                return false;
            }
            if (!consume_entry(entries, consumed, [&](const auto &entry) {
                    if (!entry.key || !entry.key->equals(key)) {
                        return false;
                    }
                    if (value_marker == Marker::remove) {
                        return entry.operation == CapturedDelta::Operation::remove;
                    }
                    const auto update = entry.operation == CapturedDelta::Operation::add ||
                                        entry.operation == CapturedDelta::Operation::update;
                    return update && matches_delta(entry.value, *pair.second);
                })) {
                return false;
            }
        }
        return all_consumed(consumed);
    }

    static bool matches_bundle(const CapturedDelta &actual, std::span<const literal::TimeSeriesLiteral> expected,
                               const TimeSeriesBundleType &bundle_type) {
        const auto &entries = std::get<std::vector<CapturedValues::Entry>>(actual.payload);
        auto consumed = std::vector<bool>(entries.size());
        for (const auto &value : expected) {
            const auto pair = bg::details::scalar_literal::pair(
                value, "a TSB update entry requires a field name and a value");
            if (marker(*pair.second) == Marker::skip) {
                continue;
            }
            const auto field_name = bg::details::scalar_literal::field_name(*pair.first);
            auto index = std::optional<std::size_t>{};
            for (auto field = std::size_t{}; field < bundle_type.item_count(); ++field) {
                if (bundle_type.item_name(field).text() == field_name) {
                    index = field;
                    break;
                }
            }
            if (!index || !consume_entry(entries, consumed, [&](const auto &entry) {
                    return entry.operation == CapturedDelta::Operation::update && !entry.key &&
                           entry.index == *index && matches_delta(entry.value, *pair.second);
                })) {
                return false;
            }
        }
        return all_consumed(consumed);
    }
};

inline auto literal_runtime_type(const TestGraphLiteralInput &input, const Signature &signature,
                                std::size_t parameter_index) -> const TimeSeriesRuntimeType & {
    if (input.runtime_type != nullptr) {
        return *input.runtime_type;
    }

    const auto *time_series_type = type_cast<TimeSeriesType>(signature.parameter(parameter_index).type);
    if (time_series_type == nullptr) {
        throw std::invalid_argument{"a generic test_graph literal input requires an explicit time-series type"};
    }

    if (time_series_type->is_resolved()) {
        return resolve_time_series_runtime_type(*time_series_type);
    }

    auto type_map = TypeMap{};
    if (const auto *guarantee = time_series_type->guarantee().variable()) {
        type_map.set(*guarantee, guarantee->minimum().value_or(TimeSeriesGuarantee::ordinary));
    }

    try {
        const auto &resolved_type = type_cast<TimeSeriesType>(time_series_type->resolve(type_map));
        return resolve_time_series_runtime_type(resolved_type);
    } catch (const TypeResolutionError &error) {
        if (error.code() != TypeResolutionErrorCode::unmapped_variable) {
            throw;
        }

        throw std::invalid_argument{"a generic test_graph literal input requires an explicit time-series type"};
    }
}

template <typename TimeSeries>
auto literal_runtime_type(const TestGraphLiteralInput &input) -> const TimeSeriesRuntimeType & {
    if (input.runtime_type != nullptr) {
        return *input.runtime_type;
    }

    if constexpr (bg::details::generic_time_series<TimeSeries>) {
        throw std::invalid_argument{"a generic test_graph literal input requires an explicit time-series type"};
    } else {
        return cpp_time_series_runtime_type<TimeSeries>();
    }
}

inline WiringPoint wire_literal_source(const TestGraphLiteralInput &input,
                                      const TimeSeriesRuntimeType &runtime_type, EvaluationTime start_time) {
    auto definition = LiteralSource{input, runtime_type, start_time};
    auto &wiring = WiringContext::current().wiring();
    const auto &source_template = wiring.own_node_template(
        std::make_unique<RuntimeSignatureNodeTemplate<LiteralSource>>(std::move(definition)));
    return *static_cast<const WiringTemplate &>(source_template)();
}

template <typename Function, typename Inputs, std::size_t... Index>
[[nodiscard]] auto run_function_literals(const Function &function, const GraphRunOptions &options,
                                         const Inputs &inputs,
                                         std::index_sequence<Index...>) {
    using FunctionInfo = test_function<Function>;
    const auto start_time = MIN_DT + EvaluationTime::duration{1};
    auto captured = CapturedValues{.start_time = start_time};
    auto cycle_count = std::size_t{1};
    for (const auto &input : inputs) {
        cycle_count = std::max(cycle_count, input.ticks.size());
    }
    captured.records.resize(cycle_count);

    auto wiring = GraphWiring{};
    {
        auto context = WiringContext{wiring};
        const auto sources = std::array<WiringPoint, sizeof...(Index)>{
            wire_literal_source(inputs[Index], literal_runtime_type<typename FunctionInfo::template input_time_series<Index>>(
                                                   inputs[Index]),
                                start_time)...};
        const auto output = std::invoke(
            function, typename FunctionInfo::template argument<Index>{sources[Index]}...);
        capture_output(output, captured);
    }

    return finish_test_graph(wiring, captured, start_time, options);
}

inline auto run_template_literals(const WiringTemplate &tested, const GraphRunOptions &options,
                                  std::span<const TestGraphLiteralInput> inputs) {
    if (tested.signature().has_variadic_parameters()) {
        throw std::invalid_argument{"test_graph literal inputs do not support variadic wiring templates"};
    }
    if (inputs.size() != tested.signature().parameter_count()) {
        throw std::invalid_argument{"test_graph literal input count does not match the wiring template"};
    }

    const auto start_time = MIN_DT + EvaluationTime::duration{1};
    auto captured = CapturedValues{.start_time = start_time};
    auto cycle_count = std::size_t{1};
    for (const auto &input : inputs) {
        cycle_count = std::max(cycle_count, input.ticks.size());
    }
    captured.records.resize(cycle_count);

    auto wiring = GraphWiring{};
    {
        auto context = WiringContext{wiring};
        auto sources = std::vector<WiringPoint>{};
        sources.reserve(inputs.size());
        for (auto index = std::size_t{}; index < inputs.size(); ++index) {
            sources.push_back(wire_literal_source(
                inputs[index], literal_runtime_type(inputs[index], tested.signature(), index), start_time));
        }

        const auto output = tested(std::span<const WiringPoint>{sources});
        if (output) {
            capture_output(*output, captured);
        }
    }

    return finish_test_graph(wiring, captured, start_time, options);
}

template <typename Function, typename Inputs, std::size_t... Index>
[[nodiscard]] auto run_function(const Function &function, const GraphRunOptions &options,
                                const Inputs &inputs,
                                std::index_sequence<Index...>) {
    using FunctionInfo = test_function<Function>;
    const auto start_time = MIN_DT + EvaluationTime::duration{1};
    auto captured = CapturedValues{.start_time = start_time};
    captured.records.resize(std::max({std::size_t{1}, std::ranges::size(std::get<Index>(inputs))...}));
    auto wiring = GraphWiring{};

    {
        auto context = WiringContext{wiring};
        [[maybe_unused]] const auto sources = std::tuple{
            wire_source<typename FunctionInfo::template input_scalar<Index>>(std::get<Index>(inputs), start_time)...};
        const auto output =
            std::invoke(function, typename FunctionInfo::template argument<Index>{std::get<Index>(sources)}...);
        capture_output(output, captured);
    }

    return finish_test_graph(wiring, captured, start_time, options);
}

template <std::ranges::contiguous_range... Inputs>
[[nodiscard]] auto run_template(const WiringTemplate &tested, const GraphRunOptions &options,
                                const Inputs &...inputs) {
    const auto start_time = MIN_DT + EvaluationTime::duration{1};
    auto captured = CapturedValues{.start_time = start_time};
    captured.records.resize(std::max({std::size_t{1}, std::ranges::size(inputs)...}));
    auto wiring = GraphWiring{};

    {
        auto context = WiringContext{wiring};
        const auto sources = std::array<WiringPoint, sizeof...(Inputs)>{
            wire_source<canonical_tick_t<std::ranges::range_value_t<Inputs>>>(inputs, start_time)...};
        const auto output = tested(std::span<const WiringPoint>{sources});

        if (output) {
            capture_output(*output, captured);
        }
    }

    return finish_test_graph(wiring, captured, start_time, options);
}

[[nodiscard]] inline auto run_runtime_template(const WiringTemplate &tested, const GraphRunOptions &options,
                                               std::span<const TestGraphInput> inputs) {
    const auto start_time = MIN_DT + EvaluationTime::duration{1};
    auto captured = CapturedValues{.start_time = start_time};
    auto cycle_count = std::size_t{1};
    auto wiring = GraphWiring{};

    {
        auto context = WiringContext{wiring};
        auto sources = std::vector<WiringPoint>{};
        sources.reserve(std::ranges::size(inputs));

        for (const auto &input : inputs) {
            cycle_count = std::max(cycle_count, input.ticks.size());

            auto definition = RuntimeSource{input, start_time};
            const auto &source_template = wiring.own_node_template(
                std::make_unique<RuntimeSignatureNodeTemplate<RuntimeSource>>(std::move(definition)));
            const auto arguments = std::array{
                WiringArgument::absent_scalar(input.runtime_type.type(), input.runtime_type),
            };
            sources.push_back(*static_cast<const WiringTemplate &>(source_template)(arguments));
        }

        const auto output = tested(std::span<const WiringPoint>{sources});
        if (output) {
            capture_output(*output, captured);
        }
    }

    captured.records.resize(cycle_count);

    return finish_test_graph(wiring, captured, start_time, options);
}

} // namespace bg::testing::details
