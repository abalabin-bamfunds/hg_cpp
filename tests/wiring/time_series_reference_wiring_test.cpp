#include <bg/graph.hpp>
#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/node.hpp>
#include <bg/operator.hpp>
#include <bg/testing/test_wiring.hpp>
#include <bg/type/time_series_reference_type.hpp>
#include <bg/time_series/tsd/tsd_type.hpp>
#include <bg/time_series/time_series_variable.hpp>
#include <bg/type/guarantee_variable.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace bg;
using namespace bg::literals;

namespace {

struct reference_wiring_source {
    Out<TS<i64>> output;
    void evaluate() const {}
};

struct reference_wiring_pass {
    In<REF<TS<i64>>> input;
    Out<REF<TS<i64>>> output;
    void evaluate() const { output.copy(input); }
};

} // namespace

TEST_CASE("wiring inserts and reuses a reference-making node",
          "[wiring][reference][feature:wiring][binding:value-to-reference]") {
    const auto source = node<reference_wiring_source>();
    const auto pass = node<reference_wiring_pass>();
    auto wiring = GraphWiring{};

    {
        const auto context = WiringContext{wiring};
        const auto value = source();
        (void)pass(value);
        (void)pass(value);
        wiring.finalize();
    }

    REQUIRE(wiring.nodes().size() == 3);
    CHECK(wiring.nodes()[2].inputs().front().index() == 1);
    CHECK(wiring.nodes()[1].signature().name == util::symbol{"reference"});
    const auto &reference = dynamic_cast<const TimeSeriesReferenceType &>(*wiring.nodes()[1].signature().output);
    CHECK(reference.guarantee().value() == TimeSeriesGuarantee::const_);
}

TEST_CASE("wiring preserves an existing reference",
          "[wiring][reference][feature:wiring][binding:reference-to-reference]") {
    const auto source = node<reference_wiring_source>();
    const auto pass = node<reference_wiring_pass>();
    auto wiring = GraphWiring{};

    {
        const auto context = WiringContext{wiring};
        const auto reference = pass(source());
        (void)pass(reference);
        wiring.finalize();

        REQUIRE(wiring.nodes().size() == 4);
        const auto input = wiring.nodes().back().inputs().front().index();
        CHECK(wiring.outputs()[input].node_index == 2);
    }
}

TEST_CASE("wiring rejects materializing a borrowed input as a reference",
          "[wiring][reference][feature:borrowed-input]") {
    const auto pass = node<reference_wiring_pass>();
    auto wiring = GraphWiring{};
    const auto context = WiringContext{wiring};
    const auto input = wiring.borrowed_input(TS<i64>::type());

    (void)pass(input);
    CHECK_THROWS_AS(wiring.finalize(), WiringError);
    CHECK(wiring.nodes().empty());
}

TEST_CASE("wiring rejects reference-typed borrowed input slots",
          "[wiring][reference][feature:borrowed-input]") {
    auto wiring = GraphWiring{};

    CHECK_THROWS_AS(wiring.borrowed_input(REF<TS<i64>>::type()), WiringError);
}

TEST_CASE("operator type-variable parameters use observed reference guarantees",
          "[feature:operator][feature:wiring][feature:reference][declaration:generic]") {
    auto operation = operator_<auto(TS_T)->TS_T>{"observed", {"value"}};
    auto calls = 0;
    operation.overload(graph("name"_n = "observed", [&](wp<TS_T> value) {
        ++calls;
        return value;
    }));

    const auto reference_source =
        node([](Out<REF<TS<i64, TimeSeriesGuarantee::deduplicated>, TimeSeriesGuarantee::deduplicated>>) {});
    const auto final_source = node([](Out<TS<i64, TimeSeriesGuarantee::final>>) {});
    const auto collection_source =
        node([](Out<REF<TSD<i64, TS<i64, TimeSeriesGuarantee::deduplicated>>, TimeSeriesGuarantee::deduplicated>>) {});
    auto collection_calls = 0;
    auto collection_operation = operator_<auto(TS_T)->TS_T>{"observed_collection", {"value"}};
    collection_operation.overload(graph(
        "name"_n = "observed_collection",
        require =
            [&](const WiringArguments &arguments) {
                ++collection_calls;
                CHECK(&arguments.resolved_type(0) == &TSD<i64, TS<i64>>::type());
                return accept;
            },
        [](wp<TS_T> value) { return value; }));

    testing::test_wiring([&] {
        (void)operation(reference_source());
        (void)operation(final_source());
        (void)collection_operation(collection_source());
    });

    CHECK(calls == 2);
    CHECK(collection_calls == 1);
}

TEST_CASE("generic time-series guarantee parameters retain observed and reference-target guarantees",
          "[feature:operator][feature:wiring][feature:reference][declaration:generic]"
          "[guarantee:ordinary][guarantee:deduplicated][guarantee:final][guarantee:const]") {
    using VALUE = TimeSeriesVariable<"OBSERVED_VALUE">;
    using OBSERVED = VALUE::with<guarantee_variable<"OBSERVED_GUARANTEE">>;
    const auto final_reference = node([](Out<REF<TS<i64, TimeSeriesGuarantee::final>, TimeSeriesGuarantee::final>>) {});
    const auto ordinary_reference =
        node([](Out<REF<TS<i64, TimeSeriesGuarantee::final>, TimeSeriesGuarantee::ordinary>>) {});
    const auto deduplicated_reference =
        node([](Out<REF<TS<i64, TimeSeriesGuarantee::final>, TimeSeriesGuarantee::deduplicated>>) {});
    const auto const_reference =
        node([](Out<REF<TS<i64, TimeSeriesGuarantee::final>, TimeSeriesGuarantee::const_>>) {});
    const auto direct_final_source = node([](Out<TS<i64, TimeSeriesGuarantee::final>>) {});
    const auto direct_deduplicated_source = node([](Out<TS<i64, TimeSeriesGuarantee::deduplicated>>) {});

    const auto check_observed = [](const auto &source, const TimeSeriesType &expected) {
        auto operation = operator_<auto(TS_T)->TS_T>{"observed_guarantee", {"value"}};
        operation.overload(graph(
            "name"_n = "observed_guarantee",
            require =
                [expected_type = &expected](const WiringArguments &arguments) {
                    CHECK(&arguments.resolved_type(0) == expected_type);
                    return accept;
                },
            [](wp<OBSERVED> value) { return value; }));

        testing::test_wiring([&] { (void)operation(source()); });
    };
    const auto check_reference_target = [](const auto &source, const TimeSeriesType &expected) {
        auto operation =
            operator_<auto(REF<TS_T, guarantee_variable<"REFERENCE_GUARANTEE">>)
                          ->REF<TS_T, guarantee_variable<"REFERENCE_GUARANTEE">>>{"reference_target", {"value"}};
        operation.overload(graph(
            "name"_n = "reference_target",
            require =
                [expected_type = &expected](const WiringArguments &arguments) {
                    CHECK(&arguments.resolved_type(0) == expected_type);
                    return accept;
                },
            [](wp<REF<TS_T, guarantee_variable<"REFERENCE_GUARANTEE">>> value) { return value; }));

        testing::test_wiring([&] { (void)operation(source()); });
    };

    check_observed(final_reference, TS<i64, TimeSeriesGuarantee::final>::type());
    check_observed(ordinary_reference, TS<i64, TimeSeriesGuarantee::ordinary>::type());
    check_observed(deduplicated_reference, TS<i64, TimeSeriesGuarantee::ordinary>::type());
    check_observed(const_reference, TS<i64, TimeSeriesGuarantee::final>::type());
    check_observed(direct_final_source, TS<i64, TimeSeriesGuarantee::final>::type());
    check_observed(direct_deduplicated_source, TS<i64, TimeSeriesGuarantee::deduplicated>::type());

    check_reference_target(final_reference,
                           REF<TS<i64, TimeSeriesGuarantee::final>, TimeSeriesGuarantee::final>::type());
    check_reference_target(ordinary_reference,
                           REF<TS<i64, TimeSeriesGuarantee::final>, TimeSeriesGuarantee::ordinary>::type());
    check_reference_target(deduplicated_reference,
                           REF<TS<i64, TimeSeriesGuarantee::final>, TimeSeriesGuarantee::deduplicated>::type());
    check_reference_target(const_reference,
                           REF<TS<i64, TimeSeriesGuarantee::final>, TimeSeriesGuarantee::const_>::type());
}
