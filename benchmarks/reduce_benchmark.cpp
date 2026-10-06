#include <bg/evaluation_context.hpp>
#include <bg/global_state.hpp>
#include <bg/graph.hpp>
#include <bg/graph/compiled_graph.hpp>
#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/runtime_graph.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/node.hpp>
#include <bg/reduce.hpp>
#include <bg/time_series/tsd/tsd_type.hpp>

#include <algorithm>
#include <bit>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if defined(__linux__)
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

enum class UpdateKind { sparse, burst, remove, restore };

struct Update {
    UpdateKind kind;
    std::size_t first_key;
    std::size_t key_count;
};

struct Checksum {
    std::uint64_t count{};
    std::uint64_t sum{};
    std::uint64_t xor_value{};

    void add(std::size_t sample, std::int64_t value) noexcept {
        auto hash = mix(static_cast<std::uint64_t>(sample));
        hash = mix(hash ^ mix(static_cast<std::uint64_t>(value)));
        ++count;
        sum += hash;
        xor_value ^= std::rotl(hash, static_cast<int>(hash & 63));
    }

    bool operator==(const Checksum &) const = default;

    static std::uint64_t mix(std::uint64_t value) noexcept {
        value += 0x9e3779b97f4a7c15ULL;
        value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
        value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
        return value ^ (value >> 31);
    }
};

struct Workload {
    std::size_t item_count{};
    std::vector<Update> updates;
    std::vector<std::int64_t> expected_sums;
    Checksum expected_checksum;
};

Workload build_workload(std::size_t item_count, std::size_t samples, std::size_t lifecycle_rounds) {
    const auto batch_size = item_count / 100;
    auto workload = Workload{.item_count = item_count,
                             .updates = {},
                             .expected_sums = {},
                             .expected_checksum = {}};
    workload.updates.reserve(samples * 2 + lifecycle_rounds * 2);
    workload.expected_sums.reserve(workload.updates.capacity() + 1);

    auto values = std::vector<std::int64_t>(item_count);
    auto total = std::int64_t{};
    for (auto key = std::size_t{}; key < item_count; ++key) {
        values[key] = static_cast<std::int64_t>(key + 1);
        total += values[key];
    }

    const auto record = [&] {
        workload.expected_sums.push_back(total);
        workload.expected_checksum.add(workload.expected_sums.size() - 1, total);
    };
    record();

    for (auto sample = std::size_t{}; sample < samples; ++sample) {
        const auto key = (sample * 7'919 + 17) % item_count;
        ++values[key];
        ++total;
        workload.updates.push_back({UpdateKind::sparse, key, 1});
        record();
    }

    for (auto sample = std::size_t{}; sample < samples; ++sample) {
        const auto first_key = sample * batch_size;
        for (auto offset = std::size_t{}; offset < batch_size; ++offset) {
            const auto key = first_key + offset;
            values[key] += 17;
            total += 17;
        }
        workload.updates.push_back({UpdateKind::burst, first_key, batch_size});
        record();
    }

    for (auto round = std::size_t{}; round < lifecycle_rounds; ++round) {
        const auto first_key = round * batch_size;
        for (auto offset = std::size_t{}; offset < batch_size; ++offset) {
            const auto key = first_key + offset;
            total -= values[key];
        }
        workload.updates.push_back({UpdateKind::remove, first_key, batch_size});
        record();

        for (auto offset = std::size_t{}; offset < batch_size; ++offset) {
            const auto key = first_key + offset;
            total += values[key];
        }
        workload.updates.push_back({UpdateKind::restore, first_key, batch_size});
        record();
    }

    return workload;
}

struct BenchmarkControl {
    const Workload *workload{};
    std::vector<std::int64_t> values;
    std::size_t sample_index{};
    bool output_mismatch{};
    Checksum actual_checksum;

    explicit BenchmarkControl(const Workload &selected_workload)
        : workload(&selected_workload), values(selected_workload.item_count) {
        for (auto key = std::size_t{}; key < values.size(); ++key) {
            values[key] = static_cast<std::int64_t>(key + 1);
        }
    }

    void observe(std::int64_t value) noexcept {
        if (sample_index >= workload->expected_sums.size() || value != workload->expected_sums[sample_index]) {
            output_mismatch = true;
            return;
        }
        actual_checksum.add(sample_index, value);
    }
};

BenchmarkControl *active_control{};

struct ReduceSource {
    State<std::size_t> next_sample;
    Out<TSD<std::int64_t, TS<std::int64_t>>> output;

    static constexpr bool uses_timed_scheduling = true;

    void start(EvaluationContext *context) const { context->schedule_now(); }

    void evaluate(EvaluationContext *context) const {
        if (*next_sample == 0) {
            populate();
        } else {
            apply(active_control->workload->updates[*next_sample - 1]);
            active_control->sample_index = *next_sample;
        }

        ++*next_sample;
        if (*next_sample <= active_control->workload->updates.size()) {
            context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
        }
    }

    void populate() const {
        for (auto key = std::size_t{}; key < active_control->values.size(); ++key) {
            output[static_cast<std::int64_t>(key)].set(active_control->values[key]);
        }
        active_control->sample_index = 0;
    }

    void apply(const Update &update) const {
        for (auto offset = std::size_t{}; offset < update.key_count; ++offset) {
            const auto key = update.first_key + offset;
            const auto item_key = static_cast<std::int64_t>(key);
            switch (update.kind) {
            case UpdateKind::sparse:
                ++active_control->values[key];
                output[item_key].set(active_control->values[key]);
                break;
            case UpdateKind::burst:
                active_control->values[key] += 17;
                output[item_key].set(active_control->values[key]);
                break;
            case UpdateKind::remove:
                output.erase(item_key);
                break;
            case UpdateKind::restore:
                output[item_key].set(active_control->values[key]);
                break;
            }
        }
    }
};

struct ReduceSink {
    In<TS<std::int64_t>> value;

    void evaluate() const { active_control->observe(*value); }
};

const auto reduce_source = node<ReduceSource>();
const auto reduce_sink = node<ReduceSink>();
const auto add_values = node([](In<TS<i64>> left, In<TS<i64>> right, Out<TS<i64>> output) {
    output.set(*left + *right);
});
const auto add_graph = graph("name"_n = "one_node_reduce_benchmark_add", [](wp<TS<i64>> left, wp<TS<i64>> right) {
    return add_values(left, right);
});

void wire_graph(GraphWiring &wiring, bool with_reduce) {
    {
        auto context = WiringContext{wiring};
        const auto values = reduce_source();
        if (with_reduce) {
            reduce_sink(reduce(add_graph, values));
        }
    }
}

std::uint64_t elapsed_nanoseconds(auto &&operation) {
    const auto start = clock_type::now();
    std::forward<decltype(operation)>(operation)();
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

struct Distribution {
    std::size_t count{};
    std::uint64_t total_ns{};
    double mean_ns{};
    std::uint64_t min_ns{};
    std::uint64_t median_ns{};
    std::uint64_t p95_ns{};
    std::uint64_t p99_ns{};
    std::uint64_t max_ns{};
};

Distribution summarize(std::vector<std::uint64_t> samples) {
    if (samples.empty()) {
        return {};
    }
    std::ranges::sort(samples);
    const auto total = std::accumulate(samples.begin(), samples.end(), std::uint64_t{});
    const auto percentile = [&](double fraction) {
        const auto index = static_cast<std::size_t>(std::ceil(fraction * static_cast<double>(samples.size()))) - 1;
        return samples[std::min(index, samples.size() - 1)];
    };
    return {samples.size(), total, static_cast<double>(total) / static_cast<double>(samples.size()), samples.front(),
            samples[(samples.size() - 1) / 2], percentile(0.95), percentile(0.99), samples.back()};
}

void print_distribution(std::string_view name, const Distribution &distribution) {
    std::cout << "    \"" << name << "\": {\"count\": " << distribution.count
              << ", \"total_ns\": " << distribution.total_ns << ", \"mean_ns\": " << distribution.mean_ns
              << ", \"min_ns\": " << distribution.min_ns << ", \"median_ns\": " << distribution.median_ns
              << ", \"p95_ns\": " << distribution.p95_ns << ", \"p99_ns\": " << distribution.p99_ns
              << ", \"max_ns\": " << distribution.max_ns << "}";
}

struct Arguments {
    std::size_t item_count{10'000};
    std::size_t samples{20};
    std::size_t lifecycle_rounds{20};
    bool with_reduce{true};
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
        } else if (argument == "--size" && index + 1 < argument_count) {
            result.item_count = parse_size(argument_values[++index], "--size");
        } else if (argument == "--samples" && index + 1 < argument_count) {
            result.samples = parse_size(argument_values[++index], "--samples");
        } else if (argument == "--lifecycle-rounds" && index + 1 < argument_count) {
            result.lifecycle_rounds = parse_size(argument_values[++index], "--lifecycle-rounds");
        } else if (argument == "--mode" && index + 1 < argument_count) {
            const auto mode = std::string_view{argument_values[++index]};
            if (mode == "reduce") {
                result.with_reduce = true;
            } else if (mode == "source") {
                result.with_reduce = false;
            } else {
                throw std::invalid_argument{"--mode must be reduce or source"};
            }
        } else {
            throw std::invalid_argument{"unknown or incomplete argument: " + std::string{argument}};
        }
    }

    if (result.item_count != 10'000 && result.item_count != 100'000) {
        throw std::invalid_argument{"--size must be 10000 or 100000"};
    }
    if (result.item_count < 100 || result.samples > 100 || result.lifecycle_rounds > 100) {
        throw std::invalid_argument{"sample counts must be at most 100 and item count at least 100"};
    }
    return result;
}

int run(const Arguments &arguments) {
    auto workload = build_workload(arguments.item_count, arguments.samples, arguments.lifecycle_rounds);
    auto control = BenchmarkControl{workload};
    active_control = &control;

    const auto baseline_rss = current_rss();
    auto wiring_nanoseconds = std::uint64_t{};
    auto wiring = std::optional<GraphWiring>{};
    wiring.emplace();
    auto compiled = std::optional<CompiledGraph>{};
    wiring_nanoseconds = elapsed_nanoseconds([&] {
        wire_graph(*wiring, arguments.with_reduce);
        wiring->finalize();
        compiled.emplace(*wiring);
    });
    const auto after_compile_rss = current_rss();

    auto global_state = GlobalState{};
    auto runtime = std::optional<RuntimeGraph>{};
    const auto runtime_construction_ns = elapsed_nanoseconds([&] { runtime.emplace(*compiled, global_state); });
    constexpr auto start_time = MIN_DT + std::chrono::microseconds{1};
    const auto runtime_start_ns = elapsed_nanoseconds([&] { runtime->start(start_time); });
    const auto empty_runtime_rss = current_rss();

    const auto population_ns = elapsed_nanoseconds([&] { runtime->evaluate(start_time); });
    const auto populated_rss = current_rss();

    auto sparse_samples = std::vector<std::uint64_t>{};
    auto burst_samples = std::vector<std::uint64_t>{};
    auto removal_samples = std::vector<std::uint64_t>{};
    auto restoration_samples = std::vector<std::uint64_t>{};
    sparse_samples.reserve(arguments.samples);
    burst_samples.reserve(arguments.samples);
    removal_samples.reserve(arguments.lifecycle_rounds);
    restoration_samples.reserve(arguments.lifecycle_rounds);

    const auto sparse_end = arguments.samples;
    const auto burst_end = sparse_end + arguments.samples;
    for (auto index = std::size_t{}; index < workload.updates.size(); ++index) {
        const auto start = clock_type::now();
        runtime->evaluate(start_time + std::chrono::microseconds{static_cast<std::int64_t>(index + 1)});
        const auto elapsed = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(clock_type::now() - start).count());

        if (index < sparse_end) {
            sparse_samples.push_back(elapsed);
        } else if (index < burst_end) {
            burst_samples.push_back(elapsed);
        } else if ((index - burst_end) % 2 == 0) {
            removal_samples.push_back(elapsed);
        } else {
            restoration_samples.push_back(elapsed);
        }
    }
    const auto final_stop_ns = elapsed_nanoseconds([&] { runtime->stop(); });
    const auto after_stop_rss = current_rss();
    const auto measured_peak_rss = peak_rss();

    const auto source_completed = control.sample_index + 1 == workload.expected_sums.size();
    const auto checksum_valid = !control.output_mismatch && control.actual_checksum == workload.expected_checksum;
    const auto run_valid = source_completed && (!arguments.with_reduce || checksum_valid);
    const auto populated_delta = static_cast<std::int64_t>(populated_rss) -
                                 static_cast<std::int64_t>(empty_runtime_rss);

    std::cout << std::fixed << std::setprecision(2);
    std::cout << "{\n"
              << "  \"benchmark\": \"large_tsd_reduce\",\n"
              << "  \"source_mode\": \"" << (arguments.with_reduce ? "reduce" : "source_only") << "\",\n"
              << "  \"item_count\": " << arguments.item_count << ",\n"
              << "  \"workload\": {\"sparse_one_key_ticks\": " << arguments.samples
              << ", \"burst_ticks\": " << arguments.samples << ", \"burst_items_per_tick\": "
              << arguments.item_count / 100 << ", \"remove_restore_pairs\": " << arguments.lifecycle_rounds
              << ", \"lifecycle_items_per_tick\": " << arguments.item_count / 100 << "},\n"
              << "  \"timing_ns\": {\"wiring_and_compile\": " << wiring_nanoseconds
              << ", \"runtime_construction\": " << runtime_construction_ns
              << ", \"runtime_start\": " << runtime_start_ns << ", \"initial_population\": " << population_ns
              << ",\n";
    print_distribution("sparse_one_key", summarize(std::move(sparse_samples)));
    std::cout << ",\n  ";
    print_distribution("burst_1_percent", summarize(std::move(burst_samples)));
    std::cout << ",\n  ";
    print_distribution("remove_1_percent", summarize(std::move(removal_samples)));
    std::cout << ",\n  ";
    print_distribution("restore_1_percent", summarize(std::move(restoration_samples)));
    std::cout << ",\n    \"final_stop\": " << final_stop_ns << "},\n"
              << "  \"memory_bytes\": {\"before_graph\": " << baseline_rss
              << ", \"after_compile\": " << after_compile_rss << ", \"empty_runtime\": " << empty_runtime_rss
              << ", \"populated\": " << populated_rss << ", \"peak_rss\": "
              << std::max({measured_peak_rss, baseline_rss, after_compile_rss, empty_runtime_rss, populated_rss,
                           after_stop_rss})
              << ", \"after_stop\": " << after_stop_rss
              << ", \"populated_runtime_delta_including_tsd\": " << populated_delta
              << ", \"populated_runtime_bytes_per_item_including_tsd\": "
              << static_cast<double>(populated_delta) / static_cast<double>(arguments.item_count) << "},\n"
              << "  \"checksum\": {\"expected_count\": " << workload.expected_checksum.count
              << ", \"expected_sum\": " << workload.expected_checksum.sum << ", \"expected_xor\": "
              << workload.expected_checksum.xor_value;
    if (arguments.with_reduce) {
        std::cout << ", \"actual_count\": " << control.actual_checksum.count << ", \"actual_sum\": "
                  << control.actual_checksum.sum << ", \"actual_xor\": " << control.actual_checksum.xor_value
                  << ", \"source_completed\": " << (source_completed ? "true" : "false")
                  << ", \"valid\": " << (checksum_valid ? "true" : "false");
    } else {
        std::cout << ", \"source_completed\": " << (source_completed ? "true" : "false")
                  << ", \"valid\": null";
    }
    std::cout << "}\n}\n";
    return run_valid ? EXIT_SUCCESS : EXIT_FAILURE;
}

} // namespace

int main(int argument_count, char **argument_values) {
    try {
        const auto arguments = parse_arguments(argument_count, argument_values);
        if (arguments.help) {
            std::cout << "usage: bg_reduce_benchmark [--size 10000|100000] [--mode reduce|source] "
                         "[--samples N] [--lifecycle-rounds N]\n";
            return EXIT_SUCCESS;
        }
        return run(arguments);
    } catch (const std::exception &error) {
        std::cerr << "bg_reduce_benchmark: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
