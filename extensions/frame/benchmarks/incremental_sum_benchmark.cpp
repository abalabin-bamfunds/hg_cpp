#include <bg/frame/frame.hpp>
#include <bg/frame/tsc.hpp>
#include <bg/graph/compiled_graph.hpp>
#include <bg/graph/details/compiled_graph_builder.hpp>
#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/runtime_graph.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/evaluation_profile.hpp>
#include <bg/evaluation_profiler.hpp>
#include <bg/library/operators.hpp>
#include <bg/node.hpp>
#include <bg/util/scope_exit.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <variant>
#include <vector>

namespace {

using namespace bg;
using namespace bg::frame;

constexpr auto warmup_cycles = std::size_t{10};
constexpr auto measured_cycles = std::size_t{101};
constexpr auto cycles = warmup_cycles + measured_cycles;
constexpr auto repeats = std::size_t{3};

template <typename Definition> const auto &node_template() {
    static const auto value = node<Definition>();
    return value;
}

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

template <std::size_t RowCount> struct ColumnSourceState {
    std::array<row_id_t, RowCount> rows{};
    std::array<i64, RowCount> values{};
};

template <std::size_t RowCount> struct ColumnSource {
    In<TS<i64>> cycle;
    State<ColumnSourceState<RowCount>> state;
    Out<TSC<i64>> output;

    static constexpr auto change_count = std::size_t{1};

    void start() const {
        state->values.fill(1);
        for (auto row = std::size_t{}; row != RowCount; ++row)
            state->rows[row] = output.add(i64{1});
    }

    void evaluate() const {
        if (*cycle == 0)
            return;

        for (auto row = std::size_t{}; row != change_count; ++row) {
            const auto value = ++state->values[row];
            output.set(state->rows[row], value);
        }
    }
};

template <template <typename, auto...> typename Field> struct BenchmarkRows {
    Field<i64> id;
    Field<i64> selected;
    Field<i64> value;

    static SchemaMetadata metadata() { return key_metadata<"id">(); }
};

template <std::size_t RowCount> struct FrameSourceState {
    std::array<row_id_t, RowCount> rows{};
    std::array<i64, RowCount> values{};
};

template <std::size_t RowCount> struct FrameSource {
    In<TS<i64>> cycle;
    State<FrameSourceState<RowCount>> state;
    Out<TSF<BenchmarkRows>> output;

    static constexpr auto selected_count = (RowCount + 1) / 2;
    static constexpr auto change_count = std::size_t{1};

    void start() const {
        state->values.fill(1);
        for (auto row = std::size_t{}; row != RowCount; ++row) {
            state->rows[row] = output.add(Row<BenchmarkRows>{.id = Cell<i64>{static_cast<i64>(row)},
                                                               .selected = Cell<i64>{row % 2 == 0 ? 1 : 0},
                                                               .value = Cell<i64>{1}});
        }
    }

    void evaluate() const {
        if (*cycle == 0)
            return;

        for (auto selected = std::size_t{}; selected != change_count; ++selected) {
            const auto row = selected * 2;
            const auto value = ++state->values[row];
            output.set(state->rows[row], RowPatch<BenchmarkRows>{.value = PatchCell<i64>{value}});
        }
    }
};

struct FullScanSum {
    In<TSC<i64>> values;
    Out<TS<i64>> output;

    void evaluate() const {
        auto total = i64{};
        values.for_each_batch([&](const auto &batch) {
            for (auto position = std::size_t{}; position != batch.cells.size; ++position)
                if (batch.cells.is_valid(position))
                    total += batch.cells.value_at(position);
        });
        output.set(total);
    }
};

template <bool Filtered> struct ResultSink {
    In<TS<i64>> cycle;
    In<TS<i64>> value;

    inline static auto values = std::array<i64, cycles + 1>{};

    void evaluate() const { values.at(static_cast<std::size_t>(*cycle)) = *value; }
};

template <bool Filtered, bool Incremental, typename Values>
NodeCallIdentity wire_sum(Values values, wp<TS<i64>> cycle, GraphWiring &wiring) {
    const auto result = [&] {
        if constexpr (Incremental) {
            return operators::sum_(values).template as<TS<i64>>();
        } else {
            return node_template<FullScanSum>()(values).template as<TS<i64>>();
        }
    }();
    static_cast<void>(node_template<ResultSink<Filtered>>()(cycle, result));

    const auto identity = wiring.node_call_identity(result);
    if (!identity)
        throw std::runtime_error{"sum benchmark result does not identify a node call"};
    return *identity;
}

struct Timing {
    double median_ms{};
    double p95_ms{};
    i64 final_value{};
};

struct ProfileTiming {
    std::vector<std::uint64_t> checkpoint_ticks;
    std::vector<double> milliseconds;
    std::vector<double> ticks_per_millisecond;
    EvaluationProfileClock clock{EvaluationProfileClock::steady_clock_nanoseconds};
    i64 final_value{};
};

struct ProfileCycleAnchor {
    EvaluationTime evaluation_time;
    EvaluationProfileCycle::Boundary boundary;
    std::uint64_t checkpoint{};
    std::int64_t steady_clock_nanoseconds{};
};

struct ProfileGraph {
    std::unique_ptr<GraphWiring> wiring;
    std::shared_ptr<const CompiledGraph> compiled;
    std::size_t sum_node_index{};
};

double milliseconds(std::chrono::steady_clock::duration duration) {
    return std::chrono::duration<double, std::milli>{duration}.count();
}

std::pair<double, double> summarize(std::span<const double> samples) {
    auto sorted = std::vector<double>{samples.begin(), samples.end()};
    std::ranges::sort(sorted);
    const auto p95_index = (sorted.size() * 95 + 99) / 100 - 1;
    return {sorted[sorted.size() / 2], sorted[p95_index]};
}

std::pair<std::uint64_t, std::uint64_t> summarize_ticks(std::span<const std::uint64_t> samples) {
    auto sorted = std::vector<std::uint64_t>{samples.begin(), samples.end()};
    std::ranges::sort(sorted);
    const auto p95_index = (sorted.size() * 95 + 99) / 100 - 1;
    return {sorted[sorted.size() / 2], sorted[p95_index]};
}

std::string_view profile_clock_name(EvaluationProfileClock clock) {
    switch (clock) {
    case EvaluationProfileClock::timestamp_counter:
        return "timestamp_counter";
    case EvaluationProfileClock::steady_clock_nanoseconds:
        return "steady_clock_nanoseconds";
    }
    throw std::runtime_error{"sum benchmark profile has an unknown checkpoint clock"};
}

template <std::size_t RowCount, bool Filtered, bool Incremental> Timing run_case() {
    using Sink = ResultSink<Filtered>;

    auto wiring = GraphWiring{};
    {
        auto context = WiringContext{wiring};
        const auto cycle = node_template<CycleClock>()();
        if constexpr (Filtered) {
            const auto frame = node_template<FrameSource<RowCount>>()(cycle);
            const auto selected = filter(frame, col("selected") > i64{0});
            const auto values = select_column<i64>(selected, util::symbol{"value"});
            static_cast<void>(wire_sum<Filtered, Incremental>(values, cycle, wiring));
        } else {
            const auto values = node_template<ColumnSource<RowCount>>()(cycle);
            static_cast<void>(wire_sum<Filtered, Incremental>(values, cycle, wiring));
        }
    }
    wiring.finalize();
    const auto compiled = CompiledGraph{wiring};

    auto samples = std::array<double, repeats * measured_cycles>{};
    auto sample_index = std::size_t{};
    const auto start_time = MIN_DT + EvaluationTime::duration{1};
    constexpr auto selected_count = Filtered ? (RowCount + 1) / 2 : RowCount;
    constexpr auto changes_per_cycle = std::size_t{1};
    constexpr auto initial_total = static_cast<i64>(selected_count);

    for (auto repeat = std::size_t{}; repeat != repeats; ++repeat) {
        Sink::values.fill(0);
        auto global_state = GlobalState{};
        auto runtime = std::make_unique<RuntimeGraph>(compiled, global_state);
        runtime->start(start_time);
        auto stop_on_exit = util::scope_exit{[&]() noexcept { runtime->stop(); }};
        runtime->evaluate(start_time);

        if (Sink::values[0] != initial_total)
            throw std::runtime_error{"sum benchmark initial total differs from the expected value"};

        for (auto cycle = std::size_t{1}; cycle <= cycles; ++cycle) {
            const auto begin = std::chrono::steady_clock::now();
            runtime->evaluate(start_time + EvaluationTime::duration{static_cast<EvaluationTime::duration::rep>(cycle)});
            const auto end = std::chrono::steady_clock::now();
            if (Sink::values[cycle] != initial_total + static_cast<i64>(changes_per_cycle * cycle))
                throw std::runtime_error{"sum benchmark tick differs from the expected value"};
            if (cycle > warmup_cycles)
                samples[sample_index++] = milliseconds(end - begin);
        }

        runtime->stop();
        stop_on_exit.release();
    }

    const auto [median_ms, p95_ms] = summarize(samples);
    return {median_ms, p95_ms, Sink::values[cycles]};
}

template <std::size_t RowCount, bool Filtered, bool Incremental> ProfileGraph make_profile_graph() {
    auto wiring = std::make_unique<GraphWiring>();
    auto sum_node = std::optional<NodeCallIdentity>{};
    {
        auto context = WiringContext{*wiring};
        const auto cycle = node_template<CycleClock>()();
        if constexpr (Filtered) {
            const auto frame = node_template<FrameSource<RowCount>>()(cycle);
            const auto selected = filter(frame, col("selected") > i64{0});
            const auto values = select_column<i64>(selected, util::symbol{"value"});
            sum_node = wire_sum<Filtered, Incremental>(values, cycle, *wiring);
        } else {
            const auto values = node_template<ColumnSource<RowCount>>()(cycle);
            sum_node = wire_sum<Filtered, Incremental>(values, cycle, *wiring);
        }
    }
    wiring->finalize();

    auto compiled_indices = std::array{wiring->node_index(*sum_node)};
    auto compiled = bg::details::compile_graph(*wiring, allocator{}, compiled_indices);
    return {std::move(wiring), std::move(compiled), compiled_indices.front()};
}

template <std::size_t RowCount, bool Filtered, bool Incremental> ProfileTiming run_profile_case() {
    using Sink = ResultSink<Filtered>;

    auto graph = make_profile_graph<RowCount, Filtered, Incremental>();
    auto result = ProfileTiming{};
    result.checkpoint_ticks.reserve(repeats * measured_cycles);
    result.milliseconds.reserve(repeats * measured_cycles);
    result.ticks_per_millisecond.reserve(repeats);

    const auto start_time = MIN_DT + EvaluationTime::duration{1};
    constexpr auto selected_count = Filtered ? (RowCount + 1) / 2 : RowCount;
    constexpr auto initial_total = static_cast<i64>(selected_count);

    for (auto repeat = std::size_t{}; repeat != repeats; ++repeat) {
        Sink::values.fill(0);
        auto global_state = GlobalState{};
        auto runtime = std::make_unique<RuntimeGraph>(*graph.compiled, global_state);
        auto profile = EvaluationProfile{};
        auto reader = EvaluationProfileReader{profile.take_recording()};
        auto profiler = EvaluationProfiler{};
        auto cycle_anchors = std::vector<ProfileCycleAnchor>{};
        if (repeat == 0)
            result.clock = reader.clock();
        else if (reader.clock() != result.clock)
            throw std::runtime_error{"sum benchmark profile checkpoint clock changed between repeats"};
        const auto read_profile = [&] {
            while (auto record = reader.read()) {
                auto cycle = std::optional<EvaluationProfileCycle>{};
                if (const auto *value = std::get_if<EvaluationProfileCycle>(&*record))
                    cycle = *value;

                profiler.consume(std::move(*record));
                if (cycle && profiler.root_graph_id() == cycle->graph_id)
                    cycle_anchors.push_back({cycle->evaluation_time, cycle->boundary,
                                             cycle->checkpoint, cycle->steady_clock_nanoseconds});
            }
        };

        runtime->start(start_time, profile);
        read_profile();
        auto stop_on_exit = util::scope_exit{[&]() noexcept { runtime->stop(profile); }};
        runtime->evaluate(start_time, profile);
        read_profile();

        if (Sink::values[0] != initial_total)
            throw std::runtime_error{"sum benchmark initial total differs from the expected value"};

        const auto graph_id = profiler.root_graph_id();
        if (!graph_id)
            throw std::runtime_error{"sum benchmark profile has no root graph"};
        auto previous = profiler.node_totals(*graph_id, static_cast<std::uint32_t>(graph.sum_node_index));
        if (!previous)
            throw std::runtime_error{"sum benchmark profile has no selected sum node totals"};

        for (auto cycle = std::size_t{1}; cycle <= cycles; ++cycle) {
            runtime->evaluate(start_time + EvaluationTime::duration{static_cast<EvaluationTime::duration::rep>(cycle)},
                              profile);
            read_profile();

            if (Sink::values[cycle] != initial_total + static_cast<i64>(cycle))
                throw std::runtime_error{"sum benchmark tick differs from the expected value"};

            const auto current = profiler.node_totals(*graph_id, static_cast<std::uint32_t>(graph.sum_node_index));
            if (!current || current->evaluations != previous->evaluations + 1 ||
                current->checkpoint_ticks < previous->checkpoint_ticks) {
                throw std::runtime_error{"sum benchmark selected sum node did not evaluate exactly once per cycle"};
            }

            if (cycle > warmup_cycles)
                result.checkpoint_ticks.push_back(current->checkpoint_ticks - previous->checkpoint_ticks);
            previous = current;
        }

        runtime->stop(profile);
        stop_on_exit.release();
        profile.finish();
        read_profile();
        if (profile.failed())
            throw std::runtime_error{"sum benchmark evaluation profile recording failed"};

        const auto first_time = start_time +
                                EvaluationTime::duration{static_cast<EvaluationTime::duration::rep>(warmup_cycles + 1)};
        const auto last_time = start_time + EvaluationTime::duration{static_cast<EvaluationTime::duration::rep>(cycles)};
        const auto find_anchor = [&](EvaluationTime time, EvaluationProfileCycle::Boundary boundary) {
            const auto found = std::ranges::find_if(cycle_anchors, [&](const auto &anchor) {
                return anchor.evaluation_time == time && anchor.boundary == boundary;
            });
            if (found == cycle_anchors.end())
                throw std::runtime_error{"sum benchmark profile is missing a cycle calibration anchor"};
            return *found;
        };
        const auto first_anchor = find_anchor(first_time, EvaluationProfileCycle::Boundary::start);
        const auto last_anchor = find_anchor(last_time, EvaluationProfileCycle::Boundary::end);
        if (last_anchor.checkpoint <= first_anchor.checkpoint ||
            last_anchor.steady_clock_nanoseconds <= first_anchor.steady_clock_nanoseconds) {
            throw std::runtime_error{"sum benchmark profile calibration interval is not positive"};
        }

        const auto ticks_per_millisecond =
            static_cast<double>(last_anchor.checkpoint - first_anchor.checkpoint) /
            (static_cast<double>(last_anchor.steady_clock_nanoseconds - first_anchor.steady_clock_nanoseconds) /
             1'000'000.0);
        result.ticks_per_millisecond.push_back(ticks_per_millisecond);
        for (auto sample = result.checkpoint_ticks.size() - measured_cycles;
             sample != result.checkpoint_ticks.size(); ++sample) {
            result.milliseconds.push_back(static_cast<double>(result.checkpoint_ticks[sample]) /
                                          ticks_per_millisecond);
        }
        result.final_value = Sink::values[cycles];
    }

    return result;
}

template <std::size_t RowCount> void print_row(bool filtered) {
    const auto incremental = filtered ? run_case<RowCount, true, true>() : run_case<RowCount, false, true>();
    const auto full_scan = filtered ? run_case<RowCount, true, false>() : run_case<RowCount, false, false>();
    const auto updates = std::size_t{1};
    if (incremental.final_value != full_scan.final_value)
        throw std::runtime_error{"incremental and full-scan sums differ"};

    const auto active_rows = filtered ? (RowCount + 1) / 2 : RowCount;
    std::cout << RowCount << ',' << active_rows << ",i64," << (filtered ? "filtered" : "column") << ',' << updates << ','
              << incremental.median_ms << ',' << incremental.p95_ms << ','
              << full_scan.median_ms << ',' << full_scan.p95_ms << ','
              << full_scan.median_ms / incremental.median_ms << ',' << incremental.final_value << '\n';
}

template <std::size_t RowCount, bool Filtered>
void print_profile_result(const ProfileTiming &timing, std::string_view implementation) {
    const auto [median_ticks, p95_ticks] = summarize_ticks(timing.checkpoint_ticks);
    const auto [median_ms, p95_ms] = summarize(timing.milliseconds);
    const auto [median_ticks_per_ms, p95_ticks_per_ms] = summarize(timing.ticks_per_millisecond);
    constexpr auto active_rows = Filtered ? (RowCount + 1) / 2 : RowCount;
    constexpr auto updates = std::size_t{1};
    std::cout << RowCount << ',' << active_rows << ",i64," << (Filtered ? "filtered" : "column") << ','
              << implementation << ',' << updates << ',' << profile_clock_name(timing.clock) << ",1," << median_ticks
              << ',' << p95_ticks << ',' << median_ms << ',' << p95_ms << ',' << median_ticks_per_ms << ','
              << p95_ticks_per_ms << ',' << timing.final_value << '\n';
}

template <std::size_t RowCount> void print_profile_row(bool filtered) {
    if (filtered) {
        const auto incremental = run_profile_case<RowCount, true, true>();
        const auto full_scan = run_profile_case<RowCount, true, false>();
        if (incremental.final_value != full_scan.final_value)
            throw std::runtime_error{"incremental and full-scan sums differ"};
        print_profile_result<RowCount, true>(incremental, "incremental");
        print_profile_result<RowCount, true>(full_scan, "full_scan");
        return;
    }

    const auto incremental = run_profile_case<RowCount, false, true>();
    const auto full_scan = run_profile_case<RowCount, false, false>();
    if (incremental.final_value != full_scan.final_value)
        throw std::runtime_error{"incremental and full-scan sums differ"};
    print_profile_result<RowCount, false>(incremental, "incremental");
    print_profile_result<RowCount, false>(full_scan, "full_scan");
}

} // namespace

int main(int argc, char **argv) {
    const auto profile_nodes = argc == 2 && std::string_view{argv[1]} == "--profile-nodes";
    if (argc > 2 || (argc == 2 && !profile_nodes))
        throw std::runtime_error{"usage: bg_frame_incremental_sum_benchmark [--profile-nodes]"};

    if (profile_nodes) {
        std::cout << std::setprecision(10)
                  << "source_rows,active_rows,cell_type,input,implementation,changed_rows_per_cycle,profile_clock,"
                     "node_evaluations_per_cycle,node_median_ticks,node_p95_ticks,node_median_ms,node_p95_ms,"
                     "checkpoint_ticks_per_ms_median,checkpoint_ticks_per_ms_p95,final_sum\n";
        print_profile_row<1'000>(false);
        print_profile_row<10'000>(false);
        print_profile_row<100'000>(false);
        print_profile_row<1'000>(true);
        print_profile_row<10'000>(true);
        print_profile_row<100'000>(true);
        return 0;
    }

    std::cout << "source_rows,active_rows,cell_type,input,changed_rows_per_cycle,incremental_median_ms,incremental_p95_ms,"
                 "full_scan_median_ms,full_scan_p95_ms,full_scan_over_incremental,final_sum\n";
    print_row<1'000>(false);
    print_row<10'000>(false);
    print_row<100'000>(false);
    print_row<1'000>(true);
    print_row<10'000>(true);
    print_row<100'000>(true);
}
