#include <bg/graph/compiled_graph.hpp>
#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/runtime_graph.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/node.hpp>
#include <bg/periodic_callback.hpp>
#include <bg/queue.hpp>
#include <bg/queue_mode.hpp>
#include <bg/run_graph.hpp>
#include <bg/simulation_evaluation_engine.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <latch>
#include <mutex>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
using Nanoseconds = std::chrono::nanoseconds;

struct LiteralBatch {
    std::vector<bg::i64> values;
    std::vector<::literal::TimeSeriesLiteral> literals;
};

struct Options {
    bool timer_only{};
    std::size_t messages{100'000};
    std::size_t latency_messages{1'000};
    std::size_t timer_cycles{1'000'000};
    std::size_t samples{7};
    std::size_t warmup_messages{2'000};
    std::size_t warmup_latency_messages{50};
    std::size_t warmup_timer_cycles{20'000};
    std::chrono::milliseconds idle_gap{1};
};

struct QueueResults {
    std::vector<bg::i64> values;
};

[[nodiscard]] auto expected_sum(std::size_t count) -> std::uint64_t {
    return static_cast<std::uint64_t>(count) * (count + 1) / 2;
}

[[nodiscard]] auto literals_for(std::size_t producer, std::size_t producers, std::size_t total_messages)
    -> LiteralBatch {
    const auto per_producer = total_messages / producers;
    auto batch = LiteralBatch{};
    batch.values.reserve(per_producer);
    for (auto sequence = std::size_t{}; sequence < per_producer; ++sequence) {
        batch.values.push_back(static_cast<bg::i64>(producer * per_producer + sequence + 1));
    }
    batch.literals.reserve(per_producer);
    for (const auto &value : batch.values) {
        batch.literals.emplace_back(::literal::TimeSeriesLiteral(value));
    }
    return batch;
}

void check_producer_sequences(const QueueResults &results, std::size_t producers, std::size_t total_messages) {
    const auto per_producer = total_messages / producers;
    auto next_sequence = std::vector<std::size_t>(producers);
    auto checksum = std::uint64_t{};
    for (const auto value : results.values) {
        if (value <= 0 || static_cast<std::uint64_t>(value) > total_messages) {
            throw std::runtime_error{"queue write benchmark produced an out-of-range value"};
        }

        const auto zero_based = static_cast<std::size_t>(value - 1);
        const auto producer = zero_based / per_producer;
        const auto sequence = zero_based % per_producer;
        if (producer >= producers || sequence != next_sequence[producer]) {
            throw std::runtime_error{"queue write benchmark changed per-producer sequence order"};
        }
        ++next_sequence[producer];
        checksum += static_cast<std::uint64_t>(value);
    }

    if (results.values.size() != total_messages ||
        std::ranges::any_of(next_sequence, [per_producer](auto count) { return count != per_producer; }) ||
        checksum != expected_sum(total_messages)) {
        throw std::runtime_error{"queue write benchmark count or checksum mismatch"};
    }
}

struct RuntimeQueue {
    bg::GraphWiring wiring;
    bg::WiringPoint output;
    bg::CompiledGraph compiled;
    bg::GlobalState global_state;
    bg::RuntimeGraph runtime;
    bg::Queue queue;
    QueueResults results;

    explicit RuntimeQueue(bg::QueueMode mode)
        : wiring{}, compiled{[&] {
              const auto queue_node = bg::queue<bg::TS<bg::i64>>(
                  bg::start = [this](bg::Queue handle) { queue = std::move(handle); }, bg::mode = mode);
              const auto sink = bg::node([this](bg::In<bg::TS<bg::i64>> input) {
                  results.values.push_back(*input);
              });
              {
                  auto context = bg::WiringContext{wiring};
                  output = queue_node();
                  sink(output);
              }
              wiring.finalize();
              return bg::CompiledGraph{wiring};
          }()},
          runtime{compiled, global_state} {}

    void start() { runtime.start(bg::MIN_DT + std::chrono::microseconds{1}); }
    void stop() { runtime.stop(); }
};

[[nodiscard]] auto elapsed_ns(Clock::time_point start, Clock::time_point finish) -> std::int64_t {
    return std::chrono::duration_cast<Nanoseconds>(finish - start).count();
}

void drain_standard(RuntimeQueue &graph, std::size_t messages) {
    const auto start_time = bg::MIN_DT + std::chrono::microseconds{1};
    for (auto message = std::size_t{}; message < messages; ++message) {
        graph.runtime.evaluate(start_time + std::chrono::microseconds{static_cast<std::int64_t>(message + 2)});
    }
}

void report_samples(std::string_view workload, std::size_t variant, const std::vector<double> &samples,
                    std::string_view unit = "message") {
    auto sorted = samples;
    std::ranges::sort(sorted);
    const auto median = sorted[sorted.size() / 2];
    const auto minimum = sorted.front();
    const auto maximum = sorted.back();

    std::cout << "{\"kind\":\"summary\",\"workload\":\"" << workload << "\",\"variant\":" << variant
              << ",\"samples\":" << samples.size() << ",\"min_ns_per_" << unit << "\":" << minimum
              << ",\"median_ns_per_" << unit << "\":" << median << ",\"max_ns_per_" << unit << "\":"
              << maximum << ",\"raw_ns_per_" << unit << "\":[";
    for (auto index = std::size_t{}; index < samples.size(); ++index) {
        if (index != 0) {
            std::cout << ',';
        }
        std::cout << samples[index];
    }
    std::cout << "]}\n";
}

[[nodiscard]] auto run_write_sample(std::size_t producers, std::size_t messages) -> std::int64_t {
    auto graph = RuntimeQueue{bg::QueueMode::standard};
    graph.results.values.reserve(messages);
    graph.start();
    auto queues = std::vector<bg::Queue>(producers, graph.queue);
    auto payloads = std::vector<LiteralBatch>{};
    payloads.reserve(producers);
    for (auto producer = std::size_t{}; producer < producers; ++producer) {
        payloads.push_back(literals_for(producer, producers, messages));
    }

    auto start_gate = std::latch{1};
    auto finished = std::latch{static_cast<std::ptrdiff_t>(producers)};
    auto accepted = std::atomic<bool>{true};
    auto threads = std::vector<std::thread>{};
    threads.reserve(producers);
    for (auto producer = std::size_t{}; producer < producers; ++producer) {
        threads.emplace_back([&, producer, queue = std::move(queues[producer])]() mutable {
            start_gate.wait();
            for (const auto &literal : payloads[producer].literals) {
                if (!queue.write(literal)) {
                    accepted.store(false, std::memory_order_relaxed);
                    break;
                }
            }
            finished.count_down();
        });
    }

    const auto started = Clock::now();
    start_gate.count_down();
    finished.wait();
    const auto finished_at = Clock::now();
    for (auto &thread : threads) {
        thread.join();
    }
    if (!accepted.load(std::memory_order_relaxed)) {
        throw std::runtime_error{"queue write was rejected during an open runtime"};
    }

    drain_standard(graph, messages);
    check_producer_sequences(graph.results, producers, messages);
    graph.stop();
    return elapsed_ns(started, finished_at);
}

void benchmark_writes(std::size_t total_messages, std::size_t samples, std::size_t warmup_messages) {
    for (const auto producers : {std::size_t{1}, std::size_t{4}}) {
        const auto messages = total_messages - (total_messages % producers);
        const auto warmup = std::max(producers, warmup_messages - (warmup_messages % producers));
        (void)run_write_sample(producers, warmup);
        auto ns_per_message = std::vector<double>{};
        ns_per_message.reserve(samples);

        for (auto sample = std::size_t{}; sample < samples; ++sample) {
            const auto elapsed = run_write_sample(producers, messages);
            const auto per_message = static_cast<double>(elapsed) / messages;
            ns_per_message.push_back(per_message);
            std::cout << "{\"kind\":\"sample\",\"workload\":\"queue_write\",\"producers\":" << producers
                      << ",\"sample\":" << sample << ",\"messages\":" << messages << ",\"elapsed_ns\":"
                      << elapsed << ",\"ns_per_message\":" << per_message << ",\"checksum\":"
                      << expected_sum(messages) << "}\n";
        }
        report_samples("queue_write", producers, ns_per_message);
    }
}

void benchmark_batch_drain(std::size_t messages, std::size_t samples);

[[nodiscard]] auto run_scalar_drain(bg::QueueMode mode, std::size_t messages) -> std::int64_t {
    auto graph = RuntimeQueue{mode};
    graph.results.values.reserve(messages);
    graph.start();
    for (auto message = std::size_t{}; message < messages; ++message) {
        const auto value = static_cast<bg::i64>(message + 1);
        if (!graph.queue.write(::literal::TimeSeriesLiteral(value))) {
            throw std::runtime_error{"queue write was rejected during preload"};
        }
    }

    const auto started = Clock::now();
    if (mode == bg::QueueMode::standard) {
        drain_standard(graph, messages);
    } else {
        graph.runtime.evaluate(bg::MIN_DT + std::chrono::microseconds{2});
    }
    const auto finished = Clock::now();
    const auto expected_count = mode == bg::QueueMode::standard ? messages : std::size_t{1};
    const auto expected_last = static_cast<bg::i64>(messages);
    const auto checksum = std::ranges::fold_left(
        graph.results.values, std::uint64_t{}, [](auto sum, auto value) { return sum + value; });
    const auto expected_checksum = mode == bg::QueueMode::standard ? expected_sum(messages)
                                                                    : static_cast<std::uint64_t>(messages);
    if (graph.results.values.size() != expected_count || graph.results.values.back() != expected_last ||
        checksum != expected_checksum) {
        throw std::runtime_error{"queue graph drain count or final value mismatch"};
    }
    if (mode == bg::QueueMode::standard) {
        check_producer_sequences(graph.results, 1, messages);
    }
    graph.stop();
    return elapsed_ns(started, finished);
}

void benchmark_graph_drain(std::size_t messages, std::size_t samples) {
    for (const auto mode : {bg::QueueMode::standard, bg::QueueMode::elide}) {
        const auto variant = mode == bg::QueueMode::standard ? std::size_t{0} : std::size_t{1};
        (void)run_scalar_drain(mode, std::min(messages, std::size_t{2'000}));
        auto ns_per_message = std::vector<double>{};
        ns_per_message.reserve(samples);
        for (auto sample = std::size_t{}; sample < samples; ++sample) {
            const auto elapsed = run_scalar_drain(mode, messages);
            const auto per_message = static_cast<double>(elapsed) / messages;
            ns_per_message.push_back(per_message);
            const auto output_ticks = mode == bg::QueueMode::standard ? messages : std::size_t{1};
            std::cout << "{\"kind\":\"sample\",\"workload\":\"graph_drain\",\"mode\":\""
                      << (mode == bg::QueueMode::standard ? "standard" : "elide") << "\",\"sample\":" << sample
                      << ",\"messages\":" << messages << ",\"output_ticks\":" << output_ticks
                      << ",\"elapsed_ns\":" << elapsed << ",\"ns_per_message\":" << per_message << "}\n";
        }
        report_samples("graph_drain", variant, ns_per_message);
    }

    benchmark_batch_drain(messages, samples);
}

struct BatchRuntime {
    bg::GraphWiring wiring;
    bg::WiringPoint output;
    bg::CompiledGraph compiled;
    bg::GlobalState global_state;
    bg::RuntimeGraph runtime;
    bg::Queue queue;
    std::size_t value_count{};
    std::uint64_t checksum{};
    bg::i64 last_value{};

    BatchRuntime()
        : wiring{}, compiled{[&] {
              const auto queue_node = bg::queue<bg::TS<bg::List<bg::i64>>>(
                  bg::start = [this](bg::Queue handle) { queue = std::move(handle); }, bg::mode = bg::QueueMode::batch);
              const auto sink = bg::node([this](bg::In<bg::TS<bg::List<bg::i64>>> input) {
                  const auto values = input.value_view();
                  value_count = values.size();
                  for (auto index = std::size_t{}; index < values.size(); ++index) {
                      const auto value = values[index].value();
                      checksum += static_cast<std::uint64_t>(value);
                      last_value = value;
                  }
              });
              {
                  auto context = bg::WiringContext{wiring};
                  output = queue_node();
                  sink(output);
              }
              wiring.finalize();
              return bg::CompiledGraph{wiring};
          }()},
          runtime{compiled, global_state} {}

    void start() { runtime.start(bg::MIN_DT + std::chrono::microseconds{1}); }
    void stop() { runtime.stop(); }
};

[[nodiscard]] auto run_batch_drain(std::size_t messages) -> std::int64_t {
    auto graph = BatchRuntime{};
    graph.start();
    for (auto message = std::size_t{}; message < messages; ++message) {
        const auto value = static_cast<bg::i64>(message + 1);
        if (!graph.queue.write(::literal::TimeSeriesLiteral(value))) {
            throw std::runtime_error{"batch queue write was rejected during preload"};
        }
    }

    const auto started = Clock::now();
    graph.runtime.evaluate(bg::MIN_DT + std::chrono::microseconds{2});
    const auto finished = Clock::now();
    if (graph.value_count != messages || graph.checksum != expected_sum(messages) ||
        graph.last_value != static_cast<bg::i64>(messages)) {
        throw std::runtime_error{"batch queue graph drain count or checksum mismatch"};
    }
    graph.stop();
    return elapsed_ns(started, finished);
}

void benchmark_batch_drain(std::size_t messages, std::size_t samples) {
    (void)run_batch_drain(std::min(messages, std::size_t{2'000}));
    auto ns_per_message = std::vector<double>{};
    ns_per_message.reserve(samples);
    for (auto sample = std::size_t{}; sample < samples; ++sample) {
        const auto elapsed = run_batch_drain(messages);
        const auto per_message = static_cast<double>(elapsed) / messages;
        ns_per_message.push_back(per_message);
        std::cout << "{\"kind\":\"sample\",\"workload\":\"graph_drain\",\"mode\":\"batch\",\"sample\":"
                  << sample << ",\"messages\":" << messages << ",\"output_ticks\":1,\"elapsed_ns\":" << elapsed
                  << ",\"ns_per_message\":" << per_message << ",\"checksum\":" << expected_sum(messages)
                  << "}\n";
    }
    report_samples("graph_drain", 2, ns_per_message);
}

struct LatencyState {
    std::mutex mutex;
    std::condition_variable changed;
    bg::Queue queue;
    bool ready{};
    bool finished{};
    std::exception_ptr error;
    bg::i64 observed_value{};
    Clock::time_point observed_at{};
};

void wait_for_queue(LatencyState &state) {
    auto lock = std::unique_lock{state.mutex};
    const auto ready = state.changed.wait_for(lock, std::chrono::seconds{10}, [&] {
        return state.ready || state.finished;
    });
    if (!ready) {
        throw std::runtime_error{"realtime queue graph did not start"};
    }
    if (state.error) {
        std::rethrow_exception(state.error);
    }
    if (!state.ready) {
        throw std::runtime_error{"realtime queue graph stopped before startup"};
    }
}

[[nodiscard]] auto deliver_and_measure(LatencyState &state, bg::Queue &queue, bg::i64 value,
                                       std::chrono::milliseconds idle_gap, bool record)
    -> std::int64_t {
    const auto started = Clock::now();
    if (!queue.write(::literal::TimeSeriesLiteral(value))) {
        throw std::runtime_error{"realtime queue write was rejected"};
    }

    auto lock = std::unique_lock{state.mutex};
    const auto delivered = state.changed.wait_for(lock, std::chrono::seconds{10}, [&] {
        return state.observed_value == value || state.finished;
    });
    if (!delivered) {
        throw std::runtime_error{"realtime queue message was not delivered"};
    }
    if (state.error) {
        std::rethrow_exception(state.error);
    }
    if (state.observed_value != value) {
        throw std::runtime_error{"realtime queue graph stopped before delivery"};
    }
    const auto latency = elapsed_ns(started, state.observed_at);
    lock.unlock();
    if (idle_gap != std::chrono::milliseconds::zero()) {
        std::this_thread::sleep_for(idle_gap);
    }
    if (!record) {
        return 0;
    }
    return latency;
}

void benchmark_realtime_latency(std::size_t messages, std::size_t samples, std::size_t warmup_messages,
                                std::chrono::milliseconds idle_gap) {
    for (auto sample = std::size_t{}; sample < samples; ++sample) {
        auto state = LatencyState{};
        auto stop_source = std::stop_source{};
        const auto queue_node = bg::queue<bg::TS<bg::i64>>(
            bg::start = [&state](bg::Queue queue) {
                auto lock = std::lock_guard{state.mutex};
                state.queue = std::move(queue);
                state.ready = true;
                state.changed.notify_all();
            });
        const auto sink = bg::node([&state](bg::In<bg::TS<bg::i64>> input) {
            auto lock = std::lock_guard{state.mutex};
            state.observed_value = *input;
            state.observed_at = Clock::now();
            state.changed.notify_all();
        });

        auto graph_thread = std::thread{[&] {
            try {
                bg::run_graph([&] { sink(queue_node()); },
                              bg::GraphRunOptions{.engine = bg::EvaluationEngine::realtime,
                                                  .stop_token = stop_source.get_token()});
            } catch (...) {
                auto lock = std::lock_guard{state.mutex};
                state.error = std::current_exception();
            }
            auto lock = std::lock_guard{state.mutex};
            state.finished = true;
            state.changed.notify_all();
        }};

        try {
            wait_for_queue(state);
            auto queue = [&] {
                auto lock = std::lock_guard{state.mutex};
                return state.queue;
            }();
            for (auto message = std::size_t{}; message < warmup_messages; ++message) {
                (void)deliver_and_measure(state, queue, static_cast<bg::i64>(message + 1), idle_gap, false);
            }

            auto latencies = std::vector<std::int64_t>{};
            latencies.reserve(messages);
            for (auto message = std::size_t{}; message < messages; ++message) {
                const auto value = static_cast<bg::i64>(warmup_messages + message + 1);
                latencies.push_back(deliver_and_measure(state, queue, value, idle_gap, true));
            }
            stop_source.request_stop();
            graph_thread.join();
            {
                auto lock = std::lock_guard{state.mutex};
                if (state.error) {
                    std::rethrow_exception(state.error);
                }
            }
            const auto raw_latencies = latencies;
            std::ranges::sort(latencies);
            const auto percentile = [&latencies](double p) {
                const auto index = static_cast<std::size_t>(p * static_cast<double>(latencies.size() - 1));
                return latencies[index];
            };
            std::cout << "{\"kind\":\"latency_sample\",\"workload\":\"realtime_delivery\",\"sample\":"
                      << sample << ",\"messages\":" << messages << ",\"idle_gap_ns\":"
                      << std::chrono::duration_cast<Nanoseconds>(idle_gap).count() << ",\"p50_ns\":"
                      << percentile(0.50) << ",\"p95_ns\":" << percentile(0.95) << ",\"p99_ns\":"
                      << percentile(0.99) << ",\"max_ns\":" << latencies.back() << ",\"raw_ns\":[";
            for (auto index = std::size_t{}; index < raw_latencies.size(); ++index) {
                if (index != 0) {
                    std::cout << ',';
                }
                std::cout << raw_latencies[index];
            }
            std::cout << "]}\n";
        } catch (...) {
            stop_source.request_stop();
            if (graph_thread.joinable()) {
                graph_thread.join();
            }
            throw;
        }
    }
}

struct TimerChainNode {
    static constexpr bool uses_timed_scheduling = true;

    void start(bg::EvaluationContext *context) const {
        context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
    }

    void evaluate(bg::EvaluationContext *context) const {
        ++evaluation_count;
        if (evaluation_count < target_cycles) {
            context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
        }
    }

    inline static std::size_t evaluation_count{};
    inline static std::size_t target_cycles{};
};

[[nodiscard]] auto run_timer_chain(std::size_t cycles, bool callback_enabled) -> std::pair<std::int64_t, std::size_t> {
    TimerChainNode::evaluation_count = 0;
    TimerChainNode::target_cycles = cycles;
    const auto source = bg::node<TimerChainNode>();
    auto callback_count = std::size_t{};
    const auto callback = callback_enabled
                              ? bg::PeriodicCallback{.callback = [&] { ++callback_count; },
                                                      .interval = std::chrono::milliseconds{50}}
                              : bg::PeriodicCallback{};
    const auto started = Clock::now();
    bg::run_graph([&] { (void)source(); },
                  bg::GraphRunOptions{.periodic_callback = callback});
    const auto finished = Clock::now();
    if (TimerChainNode::evaluation_count != cycles) {
        throw std::runtime_error{"timer chain cycle count mismatch"};
    }
    return {elapsed_ns(started, finished), callback_count};
}

void benchmark_periodic_callback(std::size_t cycles, std::size_t samples, std::size_t warmup_cycles) {
    (void)run_timer_chain(warmup_cycles, false);
    (void)run_timer_chain(warmup_cycles, true);
    auto disabled_samples = std::vector<double>{};
    auto enabled_samples = std::vector<double>{};
    disabled_samples.reserve(samples);
    enabled_samples.reserve(samples);

    for (auto sample = std::size_t{}; sample < samples; ++sample) {
        const auto run_disabled_first = sample % 2 == 0;
        auto disabled = std::pair<std::int64_t, std::size_t>{};
        auto enabled = std::pair<std::int64_t, std::size_t>{};
        if (run_disabled_first) {
            disabled = run_timer_chain(cycles, false);
            enabled = run_timer_chain(cycles, true);
        } else {
            enabled = run_timer_chain(cycles, true);
            disabled = run_timer_chain(cycles, false);
        }
        const auto disabled_ns_per_cycle = static_cast<double>(disabled.first) / cycles;
        const auto enabled_ns_per_cycle = static_cast<double>(enabled.first) / cycles;
        disabled_samples.push_back(disabled_ns_per_cycle);
        enabled_samples.push_back(enabled_ns_per_cycle);
        std::cout << "{\"kind\":\"timer_pair\",\"workload\":\"simulation_timer_chain\",\"sample\":" << sample
                  << ",\"cycles\":" << cycles << ",\"callback_interval_ms\":50,\"order\":\""
                  << (run_disabled_first ? "disabled_then_enabled" : "enabled_then_disabled")
                  << "\",\"disabled_ns_per_cycle\":" << disabled_ns_per_cycle << ",\"enabled_ns_per_cycle\":"
                  << enabled_ns_per_cycle << ",\"enabled_callbacks\":" << enabled.second << "}\n";
    }
    report_samples("timer_chain_callback_disabled", 0, disabled_samples, "cycle");
    report_samples("timer_chain_callback_enabled", 1, enabled_samples, "cycle");
}

[[nodiscard]] auto parse_size(std::string_view argument, std::string_view option) -> std::size_t {
    try {
        const auto value = std::stoull(std::string{argument});
        if (value == 0) {
            throw std::invalid_argument{"value must be positive"};
        }
        return static_cast<std::size_t>(value);
    } catch (const std::exception &) {
        throw std::invalid_argument{"invalid value for " + std::string{option}};
    }
}

[[nodiscard]] auto parse_options(int argc, char **argv) -> Options {
    auto options = Options{};
    for (auto argument = 1; argument < argc; ++argument) {
        const auto name = std::string_view{argv[argument]};
        if (name == "--timer-only") {
            options.timer_only = true;
            continue;
        }
        if (name == "--quick") {
            options.messages = 1'000;
            options.latency_messages = 50;
            options.timer_cycles = 10'000;
            options.samples = 1;
            options.warmup_messages = 100;
            options.warmup_latency_messages = 5;
            options.warmup_timer_cycles = 1'000;
            continue;
        }
        if (argument + 1 == argc) {
            throw std::invalid_argument{"missing value for " + std::string{name}};
        }
        const auto value = parse_size(argv[++argument], name);
        if (name == "--messages") {
            options.messages = value;
        } else if (name == "--latency-messages") {
            options.latency_messages = value;
        } else if (name == "--timer-cycles") {
            options.timer_cycles = value;
        } else if (name == "--samples") {
            options.samples = value;
        } else {
            throw std::invalid_argument{"unknown option " + std::string{name}};
        }
    }
    if (options.messages < 4 || options.samples == 0 || options.latency_messages == 0) {
        throw std::invalid_argument{"messages and latency messages must be positive; samples must be nonzero"};
    }
    return options;
}

} // namespace

int main(int argc, char **argv) {
    try {
        const auto options = parse_options(argc, argv);
        std::cout << "{\"kind\":\"metadata\",\"benchmark\":\"queue_realtime\",\"messages\":"
                  << options.messages << ",\"latency_messages\":" << options.latency_messages
                  << ",\"timer_cycles\":" << options.timer_cycles << ",\"samples\":" << options.samples
                  << ",\"timer_only\":" << std::boolalpha << options.timer_only
                  << ",\"idle_gap_ms\":" << options.idle_gap.count() << ",\"hardware_threads\":"
                  << std::thread::hardware_concurrency() << ",\"steady_clock_steady\":"
                  << Clock::is_steady << "}\n";

        if (!options.timer_only) {
            benchmark_writes(options.messages, options.samples, options.warmup_messages);
            benchmark_graph_drain(options.messages, options.samples);
            benchmark_realtime_latency(options.latency_messages, options.samples,
                                       options.warmup_latency_messages, options.idle_gap);
        }
        benchmark_periodic_callback(options.timer_cycles, options.samples, options.warmup_timer_cycles);
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "bg_queue_realtime_benchmark: " << error.what() << '\n';
        return 1;
    }
}
