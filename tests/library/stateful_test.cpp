#include <bg/graph.hpp>
#include <bg/library.hpp>
#include <bg/nested.hpp>
#include <bg/simulation_evaluation_engine.hpp>
#include <bg/testing/test_graph.hpp>

#include <catch2/catch_test_macros.hpp>

#include "../utils/counting_memory_resource.hpp"

#include <array>
#include <chrono>
#include <string>
#include <string_view>

using namespace bg;
using namespace bg::literals;

TEST_CASE("count counts valid input ticks and resets after invalidation",
          "[feature:operator][series:value][scalar:i64][guarantee:ordinary]"
          "[transition:first-tick][transition:update][transition:invalidation][transition:revalidation]") {
    const auto count = [](wp<TS<i64>> value) { return operators::count(value); };

    CHECK(testing::test_graph(count, {literal::INVALID, literal::SKIP, 7, 7,
                                      literal::INVALID, literal::SKIP, 9,
                                      literal::INVALID, 11})
              .equals<i64>({std::nullopt, std::nullopt, 1, 2, std::nullopt, std::nullopt, 1,
                            std::nullopt, 1}));
}

TEST_CASE("count propagates a const guarantee",
          "[feature:operator][series:value][scalar:i64][guarantee:const][transition:first-tick]") {
    const auto graph = [] {
        const auto result = operators::count(operators::const_(i64{7}));
        CHECK(&result.type() == &TS<i64, TimeSeriesGuarantee::const_>::type());
        return result;
    };

    CHECK(testing::test_graph(graph).equals<i64>({1}));
}

TEST_CASE("count counts updates to a collection root",
          "[feature:operator][series:tss][scalar:i64][guarantee:ordinary][transition:update]") {
    const auto count = [](wp<TSS<i64>> value) { return operators::count(value); };

    CHECK(testing::test_graph(count, {{1}, {1, 2}, {1, 2, 3}}).equals<i64>({1, 2, 3}));
}

TEST_CASE("count preserves a final guarantee",
          "[feature:operator][series:value][scalar:i64][guarantee:final][transition:first-tick]") {
    const auto count = [](wp<TS<i64, TimeSeriesGuarantee::final>> value) {
        const auto result = operators::count(value);
        CHECK(&result.type() == &TS<i64, TimeSeriesGuarantee::final>::type());
        return result;
    };

    CHECK(testing::test_graph(count, {literal::SKIP, 7}).equals<i64>({std::nullopt, 1}));
}

TEST_CASE("count preserves a deduplicated guarantee",
          "[feature:operator][series:value][scalar:i64][guarantee:deduplicated][transition:update]") {
    const auto count = [](wp<TS<i64, TimeSeriesGuarantee::deduplicated>> value) {
        const auto result = operators::count(value);
        CHECK(&result.type() == &TS<i64, TimeSeriesGuarantee::deduplicated>::type());
        return result;
    };

    CHECK(testing::test_graph(
              count, testing::input<TS<i64, TimeSeriesGuarantee::deduplicated>>({1, 1, 2}))
              .equals<i64>({1, std::nullopt, 2}));
}

TEST_CASE("count observes retargeted references as value updates",
          "[feature:operator][series:value][series:reference][scalar:i64][guarantee:ordinary]"
          "[binding:reference-to-value][transition:update]") {
    const auto count_selected = [](wp<TS<bool>> condition) {
        const auto selected = operators::if_then_else(condition, operators::const_(i64{10}),
                                                       operators::const_(i64{20}));
        const auto result = operators::count(selected);
        CHECK(&result.type() == &TS<i64>::type());
        return result;
    };

    CHECK(testing::test_graph(count_selected, std::array{false, true, false}).equals<i64>({1, 2, 3}));
}

TEST_CASE("count and lag observe target updates through an unchanged reference",
          "[feature:operator][feature:state][series:value][series:reference][scalar:i64]"
          "[guarantee:ordinary][binding:reference-to-value][transition:update]") {
    const auto count_selected = [](wp<TS<i64>> value) {
        const auto selected = operators::if_then_else(operators::const_(true), value, operators::const_(20));
        return operators::count(selected);
    };
    const auto lag_selected = [](wp<TS<i64>> value) {
        const auto selected = operators::if_then_else(operators::const_(true), value, operators::const_(20));
        return operators::lag(selected);
    };
    const auto inputs = std::array<i64, 3>{10, 11, 12};

    CHECK(testing::test_graph(count_selected, inputs).equals<i64>({1, 2, 3}));
    CHECK(testing::test_graph(lag_selected, inputs).equals<i64>({std::nullopt, 10, 11}));
}

TEST_CASE("lag delays values and invalidations by one input event",
          "[feature:operator][feature:state][series:value][scalar:i64][guarantee:ordinary]"
          "[validity:optional][transition:first-tick][transition:update][transition:invalidation]"
          "[transition:revalidation]") {
    const auto lag = [](wp<TS<i64>> value) { return operators::lag(value); };

    CHECK(testing::test_graph(lag, {literal::SKIP, 10, 20, literal::INVALID,
                                    literal::SKIP, 30, literal::INVALID, 40})
              .equals<i64>({std::nullopt, std::nullopt, 10, 20, std::nullopt, std::nullopt, 30,
                            std::nullopt}));
}

TEST_CASE("lag delays invalidation until the next input event",
          "[feature:operator][feature:state][series:value][scalar:i64][validity:optional]"
          "[transition:update][transition:invalidation][transition:revalidation]") {
    const auto observe = [](wp<TS<i64>> value) { return operators::valid(operators::lag(value)); };

    CHECK(testing::test_graph(observe, {10, 20, literal::INVALID, 30, 40})
              .equals<bool>({false, true, std::nullopt, false, true}));
}

TEST_CASE("lag publishes repeated equal values and owns scalar snapshots",
          "[feature:operator][feature:state][series:value][scalar:string][guarantee:ordinary]"
          "[validity:optional][transition:update]") {
    const auto lag = [](wp<TS<string>> value) { return operators::lag(value); };

    CHECK(testing::test_graph(lag, std::array<std::string, 3>{"same", "same", "next"})
              .equals<string>({std::nullopt, "same", "same"}));
}

TEST_CASE("lag retains owned collection scalar snapshots",
          "[feature:operator][feature:state][series:value][scalar:dict][scalar:string]"
          "[guarantee:ordinary][validity:optional][transition:update]") {
    const auto lag = [](wp<TS<Dict<i64, string>>> value) { return operators::lag(value); };
    const auto actual = testing::test_graph(lag, {{{1, "old"}}, {{1, "new"}}, {{2, "last"}}});

    CHECK_FALSE(actual[0]);
    REQUIRE(actual[1]);
    CHECK(scalar_cast<Dict<i64, string>>(actual[1]->view()).at(1).value() == "old");
    REQUIRE(actual[2]);
    CHECK(scalar_cast<Dict<i64, string>>(actual[2]->view()).at(1).value() == "new");
}

TEST_CASE("lag releases runtime-resolved collection state at runtime graph teardown",
          "[feature:operator][feature:state][series:value][scalar:dict][scalar:string][allocation]"
          "[transition:stop]") {
    const auto graph = [] {
        const auto values = ScalarValue<Dict<i64, string>>{
            std::array{std::pair{i64{1}, std::string_view{"a retained string"}}}};
        return operators::lag(operators::const_(values));
    };
    auto wiring = GraphWiring{};
    {
        auto context = WiringContext{wiring};
        static_cast<void>(graph());
    }
    wiring.finalize();
    const auto compiled = CompiledGraph{wiring};
    auto memory = counting_memory_resource{};
    auto global_state = GlobalState{allocator{memory}};
    const auto allocations_before = memory.allocations();

    {
        auto runtime = RuntimeGraph{compiled, allocator{memory}, global_state};
        const auto allocations_after_construction = memory.allocations();
        SimulationEvaluationEngine{}.run(runtime, MIN_DT + std::chrono::microseconds{1});
        CHECK(memory.allocations() > allocations_after_construction);
    }

    CHECK(memory.allocations() == allocations_before);
}

TEST_CASE("i64 lag uses its resolved scalar size in packed runtime state",
          "[feature:operator][feature:state][series:value][scalar:i64][allocation]") {
    const auto lag = [] { return operators::lag(operators::const_(i64{7})); };
    auto wiring = GraphWiring{};
    {
        auto context = WiringContext{wiring};
        static_cast<void>(lag());
    }
    wiring.finalize();
    const auto compiled = CompiledGraph{wiring};

    const CompiledNode *lag_node = nullptr;
    for (auto index = std::size_t{}; index < compiled.node_count(); ++index) {
        const auto &candidate = compiled.node(index);
        if (candidate.has_state() && candidate.parameters().size() == 1) {
            lag_node = &candidate;
            break;
        }
    }

    REQUIRE(lag_node != nullptr);
    CHECK(&lag_node->generic_state_runtime_type(0).type() == &int64_type());
    CHECK(lag_node->generic_state_runtime_type(0).size() == sizeof(i64));
    CHECK(lag_node->state_field_offset(0) == 0);
    CHECK(lag_node->state_field_offset(1) == sizeof(i64));
    CHECK(lag_node->state_offset() % alignof(i64) == 0);
    CHECK(lag_node->state_offset() + sizeof(i64) + sizeof(bool) <= compiled.runtime_storage_size());

    auto memory = counting_memory_resource{};
    auto global_state = GlobalState{allocator{memory}};
    const auto allocations_before = memory.allocations();
    {
        auto runtime = RuntimeGraph{compiled, allocator{memory}, global_state};
        const auto allocations_after_construction = memory.allocations();
        SimulationEvaluationEngine{}.run(runtime, MIN_DT + std::chrono::microseconds{1});
        CHECK(memory.allocations() == allocations_after_construction);
    }
    CHECK(memory.allocations() == allocations_before);
}

TEST_CASE("lag follows retargeted value references",
          "[feature:operator][feature:state][series:value][series:reference][scalar:i64]"
          "[guarantee:ordinary][binding:reference-to-value][transition:update]") {
    const auto lag_selected = [](wp<TS<bool>> condition) {
        const auto selected = operators::if_then_else(condition, operators::const_(i64{10}),
                                                       operators::const_(i64{20}));
        return operators::lag(selected);
    };

    CHECK(testing::test_graph(lag_selected, std::array{true, true, false, true})
              .equals<i64>({std::nullopt, std::nullopt, 10, 20}));
}

TEST_CASE("lag keeps an ordinary output for a const input",
          "[feature:operator][feature:state][series:value][scalar:i64][guarantee:const]"
          "[transition:first-tick]") {
    const auto graph = [] {
        const auto result = operators::lag(operators::const_(i64{7}));
        CHECK(&result.type() == &TS<i64>::type());
        return result;
    };

    CHECK(testing::test_graph(graph).equals<i64>({std::nullopt}));
}

TEST_CASE("lag state is independent across nested graph instances",
          "[feature:operator][feature:state][feature:nested-graph][series:value][scalar:i64]"
          "[guarantee:ordinary][shape:nested]") {
    const auto delayed = graph("name"_n = "delayed_value", [](wp<TS<i64>> value) {
        return wp<TS<i64>>{operators::lag(value)};
    });
    const auto combine = [&](wp<TS<i64>> lhs, wp<TS<i64>> rhs, wp<TS<bool>> run) {
        return operators::add(nested(delayed, run, lhs), nested(delayed, run, rhs));
    };

    CHECK(testing::test_graph(combine, std::array<i64, 3>{1, 2, 3}, std::array<i64, 3>{10, 20, 30},
                              std::array{true, true, true})
              .equals<i64>({std::nullopt, 11, 22}));
}

TEST_CASE("runtime-resolved lag state is independent across nested graph instances",
          "[feature:operator][feature:state][feature:nested-graph][series:value][scalar:string]"
          "[guarantee:ordinary][shape:nested]") {
    const auto delayed = graph("name"_n = "delayed_text", [](wp<TS<string>> value) {
        return wp<TS<string>>{operators::lag(value)};
    });
    const auto combine = [&](wp<TS<string>> lhs, wp<TS<string>> rhs, wp<TS<bool>> run) {
        return operators::add(nested(delayed, run, lhs), nested(delayed, run, rhs));
    };

    CHECK(testing::test_graph(combine, std::array<std::string, 2>{"left", "next"},
                              std::array<std::string, 2>{"right", "next"}, std::array{true, true})
              .equals<string>({std::nullopt, "leftright"}));
}
