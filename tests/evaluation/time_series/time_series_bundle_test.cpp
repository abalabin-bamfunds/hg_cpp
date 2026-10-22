#include <bg/graph.hpp>
#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/nested.hpp>
#include <bg/node.hpp>
#include <bg/run_graph.hpp>
#include <bg/scalar/bundle/value.hpp>
#include <bg/scalar/dict/scalar.hpp>
#include <bg/scalar/list/scalar.hpp>
#include <bg/testing/test_graph.hpp>
#include <bg/time_series/tsb/tsb.hpp>
#include <bg/time_series/tsb/tsb_input.hpp>
#include <bg/time_series/tsb/tsb_output.hpp>
#include <bg/time_series/tsl/tsl_input.hpp>
#include <bg/time_series/tsl/tsl_output.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <optional>
#include <span>
#include <utility>
#include <vector>

using namespace bg;
using namespace bg::literals;

namespace {

template <template <typename, auto...> typename Field> struct BaseBundle {
    Field<TS<i64>> count;
};

template <template <typename, auto...> typename Field> struct DerivedBundle : bundle_extends<BaseBundle, Field> {
    Field<TS<float>> value;
};

} // namespace

TEST_CASE("node code selects a named bundle field",
          "[evaluation][tsb][feature:getitem][series:tsb][schema:named][scalar:integer][scalar:floating-point]"
          "[representation:native][declaration:concrete][binding:direct]") {
    const auto count = node([](In<TSB<DerivedBundle>> input) { return *input->count; });

    const auto actual = testing::test_graph(count, {{{"count", 2}, {"value", 3.5F}}});

    CHECK(actual.equals({2}));
}

namespace {

const auto base_bundle_count = [](In<TSB<BaseBundle>> input) { return *input->count; };

const auto derived_bundle_sum = [](In<TSB<DerivedBundle>> input) {
    return static_cast<float>(*input->count) + *input->value;
};

} // namespace

TEST_CASE("a typed bundle publishes inherited and direct fields",
          "[evaluation][tsb][series:tsb][feature:collection-change][transition:update][scalar:integer]"
          "[scalar:floating-point][representation:native][declaration:concrete][binding:direct]"
          "[schema:named]") {
    struct Observation {
        i64 count;
        double value;
        bool count_updated;
        bool value_updated;
    };

    const auto observe = node([](In<TSB<DerivedBundle>> input) {
        return Observation{
            .count = *input->count,
            .value = *input->value,
            .count_updated = input.item_updated(0),
            .value_updated = input.item_updated(1),
        };
    });

    const auto actual =
        testing::test_graph(observe, {{{"count", 2}, {"value", 3.5F}}, {{"count", 4}}, {{"value", 6.5F}}});

    CHECK(actual.equals({{{"count", 2}, {"value", 3.5}, {"count_updated", true}, {"value_updated", true}},
                         {{"count", 4}, {"value", 3.5}, {"count_updated", true}, {"value_updated", false}},
                         {{"count", 4}, {"value", 6.5}, {"count_updated", false}, {"value_updated", true}}}));
}

TEST_CASE("a span-backed free TSB retains its fields through materialisation and evaluation",
          "[evaluation][tsb][series:tsb][free][representation:free][feature:wiring-materialisation]"
          "[schema:named]") {
    const auto sum = node(derived_bundle_sum);

    const auto graph = [&](wp<TS<i64>> count, wp<TS<float>> value) {
        const auto collection = [&] {
            auto fields = std::vector<WiringArgument>{};
            fields.emplace_back(util::symbol{"count"}, count);
            fields.emplace_back(util::symbol{"value"}, value);
            return bundle(std::span<const WiringArgument>{fields});
        }();
        return sum(collection);
    };

    const auto result = testing::test_graph(graph, {1, 2}, {10.0F, 20.0F});

    CHECK(result.equals({11.0F, 22.0F}));
}

namespace {

template <template <typename, auto...> typename> struct EmptyBundle {};

template <template <typename, auto...> typename Field> struct SharedBundle {
    Field<i64> count;
    Field<double> value;
};

} // namespace

TEST_CASE("an empty free TSB publishes valid at graph start",
          "[evaluation][tsb][series:tsb][free][feature:wiring-materialisation][representation:free]"
          "[transition:start]") {
    const auto observe = node([](In<TSB<EmptyBundle>> input) { return input.valid() ? 1 : 0; });

    const auto graph = [&] {
        const auto fields = std::vector<WiringArgument>{};
        return observe(bundle(fields));
    };

    CHECK(testing::test_graph(graph).equals({1}));
}

TEST_CASE("a scalar schema lifts mixed scalar and time series fields into a TSB",
          "[feature:wiring][feature:constant][series:tsb][schema:named][scalar:hierarchical]") {
    const auto graph = [](wp<TS<i64>> count) { return bundle<SharedBundle>(count, 2.5); };

    const auto actual = testing::test_graph(graph, {1, 2});

    CHECK(actual.equals({{{"count", 1}, {"value", 2.5}}, {{"count", 2}}}));
}

TEST_CASE("an explicit TSB construction lifts every scalar field",
          "[feature:wiring][feature:constant][series:tsb][schema:named][scalar:hierarchical]") {
    const auto graph = [] { return bundle<TSB<SharedBundle>>(i64{1}, 2.5); };

    const auto actual = testing::test_graph(graph);

    CHECK(actual.equals({{{"count", 1}, {"value", 2.5}}}));
}

TEST_CASE("typed TSB construction uses inherited logical field order",
          "[feature:wiring][feature:constant][series:tsb][schema:named][binding:covariant]") {
    const auto graph = [] { return bundle<TSB<DerivedBundle>>(ScalarValue<i64>{1}, ScalarValue<float>{2.5F})->value; };

    const auto actual = testing::test_graph(graph);

    CHECK(actual.equals({2.5F}));
}

TEST_CASE("an explicit time series schema selects TSB construction for scalar arguments",
          "[feature:wiring][feature:constant][series:tsb][schema:named][declaration:concrete]") {
    const auto graph = [] { return bundle<BaseBundle>(ScalarValue<i64>{1})->count; };

    const auto actual = testing::test_graph(graph);

    CHECK(actual.equals({1}));
}

TEST_CASE("a typed TSB wiring point selects an inherited field",
          "[evaluation][series:tsb][feature:getattr][schema:named][representation:native]"
          "[transition:update]") {
    const auto graph = [](wp<TSB<DerivedBundle>> input) { return input->count; };

    const auto actual = testing::test_graph(graph, {{{"count", 2}, {"value", 3.5F}}});

    CHECK(actual.equals({2}));
}

TEST_CASE("a typed free TSB wiring point selects its original source",
          "[evaluation][series:tsb][feature:getattr][schema:named][representation:free]"
          "[transition:update]") {
    const auto graph = [](wp<TS<i64>> count, wp<TS<float>> value) {
        return bundle("value"_n = value, "count"_n = count).as<TSB<DerivedBundle>>()->count;
    };

    const auto actual = testing::test_graph(graph, {1, 2, 3}, {0.5F, 1.5F, 2.5F});

    CHECK(actual.equals({1, 2, 3}));
}

TEST_CASE("a free bundle follows the receiving named schema",
          "[evaluation][tsb][series:tsb][free][representation:free][feature:collection-change]"
          "[scalar:integer][scalar:floating-point][declaration:concrete][binding:direct]") {
    const auto sum = node(derived_bundle_sum);

    const auto graph = [&](wp<TS<i64>> count, wp<TS<float>> value) {
        return sum(bundle("value"_n = value, "count"_n = count));
    };

    const auto result = testing::test_graph(graph, {1, 2}, {0.5F, 1.5F});

    CHECK(result.equals({1.5F, 3.5F}));
}

TEST_CASE("a named bundle input accepts extra free bundle fields",
          "[evaluation][tsb][series:tsb][wiring][schema:named][scalar:integer][scalar:floating-point]"
          "[representation:native][declaration:concrete][binding:direct][feature:wiring]") {
    const auto sum = node(derived_bundle_sum);

    const auto graph = [&](wp<TS<i64>> count, wp<TS<float>> value) {
        return sum(bundle("extra"_n = count, "value"_n = value, "count"_n = count));
    };

    const auto result = testing::test_graph(graph, {1, 2}, {0.5F, 1.5F});

    CHECK(result.equals({1.5F, 3.5F}));
}

TEST_CASE("a base bundle input accepts a derived bundle",
          "[evaluation][tsb][series:tsb][inheritance][feature:collection-change][scalar:integer]"
          "[scalar:floating-point][representation:native][declaration:concrete][binding:covariant]"
          "[schema:named]") {
    const auto value = node(base_bundle_count);

    const auto graph = [&](wp<TSB<DerivedBundle>> input) { return value(input); };

    const auto actual = testing::test_graph(graph, {{{"count", 2}, {"value", 3.5F}}});

    CHECK(actual.equals({2}));
}

namespace {

template <template <typename, auto...> typename Field> struct RedefinedBundle : bundle_extends<BaseBundle, Field> {
    Field<TS<i64>> count;
    Field<TS<float>> value;
};

} // namespace

TEST_CASE("a redefined bundle field shares its inherited storage",
          "[evaluation][tsb][series:tsb][inheritance][feature:collection-change][schema:named]"
          "[scalar:integer][scalar:floating-point][representation:native][declaration:concrete]"
          "[binding:direct]") {
    const auto value = node(base_bundle_count);

    const auto graph = [&](wp<TSB<RedefinedBundle>> input) { return value(input); };

    const auto actual = testing::test_graph(graph, {{{"count", 4}, {"value", 2.5F}}});

    CHECK(actual.equals({4}));
}

namespace {

template <template <typename, auto...> typename Field> struct NestedBundle {
    Field<TSB<BaseBundle>> nested;
    Field<TS<float>> value;
};

} // namespace

TEST_CASE("a bundle field can contain another bundle",
          "[evaluation][tsb][series:tsb][nested][feature:collection-change][schema:named]"
          "[scalar:integer][scalar:floating-point][representation:native][declaration:concrete]"
          "[binding:direct]") {
    const auto sum =
        node([](In<TSB<NestedBundle>> input) { return static_cast<float>(*input->nested->count) + *input->value; });

    const auto actual = testing::test_graph(sum, {{{"nested", {{"count", 3}}}, {"value", 1.5F}}});

    CHECK(actual.equals({4.5F}));
}

TEST_CASE("fixed time series lists can contain runtime-sized time series",
          "[evaluation][tsl][series:fixed-tsl][tsb][series:tsb][feature:collection-change]"
          "[scalar:integer][scalar:floating-point][representation:native][declaration:concrete]"
          "[binding:direct][schema:named]") {
    const auto value = node([](In<TSL<TSB<DerivedBundle>, 2>> input) { return *input[1]->value; });

    const auto actual = testing::test_graph(value, {{{1, {{"value", 4.5F}}}}});

    CHECK(actual.equals({4.5F}));
}

namespace {

template <template <typename, auto...> typename Field> struct NestedSharedBundle {
    Field<Bundle<SharedBundle>> value;
};

} // namespace

TEST_CASE("a hierarchical scalar is lifted through a shared TSB schema",
          "[feature:wiring][feature:constant][series:value][series:tsb][scalar:hierarchical][shape:nested]") {
    auto memory = allocator{};
    const auto read =
        node([](In<TS<Bundle<SharedBundle>>> input) { return scalar_cast<i64>(input.value()[0]).value(); });

    const auto graph = [&] {
        const auto value = bundle<SharedBundle>(memory, i64{3}, 2.5);
        return read(bundle<TSB<NestedSharedBundle>>(value)->value);
    };

    const auto actual = testing::test_graph(graph);

    CHECK(actual.equals({3}));
}

TEST_CASE("a hierarchical time series output accepts input views and suppresses equal updates",
          "[feature:scalar-api][series:value][scalar:hierarchical][guarantee:deduplicated]"
          "[transition:update]") {
    auto memory = allocator{};
    const auto publish =
        node([](i64, Scalar<Bundle<SharedBundle>> value,
                Out<TS<Bundle<SharedBundle>, TimeSeriesGuarantee::deduplicated>> output) { output.set(value); });

    const auto copy = node(
        [](In<TS<Bundle<SharedBundle>>> input, Out<TS<Bundle<SharedBundle>>> output) { output.set(input.value()); });

    const auto read = node([](In<TS<Bundle<SharedBundle>>> input) { return scalar_cast<i64>((*input)[0]).value(); });

    const auto graph = [&](wp<TS<i64>> trigger) {
        const auto value = bundle<SharedBundle>(memory, i64{3}, 2.5);
        return read(copy(publish(trigger, value)));
    };

    const auto actual = testing::test_graph(graph, {1, 2, 3});

    CHECK(actual.equals({3, std::nullopt, std::nullopt}));
}

namespace {

template <template <typename, auto...> typename Field> struct Instrument {
    Field<bg::symbol> symbol;
};

template <template <typename, auto...> typename Field> struct Equity : schema_extends<Instrument, Field> {
    Field<i64> lot_size;
};

} // namespace

TEST_CASE("a hierarchical time series output accepts a derived scalar representation",
          "[feature:scalar-api][series:value][scalar:hierarchical][binding:covariant]"
          "[representation:covariant][transition:update]") {
    auto memory = allocator{};
    const auto publish =
        node([](i64, Scalar<Bundle<Equity>> value, Out<TS<Bundle<Instrument>>> output) { output.set(value); });

    const auto equity = bundle<Equity>(memory, symbol{"ABC"}, i64{100});

    const auto graph = [&](wp<TS<i64>> trigger) { return publish(trigger, equity); };

    const auto actual = testing::test_graph(graph, {1});
    const auto result = actual[0];

    REQUIRE(result.has_value());
    CHECK(&result->type() == &equity.type());
    CHECK(scalar_cast<symbol>(result->view()[0]).value().text() == "ABC");
}

TEST_CASE("a generic bundle node forwards a derived runtime scalar through its base schema",
          "[evaluation][feature:node-api][series:value][scalar:hierarchical][schema:named][declaration:generic]"
          "[binding:covariant][transition:update]") {
    auto memory = allocator{};
    const auto publish =
        node([](i64, Scalar<Bundle<Equity>> value, Out<TS<Bundle<Instrument>>> output) { output.set(value); });

    const auto forward =
        node([](In<TS<Bundle<SCHEMA>>> input, Out<TS<Bundle<SCHEMA>>> output) { output.set(input.value()); });

    const auto inspect = node([](In<TS<Bundle<Instrument>>> input) {
        const auto equity = scalar_cast<Bundle<Equity>>(input.value());
        return scalar_cast<i64>(equity[1]).value();
    });

    const auto equity = bundle<Equity>(memory, symbol{"ABC"}, i64{100});

    const auto graph = [&](wp<TS<i64>> trigger) {
        const auto forwarded = forward(publish(trigger, equity));
        CHECK(&forwarded.type() == &TS<Bundle<Instrument>>::type());
        return inspect(forwarded);
    };

    const auto actual = testing::test_graph(graph, {1});

    CHECK(actual.equals({100}));
}

TEST_CASE("a generic bundle node constructs a runtime schema inside its allocation context",
          "[evaluation][feature:node-api][series:value][scalar:hierarchical][schema:named]"
          "[declaration:runtime][transition:update]") {
    const auto schema_fields = std::array{
        SchemaField{util::symbol{"value"}, &cpp_scalar_type<i64>(), false},
        SchemaField{util::symbol{"comment"}, &cpp_scalar_type<symbol>(), true},
    };
    const auto &runtime_schema =
        schema(util::symbol{"RuntimeNodeBundle"}, new_runtime_schema_id(), nullptr, schema_fields);
    const auto initial_field = ScalarValue<i64>{42};
    const auto initial_field_view = initial_field.view();
    const auto initial_inputs = std::array{
        std::optional<Scalar<>>{Scalar<>::from(initial_field_view)},
        std::optional<Scalar<>>{},
    };
    auto memory = allocator{};
    const auto initial = bundle(memory, runtime_schema, initial_inputs);

    const auto rebuild = node(
        eval =
            [](Scalar<Bundle<SCHEMA>> input, Out<TS<Bundle<SCHEMA>>> output) {
                auto fields = std::vector<std::optional<Scalar<>>>{};
                fields.reserve(input.field_count());
                for (auto index = std::size_t{}; index < input.field_count(); ++index) {
                    if (input.present(index)) {
                        const auto field = input[index];
                        fields.emplace_back(Scalar<>::from(field));
                    } else {
                        fields.emplace_back(std::nullopt);
                    }
                }

                const auto value = bundle(input.type().schema(), fields);
                output.set_value(value.view());
            },
        schedule_on_start = true);

    const auto read = node([](In<TS<Bundle<SCHEMA>>> input) { return scalar_cast<i64>(input.value()[0]).value(); });

    const auto graph = [&] { return read(rebuild(initial)); };

    const auto actual = testing::test_graph(graph);

    CHECK(actual.equals({42}));
}

TEST_CASE("a derived hierarchical value crosses a nested graph boundary",
          "[feature:nested-graph][series:value][scalar:hierarchical]"
          "[binding:value-to-reference][binding:reference-to-value][shape:nested]") {
    const auto make =
        node([](i64 lot_size, Out<TS<Bundle<Equity>>> output) { output.set(bundle<Equity>(symbol{"ABC"}, lot_size)); });

    const auto read = node([](In<TS<Bundle<Equity>>> input) {
        const auto equity = scalar_cast<Bundle<Equity>>(input.value());
        return scalar_cast<i64>(equity[1]).value();
    });

    const auto copy =
        node([](In<TS<Bundle<Equity>>> input, Out<TS<Bundle<Equity>>> output) { output.set(input.value()); });

    const auto pass_through =
        graph("name"_n = "pass_hierarchical_scalar",
              [&](wp<TS<Bundle<Equity>>> input) -> wp<TS<Bundle<Equity>>> { return copy(input); });

    const auto graph = [&](wp<TS<i64>> lot_size, wp<TS<bool>> run) {
        return read(nested(pass_through, run, make(lot_size)));
    };

    const auto actual = testing::test_graph(graph, {100, 200}, {true, true});

    CHECK(actual.equals({100, 200}));
}

TEST_CASE("a time series output copies a covariant scalar collection representation",
          "[feature:scalar-api][series:value][scalar:hierarchical][scalar:list][scalar:dict]"
          "[binding:covariant][transition:update]") {
    auto memory = allocator{};

    const auto list_source = node(
        [](i64, Scalar<List<Bundle<Equity>>> value, Out<TS<List<Bundle<Equity>>>> output) { output.set_value(value); });

    const auto copy_list = node([](In<TS<List<Bundle<Equity>>>> input, Out<TS<List<Bundle<Instrument>>>> output) {
        output.set_value(input.value_view());
    });

    const auto dictionary_source = node([](i64, Scalar<Dict<i64, Bundle<Equity>>> value,
                                           Out<TS<Dict<i64, Bundle<Equity>>>> output) { output.set_value(value); });

    const auto copy_dictionary =
        node([](In<TS<Dict<i64, Bundle<Equity>>>> input, Out<TS<Dict<i64, Bundle<Instrument>>>> output) {
            output.set_value(input.value_view());
        });

    const auto equity = bundle<Equity>(memory, symbol{"ABC"}, i64{100});

    const auto list = ScalarValue<List<Bundle<Equity>>>{std::array<Scalar<Bundle<Equity>>, 1>{equity.view()}};
    const auto dictionary = ScalarValue<Dict<i64, Bundle<Equity>>>{std::array{std::pair{i64{1}, equity.view()}}};

    const auto list_graph = [&](wp<TS<i64>> trigger) { return copy_list(list_source(trigger, list)); };

    const auto dictionary_graph = [&](wp<TS<i64>> trigger) {
        return copy_dictionary(dictionary_source(trigger, dictionary));
    };

    const auto actual_list = testing::test_graph(list_graph, {1});
    const auto actual_dictionary = testing::test_graph(dictionary_graph, {1});

    const auto list_result = actual_list[0];
    REQUIRE(list_result.has_value());
    const auto list_value = list_result->view();
    REQUIRE(list_value.size() == 1);
    CHECK(&list_value[0].type() == &equity.type());
    CHECK(scalar_cast<symbol>(list_value[0][0]).value().text() == "ABC");

    const auto dictionary_result = actual_dictionary[0];
    REQUIRE(dictionary_result.has_value());
    const auto dictionary_value = dictionary_result->view();
    REQUIRE(dictionary_value.size() == 1);
    CHECK(&dictionary_value.at(1).type() == &equity.type());
    const auto stored_equity = scalar_cast<Bundle<Equity>>(dictionary_value.at(1));
    CHECK(scalar_cast<i64>(stored_equity[1]).value() == 100);
}

template <template <typename, auto...> typename Field> struct ReferenceBundle {
    Field<TS<i64>> value;
    Field<REF<TS<i64>>> selected;
};

TEST_CASE("getattr follows a reference field in a referenced native bundle",
          "[evaluation][series:tsb][series:reference][feature:getattr][schema:named]"
          "[representation:native][binding:reference-to-reference][transition:update]") {
    const auto select = node([](bool use_second, In<REF<TS<i64>>> first, In<REF<TS<i64>>> second,
                                Out<REF<TS<i64>>> output) { output.copy(use_second ? second : first); });

    const auto make_bundle = node([](i64 value, In<REF<TS<i64>>> selected, Out<TSB<ReferenceBundle>> output) {
        output->value.set(value);
        output->selected.copy(selected);
    });

    const auto reference =
        node([](In<REF<TSB<ReferenceBundle>>> input, Out<REF<TSB<ReferenceBundle>>> output) { output.copy(input); });

    const auto graph = [&](wp<TS<i64>> first, wp<TS<i64>> second, wp<TS<bool>> use_second, wp<TS<i64>> value) {
        const auto selected = select(use_second, first, second);
        const auto bundle_reference = reference(make_bundle(value, selected));
        const auto bundle = wp<TSB<ReferenceBundle>>{bundle_reference};

        return bundle->selected;
    };

    const auto actual =
        testing::test_graph(graph, std::array{10, 20, 30}, std::array{100, 200, 300},
                            std::array<std::optional<bool>, 3>{false, true, std::nullopt}, std::array{1, 2, 3});

    CHECK(actual.equals({10, 200, 300}));
}

TEST_CASE("a free bundle reuses materialization for the same input type",
          "[wiring][tsb][series:tsb][representation:free][feature:wiring][shape:fan-out]"
          "[schema:named][binding:representation]") {
    const auto count = node([](Out<TS<i64>>) {});

    const auto value = node([](Out<TS<float>>) {});

    const auto first_sum = node(derived_bundle_sum);

    const auto second_sum = node(derived_bundle_sum);
    auto wiring = GraphWiring{};

    const auto context = WiringContext{wiring};
    const auto fields = bundle("value"_n = value(), "count"_n = count());
    (void)first_sum(fields);
    (void)second_sum(fields);

    (void)wiring.complete_pending_wiring();
    REQUIRE(wiring.nodes().size() == 5);
    CHECK(wiring.nodes()[3].inputs()[0] == wiring.nodes()[4].inputs()[0]);
}

TEST_CASE("a free bundle materializes independently for different input schemas",
          "[wiring][tsb][series:tsb][representation:free][feature:wiring][schema:named]"
          "[binding:representation]") {
    const auto count_source = node([](Out<TS<i64>>) {});

    const auto value_source = node([](Out<TS<float>>) {});

    const auto sum = node(derived_bundle_sum);

    const auto count = node(base_bundle_count);
    auto wiring = GraphWiring{};

    const auto context = WiringContext{wiring};
    const auto fields = bundle("value"_n = value_source(), "count"_n = count_source());
    (void)sum(fields);
    (void)count(fields);

    (void)wiring.complete_pending_wiring();
    REQUIRE(wiring.nodes().size() == 6);
    CHECK(wiring.nodes()[3].inputs()[0] != wiring.nodes()[5].inputs()[0]);
}

TEST_CASE("invalidating a bundle invalidates every field",
          "[evaluation][tsb][series:tsb][validity][feature:collection-change][transition:invalidation]"
          "[scalar:integer][scalar:floating-point][representation:native][declaration:concrete]"
          "[binding:direct][schema:named]") {
    struct Observation {
        bool root_valid;
        bool count_valid;
        bool value_valid;
        bool root_modified;
        bool count_modified;
        bool value_modified;
    };

    const auto observe = node([](In<TSB<DerivedBundle>, optional> input) {
        return Observation{
            .root_valid = input.valid(),
            .count_valid = input->count.valid(),
            .value_valid = input->value.valid(),
            .root_modified = input.modified(),
            .count_modified = input->count.modified(),
            .value_modified = input->value.modified(),
        };
    });

    const auto observe_transition = node([](In<TS_T> input) { return input.became_valid(); });

    const auto transition_graph = [&](wp<TSB<DerivedBundle>> bundle) { return observe_transition(bundle); };

    const auto actual = testing::test_graph(observe, {{{"count", 2}, {"value", 3.5F}}, literal::INVALID});
    CHECK(actual.equals({{{"root_valid", true},
                          {"count_valid", true},
                          {"value_valid", true},
                          {"root_modified", true},
                          {"count_modified", true},
                          {"value_modified", true}},
                         {{"root_valid", false},
                          {"count_valid", false},
                          {"value_valid", false},
                          {"root_modified", true},
                          {"count_modified", true},
                          {"value_modified", true}}}));

    const auto transitions = testing::test_graph(transition_graph, {{{"count", 2}, {"value", 3.5F}}, literal::INVALID});

    CHECK(transitions.equals({true, std::nullopt}));
}

TEST_CASE("a free bundle requires every field of the receiving schema",
          "[evaluation][tsb][series:tsb][wiring][scalar:integer][scalar:floating-point]"
          "[representation:native][declaration:concrete][binding:direct][schema:named][feature:wiring]") {
    const auto count = node([](Out<TS<i64>>) {});

    const auto sum = node(derived_bundle_sum);

    CHECK_THROWS(run_graph([&] { return sum(bundle("count"_n = count())); }));
}

TEST_CASE("a free bundle requires compatible field types",
          "[evaluation][tsb][series:tsb][wiring][scalar:integer][scalar:floating-point]"
          "[representation:native][declaration:concrete][binding:direct][schema:named][feature:wiring]") {
    const auto count = node([](Out<TS<i64>>) {});

    const auto value = node([](Out<TS<float>>) {});

    const auto sum = node(derived_bundle_sum);

    CHECK_THROWS(run_graph([&] { return sum(bundle("count"_n = value(), "value"_n = count())); }));
}

TEST_CASE("a free bundle rejects duplicate field names",
          "[evaluation][tsb][series:tsb][wiring][scalar:integer][scalar:floating-point]"
          "[representation:native][declaration:concrete][binding:direct][schema:named][feature:wiring]") {
    const auto count = node([](Out<TS<i64>>) {});

    const auto value = node([](Out<TS<float>>) {});

    const auto sum = node(derived_bundle_sum);

    CHECK_THROWS_AS(
        run_graph([&] { return sum(bundle("count"_n = count(), "count"_n = count(), "value"_n = value())); }),
        WiringError);
}
