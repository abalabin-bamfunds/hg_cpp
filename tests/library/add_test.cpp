#include <bg/graph.hpp>
#include <bg/graph/compiled_graph.hpp>
#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/library.hpp>
#include <bg/node.hpp>
#include <bg/nested.hpp>
#include <bg/testing/test_graph.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <optional>
#include <vector>

using namespace bg;
using namespace bg::literals;

TEST_CASE("scalar addition creates a constant graph source",
          "[feature:operator][shape:source][scalar:floating-point][guarantee:const]") {
    const auto graph = [] {
        const auto result = operators::add("lhs"_n = 1.5, "rhs"_n = 2.5);
        CHECK(&result.type() == &TS<double, TimeSeriesGuarantee::const_>::type());
        return result;
    };

    const auto actual = testing::test_graph(graph);

    CHECK(actual.equals<double>({4.0}));

    auto wiring = GraphWiring{};
    {
        auto context = WiringContext{wiring};
        (void)operators::add(1.5, 2.5);
    }

    wiring.finalize();
    const auto compiled = CompiledGraph{wiring};
    CHECK(wiring.nodes().size() == 1);
    CHECK(compiled.node_count() == 0);
}

TEST_CASE("addition accepts scalar and time-series operands in either order",
          "[feature:operator][series:value][scalar:floating-point][shape:single-node]") {
    const auto scalar_left = [](wp<TS<double>> value) { return 2.0 + value; };
    const auto scalar_right = [](wp<TS<double>> value) { return value + 2.0; };

    CHECK(testing::test_graph(scalar_left, std::array{1.0, 2.0}).equals<double>({3.0, 4.0}));
    CHECK(testing::test_graph(scalar_right, std::array{1.0, 2.0}).equals<double>({3.0, 4.0}));
}

TEST_CASE("addition does not convert an integer scalar to double",
          "[feature:operator][series:value][scalar:integer][scalar:floating-point]") {
    const auto graph = [](wp<TS<double>> value) { return value + 1; };
    const auto direct = [](wp<TS<double>> value) { return operators::add(value, 1); };

    CHECK_THROWS_AS(testing::test_graph(graph, std::array{2.0}), WiringError);
    CHECK_THROWS_AS(testing::test_graph(direct, std::array{2.0}), WiringError);
}

TEST_CASE("addition rejects optional inputs",
          "[feature:operator][feature:validity][validity:optional][series:value][scalar:floating-point]") {
    const auto direct = [](wp<TS<double>> value) { return operators::add(opt(value), 1.0); };
    const auto graph = [](wp<TS<double>> value) { return opt(value) + 1.0; };
    const auto reflected = [](wp<TS<double>> value) { return 1.0 + opt(value); };
    const auto input = std::array{2.0};

    CHECK_THROWS_AS(testing::test_graph(direct, input), WiringError);
    CHECK_THROWS_AS(testing::test_graph(graph, input), WiringError);
    CHECK_THROWS_AS(testing::test_graph(reflected, input), WiringError);
}

TEST_CASE("addition preserves the shared guarantee for direct time-series inputs",
          "[feature:operator][feature:type-system][series:value][guarantee:ordinary]"
          "[guarantee:deduplicated][guarantee:final][guarantee:const]") {
    struct deduplicated_source {
        In<TS<double>> input;
        Out<TS<double, TimeSeriesGuarantee::deduplicated>> output;

        void evaluate() const { output.set(*input); }
    };
    struct final_scalar_source {
        Scalar<double> value;
        Out<TS<double, TimeSeriesGuarantee::final>> output;

        void start() const { output.set(*value); }
    };

    const auto ordinary = [](wp<TS<double, TimeSeriesGuarantee::ordinary>> lhs,
                             wp<TS<double, TimeSeriesGuarantee::ordinary>> rhs) {
        const auto result = operators::add(lhs, rhs);
        CHECK(&result.type() == &TS<double, TimeSeriesGuarantee::ordinary>::type());
        return result;
    };
    const auto deduplicate = node<deduplicated_source>();
    const auto deduplicated = [&](wp<TS<double>> lhs, wp<TS<double>> rhs) {
        const auto result = operators::add(deduplicate(lhs), deduplicate(rhs));
        CHECK(&result.type() == &TS<double, TimeSeriesGuarantee::deduplicated>::type());
        return result;
    };
    const auto scalar_left_deduplicated = [&](wp<TS<double>> value) {
        const auto result = operators::add(2.0, deduplicate(value));
        CHECK(&result.type() == &TS<double, TimeSeriesGuarantee::deduplicated>::type());
        return result;
    };
    const auto scalar_right_deduplicated = [&](wp<TS<double>> value) {
        const auto result = operators::add(deduplicate(value), 2.0);
        CHECK(&result.type() == &TS<double, TimeSeriesGuarantee::deduplicated>::type());
        return result;
    };
    const auto final_source = node<final_scalar_source>();
    const auto final = [&] {
        const auto result = operators::add(final_source(2.0), final_source(3.0));
        CHECK(&result.type() == &TS<double, TimeSeriesGuarantee::final>::type());
        return result;
    };
    const auto final_plus_ordinary = [&](wp<TS<double>> ordinary) {
        const auto result = operators::add(final_source(2.0), ordinary);
        CHECK(&result.type() == &TS<double, TimeSeriesGuarantee::ordinary>::type());
        return result;
    };
    const auto final_plus_deduplicated = [&](wp<TS<double>> value) {
        const auto result = operators::add(final_source(2.0), deduplicate(value));
        CHECK(&result.type() == &TS<double, TimeSeriesGuarantee::deduplicated>::type());
        return result;
    };
    const auto constant = [] {
        const auto result = operators::add(operators::const_(2.0), operators::const_(3.0));
        CHECK(&result.type() == &TS<double, TimeSeriesGuarantee::const_>::type());
        return result;
    };

    CHECK(testing::test_graph(ordinary, std::array{1.0, 1.0}, std::array{2.0, 2.0})
              .equals<double>({3.0, 3.0}));
    CHECK(testing::test_graph(deduplicated, std::array{1.0, 2.0, 3.0}, std::array{2.0, 1.0, 1.0})
              .equals<double>({3.0, std::nullopt, 4.0}));
    CHECK(testing::test_graph(scalar_left_deduplicated, std::array{1.0, 2.0})
              .equals<double>({3.0, 4.0}));
    CHECK(testing::test_graph(scalar_right_deduplicated, std::array{1.0, 2.0})
              .equals<double>({3.0, 4.0}));
    CHECK(testing::test_graph(final).equals<double>({5.0}));
    CHECK(testing::test_graph(final_plus_ordinary, std::array{1.0, 2.0}).equals<double>({3.0, 4.0}));
    CHECK(testing::test_graph(final_plus_deduplicated, std::array{1.0, 2.0, 2.0, 3.0})
              .equals<double>({3.0, 4.0, std::nullopt, 5.0}));
    CHECK(testing::test_graph(constant).equals<double>({5.0}));
}

TEST_CASE("required addition invalidates and revalidates with its inputs",
          "[feature:operator][feature:validity][series:value][validity:required]"
          "[transition:invalidation][transition:revalidation]") {
    struct invalidating_source {
        In<TS<double>> input;
        Out<TS<double>> output;

        void evaluate() const {
            if (*input < 0.0) {
                output.invalidate();
                return;
            }

            output.set(*input);
        }
    };
    struct validity_observer {
        In<TS<double>, optional> input;
        Out<TS<bool>> output;

        void evaluate() const { output.set(input.valid()); }
    };

    const auto source = node<invalidating_source>();
    const auto observe = node<validity_observer>();
    const auto graph = [&](wp<TS<double>> lhs, wp<TS<double>> rhs) {
        return observe(operators::add(source(lhs), source(rhs)));
    };

    const auto actual = testing::test_graph(graph, std::array{1.0, -1.0, 3.0}, std::array{10.0, 10.0, 10.0});

    CHECK(actual.equals<bool>({true, false, true}));
}

TEST_CASE("addition accepts reference inputs with ordinary target guarantees",
          "[feature:operator][feature:reference][series:value][guarantee:ordinary]"
          "[binding:reference-to-value][transition:update]") {
    struct changing_reference {
        In<TS<bool>> select_right;
        In<REF<TS<double>>> lhs;
        In<REF<TS<double>>> rhs;
        Out<REF<TS<double>>> output;

        void evaluate() const { output.copy(*select_right ? rhs : lhs); }
    };

    const auto select = node<changing_reference>();
    const auto graph = [&](wp<TS<double>> lhs, wp<TS<double>> rhs, wp<TS<bool>> select_right) {
        const auto reference = select(select_right, lhs, rhs);
        return operators::add(reference, 1.0);
    };

    const auto actual = testing::test_graph(
        graph, std::array{10.0, 11.0, 12.0}, std::array{20.0, 21.0, 22.0}, std::array{false, true, false});

    CHECK(actual.equals<double>({11.0, 22.0, 13.0}));
}

TEST_CASE("addition runs inside a nested graph",
          "[feature:operator][feature:nested-graph][shape:nested][series:value][scalar:floating-point]") {
    const auto inner = graph(
        "name"_n = "inner_add", [](wp<TS<double>> lhs, wp<TS<double>> rhs) {
            return wp<TS<double>>{lhs + rhs};
        });
    const auto outer = [&](wp<TS<double>> lhs, wp<TS<double>> rhs, wp<TS<bool>> run) {
        return nested(inner, run, lhs, rhs);
    };

    CHECK(testing::test_graph(outer, std::array{1.0, 2.0}, std::array{10.0, 20.0}, std::array{true, true})
              .equals<double>({11.0, 22.0}));
}

TEST_CASE("an invalid input does not make addition publish at startup",
          "[feature:operator][feature:validity][shape:single-node][validity:required]"
          "[transition:first-tick]") {
    const auto graph = [](wp<TS<double>> lhs, wp<TS<double>> rhs) {
        return operators::add(lhs, rhs);
    };
    const auto lhs = std::vector<std::optional<double>>{std::nullopt, 1.0};
    const auto rhs = std::vector<std::optional<double>>{2.0, 2.0};

    CHECK(testing::test_graph(graph, lhs, rhs).equals<double>({std::nullopt, 3.0}));
}
