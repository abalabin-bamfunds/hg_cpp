#include <bg/evaluation_context.hpp>
#include <bg/graph/runtime_graph_input.hpp>
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
#include <catch2/matchers/catch_matchers.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace {

[[nodiscard]] bg::EvaluationTime time(bg::i64 microseconds) {
    return bg::MIN_DT + std::chrono::microseconds{microseconds};
}

struct BorrowedInputSink {
    bg::In<bg::TS<bg::i64>> input;
    bg::In<bg::TSS<bg::i64>> set_input;

    void start(bg::EvaluationContext *context) const {
        start_inputs_explicit = context->runtime_graph_inputs() == expected_inputs;
    }

    void stop(bg::EvaluationContext *context) const noexcept {
        stop_inputs_explicit = context->runtime_graph_inputs() == expected_inputs;
        stop_value = *input;
        stop_time = input.last_updated();
        stop_evaluation_time = context->evaluation_time();
        stop_valid = input.valid();
        stop_set_valid = set_input.valid();
        stop_set_size = set_input.size();
        ++stop_count;
    }

    void evaluate(bg::EvaluationContext *context) const {
        evaluate_inputs_explicit = context->runtime_graph_inputs() == expected_inputs;

        observations.push_back({.value = *input,
                                .time = input.last_updated(),
                                .evaluation_time = context->evaluation_time(),
                                .valid = input.valid(),
                                .modified = input.modified(),
                                .set_valid = set_input.valid(),
                                .set_size = set_input.size(),
                                .set_modified = set_input.modified()});

        if (check_different_graph_context) {
            check_different_graph_context = false;
            auto &parent = context->runtime_graph();
            auto evaluator = bg::RuntimeGraphEvaluator{};
            evaluator.bind_owner(parent, context->node_index());
            auto nested = bg::RuntimeGraph{*context_probe_graph, parent.allocation(), evaluator,
                                           context->node_index()};
            nested.start(*context);
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

    inline static const bg::CompiledGraph *context_probe_graph{};
    inline static const bg::RuntimeGraphInput *expected_inputs{};
    inline static bool start_inputs_explicit{};
    inline static bool evaluate_inputs_explicit{};
    inline static bool stop_inputs_explicit{};
    inline static bool check_different_graph_context{};
    inline static std::vector<Observation> observations;
    inline static bg::i64 stop_value{};
    inline static bg::EvaluationTime stop_time{};
    inline static bg::EvaluationTime stop_evaluation_time{};
    inline static bool stop_valid{};
    inline static bool stop_set_valid{};
    inline static std::size_t stop_set_size{};
    inline static std::size_t stop_count{};
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
        const auto &runtime_type = bg::resolve_time_series_runtime_type(bg::TS<bg::i64>::type());
        auto set_value = bg::details::time_series_set_storage{};
        const auto &set_runtime_type = bg::resolve_time_series_runtime_type(bg::TSS<bg::i64>::type());
        const auto inputs = std::array{bg::RuntimeGraphInput{&state, &value, &runtime_type,
                                                             bg::RuntimeGraphInputChange::update},
                                       bg::RuntimeGraphInput{&state, &set_value, &set_runtime_type,
                                                             bg::RuntimeGraphInputChange::update}};
        BorrowedInputSink::expected_inputs = inputs.data();
        child->start(*context, inputs.data());
    }

    void evaluate(bg::EvaluationContext *context) const {
        auto state = bg::TimeSeriesState{};
        auto value = bg::i64{};
        auto change = bg::RuntimeGraphInputChange::unchanged;
        const auto evaluation_time = context->evaluation_time();

        if (evaluation_time == time(2)) {
            state.publish(time(1));
            state.invalidate(evaluation_time);
            value = 11;
            change = bg::RuntimeGraphInputChange::invalidation;
        } else if (evaluation_time == time(3)) {
            state.publish(evaluation_time);
            value = 33;
            change = bg::RuntimeGraphInputChange::revalidation;
        } else if (evaluation_time == time(4)) {
            state.publish(time(3));
            value = 44;
        } else {
            state.publish(evaluation_time);
            value = 11;
        }

        const auto &runtime_type = bg::resolve_time_series_runtime_type(bg::TS<bg::i64>::type());
        auto set_state = bg::TimeSeriesState{evaluation_time};
        auto set_value = bg::details::time_series_set_storage{};
        const auto &set_runtime_type = bg::resolve_time_series_runtime_type(bg::TSS<bg::i64>::type());
        const auto inputs = std::array{bg::RuntimeGraphInput{&state, &value, &runtime_type, change},
                                       bg::RuntimeGraphInput{&set_state, &set_value, &set_runtime_type,
                                                             bg::RuntimeGraphInputChange::update}};
        BorrowedInputSink::expected_inputs = inputs.data();
        child->evaluate(*context, inputs.data());
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
        const auto inputs = std::array{bg::RuntimeGraphInput{&state, &value, &runtime_type,
                                                             bg::RuntimeGraphInputChange::unchanged},
                                       bg::RuntimeGraphInput{&state, &set_value, &set_runtime_type,
                                                             bg::RuntimeGraphInputChange::unchanged}};
        BorrowedInputSink::expected_inputs = inputs.data();
        child->stop(*context, inputs.data());
        child->finalize(*context);
        context->runtime_graph().allocation().delete_object(child);
        child = nullptr;
        context->runtime_graph().allocation().delete_object(evaluator);
        evaluator = nullptr;
    }

    inline static const bg::CompiledGraph *compiled_graph{};
    inline static bg::RuntimeGraphEvaluator *evaluator{};
    inline static bg::RuntimeGraph *child{};
};

struct ContextInputProbe {
    void start(bg::EvaluationContext *context) const {
        different_graph_inputs_cleared = context->runtime_graph_inputs() == nullptr;
    }

    inline static bool different_graph_inputs_cleared{};
};

struct BorrowedInputCycleCallback {
    BorrowedInputCycleCallback()
        : signature_{bg::util::symbol{"borrowed_input_cycle_callback"},
                     {{bg::util::symbol{"input"}, &bg::TS<bg::i64>::type()}}} {}

    const bg::Signature &signature() const { return signature_; }

    static void start(bg::NodeInvocation<> invocation) {
        invocation.context().add_cycle_callback(nullptr, &run_callback);
    }

    static void evaluate(bg::NodeInvocation<>) {}

    static void run_callback(void *, bg::EvaluationContext &context) noexcept {
        observed_inputs = context.runtime_graph_inputs();
        if (observed_inputs != nullptr) {
            observed_value = *static_cast<const bg::i64 *>(observed_inputs[0].value);
        }
        ++callback_count;
    }

    static constexpr bool uses_cycle_callbacks = true;

    inline static const bg::RuntimeGraphInput *observed_inputs{};
    inline static bg::i64 observed_value{};
    inline static std::size_t callback_count{};

  private:
    bg::Signature signature_;
};

struct CrossGraphCallbackMarker {
    CrossGraphCallbackMarker() : signature_{bg::util::symbol{"cross_graph_callback_marker"}, {}} {}

    const bg::Signature &signature() const { return signature_; }

    static void start(bg::NodeInvocation<> invocation) {
        invocation.context().add_cycle_callback(nullptr, &run_callback);
    }

    static void evaluate(bg::NodeInvocation<>) {}

    static void run_callback(void *, bg::EvaluationContext &context) noexcept {
        callback_inputs_cleared = context.runtime_graph_inputs() == nullptr;
        ++callback_count;
    }

    static constexpr bool uses_cycle_callbacks = true;

    inline static bool callback_inputs_cleared{};
    inline static std::size_t callback_count{};

  private:
    bg::Signature signature_;
};

struct CrossGraphCallbackRunnerState {
    static constexpr bool supports_byte_copy_initialization = true;

    bg::RuntimeGraphEvaluator evaluator;
    bg::RuntimeGraph *graph{};
};

struct CrossGraphCallbackRunner {
    using compiled_data_type = void;
    using state_type = CrossGraphCallbackRunnerState;

    CrossGraphCallbackRunner()
        : signature_{bg::util::symbol{"cross_graph_callback_runner"},
                     {{bg::util::symbol{"input"}, &bg::TS<bg::i64>::type()}}} {}

    const bg::Signature &signature() const { return signature_; }

    static void start(bg::NodeInvocation<compiled_data_type, state_type> invocation) {
        auto &parent = invocation.context().runtime_graph();
        auto &state = invocation.state();
        state.evaluator.bind_owner(parent, invocation.context().node_index());
        state.graph = parent.allocation().new_object<bg::RuntimeGraph>(
            *child_graph, parent.allocation(), state.evaluator, invocation.context().node_index());
        state.graph->start(invocation.context());
    }

    static void evaluate(bg::NodeInvocation<compiled_data_type, state_type> invocation) {
        (void)invocation.input<bg::In<bg::TS<bg::i64>>>(0);
        invocation.state().graph->evaluate(invocation.context());
    }

    static void stop(bg::NodeInvocation<compiled_data_type, state_type> invocation) noexcept {
        auto &state = invocation.state();
        if (state.graph == nullptr) {
            return;
        }
        state.graph->stop(invocation.context());
        state.graph->finalize(invocation.context());
        invocation.context().runtime_graph().allocation().delete_object(state.graph);
        state.graph = nullptr;
    }

    static constexpr bool uses_cycle_callbacks = true;

    inline static const bg::CompiledGraph *child_graph{};

  private:
    bg::Signature signature_;
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

bg::CompiledGraph make_context_probe_graph() {
    auto wiring = bg::GraphWiring{};
    const auto probe = bg::node<ContextInputProbe>();
    {
        auto context = bg::WiringContext{wiring};
        probe();
    }
    wiring.finalize();
    return bg::CompiledGraph{wiring};
}

bg::CompiledGraph make_cross_graph_callback_graph() {
    auto wiring = bg::GraphWiring{};
    const auto callback = bg::node(CrossGraphCallbackMarker{});
    {
        auto context = bg::WiringContext{wiring};
        (void)callback();
    }
    wiring.finalize();
    return bg::CompiledGraph{wiring};
}

} // namespace

TEST_CASE("same-graph cycle callbacks receive and read explicit borrowed inputs",
          "[feature:borrowed-input][feature:lifecycle][transition:lifecycle][shape:single-node]") {
    auto wiring = bg::GraphWiring{};
    const auto input = wiring.borrowed_input(bg::TS<bg::i64>::type());
    const auto callback = bg::node(BorrowedInputCycleCallback{});
    {
        auto context = bg::WiringContext{wiring};
        (void)callback(input);
    }
    wiring.finalize();
    const auto graph = bg::CompiledGraph{wiring};
    auto global_state = bg::GlobalState{};
    auto runtime = bg::RuntimeGraph{graph, global_state};
    auto state = bg::TimeSeriesState{time(1)};
    auto value = bg::i64{71};
    const auto &runtime_type = bg::resolve_time_series_runtime_type(bg::TS<bg::i64>::type());
    const auto inputs = std::array{bg::RuntimeGraphInput{&state, &value, &runtime_type,
                                                         bg::RuntimeGraphInputChange::update}};
    BorrowedInputCycleCallback::observed_inputs = nullptr;
    BorrowedInputCycleCallback::observed_value = 0;
    BorrowedInputCycleCallback::callback_count = 0;

    runtime.start(time(1), global_state, inputs.data());
    runtime.evaluate(time(1), global_state, inputs.data());
    runtime.stop(global_state, inputs.data());

    CHECK(BorrowedInputCycleCallback::callback_count == 1);
    CHECK(BorrowedInputCycleCallback::observed_inputs == inputs.data());
    CHECK(BorrowedInputCycleCallback::observed_value == 71);
}

TEST_CASE("cycle callbacks owned by another graph do not inherit borrowed inputs",
          "[feature:borrowed-input][feature:lifecycle][transition:lifecycle][shape:nested]") {
    auto child_graph = make_cross_graph_callback_graph();
    CrossGraphCallbackRunner::child_graph = &child_graph;
    CrossGraphCallbackMarker::callback_inputs_cleared = false;
    CrossGraphCallbackMarker::callback_count = 0;

    auto wiring = bg::GraphWiring{};
    const auto input = wiring.borrowed_input(bg::TS<bg::i64>::type());
    const auto runner = bg::node(CrossGraphCallbackRunner{});
    {
        auto context = bg::WiringContext{wiring};
        (void)runner(input);
    }
    wiring.finalize();
    const auto graph = bg::CompiledGraph{wiring};
    auto global_state = bg::GlobalState{};
    auto runtime = bg::RuntimeGraph{graph, global_state};
    auto state = bg::TimeSeriesState{time(1)};
    auto value = bg::i64{88};
    const auto &runtime_type = bg::resolve_time_series_runtime_type(bg::TS<bg::i64>::type());
    const auto inputs = std::array{bg::RuntimeGraphInput{&state, &value, &runtime_type,
                                                         bg::RuntimeGraphInputChange::update}};

    runtime.start(time(1), global_state, inputs.data());
    runtime.evaluate(time(1), global_state, inputs.data());
    runtime.stop(global_state, inputs.data());

    CHECK(CrossGraphCallbackMarker::callback_count == 1);
    CHECK(CrossGraphCallbackMarker::callback_inputs_cleared);
}

TEST_CASE("borrowed inputs sample values and deliver update, invalidation, and revalidation",
          "[evaluation][feature:borrowed-input][series:tss][validity][shape:nested]") {
    auto child_graph = make_borrowed_input_graph();
    auto context_probe_graph = make_context_probe_graph();
    BorrowedInputSink::context_probe_graph = &context_probe_graph;
    BorrowedInputSink::expected_inputs = nullptr;
    BorrowedInputSink::start_inputs_explicit = false;
    BorrowedInputSink::evaluate_inputs_explicit = false;
    BorrowedInputSink::stop_inputs_explicit = false;
    BorrowedInputSink::check_different_graph_context = true;
    ContextInputProbe::different_graph_inputs_cleared = false;
    BorrowedInputSink::observations.clear();
    BorrowedInputSink::stop_value = 0;
    BorrowedInputSink::stop_time = {};
    BorrowedInputSink::stop_evaluation_time = {};
    BorrowedInputSink::stop_valid = false;
    BorrowedInputSink::stop_set_valid = false;
    BorrowedInputSink::stop_set_size = 0;
    BorrowedInputSink::stop_count = 0;
    BorrowedInputRunner::compiled_graph = &child_graph;
    BorrowedInputRunner::child = nullptr;
    BorrowedInputRunner::evaluator = nullptr;

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
    runtime.schedule(0);
    runtime.evaluate(time(4));
    runtime.stop();

    REQUIRE(BorrowedInputSink::observations.size() == 4);
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
    CHECK(BorrowedInputSink::observations[3].value == 44);
    CHECK(BorrowedInputSink::observations[3].time == time(3));
    CHECK(BorrowedInputSink::observations[3].evaluation_time == time(4));
    CHECK_FALSE(BorrowedInputSink::observations[3].modified);
    CHECK(BorrowedInputSink::observations[3].set_modified);
    CHECK(BorrowedInputSink::stop_count == 1);
    CHECK(BorrowedInputSink::stop_value == 33);
    CHECK(BorrowedInputSink::stop_time == time(3));
    CHECK(BorrowedInputSink::stop_evaluation_time == time(4));
    CHECK(BorrowedInputSink::stop_valid);
    CHECK(BorrowedInputSink::stop_set_valid);
    CHECK(BorrowedInputSink::stop_set_size == 0);
    CHECK(BorrowedInputSink::start_inputs_explicit);
    CHECK(BorrowedInputSink::evaluate_inputs_explicit);
    CHECK(BorrowedInputSink::stop_inputs_explicit);
    CHECK(ContextInputProbe::different_graph_inputs_cleared);
}

TEST_CASE("borrowed storage includes scheduling spill words and validates its layout",
          "[evaluation][feature:borrowed-input][feature:graph-compilation][storage:fixed]") {
    struct BorrowedInputNoop {
        bg::In<bg::TS<bg::i64>> input;

        void evaluate() const {}
    };

    struct PendingBorrowedInputGraph {
        bool evaluate() const { return true; }
    };

    auto wiring = bg::GraphWiring{};
    const auto consumer = bg::node<BorrowedInputNoop>();
    {
        auto context = bg::WiringContext{wiring};
        for (auto index = std::size_t{}; index < bg::CompiledGraph::schedule_word_bits + 1; ++index) {
            (void)consumer(wiring.borrowed_input(bg::TS<bg::i64>::type()));
        }
    }
    wiring.finalize();
    const auto graph = bg::CompiledGraph{wiring};

    REQUIRE(graph.supports_borrowed_evaluation());
    REQUIRE(graph.schedule_word_count() == 2);
    const auto storage_size = bg::RuntimeGraph::borrowed_storage_size(graph);
    const auto alignment = bg::RuntimeGraph::borrowed_storage_alignment(graph);
    REQUIRE(alignment > 1);
    const auto spill_offset = (graph.runtime_storage_size() + alignof(bg::CompiledGraph::SchedulingInfo) - 1) &
                              ~(alignof(bg::CompiledGraph::SchedulingInfo) - 1);
    CHECK(storage_size == spill_offset + sizeof(bg::CompiledGraph::SchedulingInfo));
    CHECK(alignment >= alignof(bg::CompiledGraph::SchedulingInfo));

    const auto word_count = (storage_size + alignment + sizeof(std::max_align_t) - 1) / sizeof(std::max_align_t);
    auto allocation = std::vector<std::max_align_t>(word_count);
    auto *storage_data = reinterpret_cast<std::byte *>(allocation.data());
    auto storage = std::span{storage_data, storage_size};
    CHECK_THROWS_WITH(bg::RuntimeGraph::initialize_borrowed_storage(graph, storage.first(storage_size - 1)),
                      "borrowed runtime storage is too small");

    auto misaligned_offset = std::size_t{};
    while (reinterpret_cast<std::uintptr_t>(storage_data + misaligned_offset) % alignment == 0) {
        ++misaligned_offset;
    }
    CHECK_THROWS_WITH(bg::RuntimeGraph::initialize_borrowed_storage(
                          graph, std::span{storage_data + misaligned_offset, storage_size}),
                      "borrowed runtime storage is misaligned");

    bg::RuntimeGraph::initialize_borrowed_storage(graph, storage);
    auto global_state = bg::GlobalState{};
    auto runtime = bg::RuntimeGraph{graph, bg::allocator{}, storage, global_state};
    CHECK(runtime.input_validity() == 0);

    auto pending_wiring = bg::GraphWiring{};
    const auto pending = bg::node<PendingBorrowedInputGraph>();
    {
        auto context = bg::WiringContext{pending_wiring};
        (void)pending();
    }
    pending_wiring.finalize();
    const auto pending_graph = bg::CompiledGraph{pending_wiring};
    CHECK_FALSE(pending_graph.supports_borrowed_evaluation());
    CHECK_THROWS_WITH(bg::RuntimeGraph::initialize_borrowed_storage(pending_graph, storage),
                      "the compiled graph does not support borrowed evaluation");
}
