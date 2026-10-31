#include <bg/graph/compiled_graph.hpp>
#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/runtime_graph.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/node.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory_resource>

namespace {

struct SchedulingNode {
    std::uint32_t id;

    void evaluate() const noexcept { ++evaluation_count; }

    inline static std::uint64_t evaluation_count{};
};

class CountingMemoryResource : public std::pmr::memory_resource {
  public:
    std::size_t outstanding_bytes{};

  private:
    void *do_allocate(std::size_t bytes, std::size_t alignment) override {
        outstanding_bytes += bytes;
        return std::pmr::new_delete_resource()->allocate(bytes, alignment);
    }

    void do_deallocate(void *pointer, std::size_t bytes, std::size_t alignment) override {
        outstanding_bytes -= bytes;
        std::pmr::new_delete_resource()->deallocate(pointer, bytes, alignment);
    }

    bool do_is_equal(const std::pmr::memory_resource &other) const noexcept override { return this == &other; }
};

[[nodiscard]] auto make_compiled_graph(std::size_t node_count) {
    const auto schedule_node = bg::node<SchedulingNode>();
    auto wiring = bg::GraphWiring{};
    {
        auto context = bg::WiringContext{wiring};
        for (auto node_index = std::size_t{}; node_index < node_count; ++node_index) {
            schedule_node(static_cast<std::uint32_t>(node_index));
        }
    }

    wiring.finalize();
    return bg::CompiledGraph{wiring};
}

void report_memory(std::size_t node_count) {
    const auto compiled = make_compiled_graph(node_count);
    auto global_state = bg::GlobalState{};
    auto memory = CountingMemoryResource{};
    auto runtime = bg::RuntimeGraph{compiled, bg::allocator{memory}, global_state};
    const auto total_instance_bytes = sizeof(runtime) + memory.outstanding_bytes;
    std::cout << "memory nodes=" << node_count << " RuntimeGraph=" << sizeof(runtime)
              << " compiled_fixed=" << compiled.fixed_storage_size()
              << " runtime_storage=" << compiled.runtime_storage_size()
              << " runtime_heap_bytes=" << memory.outstanding_bytes << " total_instance=" << total_instance_bytes
              << '\n';
}

void benchmark_schedule_and_evaluate(std::size_t node_count, std::size_t cycles, bool sparse) {
    constexpr auto warmup_cycles = std::size_t{20'000};
    const auto compiled = make_compiled_graph(node_count);
    auto global_state = bg::GlobalState{};
    auto runtime = bg::RuntimeGraph{compiled, global_state};
    const auto start_time = bg::MIN_DT + std::chrono::microseconds{1};
    runtime.start(start_time);
    SchedulingNode::evaluation_count = 0;

    const auto run_cycles = [&](std::size_t first_cycle, std::size_t cycle_count) {
        for (auto cycle = first_cycle; cycle < first_cycle + cycle_count; ++cycle) {
            if (sparse) {
                runtime.schedule(node_count - 1);
            } else {
                for (auto node_index = std::size_t{}; node_index < node_count; ++node_index) {
                    runtime.schedule(node_index);
                }
            }
            runtime.evaluate(bg::MIN_DT + std::chrono::microseconds{static_cast<std::int64_t>(cycle + 1)});
        }
    };

    run_cycles(1, warmup_cycles);
    SchedulingNode::evaluation_count = 0;
    const auto start = std::chrono::steady_clock::now();
    run_cycles(warmup_cycles + 1, cycles);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    const auto nanoseconds = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();
    const auto expected_evaluations = static_cast<std::uint64_t>(sparse ? 1 : node_count) * cycles;
    if (SchedulingNode::evaluation_count != expected_evaluations) {
        std::cerr << "evaluation checksum mismatch: expected " << expected_evaluations << ", got "
                  << SchedulingNode::evaluation_count << '\n';
        std::exit(1);
    }

    std::cout << "throughput workload=" << (sparse ? "sparse" : "dense") << " nodes=" << node_count
              << " scheduled_per_cycle=" << (sparse ? 1 : node_count) << " cycles=" << cycles << " ns_per_cycle="
              << static_cast<double>(nanoseconds) / cycles << " cycles_per_second="
              << static_cast<double>(cycles) * 1.0e9 / nanoseconds << " evaluations="
              << SchedulingNode::evaluation_count << '\n';
}

} // namespace

int main() {
    std::cout << "sizeof(RuntimeGraph)=" << sizeof(bg::RuntimeGraph) << '\n';
    for (const auto node_count : {std::size_t{32}, std::size_t{56}, std::size_t{57}, std::size_t{64},
                                  std::size_t{65}}) {
        report_memory(node_count);
    }
    benchmark_schedule_and_evaluate(32, 200'000, false);
    benchmark_schedule_and_evaluate(32, 200'000, true);
}
