#include <bg/evaluation_context.hpp>
#include <bg/graph/borrowed_input.hpp>
#include <bg/graph/compiled_graph.hpp>
#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/runtime_graph.hpp>
#include <bg/graph/runtime_graph_evaluator.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/node.hpp>
#include <bg/operators.hpp>
#include <bg/time_series/time_series_runtime_type.hpp>
#include <bg/time_series/tss/tss_input.hpp>
#include <bg/time_series/tss/tss_storage.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace {

[[nodiscard]] bg::EvaluationTime time(std::int64_t microseconds) {
    return bg::MIN_DT + std::chrono::microseconds{microseconds};
}

struct BorrowedInputSink {
    bg::In<bg::TS<bg::i64>> input;
    bg::In<bg::TSS<bg::i64>> set_input;

    void stop(bg::EvaluationContext *context) const noexcept {
        stop_value = *input;
        stop_time = input.last_updated();
        stop_evaluation_time = context->evaluation_time();
        stop_valid = input.valid();
        stop_set_valid = set_input.valid();
        stop_set_size = set_input.size();
        ++stop_count;
    }

    void evaluate(bg::EvaluationContext *context) const {
        observations.push_back({.value = *input,
                                .time = input.last_updated(),
                                .evaluation_time = context->evaluation_time(),
                                .valid = input.valid(),
                                .modified = input.modified(),
                                .set_valid = set_input.valid(),
                                .set_size = set_input.size(),
                                .set_modified = set_input.modified()});

        if (check_nested_scope) {
            check_nested_scope = false;
            auto &parent = context->runtime_graph();
            auto evaluator = bg::RuntimeGraphEvaluator{};
            evaluator.bind_owner(parent, context->node_index());
            auto nested = bg::RuntimeGraph{*compiled_graph, parent.allocation(), evaluator, 0};
            try {
                nested.start(*context);
            } catch (const std::invalid_argument &) {
                nested_scope_rejected = true;
            }
            nested.finalize(*context);
        }
    }

    struct Observation {
        bg::i64 value;
        bg::EvaluationTime time;
        bg::EvaluationTime evaluation_time;
        bool valid;
        bool modified;
        bool set_valid;
        std::size_t set_size;
        bool set_modified;
    };

    inline static const bg::CompiledGraph *compiled_graph{};
    inline static std::vector<Observation> observations;
    inline static bg::i64 stop_value{};
    inline static bg::EvaluationTime stop_time{};
    inline static bg::EvaluationTime stop_evaluation_time{};
    inline static bool stop_valid{};
    inline static bool stop_set_valid{};
    inline static std::size_t stop_set_size{};
    inline static std::size_t stop_count{};
    inline static bool check_nested_scope{};
    inline static bool nested_scope_rejected{};
};

struct BorrowedInputRunner {
    void start(bg::EvaluationContext *context) const {
        auto &parent = context->runtime_graph();
        evaluator = parent.allocation().new_object<bg::RuntimeGraphEvaluator>();
        evaluator->bind_owner(parent, context->node_index());
        child = parent.allocation().new_object<bg::RuntimeGraph>(*compiled_graph, parent.allocation(), *evaluator,
                                                                 context->node_index());

        auto state = bg::TimeSeriesState{context->evaluation_time()};
        auto value = bg::i64{11};
        const auto &runtime_type = use_different_runtime_type
                                       ? bg::resolve_time_series_runtime_type(
                                             bg::TS<bg::i64, bg::TimeSeriesGuarantee::deduplicated>::type())
                                       : bg::resolve_time_series_runtime_type(bg::TS<bg::i64>::type());
        auto set_value = bg::details::time_series_set_storage{};
        const auto &set_runtime_type = bg::resolve_time_series_runtime_type(bg::TSS<bg::i64>::type());
        const auto inputs = std::array{bg::BorrowedInput{&state, &value, &runtime_type,
                                                         bg::BorrowedInputChange::update},
                                       bg::BorrowedInput{&state, &set_value, &set_runtime_type,
                                                         bg::BorrowedInputChange::update}};
        auto section = bg::details::BorrowedInputSection{*child, inputs};
        auto child_context = context->child(section);
        try {
            child->start(child_context);
        } catch (const std::invalid_argument &) {
            if (!use_different_runtime_type) {
                throw;
            }
            different_runtime_type_rejected = true;
            child->finalize(child_context);
            parent.allocation().delete_object(child);
            child = nullptr;
            parent.allocation().delete_object(evaluator);
            evaluator = nullptr;
        }
    }

    void evaluate(bg::EvaluationContext *context) const {
        auto state = bg::TimeSeriesState{};
        auto value = bg::i64{};
        auto change = bg::BorrowedInputChange::unchanged;
        const auto evaluation_time = context->evaluation_time();

        if (evaluation_time == time(2)) {
            state.publish(time(1));
            state.invalidate(evaluation_time);
            value = 11;
            change = bg::BorrowedInputChange::invalidation;
        } else if (evaluation_time == time(3)) {
            state.publish(evaluation_time);
            value = 33;
            change = bg::BorrowedInputChange::revalidation;
        } else {
            state.publish(evaluation_time);
            value = 11;
        }

        const auto &runtime_type = bg::resolve_time_series_runtime_type(bg::TS<bg::i64>::type());
        auto set_state = bg::TimeSeriesState{evaluation_time};
        auto set_value = bg::details::time_series_set_storage{};
        const auto &set_runtime_type = bg::resolve_time_series_runtime_type(bg::TSS<bg::i64>::type());
        const auto inputs = std::array{bg::BorrowedInput{&state, &value, &runtime_type, change},
                                       bg::BorrowedInput{&set_state, &set_value, &set_runtime_type,
                                                         bg::BorrowedInputChange::update}};
        auto section = bg::details::BorrowedInputSection{*child, inputs};
        auto child_context = context->child(section);
        child->evaluate(child_context);
    }

    void stop(bg::EvaluationContext *context) const noexcept {
        if (child == nullptr) {
            return;
        }

        auto state = bg::TimeSeriesState{context->evaluation_time()};
        auto value = bg::i64{33};
        const auto &runtime_type = bg::resolve_time_series_runtime_type(bg::TS<bg::i64>::type());
        auto set_value = bg::details::time_series_set_storage{};
        const auto &set_runtime_type = bg::resolve_time_series_runtime_type(bg::TSS<bg::i64>::type());
        const auto inputs = std::array{bg::BorrowedInput{&state, &value, &runtime_type,
                                                         bg::BorrowedInputChange::unchanged},
                                       bg::BorrowedInput{&state, &set_value, &set_runtime_type,
                                                         bg::BorrowedInputChange::unchanged}};
        auto section = bg::details::BorrowedInputSection{*child, inputs};
        auto child_context = context->child(section);
        child->stop(child_context);
        child->finalize(child_context);
        context->runtime_graph().allocation().delete_object(child);
        child = nullptr;
        context->runtime_graph().allocation().delete_object(evaluator);
        evaluator = nullptr;
    }

    inline static const bg::CompiledGraph *compiled_graph{};
    inline static bg::RuntimeGraphEvaluator *evaluator{};
    inline static bg::RuntimeGraph *child{};
    inline static bool use_different_runtime_type{};
    inline static bool different_runtime_type_rejected{};
};

bg::CompiledGraph make_borrowed_input_graph() {
    auto wiring = bg::GraphWiring{};
    const auto input = wiring.borrowed_input(bg::TS<bg::i64>::type());
    const auto set_input = wiring.borrowed_input(bg::TSS<bg::i64>::type());
    const auto sink = bg::node<BorrowedInputSink>();
    {
        auto context = bg::WiringContext{wiring};
        sink(bg::operators::default_(input, bg::i64{7}), set_input);
    }
    wiring.finalize();
    return bg::CompiledGraph{wiring};
}

} // namespace

TEST_CASE("borrowed inputs sample values and deliver update, invalidation, and revalidation",
          "[evaluation][feature:borrowed-input][series:tss][validity][shape:nested]") {
    auto child_graph = make_borrowed_input_graph();
    BorrowedInputSink::compiled_graph = &child_graph;
    BorrowedInputSink::observations.clear();
    BorrowedInputSink::stop_value = 0;
    BorrowedInputSink::stop_time = {};
    BorrowedInputSink::stop_evaluation_time = {};
    BorrowedInputSink::stop_valid = false;
    BorrowedInputSink::stop_set_valid = false;
    BorrowedInputSink::stop_set_size = 0;
    BorrowedInputSink::stop_count = 0;
    BorrowedInputSink::check_nested_scope = true;
    BorrowedInputSink::nested_scope_rejected = false;
    BorrowedInputRunner::compiled_graph = &child_graph;
    BorrowedInputRunner::child = nullptr;
    BorrowedInputRunner::evaluator = nullptr;
    BorrowedInputRunner::use_different_runtime_type = false;
    BorrowedInputRunner::different_runtime_type_rejected = false;

    const auto runner = bg::node<BorrowedInputRunner>();
    auto wiring = bg::GraphWiring{};
    {
        auto context = bg::WiringContext{wiring};
        runner();
    }
    wiring.finalize();
    const auto graph = bg::CompiledGraph{wiring};
    auto global_state = bg::GlobalState{};
    auto runtime = bg::RuntimeGraph{graph, global_state};

    runtime.start(time(1));
    runtime.evaluate(time(1));
    runtime.schedule(0);
    runtime.evaluate(time(2));
    runtime.schedule(0);
    runtime.evaluate(time(3));
    runtime.stop();

    REQUIRE(BorrowedInputSink::observations.size() == 3);
    CHECK(BorrowedInputSink::observations[0].value == 11);
    CHECK(BorrowedInputSink::observations[0].time == time(1));
    CHECK(BorrowedInputSink::observations[0].evaluation_time == time(1));
    CHECK(BorrowedInputSink::observations[0].valid);
    CHECK(BorrowedInputSink::observations[0].modified);
    CHECK(BorrowedInputSink::observations[0].set_valid);
    CHECK(BorrowedInputSink::observations[0].set_size == 0);
    CHECK(BorrowedInputSink::observations[0].set_modified);
    CHECK(BorrowedInputSink::observations[1].value == 7);
    CHECK(BorrowedInputSink::observations[1].time == time(2));
    CHECK(BorrowedInputSink::observations[1].modified);
    CHECK(BorrowedInputSink::observations[2].value == 33);
    CHECK(BorrowedInputSink::observations[2].time == time(3));
    CHECK(BorrowedInputSink::observations[2].modified);
    CHECK(BorrowedInputSink::stop_count == 1);
    CHECK(BorrowedInputSink::stop_value == 33);
    CHECK(BorrowedInputSink::stop_time == time(3));
    CHECK(BorrowedInputSink::stop_evaluation_time == time(3));
    CHECK(BorrowedInputSink::stop_valid);
    CHECK(BorrowedInputSink::stop_set_valid);
    CHECK(BorrowedInputSink::stop_set_size == 0);
    CHECK(BorrowedInputSink::nested_scope_rejected);
}

TEST_CASE("borrowed direct inputs reject a different time-series runtime type",
          "[evaluation][feature:borrowed-input]") {
    auto child_graph = make_borrowed_input_graph();
    BorrowedInputRunner::compiled_graph = &child_graph;
    BorrowedInputRunner::child = nullptr;
    BorrowedInputRunner::evaluator = nullptr;
    BorrowedInputRunner::use_different_runtime_type = true;
    BorrowedInputRunner::different_runtime_type_rejected = false;

    const auto runner = bg::node<BorrowedInputRunner>();
    auto wiring = bg::GraphWiring{};
    {
        auto context = bg::WiringContext{wiring};
        runner();
    }
    wiring.finalize();
    const auto graph = bg::CompiledGraph{wiring};
    auto global_state = bg::GlobalState{};
    auto runtime = bg::RuntimeGraph{graph, global_state};

    runtime.start(time(1));

    CHECK(BorrowedInputRunner::different_runtime_type_rejected);
    runtime.stop();
}
