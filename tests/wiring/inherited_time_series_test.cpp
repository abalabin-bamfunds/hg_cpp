#include <bg/graph.hpp>
#include <bg/graph/compiled_graph.hpp>
#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/nested.hpp>
#include <bg/node.hpp>
#include <bg/testing/test_graph.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <array>
#include <cstdint>
#include <string_view>

using namespace bg;
using namespace bg::literals;

namespace {

struct OrderSource {
    i64 value;
    Out<TS<i64>> output;

    void evaluate() const { output.set(value); }
};

struct OrderConsumer {
    In<TS<i64>> input;

    void evaluate() const { (void)*input; }
};

} // namespace

TEST_CASE("a late inherited provider resolves a root requirement during compilation",
          "[feature:inherited-time-series][feature:wiring]") {
    const auto identity = 0;
    const auto requirement_identity = details::NestedBindingIdentity::object(&identity);
    const auto copy = node([](i64 value) { return value; });

    const auto tested = [&](wp<TS<i64>> input) {
        auto &wiring = WiringContext::current().wiring();
        const auto requirement = wiring.require_inherited_time_series(requirement_identity, util::symbol{"late_input"},
                                                                      TS<i64>::type());
        const auto output = copy(requirement);
        wiring.provide_inherited_time_series(requirement_identity, TS<i64>::type(), input);
        return output;
    };

    CHECK(testing::test_graph(tested, std::array<i64, 1>{7}).equals({7}));
}

TEST_CASE("a late inherited provider crosses a nested graph boundary",
          "[feature:inherited-time-series][feature:nested-graph][feature:wiring]") {
    const auto identity = 0;
    const auto requirement_identity = details::NestedBindingIdentity::object(&identity);
    const auto copy = node([](i64 value) { return value; });

    const auto inner = graph("name"_n = "late_inherited_input", [&] {
        auto &wiring = WiringContext::current().wiring();
        const auto requirement = wiring.require_inherited_time_series(requirement_identity, util::symbol{"late_input"},
                                                                      TS<i64>::type());
        return copy(requirement);
    });

    const auto tested = [&](wp<TS<i64>> input, wp<TS<bool>> run) {
        auto &wiring = WiringContext::current().wiring();
        const auto output = nested(inner, run);
        wiring.provide_inherited_time_series(requirement_identity, TS<i64>::type(), input);
        return output;
    };

    CHECK(testing::test_graph(tested, std::array<i64, 2>{7, 11}, std::array{true, true}).equals({7, 11}));
}

TEST_CASE("a named context requirement cannot be provided after wiring",
          "[feature:inherited-time-series][feature:context-time-series][feature:wiring]") {
    const auto source = node([](Out<TS<i64>> output) { output.set(1); });
    auto wiring = GraphWiring{};
    const auto wiring_context = WiringContext{wiring};
    const auto provider = source();
    const auto identity = details::NestedBindingIdentity::named(util::symbol{"named_input"});
    (void)wiring.require_inherited_time_series(identity, util::symbol{"named_input"}, TS<i64>::type());

    CHECK_THROWS(wiring.provide_inherited_time_series(identity, TS<i64>::type(), provider));
}

TEST_CASE("an incompatible inherited provider is rejected before reference materialization",
          "[feature:inherited-time-series][feature:wiring]") {
    const auto identity = 0;
    const auto requirement_identity = details::NestedBindingIdentity::object(&identity);
    const auto source = node([](Out<TS<bool>> output) { output.set(true); });
    auto wiring = GraphWiring{};
    const auto wiring_context = WiringContext{wiring};
    const auto provider = source();
    (void)wiring.require_inherited_time_series(requirement_identity, util::symbol{"integer_input"}, TS<i64>::type());
    const auto node_count = wiring.nodes().size();

    CHECK_THROWS(wiring.provide_inherited_time_series(requirement_identity, TS<i64>::type(), provider));
    CHECK(wiring.nodes().size() == node_count);
}

TEST_CASE("an unresolved inherited time series fails compilation with its requirement diagnostic",
          "[feature:inherited-time-series][feature:wiring]") {
    const auto identity = 0;
    const auto requirement_identity = details::NestedBindingIdentity::object(&identity);
    const auto consume = node<OrderConsumer>();
    auto wiring = GraphWiring{};
    const auto wiring_context = WiringContext{wiring};
    const auto requirement = wiring.require_inherited_time_series(requirement_identity, util::symbol{"missing_input"},
                                                                  TS<i64>::type());
    consume(requirement);

    wiring.finalize();
    CHECK_THROWS_WITH(CompiledGraph{wiring}, "an inherited time series requirement has no provider");
}

TEST_CASE("a late inherited provider dependency cycle fails compilation",
          "[feature:inherited-time-series][feature:wiring][feature:compilation]") {
    const auto identity = 0;
    const auto requirement_identity = details::NestedBindingIdentity::object(&identity);
    const auto copy = node([](i64 input) { return input; });

    auto wiring = GraphWiring{};
    const auto wiring_context = WiringContext{wiring};
    const auto requirement = wiring.require_inherited_time_series(requirement_identity, util::symbol{"cycle_input"},
                                                                  TS<i64>::type());
    const auto provider = copy(requirement);
    wiring.provide_inherited_time_series(requirement_identity, TS<i64>::type(), provider);

    wiring.finalize();
    CHECK_THROWS_WITH(CompiledGraph{wiring},
                      Catch::Matchers::ContainsSubstring("graph wiring contains a dependency cycle"));
}

TEST_CASE("compilation keeps an inherited provider next to its consumer",
          "[feature:inherited-time-series][feature:wiring][feature:compilation]") {
    const auto identity = 0;
    const auto requirement_identity = details::NestedBindingIdentity::object(&identity);
    const auto source = node<OrderSource>([](i64 value) { return value == 1 ? "unrelated" : "provider"; });

    const auto consume = node<OrderConsumer>([](wp<TS<i64>>) { return "consumer"; });
    auto wiring = GraphWiring{};
    const auto wiring_context = WiringContext{wiring};
    const auto requirement = wiring.require_inherited_time_series(requirement_identity, util::symbol{"ordered_input"},
                                                                  TS<i64>::type());
    consume(requirement);
    (void)source(1);
    const auto provider = source(2);
    wiring.provide_inherited_time_series(requirement_identity, TS<i64>::type(), provider);

    wiring.finalize();
    const auto compiled = CompiledGraph{wiring};

    const auto index_of_label = [&](std::string_view label) {
        for (auto index = std::size_t{}; index < compiled.node_count(); ++index) {
            if (compiled.wiring_path(index).label.text() == label) {
                return index;
            }
        }
        return compiled.node_count();
    };
    const auto provider_index = index_of_label("provider");
    const auto consumer_index = index_of_label("consumer");
    const auto unrelated_index = index_of_label("unrelated");

    REQUIRE(provider_index < compiled.node_count());
    REQUIRE(consumer_index < compiled.node_count());
    REQUIRE(unrelated_index < compiled.node_count());
    CHECK(provider_index < consumer_index);
    CHECK(consumer_index < unrelated_index);
}
