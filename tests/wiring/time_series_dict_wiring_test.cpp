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

TEST_CASE("getitem leaves unsupported collection results generic", "[wiring][coverage-exempt]") {
    STATIC_CHECK(std::same_as<decltype(std::declval<bg::wp<bg::TSS<bg::i64>>>()[bg::i64{}]), bg::wp<>>);
}

TEST_CASE("getitem selects a dictionary item through a reference",
          "[wiring][series:tsd][feature:getitem][feature:operator][binding:value-to-reference]") {
    const auto source = bg::node([](bg::Out<bg::TSD<bg::i64, bg::TS<float>>>) {});
    const auto sink = bg::node([](bg::In<bg::TS<float>>) {});
    auto wiring = bg::GraphWiring{};

    const auto context = bg::WiringContext{wiring};
    const auto selected = source()[1];

    STATIC_CHECK(std::same_as<decltype(selected), const bg::wp<>>);
    const auto &reference = dynamic_cast<const bg::TimeSeriesReferenceType &>(selected.type());
    CHECK(&reference.target_type() == &bg::TS<float>::type());
    CHECK(reference.guarantee().value() == bg::TimeSeriesGuarantee::ordinary);

    sink(selected);
}

TEST_CASE("repeated dictionary item selection reuses its node",
          "[wiring][series:tsd][feature:getitem][feature:operator][shape:fan-out]") {
    const auto source = bg::node([](bg::Out<bg::TSD<bg::i64, bg::TS<float>>>) {});
    auto wiring = bg::GraphWiring{};

    const auto context = bg::WiringContext{wiring};
    const auto dictionary = source();
    (void)dictionary[1];
    (void)dictionary[1];

    (void)wiring.complete_pending_wiring();
    REQUIRE(wiring.nodes().size() == 3);
}

TEST_CASE("getitem accepts a time series dictionary key",
          "[wiring][series:tsd][series:value][feature:getitem][feature:operator]") {
    const auto dictionary_source = bg::node([](bg::Out<bg::TSD<bg::i64, bg::TS<float>>>) {});
    const auto key_source = bg::node([](bg::Out<bg::TS<bg::i64>>) {});
    auto wiring = bg::GraphWiring{};

    const auto context = bg::WiringContext{wiring};
    const auto selected = dictionary_source()[key_source()];

    const auto &reference = dynamic_cast<const bg::TimeSeriesReferenceType &>(selected.type());
    CHECK(&reference.target_type() == &bg::TS<float>::type());
}

TEST_CASE("getitem preserves a dictionary reference item's target type",
          "[wiring][series:tsd][reference][feature:getitem][feature:operator]") {
    const auto source = bg::node([](bg::Out<bg::TSD<bg::i64, bg::REF<bg::TS<float>>>>) {});
    auto wiring = bg::GraphWiring{};

    const auto context = bg::WiringContext{wiring};
    const auto selected = source()[1];

    STATIC_CHECK(std::same_as<decltype(selected), const bg::wp<>>);
    CHECK(&selected.type() ==
          &bg::time_series_reference_type(bg::TS<float>::type(), bg::TimeSeriesGuarantee::ordinary));
}

TEST_CASE("getitem preserves value item guarantees",
          "[wiring][series:tsd][series:value][reference][feature:getitem][feature:operator][guarantee:final]") {
    const auto source = bg::node(
        [](bg::Out<bg::TSD<bg::i64, bg::TS<float, bg::TimeSeriesGuarantee::final>,
                           bg::TimeSeriesGuarantee::final>>) {});

    bg::testing::test_wiring([&] {
        CHECK(&source()[1].type() ==
              &bg::REF<bg::TS<float, bg::TimeSeriesGuarantee::final>, bg::TimeSeriesGuarantee::final>::type());
    });
}

TEST_CASE("getitem preserves reference item guarantees",
          "[wiring][series:tsd][series:reference][feature:getitem][feature:operator][guarantee:final]") {
    const auto source = bg::node(
        [](bg::Out<bg::TSD<bg::i64,
                           bg::REF<bg::TS<float, bg::TimeSeriesGuarantee::final>, bg::TimeSeriesGuarantee::final>,
                           bg::TimeSeriesGuarantee::final>>) {});

    bg::testing::test_wiring([&] {
        const auto selected = source()[1];
        STATIC_CHECK(std::same_as<decltype(selected), const bg::wp<>>);
        const auto untyped = bg::wp<>{source()};
        const auto untyped_selected = untyped[1];
        STATIC_CHECK(std::same_as<decltype(untyped_selected), const bg::wp<>>);

        CHECK(&selected.type() ==
              &bg::REF<bg::TS<float, bg::TimeSeriesGuarantee::final>, bg::TimeSeriesGuarantee::final>::type());
        CHECK(&untyped_selected.type() == &selected.type());
    });
}

TEST_CASE("a time series key limits the getitem reference guarantee",
          "[wiring][series:tsd][series:reference][feature:getitem][feature:operator][guarantee:ordinary]") {
    const auto source = bg::node(
        [](bg::Out<bg::TSD<bg::i64,
                           bg::REF<bg::TS<float, bg::TimeSeriesGuarantee::final>, bg::TimeSeriesGuarantee::final>,
                           bg::TimeSeriesGuarantee::final>>) {});
    const auto key = bg::node([](bg::Out<bg::TS<bg::i64>>) {});

    bg::testing::test_wiring([&] {
        CHECK(&source()[key()].type() ==
              &bg::REF<bg::TS<float, bg::TimeSeriesGuarantee::final>, bg::TimeSeriesGuarantee::ordinary>::type());
    });
}
