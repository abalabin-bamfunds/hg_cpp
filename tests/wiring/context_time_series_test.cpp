#include <bg/graph.hpp>
#include <bg/graph/context.hpp>
#include <bg/node.hpp>
#include <bg/testing/test_graph.hpp>
#include <bg/testing/test_wiring.hpp>
#include <bg/time_series/tsb/tsb.hpp>
#include <bg/time_series/tsl/tsl.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>

#include <cstdint>
#include <stdexcept>

namespace {

using namespace bg;
using namespace literals;

const auto copy_context =
    node([](In<TS<i64>, "value"_n, context> value) { return *value; });

struct AggregateContextCopy {
    In<TS<i64>, context> value;
    Out<TS<i64>> output;

    void evaluate() const { output.set(*value); }
};

const auto copy_aggregate_context = node<AggregateContextCopy>();

struct AggregateContextGraph {
    wp<TS<i64>, context> value;

    wp<TS<i64>> wire() const { return copy_context(); }
};

const auto copy_aggregate_graph_context = graph<AggregateContextGraph>();

template <template <typename, auto...> typename Field> struct ContextBundle {
    Field<TS<i64>> value;
};

} // namespace

TEST_CASE("a callable node receives a context time series",
          "[feature:context-time-series][feature:wiring][definition:function]") {

    const auto tested = [](wp<TS<i64>> value) {
        return with_context("value"_n = value, [] { return copy_context(); });
    };

    const auto actual = testing::test_graph(tested, {3, 7});

    CHECK(actual.equals({3, 7}));
}

TEST_CASE("an aggregate node receives a context time series",
          "[feature:context-time-series][feature:wiring][definition:aggregate]") {

    const auto tested = [](wp<TS<i64>> value) {
        auto scope = with_context("value"_n = value);
        return copy_aggregate_context();
    };

    const auto actual = testing::test_graph(tested, {4, 8});

    CHECK(actual.equals({4, 8}));
}

TEST_CASE("an aggregate graph establishes a context time series",
          "[feature:context-time-series][feature:wiring][definition:aggregate]") {

    const auto tested = [](wp<TS<i64>> value) {
        return with_context("value"_n = value, [] { return copy_aggregate_graph_context(); });
    };

    const auto actual = testing::test_graph(tested, {5, 10});

    CHECK(actual.equals({5, 10}));
}

TEST_CASE("a TSL graph parameter can request lexical context",
          "[feature:context-time-series][feature:wiring][series:fixed-tsl][definition:function]") {

    const auto first_item = graph("name"_n = "context_list_item", [](wp<TSL<TS<i64>, 2>, "values"_n, context> values) {
        return values[0].as<TS<i64>>();
    });

    const auto tested = [&](wp<TS<i64>> first, wp<TS<i64>> second) {
        return with_context("values"_n = tsl(first, second), [&] { return first_item(); });
    };

    const auto actual = testing::test_graph(tested, {6, 12}, {7, 14});

    CHECK(actual.equals({6, 12}));
}

TEST_CASE("a TSB graph parameter can request lexical context",
          "[feature:context-time-series][feature:wiring][series:tsb][definition:function]") {

    const auto value_field = graph("name"_n = "context_bundle_item",
                                   [](wp<TSB<ContextBundle>, "values"_n, context> values) { return values->value; });

    const auto tested = [&](wp<TS<i64>> value) {
        const auto values = bundle("value"_n = value).as<TSB<ContextBundle>>();
        return with_context("values"_n = values, [&] { return value_field(); });
    };

    const auto actual = testing::test_graph(tested, {8, 16});

    CHECK(actual.equals({8, 16}));
}

TEST_CASE("a scalar context value is lifted to a const time series",
          "[feature:context-time-series][feature:wiring][guarantee:const]") {
    const auto tested = [] { return with_context("value"_n = i64{11}, [] { return copy_context(); }); };

    const auto actual = testing::test_graph(tested);

    CHECK(actual.equals({11}));
}

TEST_CASE("a callable graph context parameter can be supplied explicitly by name",
          "[feature:context-time-series][feature:wiring][definition:function]") {
    const auto inner =
        graph("name"_n = "explicit_context", [](wp<TS<i64>, "value"_n, context>) { return copy_context(); });

    const auto tested = [&](wp<TS<i64>> value) { return inner("value"_n = value); };

    const auto actual = testing::test_graph(tested, {5, 9});

    CHECK(actual.equals({5, 9}));
}

TEST_CASE("a callable node context parameter can be supplied explicitly by name",
          "[feature:context-time-series][feature:wiring][definition:function]") {

    const auto tested = [](wp<TS<i64>> value) { return copy_context("value"_n = value).as<TS<i64>>(); };

    const auto actual = testing::test_graph(tested, {7, 11});

    CHECK(actual.equals({7, 11}));
}

TEST_CASE("an aggregate node context parameter can be supplied explicitly by name",
          "[feature:context-time-series][feature:wiring][definition:aggregate]") {

    const auto tested = [](wp<TS<i64>> value) { return copy_aggregate_context("value"_n = value).as<TS<i64>>(); };

    const auto actual = testing::test_graph(tested, {9, 13});

    CHECK(actual.equals({9, 13}));
}

TEST_CASE("an aggregate graph context parameter can be supplied explicitly by name",
          "[feature:context-time-series][feature:wiring][definition:aggregate]") {

    const auto tested = [](wp<TS<i64>> value) { return copy_aggregate_graph_context("value"_n = value).as<TS<i64>>(); };

    const auto actual = testing::test_graph(tested, {10, 14});

    CHECK(actual.equals({10, 14}));
}

TEST_CASE("a context parameter does not accept a positional argument",
          "[feature:context-time-series][feature:wiring][definition:function]") {
    const auto inner =
        graph("name"_n = "positional_context", [](wp<TS<i64>, "value"_n, context>) { return copy_context(); });

    const auto source = node([](Out<TS<i64>>) {});

    testing::test_wiring([&] {
        auto scope = with_context("value"_n = source());

        CHECK_THROWS_AS((void)inner(source()), TypeResolutionError);
    });
}

TEST_CASE("a graph context parameter inherits and shadows lexical context",
          "[feature:context-time-series][feature:wiring][definition:function]") {
    const auto inner =
        graph("name"_n = "inherited_context", [](wp<TS<i64>, "value"_n, context>) { return copy_context(); });

    const auto tested = [&](wp<TS<i64>> outer, wp<TS<i64>> inner_value) {
        return with_context("value"_n = outer, [&] { return inner("value"_n = inner_value); });
    };

    const auto actual = testing::test_graph(tested, {1, 2}, {10, 20});

    CHECK(actual.equals({10, 20}));
}

TEST_CASE("repeated context imports share the same pending boundary provider",
          "[feature:context-time-series][feature:wiring][shape:fan-out]") {

    const auto source = node([](Out<TS<i64>>) {});
    auto root = GraphWiring{};
    const auto root_context = WiringContext{root};
    const auto value = source();
    auto lexical_context = with_context("value"_n = value);
    auto child = GraphWiring{root};

    {
        const auto child_context = WiringContext{child};
        (void)copy_context();
        (void)copy_context();
    }

    CHECK(child.inherited_requirements().size() == 1);
}

TEST_CASE("context input resolves a graph type variable and output",
          "[feature:context-time-series][feature:wiring][feature:type-system]") {
    using VALUE = ScalarVariable<"VALUE">;
    const auto identity =
        graph("name"_n = "context_identity", [](wp<TS<VALUE>, "value"_n, context> value) -> wp<TS<VALUE>> {
            return wp<TS<VALUE>>{static_cast<WiringPoint>(value)};
        });

    const auto tested = [&](wp<TS<i64>> value) {
        return with_context("value"_n = value, [&] { return identity(); }).as<TS<i64>>();
    };

    const auto actual = testing::test_graph(tested, {6, 12});

    CHECK(actual.equals({6, 12}));
}

TEST_CASE("a missing context time series reports its name", "[feature:context-time-series][feature:wiring]") {
    testing::test_wiring([] { CHECK_THROWS_WITH((void)copy_context(), "required context 'value' is not available"); });
}

TEST_CASE("the nearest incompatible context time series is an error",
          "[feature:context-time-series][feature:wiring][feature:type-system]") {

    const auto incompatible = node([](Out<TS<double>>) {});

    const auto compatible = node([](Out<TS<i64>>) {});

    testing::test_wiring([&] {
        auto outer = with_context("value"_n = compatible());
        auto inner = with_context("value"_n = incompatible());

        CHECK_THROWS_AS((void)copy_context(), TypeResolutionError);
    });
}

TEST_CASE("context snapshots preserve the nearest binding after scopes exit",
          "[feature:context-time-series][feature:wiring]") {
    const auto name = util::symbol{"value"};
    const auto outer_name = util::symbol{"outer"};
    auto snapshot = details::ContextSnapshot{};
    {
        const auto outer = details::ContextScope{outer_name, WiringArgument{i64{1}}};
        {
            const auto inner = details::ContextScope{name, WiringArgument{i64{42}}};
            snapshot = details::ContextSnapshot::capture();
        }
    }

    const auto later = details::ContextScope{name, WiringArgument{i64{99}}};
    const auto later_outer = details::ContextScope{outer_name, WiringArgument{i64{100}}};
    {
        const auto activation = snapshot.activate();
        const auto value = details::ContextScope::find(name);
        REQUIRE(value.has_value());
        CHECK(scalar_cast<i64>(value->scalar()).value() == 42);
        const auto outer = details::ContextScope::find(outer_name);
        REQUIRE(outer.has_value());
        CHECK(scalar_cast<i64>(outer->scalar()).value() == 1);
    }

    const auto restored = details::ContextScope::find(name);
    REQUIRE(restored.has_value());
    CHECK(scalar_cast<i64>(restored->scalar()).value() == 99);
    const auto restored_outer = details::ContextScope::find(outer_name);
    REQUIRE(restored_outer.has_value());
    CHECK(scalar_cast<i64>(restored_outer->scalar()).value() == 100);
}

TEST_CASE("context snapshots isolate absent bindings and empty snapshots",
          "[feature:context-time-series][feature:wiring]") {
    const auto value_name = util::symbol{"value"};
    const auto other_name = util::symbol{"other"};
    const auto empty = details::ContextSnapshot::capture();
    auto value_snapshot = details::ContextSnapshot{};
    {
        const auto value = details::ContextScope{value_name, WiringArgument{i64{7}}};
        value_snapshot = details::ContextSnapshot::capture();
    }

    const auto later_value = details::ContextScope{value_name, WiringArgument{i64{8}}};
    const auto later_other = details::ContextScope{other_name, WiringArgument{i64{9}}};
    {
        const auto activation = value_snapshot.activate();
        CHECK(details::ContextScope::find(other_name) == std::nullopt);
        const auto value = details::ContextScope::find(value_name);
        REQUIRE(value.has_value());
        CHECK(scalar_cast<i64>(value->scalar()).value() == 7);
    }
    {
        const auto activation = empty.activate();
        CHECK(details::ContextScope::find(value_name) == std::nullopt);
        CHECK(details::ContextScope::find(other_name) == std::nullopt);
    }

    CHECK(details::ContextScope::find(value_name).has_value());
    CHECK(details::ContextScope::find(other_name).has_value());
}

TEST_CASE("nested context snapshot activations restore their enclosing bindings",
          "[feature:context-time-series][feature:wiring]") {
    const auto name = util::symbol{"value"};
    auto first = details::ContextSnapshot{};
    auto second = details::ContextSnapshot{};
    {
        const auto value = details::ContextScope{name, WiringArgument{i64{1}}};
        first = details::ContextSnapshot::capture();
    }
    {
        const auto value = details::ContextScope{name, WiringArgument{i64{2}}};
        second = details::ContextSnapshot::capture();
    }

    const auto outer = details::ContextScope{name, WiringArgument{i64{9}}};
    const auto expect_value = [&](i64 expected) {
        const auto value = details::ContextScope::find(name);
        REQUIRE(value.has_value());
        CHECK(scalar_cast<i64>(value->scalar()).value() == expected);
    };
    {
        const auto first_activation = first.activate();
        expect_value(1);
        {
            const auto shadow = details::ContextScope{name, WiringArgument{i64{3}}};
            expect_value(3);
        }
        expect_value(1);
        {
            const auto second_activation = second.activate();
            expect_value(2);
        }
        expect_value(1);
        CHECK_THROWS_AS(([&] {
                            const auto shadow = details::ContextScope{name, WiringArgument{i64{4}}};
                            const auto second_activation = second.activate();
                            throw std::runtime_error{"unwind context activation"};
                        }()),
                        std::runtime_error);
        expect_value(1);
    }
    expect_value(9);
}

TEST_CASE("context snapshots own copied scalar arguments",
          "[feature:context-time-series][feature:wiring]") {
    const auto name = util::symbol{"value"};
    auto snapshot = details::ContextSnapshot{};
    {
        auto scalar = ScalarValue<i64>{42};
        const auto scope = details::ContextScope{name, WiringArgument{scalar}};
        snapshot = details::ContextSnapshot::capture();
        scalar = ScalarValue<i64>{99};
    }

    const auto activation = snapshot.activate();
    snapshot = details::ContextSnapshot{};
    const auto value = details::ContextScope::find(name);
    REQUIRE(value.has_value());
    CHECK(scalar_cast<i64>(value->scalar()).value() == 42);
}

TEST_CASE("operator cache entries include implicit context types",
          "[feature:context-time-series][feature:operator][feature:wiring]") {
    auto choose_context = operator_<auto()->TS_T>{"choose_context"};
    choose_context.overload(
        graph("name"_n = "integer_context", [](wp<TS<i64>, "value"_n, context> value) { return value; }));
    choose_context.overload(
        graph("name"_n = "floating_context", [](wp<TS<double>, "value"_n, context> value) { return value; }));

    const auto integer_source = node([](Out<TS<i64>>) {});

    const auto floating_source = node([](Out<TS<double>>) {});

    testing::test_wiring([&] {
        const auto integer = with_context("value"_n = integer_source(), [&] { return choose_context().as<TS<i64>>(); });
        const auto floating =
            with_context("value"_n = floating_source(), [&] { return choose_context().as<TS<double>>(); });

        CHECK(&static_cast<WiringPoint>(integer).type() == &TS<i64>::type());
        CHECK(&static_cast<WiringPoint>(floating).type() == &TS<double>::type());
    });
}

TEST_CASE("operator cache entries include contexts used by rejected overloads",
          "[feature:context-time-series][feature:operator][feature:wiring]") {
    using VALUE = ScalarVariable<"VALUE">;

    auto choose_context = operator_<auto()->TS<i64, TimeSeriesGuarantee::const_>>{"choose_rejected_context"};
    choose_context.overload(graph("name"_n = "generic_context",
                                  [](wp<TS<VALUE>, "value"_n, context>) -> wp<TS<i64, TimeSeriesGuarantee::const_>> {
                                      return operators::const_(i64{2}).as<TS<i64, TimeSeriesGuarantee::const_>>();
                                  }));
    choose_context.overload(graph("name"_n = "integer_context",
                                  [](wp<TS<i64>, "value"_n, context>) -> wp<TS<i64, TimeSeriesGuarantee::const_>> {
                                      return operators::const_(i64{1}).as<TS<i64, TimeSeriesGuarantee::const_>>();
                                  }));

    const auto tested = [&] {
        with_context("value"_n = 1.5, [&] { (void)choose_context(); });
        return with_context("value"_n = i64{1},
                            [&] { return choose_context().as<TS<i64, TimeSeriesGuarantee::const_>>(); });
    };

    const auto actual = testing::test_graph(tested);

    CHECK(actual.equals({1}));
}

TEST_CASE("rejected operator overloads do not import context requirements",
          "[feature:context-time-series][feature:operator][feature:wiring]") {
    auto choose = operator_<auto(TS_T)->TS_T>{"choose_context_import", {"value"}};
    choose.overload(graph("name"_n = "integer_choice",
                          [](wp<TS<i64>> value, wp<TS<i64>, "integer_context"_n, context>) { return value; }));
    choose.overload(graph("name"_n = "floating_choice",
                          [](wp<TS<double>> value, wp<TS<double>, "floating_context"_n, context>) { return value; }));

    const auto integer_source = node([](Out<TS<i64>>) {});

    const auto floating_source = node([](Out<TS<double>>) {});

    testing::test_wiring([&] {
        auto integer_scope = with_context("integer_context"_n = integer_source());
        auto floating_scope = with_context("floating_context"_n = floating_source());
        auto &outer = WiringContext::current().wiring();
        auto inner = GraphWiring{outer};
        auto inner_context = WiringContext{inner};
        const auto integer = integer_source();

        (void)choose(integer);

        REQUIRE(inner.inherited_requirements().size() == 1);
        CHECK(inner.inherited_requirements().front().name == util::symbol{"integer_context"});
    });
}

TEST_CASE("a context callback restores the previous scope after an exception",
          "[feature:context-time-series][feature:wiring]") {

    const auto source = node([](Out<TS<i64>>) {});

    testing::test_wiring([&] {
        CHECK_THROWS_WITH(with_context("value"_n = source(), [] { throw std::runtime_error{"failure"}; }), "failure");
        CHECK_THROWS_WITH((void)copy_context(), "required context 'value' is not available");
    });
}
