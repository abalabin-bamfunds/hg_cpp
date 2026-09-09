#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/node.hpp>
#include <bg/operators.hpp>
#include <bg/time_series/tsb/tsb.hpp>

#include <catch2/catch_test_macros.hpp>

#include <type_traits>

namespace {

template <template <typename> typename Field> struct TestBundle {
    Field<bg::TS<int>> count;
    Field<bg::TS<float>> value;
};

} // namespace

TEST_CASE("a typed TSB wiring point exposes typed fields",
          "[feature:getattr][feature:wiring][series:tsb][schema:named][representation:native]") {
    const auto source = bg::node([](bg::Out<bg::TSB<TestBundle>>) {});
    const auto sink = bg::node([](bg::In<bg::TS<int>>) {});
    auto wiring = bg::GraphWiring{};

    const auto context = bg::WiringContext{wiring};
    const auto bundle = source();
    const auto count = bundle->count;

    STATIC_CHECK(std::same_as<decltype(count), const bg::wp<bg::TS<int>>>);
    STATIC_CHECK(sizeof(decltype(bundle)) == sizeof(void *));
    STATIC_CHECK(std::is_trivially_copyable_v<decltype(bundle)>);
    CHECK(wiring.outputs().size() == 1);

    sink(count);

    REQUIRE(wiring.outputs().size() == 2);
    CHECK(wiring.outputs()[1].parent == 0);
    CHECK(wiring.outputs()[1].item == 0);
}

TEST_CASE("unused typed TSB fields remain unmaterialized",
          "[feature:getattr][feature:wiring][series:tsb][schema:named][representation:native]") {
    const auto source = bg::node([](bg::Out<bg::TSB<TestBundle>>) {});
    auto wiring = bg::GraphWiring{};

    const auto context = bg::WiringContext{wiring};
    const auto count = source()->count;

    CHECK(&count.type() == &bg::TS<int>::type());
    CHECK(wiring.outputs().size() == 1);
}

TEST_CASE("typed TSB field access reuses its wiring record",
          "[feature:getattr][feature:wiring][series:tsb][schema:named][representation:native]"
          "[shape:fan-out]") {
    const auto source = bg::node([](bg::Out<bg::TSB<TestBundle>>) {});
    auto wiring = bg::GraphWiring{};

    const auto context = bg::WiringContext{wiring};
    const auto bundle = source();
    const auto first = bundle->count;
    const auto second = bundle->count;

    CHECK(&bg::details::wiring_record(first) == &bg::details::wiring_record(second));
    CHECK(wiring.outputs().size() == 1);
}

TEST_CASE("typed field access preserves free TSB sources",
          "[feature:getattr][feature:wiring][series:tsb][schema:named][representation:free]") {
    using namespace bg::literals;

    const auto count_source = bg::node([](bg::Out<bg::TS<int>>) {});
    const auto value_source = bg::node([](bg::Out<bg::TS<float>>) {});
    const auto sink = bg::node([](bg::In<bg::TS<int>>) {});
    auto wiring = bg::GraphWiring{};

    const auto context = bg::WiringContext{wiring};
    const auto count = count_source();
    const auto value = value_source();
    const auto bundle = bg::bundle("value"_n = value, "count"_n = count).as<bg::TSB<TestBundle>>();
    sink(bundle->count);

    REQUIRE(wiring.nodes().size() == 3);
    CHECK(wiring.nodes().back().input_output_indices().front() == 0);
    CHECK(wiring.outputs().size() == 2);
}

TEST_CASE("getattr selects a field from an unnamed free TSB",
          "[feature:getattr][feature:operator][series:tsb][schema:unnamed][representation:free]") {
    using namespace bg::literals;
    using namespace bg::util::literals;

    const auto count_source = bg::node([](bg::Out<bg::TS<int>>) {});
    const auto value_source = bg::node([](bg::Out<bg::TS<float>>) {});
    auto wiring = bg::GraphWiring{};

    const auto context = bg::WiringContext{wiring};
    const auto bundle = bg::bundle("count"_n = count_source(), "value"_n = value_source());
    const auto count = bg::operators::getattr(bundle, "count"_sym);

    CHECK(&count.type() == &bg::TS<int>::type());
    CHECK(wiring.nodes().size() == 2);
    CHECK(wiring.outputs().size() == 2);
}
