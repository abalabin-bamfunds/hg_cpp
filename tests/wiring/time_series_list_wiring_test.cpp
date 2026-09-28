#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/node.hpp>
#include <bg/operators.hpp>
#include <bg/time_series/tsl/tsl.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>

TEST_CASE("a free fixed time series list is materialized when consumed",
          "[wiring][tsl][series:fixed-tsl][free][representation:free][feature:wiring]"
          "[binding:representation]") {
    const auto first_source = bg::node([](bg::Out<bg::TS<float>>) {});
    const auto second_source = bg::node([](bg::Out<bg::TS<float>>) {});
    const auto sink = bg::node([](bg::In<bg::TSL<bg::TS<float>, 2>>) {});
    auto wiring = bg::GraphWiring{};

    const auto context = bg::WiringContext{wiring};
    const auto first = first_source();
    const auto second = second_source();
    const auto list = bg::tsl(first, second);
    REQUIRE(wiring.nodes().size() == 2);
    (void)sink(list);

    REQUIRE(wiring.nodes().size() == 4);
    CHECK(wiring.nodes()[2].inputs().size() == 2);
}

TEST_CASE("a free fixed time series list reports incompatible item types as a wiring error",
          "[wiring][tsl][series:fixed-tsl][representation:free][feature:wiring]") {
    const auto integer = bg::node([](bg::Out<bg::TS<int>>) {});
    const auto floating = bg::node([](bg::Out<bg::TS<float>>) {});
    auto wiring = bg::GraphWiring{};

    const auto context = bg::WiringContext{wiring};
    CHECK_THROWS_AS(bg::tsl(integer(), floating()), bg::WiringError);
}

TEST_CASE("a free fixed time series list reuses materialization for the same input type",
          "[wiring][tsl][series:fixed-tsl][representation:free][feature:wiring]"
          "[binding:representation][shape:fan-out]") {
    const auto first_source = bg::node([](bg::Out<bg::TS<float>>) {});
    const auto second_source = bg::node([](bg::Out<bg::TS<float>>) {});
    const auto sink = bg::node([](bg::In<bg::TSL<bg::TS<float>, 2>>) {});
    auto wiring = bg::GraphWiring{};

    const auto context = bg::WiringContext{wiring};
    const auto first = first_source();
    const auto second = second_source();
    const auto list = bg::tsl(first, second);
    (void)sink(list);
    (void)sink(list);

    REQUIRE(wiring.nodes().size() == 5);
    CHECK(wiring.nodes()[3].inputs()[0] == wiring.nodes()[4].inputs()[0]);
}

TEST_CASE("a free fixed time series list can satisfy a reference input",
          "[wiring][tsl][reference][series:fixed-tsl][representation:free][feature:wiring]"
          "[binding:value-to-reference]") {
    const auto first_source = bg::node([](bg::Out<bg::TS<float>>) {});
    const auto second_source = bg::node([](bg::Out<bg::TS<float>>) {});
    const auto sink = bg::node([](bg::In<bg::REF<bg::TSL<bg::TS<float>, 2>>>) {});
    auto wiring = bg::GraphWiring{};

    const auto context = bg::WiringContext{wiring};
    const auto list = bg::tsl(first_source(), second_source());
    sink(list);

    REQUIRE(wiring.nodes().size() == 5);
    CHECK(wiring.nodes()[2].signature().name == bg::util::symbol{"tsl"});
    CHECK(wiring.nodes()[3].signature().name == bg::util::symbol{"reference"});
}

TEST_CASE("getitem selects an item from a free fixed time series list without materializing it",
          "[wiring][series:fixed-tsl][representation:free][feature:getitem][feature:operator]") {
    const auto source = bg::node([](bg::Out<bg::TS<float>>) {});
    auto wiring = bg::GraphWiring{};

    const auto context = bg::WiringContext{wiring};
    const auto first = source();
    const auto second = source();
    const auto list = bg::tsl(first, second);
    const auto selected = list[std::int64_t{1}];

    STATIC_CHECK(std::same_as<decltype(selected), const bg::wp<>>);
    CHECK(&selected.type() == &bg::TS<float>::type());
    CHECK(wiring.nodes().size() == 1);
    CHECK(wiring.outputs().size() == 1);
}

TEST_CASE("getitem creates a contained wired output when a stored list item is consumed",
          "[wiring][series:fixed-tsl][representation:native][feature:getitem][feature:operator]") {
    const auto source = bg::node([](bg::Out<bg::TSL<bg::TS<float>, 3>>) {});
    const auto sink = bg::node([](bg::In<bg::TS<float>>) {});
    auto wiring = bg::GraphWiring{};

    const auto context = bg::WiringContext{wiring};
    const auto selected = source()[std::int64_t{1}];

    CHECK(&selected.type() == &bg::TS<float>::type());
    CHECK(wiring.outputs().size() == 1);

    sink(selected);

    REQUIRE(wiring.outputs().size() == 2);
    CHECK(wiring.outputs()[1].parent == 0);
    CHECK(wiring.outputs()[1].item == 1);
}

TEST_CASE("getitem reuses a contained wired output",
          "[wiring][series:fixed-tsl][representation:native][feature:getitem][feature:operator]"
          "[shape:fan-out]") {
    const auto source = bg::node([](bg::Out<bg::TSL<bg::TS<float>, 3>>) {});
    const auto sink = bg::node([](bg::In<bg::TS<float>>) {});
    auto wiring = bg::GraphWiring{};

    const auto context = bg::WiringContext{wiring};
    const auto list = source();
    sink(list[std::int64_t{1}]);
    sink(list[std::int64_t{1}]);

    CHECK(wiring.outputs().size() == 2);
}

TEST_CASE("getitem distinguishes items belonging to different wired outputs",
          "[wiring][series:fixed-tsl][representation:native][feature:getitem][feature:operator]") {
    const auto first_source = bg::node([](bg::Out<bg::TSL<bg::TS<float>, 3>>) {});
    const auto second_source = bg::node([](bg::Out<bg::TSL<bg::TS<float>, 3>>) {});
    const auto sink = bg::node([](bg::In<bg::TS<float>>) {});
    auto wiring = bg::GraphWiring{};

    const auto context = bg::WiringContext{wiring};
    sink(first_source()[std::int64_t{1}]);
    sink(second_source()[std::int64_t{1}]);

    REQUIRE(wiring.outputs().size() == 4);
    CHECK(wiring.outputs()[1].parent == 0);
    CHECK(wiring.outputs()[3].parent == 2);
}

TEST_CASE("getitem selects an item through a reference-backed fixed list",
          "[feature:wiring][feature:getitem][feature:operator][feature:reference][series:fixed-tsl]"
          "[binding:reference-to-value]") {
    const auto source = bg::node([](bg::Out<bg::REF<bg::TSL<bg::TS<float>, 3>>>) {});
    const auto sink = bg::node([](bg::In<bg::TS<float>>) {});
    auto wiring = bg::GraphWiring{};

    const auto context = bg::WiringContext{wiring};
    const auto selected = source()[std::int64_t{1}];

    CHECK(&selected.type() == &bg::REF<bg::TS<float>, bg::TimeSeriesGuarantee::ordinary>::type());
    sink(selected);
}
