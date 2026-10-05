#include <bg/graph/compiled_graph.hpp>
#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/runtime_graph.hpp>
#include <bg/graph/runtime_graph_evaluator.hpp>
#include <bg/graph/runtime_graph_scheduling.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/node.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <map>
#include <optional>
#include <utility>
#include <vector>

namespace {

[[nodiscard]] bg::EvaluationTime time(std::int64_t microseconds) {
    return bg::MIN_DT + std::chrono::microseconds{microseconds};
}

class InnerGraphOwner {
  public:
    void scheduled(std::size_t token) { immediate_.push_back(token); }

    void timed_schedule_changed(bg::RuntimeGraphSchedulingContext &context, std::size_t token,
                                 std::optional<bg::EvaluationTime> time) {
        const auto previous_earliest = earliest();
        if (const auto existing = schedule_by_token_.find(token); existing != schedule_by_token_.end()) {
            schedule_.erase(existing->second);
            schedule_by_token_.erase(existing);
        }
        if (time) {
            schedule_by_token_.emplace(token, schedule_.emplace(*time, token));
        }

        update_outer_schedule(context, previous_earliest);
    }

    void evaluate(bg::EvaluationContext &context) {
        evaluated_immediate_.insert(evaluated_immediate_.end(), immediate_.begin(), immediate_.end());
        immediate_.clear();

        const auto previous_earliest = earliest();
        while (!schedule_.empty() && schedule_.begin()->first <= context.evaluation_time()) {
            const auto token = schedule_.begin()->second;
            evaluated_tokens_.push_back(token);
            schedule_by_token_.erase(token);
            schedule_.erase(schedule_.begin());
        }

        if (earliest() != previous_earliest) {
            if (outer_schedule_) {
                context.cancel(*outer_schedule_);
                outer_schedule_.reset();
            }
            if (const auto next = earliest()) {
                outer_schedule_ = context.schedule_at(*next);
            }
        }
    }

    const std::vector<std::size_t> &evaluated_immediate() const noexcept { return evaluated_immediate_; }
    const std::vector<std::size_t> &evaluated_tokens() const noexcept { return evaluated_tokens_; }

    static const bg::RuntimeGraphSchedulingCallbacks callbacks;

  private:
    using Schedule = std::multimap<bg::EvaluationTime, std::size_t>;

    std::optional<bg::EvaluationTime> earliest() const noexcept {
        if (schedule_.empty()) {
            return std::nullopt;
        }
        return schedule_.begin()->first;
    }

    void update_outer_schedule(bg::RuntimeGraphSchedulingContext &context,
                               std::optional<bg::EvaluationTime> previous_earliest) {
        if (earliest() == previous_earliest) {
            return;
        }
        if (outer_schedule_) {
            context.cancel(*outer_schedule_);
            outer_schedule_.reset();
        }
        if (const auto next = earliest()) {
            outer_schedule_ = context.schedule_at(*next);
        }
    }

    std::vector<std::size_t> immediate_;
    std::vector<std::size_t> evaluated_immediate_;
    std::vector<std::size_t> evaluated_tokens_;
    Schedule schedule_;
    std::map<std::size_t, Schedule::iterator> schedule_by_token_;
    std::optional<bg::ScheduleHandle> outer_schedule_;
};

const bg::RuntimeGraphSchedulingCallbacks InnerGraphOwner::callbacks{
    .inner_scheduled = [](void *receiver, std::size_t token) noexcept {
        static_cast<InnerGraphOwner *>(receiver)->scheduled(token);
    },
    .timed_schedule_changed = [](bg::RuntimeGraphSchedulingContext &context, void *receiver, std::size_t token,
                                  std::optional<bg::EvaluationTime> time) {
        static_cast<InnerGraphOwner *>(receiver)->timed_schedule_changed(context, token, time);
    },
};

struct OwnerNode {
    static constexpr bool uses_timed_scheduling = true;

    void evaluate(bg::EvaluationContext *context) const { owner->evaluate(*context); }

    inline static InnerGraphOwner *owner{};
};

struct InnerNode {
    static constexpr bool uses_timed_scheduling = true;

    int delay;

    void start(bg::EvaluationContext *context) const {
        if (delay != 0) {
            context->schedule_at(context->evaluation_time() + std::chrono::microseconds{delay});
        }
    }

    void evaluate() const {}
};

bg::CompiledGraph make_owner_graph() {
    const auto node = bg::node<OwnerNode>();
    auto wiring = bg::GraphWiring{};
    {
        auto context = bg::WiringContext{wiring};
        node();
    }
    return bg::CompiledGraph{wiring};
}

bg::CompiledGraph make_inner_graph(int delay) {
    const auto node = bg::node<InnerNode>();
    auto wiring = bg::GraphWiring{};
    {
        auto context = bg::WiringContext{wiring};
        node(delay);
    }
    return bg::CompiledGraph{wiring};
}

} // namespace

TEST_CASE("multiple inner graphs identify immediate scheduling to their owner",
          "[evaluation][nested][scheduling][feature:nested-graph][shape:nested]") {
    const auto outer_compiled = make_owner_graph();
    const auto inner_compiled = make_inner_graph(0);
    auto outer_global_state = bg::GlobalState{};
    auto outer = bg::RuntimeGraph{outer_compiled, outer_global_state};
    auto owner = InnerGraphOwner{};
    auto evaluator = bg::RuntimeGraphEvaluator{};
    evaluator.bind_owner(outer, 0, {.receiver = &owner, .callbacks = &owner.callbacks});
    auto first = bg::RuntimeGraph{inner_compiled, outer.allocation(), evaluator, 7};
    auto second = bg::RuntimeGraph{inner_compiled, outer.allocation(), evaluator, 11};
    OwnerNode::owner = &owner;

    outer.start(time(1));
    first.start(time(1), outer_global_state);
    second.start(time(1), outer_global_state);
    second.schedule(0);
    first.schedule(0);
    outer.evaluate(time(1));

    CHECK(owner.evaluated_immediate() == std::vector<std::size_t>{11, 7});

    first.stop(outer_global_state);
    second.stop(outer_global_state);
    first.finalize(outer_global_state);
    second.finalize(outer_global_state);
    outer.stop();
}

TEST_CASE("multiple inner graphs report scheduled-time changes without an owner scan",
          "[evaluation][nested][scheduling][timed_scheduling][feature:nested-graph][shape:nested]") {
    const auto outer_compiled = make_owner_graph();
    const auto first_compiled = make_inner_graph(2);
    const auto second_compiled = make_inner_graph(4);
    auto outer_global_state = bg::GlobalState{};
    auto outer = bg::RuntimeGraph{outer_compiled, outer_global_state};
    auto owner = InnerGraphOwner{};
    auto evaluator = bg::RuntimeGraphEvaluator{};
    evaluator.bind_owner(outer, 0, {.receiver = &owner, .callbacks = &owner.callbacks});
    auto first = bg::RuntimeGraph{first_compiled, outer.allocation(), evaluator, 7};
    auto second = bg::RuntimeGraph{second_compiled, outer.allocation(), evaluator, 11};
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
    auto outer_global_state = bg::GlobalState{};
    auto outer = bg::RuntimeGraph{outer_compiled, outer_global_state};
    auto owner = InnerGraphOwner{};
    auto evaluator = bg::RuntimeGraphEvaluator{};
    evaluator.bind_owner(outer, 0, {.receiver = &owner, .callbacks = &owner.callbacks});
    auto first = bg::RuntimeGraph{first_compiled, outer.allocation(), evaluator, 7};
    auto second = bg::RuntimeGraph{second_compiled, outer.allocation(), evaluator, 11};
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
