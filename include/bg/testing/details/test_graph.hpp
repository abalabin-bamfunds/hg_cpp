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
#include <bg/testing/time_series_tick_comparison.hpp>
#include <bg/type/type_map.hpp>
#include <bg/type/type_resolution_error.hpp>
#include <bg/time_series/details/scalar_literal.hpp>
#include <bg/time_series/tick_serializer.hpp>
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
  private:
    EvaluationTime start_time;
    util::buffer bytes;
    std::unique_ptr<util::buffer_reader> tail;
    const TimeSeriesType *type_{};

  public:
    explicit CapturedValues(EvaluationTime start_time)
        : start_time{start_time}, bytes{util::buffer::chain(128)}, tail{std::make_unique<util::buffer_reader>(bytes)} {}

    CapturedValues(CapturedValues &&other) noexcept
        : start_time{other.start_time}, bytes{std::move(other.bytes)}, tail{std::move(other.tail)},
          type_{std::exchange(other.type_, nullptr)}, records{std::move(other.records)} {}

    CapturedValues(const CapturedValues &) = delete;
    CapturedValues &operator=(const CapturedValues &) = delete;
    CapturedValues &operator=(CapturedValues &&) = delete;

    ~CapturedValues() {
        if (type_ != nullptr) {
            TimeSeriesTickReader reader{std::move(bytes), *type_};
        }
    }

    void set_type(const TimeSeriesType &type) noexcept { type_ = &type; }
    const TimeSeriesType *type() const noexcept { return type_; }

    util::buffer begin_tick() const { return tail->current_block(); }

    void set(EvaluationTime time, util::buffer bytes) {
        for (;;) {
            const auto readable = tail->readable();
            if (readable.empty()) {
                break;
            }
            tail->consume(readable.size());
        }

        const auto index = position(time);
        records[index].emplace(std::move(bytes));
    }

    std::vector<std::optional<util::buffer>> records;

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
        capture(invocation);
    }

    static void on_invalidation(Invocation invocation) { capture(invocation); }

  private:
    static void capture(Invocation invocation) {
        const auto input = invocation.input<In<TS_T>>(0);
        auto bytes = invocation.compiled_data().captured->begin_tick();
        auto writer = TimeSeriesTickWriter{bytes};
        if (writer.write(input, invocation.context().evaluation_time())) {
            invocation.compiled_data().captured->set(invocation.context().evaluation_time(), std::move(bytes));
        }
    }

    CapturedValues *captured_;
    Signature signature_;
};

inline void capture_output(WiringPoint output, CapturedValues &captured) {
    const auto &input_type = output.type().dereferenced_type();
    if (input_type.contains_reference()) {
        throw std::invalid_argument{"test_graph output capture does not support nested time-series references"};
    }

    captured.set_type(input_type);
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
    // Compare one nested child tick and advance past its complete payload on success.
    static bool matches_child(util::buffer_reader &reader, const TimeSeriesType &type,
                              const literal::TimeSeriesLiteral &expected) {
        try {
            const auto header = bg::details::TickHeader::read(reader);
            return header.operation == bg::details::TickOperation::tick &&
                   matches_payload(reader, type, header.valid, expected);
        } catch (const std::invalid_argument &) {
            return false;
        } catch (const std::out_of_range &) {
            return false;
        }
    }

    static bool matches_tick(const std::optional<util::buffer> &actual, const TimeSeriesType *type,
                             const literal::TimeSeriesLiteral &expected) {
        if (marker(expected) == Marker::skip) {
            return !actual;
        }
        if (!actual || type == nullptr) {
            return false;
        }
        auto reader = util::buffer_reader{*actual};
        return matches_child(reader, *type, expected);
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

    static bool matches_payload(util::buffer_reader &reader, const TimeSeriesType &type, bool valid,
                                const literal::TimeSeriesLiteral &expected) {
        const auto expected_marker = marker(expected);
        if (expected_marker == Marker::invalid) {
            return !valid;
        }
        if (expected_marker != Marker::value || !valid) {
            return false;
        }

        return type.visit(
            [&](const TimeSeriesValueType *value_type) { return matches_scalar(reader, *value_type, expected); },
            [&](const TimeSeriesListType *list_type) { return matches_list(reader, *list_type, expected); },
            [&](const TimeSeriesSetType *set_type) { return matches_set(reader, *set_type, expected); },
            [&](const TimeSeriesDictionaryType *dictionary_type) {
                return matches_dictionary(reader, *dictionary_type, expected);
            },
            [&](const TimeSeriesBundleType *bundle_type) {
                return matches_bundle(reader, *bundle_type, expected);
            },
            [&] {
                const auto *operations =
                    TimeSeriesDispatch<TickLiteralComparison>::instance().find(type);
                return operations != nullptr && operations->compare && operations->compare(reader, type, expected);
            }).value();
    }

    static bool matches_scalar(util::buffer_reader &reader, const TimeSeriesValueType &type,
                               const literal::TimeSeriesLiteral &expected) {
        auto matched = false;
        const auto read = bg::details::TimeSeriesTickScalarRead::visit(
            reader, type.value_type(), [&](Scalar<> actual) {
                auto memory = allocator{};
                const auto wanted = ScalarValue<>::from_literal(actual.runtime_type(), expected, memory);
                matched = actual.equals(wanted);
            });
        return read && matched;
    }

    static bool matches_list(util::buffer_reader &reader, const TimeSeriesListType &type,
                             const literal::TimeSeriesLiteral &expected_literal) {
        return matches_indexed(reader, type, expected_literal, "a TSL update entry requires an index and a value",
                               [&](const literal::TimeSeriesLiteral &selector, item_id_t index) {
                                   return bg::details::scalar_literal::list_index(selector) == index;
                               });
    }

    static bool matches_set(util::buffer_reader &reader, const TimeSeriesSetType &type,
                            const literal::TimeSeriesLiteral &expected_literal) {
        const auto expected = values(expected_literal);
        if (!expected) {
            return false;
        }

        auto matched = std::vector<bool>(expected->size());
        mark_skips(*expected, matched, [](const literal::TimeSeriesLiteral &entry) {
            return marker(entry) == Marker::skip;
        });
        for (;;) {
            const auto header = bg::details::TickHeader::read(reader);
            if (header.operation == bg::details::TickOperation::end) {
                return all_expected_matched(matched);
            }
            if (header.operation != bg::details::TickOperation::add &&
                header.operation != bg::details::TickOperation::remove) {
                return false;
            }

            auto candidate = std::optional<std::size_t>{};
            auto candidate_count = std::size_t{};
            const auto read = bg::details::TimeSeriesTickScalarRead::visit(
                reader, type.item_type(), [&](Scalar<> actual_key) {
                    for (auto expected_index = std::size_t{}; expected_index < expected->size(); ++expected_index) {
                        const auto &value = (*expected)[expected_index];
                        const auto value_marker = marker(value);
                        if (value_marker == Marker::skip) {
                            continue;
                        }

                        if (value_marker != Marker::value && value_marker != Marker::removed) {
                            continue;
                        }
                        const auto expected_operation = value_marker == Marker::removed
                                                            ? bg::details::TickOperation::remove
                                                            : bg::details::TickOperation::add;
                        if (expected_operation != header.operation) {
                            continue;
                        }

                        auto matches_key = false;
                        if (value_marker == Marker::value) {
                            matches_key = matches_scalar_literal(actual_key, value);
                        } else if (const auto *key_literal = removed_value(value)) {
                            matches_key = matches_scalar_literal(actual_key, *key_literal);
                        }
                        if (matches_key) {
                            candidate = expected_index;
                            ++candidate_count;
                        }
                    }
                });
            if (!read || candidate_count != 1 || matched[*candidate]) {
                return false;
            }
            matched[*candidate] = true;
        }
    }

    static bool matches_dictionary(util::buffer_reader &reader, const TimeSeriesDictionaryType &type,
                                   const literal::TimeSeriesLiteral &expected_literal) {
        const auto expected = values(expected_literal);
        if (!expected) {
            return false;
        }

        auto matched = std::vector<bool>(expected->size());
        mark_skips(*expected, matched, [](const literal::TimeSeriesLiteral &entry) {
            const auto pair = bg::details::scalar_literal::pair(
                entry, "a TSD update entry requires a key and a value");
            return marker(*pair.second) == Marker::skip;
        });
        for (;;) {
            const auto header = bg::details::TickHeader::read(reader);
            if (header.operation == bg::details::TickOperation::end) {
                return all_expected_matched(matched);
            }
            if (header.operation != bg::details::TickOperation::add &&
                header.operation != bg::details::TickOperation::tick &&
                header.operation != bg::details::TickOperation::remove) {
                return false;
            }

            auto candidate = std::optional<std::size_t>{};
            auto candidate_count = std::size_t{};
            const auto read = bg::details::TimeSeriesTickScalarRead::visit(
                reader, type.key_type(), [&](Scalar<> actual_key) {
                    for (auto expected_index = std::size_t{}; expected_index < expected->size(); ++expected_index) {
                        const auto pair = bg::details::scalar_literal::pair(
                            (*expected)[expected_index], "a TSD update entry requires a key and a value");
                        const auto value_marker = marker(*pair.second);
                        if (value_marker == Marker::skip || value_marker == Marker::removed ||
                            (header.operation == bg::details::TickOperation::remove) !=
                                (value_marker == Marker::remove)) {
                            continue;
                        }

                        auto memory = allocator{};
                        const auto wanted = ScalarValue<>::from_literal(actual_key.runtime_type(), *pair.first, memory);
                        if (actual_key.equals(wanted)) {
                            candidate = expected_index;
                            ++candidate_count;
                        }
                    }
                });
            if (!read || candidate_count != 1 || matched[*candidate]) {
                return false;
            }

            matched[*candidate] = true;
            const auto pair = bg::details::scalar_literal::pair(
                (*expected)[*candidate], "a TSD update entry requires a key and a value");
            if (header.operation != bg::details::TickOperation::remove &&
                !matches_payload(reader, type.value_type(), header.valid, *pair.second)) {
                return false;
            }
        }
    }

    static bool matches_bundle(util::buffer_reader &reader, const TimeSeriesBundleType &type,
                               const literal::TimeSeriesLiteral &expected_literal) {
        return matches_indexed(reader, type, expected_literal,
                               "a TSB update entry requires a field name and a value",
                               [&](const literal::TimeSeriesLiteral &selector, item_id_t index) {
                                   return bg::details::scalar_literal::field_name(selector) ==
                                          type.item_name(index).text();
                               });
    }

    template <typename SelectorMatches>
    static bool matches_indexed(util::buffer_reader &reader, const FixedItemTimeSeriesType &type,
                                const literal::TimeSeriesLiteral &expected_literal, const char *pair_error,
                                SelectorMatches &&selector_matches) {
        const auto expected = values(expected_literal);
        if (!expected) {
            return false;
        }

        auto matched = std::vector<bool>(expected->size());
        mark_skips(*expected, matched, [pair_error](const literal::TimeSeriesLiteral &entry) {
            const auto pair = bg::details::scalar_literal::pair(
                entry, pair_error);
            return marker(*pair.second) == Marker::skip;
        });
        for (;;) {
            const auto header = bg::details::TickHeader::read(reader);
            if (header.operation == bg::details::TickOperation::end) {
                return all_expected_matched(matched);
            }
            if (header.operation != bg::details::TickOperation::tick) {
                return false;
            }

            const auto index = reader.read_raw<item_id_t>();
            auto candidate = std::optional<std::size_t>{};
            const literal::TimeSeriesLiteral *child_literal{};
            for (auto expected_index = std::size_t{}; expected_index < expected->size(); ++expected_index) {
                const auto pair = bg::details::scalar_literal::pair(
                    (*expected)[expected_index], pair_error);
                if (marker(*pair.second) == Marker::skip || !std::invoke(selector_matches, *pair.first, index)) {
                    continue;
                }
                if (candidate) {
                    return false;
                }
                candidate = expected_index;
                child_literal = pair.second;
            }
            if (!candidate || matched[*candidate]) {
                return false;
            }
            matched[*candidate] = true;
            if (!matches_payload(reader, type.item_type(index), header.valid, *child_literal)) {
                return false;
            }
        }
    }

    static std::optional<std::span<const literal::TimeSeriesLiteral>> values(
        const literal::TimeSeriesLiteral &literal_value) {
        return literal_value.visit(
            [](const literal::TimeSeriesLiteralItem &)
                -> std::optional<std::span<const literal::TimeSeriesLiteral>> {
                return std::nullopt;
            },
            [](std::span<const literal::TimeSeriesLiteral> values)
                -> std::optional<std::span<const literal::TimeSeriesLiteral>> {
                return values;
            }).value();
    }

    static const literal::ScalarLiteral *removed_value(const literal::TimeSeriesLiteral &value) {
        auto result = static_cast<const literal::ScalarLiteral *>(nullptr);
        value.visit(
            [&](const literal::TimeSeriesLiteralItem &item) {
                item.visit_item(
                    [&](const literal::RemovedLiteral *removed) { result = removed->value; },
                    [](const auto *) {});
            },
            [](std::span<const literal::TimeSeriesLiteral>) {});
        return result;
    }

    template <typename Literal>
    static bool matches_scalar_literal(Scalar<> actual, const Literal &expected) {
        auto memory = allocator{};
        const auto wanted = ScalarValue<>::from_literal(actual.runtime_type(), expected, memory);
        return actual.equals(wanted);
    }

    template <typename Predicate>
    static void mark_skips(std::span<const literal::TimeSeriesLiteral> expected, std::vector<bool> &matched,
                           Predicate &&is_skip) {
        for (auto index = std::size_t{}; index < expected.size(); ++index) {
            matched[index] = std::invoke(is_skip, expected[index]);
        }
    }

    static bool all_expected_matched(const std::vector<bool> &matched) {
        return std::ranges::all_of(matched, [](bool value) { return value; });
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
    auto captured = CapturedValues{start_time};
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
    auto captured = CapturedValues{start_time};
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
    auto captured = CapturedValues{start_time};
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
    auto captured = CapturedValues{start_time};
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
    auto captured = CapturedValues{start_time};
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
