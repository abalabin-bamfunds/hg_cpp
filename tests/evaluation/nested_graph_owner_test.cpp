#include <bg/graph/compiled_graph.hpp>
#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/runtime_graph.hpp>
#include <bg/graph/runtime_graph_evaluator.hpp>
#include <bg/graph/runtime_graph_scheduling.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/node.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <utility>
#include <vector>

using namespace bg;

namespace {

[[nodiscard]] EvaluationTime time(i64 microseconds) {
    return MIN_DT + std::chrono::microseconds{microseconds};
}

class InnerGraphOwner {
  public:
    static void inner_graph_scheduled(void *receiver, std::size_t token) noexcept {
        static_cast<InnerGraphOwner *>(receiver)->ready_.push_back(token);
    }

    void evaluate() {
        evaluated_tokens_.insert(evaluated_tokens_.end(), ready_.begin(), ready_.end());
        ready_.clear();
    }

    const std::vector<std::size_t> &evaluated_tokens() const noexcept { return evaluated_tokens_; }

  private:
    std::vector<std::size_t> ready_;
    std::vector<std::size_t> evaluated_tokens_;
};

struct OwnerNode {
    void evaluate() const { owner->evaluate(); }

    inline static InnerGraphOwner *owner{};
};

struct InnerNode {
    static constexpr bool uses_timed_scheduling = true;

    int delay;

    void start(EvaluationContext *context) const {
        if (delay != 0) {
            context->schedule_at(context->evaluation_time() + std::chrono::microseconds{delay});
        }
    }

    void evaluate() const {}
};

CompiledGraph make_owner_graph() {

    const auto node = bg::node<OwnerNode>();
    auto wiring = GraphWiring{};
    {
        auto context = WiringContext{wiring};
        node();
    }
    wiring.finalize();
    return CompiledGraph{wiring};
}

CompiledGraph make_inner_graph(int delay) {

    const auto node = bg::node<InnerNode>();
    auto wiring = GraphWiring{};
    {
        auto context = WiringContext{wiring};
        node(delay);
    }
    wiring.finalize();
    return CompiledGraph{wiring};
}

} // namespace

TEST_CASE("multiple inner graphs identify immediate scheduling to their owner",
          "[evaluation][nested][scheduling][feature:nested-graph][shape:nested]") {
    const auto outer_compiled = make_owner_graph();
    const auto inner_compiled = make_inner_graph(0);
    auto outer_global_state = GlobalState{};
    auto outer = RuntimeGraph{outer_compiled, outer_global_state};
    auto owner = InnerGraphOwner{};
    auto evaluator = RuntimeGraphEvaluator{};
    evaluator.bind_owner(outer, 0, {.receiver = &owner, .inner_scheduled = InnerGraphOwner::inner_graph_scheduled});
    auto first = RuntimeGraph{inner_compiled, outer.allocation(), evaluator, 7};
    auto second = RuntimeGraph{inner_compiled, outer.allocation(), evaluator, 11};
    OwnerNode::owner = &owner;

    outer.start(time(1));
    first.start(time(1), outer_global_state);
    second.start(time(1), outer_global_state);
    second.schedule(0);
    first.schedule(0);
    outer.evaluate(time(1));

    CHECK(owner.evaluated_tokens() == std::vector<std::size_t>{11, 7});

    first.stop(outer_global_state);
    second.stop(outer_global_state);
    first.finalize(outer_global_state);
    second.finalize(outer_global_state);
    outer.stop();
}

TEST_CASE("multiple inner graphs register timers with the outer scheduler",
          "[evaluation][nested][scheduling][timed_scheduling][feature:nested-graph][shape:nested]") {
    const auto outer_compiled = make_owner_graph();
    const auto first_compiled = make_inner_graph(2);
    const auto second_compiled = make_inner_graph(4);
    auto outer_global_state = GlobalState{};
    auto outer = RuntimeGraph{outer_compiled, outer_global_state};
    auto owner = InnerGraphOwner{};
    auto evaluator = RuntimeGraphEvaluator{};
    evaluator.bind_owner(outer, 0, {.receiver = &owner, .inner_scheduled = InnerGraphOwner::inner_graph_scheduled});
    auto first = RuntimeGraph{first_compiled, outer.allocation(), evaluator, 7};
    auto second = RuntimeGraph{second_compiled, outer.allocation(), evaluator, 11};
    OwnerNode::owner = &owner;

    outer.start(time(1));
    first.start(time(1), outer_global_state);
    second.start(time(1), outer_global_state);
    REQUIRE(outer.next_scheduled_time() == time(3));

    outer.evaluate(time(3));
    CHECK(owner.evaluated_tokens() == std::vector<std::size_t>{7});
    REQUIRE(outer.next_scheduled_time() == time(5));

    outer.evaluate(time(5));
    CHECK(owner.evaluated_tokens() == std::vector<std::size_t>{7, 11});
    CHECK_FALSE(outer.next_scheduled_time());

    first.stop(outer_global_state);
    second.stop(outer_global_state);
    first.finalize(outer_global_state);
    second.finalize(outer_global_state);
    outer.stop();
}

TEST_CASE("stopping one inner graph removes only its scheduled time",
          "[evaluation][nested][lifecycle][timed_scheduling][feature:nested-graph][shape:nested]"
          "[transition:stop]") {
    const auto outer_compiled = make_owner_graph();
    const auto first_compiled = make_inner_graph(2);
    const auto second_compiled = make_inner_graph(4);
    auto outer_global_state = GlobalState{};
    auto outer = RuntimeGraph{outer_compiled, outer_global_state};
    auto owner = InnerGraphOwner{};
    auto evaluator = RuntimeGraphEvaluator{};
    evaluator.bind_owner(outer, 0, {.receiver = &owner, .inner_scheduled = InnerGraphOwner::inner_graph_scheduled});
    auto first = RuntimeGraph{first_compiled, outer.allocation(), evaluator, 7};
    auto second = RuntimeGraph{second_compiled, outer.allocation(), evaluator, 11};
    OwnerNode::owner = &owner;

    outer.start(time(1));
    first.start(time(1), outer_global_state);
    second.start(time(1), outer_global_state);
    REQUIRE(outer.next_scheduled_time() == time(3));

    first.stop(outer_global_state);
    REQUIRE(outer.next_scheduled_time() == time(5));
    outer.evaluate(time(5));

    CHECK(owner.evaluated_tokens() == std::vector<std::size_t>{11});

    second.stop(outer_global_state);
    first.finalize(outer_global_state);
    second.finalize(outer_global_state);
    outer.stop();
}
