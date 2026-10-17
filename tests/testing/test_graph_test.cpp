#include <bg/graph.hpp>
#include <bg/evaluation_context.hpp>
#include <bg/evaluation_engine.hpp>
#include <bg/evaluation_profiler.hpp>
#include <bg/global_state.hpp>
#include <bg/node.hpp>
#include <bg/testing/test_graph.hpp>
#include <bg/time_series/tsd/tsd_type.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace bg::literals;

struct Add {
    bg::In<bg::TS<float>> lhs;
    bg::In<bg::TS<float>> rhs;
    bg::Out<bg::TS<float>> output;

    void evaluate() const { output.set(*lhs + *rhs); }
};

const auto add = bg::node<Add>();

template <template <typename, auto...> typename Field> struct LiteralNestedDictionaryBundle {
    Field<bg::TSD<bg::i64, bg::TSS<bg::i64>>> items;
};

TEST_CASE("test_graph evaluates a node over input ticks",
          "[testing][evaluation][feature:test-graph][transition:update][series:value][scalar:integer]") {
    const auto lhs = std::vector{1.0F, 2.0F, 3.0F};
    const auto rhs = std::vector{10.0F, 20.0F, 30.0F};

    const auto actual = bg::testing::test_graph(add, lhs, rhs);

    CHECK(actual.equals<float>({11.0F, 22.0F, 33.0F}));
}

TEST_CASE("test_graph infers literal types for plain callable inputs",
          "[testing][evaluation][feature:test-graph][transition:update][scalar:integer]") {
    const auto add_plain = bg::node([](int lhs, int rhs) { return lhs + rhs; });

    const auto actual = bg::testing::test_graph(
        add_plain, {1, literal::SKIP, 3}, {literal::SKIP, 10, literal::SKIP});

    CHECK(actual.equals({literal::SKIP, 11, 13}));

    const auto empty = bg::testing::test_graph(add_plain, {}, {});
    CHECK(empty.equals({literal::SKIP}));

    const auto skipped = bg::testing::test_graph(
        add_plain, {literal::SKIP, literal::SKIP}, {literal::SKIP, literal::SKIP});
    CHECK(skipped.equals({literal::SKIP, literal::SKIP}));
}

struct PlainOptionalInput {
    int value;
    std::optional<int> offset;
    bg::Out<bg::TS<bg::i64>> output;

    void evaluate() const { output.set(value + offset.value_or(0)); }
};

TEST_CASE("test_graph infers literal types for optional aggregate plain inputs",
          "[testing][evaluation][feature:test-graph][transition:update][scalar:integer]") {
    const auto add_optional = bg::node<PlainOptionalInput>();

    const auto actual = bg::testing::test_graph(add_optional, {1, 2}, {literal::SKIP, 10});

    CHECK(actual.equals({1, 12}));
}

TEST_CASE("test_graph records a profile for a graph function", "[testing][evaluation][feature:profiling]") {
    const auto graph = [](bg::wp<bg::TS<bg::i64>> input) { return input; };
    auto profiler = bg::EvaluationProfiler{};

    const auto actual = bg::testing::test_graph(graph, bg::GraphRunOptions{.recorder = profiler.recorder()}, {1, 2, 3});

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

TEST_CASE("test_graph applies its configured start time to every input path",
          "[testing][evaluation][feature:test-graph][feature:simulation]") {
    struct test_graph_time_observer {
        bg::In<bg::TS<bg::i64>> input;
        bg::Out<bg::TS<bg::i64>> output;

        void evaluate(bg::EvaluationContext *context) const {
            output.set(static_cast<bg::i64>(context->evaluation_time().time_since_epoch().count()));
        }
    };

    const auto observe = bg::node<test_graph_time_observer>();
    const auto function = [&](bg::wp<bg::TS<bg::i64>> input) -> bg::wp<bg::TS<bg::i64>> {
        return observe(input);
    };
    const auto template_target = bg::graph("name"_n = "observe_time", function);
    const auto start_time = bg::EvaluationTime{std::chrono::microseconds{42}};
    const auto options = bg::GraphRunOptions{.start_time = start_time};
    const auto expected = {std::optional<bg::i64>{42}, std::optional<bg::i64>{43}};
    CHECK(bg::testing::test_graph(function, options, bg::testing::input<bg::TS<bg::i64>>({1, 2}))
              .equals<bg::i64>(expected));
    CHECK(bg::testing::test_graph(template_target, options, bg::testing::input<bg::TS<bg::i64>>({1, 2}))
              .equals<bg::i64>(expected));
    CHECK(bg::testing::test_graph(function, options, std::array<bg::i64, 2>{1, 2}).equals<bg::i64>(expected));
    CHECK(bg::testing::test_graph(template_target, options, std::array<bg::i64, 2>{1, 2}).equals<bg::i64>(expected));

    auto ticks = std::vector<std::optional<bg::ScalarValue<>>>{};
    ticks.emplace_back(std::in_place, bg::i64{1});
    ticks.emplace_back(std::in_place, bg::i64{2});
    const auto runtime_inputs = std::array{
        bg::testing::TestGraphInput{bg::cpp_scalar_runtime_type<bg::i64>(), ticks},
    };
    CHECK(bg::testing::test_graph(template_target, options,
                                  std::span<const bg::testing::TestGraphInput>{runtime_inputs})
              .equals<bg::i64>(expected));
}

TEST_CASE("test_graph rejects realtime evaluation options", "[testing][evaluation][feature:test-graph]") {
    const auto identity = [](bg::wp<bg::TS<bg::i64>> input) { return input; };

    CHECK_THROWS_AS((bg::testing::test_graph(
                        identity, bg::GraphRunOptions{.engine = bg::EvaluationEngine::realtime}, {1})),
                    std::invalid_argument);
}

TEST_CASE("test_graph records a profile for runtime-typed inputs", "[testing][evaluation][feature:profiling]") {
    const auto identity = bg::graph("name"_n = "identity", [](bg::wp<bg::TS<bg::i64>> input) { return input; });
    auto ticks = std::vector<std::optional<bg::ScalarValue<>>>{};
    ticks.emplace_back(std::in_place, bg::i64{1});
    ticks.emplace_back(std::in_place, bg::i64{2});
    const auto inputs = std::array{bg::testing::TestGraphInput{bg::cpp_scalar_runtime_type<bg::i64>(), ticks}};
    auto profiler = bg::EvaluationProfiler{};

    const auto actual = bg::testing::test_graph(
        identity,
        bg::GraphRunOptions{.recorder = profiler.recorder()},
        std::span<const bg::testing::TestGraphInput>{inputs}
    );

    CHECK(actual.equals<bg::i64>({1, 2}));
    REQUIRE(profiler.root_graph_id());
    CHECK(profiler.graph(*profiler.root_graph_id()) != nullptr);
}

TEST_CASE("test_graph records a profile for a wiring template", "[testing][evaluation][feature:profiling]") {
    const auto identity = bg::graph("name"_n = "identity", [](bg::wp<bg::TS<bg::i64>> input) { return input; });
    const auto ticks = std::vector<bg::i64>{1, 2};
    auto profiler = bg::EvaluationProfiler{};

    const auto actual = bg::testing::test_graph(identity, bg::GraphRunOptions{.recorder = profiler.recorder()}, ticks);

    CHECK(actual.equals<bg::i64>({1, 2}));
    REQUIRE(profiler.root_graph_id());
    CHECK(profiler.graph(*profiler.root_graph_id()) != nullptr);
}

TEST_CASE("test_graph supports skipped input ticks",
          "[testing][evaluation][feature:test-graph][transition:update][series:value][scalar:integer]") {
    const auto add_graph = bg::graph(
        "name"_n = "add_graph",
        [](bg::wp<bg::TS<float>> lhs, bg::wp<bg::TS<float>> rhs) -> bg::wp<bg::TS<float>> { return add(lhs, rhs); });
    const auto lhs = std::vector<std::optional<float>>{1.0F, std::nullopt, 3.0F};
    const auto rhs = std::vector{10.0F, 20.0F, 30.0F};

    const auto actual = bg::testing::test_graph(add_graph, lhs, rhs);

    CHECK(actual.equals<float>({11.0F, 21.0F, 33.0F}));
}

TEST_CASE("test_graph accepts literal time-series dictionary updates",
          "[testing][evaluation][feature:test-graph][series:tsd][transition:update][scalar:integer]") {
    const auto observe = bg::node([](bg::In<bg::TSD<bg::i64, bg::TS<bg::i64>>> input,
                                    bg::Out<bg::TS<bg::i64>> output) {
        output.set(input.contains(7) ? *input[7] : 0);
    });
    const auto graph = [&](bg::wp<bg::TSD<bg::i64, bg::TS<bg::i64>>> input) { return observe(input); };

    const auto actual =
        bg::testing::test_graph(graph, {literal::SKIP, {{7, 10}}, literal::SKIP, {{7, 15}}, {{7, literal::REMOVE}}});

    CHECK(actual.equals<bg::i64>({std::nullopt, 10, std::nullopt, 15, 0}));
}

TEST_CASE("test_graph accepts literals for deeply nested collection inputs",
          "[testing][evaluation][feature:test-graph][series:tsd][shape:deeply-nested][transition:removal]"
          "[scalar:integer]") {
    const auto observe = bg::node([](bg::In<bg::TSD<bg::i64, bg::TSB<LiteralNestedDictionaryBundle>>> input,
                                    bg::Out<bg::TS<bg::i64>> output) {
        output.set(static_cast<bg::i64>(input[1]->items[2].size()));
    });
    const auto graph = [&](bg::wp<bg::TSD<bg::i64, bg::TSB<LiteralNestedDictionaryBundle>>> input) {
        return observe(input);
    };

    const auto actual = bg::testing::test_graph(
        graph, {{{1, {{"items", {{2, {1, 2}}}}}}}, {{1, {{"items", {{2, {literal::REMOVED(2)}}}}}}}});

    CHECK(actual.equals<bg::i64>({2, 1}));
}

TEST_CASE("test_graph literal annotations support generic and multiple inputs",
          "[testing][evaluation][feature:test-graph][declaration:generic][series:value]") {
    const auto identity = bg::graph("name"_n = "identity", [](bg::wp<> input) { return bg::wp<>{input}; });
    const auto typed_result = bg::testing::test_graph(identity, bg::testing::input<bg::TS<bg::i64>>({1, 2}));
    const auto runtime_result =
        bg::testing::test_graph(identity, bg::testing::input(bg::TS<bg::i64>::type(), {3, 4}));

    const auto add_i64 = bg::node([](bg::In<bg::TS<bg::i64>> lhs, bg::In<bg::TS<bg::i64>> rhs,
                                     bg::Out<bg::TS<bg::i64>> output) { output.set(*lhs + *rhs); });
    const auto add_graph = [&](bg::wp<bg::TS<bg::i64>> lhs,
                               bg::wp<bg::TS<bg::i64>> rhs) -> bg::wp<bg::TS<bg::i64>> {
        return add_i64(lhs, rhs);
    };
    const auto multiple_result = bg::testing::test_graph(
        add_graph, bg::testing::input<bg::TS<bg::i64>>({1, 2}), bg::testing::input<bg::TS<bg::i64>>({10, 20}));
    const auto raw_multiple_result = bg::testing::test_graph(add_graph, {1, 2}, {10, 20});
    auto configured_profiler = bg::EvaluationProfiler{};
    const auto configured_raw_multiple_result =
        bg::testing::test_graph(add_graph, bg::GraphRunOptions{.recorder = configured_profiler.recorder()}, {1, 2}, {10, 20});
    const auto generic_rhs = [&](bg::wp<bg::TS<bg::i64>> lhs, bg::wp<> rhs) {
        return add_i64(lhs, rhs.as<bg::TS<bg::i64>>());
    };
    const auto annotated_second_result =
        bg::testing::test_graph(generic_rhs, {1, 2}, bg::testing::input<bg::TS<bg::i64>>({10, 20}));
    const auto generic_lhs = [&](bg::wp<> lhs, bg::wp<bg::TS<bg::i64>> rhs) {
        return add_i64(lhs.as<bg::TS<bg::i64>>(), rhs);
    };
    const auto annotated_first_result =
        bg::testing::test_graph(generic_lhs, bg::testing::input<bg::TS<bg::i64>>({1, 2}), {10, 20});

    const auto select_second = [](bg::wp<bg::TS<bg::i64>>, bg::wp<bg::TS<bg::i64>> rhs) { return rhs; };
    const auto empty_first_result = bg::testing::test_graph(select_second, {}, {30});
    const auto configured_empty_first_result =
        bg::testing::test_graph(select_second, bg::GraphRunOptions{}, {}, {30});

    const auto observe_dictionary = bg::node([](bg::In<bg::TSD<bg::i64, bg::TS<bg::i64>>> input,
                                                bg::Out<bg::TS<bg::i64>> output) {
        output.set(input.contains(7) ? *input[7] : 0);
    });
    const auto generic_dictionary = bg::graph("name"_n = "generic_dictionary", [&](bg::wp<> input) {
        return observe_dictionary(input.as<bg::TSD<bg::i64, bg::TS<bg::i64>>>());
    });
    const auto generic_dictionary_result = bg::testing::test_graph(
        generic_dictionary, bg::testing::input<bg::TSD<bg::i64, bg::TS<bg::i64>>>({{{7, 40}}}));

    CHECK(typed_result.equals<bg::i64>({1, 2}));
    CHECK(runtime_result.equals<bg::i64>({3, 4}));
    CHECK(multiple_result.equals<bg::i64>({11, 22}));
    CHECK(raw_multiple_result.equals<bg::i64>({11, 22}));
    CHECK(configured_raw_multiple_result.equals<bg::i64>({11, 22}));
    REQUIRE(configured_profiler.root_graph_id());
    CHECK(annotated_first_result.equals<bg::i64>({11, 22}));
    CHECK(annotated_second_result.equals<bg::i64>({11, 22}));
    CHECK(empty_first_result.equals<bg::i64>({30}));
    CHECK(configured_empty_first_result.equals<bg::i64>({30}));
    CHECK(generic_dictionary_result.equals<bg::i64>({40}));
    CHECK_THROWS_AS((bg::testing::test_graph(identity, {1, 2})), std::invalid_argument);
    CHECK_THROWS_AS((bg::testing::test_graph(identity, {1, 2}, {3, 4})), std::invalid_argument);
}

TEST_CASE("test_graph accepts raw nested scalar list literals",
          "[testing][evaluation][feature:test-graph][series:value][scalar:list]") {
    const auto observe = bg::node([](bg::In<bg::TS<bg::List<bg::i64>>> input,
                                    bg::Out<bg::TS<bg::i64>> output) {
        output.set(static_cast<bg::i64>(input.value_view().size()));
    });
    const auto graph = [&](bg::wp<bg::TS<bg::List<bg::i64>>> input) { return observe(input); };

    const auto actual = bg::testing::test_graph(graph, {{1, 2}, {3, 4, 5}});

    CHECK(actual.equals<bg::i64>({2, 3}));
}

TEST_CASE("test_graph accepts fixed time-series list literals",
          "[testing][evaluation][feature:test-graph][series:fixed-tsl][scalar:integer]") {
    const auto observe = bg::node([](bg::In<bg::TSL<bg::TS<bg::i64>, 2>> input,
                                    bg::Out<bg::TS<bg::i64>> output) {
        output.set(*input[0] + *input[1]);
    });
    const auto graph = [&](bg::wp<bg::TSL<bg::TS<bg::i64>, 2>> input) { return observe(input); };

    const auto actual = bg::testing::test_graph(graph, {{{0, 1}, {1, 10}}, {{0, 2}, {1, 20}}});

    CHECK(actual.equals<bg::i64>({11, 22}));
}

TEST_CASE("test_graph runs literal sources when every tick is skipped",
          "[testing][evaluation][feature:test-graph][series:value]") {
    const auto identity = [](bg::wp<bg::TS<bg::i64>> input) { return input; };

    const auto actual = bg::testing::test_graph(identity, {literal::SKIP, literal::SKIP, literal::SKIP});

    CHECK(actual.equals<bg::i64>({std::nullopt, std::nullopt, std::nullopt}));
}

TEST_CASE("test_graph rejects tagged SKIP as an incompatible scalar payload",
          "[testing][evaluation][feature:test-graph][series:value]") {
    const auto identity = [](bg::wp<bg::TS<bg::i64>> input) { return input; };

    CHECK_THROWS_AS(
        (bg::testing::test_graph(identity, {1, (literal::SKIP)(literal::ScalarLiteral{bg::i64{2}})})),
        std::invalid_argument);
}

TEST_CASE("test_graph accepts empty raw inputs for functions and wiring templates",
          "[testing][evaluation][feature:test-graph][series:value][scalar:list]") {
    const auto count_items = bg::node([](bg::In<bg::TS<bg::List<bg::i64>>> input,
                                         bg::Out<bg::TS<bg::i64>> output) {
        output.set(static_cast<bg::i64>(input.value_view().size()));
    });
    const auto function = [&](bg::wp<bg::TS<bg::List<bg::i64>>> input) { return count_items(input); };

    CHECK(bg::testing::test_graph(function, {}).equals<bg::i64>({std::nullopt}));
    CHECK(bg::testing::test_graph(function, {{}}).equals<bg::i64>({0}));
    CHECK(bg::testing::test_graph(count_items, {}).equals<bg::i64>({std::nullopt}));
    CHECK(bg::testing::test_graph(count_items, {{}}).equals<bg::i64>({0}));
    CHECK(bg::testing::test_graph(function, bg::GraphRunOptions{}, {}).equals<bg::i64>({std::nullopt}));
    CHECK(bg::testing::test_graph(function, bg::GraphRunOptions{}, {{}}).equals<bg::i64>({0}));
    CHECK(bg::testing::test_graph(count_items, bg::GraphRunOptions{}, {}).equals<bg::i64>({std::nullopt}));
    CHECK(bg::testing::test_graph(count_items, bg::GraphRunOptions{}, {{}}).equals<bg::i64>({0}));
}

TEST_CASE("test_graph literal inputs coerce values and revalidate after INVALID",
          "[testing][evaluation][feature:test-graph][transition:invalidation][transition:revalidation]") {
    const auto float_identity = [](bg::wp<bg::TS<float>> input) { return input; };
    const auto float_result = bg::testing::test_graph(float_identity, {1, 2});
    const auto integer_identity = [](bg::wp<bg::TS<bg::i64>> input) { return input; };
    const auto integer_result =
        bg::testing::test_graph(integer_identity, {1, literal::INVALID, 3});
    const auto configured_integer_result =
        bg::testing::test_graph(integer_identity, bg::GraphRunOptions{}, {1, std::nullopt, 3});

    CHECK(float_result.equals<float>({1.0F, 2.0F}));
    CHECK(integer_result.equals<bg::i64>({1, std::nullopt, 3}));
    CHECK(configured_integer_result.equals<bg::i64>({1, std::nullopt, 3}));
}

TEST_CASE("test_graph accepts descriptor spans with more than four inputs",
          "[testing][evaluation][feature:test-graph][declaration:generic][series:value]") {
    using bg::testing::TestGraphLiteralInput;

    const auto fifth = bg::graph(
        "name"_n = "fifth",
        [](bg::wp<bg::TS<bg::i64>>, bg::wp<bg::TS<bg::i64>>, bg::wp<bg::TS<bg::i64>>,
           bg::wp<bg::TS<bg::i64>>, bg::wp<bg::TS<bg::i64>> last) { return last; });
    const auto actual = bg::testing::test_graph(
        fifth, std::span<const TestGraphLiteralInput>{{{1}, {2}, {3}, {4}, {5}}});
    const auto configured_actual = bg::testing::test_graph(
        fifth, bg::GraphRunOptions{}, TestGraphLiteralInput{1}, TestGraphLiteralInput{2}, TestGraphLiteralInput{3},
        TestGraphLiteralInput{4}, TestGraphLiteralInput{5});

    CHECK(actual.equals<bg::i64>({5}));
    CHECK(configured_actual.equals<bg::i64>({5}));
}

TEST_CASE("test_graph preserves zero-input wiring template option calls",
          "[testing][evaluation][feature:test-graph][feature:profiling]") {
    const auto produce = bg::node([](bg::Out<bg::TS<bg::i64>> output) { output.set(42); });
    const auto zero_input = bg::graph("name"_n = "zero_input", [&] { return produce(); });
    const auto services = bg::ServiceCatalogue{};
    auto profiler = bg::EvaluationProfiler{};
    auto services_profiler = bg::EvaluationProfiler{};

    CHECK(bg::testing::test_graph(zero_input).equals<bg::i64>({42}));
    CHECK(bg::testing::test_graph(zero_input, bg::GraphRunOptions{.services = &services})
              .equals<bg::i64>({42}));
    CHECK(bg::testing::test_graph(zero_input, bg::GraphRunOptions{.recorder = profiler.recorder()})
              .equals<bg::i64>({42}));
    CHECK(bg::testing::test_graph(zero_input, bg::GraphRunOptions{.services = &services,
                                                               .recorder = services_profiler.recorder()})
              .equals<bg::i64>({42}));
    REQUIRE(profiler.root_graph_id());
    CHECK(profiler.graph(*profiler.root_graph_id()) != nullptr);
}

struct global_state_test_source {
    bg::Out<bg::TS<bg::i64>> output;

    void start(bg::EvaluationContext *context) const {
        const auto value = bg::ScalarValue{42};
        context->global_state().write([&](auto &state) { state.set(bg::util::symbol{"test_graph"}, value); });
        output.set(1);
    }

    void evaluate() const {}
};

TEST_CASE("test_graph uses the supplied global state",
          "[testing][evaluation][feature:test-graph][feature:global-state]") {
    const auto source = bg::node<global_state_test_source>();
    const auto graph = [&] { return source(); };
    auto global_state = bg::GlobalState{};

    const auto actual = bg::testing::test_graph(graph, bg::GraphRunOptions{.global_state = &global_state});

    CHECK(actual.equals<bg::i64>({1}));
    global_state.read([](const auto &values) {
        CHECK(*bg::scalar_cast<bg::i64>(values.at("test_graph")) == 42);
    });
}

TEST_CASE("test_graph accepts literal inputs with profiler and services",
          "[testing][evaluation][feature:test-graph][feature:profiling]") {
    const auto observe = bg::node([](bg::In<bg::TSD<bg::i64, bg::TS<bg::i64>>> input,
                                    bg::Out<bg::TS<bg::i64>> output) {
        output.set(input.contains(7) ? *input[7] : 0);
    });
    const auto graph = [&](bg::wp<bg::TSD<bg::i64, bg::TS<bg::i64>>> input) { return observe(input); };
    auto profiler = bg::EvaluationProfiler{};
    auto services = bg::ServiceCatalogue{};

    const auto actual = bg::testing::test_graph(
        graph,
        bg::GraphRunOptions{.services = &services, .recorder = profiler.recorder()},
        {{{7, 10}}, {{7, 20}}}
    );

    CHECK(actual.equals<bg::i64>({10, 20}));
    REQUIRE(profiler.root_graph_id());
    CHECK(profiler.graph(*profiler.root_graph_id()) != nullptr);
}

struct Increment {
    bg::In<bg::TS<bg::i64>> input;
    int amount;
    bg::Out<bg::TS<bg::i64>> output;

    void evaluate() const { output.set(*input + amount); }
};

const auto increment = bg::node<Increment>();

[[nodiscard]] bg::wp<bg::TS<bg::i64>> operator+(bg::wp<bg::TS<bg::i64>> input, int amount) {
    return increment(input, amount);
}

TEST_CASE("test_graph accepts a graph function and reports skipped cycles",
          "[testing][evaluation][feature:test-graph][definition:function][series:value][scalar:integer]") {
    const auto graph = [](bg::wp<bg::TS<bg::i64>> input) { return input + 1; };

    CHECK(bg::testing::test_graph(graph, {1, 2, std::nullopt, 4}).equals({2, 3, std::nullopt, 5}));
}

TEST_CASE("test_graph captures a runtime-typed output",
          "[testing][evaluation][feature:test-graph][declaration:generic][series:value]") {
    const auto graph = [](bg::wp<bg::TS<bg::i64>> input) { return bg::wp<>{input}; };

    CHECK(bg::testing::test_graph(graph, {1, 2, std::nullopt, 4}).equals<bg::i64>({1, 2, std::nullopt, 4}));
}

TEST_CASE("test_graph accepts runtime-typed inputs",
          "[testing][evaluation][feature:test-graph][declaration:generic][series:value]") {
    const auto identity =
        bg::graph("name"_n = "identity", [](bg::wp<bg::TS<bg::i64>> input) { return input; });
    auto ticks = std::vector<std::optional<bg::ScalarValue<>>>{};
    ticks.emplace_back(std::in_place, bg::i64{1});
    ticks.emplace_back(std::nullopt);
    ticks.emplace_back(std::in_place, bg::i64{3});
    const auto inputs = std::array{
        bg::testing::TestGraphInput{bg::cpp_scalar_runtime_type<bg::i64>(), ticks},
    };

    const auto actual = bg::testing::test_graph(identity, std::span<const bg::testing::TestGraphInput>{inputs});

    CHECK(actual.equals<bg::i64>({1, std::nullopt, 3}));
}

TEST_CASE("runtime-typed test_graph inputs can contain only skipped ticks",
          "[testing][evaluation][feature:test-graph][declaration:generic][series:value]") {
    const auto identity =
        bg::graph("name"_n = "identity", [](bg::wp<bg::TS<bg::i64>> input) { return input; });
    const auto ticks = std::vector<std::optional<bg::ScalarValue<>>>(3);
    const auto inputs = std::array{
        bg::testing::TestGraphInput{bg::cpp_scalar_runtime_type<bg::i64>(), ticks},
    };

    const auto actual = bg::testing::test_graph(identity, std::span<const bg::testing::TestGraphInput>{inputs});

    CHECK(actual.equals<bg::i64>({std::nullopt, std::nullopt, std::nullopt}));
}

TEST_CASE("wiring converts C++ string arguments to the canonical graph type",
          "[testing][evaluation][feature:test-graph][definition:function][series:value][scalar:string]") {
    const auto publish = bg::node(
        [](bg::In<bg::TS<bg::i64>>, std::string value, bg::Out<bg::TS<bg::i64>> output) {
            output.set(static_cast<int>(value.size()));
        });
    const auto graph = [&](bg::wp<bg::TS<bg::i64>> input) { return publish(input, std::string{"text"}); };

    CHECK(bg::testing::test_graph(graph, {1}).equals<bg::i64>({4}));
}

TEST_CASE("wiring converts optional C++ scalar arguments to the canonical graph type",
          "[testing][evaluation][feature:test-graph][definition:function][series:value][scalar:integer]") {
    const auto add = bg::node(
        [](bg::In<bg::TS<bg::i64>> input, std::optional<int> amount, bg::Out<bg::TS<bg::i64>> output) {
            output.set(*input + amount.value_or(0));
        });
    const auto graph = [&](bg::wp<bg::TS<bg::i64>> input) { return add(input, std::optional<int>{2}); };

    CHECK(bg::testing::test_graph(graph, {1, 2}).equals<bg::i64>({3, 4}));
}

} // namespace
