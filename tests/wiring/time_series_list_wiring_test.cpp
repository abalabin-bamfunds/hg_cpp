#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/node.hpp>
#include <bg/operators.hpp>
#include <bg/testing/test_wiring.hpp>
#include <bg/type/time_series_dict_type.hpp>
#include <bg/time_series/tsl/tsl.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <span>
#include <vector>

using namespace bg;

TEST_CASE("a free fixed time series list is materialized when consumed",
          "[wiring][tsl][series:fixed-tsl][free][representation:free][feature:wiring]"
          "[binding:representation]") {

    const auto first_source = node([](Out<TS<float>>) {});

    const auto second_source = node([](Out<TS<float>>) {});

    const auto sink = node([](In<TSL<TS<float>, 2>>) {});
    auto wiring = GraphWiring{};

    const auto context = WiringContext{wiring};
    const auto first = first_source();
    const auto second = second_source();
    const auto list = tsl(first, second);
    (void)wiring.complete_pending_wiring();
    REQUIRE(wiring.nodes().size() == 2);
    (void)sink(list);

    (void)wiring.complete_pending_wiring();
    REQUIRE(wiring.nodes().size() == 4);
    CHECK(wiring.nodes()[2].inputs().size() == 2);
}

TEST_CASE("a free fixed time series list reports incompatible item types as a wiring error",
          "[wiring][tsl][series:fixed-tsl][representation:free][feature:wiring]") {

    const auto integer = node([](Out<TS<i64>>) {});

    const auto floating = node([](Out<TS<float>>) {});
    auto wiring = GraphWiring{};

    const auto context = WiringContext{wiring};
    CHECK_THROWS_AS(tsl(integer(), floating()), WiringError);
}

TEST_CASE("a free fixed TSL accepts vectors and spans of wiring points",
          "[wiring][tsl][series:fixed-tsl][representation:free][feature:wiring]"
          "[feature:wiring-materialisation][binding:representation]") {

    const auto first_source = node([](Out<TS<i64>>) {});

    const auto second_source = node([](Out<TS<i64>>) {});

    const auto sink = node([](In<TSL<TS<i64>, 2>>) {});
    auto wiring = GraphWiring{};

    const auto context = WiringContext{wiring};
    const auto first = first_source();
    const auto second = second_source();
    const auto typed_items = std::array{first, second};
    const auto span_list = tsl(std::span{typed_items});
    const auto selected = span_list[i64{1}];

    CHECK(static_cast<WiringPoint>(selected).record() == static_cast<WiringPoint>(second).record());

    const auto make_vector_list = [](wp<TS<i64>> first_item, wp<TS<i64>> second_item) {
        auto vector_items = std::vector<wp<TS<i64>>>{};
        vector_items.push_back(first_item);
        vector_items.push_back(second_item);
        return tsl(vector_items);
    };
    const auto vector_list = make_vector_list(first, second);
    const auto erased_items =
        std::vector<WiringPoint>{static_cast<WiringPoint>(first), static_cast<WiringPoint>(second)};
    const auto erased_list = tsl(erased_items);
    CHECK(&erased_list.type() == &span_list.type());
    sink(vector_list);

    (void)wiring.complete_pending_wiring();
    REQUIRE(wiring.nodes().size() == 4);
}

TEST_CASE("a free fixed TSL rejects incompatible vector item types",
          "[wiring][tsl][series:fixed-tsl][representation:free][feature:wiring]") {

    const auto integer = node([](Out<TS<i64>>) {});

    const auto floating = node([](Out<TS<float>>) {});

    testing::test_wiring([&] {
        auto items = std::vector<wp<>>{};
        items.emplace_back(integer());
        items.emplace_back(floating());
        CHECK_THROWS_AS(tsl(items), WiringError);
    });
}

TEST_CASE("an empty fixed TSL requires a resolved item type",
          "[wiring][tsl][series:fixed-tsl][representation:free][feature:wiring]") {
    testing::test_wiring([] {
        const auto inferred = tsl<TS<i64>>();
        const auto explicit_type = tsl(TS<i64>::type());
        const auto &inferred_type = static_cast<const TimeSeriesListType &>(inferred.type());
        const auto &explicit_type_type = static_cast<const TimeSeriesListType &>(explicit_type.type());

        CHECK(inferred_type.is_resolved());
        CHECK(inferred_type.item_count() == 0);
        CHECK(&inferred_type.item_type() == &TS<i64>::type());
        CHECK(inferred_type.guarantee().value() == TimeSeriesGuarantee::ordinary);
        CHECK(&explicit_type_type == &inferred_type);
        CHECK_THROWS_AS(tsl(TS<SCALAR>::type()), WiringError);
        CHECK_THROWS_AS(tsl(std::span<const WiringPoint>{}), WiringError);
    });
}

TEST_CASE("free fixed TSL inference uses reference guarantees",
          "[wiring][tsl][reference][feature:wiring][guarantee:ordinary][guarantee:deduplicated]"
          "[guarantee:final][guarantee:const]") {
    const auto ordinary_reference =
        node([](Out<REF<TS<i64, TimeSeriesGuarantee::final>, TimeSeriesGuarantee::ordinary>>) {});
    const auto deduplicated_reference =
        node([](Out<REF<TS<i64, TimeSeriesGuarantee::const_>, TimeSeriesGuarantee::deduplicated>>) {});
    const auto final_reference =
        node([](Out<REF<TS<i64, TimeSeriesGuarantee::const_>, TimeSeriesGuarantee::final>>) {});
    const auto const_reference =
        node([](Out<REF<TS<i64, TimeSeriesGuarantee::const_>, TimeSeriesGuarantee::const_>>) {});

    testing::test_wiring([&] {
        const auto &ordinary = static_cast<const TimeSeriesListType &>(tsl(ordinary_reference()).type());
        const auto &deduplicated = static_cast<const TimeSeriesListType &>(tsl(deduplicated_reference()).type());
        const auto &final = static_cast<const TimeSeriesListType &>(tsl(final_reference()).type());
        const auto &constant = static_cast<const TimeSeriesListType &>(tsl(const_reference()).type());

        CHECK(ordinary.item_type().guarantee().value() == TimeSeriesGuarantee::ordinary);
        CHECK(deduplicated.item_type().guarantee().value() == TimeSeriesGuarantee::ordinary);
        CHECK(final.item_type().guarantee().value() == TimeSeriesGuarantee::final);
        CHECK(constant.item_type().guarantee().value() == TimeSeriesGuarantee::const_);

        const auto ordinary_point = ordinary_reference();
        const auto points = std::vector{ordinary_point};
        const auto typed_range = tsl(points);
        const auto erased_points = std::vector<WiringPoint>{ordinary_point};
        const auto span_conversion = tsl(erased_points);
        const auto &typed_range_type = static_cast<const TimeSeriesListType &>(typed_range.type());
        const auto &span_conversion_type = static_cast<const TimeSeriesListType &>(span_conversion.type());
        CHECK(&typed_range_type.item_type() == &ordinary.item_type());
        CHECK(&span_conversion_type.item_type() == &ordinary.item_type());
    });
}

TEST_CASE("free fixed TSL inference applies reference guarantees to nested children",
          "[wiring][tsl][reference][feature:wiring][series:tsd][guarantee:ordinary]") {

    const auto source = node([](Out<REF<TSD<i64, TS<i64, TimeSeriesGuarantee::const_>, TimeSeriesGuarantee::const_>,
                                        TimeSeriesGuarantee::ordinary>>) {});

    testing::test_wiring([&] {
        const auto list = tsl(source());
        const auto &type = static_cast<const TimeSeriesListType &>(list.type());
        const auto &dictionary = static_cast<const TimeSeriesDictionaryType &>(type.item_type());

        CHECK(dictionary.guarantee().value() == TimeSeriesGuarantee::ordinary);
        CHECK(dictionary.key_set_type().guarantee().value() == TimeSeriesGuarantee::ordinary);
        CHECK(dictionary.value_type().guarantee().value() == TimeSeriesGuarantee::ordinary);
    });
}

TEST_CASE("a free fixed time series list reuses materialization for the same input type",
          "[wiring][tsl][series:fixed-tsl][representation:free][feature:wiring]"
          "[binding:representation][shape:fan-out]") {

    const auto first_source = node([](Out<TS<float>>) {});

    const auto second_source = node([](Out<TS<float>>) {});

    const auto sink = node([](In<TSL<TS<float>, 2>>) {});
    auto wiring = GraphWiring{};

    const auto context = WiringContext{wiring};
    const auto first = first_source();
    const auto second = second_source();
    const auto list = tsl(first, second);
    (void)sink(list);
    (void)sink(list);

    (void)wiring.complete_pending_wiring();
    REQUIRE(wiring.nodes().size() == 5);
    CHECK(wiring.nodes()[3].inputs()[0] == wiring.nodes()[4].inputs()[0]);
}

TEST_CASE("a free fixed time series list can satisfy a reference input",
          "[wiring][tsl][reference][series:fixed-tsl][representation:free][feature:wiring]"
          "[binding:value-to-reference]") {

    const auto first_source = node([](Out<TS<float>>) {});

    const auto second_source = node([](Out<TS<float>>) {});

    const auto sink = node([](In<REF<TSL<TS<float>, 2>>>) {});
    auto wiring = GraphWiring{};

    const auto context = WiringContext{wiring};
    const auto list = tsl(first_source(), second_source());
    sink(list);

    (void)wiring.complete_pending_wiring();
    REQUIRE(wiring.nodes().size() == 5);
    CHECK(wiring.nodes()[2].signature().name == util::symbol{"tsl"});
    CHECK(wiring.nodes()[3].signature().name == util::symbol{"reference"});
}

TEST_CASE("getitem selects an item from a free fixed time series list without materializing it",
          "[wiring][series:fixed-tsl][representation:free][feature:getitem][feature:operator]") {

    const auto source = node([](Out<TS<float>>) {});
    auto wiring = GraphWiring{};

    const auto context = WiringContext{wiring};
    const auto first = source();
    const auto second = source();
    const auto list = tsl(first, second);
    const auto selected = list[i64{1}];

    STATIC_CHECK(std::same_as<decltype(selected), const wp<>>);
    (void)wiring.complete_pending_wiring();
    CHECK(&selected.type() == &TS<float>::type());
    CHECK(wiring.nodes().size() == 1);
    CHECK(wiring.outputs().size() == 1);
}

TEST_CASE("getitem creates a contained wired output when a stored list item is consumed",
          "[wiring][series:fixed-tsl][representation:native][feature:getitem][feature:operator]") {

    const auto source = node([](Out<TSL<TS<float>, 3>>) {});

    const auto sink = node([](In<TS<float>>) {});
    auto wiring = GraphWiring{};

    const auto context = WiringContext{wiring};
    const auto list = source();
    const auto selected = list[i64{1}];

    CHECK(&selected.type() == &TS<float>::type());
    (void)wiring.complete_pending_wiring();
    CHECK(wiring.outputs().size() == 1);

    sink(selected);
    (void)wiring.complete_pending_wiring();

    REQUIRE(wiring.outputs().size() == 2);
    CHECK(wiring.outputs()[1].parent == 0);
    CHECK(wiring.outputs()[1].item == 1);
}

TEST_CASE("getitem reuses a contained wired output",
          "[wiring][series:fixed-tsl][representation:native][feature:getitem][feature:operator]"
          "[shape:fan-out]") {

    const auto source = node([](Out<TSL<TS<float>, 3>>) {});

    const auto sink = node([](In<TS<float>>) {});
    auto wiring = GraphWiring{};

    const auto context = WiringContext{wiring};
    const auto list = source();
    sink(list[i64{1}]);
    sink(list[i64{1}]);

    (void)wiring.complete_pending_wiring();
    CHECK(wiring.outputs().size() == 2);
}

TEST_CASE("getitem distinguishes items belonging to different wired outputs",
          "[wiring][series:fixed-tsl][representation:native][feature:getitem][feature:operator]") {

    const auto first_source = node([](Out<TSL<TS<float>, 3>>) {});

    const auto second_source = node([](Out<TSL<TS<float>, 3>>) {});

    const auto sink = node([](In<TS<float>>) {});
    auto wiring = GraphWiring{};

    const auto context = WiringContext{wiring};
    sink(first_source()[i64{1}]);
    sink(second_source()[i64{1}]);

    (void)wiring.complete_pending_wiring();
    REQUIRE(wiring.outputs().size() == 4);
    CHECK(wiring.outputs()[1].parent == 0);
    CHECK(wiring.outputs()[3].parent == 2);
}

TEST_CASE("getitem selects an item through a reference-backed fixed list",
          "[feature:wiring][feature:getitem][feature:operator][feature:reference][series:fixed-tsl]"
          "[binding:reference-to-value]") {

    const auto source = node([](Out<REF<TSL<TS<float>, 3>>>) {});

    const auto sink = node([](In<TS<float>>) {});
    auto wiring = GraphWiring{};

    const auto context = WiringContext{wiring};
    const auto selected = source()[i64{1}];

    CHECK(&selected.type() == &REF<TS<float>, TimeSeriesGuarantee::ordinary>::type());
    sink(selected);
    (void)wiring.complete_pending_wiring();
}
