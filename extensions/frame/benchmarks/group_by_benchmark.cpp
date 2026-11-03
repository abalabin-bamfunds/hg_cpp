#include <hg/frame/frame.hpp>
#include <hg/graph/compiled_graph.hpp>
#include <hg/graph/details/compiled_graph_builder.hpp>
#include <hg/graph/graph_wiring.hpp>
#include <hg/graph/runtime_graph.hpp>
#include <hg/graph/wiring_context.hpp>
#include <hg/evaluation_profile.hpp>
#include <hg/evaluation_profiler.hpp>
#include <hg/library.hpp>
#include <hg/node.hpp>
#include <hg/util/scope_exit.hpp>

#ifdef HG_FRAME_GROUP_BY_ARROW_BENCHMARK
#include <hg/frame/arrow.hpp>

#include <arrow/acero/exec_plan.h>
#include <arrow/acero/options.h>
#include <arrow/api.h>
#include <arrow/compute/api.h>
#include <arrow/compute/cast.h>
#include <arrow/compute/initialize.h>
#include <arrow/config.h>
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <numeric>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace {

using namespace hg;
using namespace hg::frame;
using namespace hg::literals;

constexpr auto source_rows = std::size_t{100'000};
constexpr auto group_count = std::size_t{100};
constexpr auto mutation_stride = std::size_t{7'919};
constexpr auto warmup_cycles = std::size_t{10};
constexpr auto measured_cycles = std::size_t{101};
constexpr auto repeats = std::size_t{3};
constexpr auto cycles = warmup_cycles + measured_cycles;

enum class Mutation { value_update, key_move, remove_add };

std::string_view workload_name(Mutation operation, bool scaled) {
    if (scaled)
        return "scalar_parameter";
    switch (operation) {
    case Mutation::value_update:
        return "value_update";
    case Mutation::key_move:
        return "key_move";
    case Mutation::remove_add:
        return "remove_add";
    }
    throw std::runtime_error{"group-by benchmark has an unknown mutation"};
}

template <template <typename, auto...> typename Field> struct Rows {
    Field<i64> id;
    Field<i64> bucket;
    Field<i64> value;

    static SchemaMetadata metadata() { return key_metadata<"id">(); }
};

struct CycleClock {
    static constexpr bool uses_timed_scheduling = true;

    State<i64> value;
    Out<TS<i64>> output;

    void start(EvaluationContext *context) const {
        *value = 0;
        output.set(0);
        context->schedule_at(context->evaluation_time() + EvaluationTime::duration{1});
    }

    void evaluate(EvaluationContext *context) const {
        output.set(++*value);
        context->schedule_at(context->evaluation_time() + EvaluationTime::duration{1});
    }
};

struct SourceState {
    std::array<row_id_t, source_rows> row_ids{};
    std::array<i64, source_rows> buckets{};
    std::array<i64, source_rows> values{};
    i64 next_id{};
};

template <std::size_t ChangeCount, std::size_t GroupCount, Mutation Operation> struct FrameSource {
    In<TS<i64>> cycle;
    State<SourceState> state;
    Out<TSF<Rows>> output;

    void start() const {
        state->values.fill(1);
        state->next_id = static_cast<i64>(source_rows);
        for (auto row = std::size_t{}; row != source_rows; ++row) {
            const auto bucket = static_cast<i64>(row % GroupCount);
            state->buckets[row] = bucket;
            state->row_ids[row] = output.add(Row<Rows>{.id = Cell<i64>{static_cast<i64>(row)},
                                                        .bucket = Cell<i64>{bucket},
                                                        .value = Cell<i64>{1}});
        }
    }

    void evaluate() const {
        if (*cycle == 0)
            return;

        const auto first = static_cast<std::size_t>(*cycle - 1) * mutation_stride;
        for (auto change = std::size_t{}; change != ChangeCount; ++change) {
            const auto row = (first + change) % source_rows;
            if constexpr (Operation == Mutation::value_update) {
                const auto value = ++state->values[row];
                output.set(state->row_ids[row], RowPatch<Rows>{.value = PatchCell<i64>{value}});
            } else if constexpr (Operation == Mutation::key_move) {
                const auto bucket = static_cast<i64>((state->buckets[row] + 1) % GroupCount);
                state->buckets[row] = bucket;
                output.set(state->row_ids[row], RowPatch<Rows>{.bucket = PatchCell<i64>{bucket}});
            } else {
                static_cast<void>(output.erase(state->row_ids[row]));
                state->row_ids[row] = output.add(Row<Rows>{.id = Cell<i64>{state->next_id++},
                                                            .bucket = Cell<i64>{state->buckets[row]},
                                                            .value = Cell<i64>{state->values[row]}});
            }
        }
    }
};

template <typename Definition> const auto &node_template() {
    static const auto value = node<Definition>();
    return value;
}

struct MultiplierSource {
    In<TS<i64>> cycle;
    Out<TS<double>> output;

    void start() const { output.set(1.0); }
    void evaluate() const {
        if (*cycle != 0)
            output.set(static_cast<double>(*cycle + 1));
    }
};

template <std::size_t ChangeCount, Mutation Operation, bool Multiple, bool Scaled> struct ResultSink {
    In<TS<i64>> cycle;
    In<TSF<RESULT>> result;

    inline static bool capture{};
    inline static std::map<i64, i64> totals;
    inline static std::map<i64, double> scaled_totals;
    inline static std::map<i64, double> second_totals;
    inline static std::size_t result_size{};

    void evaluate() const {
        result_size = result.size();
        if (!capture)
            return;

        totals.clear();
        scaled_totals.clear();
        second_totals.clear();
        const auto buckets = result.column("bucket").column_view();
        const auto values = result.column("total").column_view();
        auto second = std::optional<ColumnView<>>{};
        if constexpr (Multiple)
            second = result.column("second_total").column_view();

        result.for_each_row([&](row_id_t, std::size_t position) {
            const auto bucket = buckets.cell<i64>(position);
            if (!bucket)
                throw std::runtime_error{"group-by benchmark produced an invalid key"};
            if constexpr (Scaled) {
                const auto value = values.cell<double>(position);
                if (!value)
                    throw std::runtime_error{"group-by benchmark produced an invalid scaled sum"};
                scaled_totals[*bucket] = *value;
            } else {
                const auto value = values.cell<i64>(position);
                if (!value)
                    throw std::runtime_error{"group-by benchmark produced an invalid sum"};
                totals[*bucket] = *value;
            }
            if constexpr (Multiple) {
                const auto value = second->cell<double>(position);
                if (!value)
                    throw std::runtime_error{"group-by benchmark produced an invalid second sum"};
                second_totals[*bucket] = *value;
            }
        });
    }
};

double milliseconds(std::chrono::steady_clock::duration duration) {
    return std::chrono::duration<double, std::milli>{duration}.count();
}

std::pair<double, double> summarize(std::vector<double> samples) {
    std::ranges::sort(samples);
    const auto p95_index = (samples.size() * 95 + 99) / 100 - 1;
    return {samples[samples.size() / 2], samples[p95_index]};
}

std::pair<std::uint64_t, std::uint64_t> summarize_ticks(std::vector<std::uint64_t> samples) {
    std::ranges::sort(samples);
    const auto p95_index = (samples.size() * 95 + 99) / 100 - 1;
    return {samples[samples.size() / 2], samples[p95_index]};
}

std::string_view profile_clock_name(EvaluationProfileClock clock) {
    switch (clock) {
    case EvaluationProfileClock::timestamp_counter:
        return "timestamp_counter";
    case EvaluationProfileClock::steady_clock_nanoseconds:
        return "steady_clock_nanoseconds";
    }
    throw std::runtime_error{"group-by benchmark profile has an unknown checkpoint clock"};
}

struct ProfileCycleAnchor {
    EvaluationTime evaluation_time;
    EvaluationProfileCycle::Boundary boundary;
    std::uint64_t checkpoint{};
    std::int64_t steady_clock_nanoseconds{};
};

template <std::size_t ChangeCount, std::size_t GroupCount, Mutation Operation, bool Multiple, bool Scaled>
struct CaseGraph {
    using Sink = ResultSink<ChangeCount, Operation, Multiple, Scaled>;

    std::shared_ptr<const CompiledGraph> compiled;
    std::shared_ptr<const CompiledGraph> profile_compiled;
    std::size_t group_node_index{};

    CaseGraph() {
        auto wiring = GraphWiring{};
        auto group_result = std::optional<WiringPoint>{};
        {
            const auto context = WiringContext{wiring};
            const auto cycle = node_template<CycleClock>()();
            const auto source = node_template<FrameSource<ChangeCount, GroupCount, Operation>>()(cycle);
            auto grouped = frame::group_by(source, "bucket");
            if constexpr (Scaled) {
                const auto multiplier = node_template<MultiplierSource>()(cycle);
                const auto result = grouped.agg(
                    "total"_n = operators::sum_(frame::convert<double>(col("value")) * multiplier));
                group_result = result;
                static_cast<void>(node_template<Sink>()(cycle, result));
            } else if constexpr (Multiple) {
                const auto result = grouped.agg("total"_n = operators::sum_(col("value")),
                                                "second_total"_n = operators::sum_(
                                                    frame::convert<double>(col("value"))));
                group_result = result;
                static_cast<void>(node_template<Sink>()(cycle, result));
            } else {
                const auto result = grouped.agg("total"_n = operators::sum_(col("value")));
                group_result = result;
                static_cast<void>(node_template<Sink>()(cycle, result));
            }
        }
        wiring.finalize();
        if (!group_result)
            throw std::runtime_error{"group-by benchmark has no aggregation result"};
        const auto concrete_result = wiring.materialize(*group_result, group_result->type());
        compiled = std::make_shared<CompiledGraph>(wiring);
        auto indices = std::array{wiring.output_node_index(concrete_result)};
        profile_compiled = hg::details::compile_graph(wiring, allocator{}, indices);
        group_node_index = indices.front();
    }
};

template <std::size_t ChangeCount, std::size_t GroupCount, Mutation Operation, bool Multiple, bool Scaled>
void validate_case(const CompiledGraph &compiled);

template <std::size_t ChangeCount, std::size_t GroupCount, Mutation Operation, bool Multiple, bool Scaled>
void profile_case() {
    using Graph = CaseGraph<ChangeCount, GroupCount, Operation, Multiple, Scaled>;
    using Sink = typename Graph::Sink;
    const auto graph = Graph{};
    validate_case<ChangeCount, GroupCount, Operation, Multiple, Scaled>(*graph.compiled);
    auto node_ticks = std::vector<std::uint64_t>{};
    auto node_milliseconds = std::vector<double>{};
    auto profiled_cycle_milliseconds = std::vector<double>{};
    auto ticks_per_millisecond_samples = std::vector<double>{};
    node_ticks.reserve(repeats * measured_cycles);
    node_milliseconds.reserve(repeats * measured_cycles);
    profiled_cycle_milliseconds.reserve(repeats * measured_cycles);
    ticks_per_millisecond_samples.reserve(repeats);
    auto profile_clock = EvaluationProfileClock::steady_clock_nanoseconds;
    const auto start_time = MIN_DT + EvaluationTime::duration{1};

    for (auto repeat = std::size_t{}; repeat != repeats; ++repeat) {
        Sink::capture = false;
        Sink::result_size = 0;
        auto global_state = GlobalState{};
        auto runtime = RuntimeGraph{*graph.profile_compiled, global_state};
        auto profile = EvaluationProfile{};
        auto reader = EvaluationProfileReader{profile.take_recording()};
        auto profiler = EvaluationProfiler{};
        auto cycle_anchors = std::vector<ProfileCycleAnchor>{};
        const auto read_profile = [&] {
            while (auto record = reader.read()) {
                auto cycle = std::optional<EvaluationProfileCycle>{};
                if (const auto *value = std::get_if<EvaluationProfileCycle>(&*record))
                    cycle = *value;
                profiler.consume(std::move(*record));
                if (cycle && profiler.root_graph_id() == cycle->graph_id)
                    cycle_anchors.push_back({cycle->evaluation_time, cycle->boundary, cycle->checkpoint,
                                             cycle->steady_clock_nanoseconds});
            }
        };
        if (repeat == 0)
            profile_clock = reader.clock();
        else if (reader.clock() != profile_clock)
            throw std::runtime_error{"group-by benchmark profile clock changed between repeats"};

        runtime.start(start_time, profile);
        read_profile();
        auto stop_on_exit = util::scope_exit{[&]() noexcept { runtime.stop(profile); }};
        runtime.evaluate(start_time, profile);
        read_profile();
        const auto graph_id = profiler.root_graph_id();
        if (!graph_id)
            throw std::runtime_error{"group-by benchmark profile has no root graph"};
        auto previous = profiler.node_totals(*graph_id, static_cast<std::uint32_t>(graph.group_node_index));
        if (!previous)
            throw std::runtime_error{"group-by benchmark profile has no group-by node totals"};
        const auto first_sample = node_ticks.size();

        for (auto cycle = std::size_t{1}; cycle <= cycles; ++cycle) {
            runtime.evaluate(start_time + EvaluationTime::duration{static_cast<EvaluationTime::duration::rep>(cycle)},
                              profile);
            read_profile();
            const auto current = profiler.node_totals(*graph_id, static_cast<std::uint32_t>(graph.group_node_index));
            if (!current || current->evaluations != previous->evaluations + 1 ||
                current->checkpoint_ticks < previous->checkpoint_ticks)
                throw std::runtime_error{"group-by benchmark node did not evaluate once per cycle"};
            if (cycle > warmup_cycles)
                node_ticks.push_back(current->checkpoint_ticks - previous->checkpoint_ticks);
            previous = current;
        }

        runtime.stop(profile);
        stop_on_exit.release();
        profile.finish();
        read_profile();
        if (profile.failed())
            throw std::runtime_error{"group-by benchmark evaluation profile recording failed"};

        const auto first_time = start_time +
                                EvaluationTime::duration{static_cast<EvaluationTime::duration::rep>(warmup_cycles + 1)};
        const auto last_time = start_time +
                               EvaluationTime::duration{static_cast<EvaluationTime::duration::rep>(cycles)};
        const auto find_anchor = [&](EvaluationTime time, EvaluationProfileCycle::Boundary boundary) {
            const auto found = std::ranges::find_if(cycle_anchors, [&](const auto &anchor) {
                return anchor.evaluation_time == time && anchor.boundary == boundary;
            });
            if (found == cycle_anchors.end())
                throw std::runtime_error{"group-by benchmark profile is missing a cycle calibration anchor"};
            return *found;
        };
        const auto first_anchor = find_anchor(first_time, EvaluationProfileCycle::Boundary::start);
        const auto last_anchor = find_anchor(last_time, EvaluationProfileCycle::Boundary::end);
        if (last_anchor.checkpoint <= first_anchor.checkpoint ||
            last_anchor.steady_clock_nanoseconds <= first_anchor.steady_clock_nanoseconds)
            throw std::runtime_error{"group-by benchmark profile calibration interval is not positive"};

        for (auto cycle = warmup_cycles + 1; cycle <= cycles; ++cycle) {
            const auto time = start_time +
                              EvaluationTime::duration{static_cast<EvaluationTime::duration::rep>(cycle)};
            const auto cycle_start = find_anchor(time, EvaluationProfileCycle::Boundary::start);
            const auto cycle_end = find_anchor(time, EvaluationProfileCycle::Boundary::end);
            if (cycle_end.steady_clock_nanoseconds <= cycle_start.steady_clock_nanoseconds)
                throw std::runtime_error{"group-by benchmark profiled cycle duration is not positive"};
            profiled_cycle_milliseconds.push_back(static_cast<double>(cycle_end.steady_clock_nanoseconds -
                                                                      cycle_start.steady_clock_nanoseconds) /
                                                  1'000'000.0);
        }

        const auto ticks_per_millisecond =
            static_cast<double>(last_anchor.checkpoint - first_anchor.checkpoint) /
            (static_cast<double>(last_anchor.steady_clock_nanoseconds - first_anchor.steady_clock_nanoseconds) /
             1'000'000.0);
        ticks_per_millisecond_samples.push_back(ticks_per_millisecond);
        for (auto sample = first_sample; sample != node_ticks.size(); ++sample)
            node_milliseconds.push_back(static_cast<double>(node_ticks[sample]) / ticks_per_millisecond);
    }

    const auto [median_ticks, p95_ticks] = summarize_ticks(node_ticks);
    const auto [median_ms, p95_ms] = summarize(node_milliseconds);
    const auto [cycle_median_ms, cycle_p95_ms] = summarize(profiled_cycle_milliseconds);
    const auto [median_ticks_per_ms, p95_ticks_per_ms] = summarize(ticks_per_millisecond_samples);
    if (node_milliseconds.size() != profiled_cycle_milliseconds.size())
        throw std::runtime_error{"group-by benchmark node and cycle profile sample counts differ"};
    for (auto sample = std::size_t{}; sample != node_milliseconds.size(); ++sample) {
        const auto cycle_ms = profiled_cycle_milliseconds[sample];
        const auto conversion_tolerance_ms = std::max(0.001, cycle_ms * 0.01);
        if (node_milliseconds[sample] > cycle_ms + conversion_tolerance_ms)
            throw std::runtime_error{"group-by benchmark node interval exceeds its profiled cycle duration"};
    }
    std::cout << ChangeCount << ',' << GroupCount << ',' << (Multiple ? 2 : 1) << ','
              << (Multiple ? 2 : 1) << ',' << workload_name(Operation, Scaled) << ','
              << profile_clock_name(profile_clock) << ',' << node_ticks.size() << ',' << median_ticks << ','
              << p95_ticks << ',' << median_ms << ',' << p95_ms << ',' << cycle_median_ms << ','
              << cycle_p95_ms << ',' << median_ticks_per_ms << ',' << p95_ticks_per_ms << ",1\n";
}

template <std::size_t ChangeCount, std::size_t GroupCount, Mutation Operation, bool Multiple, bool Scaled>
void validate_case(const CompiledGraph &compiled) {
    using Sink = ResultSink<ChangeCount, Operation, Multiple, Scaled>;
    Sink::capture = true;
    auto global_state = GlobalState{};
    auto runtime = RuntimeGraph{compiled, global_state};
    const auto start_time = MIN_DT + EvaluationTime::duration{1};
    runtime.start(start_time);
    auto stop_on_exit = util::scope_exit{[&]() noexcept { runtime.stop(); }};
    runtime.evaluate(start_time);
    for (auto cycle = std::size_t{1}; cycle <= cycles; ++cycle)
        runtime.evaluate(start_time + EvaluationTime::duration{static_cast<EvaluationTime::duration::rep>(cycle)});
    runtime.stop();
    stop_on_exit.release();
    Sink::capture = false;

    auto expected_totals = std::map<i64, i64>{};
    auto expected_scaled_totals = std::map<i64, double>{};
    auto expected_second_totals = std::map<i64, double>{};
    auto expected_buckets = std::array<i64, source_rows>{};
    auto expected_values = std::array<i64, source_rows>{};
    expected_values.fill(1);
    for (auto row = std::size_t{}; row != source_rows; ++row)
        expected_buckets[row] = static_cast<i64>(row % GroupCount);
    for (auto cycle = std::size_t{1}; cycle <= cycles; ++cycle) {
        const auto first = (cycle - 1) * mutation_stride;
        for (auto change = std::size_t{}; change != ChangeCount; ++change) {
            const auto row = (first + change) % source_rows;
            if constexpr (Operation == Mutation::value_update)
                ++expected_values[row];
            else if constexpr (Operation == Mutation::key_move)
                expected_buckets[row] = (expected_buckets[row] + 1) % static_cast<i64>(GroupCount);
        }
    }
    for (auto row = std::size_t{}; row != source_rows; ++row) {
        const auto bucket = expected_buckets[row];
        const auto value = expected_values[row];
        expected_totals[bucket] += value;
        expected_scaled_totals[bucket] += static_cast<double>(value * static_cast<i64>(cycles + 1));
        expected_second_totals[bucket] += static_cast<double>(value * static_cast<i64>(Scaled ? cycles + 1 : 1));
    }
    if ((!Scaled && Sink::totals != expected_totals) ||
        (Scaled && Sink::scaled_totals != expected_scaled_totals) || Sink::result_size != GroupCount)
        throw std::runtime_error{"group-by benchmark result differs from the independent sum oracle"};
    if constexpr (Multiple)
        if (Sink::second_totals != expected_second_totals)
            throw std::runtime_error{"group-by benchmark second aggregation differs from the independent oracle"};
}

template <std::size_t ChangeCount, std::size_t GroupCount, Mutation Operation, bool Multiple, bool Scaled>
void run_case() {
    using Graph = CaseGraph<ChangeCount, GroupCount, Operation, Multiple, Scaled>;
    using Sink = typename Graph::Sink;
    const auto graph = Graph{};
    validate_case<ChangeCount, GroupCount, Operation, Multiple, Scaled>(*graph.compiled);

    auto initial_samples = std::vector<double>{};
    auto update_samples = std::vector<double>{};
    initial_samples.reserve(repeats);
    update_samples.reserve(repeats * measured_cycles);
    const auto start_time = MIN_DT + EvaluationTime::duration{1};

    for (auto repeat = std::size_t{}; repeat != repeats; ++repeat) {
        auto global_state = GlobalState{};
        auto runtime = RuntimeGraph{*graph.compiled, global_state};
        runtime.start(start_time);
        auto stop_on_exit = util::scope_exit{[&]() noexcept { runtime.stop(); }};
        const auto initial_begin = std::chrono::steady_clock::now();
        runtime.evaluate(start_time);
        const auto initial_end = std::chrono::steady_clock::now();
        initial_samples.push_back(milliseconds(initial_end - initial_begin));
        if (Sink::result_size != GroupCount)
            throw std::runtime_error{"group-by benchmark initial result has an unexpected group count"};

        for (auto cycle = std::size_t{1}; cycle <= cycles; ++cycle) {
            const auto begin = std::chrono::steady_clock::now();
            runtime.evaluate(start_time + EvaluationTime::duration{static_cast<EvaluationTime::duration::rep>(cycle)});
            const auto end = std::chrono::steady_clock::now();
            if (Sink::result_size != GroupCount)
                throw std::runtime_error{"group-by benchmark update changed the expected group count"};
            if (cycle > warmup_cycles)
                update_samples.push_back(milliseconds(end - begin));
        }
        runtime.stop();
        stop_on_exit.release();
    }

    const auto [initial_median, initial_p95] = summarize(initial_samples);
    const auto [update_median, update_p95] = summarize(update_samples);
    std::cout << source_rows << ',' << GroupCount << ',' << ChangeCount << ',' << (Multiple ? 2 : 1) << ','
              << (Multiple ? 2 : 1) << ','
              << workload_name(Operation, Scaled) << ",initial," << initial_samples.size() << ','
              << initial_median << ',' << initial_p95 << ',' << GroupCount << '\n';
    std::cout << source_rows << ',' << GroupCount << ',' << ChangeCount << ',' << (Multiple ? 2 : 1) << ','
              << (Multiple ? 2 : 1) << ','
              << workload_name(Operation, Scaled) << ",update," << update_samples.size() << ','
              << update_median << ',' << update_p95 << ',' << GroupCount << '\n';
}

struct OverflowMembership {
    std::size_t row_count{};
};

std::size_t overflow_row_count(const void *context) noexcept {
    return static_cast<const OverflowMembership *>(context)->row_count;
}

bool overflow_contains(const void *context, row_id_t id) noexcept {
    return id < static_cast<const OverflowMembership *>(context)->row_count;
}

row_id_t overflow_row_at(const void *, std::size_t position) { return static_cast<row_id_t>(position); }

void for_each_overflow_row(const void *context, void *target, RowSet::visit_function visit) {
    const auto row_count = static_cast<const OverflowMembership *>(context)->row_count;
    for (auto row = row_id_t{}; row != row_count; ++row)
        visit(target, row);
}

std::uint32_t overflow_group_at(const void *, row_id_t row) { return static_cast<std::uint32_t>(row / 2); }

RowSet overflow_row_set(const OverflowMembership &membership) {
    return {.context = std::addressof(membership),
            .size = &overflow_row_count,
            .contains = &overflow_contains,
            .id_at = &overflow_row_at,
            .for_each = &for_each_overflow_row};
}

void run_overflow_recovery() {
    constexpr auto warmups = std::size_t{2};
    constexpr auto measured = std::size_t{9};
    constexpr auto maximum = std::numeric_limits<double>::max();
    const auto &kernels = sum_aggregation_kernels();
    const auto *kernel = kernels.find(cpp_scalar_type<double>());
    if (kernel == nullptr)
        throw std::runtime_error{"group-by benchmark has no double sum kernel"};

    std::cout << std::setprecision(10)
              << "scope,groups,rows,warmup_samples,sample_count,median_ms,p95_ms,validated_group_sum\n";
    for (const auto group_count : std::array<std::size_t, 3>{32, 256, 2'048}) {
        const auto row_count = group_count * 2;
        const auto memory = allocator{};
        auto state = kernel->create(memory);
        auto membership = OverflowMembership{row_count};
        auto membership_rows = AggregationRows{overflow_row_set(membership), std::addressof(membership),
                                               &overflow_group_at};
        auto row_ids = std::vector<row_id_t>(row_count);
        auto groups = std::vector<std::uint32_t>(row_count);
        auto values = std::vector<double>(row_count, maximum);
        auto changed_rows = std::vector<row_id_t>(group_count);
        auto changed_groups = std::vector<std::uint32_t>(group_count);
        auto changed_values = std::vector<double>(group_count, maximum);
        auto results = std::vector<double>(group_count);
        std::ranges::iota(row_ids, row_id_t{});
        for (auto group = std::size_t{}; group != group_count; ++group) {
            state->add_group(static_cast<std::uint32_t>(group));
            changed_rows[group] = static_cast<row_id_t>(group * 2);
            changed_groups[group] = static_cast<std::uint32_t>(group);
            groups[group * 2] = static_cast<std::uint32_t>(group);
            groups[group * 2 + 1] = static_cast<std::uint32_t>(group);
        }
        const auto all_rows = ColumnBatch<>{0, row_count, values.data(), 0, sizeof(double) * 8,
                                            ColumnBatchValidity::all_valid()};
        const auto update_changed_rows = [&] {
            const auto batch = ColumnBatch<>{0, group_count, changed_values.data(), 0, sizeof(double) * 8,
                                              ColumnBatchValidity::all_valid()};
            state->update(changed_rows, changed_groups, batch);
        };
        state->update(row_ids, groups, all_rows);
        std::ranges::fill(changed_values, -maximum);
        update_changed_rows();

        auto samples = std::vector<double>{};
        samples.reserve(measured);
        for (auto sample = std::size_t{}; sample != warmups + measured; ++sample) {
            std::ranges::fill(changed_values, maximum);
            update_changed_rows();
            std::ranges::fill(changed_values, -maximum);
            update_changed_rows();

            const auto begin = std::chrono::steady_clock::now();
            for (auto group = std::size_t{}; group != group_count; ++group) {
                const auto value = state->result(static_cast<std::uint32_t>(group), membership_rows);
                if (!value)
                    throw std::runtime_error{"group-by benchmark double sum has no result"};
                results[group] = Scalar<double>::from(*value).value();
            }
            const auto end = std::chrono::steady_clock::now();
            if (sample >= warmups)
                samples.push_back(milliseconds(end - begin));
        }

        if (std::ranges::any_of(results, [](double value) { return value != 0.0; }))
            throw std::runtime_error{"group-by benchmark overflow recovery differs from the expected sums"};
        const auto [median_ms, p95_ms] = summarize(samples);
        std::cout << "aggregation-kernel-result-only," << group_count << ',' << row_count << ',' << warmups << ','
                  << measured << ',' << median_ms << ',' << p95_ms << ",0\n";
    }
}

#ifdef HG_FRAME_GROUP_BY_ARROW_BENCHMARK
enum class ArrowLayout { borrowed_chunks, dense_copy };

std::string_view arrow_layout_name(ArrowLayout layout) {
    switch (layout) {
    case ArrowLayout::borrowed_chunks:
        return "borrowed_chunks";
    case ArrowLayout::dense_copy:
        return "dense_copy";
    }
    throw std::runtime_error{"group-by Arrow benchmark has an unknown layout"};
}

template <typename Value> Value arrow_value(arrow::Result<Value> result) {
    if (!result.ok())
        throw std::runtime_error{"group-by Arrow benchmark: " + result.status().ToString()};
    return std::move(result).ValueOrDie();
}

template <ArrowLayout Layout> std::shared_ptr<arrow::ChunkedArray> export_arrow_column(const ColumnView<> &column) {
    if constexpr (Layout == ArrowLayout::borrowed_chunks)
        return arrow_value(borrow_arrow(column));
    else
        return arrow_value(copy_to_arrow(column));
}

template <bool Multiple, bool Scaled>
std::shared_ptr<arrow::Table> arrow_group_by(std::shared_ptr<arrow::Table> input, double multiplier) {
    auto expressions = std::vector<arrow::compute::Expression>{arrow::compute::field_ref("bucket")};
    auto names = std::vector<std::string>{"bucket"};
    if constexpr (Scaled) {
        auto converted = arrow::compute::call("cast", {arrow::compute::field_ref("value")},
                                              arrow::compute::CastOptions::Safe(arrow::float64()));
        expressions.push_back(arrow::compute::call("multiply", {std::move(converted),
                                                                 arrow::compute::literal(multiplier)}));
        names.push_back("scaled_value");
    } else {
        expressions.push_back(arrow::compute::field_ref("value"));
        names.push_back("value");
        if constexpr (Multiple) {
            expressions.push_back(arrow::compute::call(
                "cast", {arrow::compute::field_ref("value")},
                arrow::compute::CastOptions::Safe(arrow::float64())));
            names.push_back("value_double");
        }
    }

    auto source = arrow::acero::Declaration{
        "table_source", arrow::acero::TableSourceNodeOptions{std::move(input)}};
    auto project = arrow::acero::Declaration{
        "project", {std::move(source)}, arrow::acero::ProjectNodeOptions{std::move(expressions), std::move(names)}};
    auto aggregates = std::vector<arrow::compute::Aggregate>{};
    if constexpr (Scaled) {
        aggregates.emplace_back("hash_sum", arrow::FieldRef{"scaled_value"}, "total");
    } else if constexpr (Multiple) {
        aggregates.emplace_back("hash_sum", arrow::FieldRef{"value"}, "total");
        aggregates.emplace_back("hash_sum", arrow::FieldRef{"value_double"}, "second_total");
    } else {
        aggregates.emplace_back("hash_sum", arrow::FieldRef{"value"}, "total");
    }
    auto aggregate = arrow::acero::Declaration{
        "aggregate", {std::move(project)},
        arrow::acero::AggregateNodeOptions{std::move(aggregates), {arrow::FieldRef{"bucket"}}}};
    return arrow_value(arrow::acero::DeclarationToTable(std::move(aggregate), false));
}

template <std::size_t ChangeCount, std::size_t GroupCount, Mutation Operation, bool Multiple, bool Scaled,
          ArrowLayout Layout>
struct ArrowAggregation {
    In<TS<i64>> cycle;
    In<TSF<Rows>> source;
    In<TS<double>> multiplier;
    Out<TS<i64>> group_count;

    struct Measurement {
        double preparation_ms{};
        double query_ms{};
        double total_ms{};
        std::size_t bucket_chunks{};
        std::size_t value_chunks{};
    };

    inline static auto measurements = std::vector<Measurement>{};
    inline static bool capture_final_result{};
    inline static auto totals = std::map<i64, i64>{};
    inline static auto scaled_totals = std::map<i64, double>{};
    inline static auto second_totals = std::map<i64, double>{};

    void start() const {
        measurements.clear();
        totals.clear();
        scaled_totals.clear();
        second_totals.clear();
    }

    void evaluate() const {
        const auto total_begin = std::chrono::steady_clock::now();
        const auto preparation_begin = total_begin;
        auto bucket_view = source.column("bucket").column_view();
        auto value_view = source.column("value").column_view();
        auto bucket_array = export_arrow_column<Layout>(bucket_view);
        auto value_array = export_arrow_column<Layout>(value_view);
        const auto bucket_chunks = static_cast<std::size_t>(bucket_array->num_chunks());
        const auto value_chunks = static_cast<std::size_t>(value_array->num_chunks());
        auto input = arrow::Table::Make(
            arrow::schema({arrow::field("bucket", arrow::int64()), arrow::field("value", arrow::int64())}),
            {bucket_array, value_array}, static_cast<std::int64_t>(source.size()));
        const auto preparation_end = std::chrono::steady_clock::now();

        auto result = arrow_group_by<Multiple, Scaled>(input, *multiplier);
        const auto query_end = std::chrono::steady_clock::now();
        const auto result_size = result->num_rows();
        if (result_size != static_cast<std::int64_t>(GroupCount))
            throw std::runtime_error{"group-by Arrow benchmark returned an unexpected number of groups"};

        result.reset();
        input.reset();
        bucket_array.reset();
        value_array.reset();
        const auto total_end = std::chrono::steady_clock::now();
        measurements.push_back({milliseconds(preparation_end - preparation_begin),
                                milliseconds(query_end - preparation_end),
                                milliseconds(total_end - total_begin), bucket_chunks, value_chunks});
        group_count.set(static_cast<i64>(result_size));

        if (capture_final_result && *cycle == static_cast<i64>(cycles))
            capture_arrow_result();
    }

    void capture_arrow_result() const {
        auto bucket_array = export_arrow_column<Layout>(source.column("bucket").column_view());
        auto value_array = export_arrow_column<Layout>(source.column("value").column_view());
        auto input = arrow::Table::Make(
            arrow::schema({arrow::field("bucket", arrow::int64()), arrow::field("value", arrow::int64())}),
            {bucket_array, value_array}, static_cast<std::int64_t>(source.size()));
        const auto result = arrow_group_by<Multiple, Scaled>(std::move(input), *multiplier);
        const auto buckets = result->GetColumnByName("bucket");
        const auto total = result->GetColumnByName("total");
        const auto second = Multiple ? result->GetColumnByName("second_total") : nullptr;
        if (buckets == nullptr || total == nullptr || (Multiple && second == nullptr))
            throw std::runtime_error{"group-by Arrow benchmark result has unexpected columns"};

        for (auto row = std::int64_t{}; row != result->num_rows(); ++row) {
            auto key_scalar = arrow_value(buckets->GetScalar(row));
            if (!key_scalar->is_valid)
                throw std::runtime_error{"group-by Arrow benchmark returned an invalid group key"};
            const auto key = std::static_pointer_cast<arrow::Int64Scalar>(key_scalar)->value;
            auto total_scalar = arrow_value(total->GetScalar(row));
            if (!total_scalar->is_valid)
                throw std::runtime_error{"group-by Arrow benchmark returned an invalid group sum"};
            if constexpr (Scaled) {
                scaled_totals[key] = std::static_pointer_cast<arrow::DoubleScalar>(total_scalar)->value;
            } else {
                totals[key] = std::static_pointer_cast<arrow::Int64Scalar>(total_scalar)->value;
            }
            if constexpr (Multiple) {
                auto second_scalar = arrow_value(second->GetScalar(row));
                if (!second_scalar->is_valid)
                    throw std::runtime_error{"group-by Arrow benchmark returned an invalid second sum"};
                second_totals[key] = std::static_pointer_cast<arrow::DoubleScalar>(second_scalar)->value;
            }
        }
    }
};

template <std::size_t GroupCount> struct ArrowSizeSink {
    In<TS<i64>> cycle;
    In<TS<i64>> group_count;

    inline static std::size_t evaluations{};

    void start() const { evaluations = 0; }
    void evaluate() const {
        if (*group_count != static_cast<i64>(GroupCount))
            throw std::runtime_error{"group-by Arrow benchmark sink saw an unexpected group count"};
        ++evaluations;
    }
};

template <std::size_t ChangeCount, std::size_t GroupCount, Mutation Operation, bool Multiple, bool Scaled,
          ArrowLayout Layout>
struct ArrowCaseGraph {
    using Aggregation = ArrowAggregation<ChangeCount, GroupCount, Operation, Multiple, Scaled, Layout>;
    using Sink = ArrowSizeSink<GroupCount>;

    std::shared_ptr<const CompiledGraph> compiled;

    ArrowCaseGraph() {
        auto wiring = GraphWiring{};
        {
            const auto context = WiringContext{wiring};
            const auto cycle = node_template<CycleClock>()();
            const auto source = node_template<FrameSource<ChangeCount, GroupCount, Operation>>()(cycle);
            const auto multiplier = node_template<MultiplierSource>()(cycle);
            const auto group_count = node_template<Aggregation>()(cycle, source, multiplier);
            static_cast<void>(node_template<Sink>()(cycle, group_count));
        }
        wiring.finalize();
        compiled = std::make_shared<CompiledGraph>(wiring);
    }
};

template <std::size_t ChangeCount, std::size_t GroupCount, Mutation Operation, bool Multiple, bool Scaled,
          ArrowLayout Layout>
void validate_arrow_case(const CompiledGraph &compiled) {
    using Graph = ArrowCaseGraph<ChangeCount, GroupCount, Operation, Multiple, Scaled, Layout>;
    using Aggregation = typename Graph::Aggregation;
    using Sink = typename Graph::Sink;
    Aggregation::capture_final_result = true;
    auto capture_guard = util::scope_exit{[]() noexcept { Aggregation::capture_final_result = false; }};
    auto global_state = GlobalState{};
    auto runtime = RuntimeGraph{compiled, global_state};
    const auto start_time = MIN_DT + EvaluationTime::duration{1};
    runtime.start(start_time);
    auto stop_on_exit = util::scope_exit{[&]() noexcept { runtime.stop(); }};
    runtime.evaluate(start_time);
    for (auto cycle = std::size_t{1}; cycle <= cycles; ++cycle)
        runtime.evaluate(start_time + EvaluationTime::duration{static_cast<EvaluationTime::duration::rep>(cycle)});
    runtime.stop();
    stop_on_exit.release();
    Aggregation::capture_final_result = false;
    capture_guard.release();
    if (Sink::evaluations != cycles + 1)
        throw std::runtime_error{"group-by Arrow benchmark sink did not evaluate on every cycle"};

    auto expected_buckets = std::vector<i64>(source_rows);
    auto expected_values = std::vector<i64>(source_rows, 1);
    for (auto row = std::size_t{}; row != source_rows; ++row)
        expected_buckets[row] = static_cast<i64>(row % GroupCount);
    for (auto cycle = std::size_t{1}; cycle <= cycles; ++cycle) {
        const auto first = (cycle - 1) * mutation_stride;
        for (auto change = std::size_t{}; change != ChangeCount; ++change) {
            const auto row = (first + change) % source_rows;
            if constexpr (Operation == Mutation::value_update)
                ++expected_values[row];
            else if constexpr (Operation == Mutation::key_move)
                expected_buckets[row] = (expected_buckets[row] + 1) % static_cast<i64>(GroupCount);
        }
    }

    auto expected_totals = std::map<i64, i64>{};
    auto expected_scaled_totals = std::map<i64, double>{};
    auto expected_second_totals = std::map<i64, double>{};
    for (auto row = std::size_t{}; row != source_rows; ++row) {
        const auto bucket = expected_buckets[row];
        const auto value = expected_values[row];
        expected_totals[bucket] += value;
        expected_scaled_totals[bucket] += static_cast<double>(value * static_cast<i64>(cycles + 1));
        expected_second_totals[bucket] += static_cast<double>(value);
    }
    if ((!Scaled && Aggregation::totals != expected_totals) ||
        (Scaled && Aggregation::scaled_totals != expected_scaled_totals) ||
        (Multiple && Aggregation::second_totals != expected_second_totals))
        throw std::runtime_error{"group-by Arrow benchmark result differs from the independent final-state oracle"};
}

struct ArrowSummary {
    double median_ms{};
    double p95_ms{};
    std::size_t chunk_min{};
    std::size_t chunk_max{};
};

template <typename Measurement, typename Duration>
ArrowSummary summarize_arrow_measurements(std::span<const Measurement> samples, Duration duration,
                                         bool bucket_chunks) {
    auto times = std::vector<double>{};
    auto chunk_counts = std::vector<std::size_t>{};
    times.reserve(samples.size());
    chunk_counts.reserve(samples.size());
    for (const auto &sample : samples) {
        times.push_back(duration(sample));
        chunk_counts.push_back(bucket_chunks ? sample.bucket_chunks : sample.value_chunks);
    }
    const auto [median_ms, p95_ms] = summarize(times);
    const auto [minimum, maximum] = std::ranges::minmax(chunk_counts);
    return {median_ms, p95_ms, minimum, maximum};
}

template <std::size_t ChangeCount, std::size_t GroupCount, Mutation Operation, bool Multiple, bool Scaled,
          ArrowLayout Layout>
void run_arrow_case(std::size_t warmups, std::size_t measured, std::size_t repeat_count) {
    using Graph = ArrowCaseGraph<ChangeCount, GroupCount, Operation, Multiple, Scaled, Layout>;
    using Aggregation = typename Graph::Aggregation;
    using Sink = typename Graph::Sink;
    const auto graph = Graph{};
    validate_arrow_case<ChangeCount, GroupCount, Operation, Multiple, Scaled, Layout>(*graph.compiled);

    auto initial_samples = std::vector<typename Aggregation::Measurement>{};
    auto update_samples = std::vector<typename Aggregation::Measurement>{};
    initial_samples.reserve(repeat_count);
    update_samples.reserve(repeat_count * measured);
    const auto start_time = MIN_DT + EvaluationTime::duration{1};
    const auto total_cycles = warmups + measured;

    for (auto repeat = std::size_t{}; repeat != repeat_count; ++repeat) {
        auto global_state = GlobalState{};
        auto runtime = RuntimeGraph{*graph.compiled, global_state};
        runtime.start(start_time);
        auto stop_on_exit = util::scope_exit{[&]() noexcept { runtime.stop(); }};
        runtime.evaluate(start_time);
        if (Aggregation::measurements.size() != 1)
            throw std::runtime_error{"group-by Arrow benchmark has no initial query sample"};
        initial_samples.push_back(Aggregation::measurements.front());

        for (auto cycle = std::size_t{1}; cycle <= total_cycles; ++cycle) {
            runtime.evaluate(start_time +
                             EvaluationTime::duration{static_cast<EvaluationTime::duration::rep>(cycle)});
            if (Aggregation::measurements.size() != cycle + 1 || Sink::evaluations != cycle + 1)
                throw std::runtime_error{"group-by Arrow benchmark did not execute one query on each cycle"};
            if (cycle > warmups)
                update_samples.push_back(Aggregation::measurements.back());
        }
        runtime.stop();
        stop_on_exit.release();
    }

    const auto append_rows = [&](std::string_view interval,
                                 std::span<const typename Aggregation::Measurement> samples) {
        const auto preparation = summarize_arrow_measurements(
            samples, [](const auto &sample) { return sample.preparation_ms; }, true);
        const auto query = summarize_arrow_measurements(
            samples, [](const auto &sample) { return sample.query_ms; }, true);
        const auto total = summarize_arrow_measurements(
            samples, [](const auto &sample) { return sample.total_ms; }, true);
        const auto value_chunks = summarize_arrow_measurements(
            samples, [](const auto &sample) { return sample.total_ms; }, false);
        const auto write_row = [&](std::string_view phase, const ArrowSummary &summary,
                                   const ArrowSummary &value_summary) {
            std::cout << arrow_layout_name(Layout) << ',' << source_rows << ',' << GroupCount << ',' << ChangeCount
                      << ',' << (Multiple ? 2 : 1) << ',' << (Multiple ? 2 : 1) << ','
                      << workload_name(Operation, Scaled) << ',' << interval << ',' << phase << ',' << samples.size()
                      << ',' << summary.median_ms << ',' << summary.p95_ms << ',' << summary.chunk_min << ','
                      << summary.chunk_max << ',' << value_summary.chunk_min << ',' << value_summary.chunk_max << ','
                      << GroupCount << '\n';
        };
        write_row("preparation", preparation, value_chunks);
        write_row("query", query, value_chunks);
        write_row("total", total, value_chunks);
    };

    append_rows("initial", initial_samples);
    append_rows("update", update_samples);
}

template <std::size_t ChangeCount, std::size_t GroupCount, Mutation Operation, bool Multiple, bool Scaled>
void run_arrow_workload(std::size_t warmups, std::size_t measured, std::size_t repeat_count) {
    run_arrow_case<ChangeCount, GroupCount, Operation, Multiple, Scaled, ArrowLayout::borrowed_chunks>(
        warmups, measured, repeat_count);
    run_arrow_case<ChangeCount, GroupCount, Operation, Multiple, Scaled, ArrowLayout::dense_copy>(
        warmups, measured, repeat_count);
}
#endif

} // namespace

#ifdef HG_FRAME_GROUP_BY_ARROW_BENCHMARK
int main(int argc, char **argv) {
    const auto smoke = argc == 2 && std::string_view{argv[1]} == "--smoke";
    if (argc > 2 || (argc == 2 && !smoke))
        throw std::runtime_error{"usage: hg_frame_group_by_arrow_benchmark [--smoke]"};

    const auto initialize_status = arrow::compute::Initialize();
    if (!initialize_status.ok())
        throw std::runtime_error{"group-by Arrow benchmark could not initialize compute: " +
                                 initialize_status.ToString()};
    const auto &build = arrow::GetBuildInfo();
    const auto runtime = arrow::GetRuntimeInfo();
    std::cout << std::setprecision(10)
              << "#arrow_version," << build.version_string << '\n'
              << "#arrow_build_type," << build.build_type << '\n'
              << "#arrow_compiler," << build.compiler_id << ' ' << build.compiler_version << '\n'
              << "#arrow_enabled_simd," << runtime.simd_level << '\n'
              << "#arrow_detected_simd," << runtime.detected_simd_level << '\n'
              << "#acero_use_threads,false\n"
              << "#prep=ColumnView export for bucket/value plus two-column Table assembly\n"
              << "#query=synchronous Acero table_source/project/hash_sum plan, execution, and result allocation\n"
              << "#total=prep+query+num_rows size check+Arrow output/input destruction\n"
              << "#source mutation and graph scheduling are outside all reported intervals\n"
              << "layout,source_rows,groups,changed_rows,aggregation_count,aggregations_affected,workload,interval,"
                 "phase,sample_count,median_ms,p95_ms,bucket_chunks_min,bucket_chunks_max,value_chunks_min,"
                 "value_chunks_max,validated_groups\n";

    const auto warmups = smoke ? std::size_t{1} : warmup_cycles;
    const auto measured = smoke ? std::size_t{2} : measured_cycles;
    const auto repeat_count = smoke ? std::size_t{1} : repeats;
    if (smoke) {
        run_arrow_workload<1, 100, Mutation::value_update, false, false>(warmups, measured, repeat_count);
        run_arrow_workload<1, 10'000, Mutation::value_update, false, false>(warmups, measured, repeat_count);
        run_arrow_workload<100, 100, Mutation::key_move, false, false>(warmups, measured, repeat_count);
        run_arrow_workload<100, 100, Mutation::remove_add, false, false>(warmups, measured, repeat_count);
        run_arrow_workload<1, 100, Mutation::value_update, true, false>(warmups, measured, repeat_count);
        run_arrow_workload<0, 100, Mutation::value_update, false, true>(warmups, measured, repeat_count);
        return 0;
    }

    run_arrow_workload<1, 100, Mutation::value_update, false, false>(warmups, measured, repeat_count);
    run_arrow_workload<100, 100, Mutation::value_update, false, false>(warmups, measured, repeat_count);
    run_arrow_workload<10'000, 100, Mutation::value_update, false, false>(warmups, measured, repeat_count);
    run_arrow_workload<1, 10'000, Mutation::value_update, false, false>(warmups, measured, repeat_count);
    run_arrow_workload<100, 10'000, Mutation::value_update, false, false>(warmups, measured, repeat_count);
    run_arrow_workload<10'000, 10'000, Mutation::value_update, false, false>(warmups, measured, repeat_count);
    run_arrow_workload<100, 100, Mutation::key_move, false, false>(warmups, measured, repeat_count);
    run_arrow_workload<100, 100, Mutation::remove_add, false, false>(warmups, measured, repeat_count);
    run_arrow_workload<1, 100, Mutation::value_update, true, false>(warmups, measured, repeat_count);
    run_arrow_workload<0, 100, Mutation::value_update, false, true>(warmups, measured, repeat_count);
    return 0;
}
#else
int main(int argc, char **argv) {
    const auto smoke = argc == 2 && std::string_view{argv[1]} == "--smoke";
    const auto profile_nodes = argc == 2 && std::string_view{argv[1]} == "--profile-nodes";
    const auto overflow_recovery = argc == 2 && std::string_view{argv[1]} == "--overflow-recovery";
    if (argc > 2 || (argc == 2 && !smoke && !profile_nodes && !overflow_recovery))
        throw std::runtime_error{
            "usage: hg_frame_group_by_benchmark [--smoke|--profile-nodes|--overflow-recovery]"};

    if (overflow_recovery) {
        run_overflow_recovery();
        return 0;
    }

    if (profile_nodes) {
        std::cout << std::setprecision(10)
                  << "changed_rows,groups,aggregation_count,aggregations_affected,workload,profile_clock,"
                     "sample_count,node_median_ticks,"
                     "node_p95_ticks,node_median_ms,node_p95_ms,profiled_cycle_median_ms,"
                     "profiled_cycle_p95_ms,checkpoint_ticks_per_ms_median,"
                     "checkpoint_ticks_per_ms_p95,node_evaluations_per_cycle\n";
        profile_case<1, 100, Mutation::value_update, false, false>();
        profile_case<100, 100, Mutation::value_update, false, false>();
        profile_case<10'000, 100, Mutation::value_update, false, false>();
        profile_case<1, 10'000, Mutation::value_update, false, false>();
        profile_case<100, 10'000, Mutation::value_update, false, false>();
        profile_case<10'000, 10'000, Mutation::value_update, false, false>();
        profile_case<100, 100, Mutation::key_move, false, false>();
        profile_case<100, 100, Mutation::remove_add, false, false>();
        profile_case<1, 100, Mutation::value_update, true, false>();
        profile_case<0, 100, Mutation::value_update, false, true>();
        return 0;
    }

    std::cout << std::setprecision(10)
              << "source_rows,groups,changed_rows,aggregation_count,aggregations_affected,workload,interval,"
                 "sample_count,median_ms,p95_ms,"
                 "validated_groups\n";
    if (smoke) {
        run_case<1, 100, Mutation::value_update, false, false>();
        run_case<1, 100, Mutation::value_update, true, false>();
        run_case<100, 100, Mutation::key_move, false, false>();
        run_case<100, 100, Mutation::remove_add, false, false>();
        run_case<0, 100, Mutation::value_update, false, true>();
        return 0;
    }
    run_case<1, 100, Mutation::value_update, false, false>();
    run_case<100, 100, Mutation::value_update, false, false>();
    run_case<10'000, 100, Mutation::value_update, false, false>();
    run_case<1, 10'000, Mutation::value_update, false, false>();
    run_case<100, 10'000, Mutation::value_update, false, false>();
    run_case<10'000, 10'000, Mutation::value_update, false, false>();
    run_case<100, 100, Mutation::key_move, false, false>();
    run_case<100, 100, Mutation::remove_add, false, false>();
    run_case<1, 100, Mutation::value_update, true, false>();
    run_case<0, 100, Mutation::value_update, false, true>();
}
#endif
