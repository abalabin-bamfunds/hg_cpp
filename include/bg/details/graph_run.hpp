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
#include <bg/realtime_evaluation_engine.hpp>
#include <bg/wall_clock.hpp>
#include <bg/graph_run_options.hpp>
#include <bg/graph_run_input.hpp>
#include <bg/testing/time_series_tick_comparison.hpp>
#include <bg/type/type_map.hpp>
#include <bg/type/type_resolution_error.hpp>
#include <bg/type/type_variable.hpp>
#include <bg/time_series/details/scalar_literal.hpp>
#include <bg/time_series/tick_serializer.hpp>

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
#include <stdexcept>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace bg::details {

struct SourceState {
    std::size_t next{};
    EvaluationTime::duration::rep start_time{};
};

struct CapturedValues {
  private:
    struct StoredEvent {
        EvaluationTime time;
        util::buffer bytes;
    };

    EvaluationTime start_time_;
    std::optional<util::buffer> bytes;
    std::unique_ptr<util::buffer_reader> tail;
    const TimeSeriesType *type_{};

  public:
    explicit CapturedValues(EvaluationTime start_time) : start_time_{start_time} {}

    CapturedValues(CapturedValues &&other) noexcept
        : start_time_{other.start_time_}, bytes{std::move(other.bytes)}, tail{std::move(other.tail)},
          type_{std::exchange(other.type_, nullptr)}, events_{std::move(other.events_)}, size_{other.size_},
          cycle_count_{other.cycle_count_} {}

    CapturedValues(const CapturedValues &) = delete;
    CapturedValues &operator=(const CapturedValues &) = delete;
    CapturedValues &operator=(CapturedValues &&) = delete;

    ~CapturedValues() {
        if (type_ != nullptr) {
            TimeSeriesTickReader reader{std::move(*bytes), *type_};
        }
    }

    void set_type(const TimeSeriesType &type) {
        auto tick_bytes = util::buffer::chain(128);
        auto tick_reader = std::make_unique<util::buffer_reader>(tick_bytes);
        bytes.emplace(std::move(tick_bytes));
        tail = std::move(tick_reader);
        type_ = &type;
        size_ = std::max(size_, cycle_count_);
    }
    const TimeSeriesType *type() const noexcept { return type_; }
    EvaluationTime start_time() const noexcept { return start_time_; }
    auto events() const noexcept {
        return events_ | std::views::transform([](const StoredEvent &event) { return event.time; });
    }
    std::size_t size() const noexcept { return size_; }
    void set_cycle_count(std::size_t count) noexcept {
        cycle_count_ = std::max(cycle_count_, count);
        if (type_ != nullptr) {
            size_ = std::max(size_, count);
        }
    }

    void set_start_time(EvaluationTime time) noexcept { start_time_ = time; }

    util::buffer begin_tick() const { return tail->current_block(); }

    void set(EvaluationTime time, util::buffer bytes) {
        for (;;) {
            const auto readable = tail->readable();
            if (readable.empty()) {
                break;
            }
            tail->consume(readable.size());
        }

        const auto index = static_cast<std::size_t>((time - start_time_).count());
        if (!events_.empty() && events_.back().time == time) {
            events_.back().bytes = std::move(bytes);
        } else {
            events_.push_back(StoredEvent{time, std::move(bytes)});
        }
        size_ = std::max(size_, index + 1);
    }

    const util::buffer *event(std::size_t index) const noexcept {
        const auto time = start_time_ + EvaluationTime::duration{static_cast<EvaluationTime::duration::rep>(index)};
        const auto event = std::ranges::lower_bound(events_, time, {}, &StoredEvent::time);
        return event != events_.end() && event->time == time ? std::addressof(event->bytes) : nullptr;
    }

    const util::buffer &event_data(std::size_t index) const { return events_.at(index).bytes; }

  private:
    std::vector<StoredEvent> events_;
    std::size_t size_{};
    std::size_t cycle_count_{};
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
          signature_{util::symbol{"graph_run_sink"}, {{.name = util::symbol{"input"}, .type = &input_type}}} {}

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
        throw std::invalid_argument{"run_graph output capture does not support nested time-series references"};
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
    };

    using compiled_data_type = CompiledData;
    using state_type = SourceState;
    using Invocation = NodeInvocation<CompiledData, state_type>;

    explicit RuntimeSource(const GraphRunInput &input)
        : input_{input},
          signature_{util::symbol{"graph_run_source"},
                     {{.name = util::symbol{"runtime_type"},
                       .type = &input.runtime_type.type(),
                       .accepts_absent_scalar = true}},
                     time_series_value_type(input.runtime_type.type(), TimeSeriesGuarantee::ordinary)} {}

    const Signature &signature() const noexcept { return signature_; }
    CompiledData compile(const ResolvedSignature &, allocator &) const noexcept { return {input_.ticks}; }

    static void start(Invocation invocation) {
        invocation.state().start_time = invocation.context().evaluation_time().time_since_epoch().count();
        publish(invocation);
    }
    static void evaluate(Invocation invocation) { publish(invocation); }

    static constexpr bool uses_timed_scheduling = true;

  private:
    static void publish(Invocation invocation) {
        const auto &data = invocation.compiled_data();
        auto &state = invocation.state();
        auto &next = state.next;

        if (next < data.ticks.size() && data.ticks[next]) {
            invocation.output().visit([&](Out<TS<SCALAR>> *output) { output->set_value(data.ticks[next]->view()); },
                                      ddv::noop);
        }

        ++next;
        while (next < data.ticks.size() && !data.ticks[next]) {
            ++next;
        }
        if (next < data.ticks.size()) {
            invocation.context().schedule_at(
                EvaluationTime{EvaluationTime::duration{state.start_time} + EvaluationTime::duration{next}});
        }
    }

    GraphRunInput input_;
    Signature signature_;
};

class LiteralSource {
  public:
    struct CompiledData { std::span<const literal::TimeSeriesLiteral> ticks; };

    using compiled_data_type = CompiledData;
    using state_type = SourceState;
    using Invocation = NodeInvocation<CompiledData, state_type>;
    static constexpr bool uses_timed_scheduling = true;
    static constexpr bool deduplicate_wiring = false;

    LiteralSource(const GraphRunLiteralInput &input, const TimeSeriesRuntimeType &runtime_type)
        : ticks_{input.ticks},
          runtime_type_{&resolve_time_series_runtime_type(
              runtime_type.type().with_guarantee_limit(TimeSeriesGuarantee::ordinary))},
          signature_{util::symbol{"graph_run_literal_source"}, {}, runtime_type_->type()} {}

    const Signature &signature() const noexcept { return signature_; }
    CompiledData compile(const ResolvedSignature &, allocator &) const noexcept { return {ticks_}; }
    const TimeSeriesRuntimeType &output_runtime_type(const WiringArguments &) const noexcept { return *runtime_type_; }

    static void start(Invocation invocation) {
        invocation.state().start_time = invocation.context().evaluation_time().time_since_epoch().count();
        publish(invocation);
    }
    static void evaluate(Invocation invocation) { publish(invocation); }

  private:
    static bool is_skip(const literal::TimeSeriesLiteral &tick) {
        const auto sentinel = literal::Sentinel::from(tick);
        return sentinel != nullptr && *sentinel == literal::SKIP;
    }

    static void publish(Invocation invocation) {
        const auto &data = invocation.compiled_data();
        auto &state = invocation.state();
        auto &next = state.next;

        if (next < data.ticks.size() &&
            invocation.context().evaluation_time() ==
                EvaluationTime{EvaluationTime::duration{state.start_time} + EvaluationTime::duration{next}}) {
            invocation.output().apply(data.ticks[next]);
            ++next;
        }

        while (next < data.ticks.size() && is_skip(data.ticks[next])) {
            ++next;
        }
        if (next < data.ticks.size()) {
            invocation.context().schedule_at(
                EvaluationTime{EvaluationTime::duration{state.start_time} + EvaluationTime::duration{next}});
        }
    }

    std::span<const literal::TimeSeriesLiteral> ticks_;
    const TimeSeriesRuntimeType *runtime_type_;
    Signature signature_;
};

template <typename T> struct optional_value : std::false_type {};
template <typename T> struct optional_value<std::optional<T>> : std::true_type {};

template <typename TimeSeries> struct graph_output_scalar {
    using type = void;
};

template <typename T, auto... Properties> struct graph_output_scalar<TS<T, Properties...>> {
    using type = T;
};

template <typename Target, auto... Properties>
struct graph_output_scalar<REF<Target, Properties...>> : graph_output_scalar<Target> {};

template <typename Result, bool = bg::details::is_typed_wiring_point<Result>> struct graph_result_scalar {
    using type = void;
};

template <typename Result> struct graph_result_scalar<Result, true> {
    using type = typename graph_output_scalar<typename bg::details::typed_wiring_point<Result>::time_series_type>::type;
};

template <typename Function> struct graph_function {
    using callable = bg::details::callable<std::remove_cvref_t<Function>>;
    using result = std::remove_cvref_t<typename callable::result_type>;
    using output_scalar = typename graph_result_scalar<result>::type;

    template <std::size_t Index> using argument = std::remove_cvref_t<typename callable::template argument_type<Index>>;
};

template <typename Graph>
concept graph_run_target = std::derived_from<std::remove_cvref_t<Graph>, WiringTemplate> ||
                           requires { typename graph_function<Graph>::callable; };

inline auto finish_graph_run(GraphWiring &wiring, CapturedValues &captured, const GraphRunOptions &options) {
    if (options.services != nullptr) {
        wiring.finalize(*options.services);
    } else {
        wiring.finalize();
    }

    const auto compiled = CompiledGraph{wiring};
    const auto default_start_time = options.engine == EvaluationEngine::realtime
                                        ? SystemWallClock::instance().now()
                                        : MIN_DT + EvaluationTime::duration{1};
    const auto start_time = options.start_time.value_or(default_start_time);
    captured.set_start_time(start_time);
    const auto stop_time = options.resolve_stop_time(start_time);
    auto run = [&](GlobalState &global_state) {
        auto runtime = RuntimeGraph{compiled, global_state};
        if (options.recorder) {
            options.recorder->run(runtime, start_time, options.engine, options.stop_token, options.periodic_callback,
                                  stop_time);
        } else if (options.engine == EvaluationEngine::realtime) {
            RealtimeEvaluationEngine{}.run(runtime, start_time, options.stop_token, options.periodic_callback,
                                           stop_time);
        } else {
            SimulationEvaluationEngine{}.run(runtime, start_time, options.periodic_callback, stop_time);
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
  private:
    class Comparison {
      public:
        Comparison &indexed_child(std::size_t index) {
            if (indexed_children_.size() <= index) {
                indexed_children_.resize(index + 1);
            }
            if (!indexed_children_[index]) {
                indexed_children_[index] = std::make_unique<Comparison>();
            }
            return *indexed_children_[index];
        }

        Comparison &dictionary_child(const Scalar<> &key) {
            const auto child = dictionary_children_.find(key);
            if (child != dictionary_children_.end()) {
                return *child->second;
            }

            auto value = std::make_unique<Comparison>();
            const auto inserted = dictionary_children_.emplace(ScalarValue<>{key}, std::move(value)).first;
            return *inserted->second;
        }

        bool retire_dictionary_child(const Scalar<> &key) {
            const auto child = dictionary_children_.find(key);
            if (child == dictionary_children_.end()) {
                return true;
            }

            const auto matched = child->second->finish();
            dictionary_children_.erase(child);
            return matched;
        }

        void invalidate(std::size_t tick_index) {
            if (extension_state_) {
                extension_state_->invalidate(tick_index);
            }
            for (const auto &child : indexed_children_) {
                if (child) {
                    child->invalidate(tick_index);
                }
            }
            for (const auto &[key, child] : dictionary_children_) {
                child->invalidate(tick_index);
            }
        }

        bool finish() {
            if (extension_state_ && !extension_state_->finish()) {
                return false;
            }
            for (const auto &child : indexed_children_) {
                if (child && !child->finish()) {
                    return false;
                }
            }
            for (const auto &[key, child] : dictionary_children_) {
                if (!child->finish()) {
                    return false;
                }
            }
            return true;
        }

        testing::TickLiteralComparisonState *extension_state(const testing::TickLiteralComparison &operations) {
            if (!extension_state_) {
                extension_state_ = operations.create_state();
            }
            return extension_state_.get();
        }

      private:
        std::unique_ptr<testing::TickLiteralComparisonState> extension_state_;
        std::vector<std::unique_ptr<Comparison>> indexed_children_;
        std::unordered_map<ScalarValue<>, std::unique_ptr<Comparison>, ScalarHash, ScalarEqual> dictionary_children_;
    };

  public:
    static bool matches_sequence(const CapturedValues &actual, const TimeSeriesType *type,
                                 std::span<const literal::TimeSeriesLiteral> expected) {
        if (actual.size() != expected.size()) {
            return false;
        }

        try {
            auto comparison = Comparison{};
            for (auto index = std::size_t{}; index < expected.size(); ++index) {
                if (!matches_tick(actual.event(index), type, expected[index], index, comparison)) {
                    return false;
                }
            }
            return comparison.finish();
        } catch (const std::invalid_argument &) {
            return false;
        } catch (const std::out_of_range &) {
            return false;
        }
    }

    static bool matches_sequence(std::span<const std::optional<util::buffer>> actual,
                                 const TimeSeriesType *type,
                                 std::span<const literal::TimeSeriesLiteral> expected) {
        if (actual.size() != expected.size()) {
            return false;
        }

        try {
            auto comparison = Comparison{};
            for (auto index = std::size_t{}; index < expected.size(); ++index) {
                const auto *event = actual[index] ? std::addressof(*actual[index]) : nullptr;
                if (!matches_tick(event, type, expected[index], index, comparison)) {
                    return false;
                }
            }
            return comparison.finish();
        } catch (const std::invalid_argument &) {
            return false;
        } catch (const std::out_of_range &) {
            return false;
        }
    }

    // Compare one nested child tick and advance past its complete payload on success.
    static bool matches_child(util::buffer_reader &reader, const TimeSeriesType &type,
                              const literal::TimeSeriesLiteral &expected) {
        try {
            auto comparison = Comparison{};
            return matches_child(reader, type, expected, 0, comparison) && comparison.finish();
        } catch (const std::invalid_argument &) {
            return false;
        } catch (const std::out_of_range &) {
            return false;
        }
    }

    static bool matches_tick(const util::buffer *actual, const TimeSeriesType *type,
                             const literal::TimeSeriesLiteral &expected) {
        auto comparison = Comparison{};
        return matches_tick(actual, type, expected, 0, comparison) && comparison.finish();
    }

    static bool matches_tick(const std::optional<util::buffer> &actual, const TimeSeriesType *type,
                             const literal::TimeSeriesLiteral &expected) {
        return matches_tick(actual ? std::addressof(*actual) : nullptr, type, expected);
    }

  private:
    static bool is_sentinel(const literal::ScalarLiteral &value, const literal::Sentinel &expected) {
        const auto *sentinel = literal::Sentinel::from(value);
        return sentinel != nullptr && *sentinel == expected;
    }

    static bool matches_tick(const util::buffer *actual, const TimeSeriesType *type,
                             const literal::TimeSeriesLiteral &expected, std::size_t tick_index,
                             Comparison &comparison) {
        if (is_sentinel(expected, literal::SKIP)) {
            return actual == nullptr;
        }
        if (actual == nullptr || type == nullptr) {
            return false;
        }

        auto reader = util::buffer_reader{*actual};
        return matches_child(reader, *type, expected, tick_index, comparison);
    }

    static bool matches_child(util::buffer_reader &reader, const TimeSeriesType &type,
                              const literal::TimeSeriesLiteral &expected, std::size_t tick_index,
                              Comparison &comparison) {
        const auto header = bg::details::TickHeader::read(reader);
        return header.operation == bg::details::TickOperation::tick &&
               matches_payload(reader, type, header.valid, expected, comparison, tick_index);
    }

    static bool matches_payload(util::buffer_reader &reader, const TimeSeriesType &type, bool valid,
                                const literal::TimeSeriesLiteral &expected, Comparison &node,
                                std::size_t tick_index) {
        if (is_sentinel(expected, literal::INVALID)) {
            if (valid) {
                return false;
            }
            node.invalidate(tick_index);
            return true;
        }
        if (!valid || is_sentinel(expected, literal::SKIP)) {
            return false;
        }

        return type.visit(
            [&](const TimeSeriesValueType *value_type) { return matches_scalar(reader, *value_type, expected); },
            [&](const TimeSeriesListType *list_type) {
                return matches_list(reader, *list_type, expected, node, tick_index);
            },
            [&](const TimeSeriesSetType *set_type) { return matches_set(reader, *set_type, expected); },
            [&](const TimeSeriesDictionaryType *dictionary_type) {
                return matches_dictionary(reader, *dictionary_type, expected, node, tick_index);
            },
            [&](const TimeSeriesBundleType *bundle_type) {
                return matches_bundle(reader, *bundle_type, expected, node, tick_index);
            },
            [&] {
                const auto *operations =
                    TimeSeriesDispatch<testing::TickLiteralComparison>::instance().find(type);
                if (operations == nullptr) {
                    return false;
                }
                if (operations->create_state) {
                    auto *state = node.extension_state(*operations);
                    return state != nullptr && state->compare(reader, type, expected, tick_index);
                }
                return operations->compare && operations->compare(reader, type, expected);
            }).value();
    }

    static bool matches_scalar(util::buffer_reader &reader, const TimeSeriesValueType &type,
                               const literal::TimeSeriesLiteral &expected) {
        auto matched = false;
        const auto read = bg::details::TimeSeriesTickScalarRead::visit(
            reader, type.value_type(), [&](Scalar<> actual) {
                matched = matches_scalar_literal(actual, expected);
            });
        return read && matched;
    }

    static bool matches_list(util::buffer_reader &reader, const TimeSeriesListType &type,
                             const literal::TimeSeriesLiteral &expected_literal, Comparison &node,
                             std::size_t tick_index) {
        return matches_indexed(reader, type, expected_literal, "a TSL update entry requires an index and a value",
                               [&](const literal::TimeSeriesLiteral &selector, item_id_t index) {
                                   return bg::details::scalar_literal::list_index(selector) == index;
                               },
                               node, tick_index);
    }

    static bool matches_set(util::buffer_reader &reader, const TimeSeriesSetType &type,
                            const literal::TimeSeriesLiteral &expected_literal) {
        const auto expected = bg::details::scalar_literal::sequence(expected_literal);

        auto matched = std::vector<bool>(expected.size());
        mark_skips(expected, matched, [](const literal::TimeSeriesLiteral &entry) {
            return is_sentinel(entry, literal::SKIP);
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
                    for (auto expected_index = std::size_t{}; expected_index < expected.size(); ++expected_index) {
                        const auto &value = expected[expected_index];
                        if (is_sentinel(value, literal::SKIP)) {
                            continue;
                        }

                        const auto removed = literal::TaggedLiteral::from(value);
                        const auto is_removal = removed && removed->sentinel == literal::REMOVED;
                        const auto expected_operation = is_removal ? bg::details::TickOperation::remove
                                                                   : bg::details::TickOperation::add;
                        if (expected_operation != header.operation) {
                            continue;
                        }

                        const auto &key_literal = is_removal ? removed->argument : value;
                        if (matches_scalar_literal(actual_key, key_literal)) {
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
                                   const literal::TimeSeriesLiteral &expected_literal, Comparison &node,
                                   std::size_t tick_index) {
        const auto expected = bg::details::scalar_literal::sequence(expected_literal);

        auto matched = std::vector<bool>(expected.size());
        mark_skips(expected, matched, [](const literal::TimeSeriesLiteral &entry) {
            const auto pair = bg::details::scalar_literal::pair(
                entry, "a TSD update entry requires a key and a value");
            return is_sentinel(*pair.second, literal::SKIP);
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
            auto matched_entry = false;
            auto matched_value = false;
            const auto read = bg::details::TimeSeriesTickScalarRead::visit(
                reader, type.key_type(), [&](Scalar<> actual_key) {
                    for (auto expected_index = std::size_t{}; expected_index < expected.size(); ++expected_index) {
                        const auto pair = bg::details::scalar_literal::pair(
                            expected[expected_index], "a TSD update entry requires a key and a value");
                        if (is_sentinel(*pair.second, literal::SKIP)) {
                            continue;
                        }
                        const auto expected_removal = is_sentinel(*pair.second, literal::REMOVE);
                        if (expected_removal != (header.operation == bg::details::TickOperation::remove)) {
                            continue;
                        }

                        if (matches_scalar_literal(actual_key, *pair.first)) {
                            candidate = expected_index;
                            ++candidate_count;
                        }
                    }

                    if (candidate_count != 1 || matched[*candidate]) {
                        return;
                    }

                    matched_entry = true;
                    matched[*candidate] = true;
                    const auto pair = bg::details::scalar_literal::pair(
                        expected[*candidate], "a TSD update entry requires a key and a value");
                    if (header.operation == bg::details::TickOperation::remove) {
                        matched_value = node.retire_dictionary_child(actual_key);
                        return;
                    }
                    if (header.operation == bg::details::TickOperation::add &&
                        !node.retire_dictionary_child(actual_key)) {
                        return;
                    }
                    matched_value = matches_payload(reader, type.value_type(), header.valid, *pair.second,
                                                    node.dictionary_child(actual_key), tick_index);
                });
            if (!read || !matched_entry || !matched_value) {
                return false;
            }
        }
    }

    static bool matches_bundle(util::buffer_reader &reader, const TimeSeriesBundleType &type,
                               const literal::TimeSeriesLiteral &expected_literal, Comparison &node,
                               std::size_t tick_index) {
        return matches_indexed(reader, type, expected_literal,
                               "a TSB update entry requires a field name and a value",
                               [&](const literal::TimeSeriesLiteral &selector, item_id_t index) {
                                   return bg::details::scalar_literal::field_name(selector) ==
                                          type.item_name(index).text();
                               },
                               node, tick_index);
    }

    template <typename SelectorMatches>
    static bool matches_indexed(util::buffer_reader &reader, const FixedItemTimeSeriesType &type,
                                const literal::TimeSeriesLiteral &expected_literal, const char *pair_error,
                                SelectorMatches &&selector_matches, Comparison &node,
                                std::size_t tick_index) {
        const auto expected = bg::details::scalar_literal::sequence(expected_literal);

        auto matched = std::vector<bool>(expected.size());
        mark_skips(expected, matched, [pair_error](const literal::TimeSeriesLiteral &entry) {
            const auto pair = bg::details::scalar_literal::pair(
                entry, pair_error);
            return is_sentinel(*pair.second, literal::SKIP);
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
            for (auto expected_index = std::size_t{}; expected_index < expected.size(); ++expected_index) {
                const auto pair = bg::details::scalar_literal::pair(
                    expected[expected_index], pair_error);
                if (is_sentinel(*pair.second, literal::SKIP) ||
                    !std::invoke(selector_matches, *pair.first, index)) {
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
            const auto &child_type = type.item_type(index);
            if (!matches_payload(reader, child_type, header.valid, *child_literal, node.indexed_child(index),
                                 tick_index)) {
                return false;
            }
        }
    }

    static bool matches_scalar_literal(Scalar<> actual, const literal::ScalarLiteral &expected) {
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

inline WiringPoint wire_literal_source(const GraphRunLiteralInput &input,
                                      const TimeSeriesRuntimeType &runtime_type) {
    auto definition = LiteralSource{input, runtime_type};
    auto &wiring = WiringContext::current().wiring();
    const auto &source_template = wiring.own_node_template(
        std::make_unique<RuntimeSignatureNodeTemplate<LiteralSource>>(std::move(definition)));
    return *static_cast<const WiringTemplate &>(source_template)();
}

inline WiringPoint wire_runtime_source(const GraphRunInput &input) {
    auto definition = RuntimeSource{input};
    auto &wiring = WiringContext::current().wiring();
    const auto &source_template = wiring.own_node_template(
        std::make_unique<RuntimeSignatureNodeTemplate<RuntimeSource>>(std::move(definition)));
    const auto arguments = std::array{
        WiringArgument::absent_scalar(input.runtime_type.type(), input.runtime_type),
    };
    return *static_cast<const WiringTemplate &>(source_template)(arguments);
}

template <typename Output> void capture_graph_output(const Output &output, CapturedValues &captured) {
    if constexpr (optional_value<std::remove_cvref_t<Output>>::value) {
        if (output) {
            capture_output(static_cast<WiringPoint>(*output), captured);
        }
    } else {
        capture_output(static_cast<WiringPoint>(output), captured);
    }
}

} // namespace bg::details
