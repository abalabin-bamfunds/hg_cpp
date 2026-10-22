#include <bg/frame/arrow.hpp>
#include <bg/frame/expression.hpp>
#include <bg/graph/compiled_graph.hpp>
#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/runtime_graph.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/node.hpp>
#include <bg/util/scope_exit.hpp>

#include <arrow/api.h>
#include <arrow/compute/api.h>
#include <arrow/compute/initialize.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <span>
#include <tuple>
#include <vector>

namespace {

using namespace bg;
using namespace bg::frame;

constexpr auto row_count = std::size_t{100'000};
constexpr auto update_limit = std::size_t{10'000};
constexpr auto update_stride = std::size_t{97};
constexpr auto warmup_cycles = std::size_t{2};
constexpr auto measured_cycles = std::size_t{11};
constexpr auto total_cycles = warmup_cycles + measured_cycles;

template <template <typename, auto...> typename Field> struct Rows {
    Field<i64> selected;
    Field<i64> payload;
};

enum class SumMethod { native, borrowed_arrow, dense_gather_arrow };

struct BenchmarkCase {
    std::string_view name;
    std::size_t selection_percent;
    bool scattered_selection;
    std::size_t deletions_per_cycle;
    bool scattered_deletion;
};

template <typename Definition> const auto &node_template() {
    static const auto value = node<Definition>();
    return value;
}

constexpr auto benchmark_cases = std::array{
    BenchmarkCase{"50% clustered selection, clustered deletion", 50, false, 128, false},
    BenchmarkCase{"50% scattered selection, clustered deletion", 50, true, 128, false},
    BenchmarkCase{"50% clustered selection, scattered deletion", 50, false, 128, true},
    BenchmarkCase{"50% scattered selection, scattered deletion", 50, true, 128, true},
    BenchmarkCase{"100% selection, no deletion", 100, false, 0, false},
    BenchmarkCase{"10% scattered selection and deletion", 10, true, 768, true},
};

struct Timing {
    double median_ms;
    double minimum_ms;
    double maximum_ms;
};

Timing summarize(std::span<const double> samples) {
    auto sorted = std::vector<double>{samples.begin(), samples.end()};
    std::ranges::sort(sorted);
    return {sorted[sorted.size() / 2], sorted.front(), sorted.back()};
}

template <std::size_t SelectionPercent, bool ScatteredSelection> bool is_selected(row_id_t id) {
    if constexpr (SelectionPercent == 100)
        return true;
    if constexpr (ScatteredSelection && SelectionPercent == 50)
        return id % 2 == 0;
    if constexpr (ScatteredSelection)
        return (static_cast<std::uint64_t>(id) * 2'654'435'761ULL) % 100 < SelectionPercent;
    return id < row_count * SelectionPercent / 100;
}

template <std::size_t DeletionsPerCycle, bool ScatteredDeletion>
row_id_t deleted_id(std::size_t cycle, std::size_t position) {
    if constexpr (DeletionsPerCycle == 0)
        return 0;
    if constexpr (ScatteredDeletion) {
        const auto sequence = cycle * DeletionsPerCycle + position;
        return static_cast<row_id_t>(20'000 + (sequence * 7'919) % 80'000);
    }
    return static_cast<row_id_t>(25'000 + cycle * DeletionsPerCycle + position);
}

template <std::size_t SelectionPercent, bool ScatteredSelection, std::size_t DeletionsPerCycle,
          bool ScatteredDeletion>
struct MutationSource {
    static constexpr bool uses_timed_scheduling = true;

    State<std::size_t> cycle;
    Out<TSF<Rows>> output;

    inline static auto mutation_time_ms = std::array<double, total_cycles>{};

    void start(EvaluationContext *context) const {
        *cycle = 0;
        mutation_time_ms.fill(0);
        const auto time = context->evaluation_time();
        for (auto id = row_id_t{}; id != row_count; ++id) {
            output.add(Rows<Cell>{.selected = Cell<i64>{is_selected<SelectionPercent, ScatteredSelection>(id)
                                                               ? 1
                                                               : 0},
                                  .payload = Cell<i64>{static_cast<i64>(id) * 17 + 13}});
        }
        context->schedule_at(time + EvaluationTime::duration{1});
    }

    void evaluate(EvaluationContext *context) const {
        const auto current_cycle = ++*cycle;
        const auto mutation_start = std::chrono::steady_clock::now();
        for (auto id = row_id_t{}; id < update_limit; id += update_stride) {
            if (is_selected<SelectionPercent, ScatteredSelection>(id)) {
                output.set(id, RowPatch<Rows>{.payload = static_cast<i64>(id) * 17 + 13 +
                                                          static_cast<i64>(current_cycle)});
            }
        }

        for (auto position = std::size_t{}; position != DeletionsPerCycle; ++position)
            output.erase(deleted_id<DeletionsPerCycle, ScatteredDeletion>(current_cycle - 1, position));
        mutation_time_ms[current_cycle - 1] =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - mutation_start).count();

        context->schedule_at(context->evaluation_time() + EvaluationTime::duration{1});
    }
};

std::int64_t arrow_sum(const std::shared_ptr<arrow::ChunkedArray> &values) {
    auto result = arrow::compute::CallFunction("sum", {arrow::Datum{values}});
    if (!result.ok())
        throw std::runtime_error{result.status().ToString()};
    const auto &scalar = result->scalar_as<arrow::Int64Scalar>();
    if (!scalar.is_valid)
        throw std::runtime_error{"Arrow sum returned an invalid scalar"};
    return scalar.value;
}

template <SumMethod Method> struct SumConsumer {
    In<TSC<i64>> column;
    State<std::size_t> cycle;
    Out<TS<i64>> output;

    inline static auto last_sum = std::int64_t{};
    inline static auto sum_time_ms = std::array<double, total_cycles>{};
    inline static auto selected_rows = std::array<std::size_t, total_cycles>{};
    inline static auto chunk_counts = std::array<std::size_t, total_cycles>{};

    void start() const { *cycle = 0; }

    void evaluate() const {
        const auto index = (*cycle)++;
        if (index >= total_cycles)
            throw std::runtime_error{"sum consumer ran more often than the benchmark cycles"};

        const auto start_time = std::chrono::steady_clock::now();
        auto value = std::int64_t{};
        auto chunks = std::size_t{};
        if constexpr (Method == SumMethod::native) {
            column.for_each_batch([&](const auto &batch) {
                ++chunks;
                for (auto position = std::size_t{}; position != batch.ids.size(); ++position)
                    if (batch.cells.is_valid(position))
                        value += batch.cells.values[position];
            });
        } else if constexpr (Method == SumMethod::borrowed_arrow) {
            auto exported = borrow_arrow(column.value_view().value());
            if (!exported.ok())
                throw std::runtime_error{exported.status().ToString()};
            const auto values = std::move(exported).ValueUnsafe();
            chunks = static_cast<std::size_t>(values->num_chunks());
            value = arrow_sum(values);
        } else {
            auto exported = copy_to_arrow(column.value_view().value());
            if (!exported.ok())
                throw std::runtime_error{exported.status().ToString()};
            const auto values = std::move(exported).ValueUnsafe();
            chunks = static_cast<std::size_t>(values->num_chunks());
            value = arrow_sum(values);
        }
        const auto elapsed = std::chrono::steady_clock::now() - start_time;
        sum_time_ms[index] = std::chrono::duration<double, std::milli>(elapsed).count();
        selected_rows[index] = column.size();
        chunk_counts[index] = chunks;
        last_sum = value;
        output.set(value);
    }
};

template <std::size_t SelectionPercent, bool ScatteredSelection>
std::vector<i64> expected_values(std::size_t cycle, const std::vector<bool> &alive) {
    auto values = std::vector<i64>{};
    for (auto id = row_id_t{}; id != row_count; ++id) {
        if (!alive[id] || !is_selected<SelectionPercent, ScatteredSelection>(id))
            continue;
        auto value = static_cast<i64>(id) * 17 + 13;
        if (id < update_limit && id % update_stride == 0)
            value += static_cast<i64>(cycle);
        values.push_back(value);
    }
    return values;
}

std::shared_ptr<arrow::ChunkedArray> plain_arrow(std::span<const i64> values) {
    auto builder = arrow::Int64Builder{};
    auto status = builder.AppendValues(values.data(), static_cast<std::int64_t>(values.size()));
    if (!status.ok())
        throw std::runtime_error{status.ToString()};
    std::shared_ptr<arrow::Array> array;
    status = builder.Finish(&array);
    if (!status.ok())
        throw std::runtime_error{status.ToString()};
    auto result = arrow::ChunkedArray::Make({std::move(array)}, arrow::int64());
    if (!result.ok())
        throw std::runtime_error{result.status().ToString()};
    return std::move(result).ValueUnsafe();
}

std::int64_t sum_values(std::span<const i64> values) {
    auto result = std::int64_t{};
    for (const auto value : values)
        result += value;
    return result;
}

struct RunResult {
    std::string_view name;
    std::string_view method;
    std::size_t selected_rows;
    std::size_t chunks;
    Timing source_mutation;
    Timing graph_cycle;
    Timing sum_method;
    Timing plain_arrow_sum;
    Timing plain_arrow_build_sum_destroy;
};

template <SumMethod Method, std::size_t SelectionPercent, bool ScatteredSelection,
          std::size_t DeletionsPerCycle, bool ScatteredDeletion>
RunResult run_case(const BenchmarkCase &benchmark_case) {
    using Source = MutationSource<SelectionPercent, ScatteredSelection, DeletionsPerCycle, ScatteredDeletion>;
    using Consumer = SumConsumer<Method>;

    Source::mutation_time_ms.fill(0);
    Consumer::last_sum = 0;
    Consumer::sum_time_ms.fill(0);
    Consumer::selected_rows.fill(0);
    Consumer::chunk_counts.fill(0);

    auto wiring = GraphWiring{};
    {
        auto context = WiringContext{wiring};
        const auto source = node_template<Source>()();
        const auto selected_rows = filter(source, col("selected") > i64{0});
        const auto column = select_column<"payload">(selected_rows);
        static_cast<void>(node_template<Consumer>()(column));
    }

    wiring.finalize();
    const auto compiled = CompiledGraph{wiring};
    auto global_state = GlobalState{};
    auto runtime = RuntimeGraph{compiled, global_state};
    const auto start_time = MIN_DT + EvaluationTime::duration{1};
    runtime.start(start_time);
    auto stop_on_exit = util::scope_exit{[&]() noexcept { runtime.stop(); }};

    auto graph_times = std::array<double, total_cycles>{};
    auto alive = std::vector<bool>(row_count, true);
    auto final_values = std::vector<i64>{};
    for (auto cycle = std::size_t{1}; cycle <= total_cycles; ++cycle) {
        for (auto position = std::size_t{}; position != DeletionsPerCycle; ++position)
            alive[deleted_id<DeletionsPerCycle, ScatteredDeletion>(cycle - 1, position)] = false;

        const auto begin = std::chrono::steady_clock::now();
        runtime.evaluate(start_time + EvaluationTime::duration{static_cast<EvaluationTime::duration::rep>(cycle)});
        const auto end = std::chrono::steady_clock::now();
        graph_times[cycle - 1] = std::chrono::duration<double, std::milli>(end - begin).count();

        auto expected = expected_values<SelectionPercent, ScatteredSelection>(cycle, alive);
        if (Consumer::last_sum != sum_values(expected))
            throw std::runtime_error{"frame graph sum differs from the deterministic source values"};
        if (cycle == total_cycles)
            final_values = std::move(expected);
    }
    runtime.stop();
    stop_on_exit.release();

    const auto plain = plain_arrow(final_values);
    const auto expected_sum = sum_values(final_values);
    if (arrow_sum(plain) != expected_sum)
        throw std::runtime_error{"plain Arrow sum differs from the deterministic source values"};
    auto plain_sum_times = std::array<double, measured_cycles>{};
    auto plain_arrow_build_sum_destroy_times = std::array<double, measured_cycles>{};
    for (auto cycle = std::size_t{}; cycle != warmup_cycles; ++cycle)
        static_cast<void>(arrow_sum(plain));
    for (auto &sample : plain_sum_times) {
        const auto begin = std::chrono::steady_clock::now();
        const auto value = arrow_sum(plain);
        const auto end = std::chrono::steady_clock::now();
        if (value != expected_sum)
            throw std::runtime_error{"plain Arrow sum changed during timing"};
        sample = std::chrono::duration<double, std::milli>(end - begin).count();
    }
    const auto measure_plain_arrow_build_sum_destroy = [&] {
        const auto begin = std::chrono::steady_clock::now();
        auto values = plain_arrow(final_values);
        const auto value = arrow_sum(values);
        values.reset();
        const auto end = std::chrono::steady_clock::now();
        if (value != expected_sum)
            throw std::runtime_error{"plain Arrow build and sum changed the deterministic values"};
        return std::chrono::duration<double, std::milli>(end - begin).count();
    };
    for (auto cycle = std::size_t{}; cycle != warmup_cycles; ++cycle)
        static_cast<void>(measure_plain_arrow_build_sum_destroy());
    for (auto &sample : plain_arrow_build_sum_destroy_times)
        sample = measure_plain_arrow_build_sum_destroy();

    const auto measured = std::span{graph_times}.subspan(warmup_cycles);
    const auto source_times = std::span{Source::mutation_time_ms}.subspan(warmup_cycles);
    const auto sum_times = std::span{Consumer::sum_time_ms}.subspan(warmup_cycles);
    constexpr auto method = Method == SumMethod::native
                                ? std::string_view{"native batch sum"}
                            : Method == SumMethod::borrowed_arrow
                                ? std::string_view{"borrowed Arrow + sum"}
                                : std::string_view{"dense gather + sum"};
    return {benchmark_case.name,
            method,
            Consumer::selected_rows.back(),
            Consumer::chunk_counts.back(),
            summarize(source_times),
            summarize(measured),
            summarize(sum_times),
            summarize(plain_sum_times),
            summarize(plain_arrow_build_sum_destroy_times)};
}

template <std::size_t SelectionPercent, bool ScatteredSelection, std::size_t DeletionsPerCycle,
          bool ScatteredDeletion>
void run_case(const BenchmarkCase &benchmark_case, std::ostream &report, std::ostream &mirror,
              std::ostream &csv) {
    const auto results = std::array{
        run_case<SumMethod::native, SelectionPercent, ScatteredSelection, DeletionsPerCycle, ScatteredDeletion>(
            benchmark_case),
        run_case<SumMethod::borrowed_arrow, SelectionPercent, ScatteredSelection, DeletionsPerCycle,
                 ScatteredDeletion>(benchmark_case),
        run_case<SumMethod::dense_gather_arrow, SelectionPercent, ScatteredSelection, DeletionsPerCycle,
                 ScatteredDeletion>(benchmark_case),
    };

    for (const auto &result : results) {
        const auto write_row = [&](std::ostream &output) {
            const auto print_timing = [&](const Timing &timing) {
                output << timing.median_ms << " [" << timing.minimum_ms << ", " << timing.maximum_ms << "]";
            };
            output << "| " << result.name << " | " << result.method << " | " << result.selected_rows << " | "
                   << result.chunks << " | ";
            print_timing(result.source_mutation);
            output << " | ";
            print_timing(result.graph_cycle);
            output << " | ";
            print_timing(result.sum_method);
            output << " | ";
            print_timing(result.plain_arrow_sum);
            output << " | ";
            print_timing(result.plain_arrow_build_sum_destroy);
            output << " |\n";
        };
        write_row(report);
        write_row(mirror);

        csv << '"' << result.name << "\",\"" << result.method << "\"," << result.selected_rows << ','
            << result.chunks << ',' << result.source_mutation.median_ms << ','
            << result.source_mutation.minimum_ms << ',' << result.source_mutation.maximum_ms << ','
            << result.graph_cycle.median_ms << ',' << result.graph_cycle.minimum_ms << ','
            << result.graph_cycle.maximum_ms << ',' << result.sum_method.median_ms << ','
            << result.sum_method.minimum_ms << ',' << result.sum_method.maximum_ms << ','
            << result.plain_arrow_sum.median_ms << ',' << result.plain_arrow_sum.minimum_ms << ','
            << result.plain_arrow_sum.maximum_ms << ',' << result.plain_arrow_build_sum_destroy.median_ms << ','
            << result.plain_arrow_build_sum_destroy.minimum_ms << ','
            << result.plain_arrow_build_sum_destroy.maximum_ms << '\n';
    }
}

} // namespace

int main(int argc, char **argv) {
    auto report_path = std::string_view{"arrow_benchmark_results.md"};
    auto csv_path = std::string_view{"arrow_benchmark_results.csv"};
    for (auto argument = 1; argument < argc; argument += 2) {
        if (argument + 1 >= argc) {
            std::cerr << "usage: bg_frame_arrow_benchmark [--report path] [--csv path]\n";
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

    const auto status = arrow::compute::Initialize();
    if (!status.ok()) {
        std::cerr << status.ToString() << '\n';
        return 1;
    }

    auto report_file = std::ofstream{std::string{report_path}};
    auto csv_file = std::ofstream{std::string{csv_path}};
    if (!report_file || !csv_file) {
        std::cerr << "could not open benchmark report or CSV output path\n";
        return 1;
    }

    const auto write_report_header = [](std::ostream &report) {
        report << std::fixed << std::setprecision(3)
               << "RuntimeGraph source -> filter(selected > 0) -> select_column -> sum consumer.\n"
                  "Each case starts with 100,000 all-valid rows. The 50% cases delete 128 rows per cycle "
                  "(1,664 total); the dense case has no deletions; the 10% scattered case deletes 768 rows "
                  "per cycle (9,984 total).\n"
                  "Payload updates use IDs below 10,000 at stride 97: 104 selected updates per cycle for "
                  "clustered 50% and dense selections, 52 for scattered 50%, and 11 for the 10% scattered "
                  "case.\n"
                  "Graph construction and 100,000-row source seeding are excluded. Two warmup cycles are excluded; "
                  "eleven steady cycles are measured.\n"
                  "Source mutation time covers frame set/erase calls. Graph-cycle time covers the complete "
                  "RuntimeGraph evaluation, including source mutation and the consumer. Consumer time covers "
                  "the selected sum path. Plain Arrow sum uses a prebuilt one-chunk array of final selected "
                  "values. Plain Arrow build + sum + destroy starts from the pre-existing final_values vector, "
                  "builds an owning one-chunk array, computes the sum, and destroys the array. Both baselines "
                  "run outside graph timing. Selected-row and chunk counts are from the final "
                  "measured cycle. Consumer timing includes Arrow export or dense-gather allocation and destruction.\n"
                  "Native sum and Arrow sum each rescan all selected values; there is no incremental aggregate yet. "
                  "When filter membership changes, the implementation rebuilds selected order by scanning the source "
                  "row set to preserve source order; this is a source-code observation, not phase attribution. "
                  "All times are median [min, max] "
                  "milliseconds.\n\n"
                  "| Selection and deletion | Method | Final-cycle selected rows | "
                  "Final-cycle chunks visited/exported | "
                  "Source mutation ms | Graph cycle ms | Consumer sum ms | "
                  "Plain Arrow sum (prebuilt array) ms | "
                  "Plain Arrow build + sum + destroy (pre-existing vector) ms |\n"
                  "|:---|:---|---:|---:|---:|---:|---:|---:|---:|\n";
    };
    write_report_header(std::cout);
    write_report_header(report_file);
    csv_file << "case,method,selected_rows,chunks,source_mutation_median_ms,source_mutation_min_ms,"
                "source_mutation_max_ms,graph_cycle_median_ms,graph_cycle_min_ms,graph_cycle_max_ms,"
                "consumer_sum_median_ms,consumer_sum_min_ms,consumer_sum_max_ms,"
                "plain_arrow_sum_prebuilt_array_median_ms,plain_arrow_sum_prebuilt_array_min_ms,"
                "plain_arrow_sum_prebuilt_array_max_ms,"
                "plain_arrow_build_sum_destroy_preexisting_vector_median_ms,"
                "plain_arrow_build_sum_destroy_preexisting_vector_min_ms,"
                "plain_arrow_build_sum_destroy_preexisting_vector_max_ms\n";

    for (const auto &benchmark_case : benchmark_cases) {
        if (benchmark_case.selection_percent == 100) {
            run_case<100, false, 0, false>(benchmark_case, report_file, std::cout, csv_file);
        } else if (benchmark_case.selection_percent == 10) {
            run_case<10, true, 768, true>(benchmark_case, report_file, std::cout, csv_file);
        } else if (benchmark_case.scattered_selection && benchmark_case.scattered_deletion) {
            run_case<50, true, 128, true>(benchmark_case, report_file, std::cout, csv_file);
        } else if (benchmark_case.scattered_selection) {
            run_case<50, true, 128, false>(benchmark_case, report_file, std::cout, csv_file);
        } else if (benchmark_case.scattered_deletion) {
            run_case<50, false, 128, true>(benchmark_case, report_file, std::cout, csv_file);
        } else {
            run_case<50, false, 128, false>(benchmark_case, report_file, std::cout, csv_file);
        }
    }
}
