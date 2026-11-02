#include <hg/graph.hpp>
#include <hg/details/evaluation_scratch.hpp>
#include <hg/graph/compiled_graph.hpp>
#include <hg/graph/details/nested.hpp>
#include <hg/graph/graph_wiring.hpp>
#include <hg/graph/nested_graph.hpp>
#include <hg/graph/runtime_graph.hpp>
#include <hg/graph/runtime_graph_evaluator.hpp>
#include <hg/graph/wiring_context.hpp>
#include <hg/nested.hpp>
#include <hg/node.hpp>
#include <hg/node/node_template.hpp>
#include <hg/switch.hpp>
#include <hg/run_graph.hpp>
#include <hg/time_series/tsb/tsb.hpp>
#include <hg/time_series/tsd/tsd_type.hpp>
#include <hg/time_series/tsl/tsl.hpp>
#include <hg/type/time_series_reference_type.hpp>
#include "../utils/counting_memory_resource.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <stdexcept>

using namespace hg;

namespace {

using namespace hg::literals;

TEST_CASE("nested graph commits omitted scalar defaults before creating boundaries",
          "[wiring][nested][feature:nested-graph][feature:wiring-defaults][definition:function][series:value]") {
    const auto combine = hg::node([](hg::In<hg::TS<hg::i64>> x, hg::In<hg::TS<hg::i64>> n,
                                     hg::Out<hg::TS<hg::i64>> output) {
        output.set(*x + *n * 10);
    });
    const auto inner = hg::graph(
        {"x"_n, "n"_n = hg::i64{2}},
        [=](hg::wp<hg::TS<hg::i64>> x, hg::wp<hg::TS<hg::i64>> n) { return combine(x, n); });
    const auto outer = [=](hg::wp<hg::TS<hg::i64>> x, hg::wp<hg::TS<bool>> run) {
        return hg::nested(inner, run, x);
    };

    CHECK(hg::run_graph(outer, std::array{3}, std::array{true}).equals({23}));
}

struct LifecycleIncrement {
    In<TS<i64>> input;
    Out<TS<i64>> output;

    void start() const noexcept { ++starts; }
    void evaluate() const { output.set(*input + 1); }
    void stop() const noexcept { ++stops; }

    inline static int starts{};
    inline static int stops{};
};

TEST_CASE("a nested graph can be stopped and started repeatedly",
          "[evaluation][nested][lifecycle][feature:nested-graph][shape:nested][transition:start]"
          "[transition:stop][transition:restart][scalar:boolean][scalar:integer][series:value]") {
    const auto increment = hg::node<LifecycleIncrement>();

    const auto inner = hg::graph("name"_n = "inner_increment",
                                 [&](hg::wp<TS<i64>> input) -> hg::wp<TS<i64>> { return increment(input); });
    LifecycleIncrement::starts = 0;
    LifecycleIncrement::stops = 0;

    const auto outer = [&](hg::wp<TS<i64>> input, hg::wp<TS<bool>> run) { return hg::nested(inner, run, input); };

    const auto values = {1, 2, 3, 4, 5};
    const auto running = {true, false, true, false, true};
    const auto actual = hg::run_graph(outer, values, running);
    CHECK(actual.equals({2, std::nullopt, 4, std::nullopt, 6}));
    CHECK(LifecycleIncrement::starts == 3);
    CHECK(LifecycleIncrement::stops == 3);
}

struct SwitchBranchLifetime {
    hg::In<hg::TS<hg::i64>> input;
    hg::State<int> marker;
    hg::Out<hg::TS<hg::i64>> output;
    int id;

    void start() const noexcept { *marker = id + 1; }
    void evaluate() const { output.set(*input); }

    void finalize() const noexcept {
        if (*marker != id + 1) {
            return;
        }
        if (finalization_count == finalizations.size()) {
            overflow = true;
            return;
        }
        finalizations[finalization_count++] = id;
    }

    inline static std::array<int, 2> finalizations{};
    inline static std::size_t finalization_count{};
    inline static bool overflow{};
};

TEST_CASE("switch finalization destroys the current branch before a retired branch",
          "[evaluation][feature:switch][feature:nested-graph][feature:lifecycle][feature:finalization]"
          "[transition:update][transition:stop][transition:restart]") {
    const auto lifetime = hg::node<SwitchBranchLifetime>();
    const auto first = hg::graph("name"_n = "switch_lifetime_first", [&](hg::wp<hg::TS<hg::i64>> input) {
        return lifetime(input, 0);
    });
    const auto second = hg::graph("name"_n = "switch_lifetime_second", [&](hg::wp<hg::TS<hg::i64>> input) {
        return lifetime(input, 1);
    });
    const auto tested = [&](hg::wp<hg::TS<hg::i64>> key, hg::wp<hg::TS<hg::i64>> value) {
        return hg::switch_(key, hg::cases{hg::case_(hg::i64{0}, first), hg::case_(hg::i64{1}, second)}, value)
            .as<hg::TS<hg::i64>>();
    };
    SwitchBranchLifetime::finalizations = {};
    SwitchBranchLifetime::finalization_count = 0;
    SwitchBranchLifetime::overflow = false;

    const auto actual = hg::run_graph(tested, {0, 1}, {10, 20});

    CHECK(actual.equals<hg::i64>({10, 20}));
    CHECK_FALSE(SwitchBranchLifetime::overflow);
    CHECK(SwitchBranchLifetime::finalization_count == 2);
    CHECK(SwitchBranchLifetime::finalizations == std::array{1, 0});
}

struct CycleCallbackOwner {
    static constexpr bool uses_cycle_callbacks = true;

    void evaluate() const noexcept {}
};

struct CycleCallbackRemovalDuringRun {
    static constexpr bool uses_cycle_callbacks = true;

    In<TS<i64>> input;
    State<bool> registered;
    bool call_on_stop;

    void evaluate(EvaluationContext *context) const {
        if (*registered) {
            return;
        }
        context->add_cycle_callback(&target_receiver, run_target, call_on_stop);
        context->add_cycle_callback(&remover_receiver, remove_target, call_on_stop);
        *registered = true;
    }

    static void run_target(void *, EvaluationContext &) noexcept { ++target_calls; }
    static void run_added(void *, EvaluationContext &) noexcept { ++added_calls; }

    static void remove_target(void *, EvaluationContext &context) noexcept {
        ++remover_calls;
        context.runtime_graph().remove_cycle_callback(&target_receiver);
        context.add_cycle_callback(&added_receiver, run_added);
    }

    inline static int target_receiver{};
    inline static int remover_receiver{};
    inline static int added_receiver{};
    inline static int target_calls{};
    inline static int remover_calls{};
    inline static int added_calls{};
};

TEST_CASE("removing a pending cycle callback during callback execution is safe",
          "[evaluation][lifecycle][feature:nested-graph][transition:removal]") {
    CycleCallbackRemovalDuringRun::target_calls = 0;
    CycleCallbackRemovalDuringRun::remover_calls = 0;
    CycleCallbackRemovalDuringRun::added_calls = 0;
    const auto remove_callback = hg::node<CycleCallbackRemovalDuringRun>();

    hg::run_graph([&](wp<TS<i64>> input) { remove_callback(input, false); }, {1, 2, 3});

    CHECK(CycleCallbackRemovalDuringRun::remover_calls == 1);
    CHECK(CycleCallbackRemovalDuringRun::target_calls == 0);
    CHECK(CycleCallbackRemovalDuringRun::added_calls == 1);
}

TEST_CASE("removing a pending cycle callback during stop is safe",
          "[evaluation][lifecycle][feature:nested-graph][transition:stop][transition:removal]") {
    CycleCallbackRemovalDuringRun::target_calls = 0;
    CycleCallbackRemovalDuringRun::remover_calls = 0;
    CycleCallbackRemovalDuringRun::added_calls = 0;
    const auto remove_callback = hg::node<CycleCallbackRemovalDuringRun>();

    hg::run_graph([&](wp<TS<i64>> input) { remove_callback(input, true); }, {1});

    CHECK(CycleCallbackRemovalDuringRun::remover_calls == 1);
    CHECK(CycleCallbackRemovalDuringRun::target_calls == 0);
    CHECK(CycleCallbackRemovalDuringRun::added_calls == 0);
}

struct NestedGraphLifetime {
    hg::State<int> marker;
    int id;

    void start() const noexcept { *marker = id + 1; }

    void finalize() const noexcept {
        if (*marker == id + 1) {
            ++finalizations;
        }
    }

    inline static int finalizations{};
};

struct NestedGraphStartFailure {
    hg::State<int> marker;

    void start() const {
        *marker = 1;
        throw std::runtime_error{"nested graph startup failed"};
    }

    void finalize() const noexcept {
        if (*marker == 1) {
            ++finalizations;
        }
    }

    inline static int finalizations{};
};

TEST_CASE("nested graph replacement defers destruction and rolls back a failed start",
          "[evaluation][feature:nested-graph][feature:lifecycle][feature:finalization]"
          "[transition:start][transition:stop][transition:restart]") {
    CHECK(sizeof(hg::RuntimeGraph) == 96);
    CHECK(sizeof(hg::NestedGraph) == 16);

    auto memory = counting_memory_resource{};
    {
        const auto owner = hg::node<CycleCallbackOwner>();
        const auto lifetime = hg::node<NestedGraphLifetime>();
        const auto failure = hg::node<NestedGraphStartFailure>();
        const auto compile_graph = [](auto define) {
            auto wiring = hg::GraphWiring{};
            {
                auto context = hg::WiringContext{wiring};
                define();
            }
            wiring.finalize();
            return std::make_shared<const hg::CompiledGraph>(wiring);
        };
        const auto root_graph = compile_graph([&] { owner(); });
        auto global_state = hg::GlobalState{hg::allocator{memory}};
        auto root = hg::RuntimeGraph{*root_graph, hg::allocator{memory}, global_state};
        root.start(hg::MIN_DT + std::chrono::microseconds{1});

        auto scratch = hg::details::EvaluationScratch{memory};
        auto global_context = root.make_global_evaluation_context(global_state, scratch,
                                                                  hg::SystemWallClock::instance());
        auto context = hg::EvaluationContext{global_context, hg::MIN_DT + std::chrono::microseconds{1}, root};
        auto evaluator = hg::RuntimeGraphEvaluator{};
        evaluator.bind_owner(root, 0);

        auto nested = hg::NestedGraph{};
        const auto first = hg::CompiledNestedGraph{.graph = compile_graph([&] { lifetime(0); })};
        const auto failed = hg::CompiledNestedGraph{.graph = compile_graph([&] { failure(); })};
        NestedGraphLifetime::finalizations = 0;
        NestedGraphStartFailure::finalizations = 0;

        nested.start(first, evaluator, context, {});
        CHECK(nested.running());
        nested.stop(context);
        CHECK_FALSE(nested.running());
        CHECK(NestedGraphLifetime::finalizations == 0);

        CHECK_THROWS_AS(nested.start(failed, evaluator, context, {}), std::runtime_error);
        CHECK_FALSE(nested.running());
        CHECK(NestedGraphStartFailure::finalizations == 1);
        CHECK(NestedGraphLifetime::finalizations == 0);

        root.evaluate(hg::MIN_DT + std::chrono::microseconds{2});
        CHECK(NestedGraphLifetime::finalizations == 1);
        nested.stop_and_destroy(context);
    }

    CHECK(memory.allocated_bytes() == 0);
    CHECK(memory.allocations() == 0);
}

TEST_CASE("nested graph supports inferred callable output guarantees",
          "[evaluation][nested][feature:nested-graph][feature:node-api][definition:function][series:value]"
          "[guarantee:ordinary]") {
    const auto increment = hg::node([](i64 value) { return value + 1; });
    const auto inner =
        hg::graph("name"_n = "inferred_callable_nested_graph", [=](hg::wp<TS<i64>> input) { return increment(input); });

    const auto outer = [=](hg::wp<TS<i64>> input, hg::wp<TS<bool>> run) { return hg::nested(inner, run, input); };

    const auto inputs = std::array{1, 2, 3};
    const auto running = std::array{true, true, true};
    CHECK(hg::run_graph(outer, inputs, running).equals<i64>({2, 3, 4}));
}

TEST_CASE("nested reference signature types outlive their node templates",
          "[wiring][nested][reference][feature:type-system][declaration:generic]") {
    const auto wiring = std::make_shared<GraphWiring>();
    const auto &input_type = REF<TS<i64>, TimeSeriesGuarantee::const_>::type();
    const auto graph_signature =
        hg::ResolvedSignature{hg::util::symbol{"inner"}, {{hg::util::symbol{"input"}, &input_type}}, &TS<i64>::type()};

    const auto make_node = [&] {
        return hg::details::make_nested_node({wiring, {}, 0}, graph_signature);
    };

    auto first_node = make_node();
    const auto *reference_type = first_node->signature().parameter(1).type;
    first_node.reset();

    REQUIRE(reference_type == &input_type);
    CHECK(reference_type->is_time_series());
    const auto &reference = hg::type_cast<TimeSeriesReferenceType>(*reference_type);
    CHECK(&reference.target_type() == &TS<i64>::type());
    CHECK(reference.guarantee().value() == TimeSeriesGuarantee::const_);

    const auto second_node = make_node();
    CHECK(second_node->signature().parameter(1).type == reference_type);
}

struct FinalizingInner {
    In<TS<i64>> input;
    State<int> marker;
    Out<TS<i64>> output;

    void start(EvaluationContext *context) const noexcept {
        *marker = 1;
        if (global_state == nullptr) {
            global_state = &context->global_state();
        }
    }

    void evaluate() const { output.set(*input); }

    void finalize(EvaluationContext *context) const noexcept {
        if (finalization_count < finalization_times.size()) {
            finalization_times[finalization_count] = context->evaluation_time();
            correct_state[finalization_count] = *marker == 1 && &context->global_state() == global_state;
        }
        ++finalization_count;
    }

    inline static GlobalState *global_state{};
    inline static std::array<EvaluationTime, 2> finalization_times{};
    inline static std::array<bool, 2> correct_state{};
    inline static std::size_t finalization_count{};
};

TEST_CASE("inner graph finalization uses the outer cycle's global state",
          "[evaluation][nested][lifecycle][feature:finalization][transition:removal]") {
    const auto finalizing = hg::node<FinalizingInner>();
    const auto inner =
        hg::graph("name"_n = "finalizing_inner", [&](hg::wp<TS<i64>> input) { return finalizing(input); });

    const auto outer = [&](hg::wp<TS<i64>> input, hg::wp<TS<bool>> run) { return hg::nested(inner, run, input); };
    FinalizingInner::global_state = nullptr;
    FinalizingInner::finalization_count = 0;

    const auto actual = hg::run_graph(outer, {1, 2, 3, 4}, {true, false, false, true});

    CHECK(actual.equals({1, std::nullopt, std::nullopt, 4}));
    CHECK(FinalizingInner::finalization_count == 2);
    CHECK(FinalizingInner::finalization_times ==
          std::array{MIN_DT + std::chrono::microseconds{3}, MIN_DT + std::chrono::microseconds{4}});
    CHECK(FinalizingInner::correct_state == std::array{true, true});
}

struct NestedTransitionSink {
    In<TS<i64>, hg::optional> input;
    Out<TS<i64>> output;

    void evaluate(EvaluationContext *context) const {
        observed.push_back(
            {input.valid(), input.became_valid(), input.became_invalid(), input.modified(),
             input.last_updated() == context->evaluation_time()});
        if (input.valid()) {
            output.set(*input);
        }
    }

    inline static std::vector<std::array<bool, 5>> observed;
};

TEST_CASE("a nested boundary samples an existing valid input when it starts",
          "[evaluation][nested][reference][feature:validity][transition:first-tick][validity:optional]"
          "[series:value][binding:value-to-reference]") {
    const auto sink = hg::node<NestedTransitionSink>();
    const auto inner =
        hg::graph("name"_n = "nested_transition_sink", [&](hg::wp<TS<i64>> input) { return sink(input); });

    const auto outer = [&](hg::wp<TS<i64>> input, hg::wp<TS<bool>> run) { return hg::nested(inner, run, input); };
    NestedTransitionSink::observed.clear();

    const auto actual = hg::run_graph(
        outer, std::array<std::optional<i64>, 3>{10, std::nullopt, std::nullopt}, std::array{false, false, true});

    CHECK(actual.equals({std::nullopt, std::nullopt, 10}));
    CHECK(NestedTransitionSink::observed ==
          std::vector<std::array<bool, 5>>{{true, true, false, true, true}});
}

TEST_CASE("a nested boundary retains a direct final target guarantee",
          "[evaluation][nested][reference][feature:nested-graph][shape:nested][series:value]"
          "[guarantee:final][binding:value-to-reference][binding:reference-to-value]") {
    const auto publish_final =
        hg::node([](In<TS<i64>> input, Out<TS<i64, TimeSeriesGuarantee::final>> output) { output.set(*input); });

    const auto read_final = hg::node([](In<TS<i64, TimeSeriesGuarantee::final>> input,
                                        Out<TS<i64, TimeSeriesGuarantee::final>> output) { output.set(*input); });
    const auto inner =
        hg::graph("name"_n = "read_final_nested_input",
                  [&](hg::wp<REF<TS<i64, TimeSeriesGuarantee::final>, TimeSeriesGuarantee::const_>> input) {
                      return read_final(input);
                  });

    const auto outer = [&](hg::wp<TS<i64>> input, hg::wp<TS<bool>> run) {
        return hg::nested(inner, run, publish_final(input));
    };

    CHECK(hg::run_graph(outer, {17}, {true}).equals({17}));
}

TEST_CASE("a validity-filtered nested output suspends its target after publishing a reference",
          "[evaluation][nested][reference][feature:validity][feature:input-activity][transition:update]") {
    const auto observe_reference = node([](In<TS<i64>> trigger, In<REF<TS<i64>>, passive, optional> reference) {
        return reference.modified();
    });

    const auto graph = [&](wp<TS<i64>> value, wp<TS<i64>> trigger) {
        const auto filtered = details::wire_validity_filtered_nested_output(value, TS<i64>::type());
        return observe_reference(trigger, filtered);
    };

    const auto actual = hg::run_graph(graph, {10, 20, 30}, {1, 2, 3});

    CHECK(actual.equals<bool>({true, false, false}));
}

TEST_CASE("a validity-filtered nested output invalidates and restores its reference with its target",
          "[evaluation][nested][reference][feature:validity][transition:invalidation]"
          "[transition:revalidation]") {
    const auto observe_reference = node([](In<TS<i64>> trigger, In<REF<TS<i64>>, passive, optional> reference) {
        return reference.modified();
    });

    const auto graph = [&](wp<TS<i64>> value, wp<TS<i64>> trigger) {
        const auto filtered = details::wire_validity_filtered_nested_output(value, TS<i64>::type());
        return observe_reference(trigger, filtered);
    };

    const auto actual = hg::run_graph(graph, {literal::INVALID, 10, literal::INVALID, 20}, {1, 2, 3, 4});

    CHECK(actual.equals<bool>({false, true, true, true}));
}

TEST_CASE("a validity-filtered nested output unbinds and restores its source reference",
          "[evaluation][nested][reference][feature:validity][transition:invalidation]"
          "[transition:revalidation][binding:reference-to-reference]") {
    const auto select = node([](In<TS<bool>> bound, In<REF<TS<i64>>> target, Out<REF<TS<i64>>> output) {
        if (*bound) {
            output.copy(target);
        } else {
            output.invalidate();
        }
    });
    const auto observe_reference = node([](In<TS<i64>> trigger,
                                           In<REF<TS<i64>>, passive, optional> reference) {
        return *trigger >= 0 && reference.modified();
    });

    const auto graph = [&](wp<TS<bool>> bound, wp<TS<i64>> value, wp<TS<i64>> trigger) {
        const auto selected = select(bound, value);
        const auto filtered = details::wire_validity_filtered_nested_output(selected, TS<i64>::type());
        return observe_reference(trigger, filtered);
    };

    const auto actual = hg::run_graph(graph, {true, false, literal::SKIP, true, literal::SKIP},
                                      {10, 20, 30, 40, 50}, {1, 2, 3, 4, 5});

    CHECK(actual.equals<bool>({true, true, false, true, false}));
}

template <int Delay, i64 Value> struct DelayedInvalidTarget {
    static constexpr bool uses_timed_scheduling = true;

    Out<TS<i64>> output;

    void start(EvaluationContext *context) const {
        context->schedule_at(context->evaluation_time() + std::chrono::microseconds{Delay});
    }

    void evaluate() const { output.set(Value); }
};

TEST_CASE("a validity-filtered nested output tracks a reference replacement while both targets are invalid",
          "[evaluation][nested][reference][feature:validity][transition:update]"
          "[binding:reference-to-reference]") {
    const auto first_target = node<DelayedInvalidTarget<2, 10>>();
    const auto second_target = node<DelayedInvalidTarget<3, 20>>();
    const auto select = node([](In<TS<i64>> condition, In<REF<TS<i64>>> first, In<REF<TS<i64>>> second,
                                Out<REF<TS<i64>>> output) { output.copy(*condition ? second : first); });

    const auto graph = [&](wp<TS<i64>> condition) {
        const auto first = first_target();
        const auto second = second_target();
        const auto selected = select(condition, first, second);
        const auto filtered = details::wire_validity_filtered_nested_output(selected, TS<i64>::type());
        return wp<TS<i64>>{filtered};
    };

    const auto actual = hg::run_graph(graph, {0, 1, literal::SKIP, literal::SKIP});

    CHECK(actual.equals({std::nullopt, std::nullopt, std::nullopt, 20}));
}

TEST_CASE("a dynamic input controls scheduling inside a nested graph",
          "[feature:input-activity][feature:nested-graph][series:value][definition:function]"
          "[shape:nested][activity:active][transition:update]") {
    const auto gate = hg::node([](In<TS<i64>, hg::dynamic_activity> input, Out<TS<i64>> output) {
        output.set(*input);
        input.make_passive();
    });

    const auto inner = hg::graph("name"_n = "dynamic_input", [&](hg::wp<TS<i64>> input) { return gate(input); });

    const auto outer = [&](hg::wp<TS<i64>> input, hg::wp<TS<bool>> run) { return hg::nested(inner, run, input); };

    CHECK(hg::run_graph(outer, {1, 2}, {true, true})
              .equals({1, std::nullopt}));
}

struct ReferenceIncrement {
    In<TS<i64>> input;
    Out<TS<i64>> output;

    void start() const noexcept { ++starts; }
    void evaluate() const { output.set(*input + 1); }
    void stop() const noexcept { ++stops; }

    inline static int starts{};
    inline static int stops{};
};

struct ReferenceDouble {
    In<TS<i64>> input;
    Out<TS<i64>> output;

    void start() const noexcept { ++starts; }
    void evaluate() const { output.set(*input * 2); }
    void stop() const noexcept { ++stops; }

    inline static int starts{};
    inline static int stops{};
};

TEST_CASE("a reference connects two nested graphs",
          "[evaluation][nested][reference][feature:nested-graph][shape:nested][scalar:integer]"
          "[series:value][binding:value-to-reference][binding:reference-to-value]") {
    const auto increment = hg::node<ReferenceIncrement>();

    const auto twice = hg::node<ReferenceDouble>();
    const auto first =
        hg::graph("name"_n = "first", [&](hg::wp<TS<i64>> input) -> hg::wp<TS<i64>> { return increment(input); });
    const auto second =
        hg::graph("name"_n = "second", [&](hg::wp<TS<i64>> input) -> hg::wp<TS<i64>> { return twice(input); });
    ReferenceIncrement::starts = 0;
    ReferenceIncrement::stops = 0;
    ReferenceDouble::starts = 0;
    ReferenceDouble::stops = 0;

    const auto outer = [&](hg::wp<TS<i64>> input, hg::wp<TS<bool>> run) {
        const auto first_output = hg::nested(first, run, input);
        return hg::nested(second, run, first_output);
    };

    const auto values = {1, 2, 3, 4, 5};
    const auto running = {true, true, false, true, true};
    const auto actual = hg::run_graph(outer, values, running);
    CHECK(actual.equals({4, 6, std::nullopt, 10, 12}));
    CHECK(ReferenceIncrement::starts == 2);
    CHECK(ReferenceIncrement::stops == 2);
    CHECK(ReferenceDouble::starts == 2);
    CHECK(ReferenceDouble::stops == 2);
}

struct NestedLeafIncrement {
    In<TS<i64>> input;
    Out<TS<i64>> output;

    void start() const noexcept { ++starts; }
    void evaluate() const { output.set(*input + 1); }
    void stop() const noexcept { ++stops; }

    inline static int starts{};
    inline static int stops{};
};

struct NestedMiddlePassThrough {
    In<TS<i64>> input;
    Out<TS<i64>> output;

    void start() const noexcept { ++starts; }
    void evaluate() const { output.set(*input); }
    void stop() const noexcept { ++stops; }

    inline static int starts{};
    inline static int stops{};
};

struct GlobalContextRootPassThrough {
    In<TS<i64>> input;
    Out<TS<i64>> output;

    void start(EvaluationContext *context) const {
        ++start_count;
        observe(*context);
    }

    void evaluate(EvaluationContext *context) const {
        ++evaluate_count;
        observe(*context);
        output.set(*input);
    }

    void stop(EvaluationContext *context) const noexcept {
        ++stop_count;
        observe(*context);
    }

    static void observe(EvaluationContext &context) noexcept {
        const auto &global = context.global_context();
        if (global_context == nullptr) {
            global_context = &global;
            root_graph = &context.runtime_graph();
            root_evaluator = global.root_runtime_graph_evaluator;
        } else {
            root_context_is_stable = root_context_is_stable && global_context == &global &&
                                     root_graph == &context.runtime_graph() &&
                                     root_evaluator == global.root_runtime_graph_evaluator;
        }
    }

    inline static const hg::GlobalEvaluationContext *global_context{};
    inline static RuntimeGraph *root_graph{};
    inline static RuntimeGraphEvaluator *root_evaluator{};
    inline static bool root_context_is_stable{true};
    inline static std::size_t start_count{};
    inline static std::size_t evaluate_count{};
    inline static std::size_t stop_count{};
};

struct GlobalContextLeafProbe {
    In<TS<i64>> input;
    Out<TS<i64>> output;

    void start(EvaluationContext *context) const {
        ++start_count;
        observe(*context);
    }

    void evaluate(EvaluationContext *context) const {
        ++evaluate_count;
        observe(*context);
        output.set(*input);
    }

    void stop(EvaluationContext *context) const noexcept {
        ++stop_count;
        observe(*context);
    }

    static void observe(EvaluationContext &context) noexcept {
        const auto &global = context.global_context();
        shares_global_context = shares_global_context && &global == GlobalContextRootPassThrough::global_context;
        resolves_root_graph = resolves_root_graph &&
                              &global.root_runtime_graph == GlobalContextRootPassThrough::root_graph;
        resolves_root_evaluator = resolves_root_evaluator && global.root_runtime_graph_evaluator != nullptr &&
                                  global.root_runtime_graph_evaluator == GlobalContextRootPassThrough::root_evaluator;
        retains_local_graph = retains_local_graph &&
                              &context.runtime_graph() != GlobalContextRootPassThrough::root_graph;
    }

    inline static bool shares_global_context{true};
    inline static bool resolves_root_graph{true};
    inline static bool resolves_root_evaluator{true};
    inline static bool retains_local_graph{true};
    inline static std::size_t start_count{};
    inline static std::size_t evaluate_count{};
    inline static std::size_t stop_count{};
};

TEST_CASE("nested evaluation contexts share root graph services",
          "[evaluation][nested][context][feature:evaluation-context][feature:nested-graph]"
          "[shape:doubly-nested][series:value][scalar:integer][transition:lifecycle]") {
    const auto root_pass_through = hg::node<GlobalContextRootPassThrough>();

    const auto leaf_probe = hg::node<GlobalContextLeafProbe>();

    const auto leaf = hg::graph("name"_n = "global_context_leaf",
                                [&](hg::wp<TS<i64>> input) -> hg::wp<TS<i64>> { return leaf_probe(input); });

    const auto middle = hg::graph("name"_n = "global_context_middle",
                                  [&](hg::wp<TS<i64>> input, hg::wp<TS<bool>> run) -> hg::wp<TS<i64>> {
                                      return hg::wp<TS<i64>>{hg::nested(leaf, run, input)};
                                  });

    const auto outer = [&](hg::wp<TS<i64>> input, hg::wp<TS<bool>> run) -> hg::wp<TS<i64>> {
        return hg::wp<TS<i64>>{hg::nested(middle, run, root_pass_through(input), run)};
    };

    GlobalContextRootPassThrough::global_context = nullptr;
    GlobalContextRootPassThrough::root_graph = nullptr;
    GlobalContextRootPassThrough::root_evaluator = nullptr;
    GlobalContextRootPassThrough::root_context_is_stable = true;
    GlobalContextRootPassThrough::start_count = 0;
    GlobalContextRootPassThrough::evaluate_count = 0;
    GlobalContextRootPassThrough::stop_count = 0;
    GlobalContextLeafProbe::shares_global_context = true;
    GlobalContextLeafProbe::resolves_root_graph = true;
    GlobalContextLeafProbe::resolves_root_evaluator = true;
    GlobalContextLeafProbe::retains_local_graph = true;
    GlobalContextLeafProbe::start_count = 0;
    GlobalContextLeafProbe::evaluate_count = 0;
    GlobalContextLeafProbe::stop_count = 0;

    const auto values = {1, 2, 3};
    const auto running = {true, true, true};
    CHECK(hg::run_graph(outer, values, running).equals<i64>({1, 2, 3}));
    CHECK(GlobalContextRootPassThrough::root_context_is_stable);
    CHECK(GlobalContextRootPassThrough::start_count == 1);
    CHECK(GlobalContextRootPassThrough::evaluate_count == 3);
    CHECK(GlobalContextRootPassThrough::stop_count == 1);
    CHECK(GlobalContextRootPassThrough::root_evaluator != nullptr);
    CHECK(GlobalContextLeafProbe::start_count == 1);
    CHECK(GlobalContextLeafProbe::evaluate_count == 3);
    CHECK(GlobalContextLeafProbe::stop_count == 1);
    CHECK(GlobalContextLeafProbe::shares_global_context);
    CHECK(GlobalContextLeafProbe::resolves_root_graph);
    CHECK(GlobalContextLeafProbe::resolves_root_evaluator);
    CHECK(GlobalContextLeafProbe::retains_local_graph);
}

TEST_CASE("a nested graph can own another nested graph",
          "[evaluation][nested][lifecycle][reference][feature:nested-graph][shape:nested]"
          "[shape:doubly-nested][scalar:integer][series:value][binding:value-to-reference]"
          "[binding:reference-to-value]") {
    const auto increment = hg::node<NestedLeafIncrement>();

    const auto pass_through = hg::node<NestedMiddlePassThrough>();
    const auto leaf =
        hg::graph("name"_n = "leaf", [&](hg::wp<TS<i64>> input) -> hg::wp<TS<i64>> { return increment(input); });
    const auto middle =
        hg::graph("name"_n = "middle", [&](hg::wp<TS<i64>> input, hg::wp<TS<bool>> run) -> hg::wp<TS<i64>> {
            const auto value = pass_through(input);
            return hg::wp<TS<i64>>{hg::nested(leaf, run, value)};
        });
    NestedLeafIncrement::starts = 0;
    NestedLeafIncrement::stops = 0;
    NestedMiddlePassThrough::starts = 0;
    NestedMiddlePassThrough::stops = 0;

    const auto outer = [&](hg::wp<TS<i64>> input, hg::wp<TS<bool>> run) { return hg::nested(middle, run, input, run); };

    const auto values = {1, 2, 3, 4, 5};
    const auto running = {true, false, true, false, true};
    const auto actual = hg::run_graph(outer, values, running);
    CHECK(actual.equals({2, std::nullopt, 4, std::nullopt, 6}));
    CHECK(NestedMiddlePassThrough::starts == 3);
    CHECK(NestedMiddlePassThrough::stops == 3);
    CHECK(NestedLeafIncrement::starts == 3);
    CHECK(NestedLeafIncrement::stops == 3);
}

struct InnerTimer {
    static constexpr bool uses_timed_scheduling = true;

    In<TS<i64>, hg::passive> input;
    State<int> step;
    Out<TS<i64>> output;

    void start(EvaluationContext *context) const {
        output.set(*input);
        context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
    }

    void evaluate(EvaluationContext *context) const {
        ++*step;
        output.set(*input + *step);
        if (*step != 2) {
            context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
        }
    }
};

TEST_CASE("timed inner scheduling selects an outer cycle",
          "[evaluation][nested][scheduling][feature:nested-graph][shape:nested][scalar:integer]"
          "[series:value]") {
    const auto timer = hg::node<InnerTimer>();
    const auto inner =
        hg::graph("name"_n = "inner_timer", [&](hg::wp<TS<i64>> input) -> hg::wp<TS<i64>> { return timer(input); });

    const auto outer = [&](hg::wp<TS<i64>> input, hg::wp<TS<bool>> run) { return hg::nested(inner, run, input); };

    const auto values = std::array<i64, 1>{10};
    const auto running = std::array{true};
    const auto actual = hg::run_graph(outer, values, running);
    CHECK(actual.equals({10, 11, 12}));
}

struct NestedFreeListSum {
    In<TSL<TS<i64>, 2>> input;
    Out<TS<i64>> output;

    void evaluate() const { output.set(*input[0] + *input[1]); }
};

TEST_CASE("a free list processes reference changes in a nested graph",
          "[evaluation][nested][reference][tsl][series:fixed-tsl][free][representation:free]"
          "[feature:nested-graph][shape:nested][transition:update][scalar:integer]"
          "[binding:value-to-reference][binding:reference-to-value]") {
    const auto sum = hg::node<NestedFreeListSum>();
    const auto inner =
        hg::graph("name"_n = "inner_free_list", [&](hg::wp<TS<i64>> first, hg::wp<TS<i64>> second) -> hg::wp<TS<i64>> {
            return sum(hg::tsl(first, second));
        });

    const auto outer = [&](hg::wp<TS<i64>> first, hg::wp<TS<i64>> second, hg::wp<TS<bool>> run) {
        return hg::nested(inner, run, first, second);
    };
    const auto first = std::array<i64, 3>{1, 2, 3};
    const auto second = std::array<i64, 3>{10, 20, 30};
    const auto running = std::array{true, true, true};

    const auto actual = hg::run_graph(outer, first, second, running);

    CHECK(actual.equals({11, 22, 33}));
}

struct NestedListSum {
    In<TSL<TS<i64>, 2>> input;
    Out<TS<i64>> output;

    void evaluate() const { output.set(*input[0] + *input[1]); }
};

TEST_CASE("a reference to an outer free list works in a nested graph",
          "[evaluation][nested][reference][tsl][series:fixed-tsl][free][representation:free][boundary]"
          "[feature:nested-graph][shape:nested][scalar:integer][binding:value-to-reference]"
          "[binding:reference-to-value]") {
    const auto sum = hg::node<NestedListSum>();

    const auto inner = hg::graph("name"_n = "consume_outer_free_list",
                                 [&](hg::wp<TSL<TS<i64>, 2>> input) -> hg::wp<TS<i64>> { return sum(input); });

    const auto outer = [&](hg::wp<TS<i64>> first, hg::wp<TS<i64>> second, hg::wp<TS<bool>> run) {
        return hg::nested(inner, run, hg::tsl(first, second));
    };

    const auto actual =
        hg::run_graph(outer, {1, 2, 3, 4}, {10, 20, 30, 40}, {true, true, false, true});

    CHECK(actual.equals({11, 22, std::nullopt, 44}));
}

TEST_CASE("a reference to an inner free list works in the outer graph",
          "[evaluation][nested][reference][tsl][series:fixed-tsl][free][representation:free][boundary]"
          "[feature:nested-graph][shape:nested][scalar:integer][binding:value-to-reference]"
          "[binding:reference-to-value]") {
    const auto sum = hg::node<NestedListSum>();

    const auto materialize = hg::graph("name"_n = "materialize_free_list",
                                       [](hg::wp<TSL<TS<i64>, 2>> input) -> hg::wp<TSL<TS<i64>, 2>> { return input; });

    const auto inner = hg::graph("name"_n = "produce_inner_free_list",
                                 [&](hg::wp<TS<i64>> first, hg::wp<TS<i64>> second) -> hg::wp<TSL<TS<i64>, 2>> {
                                     return materialize(hg::tsl(first, second));
                                 });

    const auto outer = [&](hg::wp<TS<i64>> first, hg::wp<TS<i64>> second, hg::wp<TS<bool>> run) {
        return sum(hg::nested(inner, run, first, second));
    };

    const auto actual =
        hg::run_graph(outer, {1, 2, 3, 4}, {10, 20, 30, 40}, {true, true, false, true});

    CHECK(actual.equals({11, 22, std::nullopt, 44}));
}

TEST_CASE("a free list reference passes through a nested graph",
          "[evaluation][nested][reference][tsl][series:fixed-tsl][free][representation:free][boundary]"
          "[feature:nested-graph][shape:nested][scalar:integer][binding:value-to-reference]"
          "[binding:reference-to-value][binding:reference-to-reference]") {
    const auto sum = hg::node<NestedListSum>();

    const auto inner = hg::graph("name"_n = "pass_free_list",
                                 [](hg::wp<TSL<TS<i64>, 2>> input) -> hg::wp<TSL<TS<i64>, 2>> { return input; });

    const auto outer = [&](hg::wp<TS<i64>> first, hg::wp<TS<i64>> second, hg::wp<TS<bool>> run) {
        return sum(hg::nested(inner, run, hg::tsl(first, second)));
    };

    const auto actual =
        hg::run_graph(outer, {1, 2, 3, 4}, {10, 20, 30, 40}, {true, true, false, true});

    CHECK(actual.equals({11, 22, std::nullopt, 44}));
}

TEST_CASE("an outer free list reference item rebinds across a nested graph boundary",
          "[evaluation][nested][reference][tsl][series:fixed-tsl][free][representation:free][boundary]"
          "[feature:nested-graph][shape:nested][scalar:integer][binding:value-to-reference]"
          "[binding:reference-to-value][binding:reference-to-reference][transition:update]") {
    const auto select = hg::node([](In<TS<bool>> use_second, In<REF<TS<i64>>> first, In<REF<TS<i64>>> second,
                                    Out<REF<TS<i64>>> output) { output.copy(*use_second ? second : first); });

    const auto add = hg::node([](i64 left, i64 right) { return left + right; });

    const auto inner =
        hg::graph("name"_n = "consume_outer_free_reference_list",
                  [&](hg::wp<TSL<REF<TS<i64>, TimeSeriesGuarantee::ordinary>, 2>> input) -> hg::wp<TS<i64>> {
                      return add(input[0], input[1]);
                  });

    const auto outer = [&](hg::wp<TS<i64>> first, hg::wp<TS<i64>> second, hg::wp<TS<bool>> use_second,
                           hg::wp<TS<i64>> value, hg::wp<TS<bool>> run) {
        const auto selected = select(use_second, first, second);
        return hg::nested(inner, run, hg::tsl(value, selected));
    };

    const auto actual = hg::run_graph(
        outer, std::array<std::optional<i64>, 5>{10, 20, 30, std::nullopt, std::nullopt},
        std::array<std::optional<i64>, 5>{100, 200, std::nullopt, 300, std::nullopt},
        std::array<std::optional<bool>, 5>{false, true, std::nullopt, std::nullopt, std::nullopt},
        std::array<std::optional<i64>, 5>{1, 2, std::nullopt, std::nullopt, 3},
        std::array<std::optional<bool>, 5>{true, std::nullopt, std::nullopt, std::nullopt, std::nullopt});

    CHECK(actual.equals({11, 202, std::nullopt, 302, 303}));
}

TEST_CASE("an inner free list reference item rebinds across a nested graph boundary",
          "[evaluation][nested][reference][tsl][series:fixed-tsl][free][representation:free][boundary]"
          "[feature:nested-graph][shape:nested][scalar:integer][binding:value-to-reference]"
          "[binding:reference-to-value][binding:reference-to-reference][transition:update]") {
    const auto select = hg::node([](In<TS<bool>> use_second, In<REF<TS<i64>>> first, In<REF<TS<i64>>> second,
                                    Out<REF<TS<i64>>> output) { output.copy(*use_second ? second : first); });

    const auto add = hg::node([](i64 left, i64 right) { return left + right; });

    const auto materialize =
        hg::graph("name"_n = "materialize_free_reference_list",
                  [](hg::wp<TSL<REF<TS<i64>, TimeSeriesGuarantee::ordinary>, 2>> input)
                      -> hg::wp<TSL<REF<TS<i64>, TimeSeriesGuarantee::ordinary>, 2>> { return input; });
    const auto inner =
        hg::graph("name"_n = "produce_inner_free_reference_list",
                  [&](hg::wp<TS<i64>> first, hg::wp<TS<i64>> second, hg::wp<TS<bool>> use_second,
                      hg::wp<TS<i64>> value) -> hg::wp<TSL<REF<TS<i64>, TimeSeriesGuarantee::ordinary>, 2>> {
                      const auto selected = select(use_second, first, second);
                      return materialize(hg::tsl(value, selected));
                  });

    const auto outer = [&](hg::wp<TS<i64>> first, hg::wp<TS<i64>> second, hg::wp<TS<bool>> use_second,
                           hg::wp<TS<i64>> value, hg::wp<TS<bool>> run) {
        const auto reference = hg::nested(inner, run, first, second, use_second, value);
        const auto list = hg::wp<TSL<REF<TS<i64>, TimeSeriesGuarantee::ordinary>, 2>>{reference};
        return add(list[0], list[1]);
    };

    const auto actual = hg::run_graph(
        outer, std::array<std::optional<i64>, 5>{10, 20, 30, std::nullopt, std::nullopt},
        std::array<std::optional<i64>, 5>{100, 200, std::nullopt, 300, std::nullopt},
        std::array<std::optional<bool>, 5>{false, true, std::nullopt, std::nullopt, std::nullopt},
        std::array<std::optional<i64>, 5>{1, 2, std::nullopt, std::nullopt, 3},
        std::array<std::optional<bool>, 5>{true, std::nullopt, std::nullopt, std::nullopt, std::nullopt});

    CHECK(actual.equals({11, 202, std::nullopt, 302, 303}));
}

TEST_CASE("a free list reference item survives two boundaries and repeated lifecycle changes",
          "[evaluation][nested][reference][tsl][series:fixed-tsl][free][representation:free][boundary]"
          "[feature:nested-graph][shape:doubly-nested][scalar:integer][binding:value-to-reference]"
          "[binding:reference-to-value][binding:reference-to-reference][transition:update]"
          "[transition:stop][transition:restart]") {
    const auto select = hg::node([](In<TS<bool>> use_second, In<REF<TS<i64>>> first, In<REF<TS<i64>>> second,
                                    Out<REF<TS<i64>>> output) { output.copy(*use_second ? second : first); });

    const auto add = hg::node([](i64 left, i64 right) { return left + right; });

    const auto pass_through =
        hg::graph("name"_n = "pass_free_reference_list",
                  [](hg::wp<TSL<REF<TS<i64>, TimeSeriesGuarantee::ordinary>, 2>> input)
                      -> hg::wp<TSL<REF<TS<i64>, TimeSeriesGuarantee::ordinary>, 2>> { return input; });

    const auto outer = [&](hg::wp<TS<i64>> first, hg::wp<TS<i64>> second, hg::wp<TS<bool>> use_second,
                           hg::wp<TS<i64>> value, hg::wp<TS<bool>> run) {
        const auto selected = select(use_second, first, second);
        const auto list = hg::tsl(value, selected);
        const auto first_boundary = hg::nested(pass_through, run, list);
        const auto second_boundary = hg::nested(pass_through, run, first_boundary);
        const auto result = hg::wp<TSL<REF<TS<i64>, TimeSeriesGuarantee::ordinary>, 2>>{second_boundary};
        return add(result[0], result[1]);
    };

    const auto actual = hg::run_graph(
        outer, std::array<std::optional<i64>, 8>{10, 20, 30, std::nullopt, 40, std::nullopt, std::nullopt, 50},
        std::array<std::optional<i64>, 8>{100, 200, std::nullopt, 300, 400, std::nullopt, 500, std::nullopt},
        std::array<std::optional<bool>, 8>{false, true, std::nullopt, std::nullopt, false, std::nullopt, std::nullopt,
                                           std::nullopt},
        std::array<std::optional<i64>, 8>{1, 2, std::nullopt, std::nullopt, 4, std::nullopt, std::nullopt,
                                          std::nullopt},
        std::array<std::optional<bool>, 8>{true, std::nullopt, std::nullopt, false, std::nullopt, true, std::nullopt,
                                           std::nullopt});

    CHECK(actual.equals({11, 202, std::nullopt, std::nullopt, std::nullopt, 44, std::nullopt, 54}));
}

template <template <typename, auto...> typename Field> struct NestedIntBundle {
    Field<TS<i64>> first;
    Field<TS<i64>> second;
};

struct NestedBundleSum {
    In<TSB<NestedIntBundle>> input;
    Out<TS<i64>> output;

    void evaluate() const { output.set(*input->first + *input->second); }
};

TEST_CASE("a reference to an outer free bundle works in a nested graph",
          "[evaluation][nested][reference][tsb][series:tsb][free][representation:free][boundary]"
          "[feature:nested-graph][shape:nested][scalar:integer][binding:value-to-reference]"
          "[binding:reference-to-value]") {
    const auto sum = hg::node<NestedBundleSum>();

    const auto inner = hg::graph("name"_n = "consume_outer_free_bundle",
                                 [&](hg::wp<TSB<NestedIntBundle>> input) -> hg::wp<TS<i64>> { return sum(input); });

    const auto outer = [&](hg::wp<TS<i64>> first, hg::wp<TS<i64>> second, hg::wp<TS<bool>> run) {
        return hg::nested(inner, run, hg::bundle("first"_n = first, "second"_n = second));
    };

    const auto actual =
        hg::run_graph(outer, {1, 2, 3, 4}, {10, 20, 30, 40}, {true, true, false, true});

    CHECK(actual.equals({11, 22, std::nullopt, 44}));
}

TEST_CASE("a reference to an inner free bundle works in the outer graph",
          "[evaluation][nested][reference][tsb][series:tsb][free][representation:free][boundary]"
          "[feature:nested-graph][shape:nested][scalar:integer][binding:value-to-reference]"
          "[binding:reference-to-value]") {
    const auto sum = hg::node<NestedBundleSum>();
    const auto materialize =
        hg::graph("name"_n = "materialize_free_bundle",
                  [](hg::wp<TSB<NestedIntBundle>> input) -> hg::wp<TSB<NestedIntBundle>> { return input; });

    const auto inner = hg::graph("name"_n = "produce_inner_free_bundle",
                                 [&](hg::wp<TS<i64>> first, hg::wp<TS<i64>> second) -> hg::wp<TSB<NestedIntBundle>> {
                                     return materialize(hg::bundle("first"_n = first, "second"_n = second));
                                 });

    const auto outer = [&](hg::wp<TS<i64>> first, hg::wp<TS<i64>> second, hg::wp<TS<bool>> run) {
        return sum(hg::nested(inner, run, first, second));
    };

    const auto actual =
        hg::run_graph(outer, {1, 2, 3, 4}, {10, 20, 30, 40}, {true, true, false, true});

    CHECK(actual.equals({11, 22, std::nullopt, 44}));
}

TEST_CASE("a free bundle reference passes through a nested graph",
          "[evaluation][nested][reference][tsb][series:tsb][free][representation:free][boundary]"
          "[feature:nested-graph][shape:nested][scalar:integer][binding:value-to-reference]"
          "[binding:reference-to-value][binding:reference-to-reference]") {
    const auto sum = hg::node<NestedBundleSum>();
    const auto inner =
        hg::graph("name"_n = "pass_free_bundle",
                  [](hg::wp<TSB<NestedIntBundle>> input) -> hg::wp<TSB<NestedIntBundle>> { return input; });

    const auto outer = [&](hg::wp<TS<i64>> first, hg::wp<TS<i64>> second, hg::wp<TS<bool>> run) {
        return sum(hg::nested(inner, run, hg::bundle("first"_n = first, "second"_n = second)));
    };

    const auto actual =
        hg::run_graph(outer, {1, 2, 3, 4},
                                {10, 20, 30, 40}, {true, true, false, true});

    CHECK(actual.equals({11, 22, std::nullopt, 44}));
}

template <template <typename, auto...> typename Field> struct NestedReferenceBundle {
    Field<TS<i64>> value;
    Field<REF<TS<i64>, TimeSeriesGuarantee::ordinary>> selected;
};

TEST_CASE("an outer free bundle reference field rebinds across a nested graph boundary",
          "[evaluation][nested][reference][tsb][series:tsb][free][representation:free][boundary]"
          "[feature:nested-graph][shape:nested][scalar:integer][binding:value-to-reference]"
          "[binding:reference-to-value][binding:reference-to-reference][transition:update]") {
    const auto select = hg::node([](In<TS<bool>> use_second, In<REF<TS<i64>>> first, In<REF<TS<i64>>> second,
                                    Out<REF<TS<i64>>> output) { output.copy(*use_second ? second : first); });

    const auto add = hg::node([](i64 left, i64 right) { return left + right; });

    const auto inner = hg::graph("name"_n = "consume_outer_free_reference_bundle",
                                 [&](hg::wp<TSB<NestedReferenceBundle>> input) -> hg::wp<TS<i64>> {
                                     return add(input->value, input->selected);
                                 });

    const auto outer = [&](hg::wp<TS<i64>> first, hg::wp<TS<i64>> second, hg::wp<TS<bool>> use_second,
                           hg::wp<TS<i64>> value, hg::wp<TS<bool>> run) {
        const auto selected = select(use_second, first, second);
        const auto bundle = hg::bundle("value"_n = value, "selected"_n = selected);
        return hg::nested(inner, run, bundle);
    };

    const auto actual = hg::run_graph(
        outer, std::array<std::optional<i64>, 5>{10, 20, 30, std::nullopt, std::nullopt},
        std::array<std::optional<i64>, 5>{100, 200, std::nullopt, 300, std::nullopt},
        std::array<std::optional<bool>, 5>{false, true, std::nullopt, std::nullopt, std::nullopt},
        std::array<std::optional<i64>, 5>{1, 2, std::nullopt, std::nullopt, 3},
        std::array<std::optional<bool>, 5>{true, std::nullopt, std::nullopt, std::nullopt, std::nullopt});

    CHECK(actual.equals({11, 202, std::nullopt, 302, 303}));
}

TEST_CASE("an inner free bundle reference field rebinds across a nested graph boundary",
          "[evaluation][nested][reference][tsb][series:tsb][free][representation:free][boundary]"
          "[feature:nested-graph][shape:nested][scalar:integer][binding:value-to-reference]"
          "[binding:reference-to-value][binding:reference-to-reference][transition:update]") {
    const auto select = hg::node([](In<TS<bool>> use_second, In<REF<TS<i64>>> first, In<REF<TS<i64>>> second,
                                    Out<REF<TS<i64>>> output) { output.copy(*use_second ? second : first); });

    const auto add = hg::node([](i64 left, i64 right) { return left + right; });

    const auto materialize =
        hg::graph("name"_n = "materialize_free_reference_bundle",
                  [](hg::wp<TSB<NestedReferenceBundle>> input) -> hg::wp<TSB<NestedReferenceBundle>> { return input; });

    const auto inner = hg::graph("name"_n = "produce_inner_free_reference_bundle",
                                 [&](hg::wp<TS<i64>> first, hg::wp<TS<i64>> second, hg::wp<TS<bool>> use_second,
                                     hg::wp<TS<i64>> value) -> hg::wp<TSB<NestedReferenceBundle>> {
                                     const auto selected = select(use_second, first, second);
                                     return materialize(hg::bundle("value"_n = value, "selected"_n = selected));
                                 });

    const auto outer = [&](hg::wp<TS<i64>> first, hg::wp<TS<i64>> second, hg::wp<TS<bool>> use_second,
                           hg::wp<TS<i64>> value, hg::wp<TS<bool>> run) {
        const auto reference = hg::nested(inner, run, first, second, use_second, value);
        const auto bundle = hg::wp<TSB<NestedReferenceBundle>>{reference};
        return add(bundle->value, bundle->selected);
    };

    const auto actual = hg::run_graph(
        outer, std::array<std::optional<i64>, 5>{10, 20, 30, std::nullopt, std::nullopt},
        std::array<std::optional<i64>, 5>{100, 200, std::nullopt, 300, std::nullopt},
        std::array<std::optional<bool>, 5>{false, true, std::nullopt, std::nullopt, std::nullopt},
        std::array<std::optional<i64>, 5>{1, 2, std::nullopt, std::nullopt, 3},
        std::array<std::optional<bool>, 5>{true, std::nullopt, std::nullopt, std::nullopt, std::nullopt});

    CHECK(actual.equals({11, 202, std::nullopt, 302, 303}));
}

TEST_CASE("a free bundle reference field survives two boundaries and repeated lifecycle changes",
          "[evaluation][nested][reference][tsb][series:tsb][free][representation:free][boundary]"
          "[feature:nested-graph][shape:doubly-nested][scalar:integer][binding:value-to-reference]"
          "[binding:reference-to-value][binding:reference-to-reference][transition:update]"
          "[transition:stop][transition:restart]") {
    const auto select = hg::node([](In<TS<bool>> use_second, In<REF<TS<i64>>> first, In<REF<TS<i64>>> second,
                                    Out<REF<TS<i64>>> output) { output.copy(*use_second ? second : first); });

    const auto add = hg::node([](i64 left, i64 right) { return left + right; });

    const auto pass_through =
        hg::graph("name"_n = "pass_free_reference_bundle",
                  [](hg::wp<TSB<NestedReferenceBundle>> input) -> hg::wp<TSB<NestedReferenceBundle>> { return input; });

    const auto outer = [&](hg::wp<TS<i64>> first, hg::wp<TS<i64>> second, hg::wp<TS<bool>> use_second,
                           hg::wp<TS<i64>> value, hg::wp<TS<bool>> run) {
        const auto selected = select(use_second, first, second);
        const auto bundle = hg::bundle("value"_n = value, "selected"_n = selected);
        const auto first_boundary = hg::nested(pass_through, run, bundle);
        const auto second_boundary = hg::nested(pass_through, run, first_boundary);
        const auto result = hg::wp<TSB<NestedReferenceBundle>>{second_boundary};
        return add(result->value, result->selected);
    };

    const auto actual = hg::run_graph(
        outer, std::array<std::optional<i64>, 8>{10, 20, 30, std::nullopt, 40, std::nullopt, std::nullopt, 50},
        std::array<std::optional<i64>, 8>{100, 200, std::nullopt, 300, 400, std::nullopt, 500, std::nullopt},
        std::array<std::optional<bool>, 8>{false, true, std::nullopt, std::nullopt, false, std::nullopt, std::nullopt,
                                           std::nullopt},
        std::array<std::optional<i64>, 8>{1, 2, std::nullopt, std::nullopt, 4, std::nullopt, std::nullopt,
                                          std::nullopt},
        std::array<std::optional<bool>, 8>{true, std::nullopt, std::nullopt, false, std::nullopt, true, std::nullopt,
                                           std::nullopt});

    CHECK(actual.equals({11, 202, std::nullopt, std::nullopt, std::nullopt, 44, std::nullopt, 54}));
}

} // namespace
