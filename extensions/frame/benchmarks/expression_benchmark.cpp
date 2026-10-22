#include <bg/frame/expression.hpp>
#include <bg/frame/frame.hpp>
#include <bg/graph/compiled_graph.hpp>
#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/runtime_graph.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/library.hpp>
#include <bg/node.hpp>
#include <bg/util/scope_exit.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {

using namespace bg;
using namespace bg::frame;

constexpr auto row_count = std::size_t{100'000};
constexpr auto hole_count = std::size_t{10'000};
constexpr auto changes_per_cycle = std::size_t{1'000};
constexpr auto group_size = changes_per_cycle / 2;
constexpr auto warmup_cycles = std::size_t{2};
constexpr auto measured_cycles = std::size_t{11};
constexpr auto scheduled_cycles = warmup_cycles + measured_cycles;
constexpr auto total_cycles = scheduled_cycles + 1;

enum class Operation { payload, predicate_preserving, predicate_changing, threshold_preserving,
                       threshold_changing, row_add_remove };
enum class Layout { clustered_holes, scattered_holes };

template <template <typename, auto...> typename Field> struct Rows {
    Field<i64> id;
    Field<i64> score;
    Field<i64> payload;

    static SchemaMetadata metadata() { return key_metadata<"id">(); }
};

struct Timing {
    double median_ms{};
    double minimum_ms{};
    double maximum_ms{};
};

Timing summarize(std::span<const double> samples) {
    auto sorted = std::vector<double>{samples.begin(), samples.end()};
    std::ranges::sort(sorted);
    return {sorted[sorted.size() / 2], sorted.front(), sorted.back()};
}

double milliseconds(std::chrono::steady_clock::duration duration) {
    return std::chrono::duration<double, std::milli>{duration}.count();
}

template <typename Definition> const auto &node_template() {
    static const auto value = node<Definition>();
    return value;
}

struct CycleClock {
    static constexpr bool uses_timed_scheduling = true;

    State<i64> cycle;
    Out<TS<i64>> output;

    void start(EvaluationContext *context) const {
        *cycle = 0;
        output.set(0);
        context->schedule_at(context->evaluation_time() + EvaluationTime::duration{1});
    }

    void evaluate(EvaluationContext *context) const {
        output.set(++*cycle);
        context->schedule_at(context->evaluation_time() + EvaluationTime::duration{1});
    }
};

template <Operation Op> struct ThresholdSource {
    In<TS<i64>> cycle;
    State<i64> current;
    Out<TS<i64>> output;

    void start() const {
        *current = 50;
        output.set(*current);
    }

    void evaluate() const {
        if (*cycle == 0)
            return;
        if constexpr (Op == Operation::threshold_preserving || Op == Operation::threshold_changing) {
            const auto next = Op == Operation::threshold_preserving
                                  ? i64{50} + ((*cycle % 2) == 0 ? 0 : 1)
                                  : ((*cycle % 2) == 0 ? i64{50} : i64{101});
            if (next == *current)
                return;
            *current = next;
            output.set(next);
        }
    }
};

struct SourceState {
    std::array<row_id_t, row_count> row_ids{};
    std::array<row_id_t, row_count / 2> selected_keys{};
    std::array<row_id_t, row_count / 2> unselected_keys{};
    std::array<row_id_t, hole_count / 2> free_selected_keys{};
    std::array<row_id_t, hole_count / 2> free_unselected_keys{};
    std::size_t selected_count{};
    std::size_t unselected_count{};
    std::size_t free_selected_count{};
    std::size_t free_unselected_count{};
    std::array<row_id_t, group_size> selected_targets{};
    std::array<row_id_t, group_size> unselected_targets{};
};

struct SourceChanges {
    std::size_t changed{};
    std::size_t added{};
    std::size_t removed{};
};

struct FilterChanges {
    std::size_t changed{};
    std::size_t removed{};
};

template <Operation Op, Layout SourceLayout> struct FrameSource {
    In<TS<i64>> cycle;
    State<SourceState> state;
    Out<TSF<Rows>> output;

    inline static auto changes = std::array<SourceChanges, total_cycles>{};
    inline static auto mutation_ms = std::array<double, total_cycles>{};

    static bool is_hole(row_id_t id) {
        if constexpr (SourceLayout == Layout::clustered_holes)
            return id >= row_count - hole_count;
        return id % 10 == 0;
    }

    void start() const {
        changes.fill({});
        mutation_ms.fill(0);

        for (auto id = row_id_t{}; id != row_count; ++id) {
            const auto score = id % 100 < 50 ? i64{100} : i64{0};
            state->row_ids[id] = output.add(Row<Rows>{.id = Cell<i64>{static_cast<i64>(id)},
                                                       .score = Cell<i64>{score},
                                                       .payload = Cell<i64>{static_cast<i64>(id) * 17}});
        }

        for (auto id = row_id_t{}; id != row_count; ++id)
            if (is_hole(id))
                output.erase(state->row_ids[id]);

        if constexpr (Op == Operation::row_add_remove) {
            for (auto id = row_id_t{}; id != row_count; ++id) {
                if (id % 100 < 50) {
                    if (is_hole(id))
                        state->free_selected_keys[state->free_selected_count++] = id;
                    else
                        state->selected_keys[state->selected_count++] = id;
                } else if (is_hole(id)) {
                    state->free_unselected_keys[state->free_unselected_count++] = id;
                } else {
                    state->unselected_keys[state->unselected_count++] = id;
                }
            }
            return;
        }

        auto selected_count = std::size_t{};
        auto unselected_count = std::size_t{};
        for (auto id = row_id_t{}; id != row_count; ++id) {
            if (is_hole(id))
                continue;
            if (id % 100 < 50 && selected_count != group_size)
                state->selected_targets[selected_count++] = id;
            if (id % 100 >= 50 && unselected_count != group_size)
                state->unselected_targets[unselected_count++] = id;
        }
        if (selected_count != group_size || unselected_count != group_size)
            throw std::runtime_error{"source layout did not provide the requested update targets"};
    }

    void evaluate() const {
        const auto index = static_cast<std::size_t>(*cycle);
        auto &counts = changes.at(index);
        if (index == 0)
            return;
        const auto mutation_start = std::chrono::steady_clock::now();

        if constexpr (Op == Operation::payload) {
            for (const auto id : state->selected_targets)
                output.set(state->row_ids[id], RowPatch<Rows>{.payload = static_cast<i64>(id) * 17 + *cycle});
            for (const auto id : state->unselected_targets)
                output.set(state->row_ids[id], RowPatch<Rows>{.payload = static_cast<i64>(id) * 17 + *cycle});
            counts.changed = changes_per_cycle;
        } else if constexpr (Op == Operation::predicate_preserving) {
            const auto selected_score = (*cycle % 2) == 0 ? i64{100} : i64{99};
            const auto unselected_score = (*cycle % 2) == 0 ? i64{0} : i64{1};
            for (const auto id : state->selected_targets)
                output.set(state->row_ids[id], RowPatch<Rows>{.score = selected_score});
            for (const auto id : state->unselected_targets)
                output.set(state->row_ids[id], RowPatch<Rows>{.score = unselected_score});
            counts.changed = changes_per_cycle;
        } else if constexpr (Op == Operation::predicate_changing) {
            const auto selected_score = (*cycle % 2) == 0 ? i64{100} : i64{0};
            const auto unselected_score = (*cycle % 2) == 0 ? i64{0} : i64{100};
            for (const auto id : state->selected_targets)
                output.set(state->row_ids[id], RowPatch<Rows>{.score = selected_score});
            for (const auto id : state->unselected_targets)
                output.set(state->row_ids[id], RowPatch<Rows>{.score = unselected_score});
            counts.changed = changes_per_cycle;
        } else if constexpr (Op == Operation::row_add_remove) {
            auto add_selected = std::array<row_id_t, group_size>{};
            auto add_unselected = std::array<row_id_t, group_size>{};
            auto remove_selected = std::array<row_id_t, group_size>{};
            auto remove_unselected = std::array<row_id_t, group_size>{};
            for (auto position = std::size_t{}; position != group_size; ++position) {
                add_selected[position] = state->free_selected_keys[--state->free_selected_count];
                add_unselected[position] = state->free_unselected_keys[--state->free_unselected_count];
                remove_selected[position] = state->selected_keys[--state->selected_count];
                remove_unselected[position] = state->unselected_keys[--state->unselected_count];
            }

            for (const auto id : remove_selected)
                if (!output.erase(state->row_ids[id]))
                    throw std::runtime_error{"could not remove a selected row"};
            for (const auto id : remove_unselected)
                if (!output.erase(state->row_ids[id]))
                    throw std::runtime_error{"could not remove an unselected row"};

            const auto add_row = [&](row_id_t id) {
                const auto score = id % 100 < 50 ? i64{100} : i64{0};
                state->row_ids[id] = output.add(Row<Rows>{.id = Cell<i64>{static_cast<i64>(id)},
                                                          .score = Cell<i64>{score},
                                                          .payload = Cell<i64>{static_cast<i64>(id) * 17 + *cycle}});
            };
            for (const auto id : add_selected)
                add_row(id);
            for (const auto id : add_unselected)
                add_row(id);

            for (const auto id : remove_selected)
                state->free_selected_keys[state->free_selected_count++] = id;
            for (const auto id : remove_unselected)
                state->free_unselected_keys[state->free_unselected_count++] = id;
            for (const auto id : add_selected)
                state->selected_keys[state->selected_count++] = id;
            for (const auto id : add_unselected)
                state->unselected_keys[state->unselected_count++] = id;
            counts.added = changes_per_cycle;
            counts.removed = changes_per_cycle;
        }
        mutation_ms[index] = milliseconds(std::chrono::steady_clock::now() - mutation_start);
    }
};

template <Operation Op, Layout SourceLayout> struct CountSink {
    In<TSF<Rows>> frame;
    In<TS<i64>> cycle;

    inline static auto selected_rows = std::array<std::size_t, total_cycles>{};
    inline static auto changes = std::array<FilterChanges, total_cycles>{};

    void start() const {
        selected_rows.fill(0);
        changes.fill({});
    }

    void evaluate() const {
        const auto index = static_cast<std::size_t>(*cycle);
        selected_rows.at(index) = frame.size();
        auto &delta = changes.at(index);
        frame.for_each_changed([&](row_id_t) { ++delta.changed; });
        frame.for_each_removed([&](row_id_t) { ++delta.removed; });
    }
};

struct CaseResult {
    std::string_view operation;
    std::string_view layout;
    std::size_t source_rows{};
    std::size_t selected_min{};
    std::size_t selected_max{};
    double density_min{};
    double density_max{};
    SourceChanges source_changes{};
    std::size_t threshold_updates{};
    FilterChanges filter_changes_min{};
    FilterChanges filter_changes_max{};
    double runtime_allocation_ms{};
    double start_and_seed_ms{};
    double first_evaluation_ms{};
    Timing source_mutation;
    Timing steady_cycle;
    double stop_ms{};
    double destruction_ms{};
};

constexpr auto operation_names = std::array<std::string_view, 6>{
    "payload-only update", "predicate update preserving membership", "predicate update changing membership",
    "threshold update preserving membership", "threshold update changing membership", "row add/remove"};

constexpr auto layout_names = std::array<std::string_view, 2>{"clustered holes", "scattered holes"};

template <Operation Op, Layout SourceLayout> std::size_t expected_selected(std::size_t cycle) {
    if constexpr (Op == Operation::threshold_changing)
        return cycle % 2 == 0 ? 45'000 : 0;
    return 45'000;
}

template <Operation Op> FilterChanges expected_filter_changes(std::size_t cycle) {
    if (cycle == 0)
        return {45'000, 0};
    if constexpr (Op == Operation::payload || Op == Operation::predicate_preserving)
        return {group_size, 0};
    if constexpr (Op == Operation::predicate_changing || Op == Operation::row_add_remove)
        return {group_size, group_size};
    if constexpr (Op == Operation::threshold_changing)
        return cycle % 2 == 0 ? FilterChanges{45'000, 0} : FilterChanges{0, 45'000};
    return {};
}

template <Operation Op, Layout SourceLayout> CaseResult run_case() {
    using Source = FrameSource<Op, SourceLayout>;
    using Sink = CountSink<Op, SourceLayout>;

    auto wiring = GraphWiring{};
    {
        auto context = WiringContext{wiring};
        const auto cycle = node_template<CycleClock>()();
        const auto source = node_template<Source>()(cycle);
        const auto threshold = node_template<ThresholdSource<Op>>()(cycle);
        const auto selected = filter(source, col("score") + i64{1} > threshold);
        static_cast<void>(node_template<Sink>()(selected, cycle));
    }
    wiring.finalize();
    const auto compiled = CompiledGraph{wiring};
    auto global_state = GlobalState{};

    auto allocation_start = std::chrono::steady_clock::now();
    auto runtime = std::make_unique<RuntimeGraph>(compiled, global_state);
    const auto allocation_end = std::chrono::steady_clock::now();
    const auto start_time = MIN_DT + EvaluationTime::duration{1};

    auto begin = std::chrono::steady_clock::now();
    runtime->start(start_time);
    const auto start_end = std::chrono::steady_clock::now();
    const auto start_and_seed_ms = milliseconds(start_end - begin);
    auto stop_on_exit = util::scope_exit{[&]() noexcept { runtime->stop(); }};

    begin = std::chrono::steady_clock::now();
    runtime->evaluate(start_time);
    const auto first_evaluation_end = std::chrono::steady_clock::now();
    const auto first_evaluation_ms = milliseconds(first_evaluation_end - begin);
    if (Sink::selected_rows[0] != expected_selected<Op, SourceLayout>(0) ||
        Sink::changes[0].changed != 45'000 || Sink::changes[0].removed != 0)
        throw std::runtime_error{"initial filtered row count or delta differs from the expected seed"};

    auto graph_times = std::array<double, scheduled_cycles>{};
    for (auto cycle = std::size_t{1}; cycle <= scheduled_cycles; ++cycle) {
        begin = std::chrono::steady_clock::now();
        runtime->evaluate(start_time + EvaluationTime::duration{static_cast<EvaluationTime::duration::rep>(cycle)});
        const auto end = std::chrono::steady_clock::now();
        graph_times[cycle - 1] = milliseconds(end - begin);
        const auto observed = Sink::selected_rows[cycle];
        if (observed != expected_selected<Op, SourceLayout>(cycle))
            throw std::runtime_error{"filtered row count differs from the expected cycle count"};
        const auto expected_delta = expected_filter_changes<Op>(cycle);
        const auto actual_delta = Sink::changes[cycle];
        if (actual_delta.changed != expected_delta.changed || actual_delta.removed != expected_delta.removed)
            throw std::runtime_error{"filtered changed or removed row count differs from the expected cycle delta"};
    }

    const auto stop_start = std::chrono::steady_clock::now();
    runtime->stop();
    const auto stop_end = std::chrono::steady_clock::now();
    stop_on_exit.release();

    const auto selected_samples = std::span{Sink::selected_rows}.subspan(1 + warmup_cycles, measured_cycles);
    const auto [selected_min_it, selected_max_it] =
        std::ranges::minmax_element(selected_samples);
    const auto selected_min = *selected_min_it;
    const auto selected_max = *selected_max_it;
    const auto filter_changes = std::span{Sink::changes}.subspan(1 + warmup_cycles, measured_cycles);
    const auto [filter_changes_min_it, filter_changes_max_it] =
        std::ranges::minmax_element(filter_changes, {}, &FilterChanges::changed);
    const auto [filter_removed_min_it, filter_removed_max_it] =
        std::ranges::minmax_element(filter_changes, {}, &FilterChanges::removed);
    const auto filter_changes_min = FilterChanges{filter_changes_min_it->changed, filter_removed_min_it->removed};
    const auto filter_changes_max = FilterChanges{filter_changes_max_it->changed, filter_removed_max_it->removed};
    const auto measured = std::span{graph_times}.subspan(warmup_cycles, measured_cycles);
    const auto mutation_samples = std::span{Source::mutation_ms}.subspan(1 + warmup_cycles, measured_cycles);
    const auto final_counts = Source::changes[scheduled_cycles];

    const auto destruction_start = std::chrono::steady_clock::now();
    runtime.reset();
    const auto destruction_end = std::chrono::steady_clock::now();
    constexpr auto threshold_updates = Op == Operation::threshold_preserving ||
                                               Op == Operation::threshold_changing
                                           ? std::size_t{1}
                                           : std::size_t{0};

    return {operation_names[static_cast<std::size_t>(Op)],
            layout_names[static_cast<std::size_t>(SourceLayout)],
            row_count - hole_count,
            selected_min,
            selected_max,
            100.0 * static_cast<double>(selected_min) / static_cast<double>(row_count - hole_count),
            100.0 * static_cast<double>(selected_max) / static_cast<double>(row_count - hole_count),
            final_counts,
            threshold_updates,
            filter_changes_min,
            filter_changes_max,
            milliseconds(allocation_end - allocation_start),
            start_and_seed_ms,
            first_evaluation_ms,
            summarize(mutation_samples),
            summarize(measured),
            milliseconds(stop_end - stop_start),
            milliseconds(destruction_end - destruction_start)};
}

void write_header(std::ostream &report) {
    report << std::fixed << std::setprecision(3)
           << "Expression benchmark: RuntimeGraph source -> fused filter(source, col(\"score\") + 1 > threshold) -> "
              "count sink. Each source seeds 100,000 keyed rows and removes 10,000 IDs, leaving 90,000 rows. "
              "Clustered layout removes the final 10,000 IDs; scattered layout removes every tenth ID. Initial "
              "scores are 100 for IDs where id % 100 < 50 and 0 otherwise, with threshold 50, so both layouts "
              "select 45,000 rows. The cycle clock is separately scheduled; source and threshold consume its tick, "
              "and the sink consumes both the filtered TSF and the tick. Threshold nodes publish only when the "
              "threshold changes. Graph wiring and compilation are excluded. Runtime allocation, start plus seed, "
              "first evaluation, steady cycles, stop, and destruction are timed separately. The first evaluation "
              "and two warmup cycles are excluded from steady timings; eleven cycles are measured. Lifecycle "
              "timings are single samples. Steady cycle timings are median [min, max] in milliseconds. Counts "
              "and filtered row totals are validated outside each timed evaluation.\n\n"
           << "| Operation | Layout | Live rows | Selected rows, measured min-max | Selected density, measured min-max | "
              "Updates/cycle | Changed rows/cycle | Added rows/cycle | Removed rows/cycle | "
              "Threshold updates/cycle | Filter changed min-max | Filter removed min-max | "
              "Runtime allocation ms | Start + seed ms | First evaluation ms | Source mutation ms | "
              "Steady cycle ms | Stop ms | Destruction ms |\n"
              "|:---|:---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|:---:|---:|---:|\n";
}

void write_result(std::ostream &report, const CaseResult &result) {
    const auto print_timing = [&](const Timing &timing) {
        report << timing.median_ms << " [" << timing.minimum_ms << ", " << timing.maximum_ms << "]";
    };
    report << "| " << result.operation << " | " << result.layout << " | " << result.source_rows << " | "
           << result.selected_min << "-" << result.selected_max << " | " << result.density_min << "%-"
           << result.density_max << "% | "
           << result.source_changes.changed + result.source_changes.added + result.source_changes.removed +
                  result.threshold_updates
           << " | " << result.source_changes.changed << " | " << result.source_changes.added << " | "
           << result.source_changes.removed << " | " << result.threshold_updates << " | "
           << result.filter_changes_min.changed << "-" << result.filter_changes_max.changed << " | "
           << result.filter_changes_min.removed << "-" << result.filter_changes_max.removed << " | "
           << result.runtime_allocation_ms << " | " << result.start_and_seed_ms << " | "
           << result.first_evaluation_ms << " | ";
    print_timing(result.source_mutation);
    report << " | ";
    print_timing(result.steady_cycle);
    report << " | " << result.stop_ms << " | " << result.destruction_ms << " |\n";

}

void write_csv_result(std::ostream &csv, const CaseResult &result) {
    csv << '"' << result.operation << "\",\"" << result.layout << "\"," << result.source_rows << ','
        << result.selected_min << ',' << result.selected_max << ',' << result.density_min << ','
        << result.density_max << ','
        << result.source_changes.changed + result.source_changes.added + result.source_changes.removed +
               result.threshold_updates
        << ',' << result.source_changes.changed << ',' << result.source_changes.added << ','
        << result.source_changes.removed << ',' << result.threshold_updates << ','
        << result.filter_changes_min.changed << ',' << result.filter_changes_max.changed << ','
        << result.filter_changes_min.removed << ',' << result.filter_changes_max.removed << ','
        << result.runtime_allocation_ms << ',' << result.start_and_seed_ms << ','
        << result.first_evaluation_ms << ',' << result.source_mutation.median_ms << ','
        << result.source_mutation.minimum_ms << ',' << result.source_mutation.maximum_ms << ','
        << result.steady_cycle.median_ms << ',' << result.steady_cycle.minimum_ms << ','
        << result.steady_cycle.maximum_ms << ',' << result.stop_ms << ',' << result.destruction_ms << '\n';
}

} // namespace

int main(int argc, char **argv) {
    auto report_path = std::string_view{"expression_benchmark_results.md"};
    auto csv_path = std::string_view{"expression_benchmark_results.csv"};
    for (auto argument = 1; argument < argc; argument += 2) {
        if (argument + 1 >= argc) {
            std::cerr << "usage: bg_frame_expression_benchmark [--report path] [--csv path]\n";
            return 2;
        }
        const auto option = std::string_view{argv[argument]};
        if (option == "--report")
            report_path = argv[argument + 1];
        else if (option == "--csv")
            csv_path = argv[argument + 1];
        else {
            std::cerr << "unknown option: " << option << '\n';
            return 2;
        }
    }

    auto report_file = std::ofstream{std::string{report_path}};
    auto csv_file = std::ofstream{std::string{csv_path}};
    if (!report_file || !csv_file) {
        std::cerr << "could not open benchmark report or CSV output path\n";
        return 1;
    }

    write_header(std::cout);
    write_header(report_file);
    csv_file << "operation,layout,source_rows,selected_min,selected_max,density_min_percent,density_max_percent,"
                "updates_per_cycle,changed_rows_per_cycle,added_rows_per_cycle,removed_rows_per_cycle,"
                "threshold_updates_per_cycle,filter_changed_min,filter_changed_max,filter_removed_min,"
                "filter_removed_max,runtime_allocation_ms,start_and_seed_ms,first_evaluation_ms,"
                "source_mutation_median_ms,source_mutation_min_ms,source_mutation_max_ms,"
                "steady_cycle_median_ms,steady_cycle_min_ms,steady_cycle_max_ms,stop_ms,destruction_ms\n";
    csv_file << std::fixed << std::setprecision(6);

    for (auto operation = std::size_t{}; operation != operation_names.size(); ++operation) {
        for (auto layout = std::size_t{}; layout != layout_names.size(); ++layout) {
            CaseResult result;
            switch (static_cast<Operation>(operation)) {
            case Operation::payload:
                result = layout == 0 ? run_case<Operation::payload, Layout::clustered_holes>()
                                     : run_case<Operation::payload, Layout::scattered_holes>();
                break;
            case Operation::predicate_preserving:
                result = layout == 0 ? run_case<Operation::predicate_preserving, Layout::clustered_holes>()
                                     : run_case<Operation::predicate_preserving, Layout::scattered_holes>();
                break;
            case Operation::predicate_changing:
                result = layout == 0 ? run_case<Operation::predicate_changing, Layout::clustered_holes>()
                                     : run_case<Operation::predicate_changing, Layout::scattered_holes>();
                break;
            case Operation::threshold_preserving:
                result = layout == 0 ? run_case<Operation::threshold_preserving, Layout::clustered_holes>()
                                     : run_case<Operation::threshold_preserving, Layout::scattered_holes>();
                break;
            case Operation::threshold_changing:
                result = layout == 0 ? run_case<Operation::threshold_changing, Layout::clustered_holes>()
                                     : run_case<Operation::threshold_changing, Layout::scattered_holes>();
                break;
            case Operation::row_add_remove:
                result = layout == 0 ? run_case<Operation::row_add_remove, Layout::clustered_holes>()
                                     : run_case<Operation::row_add_remove, Layout::scattered_holes>();
                break;
            }
            write_result(report_file, result);
            write_csv_result(csv_file, result);
            write_result(std::cout, result);
        }
    }
}
