#include <hg/evaluation_context.hpp>
#include <hg/graph/runtime_graph_input.hpp>
#include <hg/graph/compiled_graph.hpp>
#include <hg/graph/graph_wiring.hpp>
#include <hg/graph/runtime_graph.hpp>
#include <hg/graph/runtime_graph_evaluator.hpp>
#include <hg/graph/wiring_context.hpp>
#include <hg/node.hpp>
#include <hg/operators.hpp>
#include <hg/time_series/time_series_runtime_type.hpp>
#include <hg/time_series/tss/tss_input.hpp>
#include <hg/time_series/tss/tss_storage.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace {

[[nodiscard]] hg::EvaluationTime time(hg::i64 microseconds) {
    return hg::MIN_DT + std::chrono::microseconds{microseconds};
}

struct BorrowedInputSink {
    hg::In<hg::TS<hg::i64>> input;
    hg::In<hg::TSS<hg::i64>> set_input;

    void start(hg::EvaluationContext *context) const {
        start_inputs_explicit = context->runtime_graph_inputs() == expected_inputs;
    }

    void stop(hg::EvaluationContext *context) const noexcept {
        stop_inputs_explicit = context->runtime_graph_inputs() == expected_inputs;
        stop_value = *input;
        stop_time = input.last_updated();
        stop_evaluation_time = context->evaluation_time();
        stop_valid = input.valid();
        stop_set_valid = set_input.valid();
        stop_set_size = set_input.size();
        ++stop_count;
    }

    void evaluate(hg::EvaluationContext *context) const {
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
            auto evaluator = hg::RuntimeGraphEvaluator{};
            evaluator.bind_owner(parent, context->node_index());
            auto nested = hg::RuntimeGraph{*context_probe_graph, parent.allocation(), evaluator,
                                           context->node_index()};
            nested.start(*context);
            nested.finalize(*context);
        }
    }

    struct Observation {
        hg::i64 value;
        hg::EvaluationTime time;
        hg::EvaluationTime evaluation_time;
        bool valid;
        bool modified;
        bool set_valid;
        std::size_t set_size;
        bool set_modified;
    };

    inline static const hg::CompiledGraph *context_probe_graph{};
    inline static const hg::RuntimeGraphInput *expected_inputs{};
    inline static bool start_inputs_explicit{};
    inline static bool evaluate_inputs_explicit{};
    inline static bool stop_inputs_explicit{};
    inline static bool check_different_graph_context{};
    inline static std::vector<Observation> observations;
    inline static hg::i64 stop_value{};
    inline static hg::EvaluationTime stop_time{};
    inline static hg::EvaluationTime stop_evaluation_time{};
    inline static bool stop_valid{};
    inline static bool stop_set_valid{};
    inline static std::size_t stop_set_size{};
    inline static std::size_t stop_count{};
};

struct BorrowedInputRunner {
    void start(hg::EvaluationContext *context) const {
        auto &parent = context->runtime_graph();
        evaluator = parent.allocation().new_object<hg::RuntimeGraphEvaluator>();
        evaluator->bind_owner(parent, context->node_index());
        child = parent.allocation().new_object<hg::RuntimeGraph>(*compiled_graph, parent.allocation(), *evaluator,
                                                                 context->node_index());

        auto state = hg::TimeSeriesState{context->evaluation_time()};
        auto value = hg::i64{11};
        const auto &runtime_type = hg::resolve_time_series_runtime_type(hg::TS<hg::i64>::type());
        auto set_value = hg::details::time_series_set_storage{};
        const auto &set_runtime_type = hg::resolve_time_series_runtime_type(hg::TSS<hg::i64>::type());
        const auto inputs = std::array{hg::RuntimeGraphInput{&state, &value, &runtime_type,
                                                             hg::RuntimeGraphInputChange::update},
                                       hg::RuntimeGraphInput{&state, &set_value, &set_runtime_type,
                                                             hg::RuntimeGraphInputChange::update}};
        BorrowedInputSink::expected_inputs = inputs.data();
        child->start(*context, inputs.data());
    }

    void evaluate(hg::EvaluationContext *context) const {
        auto state = hg::TimeSeriesState{};
        auto value = hg::i64{};
        auto change = hg::RuntimeGraphInputChange::unchanged;
        const auto evaluation_time = context->evaluation_time();

        if (evaluation_time == time(2)) {
            state.publish(time(1));
            state.invalidate(evaluation_time);
            value = 11;
            change = hg::RuntimeGraphInputChange::invalidation;
        } else if (evaluation_time == time(3)) {
            state.publish(evaluation_time);
            value = 33;
            change = hg::RuntimeGraphInputChange::revalidation;
        } else if (evaluation_time == time(4)) {
            state.publish(time(3));
            value = 44;
        } else {
            state.publish(evaluation_time);
            value = 11;
        }

        const auto &runtime_type = hg::resolve_time_series_runtime_type(hg::TS<hg::i64>::type());
        auto set_state = hg::TimeSeriesState{evaluation_time};
        auto set_value = hg::details::time_series_set_storage{};
        const auto &set_runtime_type = hg::resolve_time_series_runtime_type(hg::TSS<hg::i64>::type());
        const auto inputs = std::array{hg::RuntimeGraphInput{&state, &value, &runtime_type, change},
                                       hg::RuntimeGraphInput{&set_state, &set_value, &set_runtime_type,
                                                             hg::RuntimeGraphInputChange::update}};
        BorrowedInputSink::expected_inputs = inputs.data();
        child->evaluate(*context, inputs.data());
    }

    void stop(hg::EvaluationContext *context) const noexcept {
        if (child == nullptr) {
            return;
        }

        auto state = hg::TimeSeriesState{context->evaluation_time()};
        auto value = hg::i64{33};
        const auto &runtime_type = hg::resolve_time_series_runtime_type(hg::TS<hg::i64>::type());
        auto set_value = hg::details::time_series_set_storage{};
        const auto &set_runtime_type = hg::resolve_time_series_runtime_type(hg::TSS<hg::i64>::type());
        const auto inputs = std::array{hg::RuntimeGraphInput{&state, &value, &runtime_type,
                                                             hg::RuntimeGraphInputChange::unchanged},
                                       hg::RuntimeGraphInput{&state, &set_value, &set_runtime_type,
                                                             hg::RuntimeGraphInputChange::unchanged}};
        BorrowedInputSink::expected_inputs = inputs.data();
        child->stop(*context, inputs.data());
        child->finalize(*context);
        context->runtime_graph().allocation().delete_object(child);
        child = nullptr;
        context->runtime_graph().allocation().delete_object(evaluator);
        evaluator = nullptr;
    }

    inline static const hg::CompiledGraph *compiled_graph{};
    inline static hg::RuntimeGraphEvaluator *evaluator{};
    inline static hg::RuntimeGraph *child{};
};

struct ContextInputProbe {
    void start(hg::EvaluationContext *context) const {
        different_graph_inputs_cleared = context->runtime_graph_inputs() == nullptr;
    }

    inline static bool different_graph_inputs_cleared{};
};

struct BorrowedInputCycleCallback {
    BorrowedInputCycleCallback()
        : signature_{hg::util::symbol{"borrowed_input_cycle_callback"},
                     {{hg::util::symbol{"input"}, &hg::TS<hg::i64>::type()}}} {}

    const hg::Signature &signature() const { return signature_; }

    static void start(hg::NodeInvocation<> invocation) {
        invocation.context().add_cycle_callback(nullptr, &run_callback);
    }

    static void evaluate(hg::NodeInvocation<>) {}

    static void run_callback(void *, hg::EvaluationContext &context) noexcept {
        observed_inputs = context.runtime_graph_inputs();
        if (observed_inputs != nullptr) {
            observed_value = *static_cast<const hg::i64 *>(observed_inputs[0].value);
        }
        ++callback_count;
    }

    static constexpr bool uses_cycle_callbacks = true;

    inline static const hg::RuntimeGraphInput *observed_inputs{};
    inline static hg::i64 observed_value{};
    inline static std::size_t callback_count{};

  private:
    hg::Signature signature_;
};

struct CrossGraphCallbackMarker {
    CrossGraphCallbackMarker() : signature_{hg::util::symbol{"cross_graph_callback_marker"}, {}} {}

    const hg::Signature &signature() const { return signature_; }

    static void start(hg::NodeInvocation<> invocation) {
        invocation.context().add_cycle_callback(nullptr, &run_callback);
    }

    static void evaluate(hg::NodeInvocation<>) {}

    static void run_callback(void *, hg::EvaluationContext &context) noexcept {
        callback_inputs_cleared = context.runtime_graph_inputs() == nullptr;
        ++callback_count;
    }

    static constexpr bool uses_cycle_callbacks = true;

    inline static bool callback_inputs_cleared{};
    inline static std::size_t callback_count{};

  private:
    hg::Signature signature_;
};

struct CrossGraphCallbackRunnerState {
    static constexpr bool supports_byte_copy_initialization = true;

    hg::RuntimeGraphEvaluator evaluator;
    hg::RuntimeGraph *graph{};
};

struct CrossGraphCallbackRunner {
    using compiled_data_type = void;
    using state_type = CrossGraphCallbackRunnerState;

    CrossGraphCallbackRunner()
        : signature_{hg::util::symbol{"cross_graph_callback_runner"},
                     {{hg::util::symbol{"input"}, &hg::TS<hg::i64>::type()}}} {}

    const hg::Signature &signature() const { return signature_; }

    static void start(hg::NodeInvocation<compiled_data_type, state_type> invocation) {
        auto &parent = invocation.context().runtime_graph();
        auto &state = invocation.state();
        state.evaluator.bind_owner(parent, invocation.context().node_index());
        state.graph = parent.allocation().new_object<hg::RuntimeGraph>(
            *child_graph, parent.allocation(), state.evaluator, invocation.context().node_index());
        state.graph->start(invocation.context());
    }

    static void evaluate(hg::NodeInvocation<compiled_data_type, state_type> invocation) {
        (void)invocation.input<hg::In<hg::TS<hg::i64>>>(0);
        invocation.state().graph->evaluate(invocation.context());
    }

    static void stop(hg::NodeInvocation<compiled_data_type, state_type> invocation) noexcept {
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

    inline static const hg::CompiledGraph *child_graph{};

  private:
    hg::Signature signature_;
};

hg::CompiledGraph make_borrowed_input_graph() {
    auto wiring = hg::GraphWiring{};
    const auto input = wiring.borrowed_input(hg::TS<hg::i64>::type());
    const auto set_input = wiring.borrowed_input(hg::TSS<hg::i64>::type());

    const auto sink = hg::node<BorrowedInputSink>();
    {
        auto context = hg::WiringContext{wiring};
        sink(hg::operators::default_(input, hg::i64{7}), set_input);
    }
    wiring.finalize();
    return hg::CompiledGraph{wiring};
}

hg::CompiledGraph make_context_probe_graph() {
    auto wiring = hg::GraphWiring{};

    const auto probe = hg::node<ContextInputProbe>();
    {
        auto context = hg::WiringContext{wiring};
        probe();
    }
    wiring.finalize();
    return hg::CompiledGraph{wiring};
}

hg::CompiledGraph make_cross_graph_callback_graph() {
    auto wiring = hg::GraphWiring{};

    const auto callback = hg::node(CrossGraphCallbackMarker{});
    {
        auto context = hg::WiringContext{wiring};
        (void)callback();
    }
    wiring.finalize();
    return hg::CompiledGraph{wiring};
}

} // namespace

TEST_CASE("same-graph cycle callbacks receive and read explicit borrowed inputs",
          "[feature:borrowed-input][feature:lifecycle][transition:lifecycle][shape:single-node]") {
    auto wiring = hg::GraphWiring{};
    const auto input = wiring.borrowed_input(hg::TS<hg::i64>::type());

    const auto callback = hg::node(BorrowedInputCycleCallback{});
    {
        auto context = hg::WiringContext{wiring};
        (void)callback(input);
    }
    wiring.finalize();
    const auto graph = hg::CompiledGraph{wiring};
    auto global_state = hg::GlobalState{};
    auto runtime = hg::RuntimeGraph{graph, global_state};
    auto state = hg::TimeSeriesState{time(1)};
    auto value = hg::i64{71};
    const auto &runtime_type = hg::resolve_time_series_runtime_type(hg::TS<hg::i64>::type());
    const auto inputs = std::array{hg::RuntimeGraphInput{&state, &value, &runtime_type,
                                                         hg::RuntimeGraphInputChange::update}};
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

    auto wiring = hg::GraphWiring{};
    const auto input = wiring.borrowed_input(hg::TS<hg::i64>::type());

    const auto runner = hg::node(CrossGraphCallbackRunner{});
    {
        auto context = hg::WiringContext{wiring};
        (void)runner(input);
    }
    wiring.finalize();
    const auto graph = hg::CompiledGraph{wiring};
    auto global_state = hg::GlobalState{};
    auto runtime = hg::RuntimeGraph{graph, global_state};
    auto state = hg::TimeSeriesState{time(1)};
    auto value = hg::i64{88};
    const auto &runtime_type = hg::resolve_time_series_runtime_type(hg::TS<hg::i64>::type());
    const auto inputs = std::array{hg::RuntimeGraphInput{&state, &value, &runtime_type,
                                                         hg::RuntimeGraphInputChange::update}};

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

    const auto runner = hg::node<BorrowedInputRunner>();
    auto wiring = hg::GraphWiring{};
    {
        auto context = hg::WiringContext{wiring};
        runner();
    }
    wiring.finalize();
    const auto graph = hg::CompiledGraph{wiring};
    auto global_state = hg::GlobalState{};
    auto runtime = hg::RuntimeGraph{graph, global_state};

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
        hg::In<hg::TS<hg::i64>> input;

        void evaluate() const {}
    };

    struct PendingBorrowedInputGraph {
        bool evaluate() const { return true; }
    };

    auto wiring = hg::GraphWiring{};

    const auto consumer = hg::node<BorrowedInputNoop>();
    {
        auto context = hg::WiringContext{wiring};
        for (auto index = std::size_t{}; index < hg::CompiledGraph::schedule_word_bits + 1; ++index) {
            (void)consumer(wiring.borrowed_input(hg::TS<hg::i64>::type()));
        }
    }
    wiring.finalize();
    const auto graph = hg::CompiledGraph{wiring};

    REQUIRE(graph.supports_borrowed_evaluation());
    REQUIRE(graph.schedule_word_count() == 2);
    const auto storage_size = hg::RuntimeGraph::borrowed_storage_size(graph);
    const auto alignment = hg::RuntimeGraph::borrowed_storage_alignment(graph);
    REQUIRE(alignment > 1);
    const auto spill_offset = (graph.runtime_storage_size() + alignof(hg::CompiledGraph::SchedulingInfo) - 1) &
                              ~(alignof(hg::CompiledGraph::SchedulingInfo) - 1);
    CHECK(storage_size == spill_offset + sizeof(hg::CompiledGraph::SchedulingInfo));
    CHECK(alignment >= alignof(hg::CompiledGraph::SchedulingInfo));

    const auto word_count = (storage_size + alignment + sizeof(std::max_align_t) - 1) / sizeof(std::max_align_t);
    auto allocation = std::vector<std::max_align_t>(word_count);
    auto *storage_data = reinterpret_cast<std::byte *>(allocation.data());
    auto storage = std::span{storage_data, storage_size};
    CHECK_THROWS_WITH(hg::RuntimeGraph::initialize_borrowed_storage(graph, storage.first(storage_size - 1)),
                      "borrowed runtime storage is too small");

    auto misaligned_offset = std::size_t{};
    while (reinterpret_cast<std::uintptr_t>(storage_data + misaligned_offset) % alignment == 0) {
        ++misaligned_offset;
    }
    CHECK_THROWS_WITH(hg::RuntimeGraph::initialize_borrowed_storage(
                          graph, std::span{storage_data + misaligned_offset, storage_size}),
                      "borrowed runtime storage is misaligned");

    hg::RuntimeGraph::initialize_borrowed_storage(graph, storage);
    auto global_state = hg::GlobalState{};
    auto runtime = hg::RuntimeGraph{graph, hg::allocator{}, storage, global_state};
    CHECK(runtime.input_validity() == 0);

    auto pending_wiring = hg::GraphWiring{};

    const auto pending = hg::node<PendingBorrowedInputGraph>();
    {
        auto context = hg::WiringContext{pending_wiring};
        (void)pending();
    }
    pending_wiring.finalize();
    const auto pending_graph = hg::CompiledGraph{pending_wiring};
    CHECK_FALSE(pending_graph.supports_borrowed_evaluation());
    CHECK_THROWS_WITH(hg::RuntimeGraph::initialize_borrowed_storage(pending_graph, storage),
                      "the compiled graph does not support borrowed evaluation");
}
