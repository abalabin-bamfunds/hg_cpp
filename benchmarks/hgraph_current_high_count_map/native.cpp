#include "high_count_map_workload.hpp"

#include <hgraph/lib/std/std_operators.h>
#include <hgraph/runtime/runtime.h>
#include <hgraph/types/graph_wiring.h>
#include <hgraph/types/static_node.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/resource.h>
#include <unistd.h>

namespace {

using namespace hgraph;
using namespace high_count_map;
using Clock = std::chrono::steady_clock;

struct Control {
    const Workload *workload{};
    std::size_t cycle{};
    bool measuring{};
    EventChecksum actual{};

    void observe(Branch branch, Int parent, Int key, bool present, Int value) {
        if (measuring) actual.add(cycle, branch, parent, key, present, value);
    }
};

Control *control{};

struct FlatSource {
    static constexpr auto name = "benchmark_flat_source";
    static constexpr bool schedule_on_start = true;

    static void eval(State<Int> step, NodeScheduler scheduler, Out<TSD<Int, TS<Int>>> out) {
        const auto current = step.get();
        const auto &profile = control->workload->selected_profile;
        if (current == 0) {
            for (Int key = 0; key < profile.flat_count; ++key) out.set(key, initial_flat_value(key));
        } else {
            const auto &plan = control->workload->cycles[static_cast<std::size_t>(current - 1)];
            for (const auto &[key, value] : plan.flat_updates) out.set(key, value);
            if (plan.lifecycle == Lifecycle::remove_flat_and_parents) {
                for (Int key : plan.flat_lifecycle_keys) (void)out.erase(key);
            } else if (plan.lifecycle == Lifecycle::restore_flat_and_parents) {
                for (Int key : plan.flat_lifecycle_keys) out.set(key, flat_source_value(current - 1, key));
            }
        }
        step.set(current + 1);
        if (static_cast<std::size_t>(current) < control->workload->cycles.size()) scheduler.schedule(MIN_TD);
    }
};

struct ParentSource {
    static constexpr auto name = "benchmark_parent_source";
    static constexpr bool schedule_on_start = true;

    static void eval(State<Int> step, NodeScheduler scheduler, Out<TSD<Int, TSD<Int, TS<Int>>>> out) {
        const auto current = step.get();
        const auto &profile = control->workload->selected_profile;
        if (current == 0) {
            for (Int parent = 0; parent < profile.parent_count; ++parent) {
                for (Int child = 0; child < child_count; ++child) {
                    out[parent].set(child, initial_child_value(parent, child));
                }
            }
        } else {
            const auto &plan = control->workload->cycles[static_cast<std::size_t>(current - 1)];
            for (const auto &[parent, child, value] : plan.child_updates) out[parent].set(child, value);
            if (plan.lifecycle == Lifecycle::remove_flat_and_parents) {
                for (Int parent : plan.parent_lifecycle_keys) (void)out.erase(parent);
            } else if (plan.lifecycle == Lifecycle::restore_flat_and_parents) {
                for (Int parent : plan.parent_lifecycle_keys) {
                    for (Int child = 0; child < child_count; ++child) {
                        out[parent].set(child, child_source_value(current - 1, parent, child));
                    }
                }
            } else if (plan.lifecycle == Lifecycle::remove_children) {
                for (const auto &[parent, child] : plan.child_lifecycle_keys) (void)out[parent].erase(child);
            } else if (plan.lifecycle == Lifecycle::restore_children) {
                for (const auto &[parent, child] : plan.child_lifecycle_keys) {
                    out[parent].set(child, child_source_value(current - 1, parent, child));
                }
            }
        }
        step.set(current + 1);
        if (static_cast<std::size_t>(current) < control->workload->cycles.size()) scheduler.schedule(MIN_TD);
    }
};

struct BiasSource {
    static constexpr auto name = "benchmark_bias_source";
    static constexpr bool schedule_on_start = true;

    static void eval(State<Int> step, NodeScheduler scheduler, Out<TS<Int>> out) {
        const auto current = step.get();
        if (current == 0) out.set(Int{1});
        else if (auto bias = control->workload->cycles[static_cast<std::size_t>(current - 1)].bias) out.set(*bias);
        step.set(current + 1);
        if (static_cast<std::size_t>(current) < control->workload->cycles.size()) scheduler.schedule(MIN_TD);
    }
};

struct AddValues {
    static constexpr auto name = "benchmark_add_values";
    static void eval(In<"lhs", TS<Int>> lhs, In<"rhs", TS<Int>> rhs, Out<TS<Int>> out) {
        out.set(lhs.value() + rhs.value());
    }
};

struct Multiply {
    static constexpr auto name = "benchmark_multiply";
    static void eval(In<"value", TS<Int>> value, Scalar<"factor", Int> factor, Out<TS<Int>> out) {
        out.set(value.value() * factor.value());
    }
};

struct AddConstant {
    static constexpr auto name = "benchmark_add_constant";
    static void eval(In<"value", TS<Int>> value, Scalar<"addend", Int> addend, Out<TS<Int>> out) {
        out.set(value.value() + addend.value());
    }
};

struct FlatSink {
    static constexpr auto name = "benchmark_flat_sink";
    static void eval(In<"key", TS<Int>> key, In<"value", TS<Int>> value, State<Int> stored_key) {
        stored_key.set(key.value() + 1);
        control->observe(Branch::flat, 0, key.value(), true, value.value());
    }
    static void stop(State<Int> stored_key) {
        if (stored_key.get() != 0) control->observe(Branch::flat, 0, stored_key.get() - 1, false, 0);
    }
};

struct ChildSink {
    static constexpr auto name = "benchmark_child_sink";
    static void eval(In<"parent", TS<Int>> parent, In<"key", TS<Int>> key,
                     In<"value", TS<Int>> value, State<Int> stored_key) {
        stored_key.set(parent.value() * child_count + key.value() + 1);
        control->observe(Branch::child, parent.value(), key.value(), true, value.value());
    }
    static void stop(State<Int> stored_key) {
        if (stored_key.get() == 0) return;
        const Int packed = stored_key.get() - 1;
        control->observe(Branch::child, packed / child_count, packed % child_count, false, 0);
    }
};

template <typename Schema> struct MapOutputSink {
    static constexpr auto name = "benchmark_map_output_sink";
    static void eval(In<"value", Schema>) {}
};

struct StageA {
    static constexpr auto name = "benchmark_stage_a";
    static Port<TS<Int>> compose(Wiring &w, NamedPort<"key", TS<Int>> key,
                                  Port<TS<Int>> value, Port<TS<Int>> bias) {
        auto a1 = wire<AddValues>(w, value, bias);
        auto a2 = wire<Multiply>(w, a1, Int{3});
        auto a3 = wire<AddValues>(w, a2, key);
        auto a4 = wire<Multiply>(w, a3, Int{5});
        return wire<AddConstant>(w, a4, Int{7});
    }
};

struct StageB {
    static constexpr auto name = "benchmark_stage_b";
    static Port<TS<Int>> compose(Wiring &w, NamedPort<"key", TS<Int>> key,
                                  Port<TS<Int>> value, Port<TS<Int>> bias) {
        auto b1 = wire<AddValues>(w, value, bias);
        auto b2 = wire<Multiply>(w, b1, Int{7});
        auto b3 = wire<AddValues>(w, b2, key);
        auto b4 = wire<Multiply>(w, b3, Int{3});
        auto output = wire<AddConstant>(w, b4, Int{11});
        wire<FlatSink>(w, key, output);
        return output;
    }
};

struct ChildGraph {
    static constexpr auto name = "benchmark_child_graph";
    static Port<TS<Int>> compose(Wiring &w, NamedPort<"key", TS<Int>> key, Port<TS<Int>> value,
                                  Port<TS<Int>> parent, Port<TS<Int>> seed, Port<TS<Int>> bias) {
        auto c1 = wire<AddValues>(w, value, seed);
        auto c2 = wire<Multiply>(w, c1, Int{3});
        auto c3 = wire<AddValues>(w, c2, key);
        auto c4 = wire<AddValues>(w, c3, bias);
        auto output = wire<Multiply>(w, c4, Int{7});
        wire<ChildSink>(w, parent, key, output);
        return output;
    }
};

struct ParentGraph {
    static constexpr auto name = "benchmark_parent_graph";
    static Port<TSD<Int, TS<Int>>> compose(Wiring &w, NamedPort<"key", TS<Int>> key,
                                            Port<TSD<Int, TS<Int>>> children, Port<TS<Int>> bias) {
        auto p1 = wire<AddValues>(w, bias, key);
        auto seed = wire<Multiply>(w, p1, Int{5});
        return wire<stdlib::map_>(w, fn<ChildGraph>(), children, key, seed, bias).as<TSD<Int, TS<Int>>>();
    }
};

struct BenchmarkGraph {
    static constexpr auto name = "benchmark_high_count_map";
    static void compose(Wiring &w) {
        auto flat = wire<FlatSource>(w);
        auto parents = wire<ParentSource>(w);
        auto bias = wire<BiasSource>(w);
        auto first = wire<stdlib::map_>(w, fn<StageA>(), flat, bias).as<TSD<Int, TS<Int>>>();
        auto second = wire<stdlib::map_>(w, fn<StageB>(), first, bias);
        auto nested = wire<stdlib::map_>(w, fn<ParentGraph>(), parents, bias);
        // The top-level sinks retain both map outputs; branch sinks record their item changes.
        wire<MapOutputSink<TSD<Int, TS<Int>>>>(w, second);
        wire<MapOutputSink<TSD<Int, TSD<Int, TS<Int>>>>>(w, nested);
    }
};

std::size_t current_rss() {
    std::ifstream statm{"/proc/self/statm"};
    std::size_t pages{}, resident{};
    if (!(statm >> pages >> resident)) return 0;
    return resident * static_cast<std::size_t>(::sysconf(_SC_PAGESIZE));
}

std::size_t peak_rss() {
    struct rusage usage {};
    return getrusage(RUSAGE_SELF, &usage) == 0 ? static_cast<std::size_t>(usage.ru_maxrss) * 1024 : 0;
}

struct Summary {
    std::size_t count{};
    double total{}, mean{}, median{}, p95{}, p99{}, maximum{};
};

Summary summarize(std::vector<double> samples) {
    if (samples.empty()) return {};
    std::ranges::sort(samples);
    const auto percentile = [&](double fraction) {
        const auto position = fraction * static_cast<double>(samples.size() - 1);
        const auto lower = static_cast<std::size_t>(position);
        const auto upper = std::min(lower + 1, samples.size() - 1);
        return samples[lower] + (samples[upper] - samples[lower]) * (position - lower);
    };
    const auto total = std::accumulate(samples.begin(), samples.end(), 0.0);
    return {samples.size(), total, total / samples.size(), percentile(.5), percentile(.95),
            percentile(.99), samples.back()};
}

void print_summary(std::string_view name, const Summary &s, bool comma) {
    std::cout << "    \"" << name << "\": {\"count\": " << s.count << ", \"total_ns\": " << s.total
              << ", \"mean_ns\": " << s.mean << ", \"median_ns\": " << s.median
              << ", \"p95_ns\": " << s.p95 << ", \"p99_ns\": " << s.p99
              << ", \"max_ns\": " << s.maximum << "}" << (comma ? "," : "") << '\n';
}

int run(std::string_view profile_name, std::size_t cycles) {
    auto generation_start = Clock::now();
    auto workload = build_workload(profile(profile_name), cycles);
    const auto generation_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - generation_start).count();
    auto run_control = Control{.workload = &workload};
    control = &run_control;
    const auto baseline_rss = current_rss();

    stdlib::register_standard_operators();
    auto wiring_start = Clock::now();
    auto graph = build_graph<BenchmarkGraph>();
    const auto wiring_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - wiring_start).count();
    const auto after_wiring_rss = current_rss();

    auto factory_start = Clock::now();
    GraphExecutorBuilder builder;
    builder.graph_builder(std::move(graph)).mode(GraphExecutorMode::ExternallyDriven)
        .start_time(MIN_ST).end_time(MIN_ST + MIN_TD * static_cast<Int>(cycles + 2));
    auto executor = builder.make_executor();
    auto view = executor.view();
    const auto factory_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - factory_start).count();
    auto start = Clock::now();
    view.start_external(MIN_ST);
    const auto start_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count();
    const auto empty_rss = current_rss();

    auto population_start = Clock::now();
    if (!view.step(MIN_ST)) throw std::runtime_error("population evaluation pending");
    const auto population_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - population_start).count();
    const auto populated_rss = current_rss();

    std::vector<double> ordinary, flat_removal, flat_restore, child_removal, child_restore, broadcast;
    ordinary.reserve(cycles);
    auto run_start = Clock::now();
    run_control.measuring = true;
    for (std::size_t cycle = 0; cycle < cycles; ++cycle) {
        run_control.cycle = cycle;
        const auto when = MIN_ST + MIN_TD * static_cast<Int>(cycle + 1);
        if (const auto next = view.graph().next_scheduled_time(); next != when) {
            throw std::runtime_error("source schedule differs from workload at cycle " + std::to_string(cycle) +
                                     ": next=" + std::to_string(next.time_since_epoch().count()) +
                                     ", expected=" + std::to_string(when.time_since_epoch().count()));
        }
        auto cycle_start = Clock::now();
        if (!view.step(when)) throw std::runtime_error("measured evaluation pending");
        const auto duration = std::chrono::duration<double, std::nano>(Clock::now() - cycle_start).count();
        const auto &plan = workload.cycles[cycle];
        if (plan.bias) broadcast.push_back(duration);
        else if (plan.lifecycle == Lifecycle::remove_flat_and_parents) flat_removal.push_back(duration);
        else if (plan.lifecycle == Lifecycle::restore_flat_and_parents) flat_restore.push_back(duration);
        else if (plan.lifecycle == Lifecycle::remove_children) child_removal.push_back(duration);
        else if (plan.lifecycle == Lifecycle::restore_children) child_restore.push_back(duration);
        else ordinary.push_back(duration);
    }
    const auto wall_run_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - run_start).count();
    run_control.measuring = false;
    const auto before_stop_rss = current_rss();
    auto stop_start = Clock::now();
    view.stop_external();
    const auto stop_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - stop_start).count();
    const auto after_stop_rss = current_rss();
    const auto graph_rss_delta =
        static_cast<std::int64_t>(before_stop_rss) - static_cast<std::int64_t>(baseline_rss);
    const auto reported_peak_rss = std::max({peak_rss(), baseline_rss, after_wiring_rss, empty_rss,
                                             populated_rss, after_stop_rss});

    const auto o = summarize(std::move(ordinary));
    const auto fr = summarize(std::move(flat_removal));
    const auto fc = summarize(std::move(flat_restore));
    const auto cr = summarize(std::move(child_removal));
    const auto cc = summarize(std::move(child_restore));
    const auto b = summarize(std::move(broadcast));
    const auto measured_ns = o.total + fr.total + fc.total + cr.total + cc.total + b.total;
    const auto valid = run_control.actual == workload.expected;
    const auto rss_delta = static_cast<std::int64_t>(populated_rss) - static_cast<std::int64_t>(empty_rss);

    std::cout << std::fixed << std::setprecision(3) << "{\n"
              << "  \"schema_version\": 1,\n"
              << "  \"benchmark\": \"portable_high_count_map\",\n"
              << "  \"engine\": \"hgraph-current\",\n"
              << "  \"runtime\": \"native-cpp\",\n"
              << "  \"profile\": \"" << profile_name << "\",\n"
              << "  \"topology\": {\"flat_items\": " << workload.selected_profile.flat_count
              << ", \"parents\": " << workload.selected_profile.parent_count
              << ", \"children_per_parent\": " << child_count
              << ", \"active_inner_graphs\": " << workload.selected_profile.active_inner_graphs() << "},\n"
              << "  \"workload\": {\"cycles\": " << cycles << "},\n"
              << "  \"timing\": {\"workload_generation_ns\": " << generation_ns
              << ", \"wiring_ns\": " << wiring_ns << ", \"runtime_factory_ns\": " << factory_ns
              << ", \"runtime_start_ns\": " << start_ns << ", \"population_ns\": " << population_ns
              << ", \"measured_cycles_ns\": " << static_cast<std::uint64_t>(measured_ns)
              << ", \"wall_run_ns\": " << wall_run_ns << ", \"final_stop_ns\": " << stop_ns << "},\n"
              << "  \"latency\": {\n";
    print_summary("ordinary", o, true);
    print_summary("flat_parent_removal", fr, true);
    print_summary("flat_parent_restoration_cleanup", fc, true);
    print_summary("child_removal", cr, true);
    print_summary("child_restoration_cleanup", cc, true);
    print_summary("broadcast", b, false);
    std::cout << "  },\n"
              << "  \"output\": {\"actual\": {\"count\": " << run_control.actual.count
              << ", \"sum\": " << run_control.actual.sum << ", \"xor\": " << run_control.actual.xor_value
              << "}, \"expected\": {\"count\": " << workload.expected.count
              << ", \"sum\": " << workload.expected.sum << ", \"xor\": " << workload.expected.xor_value
              << "}, \"valid\": " << (valid ? "true" : "false") << "},\n"
              << "  \"memory\": {\"baseline_rss_bytes\": " << baseline_rss
              << ", \"after_wiring_rss_bytes\": " << after_wiring_rss
              << ", \"empty_runtime_rss_bytes\": " << empty_rss
              << ", \"populated_rss_bytes\": " << populated_rss
              << ", \"before_stop_rss_bytes\": " << before_stop_rss
              << ", \"graph_rss_delta_bytes\": " << graph_rss_delta
              << ", \"peak_rss_bytes\": " << reported_peak_rss
              << ", \"after_stop_rss_bytes\": " << after_stop_rss
              << ", \"populated_rss_delta_bytes\": " << rss_delta
              << ", \"bytes_per_active_inner_graph\": "
              << static_cast<double>(rss_delta) / workload.selected_profile.active_inner_graphs() << "}\n}\n";
    return valid ? EXIT_SUCCESS : EXIT_FAILURE;
}

} // namespace

int main(int argc, char **argv) {
    try {
        std::string_view profile_name = "250k";
        std::size_t cycles = 10'000;
        for (int i = 1; i < argc; ++i) {
            const std::string_view option = argv[i];
            if (option == "--profile" && ++i < argc) profile_name = argv[i];
            else if (option == "--cycles" && ++i < argc) cycles = std::stoull(argv[i]);
            else if (option == "--verify-workload") { verify_workload_manifest(); return EXIT_SUCCESS; }
            else throw std::invalid_argument("unknown or incomplete option");
        }
        if (cycles == 0) throw std::invalid_argument("cycles must be positive");
        return run(profile_name, cycles);
    } catch (const std::exception &error) {
        std::cerr << "hgraph_current_high_count_map: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
