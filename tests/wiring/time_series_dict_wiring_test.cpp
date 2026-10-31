#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/node.hpp>
#include <bg/operators.hpp>
#include <bg/testing/test_wiring.hpp>
#include <bg/time_series/tsd/tsd_type.hpp>
#include <bg/time_series/tss/tss_type.hpp>
#include <bg/type/time_series_reference_type.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <type_traits>
#include <utility>

using namespace bg;

TEST_CASE("getitem leaves unsupported collection results generic", "[wiring][coverage-exempt]") {
    STATIC_CHECK(std::same_as<decltype(std::declval<wp<TSS<i64>>>()[i64{}]), wp<>>);
}

TEST_CASE("getitem selects a dictionary item through a reference",
          "[wiring][series:tsd][feature:getitem][feature:operator][binding:value-to-reference]") {
    const auto source = node([](Out<TSD<i64, TS<float>>>) {});

    const auto sink = node([](In<TS<float>>) {});
    auto wiring = GraphWiring{};

    const auto context = WiringContext{wiring};
    const auto selected = source()[1];

    STATIC_CHECK(std::same_as<decltype(selected), const wp<>>);
    const auto &reference = dynamic_cast<const TimeSeriesReferenceType &>(selected.type());
    CHECK(&reference.target_type() == &TS<float>::type());
    CHECK(reference.guarantee().value() == TimeSeriesGuarantee::ordinary);

    sink(selected);
}

TEST_CASE("repeated dictionary item selection reuses its node",
          "[wiring][series:tsd][feature:getitem][feature:operator][shape:fan-out]") {
    const auto source = node([](Out<TSD<i64, TS<float>>>) {});
    auto wiring = GraphWiring{};

    const auto context = WiringContext{wiring};
    const auto dictionary = source();
    (void)dictionary[1];
    (void)dictionary[1];

    (void)wiring.complete_pending_wiring();
    REQUIRE(wiring.nodes().size() == 3);
}

TEST_CASE("getitem accepts a time series dictionary key",
          "[wiring][series:tsd][series:value][feature:getitem][feature:operator]") {
    const auto dictionary_source = node([](Out<TSD<i64, TS<float>>>) {});

    const auto key_source = node([](Out<TS<i64>>) {});
    auto wiring = GraphWiring{};

    const auto context = WiringContext{wiring};
    const auto selected = dictionary_source()[key_source()];

    const auto &reference = dynamic_cast<const TimeSeriesReferenceType &>(selected.type());
    CHECK(&reference.target_type() == &TS<float>::type());
}

TEST_CASE("TSS-keyed getitem preserves final item references",
          "[wiring][series:tsd][series:tss][feature:getitem][feature:operator][guarantee:final]") {
    const auto dictionary = node([](Out<TSD<i64, TS<float, TimeSeriesGuarantee::final>, TimeSeriesGuarantee::final>>) {
    });
    const auto keys = node([](Out<TSS<i64>>) {});

    testing::test_wiring([&] {
        const auto selected = dictionary()[keys()];
        CHECK(&selected.type() ==
              &TSD<i64, REF<TS<float, TimeSeriesGuarantee::final>, TimeSeriesGuarantee::final>>::type());
    });
}

TEST_CASE("getitem preserves a dictionary reference item's target type",
          "[wiring][series:tsd][reference][feature:getitem][feature:operator]") {
    const auto source = node([](Out<TSD<i64, REF<TS<float>>>>) {});
    auto wiring = GraphWiring{};

    const auto context = WiringContext{wiring};
    const auto selected = source()[1];

    STATIC_CHECK(std::same_as<decltype(selected), const wp<>>);
    CHECK(&selected.type() == &time_series_reference_type(TS<float>::type(), TimeSeriesGuarantee::ordinary));
}

TEST_CASE("getitem preserves value item guarantees",
          "[wiring][series:tsd][series:value][reference][feature:getitem][feature:operator][guarantee:final]") {
    const auto source = node([](Out<TSD<i64, TS<float, TimeSeriesGuarantee::final>, TimeSeriesGuarantee::final>>) {});

    testing::test_wiring([&] {
        CHECK(&source()[1].type() == &REF<TS<float, TimeSeriesGuarantee::final>, TimeSeriesGuarantee::final>::type());
    });
}

TEST_CASE("getitem preserves reference item guarantees",
          "[wiring][series:tsd][series:reference][feature:getitem][feature:operator][guarantee:final]") {
    const auto source = node([](Out<TSD<i64, REF<TS<float, TimeSeriesGuarantee::final>, TimeSeriesGuarantee::final>,
                                        TimeSeriesGuarantee::final>>) {});

    testing::test_wiring([&] {
        const auto selected = source()[1];
        STATIC_CHECK(std::same_as<decltype(selected), const wp<>>);
        const auto untyped = wp<>{source()};
        const auto untyped_selected = untyped[1];
        STATIC_CHECK(std::same_as<decltype(untyped_selected), const wp<>>);

        CHECK(&selected.type() == &REF<TS<float, TimeSeriesGuarantee::final>, TimeSeriesGuarantee::final>::type());
        CHECK(&untyped_selected.type() == &selected.type());
    });
}

TEST_CASE("a time series key limits the getitem reference guarantee",
          "[wiring][series:tsd][series:reference][feature:getitem][feature:operator][guarantee:ordinary]") {
    const auto source = node([](Out<TSD<i64, REF<TS<float, TimeSeriesGuarantee::final>, TimeSeriesGuarantee::final>,
                                        TimeSeriesGuarantee::final>>) {});

    const auto key = node([](Out<TS<i64>>) {});

    testing::test_wiring([&] {
        CHECK(&source()[key()].type() ==
              &REF<TS<float, TimeSeriesGuarantee::final>, TimeSeriesGuarantee::ordinary>::type());
    });
}
