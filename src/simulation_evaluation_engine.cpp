#include <bg/simulation_evaluation_engine.hpp>

#include <bg/details/evaluation_scratch.hpp>
#include <bg/evaluation_context.hpp>
#include <bg/graph/runtime_graph.hpp>

#include <stdexcept>

namespace bg {
namespace {

class SimulationWallClock final : public WallClock {
  public:
    EvaluationTime now() const noexcept override { return time_; }

    void set_time(EvaluationTime time) noexcept { time_ = time; }

  private:
    EvaluationTime time_{MIN_DT};
};

} // namespace

void SimulationEvaluationEngine::run(RuntimeGraph &runtime_graph, EvaluationTime start_time) const {
    auto scratch = details::EvaluationScratch{runtime_graph.allocator_.memory_resource()};
    auto wall_clock = SimulationWallClock{};
    wall_clock.set_time(start_time);
    auto start_context = EvaluationContext{start_time, runtime_graph, scratch, wall_clock};
    runtime_graph.start(start_context);

    const auto evaluate = [&](EvaluationTime time) {
        auto context = EvaluationContext{time, runtime_graph, scratch, wall_clock};
        if (!runtime_graph.evaluate(context)) {
            throw std::logic_error{"a pending evaluation escaped the outer graph"};
        }
    };
    evaluate(start_time);

    while (const auto time = runtime_graph.next_scheduled_time()) {
        wall_clock.set_time(*time);
        evaluate(*time);
    }

    const auto stop_time = runtime_graph.evaluation_time();
    auto stop_context = EvaluationContext{stop_time, runtime_graph, scratch, wall_clock};
    runtime_graph.stop(stop_context);
}

} // namespace bg
