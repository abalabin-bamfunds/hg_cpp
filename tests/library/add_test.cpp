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

namespace {

using namespace bg::literals;

struct deduplicated_source {
    bg::In<bg::TS<double>> input;
    bg::Out<bg::TS<double, bg::TimeSeriesGuarantee::deduplicated>> output;

    void evaluate() const { output.set(*input); }
};

struct invalidating_source {
    bg::In<bg::TS<double>> input;
    bg::Out<bg::TS<double>> output;

    void evaluate() const {
        if (*input < 0.0) {
            output.invalidate();
            return;
        }

        output.set(*input);
    }
};

struct validity_observer {
    bg::In<bg::TS<double>, bg::optional> input;
    bg::Out<bg::TS<bool>> output;

    void evaluate() const { output.set(input.valid()); }
};

struct final_scalar_source {
    bg::Scalar<double> value;
    bg::Out<bg::TS<double, bg::TimeSeriesGuarantee::final>> output;

    void start() const { output.set(*value); }
};

struct changing_reference {
    bg::In<bg::TS<bool>> select_right;
    bg::In<bg::REF<bg::TS<double>>> lhs;
    bg::In<bg::REF<bg::TS<double>>> rhs;
    bg::Out<bg::REF<bg::TS<double>>> output;

    void evaluate() const { output.copy(*select_right ? rhs : lhs); }
};

} // namespace

TEST_CASE("scalar addition creates a constant graph source",
          "[feature:operator][shape:source][scalar:floating-point][guarantee:const]") {
    const auto graph = [] {
        const auto result = bg::operators::add("lhs"_n = 1.5, "rhs"_n = 2.5);
        CHECK(&result.type() == &bg::TS<double, bg::TimeSeriesGuarantee::const_>::type());
        return result;
    };

    const auto actual = bg::testing::test_graph(graph);

    CHECK(actual.equals<double>({4.0}));

    auto wiring = bg::GraphWiring{};
    {
        auto context = bg::WiringContext{wiring};
        (void)bg::operators::add(1.5, 2.5);
    }

    wiring.finalize();
    const auto compiled = bg::CompiledGraph{wiring};
    CHECK(wiring.nodes().size() == 1);
    CHECK(compiled.node_count() == 0);
}

TEST_CASE("addition accepts scalar and time-series operands in either order",
          "[feature:operator][series:value][scalar:floating-point][shape:single-node]") {
    const auto scalar_left = [](bg::wp<bg::TS<double>> value) { return 2.0 + value; };
    const auto scalar_right = [](bg::wp<bg::TS<double>> value) { return value + 2.0; };

    CHECK(bg::testing::test_graph(scalar_left, std::array{1.0, 2.0}).equals<double>({3.0, 4.0}));
    CHECK(bg::testing::test_graph(scalar_right, std::array{1.0, 2.0}).equals<double>({3.0, 4.0}));
}

TEST_CASE("addition does not convert an integer scalar to double",
          "[feature:operator][series:value][scalar:integer][scalar:floating-point]") {
    const auto graph = [](bg::wp<bg::TS<double>> value) { return value + 1; };
    const auto direct = [](bg::wp<bg::TS<double>> value) { return bg::operators::add(value, 1); };

    CHECK_THROWS_AS(bg::testing::test_graph(graph, std::array{2.0}), bg::WiringError);
    CHECK_THROWS_AS(bg::testing::test_graph(direct, std::array{2.0}), bg::WiringError);
}

TEST_CASE("addition preserves the shared guarantee for direct time-series inputs",
          "[feature:operator][feature:type-system][series:value][guarantee:ordinary]"
          "[guarantee:deduplicated][guarantee:final][guarantee:const]") {
    const auto ordinary = [](bg::wp<bg::TS<double, bg::TimeSeriesGuarantee::ordinary>> lhs,
                             bg::wp<bg::TS<double, bg::TimeSeriesGuarantee::ordinary>> rhs) {
        const auto result = bg::operators::add(lhs, rhs);
        CHECK(&result.type() == &bg::TS<double, bg::TimeSeriesGuarantee::ordinary>::type());
        return result;
    };
    const auto deduplicate = bg::node<deduplicated_source>();
    const auto deduplicated = [&](bg::wp<bg::TS<double>> lhs, bg::wp<bg::TS<double>> rhs) {
        const auto result = bg::operators::add(deduplicate(lhs), deduplicate(rhs));
        CHECK(&result.type() == &bg::TS<double, bg::TimeSeriesGuarantee::deduplicated>::type());
        return result;
    };
    const auto final_source = bg::node<final_scalar_source>();
    const auto final = [&] {
        const auto result = bg::operators::add(final_source(2.0), final_source(3.0));
        CHECK(&result.type() == &bg::TS<double, bg::TimeSeriesGuarantee::final>::type());
        return result;
    };
    const auto constant = [] {
        const auto result = bg::operators::add(bg::operators::const_(2.0), bg::operators::const_(3.0));
        CHECK(&result.type() == &bg::TS<double, bg::TimeSeriesGuarantee::const_>::type());
        return result;
    };

    CHECK(bg::testing::test_graph(ordinary, std::array{1.0, 1.0}, std::array{2.0, 2.0})
              .equals<double>({3.0, 3.0}));
    CHECK(bg::testing::test_graph(deduplicated, std::array{1.0, 2.0, 3.0}, std::array{2.0, 1.0, 1.0})
              .equals<double>({3.0, std::nullopt, 4.0}));
    CHECK(bg::testing::test_graph(final).equals<double>({5.0}));
    CHECK(bg::testing::test_graph(constant).equals<double>({5.0}));
}

TEST_CASE("required addition invalidates and revalidates with its inputs",
          "[feature:operator][feature:validity][series:value][validity:required]"
          "[transition:invalidation][transition:revalidation]") {
    const auto source = bg::node<invalidating_source>();
    const auto observe = bg::node<validity_observer>();
    const auto graph = [&](bg::wp<bg::TS<double>> lhs, bg::wp<bg::TS<double>> rhs) {
        return observe(bg::operators::add(source(lhs), source(rhs)));
    };

    const auto actual = bg::testing::test_graph(graph, std::array{1.0, -1.0, 3.0}, std::array{10.0, 10.0, 10.0});

    CHECK(actual.equals<bool>({true, false, true}));
}

TEST_CASE("addition accepts reference inputs with ordinary target guarantees",
          "[feature:operator][feature:reference][series:value][guarantee:ordinary]"
          "[binding:reference-to-value][transition:update]") {
    const auto select = bg::node<changing_reference>();
    const auto graph = [&](bg::wp<bg::TS<double>> lhs, bg::wp<bg::TS<double>> rhs,
                           bg::wp<bg::TS<bool>> select_right) {
        const auto reference = select(select_right, lhs, rhs);
        return bg::operators::add(reference, 1.0);
    };

    const auto actual = bg::testing::test_graph(graph, std::array{10.0, 11.0, 12.0},
                                                 std::array{20.0, 21.0, 22.0},
                                                 std::array{false, true, false});

    CHECK(actual.equals<double>({11.0, 22.0, 13.0}));
}

TEST_CASE("addition runs inside a nested graph",
          "[feature:operator][feature:nested-graph][shape:nested][series:value][scalar:floating-point]") {
    const auto inner = bg::graph("name"_n = "inner_add", [](bg::wp<bg::TS<double>> lhs,
                                                              bg::wp<bg::TS<double>> rhs) {
        return bg::wp<bg::TS<double>>{lhs + rhs};
    });
    const auto outer = [&](bg::wp<bg::TS<double>> lhs, bg::wp<bg::TS<double>> rhs, bg::wp<bg::TS<bool>> run) {
        return bg::nested(inner, run, lhs, rhs);
    };

    CHECK(bg::testing::test_graph(outer, std::array{1.0, 2.0}, std::array{10.0, 20.0}, std::array{true, true})
              .equals<double>({11.0, 22.0}));
}

TEST_CASE("an invalid input does not make addition publish at startup",
          "[feature:operator][feature:validity][shape:single-node][validity:required]"
          "[transition:first-tick]") {
    const auto graph = [](bg::wp<bg::TS<double>> lhs, bg::wp<bg::TS<double>> rhs) {
        return bg::operators::add(lhs, rhs);
    };
    const auto lhs = std::vector<std::optional<double>>{std::nullopt, 1.0};
    const auto rhs = std::vector<std::optional<double>>{2.0, 2.0};

    CHECK(bg::testing::test_graph(graph, lhs, rhs).equals<double>({std::nullopt, 3.0}));
}
