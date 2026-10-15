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
    (void)wiring.complete_pending_wiring();
    REQUIRE(wiring.nodes().size() == 2);
    (void)sink(list);

    (void)wiring.complete_pending_wiring();
    REQUIRE(wiring.nodes().size() == 4);
    CHECK(wiring.nodes()[2].inputs().size() == 2);
}

TEST_CASE("a free fixed time series list reports incompatible item types as a wiring error",
          "[wiring][tsl][series:fixed-tsl][representation:free][feature:wiring]") {
    const auto integer = bg::node([](bg::Out<bg::TS<bg::i64>>) {});
    const auto floating = bg::node([](bg::Out<bg::TS<float>>) {});
    auto wiring = bg::GraphWiring{};

    const auto context = bg::WiringContext{wiring};
    CHECK_THROWS_AS(bg::tsl(integer(), floating()), bg::WiringError);
}

TEST_CASE("a free fixed TSL accepts vectors and spans of wiring points",
          "[wiring][tsl][series:fixed-tsl][representation:free][feature:wiring]"
          "[feature:wiring-materialisation][binding:representation]") {
    const auto first_source = bg::node([](bg::Out<bg::TS<bg::i64>>) {});
    const auto second_source = bg::node([](bg::Out<bg::TS<bg::i64>>) {});
    const auto sink = bg::node([](bg::In<bg::TSL<bg::TS<bg::i64>, 2>>) {});
    auto wiring = bg::GraphWiring{};

    const auto context = bg::WiringContext{wiring};
    const auto first = first_source();
    const auto second = second_source();
    const auto typed_items = std::array{first, second};
    const auto span_list = bg::tsl(std::span{typed_items});
    const auto selected = span_list[bg::i64{1}];

    CHECK(static_cast<bg::WiringPoint>(selected).record() == static_cast<bg::WiringPoint>(second).record());

    const auto make_vector_list = [](bg::wp<bg::TS<bg::i64>> first_item,
                                     bg::wp<bg::TS<bg::i64>> second_item) {
        auto vector_items = std::vector<bg::wp<bg::TS<bg::i64>>>{};
        vector_items.push_back(first_item);
        vector_items.push_back(second_item);
        return bg::tsl(vector_items);
    };
    const auto vector_list = make_vector_list(first, second);
    const auto erased_items = std::vector<bg::WiringPoint>{static_cast<bg::WiringPoint>(first),
                                                           static_cast<bg::WiringPoint>(second)};
    const auto erased_list = bg::tsl(erased_items);
    CHECK(&erased_list.type() == &span_list.type());
    sink(vector_list);

    (void)wiring.complete_pending_wiring();
    REQUIRE(wiring.nodes().size() == 4);
}

TEST_CASE("a free fixed TSL rejects incompatible vector item types",
          "[wiring][tsl][series:fixed-tsl][representation:free][feature:wiring]") {
    const auto integer = bg::node([](bg::Out<bg::TS<bg::i64>>) {});
    const auto floating = bg::node([](bg::Out<bg::TS<float>>) {});

    bg::testing::test_wiring([&] {
        auto items = std::vector<bg::wp<>>{};
        items.emplace_back(integer());
        items.emplace_back(floating());
        CHECK_THROWS_AS(bg::tsl(items), bg::WiringError);
    });
}

TEST_CASE("an empty fixed TSL requires a resolved item type",
          "[wiring][tsl][series:fixed-tsl][representation:free][feature:wiring]") {
    bg::testing::test_wiring([] {
        const auto inferred = bg::tsl<bg::TS<bg::i64>>();
        const auto explicit_type = bg::tsl(bg::TS<bg::i64>::type());
        const auto &inferred_type = static_cast<const bg::TimeSeriesListType &>(inferred.type());
        const auto &explicit_type_type = static_cast<const bg::TimeSeriesListType &>(explicit_type.type());

        CHECK(inferred_type.is_resolved());
        CHECK(inferred_type.item_count() == 0);
        CHECK(&inferred_type.item_type() == &bg::TS<bg::i64>::type());
        CHECK(inferred_type.guarantee().value() == bg::TimeSeriesGuarantee::ordinary);
        CHECK(&explicit_type_type == &inferred_type);
        CHECK_THROWS_AS(bg::tsl(bg::TS<bg::SCALAR>::type()), bg::WiringError);
        CHECK_THROWS_AS(bg::tsl(std::span<const bg::WiringPoint>{}), bg::WiringError);
    });
}

TEST_CASE("free fixed TSL inference uses reference guarantees",
          "[wiring][tsl][reference][feature:wiring][guarantee:ordinary][guarantee:deduplicated]"
          "[guarantee:final][guarantee:const]") {
    const auto ordinary_reference = bg::node([](bg::Out<bg::REF<
                                                   bg::TS<bg::i64, bg::TimeSeriesGuarantee::final>,
                                                   bg::TimeSeriesGuarantee::ordinary>>) {});
    const auto deduplicated_reference = bg::node([](bg::Out<bg::REF<
                                                       bg::TS<bg::i64, bg::TimeSeriesGuarantee::const_>,
                                                       bg::TimeSeriesGuarantee::deduplicated>>) {});
    const auto final_reference = bg::node([](bg::Out<bg::REF<
                                                bg::TS<bg::i64, bg::TimeSeriesGuarantee::const_>,
                                                bg::TimeSeriesGuarantee::final>>) {});
    const auto const_reference = bg::node([](bg::Out<bg::REF<
                                                bg::TS<bg::i64, bg::TimeSeriesGuarantee::const_>,
                                                bg::TimeSeriesGuarantee::const_>>) {});

    bg::testing::test_wiring([&] {
        const auto &ordinary = static_cast<const bg::TimeSeriesListType &>(bg::tsl(ordinary_reference()).type());
        const auto &deduplicated =
            static_cast<const bg::TimeSeriesListType &>(bg::tsl(deduplicated_reference()).type());
        const auto &final = static_cast<const bg::TimeSeriesListType &>(bg::tsl(final_reference()).type());
        const auto &constant = static_cast<const bg::TimeSeriesListType &>(bg::tsl(const_reference()).type());

        CHECK(ordinary.item_type().guarantee().value() == bg::TimeSeriesGuarantee::ordinary);
        CHECK(deduplicated.item_type().guarantee().value() == bg::TimeSeriesGuarantee::ordinary);
        CHECK(final.item_type().guarantee().value() == bg::TimeSeriesGuarantee::final);
        CHECK(constant.item_type().guarantee().value() == bg::TimeSeriesGuarantee::const_);

        const auto ordinary_point = ordinary_reference();
        const auto points = std::vector{ordinary_point};
        const auto typed_range = bg::tsl(points);
        const auto erased_points = std::vector<bg::WiringPoint>{ordinary_point};
        const auto span_conversion = bg::tsl(erased_points);
        const auto &typed_range_type = static_cast<const bg::TimeSeriesListType &>(typed_range.type());
        const auto &span_conversion_type = static_cast<const bg::TimeSeriesListType &>(span_conversion.type());
        CHECK(&typed_range_type.item_type() == &ordinary.item_type());
        CHECK(&span_conversion_type.item_type() == &ordinary.item_type());
    });
}

TEST_CASE("free fixed TSL inference applies reference guarantees to nested children",
          "[wiring][tsl][reference][feature:wiring][series:tsd][guarantee:ordinary]") {
    const auto source = bg::node([](bg::Out<bg::REF<
                                        bg::TSD<bg::i64, bg::TS<bg::i64, bg::TimeSeriesGuarantee::const_>,
                                                bg::TimeSeriesGuarantee::const_>,
                                        bg::TimeSeriesGuarantee::ordinary>>) {});

    bg::testing::test_wiring([&] {
        const auto list = bg::tsl(source());
        const auto &type = static_cast<const bg::TimeSeriesListType &>(list.type());
        const auto &dictionary = static_cast<const bg::TimeSeriesDictionaryType &>(type.item_type());

        CHECK(dictionary.guarantee().value() == bg::TimeSeriesGuarantee::ordinary);
        CHECK(dictionary.key_set_type().guarantee().value() == bg::TimeSeriesGuarantee::ordinary);
        CHECK(dictionary.value_type().guarantee().value() == bg::TimeSeriesGuarantee::ordinary);
    });
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

    (void)wiring.complete_pending_wiring();
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

    (void)wiring.complete_pending_wiring();
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
    const auto selected = list[bg::i64{1}];

    STATIC_CHECK(std::same_as<decltype(selected), const bg::wp<>>);
    (void)wiring.complete_pending_wiring();
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
    const auto list = source();
    const auto selected = list[bg::i64{1}];

    CHECK(&selected.type() == &bg::TS<float>::type());
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
    const auto source = bg::node([](bg::Out<bg::TSL<bg::TS<float>, 3>>) {});
    const auto sink = bg::node([](bg::In<bg::TS<float>>) {});
    auto wiring = bg::GraphWiring{};

    const auto context = bg::WiringContext{wiring};
    const auto list = source();
    sink(list[bg::i64{1}]);
    sink(list[bg::i64{1}]);

    (void)wiring.complete_pending_wiring();
    CHECK(wiring.outputs().size() == 2);
}

TEST_CASE("getitem distinguishes items belonging to different wired outputs",
          "[wiring][series:fixed-tsl][representation:native][feature:getitem][feature:operator]") {
    const auto first_source = bg::node([](bg::Out<bg::TSL<bg::TS<float>, 3>>) {});
    const auto second_source = bg::node([](bg::Out<bg::TSL<bg::TS<float>, 3>>) {});
    const auto sink = bg::node([](bg::In<bg::TS<float>>) {});
    auto wiring = bg::GraphWiring{};

    const auto context = bg::WiringContext{wiring};
    sink(first_source()[bg::i64{1}]);
    sink(second_source()[bg::i64{1}]);

    (void)wiring.complete_pending_wiring();
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
    const auto selected = source()[bg::i64{1}];

    CHECK(&selected.type() == &bg::REF<bg::TS<float>, bg::TimeSeriesGuarantee::ordinary>::type());
    sink(selected);
    (void)wiring.complete_pending_wiring();
}
