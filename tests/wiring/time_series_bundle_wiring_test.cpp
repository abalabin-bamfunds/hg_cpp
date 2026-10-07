#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/node.hpp>
#include <bg/operators.hpp>
#include <bg/testing/test_wiring.hpp>
#include <bg/time_series/tsb/tsb.hpp>

#include <catch2/catch_test_macros.hpp>

#include <type_traits>
#include <utility>

namespace {

template <template <typename, auto...> typename> struct EmptyBundle {};

template <template <typename, auto...> typename Field> struct TestBundle {
    Field<bg::TS<bg::i64>> count;
    Field<bg::TS<float>> value;
};

template <template <typename, auto...> typename Field> struct ReferenceFieldBundle {
    Field<bg::TS<bg::i64, bg::TimeSeriesGuarantee::final>> value;
    Field<bg::REF<bg::TS<float, bg::TimeSeriesGuarantee::final>, bg::TimeSeriesGuarantee::final>> selected;
};

template <template <typename, auto...> typename Field> struct OptionalFieldBundle {
    Field<bg::TS<bg::i64>, bg::optional> value;
};

template <template <typename, auto...> typename Field> struct SharedBundle {
    Field<bg::i64> count;
    Field<double> value;
};

} // namespace

TEST_CASE("typed TSB construction supports an empty schema",
          "[feature:wiring][series:tsb][schema:named][declaration:concrete]") {
    bg::testing::test_wiring([] {
        const auto value = bg::bundle<bg::TSB<EmptyBundle>>();

        STATIC_CHECK(std::same_as<std::remove_cvref_t<decltype(value)>, bg::wp<bg::TSB<EmptyBundle>>>);
    });
}

TEST_CASE("a typed TSB wiring point exposes typed fields",
          "[feature:getattr][feature:wiring][series:tsb][schema:named][representation:native]") {
    const auto source = bg::node([](bg::Out<bg::TSB<TestBundle>>) {});
    const auto sink = bg::node([](bg::In<bg::TS<bg::i64>>) {});
    auto wiring = bg::GraphWiring{};

    const auto context = bg::WiringContext{wiring};
    const auto bundle = source();
    (void)wiring.complete_pending_wiring(static_cast<bg::WiringPoint>(bundle));
    const auto count = bundle->count;

    STATIC_CHECK(std::same_as<decltype(count), const bg::wp<bg::TS<bg::i64>>>);
    STATIC_CHECK(sizeof(decltype(bundle)) == sizeof(void *));
    STATIC_CHECK(std::is_trivially_copyable_v<decltype(bundle)>);
    CHECK(wiring.outputs().size() == 1);

    sink(count);
    (void)wiring.complete_pending_wiring();

    REQUIRE(wiring.outputs().size() == 2);
    CHECK(wiring.outputs()[1].parent == 0);
    CHECK(wiring.outputs()[1].item == 0);
}

TEST_CASE("a TSB field projects its properties to its input view",
          "[tsb][feature:getattr][feature:wiring][series:tsb][schema:named][validity]") {
    using InputFields = typename bg::In<bg::TSB<OptionalFieldBundle>>::fields_type;
    using InputField = std::remove_cvref_t<decltype(std::declval<InputFields>().value)>;

    STATIC_CHECK(InputField::validity == bg::optional);

    const auto source = bg::node([](bg::Out<bg::TSB<OptionalFieldBundle>>) {});
    auto wiring = bg::GraphWiring{};

    const auto context = bg::WiringContext{wiring};
    const auto value = source()->value;

    STATIC_CHECK(std::same_as<decltype(value), const bg::wp<bg::TS<bg::i64>>>);
}

TEST_CASE("unused typed TSB fields remain unmaterialized",
          "[feature:getattr][feature:wiring][series:tsb][schema:named][representation:native]") {
    const auto source = bg::node([](bg::Out<bg::TSB<TestBundle>>) {});
    auto wiring = bg::GraphWiring{};

    const auto context = bg::WiringContext{wiring};
    const auto bundle = source();
    (void)wiring.complete_pending_wiring(static_cast<bg::WiringPoint>(bundle));
    const auto count = bundle->count;

    CHECK(&count.type() == &bg::TS<bg::i64>::type());
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
    (void)wiring.complete_pending_wiring(static_cast<bg::WiringPoint>(bundle));

    CHECK(static_cast<bg::WiringPoint>(first).record() == static_cast<bg::WiringPoint>(second).record());
    CHECK(wiring.outputs().size() == 1);
}

TEST_CASE("typed field access preserves free TSB sources",
          "[feature:getattr][feature:wiring][series:tsb][schema:named][representation:free]") {
    using namespace bg::literals;

    const auto count_source = bg::node([](bg::Out<bg::TS<bg::i64>>) {});
    const auto value_source = bg::node([](bg::Out<bg::TS<float>>) {});
    const auto sink = bg::node([](bg::In<bg::TS<bg::i64>>) {});
    auto wiring = bg::GraphWiring{};

    const auto context = bg::WiringContext{wiring};
    const auto count = count_source();
    const auto value = value_source();
    const auto bundle = bg::bundle("value"_n = value, "count"_n = count).as<bg::TSB<TestBundle>>();
    sink(bundle->count);

    (void)wiring.complete_pending_wiring();
    REQUIRE(wiring.nodes().size() == 3);
    CHECK(wiring.nodes().back().inputs().front().index() == 0);
    CHECK(wiring.outputs().size() == 2);
}

TEST_CASE("typed TSB construction reports invalid schema arguments as wiring errors",
          "[feature:wiring][series:tsb][schema:named][declaration:concrete]") {
    using namespace bg::literals;

    CHECK_THROWS_AS(
        bg::testing::test_wiring([] { (void)bg::bundle<bg::TSB<SharedBundle>>(1); }), bg::WiringError);
    CHECK_THROWS_AS(
        bg::testing::test_wiring(
            [] { (void)bg::bundle<bg::TSB<SharedBundle>>("missing"_n = 1, "value"_n = 2.5); }),
        bg::WiringError);
    CHECK_THROWS_AS(
        bg::testing::test_wiring([] {
            (void)bg::bundle<bg::TSB<SharedBundle>>(
                "count"_n = 1, "count"_n = 2, "value"_n = 2.5);
        }),
        bg::WiringError);
    CHECK_THROWS_AS(
        bg::testing::test_wiring(
            [] { (void)bg::bundle<bg::TSB<SharedBundle>>("value"_n = 2.5, 1); }),
        bg::WiringError);
    CHECK_THROWS_AS(
        bg::testing::test_wiring([] { (void)bg::bundle<bg::TSB<SharedBundle>>(1.0, 2.5); }),
        bg::WiringError);
}

TEST_CASE("getattr selects a field from an unnamed free TSB",
          "[feature:getattr][feature:operator][series:tsb][schema:unnamed][representation:free]") {
    using namespace bg::literals;
    using namespace bg::util::literals;

    const auto count_source = bg::node([](bg::Out<bg::TS<bg::i64>>) {});
    const auto value_source = bg::node([](bg::Out<bg::TS<float>>) {});
    auto wiring = bg::GraphWiring{};

    const auto context = bg::WiringContext{wiring};
    const auto bundle = bg::bundle("count"_n = count_source(), "value"_n = value_source());
    const auto count = bg::operators::getattr(bundle, "count"_sym);

    (void)wiring.complete_pending_wiring();
    CHECK(&count.type() == &bg::TS<bg::i64>::type());
    CHECK(wiring.nodes().size() == 2);
    CHECK(wiring.outputs().size() == 2);
}

TEST_CASE("getattr on a TSB reference returns a reference to a value field",
          "[feature:getattr][feature:operator][series:tsb][series:reference][guarantee:final]") {
    using namespace bg::util::literals;

    const auto source = bg::node([](bg::Out<bg::TSB<ReferenceFieldBundle>>) {});
    const auto reference =
        bg::node([](bg::In<bg::REF<bg::TSB<ReferenceFieldBundle>, bg::TimeSeriesGuarantee::ordinary>> input,
                    bg::Out<bg::REF<bg::TSB<ReferenceFieldBundle>, bg::TimeSeriesGuarantee::ordinary>> output) {
            output.copy(input);
        });

    bg::testing::test_wiring([&] {
        const auto selected = bg::operators::getattr(reference(source()), "value"_sym);

        CHECK(&selected.type() ==
              &bg::REF<bg::TS<bg::i64, bg::TimeSeriesGuarantee::final>, bg::TimeSeriesGuarantee::ordinary>::type());
    });
}

TEST_CASE("getattr on a TSB reference preserves a reference field guarantee",
          "[feature:getattr][feature:operator][series:tsb][series:reference][guarantee:final]") {
    using namespace bg::util::literals;

    const auto source = bg::node([](bg::Out<bg::TSB<ReferenceFieldBundle, bg::TimeSeriesGuarantee::final>>) {});
    const auto reference = bg::node(
        [](bg::In<
               bg::REF<bg::TSB<ReferenceFieldBundle, bg::TimeSeriesGuarantee::final>, bg::TimeSeriesGuarantee::const_>>
               input,
           bg::Out<
               bg::REF<bg::TSB<ReferenceFieldBundle, bg::TimeSeriesGuarantee::final>, bg::TimeSeriesGuarantee::const_>>
               output) { output.copy(input); });

    bg::testing::test_wiring([&] {
        const auto selected = bg::operators::getattr(reference(source()), "selected"_sym);

        CHECK(&selected.type() ==
              &bg::REF<bg::TS<float, bg::TimeSeriesGuarantee::final>, bg::TimeSeriesGuarantee::final>::type());
    });
}
