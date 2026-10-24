#include <bg/graph.hpp>
#include <bg/evaluation_context.hpp>
#include <bg/evaluation_engine.hpp>
#include <bg/evaluation_profiler.hpp>
#include <bg/global_state.hpp>
#include <bg/node.hpp>
#include <bg/run_graph.hpp>
#include <bg/time_series/tsd/tsd_type.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <optional>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace bg;

namespace {

using namespace literals;

TEST_CASE("run_graph runs a node with initial values and tick histories",
          "[testing][evaluation][feature:test-graph][series:value][scalar:integer]") {
    const auto increment = node([](i64 input) { return input + 1; });

    CHECK(run_graph(increment, 41).equals<i64>({42}));
    CHECK(run_graph(increment, {1, 2, 3}).equals<i64>({2, 3, 4}));
}

TEST_CASE("run_graph accepts heterogeneous numeric tick histories",
          "[testing][evaluation][feature:test-graph][series:value][scalar:integer]") {
    const auto identity = [](wp<TS<i64>> input) { return input; };

    CHECK(run_graph(identity, {i64{3}, 1, -1}).equals<i64>({3, 1, -1}));
    CHECK(run_graph(identity, {}).equals<i64>({std::nullopt}));
}

TEST_CASE("run_graph accepts string literal tick histories",
          "[testing][evaluation][feature:test-graph][series:value][scalar:string]") {
    const auto identity = [](wp<TS<string>> input) { return input; };

    CHECK(run_graph(identity, {"first", "second", "third"}).equals<string>({"first", "second", "third"}));
}

TEST_CASE("run_graph distinguishes a collection event from a tick history",
          "[testing][evaluation][feature:test-graph][series:value][scalar:list]") {
    const auto count_items = node([](In<TS<List<i64>>> input) {
        return static_cast<i64>(input.value().size());
    });

    CHECK(run_graph(count_items, {1, 2, 3}).equals<i64>({3}));
    CHECK(run_graph(count_items, {}).equals<i64>({0}));
    CHECK(run_graph(count_items, {{1, 2}, {3}}).equals<i64>({2, 1}));
    CHECK(run_graph(count_items, ticks(std::vector<std::vector<i64>>{{1, 2}, {3}})).equals<i64>({2, 1}));

    auto values = std::istringstream{"1 2 3"};
    CHECK(run_graph(count_items, std::ranges::istream_view<i64>{values}).equals<i64>({3}));

    auto nested_values = std::istringstream{"4 5 6"};
    auto nested_range = std::ranges::istream_view<i64>{nested_values};
    CHECK(run_graph(count_items, ticks(std::views::single(std::move(nested_range)))).equals<i64>({3}));

    const auto count_const_list = [&](wp<TS<List<i64>, TimeSeriesGuarantee::const_>> input) {
        return count_items(input);
    };
    CHECK(run_graph(count_const_list, {1, 2, 3}).equals<i64>({3}));
    CHECK_THROWS_AS((run_graph(count_const_list, ticks(std::vector<std::vector<i64>>{{1}, {2}}))),
                    TypeResolutionError);

    const auto nested_constant = [](wp<TSD<i64, TS<i64, TimeSeriesGuarantee::const_>,
                                          TimeSeriesGuarantee::const_>> input) { return input; };
    CHECK_THROWS_AS((run_graph(nested_constant, GraphRunOptions{.capture_output = false},
                               input<TSD<i64, TS<i64, TimeSeriesGuarantee::const_>,
                                     TimeSeriesGuarantee::const_>>({{{1, 1}}}))),
                    TypeResolutionError);
}

TEST_CASE("run_graph preserves nested single-pass ranges during generic input inference",
          "[testing][evaluation][feature:test-graph][series:value][scalar:list]") {
    const auto count_inner_items = node([](In<TS<List<List<i64>>>> input) {
        return static_cast<i64>(input.value_view()[0].size());
    });

    auto values = std::istringstream{"1 2 3"};
    auto nested_range = std::views::single(std::ranges::istream_view<i64>{values});

    CHECK(run_graph(count_inner_items, nested_range).equals<i64>({3}));
}

TEST_CASE("run_graph constructs forced collection histories using their input types",
          "[testing][evaluation][feature:test-graph][transition:update][scalar:set][scalar:dict]") {
    const auto count_set = node([](In<TS<Set<i64>>> input) {
        return static_cast<i64>(input.value().size());
    });

    const auto sum_dictionary = node([](In<TS<Dict<i64, i64>>> input) {
        auto sum = i64{};
        input.value().for_each([&](auto, auto value) { sum += value.value(); });
        return sum;
    });

    const auto count_true_set = node([](In<TS<Set<bool>>> input) {
        auto count = i64{};
        input.value().for_each([&](auto value) { count += value.value(); });
        return count;
    });

    const auto set_history = ticks(std::vector<std::vector<i64>>{{1, 2}, {3}});
    const auto dictionary_history = ticks(std::vector<std::vector<std::pair<i64, i64>>>{
        {{1, 2}, {3, 4}},
        {{5, 6}},
    });
    const auto proxy_history = ticks(std::vector<std::vector<bool>>{{true, false}, {false}});

    CHECK(run_graph(count_set, set_history).equals<i64>({2, 1}));
    CHECK(run_graph(sum_dictionary, dictionary_history).equals<i64>({6, 6}));
    CHECK(run_graph(count_true_set, proxy_history).equals<i64>({1, 0}));
}

TEST_CASE("run_graph evaluates a node over input ticks",
          "[testing][evaluation][feature:test-graph][transition:update][series:value][scalar:integer]") {
    const auto add = node([](float lhs, float rhs) { return lhs + rhs; });
    const auto lhs = std::vector{1.0F, 2.0F, 3.0F};
    const auto rhs = std::vector{10.0F, 20.0F, 30.0F};

    const auto actual = run_graph(add, lhs, rhs);

    CHECK(actual.equals<float>({11.0F, 22.0F, 33.0F}));
    CHECK(run_graph(add, 10.0F, ticks({1.0F, 2.0F})).equals<float>({11.0F, 12.0F}));
}

TEST_CASE("run_graph infers literal types for plain callable inputs",
          "[testing][evaluation][feature:test-graph][transition:update][scalar:integer]") {
    const auto add_plain = node([](int lhs, int rhs) { return lhs + rhs; });

    const auto actual = run_graph(
        add_plain, {1, literal::SKIP, 3}, {literal::SKIP, 10, literal::SKIP});

    CHECK(actual.equals({literal::SKIP, 11, 13}));

    const auto empty = run_graph(add_plain, {}, {});
    CHECK(empty.equals({literal::SKIP}));

    const auto skipped = run_graph(
        add_plain, {literal::SKIP, literal::SKIP}, {literal::SKIP, literal::SKIP});
    CHECK(skipped.equals({literal::SKIP, literal::SKIP}));
}

TEST_CASE("run_graph infers literal types for optional aggregate plain inputs",
          "[testing][evaluation][feature:test-graph][transition:update][scalar:integer]") {
    struct PlainOptionalInput {
        int value;
        std::optional<int> offset;
        Out<TS<i64>> output;

        void evaluate() const { output.set(value + offset.value_or(0)); }
    };

    const auto add_optional = node<PlainOptionalInput>();

    const auto actual = run_graph(add_optional, {1, 2}, {literal::SKIP, 10});

    CHECK(actual.equals({1, 12}));
}

TEST_CASE("run_graph records a profile for a graph function", "[testing][evaluation][feature:profiling]") {
    const auto graph = [](wp<TS<i64>> input) { return input; };
    auto profiler = EvaluationProfiler{};

    const auto actual = run_graph(graph, GraphRunOptions{.recorder = profiler.recorder()}, {1, 2, 3});

    CHECK(actual.equals({1, 2, 3}));
    const auto root = profiler.root_graph_id();
    REQUIRE(root);
    const auto *summary = profiler.graph(*root);
    REQUIRE(summary);
    REQUIRE(summary->last_updated);
    const auto period = std::chrono::floor<std::chrono::minutes>(*summary->last_updated);
    REQUIRE(profiler.graph_period(*root, period));
    CHECK(profiler.graph_period(*root, period)->evaluations == 3);
}

TEST_CASE("run_graph applies its configured start time to every input path",
          "[testing][evaluation][feature:test-graph][feature:simulation]") {
    struct graph_run_time_observer {
        In<TS<i64>> input;
        Out<TS<i64>> output;

        void evaluate(EvaluationContext *context) const {
            output.set(static_cast<i64>(context->evaluation_time().time_since_epoch().count()));
        }
    };

    const auto observe = node<graph_run_time_observer>();

    const auto function = [&](wp<TS<i64>> input) { return observe(input); };

    const auto template_target = graph("name"_n = "observe_time", function);
    const auto start_time = EvaluationTime{std::chrono::microseconds{42}};
    const auto options = GraphRunOptions{.start_time = start_time};
    const auto expected = {std::optional<i64>{42}, std::optional<i64>{43}};

    SECTION("function typed literal") {
        CHECK(run_graph(function, options, input<TS<i64>>({1, 2})).equals<i64>(expected));
    }

    SECTION("template typed literal") {
        CHECK(run_graph(template_target, options, input<TS<i64>>({1, 2})).equals<i64>(expected));
    }

    SECTION("function inferred braces") {
        CHECK(run_graph(function, options, {1, 2}).equals<i64>(expected));
    }

    SECTION("template inferred braces") {
        CHECK(run_graph(template_target, options, {1, 2}).equals<i64>(expected));
    }

    SECTION("function numeric array") {
        CHECK(run_graph(function, options, std::array<i64, 2>{1, 2}).equals<i64>(expected));
    }

    SECTION("template numeric array") {
        CHECK(run_graph(template_target, options, std::array<i64, 2>{1, 2}).equals<i64>(expected));
    }

    SECTION("template runtime descriptor span") {
        auto ticks = std::vector<std::optional<ScalarValue<>>>{};
        ticks.emplace_back(std::in_place, i64{1});
        ticks.emplace_back(std::in_place, i64{2});
        const auto runtime_inputs = std::array{
            GraphRunInput{cpp_scalar_runtime_type<i64>(), ticks},
        };
        CHECK(run_graph(template_target, options, std::span<const GraphRunInput>{runtime_inputs})
                  .equals<i64>(expected));
    }
}

TEST_CASE("run_graph captures realtime output until its run-time limit",
          "[testing][evaluation][feature:test-graph][feature:realtime]") {
    const auto identity = [](wp<TS<i64>> input) { return input; };
    const auto ticks = std::array<i64, 2>{1, 2};

    const auto actual = run_graph(
        identity,
        GraphRunOptions{.engine = EvaluationEngine::realtime,
                       .run_time = std::chrono::milliseconds{50}},
        ticks);

    CHECK(actual.equals<i64>({1, 2}));
    CHECK(run_graph(identity, GraphRunOptions{.capture_output = false}, ticks).size() == 0);
}

TEST_CASE("run_graph records a profile for runtime-typed inputs", "[testing][evaluation][feature:profiling]") {
    const auto identity = graph("name"_n = "identity", [](wp<TS<i64>> input) { return input; });
    auto ticks = std::vector<std::optional<ScalarValue<>>>{};
    ticks.emplace_back(std::in_place, i64{1});
    ticks.emplace_back(std::in_place, i64{2});
    const auto inputs = std::array{GraphRunInput{cpp_scalar_runtime_type<i64>(), ticks}};
    auto profiler = EvaluationProfiler{};

    const auto actual = run_graph(
        identity,
        GraphRunOptions{.recorder = profiler.recorder()},
        std::span<const GraphRunInput>{inputs}
    );

    CHECK(actual.equals<i64>({1, 2}));
    REQUIRE(profiler.root_graph_id());
    CHECK(profiler.graph(*profiler.root_graph_id()) != nullptr);
}

TEST_CASE("run_graph records a profile for a wiring template", "[testing][evaluation][feature:profiling]") {
    const auto identity = graph("name"_n = "identity", [](wp<TS<i64>> input) { return input; });
    const auto ticks = std::vector<i64>{1, 2};
    auto profiler = EvaluationProfiler{};

    const auto actual = run_graph(identity, GraphRunOptions{.recorder = profiler.recorder()}, ticks);

    CHECK(actual.equals<i64>({1, 2}));
    REQUIRE(profiler.root_graph_id());
    CHECK(profiler.graph(*profiler.root_graph_id()) != nullptr);
}

TEST_CASE("run_graph supports skipped input ticks",
          "[testing][evaluation][feature:test-graph][transition:update][series:value][scalar:integer]") {
    const auto add = node([](float lhs, float rhs) { return lhs + rhs; });

    const auto add_graph = graph(
        "name"_n = "add_graph",
        [&](wp<TS<float>> lhs, wp<TS<float>> rhs) -> wp<TS<float>> { return add(lhs, rhs); });
    const auto lhs = std::vector<std::optional<float>>{1.0F, std::nullopt, 3.0F};
    const auto rhs = std::vector{10.0F, 20.0F, 30.0F};

    const auto actual = run_graph(add_graph, lhs, rhs);

    CHECK(actual.equals<float>({11.0F, 21.0F, 33.0F}));
}

TEST_CASE("run_graph accepts literal time-series dictionary updates",
          "[testing][evaluation][feature:test-graph][series:tsd][transition:update][scalar:integer]") {
    const auto observe = node([](In<TSD<i64, TS<i64>>> input,
                                    Out<TS<i64>> output) {
        output.set(input.contains(7) ? *input[7] : 0);
    });

    const auto graph = [&](wp<TSD<i64, TS<i64>>> input) { return observe(input); };

    const auto actual =
        run_graph(graph, {literal::SKIP, {{7, 10}}, literal::SKIP, {{7, 15}}, {{7, literal::REMOVE}}});

    CHECK(actual.equals<i64>({std::nullopt, 10, std::nullopt, 15, 0}));
}

template <template <typename, auto...> typename Field> struct LiteralNestedDictionaryBundle {
    Field<TSD<i64, TSS<i64>>> items;
};

TEST_CASE("run_graph accepts literals for deeply nested collection inputs",
          "[testing][evaluation][feature:test-graph][series:tsd][shape:deeply-nested][transition:removal]"
          "[scalar:integer]") {
    const auto observe = node([](In<TSD<i64, TSB<LiteralNestedDictionaryBundle>>> input,
                                    Out<TS<i64>> output) {
        output.set(static_cast<i64>(input[1]->items[2].size()));
    });

    const auto graph = [&](wp<TSD<i64, TSB<LiteralNestedDictionaryBundle>>> input) {
        return observe(input);
    };

    const auto actual = run_graph(
        graph, {{{1, {{"items", {{2, {1, 2}}}}}}}, {{1, {{"items", {{2, {literal::REMOVED(2)}}}}}}}});

    CHECK(actual.equals<i64>({2, 1}));
}

TEST_CASE("run_graph literal annotations support generic and multiple inputs",
          "[testing][evaluation][feature:test-graph][declaration:generic][series:value]") {
    const auto identity = graph("name"_n = "identity", [](wp<> input) { return wp<>{input}; });
    const auto typed_result = run_graph(identity, input<TS<i64>>({1, 2}));
    const auto runtime_result =
        run_graph(identity, input(TS<i64>::type(), {3, 4}));

    const auto add_i64 = node([](In<TS<i64>> lhs, In<TS<i64>> rhs,
                                     Out<TS<i64>> output) { output.set(*lhs + *rhs); });

    const auto add_graph = [&](wp<TS<i64>> lhs,
                               wp<TS<i64>> rhs) -> wp<TS<i64>> {
        return add_i64(lhs, rhs);
    };

    const auto multiple_result = run_graph(
        add_graph, input<TS<i64>>({1, 2}), input<TS<i64>>({10, 20}));
    const auto raw_multiple_result = run_graph(add_graph, {1, 2}, {10, 20});
    auto configured_profiler = EvaluationProfiler{};
    const auto configured_raw_multiple_result =
        run_graph(add_graph, GraphRunOptions{.recorder = configured_profiler.recorder()}, {1, 2}, {10, 20});

    const auto generic_rhs = [&](wp<TS<i64>> lhs, wp<> rhs) {
        return add_i64(lhs, rhs);
    };

    const auto annotated_second_result =
        run_graph(generic_rhs, ticks({1, 2}), input<TS<i64>>({10, 20}));

    const auto generic_lhs = [&](wp<> lhs, wp<TS<i64>> rhs) {
        return add_i64(lhs, rhs);
    };

    const auto annotated_first_result =
        run_graph(generic_lhs, input<TS<i64>>({1, 2}), ticks({10, 20}));

    const auto select_second = [](wp<TS<i64>>, wp<TS<i64>> rhs) { return rhs; };
    const auto empty_first_result =
        run_graph(select_second, ticks({}), ticks({30}));
    const auto configured_empty_first_result =
        run_graph(select_second, GraphRunOptions{}, ticks({}), ticks({30}));

    const auto observe_dictionary = node([](In<TSD<i64, TS<i64>>> input,
                                                Out<TS<i64>> output) {
        output.set(input.contains(7) ? *input[7] : 0);
    });

    const auto generic_dictionary = graph("name"_n = "generic_dictionary", [&](wp<> input) {
        return observe_dictionary(input);
    });
    const auto generic_dictionary_result = run_graph(
        generic_dictionary, input<TSD<i64, TS<i64>>>({{{7, 40}}}));

    CHECK(typed_result.equals<i64>({1, 2}));
    CHECK(runtime_result.equals<i64>({3, 4}));
    CHECK(multiple_result.equals<i64>({11, 22}));
    CHECK(raw_multiple_result.equals<i64>({11, 22}));
    CHECK(configured_raw_multiple_result.equals<i64>({11, 22}));
    REQUIRE(configured_profiler.root_graph_id());
    CHECK(annotated_first_result.equals<i64>({11, 22}));
    CHECK(annotated_second_result.equals<i64>({11, 22}));
    CHECK(empty_first_result.equals<i64>({30}));
    CHECK(configured_empty_first_result.equals<i64>({30}));
    CHECK(generic_dictionary_result.equals<i64>({40}));
    CHECK_THROWS_AS((run_graph(identity, {1, 2})), std::invalid_argument);
    CHECK_THROWS_AS((run_graph(identity, {1, 2}, {3, 4})), std::invalid_argument);
}

TEST_CASE("run_graph accepts raw nested scalar list literals",
          "[testing][evaluation][feature:test-graph][series:value][scalar:list]") {
    const auto observe = node([](In<TS<List<i64>>> input,
                                    Out<TS<i64>> output) {
        output.set(static_cast<i64>(input.value_view().size()));
    });

    const auto graph = [&](wp<TS<List<i64>>> input) { return observe(input); };

    const auto actual = run_graph(graph, {{1, 2}, {3, 4, 5}});

    CHECK(actual.equals<i64>({2, 3}));
}

TEST_CASE("run_graph accepts fixed time-series list literals",
          "[testing][evaluation][feature:test-graph][series:fixed-tsl][scalar:integer]") {
    const auto observe = node([](In<TSL<TS<i64>, 2>> input,
                                    Out<TS<i64>> output) {
        output.set(*input[0] + *input[1]);
    });

    const auto graph = [&](wp<TSL<TS<i64>, 2>> input) { return observe(input); };

    const auto actual = run_graph(graph, {{{0, 1}, {1, 10}}, {{0, 2}, {1, 20}}});

    CHECK(actual.equals<i64>({11, 22}));
}

TEST_CASE("run_graph runs literal sources when every tick is skipped",
          "[testing][evaluation][feature:test-graph][series:value]") {
    const auto identity = [](wp<TS<i64>> input) { return input; };

    const auto actual = run_graph(identity, {literal::SKIP, literal::SKIP, literal::SKIP});

    CHECK(actual.equals<i64>({std::nullopt, std::nullopt, std::nullopt}));
}

TEST_CASE("run_graph rejects tagged SKIP as an incompatible scalar payload",
          "[testing][evaluation][feature:test-graph][series:value]") {
    const auto identity = [](wp<TS<i64>> input) { return input; };

    CHECK_THROWS_AS(
        (run_graph(identity, {1, (literal::SKIP)(literal::ScalarLiteral{i64{2}})})),
        std::invalid_argument);
}

TEST_CASE("run_graph accepts empty raw inputs for functions and wiring templates",
          "[testing][evaluation][feature:test-graph][series:value][scalar:list]") {
    const auto count_items = node([](In<TS<List<i64>>> input,
                                         Out<TS<i64>> output) {
        output.set(static_cast<i64>(input.value_view().size()));
    });

    const auto function = [&](wp<TS<List<i64>>> input) { return count_items(input); };

    CHECK(run_graph(function, {}).equals<i64>({0}));
    CHECK(run_graph(function, {{}}).equals<i64>({0}));
    CHECK(run_graph(count_items, {}).equals<i64>({0}));
    CHECK(run_graph(count_items, {{}}).equals<i64>({0}));
    CHECK(run_graph(function, GraphRunOptions{}, {}).equals<i64>({0}));
    CHECK(run_graph(function, GraphRunOptions{}, {{}}).equals<i64>({0}));
    CHECK(run_graph(count_items, GraphRunOptions{}, {}).equals<i64>({0}));
    CHECK(run_graph(count_items, GraphRunOptions{}, {{}}).equals<i64>({0}));
}

TEST_CASE("run_graph literal inputs coerce values and revalidate after INVALID",
          "[testing][evaluation][feature:test-graph][transition:invalidation][transition:revalidation]") {
    const auto float_identity = [](wp<TS<float>> input) { return input; };
    const auto initial_float = run_graph(float_identity, 42);
    const auto float_result = run_graph(float_identity, {1, 2});
    const auto float_range_result = run_graph(float_identity, std::array{1, 2});
    const auto float_tick_result = run_graph(float_identity, ticks({1, 2}));
    const auto float_literal_result = run_graph(float_identity, {1, literal::SKIP, 3});
    const auto float_invalid_result = run_graph(float_identity, {1, literal::INVALID, 3});
    const auto double_identity = [](wp<TS<double>> input) { return input; };
    const auto double_result = run_graph(double_identity, 42);

    const auto integer_identity = [](wp<TS<i64>> input) { return input; };
    const auto integer_result =
        run_graph(integer_identity, {1, literal::INVALID, 3});
    const auto configured_integer_result =
        run_graph(integer_identity, GraphRunOptions{}, {1, literal::SKIP, 3});

    CHECK(initial_float.equals<float>({42.0F}));
    CHECK(float_result.equals<float>({1.0F, 2.0F}));
    CHECK(float_range_result.equals<float>({1.0F, 2.0F}));
    CHECK(float_tick_result.equals<float>({1.0F, 2.0F}));
    CHECK(float_literal_result.equals<float>({1.0F, std::nullopt, 3.0F}));
    CHECK(float_invalid_result.equals<float>({1.0F, std::nullopt, 3.0F}));
    CHECK(double_result.equals<double>({42.0}));
    CHECK(integer_result.equals<i64>({1, std::nullopt, 3}));
    CHECK(configured_integer_result.equals<i64>({1, std::nullopt, 3}));
}

TEST_CASE("run_graph accepts descriptor spans with more than four inputs",
          "[testing][evaluation][feature:test-graph][declaration:generic][series:value]") {
    const auto fifth = graph(
        "name"_n = "fifth",
        [](wp<TS<i64>>, wp<TS<i64>>, wp<TS<i64>>,
           wp<TS<i64>>, wp<TS<i64>> last) { return last; });
    const auto actual = run_graph(
        fifth, std::span<const GraphRunLiteralInput>{{{1}, {2}, {3}, {4}, {5}}});
    const auto configured_actual = run_graph(
        fifth, GraphRunOptions{}, GraphRunLiteralInput{1}, GraphRunLiteralInput{2}, GraphRunLiteralInput{3},
        GraphRunLiteralInput{4}, GraphRunLiteralInput{5});

    CHECK(actual.equals<i64>({5}));
    CHECK(configured_actual.equals<i64>({5}));
}

TEST_CASE("run_graph preserves zero-input wiring template option calls",
          "[testing][evaluation][feature:test-graph][feature:profiling]") {
    const auto produce = node([](Out<TS<i64>> output) { output.set(42); });

    const auto zero_input = graph("name"_n = "zero_input", [&] { return produce(); });
    const auto services = ServiceCatalogue{};
    auto profiler = EvaluationProfiler{};
    auto services_profiler = EvaluationProfiler{};

    CHECK(run_graph(zero_input).equals<i64>({42}));
    CHECK(run_graph(zero_input, GraphRunOptions{.services = &services})
              .equals<i64>({42}));
    CHECK(run_graph(zero_input, GraphRunOptions{.recorder = profiler.recorder()})
              .equals<i64>({42}));
    CHECK(run_graph(zero_input, GraphRunOptions{.services = &services,
                                                               .recorder = services_profiler.recorder()})
              .equals<i64>({42}));
    REQUIRE(profiler.root_graph_id());
    CHECK(profiler.graph(*profiler.root_graph_id()) != nullptr);
}

TEST_CASE("run_graph uses the supplied global state",
          "[testing][evaluation][feature:test-graph][feature:global-state]") {
    struct global_state_test_source {
        Out<TS<i64>> output;

        void start(EvaluationContext *context) const {
            const auto value = ScalarValue{42};
            context->global_state().write([&](auto &state) { state.set(util::symbol{"run_graph"}, value); });
            output.set(1);
        }

        void evaluate() const {}
    };

    const auto source = node<global_state_test_source>();

    const auto graph = [&] { return source(); };
    auto global_state = GlobalState{};

    const auto actual = run_graph(graph, GraphRunOptions{.global_state = &global_state});

    CHECK(actual.equals<i64>({1}));
    global_state.read([](const auto &values) {
        CHECK(*scalar_cast<i64>(values.at("run_graph")) == 42);
    });
}

TEST_CASE("run_graph accepts literal inputs with profiler and services",
          "[testing][evaluation][feature:test-graph][feature:profiling]") {
    const auto observe = node([](In<TSD<i64, TS<i64>>> input,
                                    Out<TS<i64>> output) {
        output.set(input.contains(7) ? *input[7] : 0);
    });

    const auto graph = [&](wp<TSD<i64, TS<i64>>> input) { return observe(input); };
    auto profiler = EvaluationProfiler{};
    auto services = ServiceCatalogue{};

    const auto actual = run_graph(
        graph,
        GraphRunOptions{.services = &services, .recorder = profiler.recorder()},
        {{{7, 10}}, {{7, 20}}}
    );

    CHECK(actual.equals<i64>({10, 20}));
    REQUIRE(profiler.root_graph_id());
    CHECK(profiler.graph(*profiler.root_graph_id()) != nullptr);
}

TEST_CASE("run_graph accepts a graph function and reports skipped cycles",
          "[testing][evaluation][feature:test-graph][definition:function][series:value][scalar:integer]") {
    const auto increment = node([](i64 input) { return input + 1; });

    const auto graph = [&](wp<TS<i64>> input) { return increment(input); };

    CHECK(run_graph(graph, {1, 2, literal::SKIP, 4}).equals({2, 3, std::nullopt, 5}));
}

TEST_CASE("run_graph captures a runtime-typed output",
          "[testing][evaluation][feature:test-graph][declaration:generic][series:value]") {
    const auto graph = [](wp<TS<i64>> input) { return wp<>{input}; };

    CHECK(run_graph(graph, {1, 2, literal::SKIP, 4}).equals<i64>({1, 2, std::nullopt, 4}));
}

TEST_CASE("run_graph accepts runtime-typed inputs",
          "[testing][evaluation][feature:test-graph][declaration:generic][series:value]") {
    const auto identity =
        graph("name"_n = "identity", [](wp<TS<i64>> input) { return input; });
    auto ticks = std::vector<std::optional<ScalarValue<>>>{};
    ticks.emplace_back(std::in_place, i64{1});
    ticks.emplace_back(std::nullopt);
    ticks.emplace_back(std::in_place, i64{3});
    const auto inputs = std::array{
        GraphRunInput{cpp_scalar_runtime_type<i64>(), ticks},
    };

    const auto actual = run_graph(identity, std::span<const GraphRunInput>{inputs});

    CHECK(actual.equals<i64>({1, std::nullopt, 3}));
}

TEST_CASE("runtime-typed run_graph inputs can contain only skipped ticks",
          "[testing][evaluation][feature:test-graph][declaration:generic][series:value]") {
    const auto identity =
        graph("name"_n = "identity", [](wp<TS<i64>> input) { return input; });
    const auto ticks = std::vector<std::optional<ScalarValue<>>>(3);
    const auto inputs = std::array{
        GraphRunInput{cpp_scalar_runtime_type<i64>(), ticks},
    };

    const auto actual = run_graph(identity, std::span<const GraphRunInput>{inputs});

    CHECK(actual.equals<i64>({std::nullopt, std::nullopt, std::nullopt}));
}

TEST_CASE("wiring converts C++ string arguments to the canonical graph type",
          "[testing][evaluation][feature:test-graph][definition:function][series:value][scalar:string]") {
    const auto publish = node(
        [](In<TS<i64>>, std::string value, Out<TS<i64>> output) {
            output.set(static_cast<int>(value.size()));
        });

    const auto graph = [&](wp<TS<i64>> input) { return publish(input, std::string{"text"}); };

    CHECK(run_graph(graph, {1}).equals<i64>({4}));
}

TEST_CASE("wiring converts optional C++ scalar arguments to the canonical graph type",
          "[testing][evaluation][feature:test-graph][definition:function][series:value][scalar:integer]") {
    const auto add = node(
        [](In<TS<i64>> input, std::optional<int> amount, Out<TS<i64>> output) {
            output.set(*input + amount.value_or(0));
        });

    const auto graph = [&](wp<TS<i64>> input) { return add(input, std::optional<int>{2}); };

    CHECK(run_graph(graph, {1, 2}).equals<i64>({3, 4}));
}

} // namespace
