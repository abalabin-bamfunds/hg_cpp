#include <bg/evaluation_context.hpp>
#include <bg/graph/compiled_graph.hpp>
#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/runtime_graph.hpp>
#include <bg/graph/runtime_graph_evaluator.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/node.hpp>
#include <bg/simulation_evaluation_engine.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace {

constexpr auto start_time = bg::MIN_DT + std::chrono::microseconds{1};
constexpr auto construction_count = std::size_t{1'000'000};
constexpr auto evaluation_cycle_count = std::size_t{200'000};
constexpr auto sample_count = std::size_t{9};

struct ContextConstructionNode {
    void start(bg::EvaluationContext *context) const { context->schedule_now(); }

    void evaluate(bg::EvaluationContext *context) const {
        for (auto index = std::size_t{}; index < construction_count; ++index) {
            auto child = context->child(context->runtime_graph(), nullptr);
            construction_checksum += static_cast<std::uint64_t>(
                child.evaluation_time().time_since_epoch().count());
        }
    }

    inline static std::uint64_t construction_checksum{};
};

struct TimeReadNode {
    static constexpr bool uses_timed_scheduling = true;

    void start(bg::EvaluationContext *context) const { context->schedule_now(); }

    void evaluate(bg::EvaluationContext *context) const {
        ++evaluation_count;
        time_checksum += static_cast<std::uint64_t>(context->evaluation_time().time_since_epoch().count());
        if (evaluation_count < evaluation_cycle_count) {
            context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
        }
    }

    inline static std::uint64_t evaluation_count{};
    inline static std::uint64_t time_checksum{};
};

struct ScalarOutputNode {
    static constexpr bool uses_timed_scheduling = true;

    bg::Out<bg::TS<bg::i64>> output;

    void start(bg::EvaluationContext *context) const { context->schedule_now(); }

    void evaluate(bg::EvaluationContext *context) const {
        ++evaluation_count;
        const auto time = context->evaluation_time().time_since_epoch().count();
        time_checksum += static_cast<std::uint64_t>(time);
        output.set(static_cast<bg::i64>(time));
        if (evaluation_count < evaluation_cycle_count) {
            context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
        }
    }

    inline static std::uint64_t evaluation_count{};
    inline static std::uint64_t time_checksum{};
};

struct NestedEvaluationNode {
    static constexpr bool uses_timed_scheduling = true;

    void start(bg::EvaluationContext *context) const {
        evaluator->bind_owner(context->runtime_graph(), context->node_index());
        child_graph.emplace(*child_compiled_graph, context->runtime_graph().allocation(), *evaluator, 0);
        auto child = context->child(*child_graph, nullptr);
        child_graph->start(child);
    }

    void evaluate(bg::EvaluationContext *context) const {
        auto child = context->child(*child_graph, nullptr);
        if (!child_graph->evaluate(child)) {
            std::terminate();
        }
    }

    void stop(bg::EvaluationContext *context) const noexcept {
        auto child = context->child(*child_graph, nullptr);
        child_graph->stop(child);
        child_graph->finalize(child);
    }

    inline static bg::RuntimeGraphEvaluator *evaluator{};
    inline static std::optional<bg::RuntimeGraph> child_graph;
    inline static const bg::CompiledGraph *child_compiled_graph{};
};

[[nodiscard]] auto compile_graph(auto node_call) {
    auto wiring = bg::GraphWiring{};
    {
        auto context = bg::WiringContext{wiring};
        node_call();
    }
    wiring.finalize();
    return bg::CompiledGraph{wiring};
}

template <typename Function>
[[nodiscard]] auto measure(Function &&function) {
    const auto start = std::chrono::steady_clock::now();
    const auto checksum = function();
    const auto elapsed = std::chrono::steady_clock::now() - start;
    return std::pair{std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count(), checksum};
}

void report(std::string_view name, const std::vector<std::int64_t> &samples, std::uint64_t checksum,
            double units_per_sample, std::string_view unit_name) {
    auto sorted = samples;
    std::ranges::sort(sorted);
    std::cout << name << " samples_ns=";
    for (const auto sample : samples) {
        std::cout << sample << ',';
    }
    std::cout << " median_ns=" << sorted[sorted.size() / 2]
              << " min_ns=" << sorted.front() << " max_ns=" << sorted.back()
              << " median_ns_per_" << unit_name << '='
              << static_cast<double>(sorted[sorted.size() / 2]) / units_per_sample
              << " checksum=" << checksum << '\n';
}

void benchmark_context_construction(const bg::CompiledGraph &compiled) {
    auto samples = std::vector<std::int64_t>{};
    std::uint64_t expected_checksum{};
    for (auto sample_index = std::size_t{}; sample_index < sample_count; ++sample_index) {
        auto global_state = bg::GlobalState{};
        auto runtime_graph = bg::RuntimeGraph{compiled, global_state};
        ContextConstructionNode::construction_checksum = 0;
        const auto [elapsed, checksum] = measure([&] {
            bg::SimulationEvaluationEngine{}.run(runtime_graph, start_time);
            return ContextConstructionNode::construction_checksum;
        });
        const auto expected_sample_checksum =
            static_cast<std::uint64_t>(start_time.time_since_epoch().count()) * construction_count;
        if (checksum != expected_sample_checksum) {
            std::terminate();
        }
        expected_checksum = checksum;
        samples.push_back(elapsed);
    }
    report("child_context_construction", samples, expected_checksum, static_cast<double>(construction_count),
           "construction");
}

void benchmark_nested_evaluation(const bg::CompiledGraph &parent_compiled,
                                 const bg::CompiledGraph &child_compiled) {
    auto samples = std::vector<std::int64_t>{};
    std::uint64_t expected_checksum{};
    for (auto sample_index = std::size_t{}; sample_index < sample_count; ++sample_index) {
        auto global_state = bg::GlobalState{};
        auto evaluator = std::make_unique<bg::RuntimeGraphEvaluator>();
        NestedEvaluationNode::evaluator = evaluator.get();
        auto parent_graph = bg::RuntimeGraph{parent_compiled, global_state};
        NestedEvaluationNode::child_compiled_graph = &child_compiled;
        NestedEvaluationNode::child_graph.reset();
        TimeReadNode::evaluation_count = 0;
        TimeReadNode::time_checksum = 0;
        const auto [elapsed, checksum] = measure([&] {
            bg::SimulationEvaluationEngine{}.run(parent_graph, start_time);
            return TimeReadNode::time_checksum;
        });
        if (TimeReadNode::evaluation_count != evaluation_cycle_count) {
            std::terminate();
        }
        const auto first_time = start_time.time_since_epoch().count();
        const auto cycle_sum = static_cast<std::uint64_t>(evaluation_cycle_count) *
                               static_cast<std::uint64_t>(first_time) +
                               static_cast<std::uint64_t>(evaluation_cycle_count - 1) * evaluation_cycle_count / 2;
        if (TimeReadNode::time_checksum != cycle_sum) {
            std::terminate();
        }
        expected_checksum = checksum;
        samples.push_back(elapsed);
        NestedEvaluationNode::child_graph.reset();
        NestedEvaluationNode::evaluator = nullptr;
    }

    report("nested_graph_evaluation", samples, expected_checksum, static_cast<double>(evaluation_cycle_count),
           "cycle");
}

void benchmark_flat_evaluation(const bg::CompiledGraph &compiled) {
    auto samples = std::vector<std::int64_t>{};
    std::uint64_t expected_checksum{};
    for (auto sample_index = std::size_t{}; sample_index < sample_count; ++sample_index) {
        auto global_state = bg::GlobalState{};
        auto runtime_graph = bg::RuntimeGraph{compiled, global_state};
        TimeReadNode::evaluation_count = 0;
        TimeReadNode::time_checksum = 0;
        const auto [elapsed, checksum] = measure([&] {
            bg::SimulationEvaluationEngine{}.run(runtime_graph, start_time);
            return TimeReadNode::time_checksum;
        });
        if (TimeReadNode::evaluation_count != evaluation_cycle_count) {
            std::terminate();
        }
        const auto first_time = start_time.time_since_epoch().count();
        const auto cycle_sum = static_cast<std::uint64_t>(evaluation_cycle_count) *
                               static_cast<std::uint64_t>(first_time) +
                               static_cast<std::uint64_t>(evaluation_cycle_count - 1) * evaluation_cycle_count / 2;
        if (checksum != cycle_sum) {
            std::terminate();
        }
        expected_checksum = checksum;
        samples.push_back(elapsed);
    }

    report("flat_graph_evaluation", samples, expected_checksum, static_cast<double>(evaluation_cycle_count),
           "cycle");
}

void benchmark_scalar_output(const bg::CompiledGraph &compiled) {
    auto samples = std::vector<std::int64_t>{};
    std::uint64_t expected_checksum{};
    for (auto sample_index = std::size_t{}; sample_index < sample_count; ++sample_index) {
        auto global_state = bg::GlobalState{};
        auto runtime_graph = bg::RuntimeGraph{compiled, global_state};
        ScalarOutputNode::evaluation_count = 0;
        ScalarOutputNode::time_checksum = 0;
        const auto [elapsed, checksum] = measure([&] {
            bg::SimulationEvaluationEngine{}.run(runtime_graph, start_time);
            return ScalarOutputNode::time_checksum;
        });
        if (ScalarOutputNode::evaluation_count != evaluation_cycle_count) {
            std::terminate();
        }
        const auto first_time = start_time.time_since_epoch().count();
        const auto cycle_sum = static_cast<std::uint64_t>(evaluation_cycle_count) *
                                   static_cast<std::uint64_t>(first_time) +
                               static_cast<std::uint64_t>(evaluation_cycle_count - 1) * evaluation_cycle_count / 2;
        if (checksum != cycle_sum) {
            std::terminate();
        }
        expected_checksum = checksum;
        samples.push_back(elapsed);
    }

    report("scalar_output_evaluation", samples, expected_checksum, static_cast<double>(evaluation_cycle_count),
           "cycle");
}

} // namespace

int main() {
#if defined(__clang__)
    std::cout << "compiler=Clang " << __clang_version__;
#elif defined(__GNUC__)
    std::cout << "compiler=GCC " << __VERSION__;
#elif defined(_MSC_VER)
    std::cout << "compiler=MSVC " << _MSC_VER;
#else
    std::cout << "compiler=unknown";
#endif
#ifdef NDEBUG
    std::cout << " build=optimized";
#else
    std::cout << " build=debug";
#endif
    std::cout << " sizeof_EvaluationContext=" << sizeof(bg::EvaluationContext)
              << " sizeof_GlobalEvaluationContext=" << sizeof(bg::GlobalEvaluationContext)
              << " sizeof_RuntimeGraphEvaluator=" << sizeof(bg::RuntimeGraphEvaluator)
              << " sizeof_RuntimeGraph=" << sizeof(bg::RuntimeGraph)
              << " construction_count=" << construction_count
              << " evaluation_cycle_count=" << evaluation_cycle_count
              << " samples=" << sample_count << '\n';

    auto construction_node = bg::node<ContextConstructionNode>();
    const auto construction_graph = compile_graph([&] { construction_node(); });
    benchmark_context_construction(construction_graph);

    auto time_read_node = bg::node<TimeReadNode>();
    const auto child_graph = compile_graph([&] { time_read_node(); });
    benchmark_flat_evaluation(child_graph);
    auto scalar_output_node = bg::node<ScalarOutputNode>();
    const auto scalar_output_graph = compile_graph([&] { scalar_output_node(); });
    benchmark_scalar_output(scalar_output_graph);
    auto nested_node = bg::node<NestedEvaluationNode>();
    const auto parent_graph = compile_graph([&] { nested_node(); });
    benchmark_nested_evaluation(parent_graph, child_graph);
}
