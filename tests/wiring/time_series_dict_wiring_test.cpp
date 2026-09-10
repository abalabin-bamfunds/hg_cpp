#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/node.hpp>
#include <bg/operators.hpp>
#include <bg/time_series/tsd/tsd_type.hpp>
#include <bg/type/time_series_reference_type.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <type_traits>

TEST_CASE("getitem selects a dictionary item through a deduplicated reference",
          "[wiring][series:tsd][feature:getitem][feature:operator][binding:value-to-reference]") {
    const auto source = bg::node([](bg::Out<bg::TSD<std::int64_t, bg::TS<float>>>) {});
    const auto sink = bg::node([](bg::In<bg::TS<float>>) {});
    auto wiring = bg::GraphWiring{};

    const auto context = bg::WiringContext{wiring};
    const auto selected = source()[1];

    STATIC_CHECK(std::same_as<decltype(selected), const bg::wp<>>);
    const auto &reference = dynamic_cast<const bg::TimeSeriesReferenceType &>(selected.type());
    CHECK(&reference.target_type() == &bg::TS<float>::type());
    CHECK(reference.guarantee().value() == bg::TimeSeriesGuarantee::deduplicated);

    sink(selected);
}

TEST_CASE("repeated dictionary item selection reuses its node",
          "[wiring][series:tsd][feature:getitem][feature:operator][shape:fan-out]") {
    const auto source = bg::node([](bg::Out<bg::TSD<std::int64_t, bg::TS<float>>>) {});
    auto wiring = bg::GraphWiring{};

    const auto context = bg::WiringContext{wiring};
    const auto dictionary = source();
    (void)dictionary[1];
    (void)dictionary[1];

    REQUIRE(wiring.nodes().size() == 3);
    CHECK(wiring.nodes().back().signature().name == bg::util::symbol{"tsd_getitem_scalar"});
}

TEST_CASE("getitem accepts a time series dictionary key",
          "[wiring][series:tsd][series:value][feature:getitem][feature:operator]") {
    const auto dictionary_source = bg::node([](bg::Out<bg::TSD<std::int64_t, bg::TS<float>>>) {});
    const auto key_source = bg::node([](bg::Out<bg::TS<std::int64_t>>) {});
    auto wiring = bg::GraphWiring{};

    const auto context = bg::WiringContext{wiring};
    const auto selected = dictionary_source()[key_source()];

    const auto &reference = dynamic_cast<const bg::TimeSeriesReferenceType &>(selected.type());
    CHECK(&reference.target_type() == &bg::TS<float>::type());
    CHECK(wiring.nodes().back().signature().name == bg::util::symbol{"tsd_getitem_time_series"});
}
