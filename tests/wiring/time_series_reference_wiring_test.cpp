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

using namespace bg::literals;

namespace {

struct reference_wiring_source {
    bg::Out<bg::TS<bg::i64>> output;
    void evaluate() const {}
};

struct reference_wiring_pass {
    bg::In<bg::REF<bg::TS<bg::i64>>> input;
    bg::Out<bg::REF<bg::TS<bg::i64>>> output;
    void evaluate() const { output.copy(input); }
};

} // namespace

TEST_CASE("wiring inserts and reuses a reference-making node",
          "[wiring][reference][feature:wiring][binding:value-to-reference]") {
    const auto source = bg::node<reference_wiring_source>();
    const auto pass = bg::node<reference_wiring_pass>();
    auto wiring = bg::GraphWiring{};

    {
        const auto context = bg::WiringContext{wiring};
        const auto value = source();
        (void)pass(value);
        (void)pass(value);
        wiring.finalize();
    }

    REQUIRE(wiring.nodes().size() == 3);
    CHECK(wiring.nodes()[2].inputs().front().index() == 1);
    CHECK(wiring.nodes()[1].signature().name == bg::util::symbol{"reference"});
    const auto &reference = dynamic_cast<const bg::TimeSeriesReferenceType &>(*wiring.nodes()[1].signature().output);
    CHECK(reference.guarantee().value() == bg::TimeSeriesGuarantee::const_);
}

TEST_CASE("wiring preserves an existing reference",
          "[wiring][reference][feature:wiring][binding:reference-to-reference]") {
    const auto source = bg::node<reference_wiring_source>();
    const auto pass = bg::node<reference_wiring_pass>();
    auto wiring = bg::GraphWiring{};

    {
        const auto context = bg::WiringContext{wiring};
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
    const auto pass = bg::node<reference_wiring_pass>();
    auto wiring = bg::GraphWiring{};
    const auto context = bg::WiringContext{wiring};
    const auto input = wiring.borrowed_input(bg::TS<bg::i64>::type());

    (void)pass(input);
    CHECK_THROWS_AS(wiring.finalize(), bg::WiringError);
    CHECK(wiring.nodes().empty());
}

TEST_CASE("wiring rejects reference-typed borrowed input slots",
          "[wiring][reference][feature:borrowed-input]") {
    auto wiring = bg::GraphWiring{};

    CHECK_THROWS_AS(wiring.borrowed_input(bg::REF<bg::TS<bg::i64>>::type()), bg::WiringError);
}

TEST_CASE("operator type-variable parameters use observed reference guarantees",
          "[feature:operator][feature:wiring][feature:reference][declaration:generic]") {
    auto operation = bg::operator_<auto(bg::TS_T) -> bg::TS_T>{"observed", {"value"}};
    auto calls = 0;
    operation.overload(bg::graph("name"_n = "observed", [&](bg::wp<bg::TS_T> value) {
        ++calls;
        return value;
    }));

    const auto reference_source = bg::node([](bg::Out<bg::REF<
                                                  bg::TS<bg::i64, bg::TimeSeriesGuarantee::deduplicated>,
                                                  bg::TimeSeriesGuarantee::deduplicated>>) {});
    const auto final_source = bg::node([](bg::Out<bg::TS<bg::i64, bg::TimeSeriesGuarantee::final>>) {});
    const auto collection_source = bg::node([](bg::Out<bg::REF<
                                                    bg::TSD<bg::i64,
                                                           bg::TS<bg::i64, bg::TimeSeriesGuarantee::deduplicated>>,
                                                    bg::TimeSeriesGuarantee::deduplicated>>) {});
    auto collection_calls = 0;
    auto collection_operation = bg::operator_<auto(bg::TS_T) -> bg::TS_T>{"observed_collection", {"value"}};
    collection_operation.overload(bg::graph(
        "name"_n = "observed_collection",
        bg::require = [&](const bg::WiringArguments &arguments) {
            ++collection_calls;
            CHECK(&arguments.resolved_type(0) == &bg::TSD<bg::i64, bg::TS<bg::i64>>::type());
            return bg::accept;
        },
        [](bg::wp<bg::TS_T> value) { return value; }));

    bg::testing::test_wiring([&] {
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
    using VALUE = bg::TimeSeriesVariable<"OBSERVED_VALUE">;
    using OBSERVED = VALUE::with<bg::guarantee_variable<"OBSERVED_GUARANTEE">>;
    const auto final_reference = bg::node([](bg::Out<bg::REF<
                                                 bg::TS<bg::i64, bg::TimeSeriesGuarantee::final>,
                                                 bg::TimeSeriesGuarantee::final>>) {});
    const auto ordinary_reference = bg::node([](bg::Out<bg::REF<
                                                    bg::TS<bg::i64, bg::TimeSeriesGuarantee::final>,
                                                    bg::TimeSeriesGuarantee::ordinary>>) {});
    const auto deduplicated_reference = bg::node([](bg::Out<bg::REF<
                                                           bg::TS<bg::i64, bg::TimeSeriesGuarantee::final>,
                                                           bg::TimeSeriesGuarantee::deduplicated>>) {});
    const auto const_reference = bg::node([](bg::Out<bg::REF<
                                               bg::TS<bg::i64, bg::TimeSeriesGuarantee::final>,
                                               bg::TimeSeriesGuarantee::const_>>) {});
    const auto direct_final_source = bg::node([](bg::Out<bg::TS<bg::i64, bg::TimeSeriesGuarantee::final>>) {});
    const auto direct_deduplicated_source =
        bg::node([](bg::Out<bg::TS<bg::i64, bg::TimeSeriesGuarantee::deduplicated>>) {});

    const auto check_observed = [](const auto &source, const bg::TimeSeriesType &expected) {
        auto operation = bg::operator_<auto(bg::TS_T) -> bg::TS_T>{"observed_guarantee", {"value"}};
        operation.overload(bg::graph(
            "name"_n = "observed_guarantee",
            bg::require = [expected_type = &expected](const bg::WiringArguments &arguments) {
                CHECK(&arguments.resolved_type(0) == expected_type);
                return bg::accept;
            },
            [](bg::wp<OBSERVED> value) { return value; }));

        bg::testing::test_wiring([&] { (void)operation(source()); });
    };
    const auto check_reference_target = [](const auto &source, const bg::TimeSeriesType &expected) {
        auto operation = bg::operator_<
            auto(bg::REF<bg::TS_T, bg::guarantee_variable<"REFERENCE_GUARANTEE">>) ->
                bg::REF<bg::TS_T, bg::guarantee_variable<"REFERENCE_GUARANTEE">>>{"reference_target", {"value"}};
        operation.overload(bg::graph(
            "name"_n = "reference_target",
            bg::require = [expected_type = &expected](const bg::WiringArguments &arguments) {
                CHECK(&arguments.resolved_type(0) == expected_type);
                return bg::accept;
            },
            [](bg::wp<bg::REF<bg::TS_T, bg::guarantee_variable<"REFERENCE_GUARANTEE">>> value) {
                return value;
            }));

        bg::testing::test_wiring([&] { (void)operation(source()); });
    };

    check_observed(final_reference, bg::TS<bg::i64, bg::TimeSeriesGuarantee::final>::type());
    check_observed(ordinary_reference, bg::TS<bg::i64, bg::TimeSeriesGuarantee::ordinary>::type());
    check_observed(deduplicated_reference, bg::TS<bg::i64, bg::TimeSeriesGuarantee::ordinary>::type());
    check_observed(const_reference, bg::TS<bg::i64, bg::TimeSeriesGuarantee::final>::type());
    check_observed(direct_final_source, bg::TS<bg::i64, bg::TimeSeriesGuarantee::final>::type());
    check_observed(direct_deduplicated_source, bg::TS<bg::i64, bg::TimeSeriesGuarantee::deduplicated>::type());

    check_reference_target(
        final_reference,
        bg::REF<bg::TS<bg::i64, bg::TimeSeriesGuarantee::final>, bg::TimeSeriesGuarantee::final>::type());
    check_reference_target(
        ordinary_reference,
        bg::REF<bg::TS<bg::i64, bg::TimeSeriesGuarantee::final>, bg::TimeSeriesGuarantee::ordinary>::type());
    check_reference_target(
        deduplicated_reference,
        bg::REF<bg::TS<bg::i64, bg::TimeSeriesGuarantee::final>, bg::TimeSeriesGuarantee::deduplicated>::type());
    check_reference_target(
        const_reference,
        bg::REF<bg::TS<bg::i64, bg::TimeSeriesGuarantee::final>, bg::TimeSeriesGuarantee::const_>::type());
}
