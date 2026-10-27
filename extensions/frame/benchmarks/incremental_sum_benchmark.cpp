#include <bg/frame/frame.hpp>
#include <bg/frame/tsc.hpp>
#include <bg/graph/compiled_graph.hpp>
#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/runtime_graph.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/library/operators.hpp>
#include <bg/node.hpp>
#include <bg/util/scope_exit.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
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
void wire_sum(Values values, wp<TS<i64>> cycle) {
    if constexpr (Incremental) {
        const auto result = operators::sum_(values).template as<TS<i64>>();
        static_cast<void>(node_template<ResultSink<Filtered>>()(cycle, result));
    } else {
        const auto result = node_template<FullScanSum>()(values).template as<TS<i64>>();
        static_cast<void>(node_template<ResultSink<Filtered>>()(cycle, result));
    }
}

struct Timing {
    double median_ms{};
    double p95_ms{};
    i64 final_value{};
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
            wire_sum<Filtered, Incremental>(values, cycle);
        } else {
            const auto values = node_template<ColumnSource<RowCount>>()(cycle);
            wire_sum<Filtered, Incremental>(values, cycle);
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

} // namespace

int main() {
    std::cout << "source_rows,active_rows,cell_type,input,changed_rows_per_cycle,incremental_median_ms,incremental_p95_ms,"
                 "full_scan_median_ms,full_scan_p95_ms,full_scan_over_incremental,final_sum\n";
    print_row<1'000>(false);
    print_row<10'000>(false);
    print_row<100'000>(false);
    print_row<1'000>(true);
    print_row<10'000>(true);
    print_row<100'000>(true);
}
