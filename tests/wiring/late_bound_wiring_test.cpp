#include <bg/graph.hpp>
#include <bg/graph/compiled_graph.hpp>
#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/nested.hpp>
#include <bg/node.hpp>
#include <bg/testing/test_graph.hpp>
#include <bg/time_series/tsb/tsb.hpp>
#include <bg/time_series/tsl/tsl.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <memory>

namespace {

using namespace bg;
using namespace bg::literals;

template <template <typename, auto...> typename Field> struct LateBoundBundle {
    Field<TS<i64>> value;
};

} // namespace

TEST_CASE("a late-bound wiring point resolves directly to its provider",
          "[feature:wiring][feature:late-bound-wiring]") {
    const auto source = node([](Out<TS<i64>> output) { output.set(7); });

    const auto copy = node([](In<TS<i64>> input, Out<TS<i64>> output) { output.set(*input); });
    auto wiring = GraphWiring{};
    const auto context = WiringContext{wiring};

    const auto late = wiring.late_bound_wiring(TS<i64>::type());
    (void)copy(late);
    const auto provider = source();
    wiring.provide_late_bound_wiring(late, provider);
    wiring.finalize();

    const auto compiled = CompiledGraph{wiring};
    CHECK(compiled.node_count() == 2);
}

TEST_CASE("late-bound readiness propagates through pending node outputs",
          "[feature:wiring][feature:late-bound-wiring]") {
    const auto source = node([](Out<TS<i64>> output) { output.set(7); });

    const auto copy = node([](In<TS<i64>> input, Out<TS<i64>> output) { output.set(*input); });
    auto wiring = GraphWiring{};
    const auto context = WiringContext{wiring};

    const auto late = wiring.late_bound_wiring(TS<i64>::type());
    const auto first = copy(late);
    (void)copy(first);
    const auto provider = source();
    wiring.provide_late_bound_wiring(late, provider);
    wiring.finalize();

    const auto compiled = CompiledGraph{wiring};
    CHECK(compiled.node_count() == 3);
}

TEST_CASE("late-bound points can alias through an acyclic chain",
          "[feature:wiring][feature:late-bound-wiring]") {
    const auto source = node([](Out<TS<i64>>) {});

    const auto copy = node([](In<TS<i64>>, Out<TS<i64>>) {});
    auto wiring = GraphWiring{};
    const auto context = WiringContext{wiring};

    const auto first = wiring.late_bound_wiring(TS<i64>::type());
    const auto second = wiring.late_bound_wiring(TS<i64>::type());
    (void)copy(first);
    wiring.provide_late_bound_wiring(first, second);
    wiring.provide_late_bound_wiring(second, source());
    wiring.finalize();

    CHECK(CompiledGraph{wiring}.node_count() == 2);
}

TEST_CASE("an indirect late-bound provider cycle fails materialization",
          "[feature:wiring][feature:late-bound-wiring][series:tsb]") {
    using BUNDLE = TSB<LateBoundBundle>;

    const auto copy = node([](In<TS<i64>>, Out<TS<i64>>) {});
    auto wiring = GraphWiring{};
    const auto context = WiringContext{wiring};

    const auto late = wiring.late_bound_wiring(BUNDLE::type());
    const auto late_bundle = wp<BUNDLE>{late};
    const auto field = late_bundle->value;
    const auto provider = bundle<BUNDLE>("value"_n = field);
    wiring.provide_late_bound_wiring(late, provider);
    (void)copy(field);

    CHECK_THROWS_WITH(wiring.complete_pending_wiring(),
                      Catch::Matchers::ContainsSubstring("materialization contains a dependency cycle"));
}

TEST_CASE("pending wiring can continue after an unresolved late-bound point",
          "[feature:wiring][feature:late-bound-wiring]") {
    const auto source = node([](Out<TS<i64>> output) { output.set(7); });

    const auto copy = node([](In<TS<i64>> input, Out<TS<i64>> output) { output.set(*input); });
    auto wiring = GraphWiring{};
    const auto context = WiringContext{wiring};
    const auto late = wiring.late_bound_wiring(TS<i64>::type());
    (void)copy(late);

    CHECK_FALSE(wiring.complete_pending_wiring());

    wiring.provide_late_bound_wiring(late, source());
    CHECK(wiring.complete_pending_wiring());
    wiring.finalize();
    CHECK(CompiledGraph{wiring}.node_count() == 2);
}

TEST_CASE("service building can provide a late-bound point on a later fixed-point pass",
          "[feature:wiring][feature:late-bound-wiring][feature:service]") {
    const auto source = node([](Out<TS<i64>> output) { output.set(7); });

    const auto copy = node([](In<TS<i64>> input, Out<TS<i64>> output) { output.set(*input); });
    auto wiring = GraphWiring{};
    const auto context = WiringContext{wiring};
    const auto late = wiring.late_bound_wiring(TS<i64>::type());
    (void)copy(late);
    const auto catalogue_identity = 0;
    auto build_pass = std::size_t{};
    wiring.attach_service_build_session(
        &catalogue_identity, std::make_shared<int>(), [&] {
            ++build_pass;
            if (build_pass == 1) {
                return true;
            }
            if (build_pass == 2) {
                wiring.provide_late_bound_wiring(late, source());
                return true;
            }
            return false;
        },
        [] {});

    wiring.finalize();

    CHECK(build_pass == 3);
    CHECK(CompiledGraph{wiring}.node_count() == 2);
}

TEST_CASE("a nested graph can consume a late-bound wiring point",
          "[feature:wiring][feature:late-bound-wiring][feature:nested-graph]") {
    const auto copy = node([](In<TS<i64>> input, Out<TS<i64>> output) { output.set(*input); });

    const auto inner = graph("name"_n = "late_bound_inner", [&](wp<TS<i64>> input) { return copy(input); });

    const auto tested = [&](wp<TS<i64>> provider, wp<TS<bool>> run) {
        auto &wiring = WiringContext::current().wiring();
        const auto late = wiring.late_bound_wiring(TS<i64>::type());
        const auto output = nested(inner, run, late);
        wiring.provide_late_bound_wiring(late, provider);
        return output;
    };

    CHECK(testing::test_graph(tested, {7, 11}, {true, true}).equals({7, 11}));
}

TEST_CASE("late-bound readiness propagates through TSB fields and kwargs",
          "[feature:wiring][feature:late-bound-wiring][series:tsb]") {
    using BUNDLE = TSB<LateBoundBundle>;

    const auto source = node([](Out<BUNDLE>) {});

    const auto copy = node([](In<TS<i64>>, Out<TS<i64>>) {});

    const auto consume_kwargs = node(eval = [](In<TSB<SCHEMA>, var_kwargs>) {});
    auto wiring = GraphWiring{};
    const auto context = WiringContext{wiring};

    const auto late = wiring.late_bound_wiring(BUNDLE::type());
    const auto late_bundle = wp<BUNDLE>{late};
    (void)copy(late_bundle->value);
    (void)consume_kwargs(kwargs(late_bundle));

    CHECK_FALSE(wiring.complete_pending_wiring());

    const auto provider = source();
    wiring.provide_late_bound_wiring(late, provider);
    wiring.finalize();

    CHECK(CompiledGraph{wiring}.node_count() == 4);
}

TEST_CASE("late-bound readiness propagates through a TSL item",
          "[feature:wiring][feature:late-bound-wiring][series:fixed-tsl]") {
    using LIST = TSL<TS<i64>, 1>;

    const auto source = node([](Out<LIST>) {});

    const auto copy = node([](In<TS<i64>>, Out<TS<i64>>) {});
    auto wiring = GraphWiring{};
    const auto context = WiringContext{wiring};

    const auto late = wiring.late_bound_wiring(LIST::type());
    (void)copy(wp<LIST>{late}[0]);
    CHECK_FALSE(wiring.complete_pending_wiring());

    wiring.provide_late_bound_wiring(late, source());
    wiring.finalize();

    CHECK(CompiledGraph{wiring}.node_count() == 2);
}

TEST_CASE("an unprovided late-bound wiring point fails finalization",
          "[feature:wiring][feature:late-bound-wiring]") {
    const auto consume = node([](In<TS<i64>> input, Out<TS<i64>> output) { output.set(*input); });
    auto wiring = GraphWiring{};
    const auto context = WiringContext{wiring};
    const auto late = wiring.late_bound_wiring(TS<i64>::type());
    (void)consume(late);

    CHECK_THROWS_WITH(wiring.finalize(), Catch::Matchers::ContainsSubstring("late-bound wiring point has no provider"));
}

TEST_CASE("a late-bound provider must be compatible and supplied once",
          "[feature:wiring][feature:late-bound-wiring]") {
    const auto integer_source = node([](Out<TS<i64>> output) { output.set(1); });

    const auto boolean_source = node([](Out<TS<bool>> output) { output.set(true); });
    auto wiring = GraphWiring{};
    const auto context = WiringContext{wiring};
    const auto late = wiring.late_bound_wiring(TS<i64>::type());
    const auto integer_provider = integer_source();
    const auto boolean_provider = boolean_source();

    CHECK_THROWS_WITH(wiring.provide_late_bound_wiring(late, boolean_provider),
                      Catch::Matchers::ContainsSubstring("incompatible type"));
    wiring.provide_late_bound_wiring(late, integer_provider);
    CHECK_THROWS_WITH(wiring.provide_late_bound_wiring(late, integer_provider),
                      Catch::Matchers::ContainsSubstring("already has a provider"));
}

TEST_CASE("late-bound aliases cannot form a cycle", "[feature:wiring][feature:late-bound-wiring]") {
    auto wiring = GraphWiring{};
    const auto context = WiringContext{wiring};
    const auto first = wiring.late_bound_wiring(TS<i64>::type());
    const auto second = wiring.late_bound_wiring(TS<i64>::type());

    wiring.provide_late_bound_wiring(first, second);
    CHECK_THROWS_WITH(wiring.provide_late_bound_wiring(second, first),
                      Catch::Matchers::ContainsSubstring("alias cycle"));
}

TEST_CASE("a result cannot be completed while its late-bound input is unresolved",
          "[feature:wiring][feature:late-bound-wiring]") {
    const auto consume = node([](In<TS<i64>> input, Out<TS<i64>> output) { output.set(*input); });
    auto wiring = GraphWiring{};
    const auto context = WiringContext{wiring};
    const auto late = wiring.late_bound_wiring(TS<i64>::type());
    const auto result = consume(late);

    CHECK_THROWS_WITH(wiring.complete_pending_wiring(result),
                      Catch::Matchers::ContainsSubstring("result depends on an unprovided late-bound wiring point"));
}
