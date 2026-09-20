#include <bg/graph/compiled_graph.hpp>
#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/runtime_graph.hpp>
#include <bg/graph/runtime_graph_owner.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/node.hpp>

#include <catch2/catch_test_macros.hpp>

#include "../runtime_graph_access.hpp"

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

    void future_schedule_changed(bg::RuntimeGraphOwnerContext &context, std::size_t token,
                                 std::optional<bg::EvaluationTime> time) {
        const auto previous_earliest = earliest();
        if (const auto existing = future_by_token_.find(token); existing != future_by_token_.end()) {
            future_.erase(existing->second);
            future_by_token_.erase(existing);
        }
        if (time) {
            future_by_token_.emplace(token, future_.emplace(*time, token));
        }

        update_outer_schedule(context, previous_earliest);
    }

    void evaluate(bg::EvaluationContext &context) {
        evaluated_immediate_.insert(evaluated_immediate_.end(), immediate_.begin(), immediate_.end());
        immediate_.clear();

        const auto previous_earliest = earliest();
        while (!future_.empty() && future_.begin()->first <= context.evaluation_time()) {
            const auto token = future_.begin()->second;
            evaluated_future_.push_back(token);
            future_by_token_.erase(token);
            future_.erase(future_.begin());
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
    const std::vector<std::size_t> &evaluated_future() const noexcept { return evaluated_future_; }

    static const bg::RuntimeGraphOwnerCallbacks callbacks;

  private:
    using Future = std::multimap<bg::EvaluationTime, std::size_t>;

    std::optional<bg::EvaluationTime> earliest() const noexcept {
        if (future_.empty()) {
            return std::nullopt;
        }
        return future_.begin()->first;
    }

    void update_outer_schedule(bg::RuntimeGraphOwnerContext &context,
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
    std::vector<std::size_t> evaluated_future_;
    Future future_;
    std::map<std::size_t, Future::iterator> future_by_token_;
    std::optional<bg::ScheduleHandle> outer_schedule_;
};

const bg::RuntimeGraphOwnerCallbacks InnerGraphOwner::callbacks{
    .inner_scheduled = [](void *receiver, std::size_t token) noexcept {
        static_cast<InnerGraphOwner *>(receiver)->scheduled(token);
    },
    .future_schedule_changed = [](bg::RuntimeGraphOwnerContext &context, void *receiver, std::size_t token,
                                  std::optional<bg::EvaluationTime> time) {
        static_cast<InnerGraphOwner *>(receiver)->future_schedule_changed(context, token, time);
    },
};

struct OwnerNode {
    static constexpr bool uses_future_scheduling = true;

    void evaluate(bg::EvaluationContext *context) const { owner->evaluate(*context); }

    inline static InnerGraphOwner *owner{};
};

struct InnerNode {
    static constexpr bool uses_future_scheduling = true;

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

void set_owner(bg::RuntimeGraph &inner, bg::RuntimeGraph &outer, InnerGraphOwner &owner, std::size_t token) {
    bg::testing::RuntimeGraphAccess::set_owner(
        inner, outer, 0, bg::RuntimeGraphOwner{.receiver = &owner, .token = token, .callbacks = &owner.callbacks});
}

} // namespace

TEST_CASE("multiple inner graphs identify immediate scheduling to their owner",
          "[evaluation][nested][scheduling][feature:nested-graph][shape:nested]") {
    const auto outer_compiled = make_owner_graph();
    const auto inner_compiled = make_inner_graph(0);
    auto outer = bg::RuntimeGraph{outer_compiled};
    auto first = bg::RuntimeGraph{inner_compiled, outer.allocation(), outer.global_state()};
    auto second = bg::RuntimeGraph{inner_compiled, outer.allocation(), outer.global_state()};
    auto owner = InnerGraphOwner{};
    OwnerNode::owner = &owner;
    set_owner(first, outer, owner, 7);
    set_owner(second, outer, owner, 11);

    outer.start(time(1));
    first.start(time(1));
    second.start(time(1));
    bg::testing::RuntimeGraphAccess::schedule(second, 0);
    bg::testing::RuntimeGraphAccess::schedule(first, 0);
    outer.evaluate(time(1));

    CHECK(owner.evaluated_immediate() == std::vector<std::size_t>{11, 7});

    first.stop();
    second.stop();
    outer.stop();
}

TEST_CASE("multiple inner graphs report future schedule changes without an owner scan",
          "[evaluation][nested][scheduling][future_scheduling][feature:nested-graph][shape:nested]") {
    const auto outer_compiled = make_owner_graph();
    const auto first_compiled = make_inner_graph(2);
    const auto second_compiled = make_inner_graph(4);
    auto outer = bg::RuntimeGraph{outer_compiled};
    auto first = bg::RuntimeGraph{first_compiled, outer.allocation(), outer.global_state()};
    auto second = bg::RuntimeGraph{second_compiled, outer.allocation(), outer.global_state()};
    auto owner = InnerGraphOwner{};
    OwnerNode::owner = &owner;
    set_owner(first, outer, owner, 7);
    set_owner(second, outer, owner, 11);

    outer.start(time(1));
    first.start(time(1));
    second.start(time(1));
    REQUIRE(outer.next_scheduled_time() == time(3));

    outer.evaluate(time(3));
    CHECK(owner.evaluated_future() == std::vector<std::size_t>{7});
    REQUIRE(outer.next_scheduled_time() == time(5));

    outer.evaluate(time(5));
    CHECK(owner.evaluated_future() == std::vector<std::size_t>{7, 11});
    CHECK_FALSE(outer.next_scheduled_time());

    first.stop();
    second.stop();
    outer.stop();
}

TEST_CASE("stopping one inner graph removes only its keyed future schedule",
          "[evaluation][nested][lifecycle][future_scheduling][feature:nested-graph][shape:nested]"
          "[transition:stop]") {
    const auto outer_compiled = make_owner_graph();
    const auto first_compiled = make_inner_graph(2);
    const auto second_compiled = make_inner_graph(4);
    auto outer = bg::RuntimeGraph{outer_compiled};
    auto first = bg::RuntimeGraph{first_compiled, outer.allocation(), outer.global_state()};
    auto second = bg::RuntimeGraph{second_compiled, outer.allocation(), outer.global_state()};
    auto owner = InnerGraphOwner{};
    OwnerNode::owner = &owner;
    set_owner(first, outer, owner, 7);
    set_owner(second, outer, owner, 11);

    outer.start(time(1));
    first.start(time(1));
    second.start(time(1));
    REQUIRE(outer.next_scheduled_time() == time(3));

    first.stop();
    REQUIRE(outer.next_scheduled_time() == time(5));
    outer.evaluate(time(5));

    CHECK(owner.evaluated_future() == std::vector<std::size_t>{11});

    second.stop();
    outer.stop();
}
