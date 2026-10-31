#include <bg/evaluation_context.hpp>
#include <bg/global_state.hpp>
#include <bg/graph/compiled_graph.hpp>
#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/runtime_graph.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/library.hpp>
#include <bg/library/operators.hpp>
#include <bg/node.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string_view>

namespace {

using clock_type = std::chrono::steady_clock;
constexpr auto WarmupCycles = std::size_t{10'000};
constexpr auto MeasuredCycles = std::size_t{100'000};
constexpr auto SampleCount = std::size_t{7};

struct RegexTextSource {
    static constexpr bool uses_timed_scheduling = true;

    bg::Scalar<bool> found;
    bg::Out<bg::TS<bg::string>> output;

    void start(bg::EvaluationContext *context) const { context->schedule_now(); }

    void evaluate(bg::EvaluationContext *context) const {
        output.set(found.value() ? "prefix-needle-suffix" : "prefix-missing-suffix");
        context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
    }
};

std::size_t wire_graph(bg::GraphWiring &wiring, bool found, std::string_view pattern, bool captures) {
    static const auto source = bg::node<RegexTextSource>();
    auto output_identity = std::optional<bg::NodeCallIdentity>{};
    {
        auto context = bg::WiringContext{wiring};
        const auto text = source(found);
        if (captures) {
            const auto output = bg::operators::match_(pattern, text);
            output_identity = wiring.node_call_identity(static_cast<bg::WiringPoint>(output));
        } else {
            const auto output = bg::operators::matches(pattern, text);
            output_identity = wiring.node_call_identity(static_cast<bg::WiringPoint>(output));
        }
    }

    wiring.finalize();
    if (!output_identity) {
        throw std::runtime_error{"regex benchmark output is not backed by a node"};
    }
    return wiring.node_index(*output_identity);
}

class RegexBenchmarkGraph {
  public:
    RegexBenchmarkGraph(bool found, std::string_view pattern, bool captures)
        : output_node_index_{wire_graph(wiring_, found, pattern, captures)}, captures_{captures}, compiled_{wiring_},
          runtime_{compiled_, global_state_} {
        if (compiled_.node_count() != 2) {
            throw std::runtime_error{"regex benchmark graph must retain its source and regex nodes"};
        }
    }

    void start() { runtime_.start(bg::MIN_DT + std::chrono::microseconds{1}); }

    void run(std::size_t first_cycle, std::size_t cycle_count) {
        for (auto cycle = first_cycle; cycle < first_cycle + cycle_count; ++cycle) {
            const auto time = bg::MIN_DT + std::chrono::microseconds{static_cast<std::int64_t>(cycle)};
            runtime_.evaluate(time);
        }
    }

    double measure(std::size_t first_cycle) {
        const auto start = clock_type::now();
        run(first_cycle, MeasuredCycles);
        const auto elapsed = std::chrono::duration<double, std::nano>(clock_type::now() - start).count();
        return elapsed / MeasuredCycles;
    }

    void check_output(bool expected) {
        const auto output = runtime_.node(output_node_index_);
        const auto valid = output.output_last_updated().valid();
        if (captures_) {
            if (valid != expected) {
                throw std::runtime_error{"regex benchmark produced an unexpected capture validity"};
            }
            return;
        }

        if (!valid || *static_cast<const bool *>(output.output_value()) != expected) {
            throw std::runtime_error{"regex benchmark produced an unexpected result"};
        }
    }

    void stop() { runtime_.stop(); }

  private:
    bg::GraphWiring wiring_;
    std::size_t output_node_index_;
    bool captures_;
    bg::CompiledGraph compiled_;
    bg::GlobalState global_state_;
    bg::RuntimeGraph runtime_;
};

double median(std::array<double, SampleCount> samples) {
    std::ranges::sort(samples);
    return samples[SampleCount / 2];
}

void benchmark(bool found, std::string_view pattern, std::size_t group_count) {
    auto matches = RegexBenchmarkGraph{found, pattern, false};
    auto match = RegexBenchmarkGraph{found, pattern, true};
    matches.start();
    match.start();

    matches.run(2, WarmupCycles);
    match.run(2, WarmupCycles);

    auto matches_samples = std::array<double, SampleCount>{};
    auto match_samples = std::array<double, SampleCount>{};
    auto first_cycle = WarmupCycles + 2;
    for (auto sample = std::size_t{}; sample < SampleCount; ++sample) {
        if (sample % 2 == 0) {
            matches_samples[sample] = matches.measure(first_cycle);
            match_samples[sample] = match.measure(first_cycle);
        } else {
            match_samples[sample] = match.measure(first_cycle);
            matches_samples[sample] = matches.measure(first_cycle);
        }
        first_cycle += MeasuredCycles;
    }

    matches.check_output(found);
    match.check_output(found);
    matches.stop();
    match.stop();

    std::cout << (found ? "success" : "failure") << " capture_groups=" << group_count
              << " matches_median_ns_per_cycle=" << median(matches_samples)
              << " match_median_ns_per_cycle=" << median(match_samples) << '\n';
}

} // namespace

int main() {
    std::cout << std::fixed << std::setprecision(2);
    benchmark(true, "(needle)", 1);
    benchmark(true, "(nee)(dle)", 2);
    benchmark(false, "(needle)", 1);
    benchmark(false, "(nee)(dle)", 2);
}
