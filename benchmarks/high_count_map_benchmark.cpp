#include <bg/evaluation_context.hpp>
#include <bg/evaluation_profile.hpp>
#include <bg/global_state.hpp>
#include <bg/graph.hpp>
#include <bg/graph/compiled_graph.hpp>
#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/runtime_graph.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/map.hpp>
#include <bg/node.hpp>
#include <bg/time_series/tsb/tsb.hpp>
#include <bg/time_series/tsd/tsd_type.hpp>
#include "high_count_map_workload.hpp"

#include <algorithm>
#include <bit>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#if defined(__linux__)
#include <cstdio>
#include <pthread.h>
#include <sched.h>
#include <sys/resource.h>
#include <unistd.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#include <sys/resource.h>
#elif defined(_WIN32)
#define NOMINMAX
#include <psapi.h>
#include <windows.h>
#endif

namespace {

using namespace bg;
using namespace bg::literals;

using clock_type = std::chrono::steady_clock;
using nanoseconds = std::chrono::duration<double, std::nano>;

using namespace high_count_map;
constexpr auto benchmark_start_time = MIN_DT + std::chrono::microseconds{1};

struct BenchmarkControl {
    const Workload *workload{};
    bool measuring{};
    std::size_t cycle{};
    EventChecksum actual;

    void observe(Branch branch, std::int64_t parent_key, std::int64_t item_key, bool present,
                 std::int64_t value) noexcept {
        if (measuring) {
            actual.add(cycle, branch, parent_key, item_key, present, value);
        }
    }
};

BenchmarkControl *control{};

template <template <typename> typename Field> struct BenchmarkInputs {
    Field<TSD<std::int64_t, TS<std::int64_t>>> flat;
    Field<TSD<std::int64_t, TSD<std::int64_t, TS<std::int64_t>>>> parents;
    Field<TS<std::int64_t>> bias;
};

struct BenchmarkSource {
    State<std::size_t> next_step;
    Out<TSB<BenchmarkInputs>> output;

    static constexpr bool uses_timed_scheduling = true;

    void start(EvaluationContext *context) const { context->schedule_now(); }

    void evaluate(EvaluationContext *context) const {
        if (*next_step == 0) {
            populate();
        } else {
            apply(control->workload->cycles[*next_step - 1], *next_step - 1);
        }

        ++*next_step;
        if (*next_step <= control->workload->cycles.size()) {
            context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
        }
    }

    void populate() const {
        const auto &selected_profile = control->workload->selected_profile;
        for (auto key = std::int64_t{}; key < selected_profile.flat_count; ++key) {
            output->flat[key].set(initial_flat_value(key));
        }
        for (auto parent_key = std::int64_t{}; parent_key < selected_profile.parent_count; ++parent_key) {
            for (auto item_key = std::int64_t{}; item_key < child_count; ++item_key) {
                output->parents[parent_key][item_key].set(initial_child_value(parent_key, item_key));
            }
        }
        output->bias.set(1);
    }

    void apply(const CyclePlan &plan, std::size_t cycle) const {
        for (const auto &[key, value] : plan.flat_updates) {
            output->flat[key].set(value);
        }
        for (const auto &[parent_key, item_key, value] : plan.child_updates) {
            output->parents[parent_key][item_key].set(value);
        }

        if (plan.lifecycle == Lifecycle::remove_flat_and_parents) {
            for (const auto key : plan.flat_lifecycle_keys) {
                output->flat.erase(key);
            }
            for (const auto parent_key : plan.parent_lifecycle_keys) {
                output->parents.erase(parent_key);
            }
        } else if (plan.lifecycle == Lifecycle::restore_flat_and_parents) {
            for (const auto key : plan.flat_lifecycle_keys) {
                output->flat[key].set(flat_source_value(cycle, key));
            }
            for (const auto parent_key : plan.parent_lifecycle_keys) {
                for (auto item_key = std::int64_t{}; item_key < child_count; ++item_key) {
                    output->parents[parent_key][item_key].set(child_source_value(cycle, parent_key, item_key));
                }
            }
        } else if (plan.lifecycle == Lifecycle::remove_children) {
            for (const auto &[parent_key, item_key] : plan.child_lifecycle_keys) {
                output->parents[parent_key].erase(item_key);
            }
        } else if (plan.lifecycle == Lifecycle::restore_children) {
            for (const auto &[parent_key, item_key] : plan.child_lifecycle_keys) {
                output->parents[parent_key][item_key].set(child_source_value(cycle, parent_key, item_key));
            }
        }

        if (plan.bias) {
            output->bias.set(*plan.bias);
        }
    }
};

struct AddValues {
    In<TS<std::int64_t>> lhs;
    In<TS<std::int64_t>> rhs;
    State<std::int64_t> evaluation_count;
    Out<TS<std::int64_t>> output;

    void evaluate() const {
        ++*evaluation_count;
        output.set(*lhs + *rhs);
    }
};

struct Multiply {
    In<TS<std::int64_t>> value;
    std::int64_t factor;
    State<std::int64_t> evaluation_count;
    Out<TS<std::int64_t>> output;

    void evaluate() const {
        ++*evaluation_count;
        output.set(*value * factor);
    }
};

struct AddConstant {
    In<TS<std::int64_t>> value;
    std::int64_t addend;
    State<std::int64_t> evaluation_count;
    Out<TS<std::int64_t>> output;

    void evaluate() const {
        ++*evaluation_count;
        output.set(*value + addend);
    }
};

struct FlatSink {
    In<TS<std::int64_t>> value;
    In<TS<std::int64_t, TimeSeriesGuarantee::const_>> key;

    void evaluate() const { control->observe(Branch::flat, 0, *key, true, *value); }
    void stop() const noexcept { control->observe(Branch::flat, 0, *key, false, 0); }
};

struct ChildSink {
    In<TS<std::int64_t>> value;
    In<TS<std::int64_t, TimeSeriesGuarantee::const_>> parent_key;
    In<TS<std::int64_t, TimeSeriesGuarantee::const_>> child_key;

    void evaluate() const { control->observe(Branch::child, *parent_key, *child_key, true, *value); }
    void stop() const noexcept { control->observe(Branch::child, *parent_key, *child_key, false, 0); }
};

const auto benchmark_source = node<BenchmarkSource>();
const auto add_values = node<AddValues>();
const auto multiply = node<Multiply>();
const auto add_constant = node<AddConstant>();
const auto flat_sink = node<FlatSink>();
const auto child_sink = node<ChildSink>();

const auto stage_a = graph(
    "name"_n = "benchmark_stage_a",
    [](wp<TS<std::int64_t>> value, wp<TS<std::int64_t, TimeSeriesGuarantee::const_>> key, wp<TS<std::int64_t>> bias) {
        const auto a1 = add_values(value, bias);
        const auto a2 = multiply(a1, std::int64_t{3});
        const auto a3 = add_values(a2, key);
        const auto a4 = multiply(a3, std::int64_t{5});
        return add_constant(a4, std::int64_t{7});
    });

const auto stage_b = graph(
    "name"_n = "benchmark_stage_b",
    [](wp<TS<std::int64_t>> value, wp<TS<std::int64_t, TimeSeriesGuarantee::const_>> key, wp<TS<std::int64_t>> bias) {
        const auto b1 = add_values(value, bias);
        const auto b2 = multiply(b1, std::int64_t{7});
        const auto b3 = add_values(b2, key);
        const auto b4 = multiply(b3, std::int64_t{3});
        const auto output = add_constant(b4, std::int64_t{11});
        flat_sink(output, key);
        return output;
    });

const auto child_graph =
    graph("name"_n = "benchmark_child",
          [](wp<TS<std::int64_t>> value, wp<TS<std::int64_t, TimeSeriesGuarantee::const_>> child_key,
             wp<TS<std::int64_t, TimeSeriesGuarantee::const_>> parent_key, wp<TS<std::int64_t>> seed,
             wp<TS<std::int64_t>> bias) {
              const auto c1 = add_values(value, seed);
              const auto c2 = multiply(c1, std::int64_t{3});
              const auto c3 = add_values(c2, child_key);
              const auto c4 = add_values(c3, bias);
              const auto output = multiply(c4, std::int64_t{7});
              child_sink(output, parent_key, child_key);
              return output;
          });

const auto parent_graph =
    graph("name"_n = "benchmark_parent",
          [](wp<TSD<std::int64_t, TS<std::int64_t>>> children,
             wp<TS<std::int64_t, TimeSeriesGuarantee::const_>> parent_key, wp<TS<std::int64_t>> bias) {
              const auto p1 = add_values(bias, parent_key);
              const auto seed = multiply(p1, std::int64_t{5});

              // The callable graph needs a concrete return type; map returns an untyped wiring point.
              return map(child_graph, children, parent_key, seed, bias, "__key_arg__"_n = 1)
                  .as<TSD<std::int64_t, REF<TS<std::int64_t>, TimeSeriesGuarantee::deduplicated>>>();
          });

std::uint64_t elapsed_nanoseconds(auto &&function) {
    const auto start = clock_type::now();
    std::forward<decltype(function)>(function)();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(clock_type::now() - start).count());
}

std::size_t current_rss() noexcept {
#if defined(__linux__)
    auto resident_pages = std::size_t{};
    if (auto *file = std::fopen("/proc/self/statm", "r")) {
        auto ignored_pages = std::size_t{};
        if (std::fscanf(file, "%zu %zu", &ignored_pages, &resident_pages) != 2) {
            resident_pages = 0;
        }
        std::fclose(file);
    }
    return resident_pages * static_cast<std::size_t>(::sysconf(_SC_PAGESIZE));
#elif defined(__APPLE__)
    auto info = mach_task_basic_info{};
    auto count = mach_msg_type_number_t{MACH_TASK_BASIC_INFO_COUNT};
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info), &count) !=
        KERN_SUCCESS) {
        return 0;
    }
    return static_cast<std::size_t>(info.resident_size);
#elif defined(_WIN32)
    auto counters = PROCESS_MEMORY_COUNTERS{};
    counters.cb = sizeof(counters);
    if (!GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters))) {
        return 0;
    }
    return static_cast<std::size_t>(counters.WorkingSetSize);
#else
    return 0;
#endif
}

std::size_t peak_rss() noexcept {
#if defined(__linux__) || defined(__APPLE__)
    auto usage = rusage{};
    if (getrusage(RUSAGE_SELF, &usage) != 0) {
        return 0;
    }
#if defined(__APPLE__)
    return static_cast<std::size_t>(usage.ru_maxrss);
#else
    return static_cast<std::size_t>(usage.ru_maxrss) * 1024;
#endif
#elif defined(_WIN32)
    auto counters = PROCESS_MEMORY_COUNTERS{};
    counters.cb = sizeof(counters);
    if (!GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters))) {
        return 0;
    }
    return static_cast<std::size_t>(counters.PeakWorkingSetSize);
#else
    return 0;
#endif
}

enum class LatencyClass {
    ordinary,
    flat_parent_removal,
    flat_parent_restoration_cleanup,
    child_removal,
    child_restoration_cleanup,
    broadcast
};

LatencyClass latency_class(const CyclePlan &plan) noexcept {
    if (plan.bias) {
        return LatencyClass::broadcast;
    }
    switch (plan.lifecycle) {
    case Lifecycle::remove_flat_and_parents:
        return LatencyClass::flat_parent_removal;
    case Lifecycle::restore_flat_and_parents:
        return LatencyClass::flat_parent_restoration_cleanup;
    case Lifecycle::remove_children:
        return LatencyClass::child_removal;
    case Lifecycle::restore_children:
        return LatencyClass::child_restoration_cleanup;
    case Lifecycle::none:
        return LatencyClass::ordinary;
    }
    std::unreachable();
}

struct LatencySummary {
    std::size_t count{};
    double total{};
    double mean{};
    double median{};
    double p95{};
    double p99{};
    double maximum{};
};

LatencySummary summarize(std::vector<double> samples) {
    if (samples.empty()) {
        return {};
    }

    std::ranges::sort(samples);
    auto percentile = [&](double quantile) {
        const auto position = quantile * static_cast<double>(samples.size() - 1);
        const auto lower = static_cast<std::size_t>(position);
        const auto upper = std::min(lower + 1, samples.size() - 1);
        return samples[lower] + (samples[upper] - samples[lower]) * (position - static_cast<double>(lower));
    };
    auto sum = 0.0;
    for (const auto sample : samples) {
        sum += sample;
    }

    return {.count = samples.size(),
            .total = sum,
            .mean = sum / static_cast<double>(samples.size()),
            .median = percentile(0.50),
            .p95 = percentile(0.95),
            .p99 = percentile(0.99),
            .maximum = samples.back()};
}

void print_latency(std::string_view name, const LatencySummary &summary, bool trailing_comma) {
    std::cout << "    \"" << name << "\": {\"count\": " << summary.count << ", \"total_ns\": " << summary.total
              << ", \"mean_ns\": " << summary.mean << ", \"median_ns\": " << summary.median
              << ", \"p95_ns\": " << summary.p95 << ", \"p99_ns\": " << summary.p99
              << ", \"max_ns\": " << summary.maximum << '}';
    if (trailing_comma) {
        std::cout << ',';
    }
    std::cout << '\n';
}

struct Arguments {
    std::string_view profile_name{"250k"};
    std::size_t cycles{10'000};
    std::optional<unsigned> reader_cpu{};
    bool record_evaluations{};
    bool verify_workload{};
    bool help{};
};

std::size_t parse_size(std::string_view value, std::string_view option) {
    auto result = std::size_t{};
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result);
    if (error != std::errc{} || end != value.data() + value.size() || result == 0) {
        throw std::invalid_argument{"invalid value for " + std::string{option}};
    }
    return result;
}

Arguments parse_arguments(int argument_count, char **argument_values) {
    auto result = Arguments{};
    for (auto index = 1; index < argument_count; ++index) {
        const auto argument = std::string_view{argument_values[index]};
        if (argument == "--help") {
            result.help = true;
        } else if (argument == "--record-evaluations") {
            result.record_evaluations = true;
        } else if (argument == "--verify-workload") {
            result.verify_workload = true;
        } else if (argument == "--profile" && index + 1 < argument_count) {
            result.profile_name = argument_values[++index];
        } else if (argument.starts_with("--profile=")) {
            result.profile_name = argument.substr(std::string_view{"--profile="}.size());
        } else if (argument == "--cycles" && index + 1 < argument_count) {
            result.cycles = parse_size(argument_values[++index], "--cycles");
        } else if (argument.starts_with("--cycles=")) {
            result.cycles = parse_size(argument.substr(std::string_view{"--cycles="}.size()), "--cycles");
        } else if (argument == "--reader-cpu" && index + 1 < argument_count) {
            const auto value = std::string_view{argument_values[++index]};
            auto cpu = unsigned{};
            const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), cpu);
            if (error != std::errc{} || end != value.data() + value.size()) {
                throw std::invalid_argument{"invalid value for --reader-cpu"};
            }
            result.reader_cpu = cpu;
        } else {
            throw std::invalid_argument{"unknown or incomplete argument: " + std::string{argument}};
        }
    }
    return result;
}

int run(const Arguments &arguments) {
    const auto workload_generation_start = clock_type::now();
    auto workload = build_workload(profile(arguments.profile_name), arguments.cycles);
    const auto workload_generation_nanoseconds = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(clock_type::now() - workload_generation_start).count());
    auto benchmark_control = BenchmarkControl{.workload = &workload, .measuring = false, .cycle = 0, .actual = {}};
    control = &benchmark_control;

    const auto baseline_rss = current_rss();

    auto wiring = std::optional<GraphWiring>{};
    const auto wiring_nanoseconds = elapsed_nanoseconds([&] {
        wiring.emplace();
        auto context = WiringContext{*wiring};
        const auto inputs = benchmark_source();
        const auto mapped_a = map(stage_a, inputs->flat, inputs->bias, "__key_arg__"_n = 1);
        (void)map(stage_b, mapped_a, inputs->bias, "__key_arg__"_n = 1);
        (void)map(parent_graph, inputs->parents, inputs->bias, "__key_arg__"_n = 1);
    });
    wiring->finalize();

    auto compiled = std::optional<CompiledGraph>{};
    const auto compilation_nanoseconds = elapsed_nanoseconds([&] { compiled.emplace(*wiring); });
    const auto after_graph_construction_rss = current_rss();

    auto global_state = GlobalState{};
    auto runtime = std::optional<RuntimeGraph>{};
    const auto runtime_construction_nanoseconds = elapsed_nanoseconds([&] { runtime.emplace(*compiled, global_state); });
    auto evaluation_profile = std::optional<EvaluationProfile>{};
    auto profile_reader = std::optional<EvaluationProfileReader>{};
    auto profile_reader_thread = std::jthread{};
    if (arguments.record_evaluations) {
        evaluation_profile.emplace();
        profile_reader.emplace(evaluation_profile->take_recording());
    }

    const auto start_nanoseconds = elapsed_nanoseconds([&] {
        if (evaluation_profile) {
            runtime->start(benchmark_start_time, *evaluation_profile);
        } else {
            runtime->start(benchmark_start_time);
        }
    });
    if (profile_reader) {
        profile_reader_thread = std::jthread{[&](std::stop_token stop) {
            while (!stop.stop_requested()) {
                while (profile_reader->read()) {
                }
                std::this_thread::sleep_for(std::chrono::seconds{1});
            }
            while (profile_reader->read()) {
            }
        }};
    }
    if (arguments.reader_cpu) {
        if (!profile_reader_thread.joinable()) {
            throw std::invalid_argument{"--reader-cpu requires --record-evaluations"};
        }
#if defined(__linux__)
        if (*arguments.reader_cpu >= CPU_SETSIZE) {
            throw std::invalid_argument{"--reader-cpu is outside the supported CPU set"};
        }
        auto affinity = cpu_set_t{};
        CPU_ZERO(&affinity);
        CPU_SET(*arguments.reader_cpu, &affinity);
        if (pthread_setaffinity_np(profile_reader_thread.native_handle(), sizeof(affinity), &affinity) != 0) {
            throw std::runtime_error{"cannot pin profile reader to the requested CPU"};
        }
#else
        throw std::invalid_argument{"--reader-cpu requires Linux"};
#endif
    }
    const auto empty_started_rss = current_rss();

    const auto population_nanoseconds = elapsed_nanoseconds([&] {
        if (evaluation_profile) {
            runtime->evaluate(benchmark_start_time, *evaluation_profile);
        } else {
            runtime->evaluate(benchmark_start_time);
        }
    });
    const auto populated_rss = current_rss();

    auto ordinary_latencies = std::vector<double>{};
    auto flat_parent_removal_latencies = std::vector<double>{};
    auto flat_parent_restoration_cleanup_latencies = std::vector<double>{};
    auto child_removal_latencies = std::vector<double>{};
    auto child_restoration_cleanup_latencies = std::vector<double>{};
    auto broadcast_latencies = std::vector<double>{};
    ordinary_latencies.reserve(arguments.cycles);
    flat_parent_removal_latencies.reserve(arguments.cycles / 100 + 1);
    flat_parent_restoration_cleanup_latencies.reserve(arguments.cycles / 100 + 1);
    child_removal_latencies.reserve(arguments.cycles / 100 + 1);
    child_restoration_cleanup_latencies.reserve(arguments.cycles / 100 + 1);
    broadcast_latencies.reserve(arguments.cycles / 1'000 + 1);

    benchmark_control.measuring = true;
    auto run_nanoseconds = std::uint64_t{};
    auto measure_cycles = [&]<bool Profiled>() {
        const auto run_start = clock_type::now();
        for (auto cycle = std::size_t{}; cycle < arguments.cycles; ++cycle) {
            benchmark_control.cycle = cycle;
            const auto cycle_start = clock_type::now();
            const auto evaluation_time = benchmark_start_time + std::chrono::microseconds{cycle + 1};
            if constexpr (Profiled) {
                runtime->evaluate(evaluation_time, *evaluation_profile);
            } else {
                runtime->evaluate(evaluation_time);
            }
            const auto elapsed = nanoseconds{clock_type::now() - cycle_start}.count();

            switch (latency_class(workload.cycles[cycle])) {
            case LatencyClass::ordinary:
                ordinary_latencies.push_back(elapsed);
                break;
            case LatencyClass::flat_parent_removal:
                flat_parent_removal_latencies.push_back(elapsed);
                break;
            case LatencyClass::flat_parent_restoration_cleanup:
                flat_parent_restoration_cleanup_latencies.push_back(elapsed);
                break;
            case LatencyClass::child_removal:
                child_removal_latencies.push_back(elapsed);
                break;
            case LatencyClass::child_restoration_cleanup:
                child_restoration_cleanup_latencies.push_back(elapsed);
                break;
            case LatencyClass::broadcast:
                broadcast_latencies.push_back(elapsed);
                break;
            }
        }
        run_nanoseconds = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(clock_type::now() - run_start).count());
    };
    if (evaluation_profile) {
        measure_cycles.template operator()<true>();
    } else {
        measure_cycles.template operator()<false>();
    }
    const auto process_peak_rss = peak_rss();

    benchmark_control.measuring = false;
    const auto final_stop_nanoseconds = elapsed_nanoseconds([&] {
        if (evaluation_profile) {
            runtime->stop(*evaluation_profile);
            evaluation_profile->finish();
        } else {
            runtime->stop();
        }
    });
    if (profile_reader_thread.joinable()) {
        profile_reader_thread.request_stop();
        profile_reader_thread.join();
    }
    if (evaluation_profile && evaluation_profile->failed()) {
        throw std::runtime_error{"evaluation profile recording failed"};
    }
    const auto profile_recorded_bytes = evaluation_profile ? evaluation_profile->recorded_bytes() : 0;
    const auto after_stop_rss = current_rss();

    const auto ordinary = summarize(std::move(ordinary_latencies));
    const auto flat_parent_removal = summarize(std::move(flat_parent_removal_latencies));
    const auto flat_parent_restoration_cleanup = summarize(std::move(flat_parent_restoration_cleanup_latencies));
    const auto child_removal = summarize(std::move(child_removal_latencies));
    const auto child_restoration_cleanup = summarize(std::move(child_restoration_cleanup_latencies));
    const auto broadcast = summarize(std::move(broadcast_latencies));
    const auto measured_cycles_nanoseconds = ordinary.total + flat_parent_removal.total +
                                             flat_parent_restoration_cleanup.total + child_removal.total +
                                             child_restoration_cleanup.total + broadcast.total;
    const auto validation_passed = benchmark_control.actual == workload.expected;
    const auto rss_delta = static_cast<std::int64_t>(populated_rss) - static_cast<std::int64_t>(empty_started_rss);
    const auto reported_peak_rss = std::max({process_peak_rss, baseline_rss, after_graph_construction_rss,
                                             empty_started_rss, populated_rss, after_stop_rss});
    const auto bytes_per_inner_graph =
        static_cast<double>(rss_delta) / static_cast<double>(workload.selected_profile.active_inner_graphs());
    const auto graph_construction_nanoseconds = wiring_nanoseconds + compilation_nanoseconds;
    const auto cycles_per_second = static_cast<double>(arguments.cycles) * 1.0e9 / measured_cycles_nanoseconds;
    const auto events_per_second =
        static_cast<double>(benchmark_control.actual.count) * 1.0e9 / measured_cycles_nanoseconds;

    std::cout << std::fixed << std::setprecision(3);
    std::cout << "{\n";
    std::cout << "  \"schema_version\": 1,\n";
    std::cout << "  \"benchmark\": \"portable_high_count_map\",\n";
    std::cout << "  \"engine\": \"bg\",\n";
    std::cout << "  \"runtime\": \"native-cpp\",\n";
    std::cout << "  \"profile\": \"" << workload.selected_profile.name << "\",\n";
    std::cout << "  \"record_evaluations\": " << (evaluation_profile ? "true" : "false") << ",\n";
    std::cout << "  \"value_type\": \"int64_nonnegative\",\n";
    std::cout << "  \"topology\": {\"flat_items\": " << workload.selected_profile.flat_count
              << ", \"parents\": " << workload.selected_profile.parent_count
              << ", \"children_per_parent\": " << child_count
              << ", \"active_inner_graphs\": " << workload.selected_profile.active_inner_graphs() << "},\n";
    std::cout << "  \"workload\": {\"cycles\": " << arguments.cycles
              << ", \"flat_updates_per_cycle\": " << workload.selected_profile.flat_updates
              << ", \"child_updates_per_cycle\": " << workload.selected_profile.child_updates
              << ", \"flat_lifecycle_count\": " << workload.selected_profile.flat_lifecycle_count
              << ", \"parent_lifecycle_count\": " << workload.selected_profile.parent_lifecycle_count
              << ", \"child_lifecycle_count\": " << workload.selected_profile.child_lifecycle_count
              << ", \"lifecycle_period\": 100, \"broadcast_period\": 1000},\n";
    std::cout << "  \"timing\": {\"workload_generation_ns\": " << workload_generation_nanoseconds
              << ", \"wiring_ns\": " << graph_construction_nanoseconds
              << ", \"runtime_factory_ns\": " << runtime_construction_nanoseconds
              << ", \"runtime_start_ns\": " << start_nanoseconds << ", \"population_ns\": " << population_nanoseconds
              << ", \"measured_cycles_ns\": " << static_cast<std::uint64_t>(measured_cycles_nanoseconds)
              << ", \"wall_run_ns\": " << run_nanoseconds << ", \"final_stop_ns\": " << final_stop_nanoseconds
              << ", \"profile_recorded_bytes\": " << profile_recorded_bytes
              << "},\n";
    std::cout << "  \"throughput\": {\"cycles_per_second\": " << cycles_per_second
              << ", \"output_events_per_second\": " << events_per_second << "},\n";
    std::cout << "  \"latency\": {\n";
    print_latency("ordinary", ordinary, true);
    print_latency("flat_parent_removal", flat_parent_removal, true);
    print_latency("flat_parent_restoration_cleanup", flat_parent_restoration_cleanup, true);
    print_latency("child_removal", child_removal, true);
    print_latency("child_restoration_cleanup", child_restoration_cleanup, true);
    print_latency("broadcast", broadcast, false);
    std::cout << "  },\n";
    std::cout << "  \"output\": {\"actual\": {\"count\": " << benchmark_control.actual.count
              << ", \"sum\": " << benchmark_control.actual.sum << ", \"xor\": " << benchmark_control.actual.xor_value
              << "}, \"expected\": {\"count\": " << workload.expected.count << ", \"sum\": " << workload.expected.sum
              << ", \"xor\": " << workload.expected.xor_value
              << "}, \"valid\": " << (validation_passed ? "true" : "false") << "},\n";
    std::cout << "  \"memory\": {\"baseline_rss_bytes\": " << baseline_rss
              << ", \"after_wiring_rss_bytes\": " << after_graph_construction_rss
              << ", \"empty_runtime_rss_bytes\": " << empty_started_rss
              << ", \"populated_rss_bytes\": " << populated_rss << ", \"peak_rss_bytes\": " << reported_peak_rss
              << ", \"after_stop_rss_bytes\": " << after_stop_rss << ", \"populated_rss_delta_bytes\": " << rss_delta
              << ", \"bytes_per_active_inner_graph\": " << bytes_per_inner_graph << "}\n";
    std::cout << "}\n";

    return validation_passed ? EXIT_SUCCESS : EXIT_FAILURE;
}

} // namespace

int main(int argument_count, char **argument_values) {
    try {
        const auto arguments = parse_arguments(argument_count, argument_values);
        if (arguments.help) {
            std::cout << "usage: bg_high_count_map_benchmark [--profile smoke|25k|100k|250k] [--cycles N] "
                         "[--record-evaluations] [--reader-cpu N (Linux)] [--verify-workload]\n";
            return EXIT_SUCCESS;
        }
        if (arguments.verify_workload) {
            verify_workload_manifest();
            return EXIT_SUCCESS;
        }
        return run(arguments);
    } catch (const std::exception &error) {
        std::cerr << "bg_high_count_map_benchmark: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
