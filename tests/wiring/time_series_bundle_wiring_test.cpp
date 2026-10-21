#include <bg/graph.hpp>
#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/node.hpp>
#include <bg/operators.hpp>
#include <bg/testing/test_wiring.hpp>
#include <bg/time_series/tsb/tsb.hpp>
#include <bg/time_series/tsb/details/wiring.hpp>
#include <bg/time_series/tsd/tsd_type.hpp>

#include <catch2/catch_test_macros.hpp>

#include <span>
#include <type_traits>
#include <utility>
#include <vector>

using namespace bg;

namespace {

using namespace bg::literals;

template <template <typename, auto...> typename> struct EmptyBundle {};

template <template <typename, auto...> typename Field> struct TestBundle {
    Field<TS<i64>> count;
    Field<TS<float>> value;
};

template <template <typename, auto...> typename Field> struct ReferenceFieldBundle {
    Field<TS<i64, TimeSeriesGuarantee::final>> value;
    Field<REF<TS<float, TimeSeriesGuarantee::final>, TimeSeriesGuarantee::final>> selected;
};

template <template <typename, auto...> typename Field> struct OptionalFieldBundle {
    Field<TS<i64>, optional> value;
};

template <template <typename, auto...> typename Field> struct DictionaryFieldBundle {
    Field<TSD<i64, TS<i64>>> dictionary;
};

template <template <typename, auto...> typename Field> struct SharedBundle {
    Field<i64> count;
    Field<double> value;
};

struct NamedScalarFixedGraph {
    i64 count;
    wp<TS<float>, "value"_n> value;

    void wire() const {
        CHECK(count == 7);
        CHECK(&value.type() == &TS<float>::type());
    }
};

} // namespace

TEST_CASE("typed TSB construction supports an empty schema",
          "[feature:wiring][series:tsb][schema:named][declaration:concrete]") {
    testing::test_wiring([] {
        const auto value = bundle<TSB<EmptyBundle>>();

        STATIC_CHECK(std::same_as<std::remove_cvref_t<decltype(value)>, wp<TSB<EmptyBundle>>>);
    });
}

TEST_CASE("free TSB materialization rejects borrowed input bindings",
          "[feature:wiring][feature:borrowed-input][series:tsb][representation:free]") {
    auto wiring = GraphWiring{};
    const auto context = WiringContext{wiring};
    const auto input = wiring.borrowed_input(TS<i64>::type());
    const auto fields =
        std::array{WiringArgument{util::symbol{"count"}, i64{7}}, WiringArgument{util::symbol{"value"}, input}};
    const auto bundle = details::make_free_time_series_bundle(fields);
    const auto node_count = wiring.nodes().size();

    CHECK_THROWS_AS(wiring.materialize(bundle, bundle.type()), WiringError);
    CHECK(wiring.nodes().size() == node_count);
}

TEST_CASE("a typed TSB wiring point exposes typed fields",
          "[feature:getattr][feature:wiring][series:tsb][schema:named][representation:native]") {

    const auto source = node([](Out<TSB<TestBundle>>) {});

    const auto sink = node([](In<TS<i64>>) {});
    auto wiring = GraphWiring{};

    const auto context = WiringContext{wiring};
    const auto bundle = source();
    (void)wiring.complete_pending_wiring(static_cast<WiringPoint>(bundle));
    const auto count = bundle->count;

    STATIC_CHECK(std::same_as<decltype(count), const wp<TS<i64>>>);
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
    using InputFields = typename In<TSB<OptionalFieldBundle>>::fields_type;
    using InputField = std::remove_cvref_t<decltype(std::declval<InputFields>().value)>;

    STATIC_CHECK(InputField::validity == optional);

    const auto source = node([](Out<TSB<OptionalFieldBundle>>) {});
    auto wiring = GraphWiring{};

    const auto context = WiringContext{wiring};
    const auto value = source()->value;

    STATIC_CHECK(std::same_as<decltype(value), const wp<TS<i64>>>);
}

TEST_CASE("unused typed TSB fields remain unmaterialized",
          "[feature:getattr][feature:wiring][series:tsb][schema:named][representation:native]") {

    const auto source = node([](Out<TSB<TestBundle>>) {});
    auto wiring = GraphWiring{};

    const auto context = WiringContext{wiring};
    const auto bundle = source();
    (void)wiring.complete_pending_wiring(static_cast<WiringPoint>(bundle));
    const auto count = bundle->count;

    CHECK(&count.type() == &TS<i64>::type());
    CHECK(wiring.outputs().size() == 1);
}

TEST_CASE("typed TSB field access reuses its wiring record",
          "[feature:getattr][feature:wiring][series:tsb][schema:named][representation:native]"
          "[shape:fan-out]") {

    const auto source = node([](Out<TSB<TestBundle>>) {});
    auto wiring = GraphWiring{};

    const auto context = WiringContext{wiring};
    const auto bundle = source();
    const auto first = bundle->count;
    const auto second = bundle->count;
    (void)wiring.complete_pending_wiring(static_cast<WiringPoint>(bundle));

    CHECK(static_cast<WiringPoint>(first).record() == static_cast<WiringPoint>(second).record());
    CHECK(wiring.outputs().size() == 1);
}

TEST_CASE("typed field access preserves free TSB sources",
          "[feature:getattr][feature:wiring][series:tsb][schema:named][representation:free]") {
    using namespace bg::literals;

    const auto count_source = node([](Out<TS<i64>>) {});

    const auto value_source = node([](Out<TS<float>>) {});

    const auto sink = node([](In<TS<i64>>) {});
    auto wiring = GraphWiring{};

    const auto context = WiringContext{wiring};
    const auto count = count_source();
    const auto value = value_source();
    const auto bundle = bg::bundle("value"_n = value, "count"_n = count).as<TSB<TestBundle>>();
    sink(bundle->count);

    (void)wiring.complete_pending_wiring();
    REQUIRE(wiring.nodes().size() == 3);
    CHECK(wiring.nodes().back().inputs().front().index() == 0);
    CHECK(wiring.outputs().size() == 2);
}

TEST_CASE("typed TSB construction preserves field annotations",
          "[feature:wiring][feature:variadic-wiring][series:tsb][schema:named]"
          "[input-guarantee:valid-items][representation:free]") {

    const auto dictionary = node([](Out<TSD<i64, TS<i64>>>) {});

    const auto receiver = graph("name"_n = "typed_bundle_receiver", [](wp<TSB<SCHEMA>, "values"_n, var_kwargs> values) {
        const auto field = values.find("dictionary");
        REQUIRE(field);
        CHECK(field->annotations().valid_items);
        CHECK_FALSE(field->annotations().optional);
    });

    testing::test_wiring([&] {
        const auto fields = bundle<TSB<DictionaryFieldBundle>>("dictionary"_n = valid_items(dictionary()));
        receiver("values"_n = fields);
    });
}

TEST_CASE("typed TSB construction reports invalid schema arguments as wiring errors",
          "[feature:wiring][series:tsb][schema:named][declaration:concrete]") {
    using namespace bg::literals;

    CHECK_THROWS_AS(testing::test_wiring([] { (void)bundle<TSB<SharedBundle>>(1); }), WiringError);
    CHECK_THROWS_AS(testing::test_wiring([] { (void)bundle<TSB<SharedBundle>>("missing"_n = 1, "value"_n = 2.5); }),
                    WiringError);
    CHECK_THROWS_AS(
        testing::test_wiring([] { (void)bundle<TSB<SharedBundle>>("count"_n = 1, "count"_n = 2, "value"_n = 2.5); }),
        WiringError);
    CHECK_THROWS_AS(testing::test_wiring([] { (void)bundle<TSB<SharedBundle>>("value"_n = 2.5, 1); }), WiringError);
    CHECK_THROWS_AS(testing::test_wiring([] { (void)bundle<TSB<SharedBundle>>(1.0, 2.5); }), WiringError);
}

TEST_CASE("getattr selects a field from an unnamed free TSB",
          "[feature:getattr][feature:operator][series:tsb][schema:unnamed][representation:free]") {
    using namespace bg::literals;
    using namespace bg::util::literals;

    const auto count_source = node([](Out<TS<i64>>) {});

    const auto value_source = node([](Out<TS<float>>) {});
    auto wiring = GraphWiring{};

    const auto context = WiringContext{wiring};
    const auto bundle = bg::bundle("count"_n = count_source(), "value"_n = value_source());
    const auto count = operators::getattr(bundle, "count"_sym);

    (void)wiring.complete_pending_wiring();
    CHECK(&count.type() == &TS<i64>::type());
    CHECK(wiring.nodes().size() == 2);
    CHECK(wiring.outputs().size() == 2);
}

TEST_CASE("a free TSB accepts a span of named wiring arguments",
          "[feature:wiring][series:tsb][schema:named][representation:free]") {

    const auto count_source = node([](Out<TS<i64>>) {});

    const auto value_source = node([](Out<TS<float>>) {});

    const auto sink = node([](In<TSB<TestBundle>>) {});
    auto wiring = GraphWiring{};
    const auto context = WiringContext{wiring};

    const auto count = count_source();
    const auto value = value_source();
    const auto free_bundle = [&] {
        auto fields = std::vector<WiringArgument>{};
        fields.emplace_back(util::symbol{"count"}, count);
        fields.emplace_back(util::symbol{"value"}, value);
        return bundle(fields);
    }();

    sink(free_bundle.as<TSB<TestBundle>>());
    (void)wiring.complete_pending_wiring();

    CHECK(wiring.outputs().size() == 3);
}

TEST_CASE("a free TSB span rejects duplicate field names",
          "[feature:wiring][series:tsb][schema:named][representation:free]") {

    const auto source = node([](Out<TS<i64>>) {});

    testing::test_wiring([&] {
        auto fields = std::vector<WiringArgument>{};
        fields.emplace_back(util::symbol{"value"}, source());
        fields.emplace_back(util::symbol{"value"}, source());
        CHECK_THROWS_AS(bundle(std::span<const WiringArgument>{fields}), WiringError);
    });
}

TEST_CASE("a free TSB span requires named fields and projects scalars to const time series",
          "[feature:wiring][series:tsb][schema:named][representation:free]") {

    const auto source = node([](Out<TS<float>>) {});

    testing::test_wiring([&] {
        auto unnamed = std::vector<WiringArgument>{};
        unnamed.emplace_back(source());
        CHECK_THROWS_AS(bundle(std::span<const WiringArgument>{unnamed}), WiringError);

        auto scalar = std::vector<WiringArgument>{};
        scalar.emplace_back(util::symbol{"count"}, i64{7});
        scalar.emplace_back(util::symbol{"value"}, source());
        const auto fields = bundle(std::span<const WiringArgument>{scalar});
        const auto &type = static_cast<const TimeSeriesBundleType &>(fields.type());
        CHECK(&type.item_type(0) == &TS<i64, TimeSeriesGuarantee::const_>::type());

        const auto fixed = graph<NamedScalarFixedGraph>();
        fixed(kwargs(fields));

        const auto sink = node([](In<TSB<TestBundle>>) {});
        sink(fields.as<TSB<TestBundle>>());

        const auto named_fields = bundle("count"_n = i64{7}, "value"_n = source());
        const auto &named_type = static_cast<const TimeSeriesBundleType &>(named_fields.type());
        CHECK(&named_type.item_type(0) == &TS<i64, TimeSeriesGuarantee::const_>::type());
        fixed(kwargs(named_fields));
        sink(named_fields.as<TSB<TestBundle>>());
    });
}

TEST_CASE("field selection from an argument-backed TSB does not materialize the bundle",
          "[feature:wiring][feature:getattr][series:tsb][schema:named][representation:free]") {

    const auto source = node([](Out<TS<float>>) {});
    auto wiring = GraphWiring{};
    const auto context = WiringContext{wiring};

    auto fields = std::vector<WiringArgument>{};
    fields.emplace_back(util::symbol{"count"}, i64{7});
    fields.emplace_back(util::symbol{"value"}, source());
    const auto bundle = bg::bundle(std::span<const WiringArgument>{fields});
    const auto value = details::time_series_bundle_field(bundle, util::symbol{"value"});
    const auto count = details::time_series_bundle_field(bundle, util::symbol{"count"});
    CHECK(&count.type() == &TS<i64, TimeSeriesGuarantee::const_>::type());

    (void)wiring.complete_pending_wiring(value);
    (void)wiring.complete_pending_wiring(count);
    CHECK(wiring.nodes().size() == 2);
    CHECK(wiring.outputs().size() == 2);
}

TEST_CASE("getattr on a TSB reference returns a reference to a value field",
          "[feature:getattr][feature:operator][series:tsb][series:reference][guarantee:final]") {
    using namespace bg::util::literals;

    const auto source = node([](Out<TSB<ReferenceFieldBundle>>) {});
    const auto reference =
        node([](In<REF<TSB<ReferenceFieldBundle>, TimeSeriesGuarantee::ordinary>> input,
                Out<REF<TSB<ReferenceFieldBundle>, TimeSeriesGuarantee::ordinary>> output) { output.copy(input); });

    testing::test_wiring([&] {
        const auto selected = operators::getattr(reference(source()), "value"_sym);

        CHECK(&selected.type() == &REF<TS<i64, TimeSeriesGuarantee::final>, TimeSeriesGuarantee::ordinary>::type());
    });
}

TEST_CASE("getattr on a TSB reference preserves a reference field guarantee",
          "[feature:getattr][feature:operator][series:tsb][series:reference][guarantee:final]") {
    using namespace bg::util::literals;

    const auto source = node([](Out<TSB<ReferenceFieldBundle, TimeSeriesGuarantee::final>>) {});
    const auto reference =
        node([](In<REF<TSB<ReferenceFieldBundle, TimeSeriesGuarantee::final>, TimeSeriesGuarantee::const_>> input,
                Out<REF<TSB<ReferenceFieldBundle, TimeSeriesGuarantee::final>, TimeSeriesGuarantee::const_>> output) {
            output.copy(input);
        });

    testing::test_wiring([&] {
        const auto selected = operators::getattr(reference(source()), "selected"_sym);

        CHECK(&selected.type() == &REF<TS<float, TimeSeriesGuarantee::final>, TimeSeriesGuarantee::final>::type());
    });
}
