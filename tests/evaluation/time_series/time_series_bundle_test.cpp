#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/wiring_context.hpp>
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
#include <chrono>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {

template <template <typename, auto...> typename Field> struct BaseBundle {
    Field<bg::TS<bg::i64>> count;
};

template <template <typename, auto...> typename Field> struct DerivedBundle : bg::bundle_extends<BaseBundle, Field> {
    Field<bg::TS<float>> value;
};

template <template <typename, auto...> typename Field> struct SharedBundle {
    Field<bg::i64> count;
    Field<double> value;
};

template <template <typename, auto...> typename Field> struct NestedSharedBundle {
    Field<bg::Bundle<SharedBundle>> value;
};

template <template <typename, auto...> typename Field> struct Instrument {
    Field<bg::symbol> symbol;
};

template <template <typename, auto...> typename Field> struct Equity : bg::schema_extends<Instrument, Field> {
    Field<bg::i64> lot_size;
};

struct bundle_source_definition {
    bg::Out<bg::TSB<DerivedBundle>> output;

    void start() const {
        output->count.set(2);
        output->value.set(3.5F);
    }
};

struct bundle_sink_definition {
    bg::In<bg::TSB<DerivedBundle>> input;

    void evaluate() const {
        values = {static_cast<float>(*input->count), *input->value};
        updated = {input.item_updated(0), input.item_updated(1)};
    }

    inline static std::array<float, 2> values{};
    inline static std::array<bool, 2> updated{};
};

struct base_bundle_value_definition {
    bg::In<bg::TSB<BaseBundle>> input;
    bg::Out<bg::TS<bg::i64>> output;

    void evaluate() const { output.set(*input->count); }
};

struct bundle_sum_definition {
    bg::In<bg::TSB<DerivedBundle>> input;
    bg::Out<bg::TS<float>> output;

    void evaluate() const { output.set(static_cast<float>(*input->count) + *input->value); }
};

struct bundle_list_source_definition {
    bg::Out<bg::TSL<bg::TSB<DerivedBundle>, 2>> output;

    void start() const { output[1]->value.set(4.5F); }
};

} // namespace

TEST_CASE("a typed bundle publishes inherited and direct fields",
          "[evaluation][tsb][series:tsb][feature:collection-change][transition:update][scalar:integer]"
          "[scalar:floating-point][representation:native][declaration:concrete][binding:direct]"
          "[schema:named]") {
    const auto source = bg::node<bundle_source_definition>();
    const auto sink = bg::node<bundle_sink_definition>();

    bg::run_graph([&] { sink(source()); });

    CHECK(bundle_sink_definition::values == std::array{2.0F, 3.5F});
    CHECK(bundle_sink_definition::updated == std::array{true, true});
}

TEST_CASE("a scalar schema lifts mixed scalar and time series fields into a TSB",
          "[feature:wiring][feature:constant][series:tsb][schema:named][scalar:hierarchical]") {
    const auto graph = [](bg::wp<bg::TS<bg::i64>> count) {
        return bg::bundle<SharedBundle>(count, 2.5)->count;
    };

    const auto actual = bg::testing::test_graph(graph, std::array<bg::i64, 2>{1, 2});

    CHECK(actual.equals({1, 2}));
}

TEST_CASE("an explicit TSB construction lifts every scalar field",
          "[feature:wiring][feature:constant][series:tsb][schema:named][scalar:hierarchical]") {
    const auto graph = [] { return bg::bundle<bg::TSB<SharedBundle>>(bg::i64{1}, 2.5)->count; };

    const auto actual = bg::testing::test_graph(graph);

    CHECK(actual.equals({1}));
}

TEST_CASE("typed TSB construction uses inherited logical field order",
          "[feature:wiring][feature:constant][series:tsb][schema:named][binding:covariant]") {
    const auto graph = [] {
        return bg::bundle<bg::TSB<DerivedBundle>>(bg::ScalarValue<bg::i64>{1}, bg::ScalarValue<float>{2.5F})->value;
    };

    const auto actual = bg::testing::test_graph(graph);

    CHECK(actual.equals({2.5F}));
}

TEST_CASE("an explicit time series schema selects TSB construction for scalar arguments",
          "[feature:wiring][feature:constant][series:tsb][schema:named][declaration:concrete]") {
    const auto graph = [] { return bg::bundle<BaseBundle>(bg::ScalarValue<bg::i64>{1})->count; };

    const auto actual = bg::testing::test_graph(graph);

    CHECK(actual.equals({1}));
}

TEST_CASE("a hierarchical scalar is lifted through a shared TSB schema",
          "[feature:wiring][feature:constant][series:value][series:tsb][scalar:hierarchical][shape:nested]") {
    const auto read = bg::node(
        [](bg::In<bg::TS<bg::Bundle<SharedBundle>>> input, bg::Out<bg::TS<bg::i64>> output) {
            output.set(bg::scalar_cast<bg::i64>(input.value()[0]).value());
        });
    const auto graph = [&] {
        const auto value = bg::bundle<SharedBundle>(bg::i64{3}, 2.5);
        return read(bg::bundle<bg::TSB<NestedSharedBundle>>(value)->value);
    };

    const auto actual = bg::testing::test_graph(graph);

    CHECK(actual.equals({3}));
}

TEST_CASE("a hierarchical time series output accepts input views and suppresses equal updates",
          "[feature:scalar-api][series:value][scalar:hierarchical][guarantee:deduplicated]"
          "[transition:update]") {
    const auto publish = bg::node(
        [](bg::In<bg::TS<bg::i64>> trigger, bg::Scalar<bg::Bundle<SharedBundle>> value,
           bg::Out<bg::TS<bg::Bundle<SharedBundle>, bg::TimeSeriesGuarantee::deduplicated>> output) {
            (void)trigger;
            output.set(value);
        });
    const auto copy = bg::node(
        [](bg::In<bg::TS<bg::Bundle<SharedBundle>>> input,
           bg::Out<bg::TS<bg::Bundle<SharedBundle>>> output) { output.set(input.value()); });
    const auto read = bg::node(
        [](bg::In<bg::TS<bg::Bundle<SharedBundle>>> input, bg::Out<bg::TS<bg::i64>> output) {
            output.set(bg::scalar_cast<bg::i64>((*input)[0]).value());
        });
    const auto graph = [&](bg::wp<bg::TS<bg::i64>> trigger) {
        const auto value = bg::bundle<SharedBundle>(bg::i64{3}, 2.5);
        return read(copy(publish(trigger, value)));
    };

    const auto actual = bg::testing::test_graph(graph, std::array{1, 2, 3});

    CHECK(actual.equals({3, std::nullopt, std::nullopt}));
}

TEST_CASE("a hierarchical time series output accepts a derived scalar representation",
          "[feature:scalar-api][series:value][scalar:hierarchical][binding:covariant]"
          "[representation:covariant][transition:update]") {
    const auto publish = bg::node(
        [](bg::In<bg::TS<bg::i64>> trigger, bg::Scalar<bg::Bundle<Equity>> value,
           bg::Out<bg::TS<bg::Bundle<Instrument>>> output) {
            (void)trigger;
            output.set(value);
        });
    const auto equity = bg::bundle<Equity>(bg::symbol{"ABC"}, bg::i64{100});
    const auto graph = [&](bg::wp<bg::TS<bg::i64>> trigger) { return publish(trigger, equity); };

    const auto actual = bg::testing::test_graph(graph, std::array{1});
    const auto result = actual[0];

    REQUIRE(result.has_value());
    CHECK(&result->type() == &equity.type());
    CHECK(bg::scalar_cast<bg::symbol>((*result)[0]).value().text() == "ABC");
}

TEST_CASE("a time series output copies a covariant scalar collection representation",
          "[feature:scalar-api][series:value][scalar:hierarchical][scalar:list][scalar:dict]"
          "[binding:covariant][transition:update]") {
    const auto list_source = bg::node(
        [](bg::In<bg::TS<bg::i64>> trigger, bg::Scalar<bg::List<bg::Bundle<Equity>>> value,
           bg::Out<bg::TS<bg::List<bg::Bundle<Equity>>>> output) {
            (void)trigger;
            output.set_value(value);
        });
    const auto copy_list = bg::node(
        [](bg::In<bg::TS<bg::List<bg::Bundle<Equity>>>> input,
           bg::Out<bg::TS<bg::List<bg::Bundle<Instrument>>>> output) { output.set_value(input.value_view()); });

    const auto dictionary_source = bg::node(
        [](bg::In<bg::TS<bg::i64>> trigger,
           bg::Scalar<bg::Dict<bg::i64, bg::Bundle<Equity>>> value,
           bg::Out<bg::TS<bg::Dict<bg::i64, bg::Bundle<Equity>>>> output) {
            (void)trigger;
            output.set_value(value);
        });
    const auto copy_dictionary = bg::node(
        [](bg::In<bg::TS<bg::Dict<bg::i64, bg::Bundle<Equity>>>> input,
           bg::Out<bg::TS<bg::Dict<bg::i64, bg::Bundle<Instrument>>>> output) {
            output.set_value(input.value_view());
        });

    const auto equity = bg::bundle<Equity>(bg::symbol{"ABC"}, bg::i64{100});
    const auto list = bg::ScalarValue<bg::List<bg::Bundle<Equity>>>{
        std::array<bg::Scalar<bg::Bundle<Equity>>, 1>{equity.view()}};
    const auto dictionary = bg::ScalarValue<bg::Dict<bg::i64, bg::Bundle<Equity>>>{
        std::array{std::pair{bg::i64{1}, equity.view()}}};
    const auto list_graph = [&](bg::wp<bg::TS<bg::i64>> trigger) { return copy_list(list_source(trigger, list)); };
    const auto dictionary_graph = [&](bg::wp<bg::TS<bg::i64>> trigger) {
        return copy_dictionary(dictionary_source(trigger, dictionary));
    };

    const auto actual_list = bg::testing::test_graph(list_graph, std::array{1});
    const auto actual_dictionary = bg::testing::test_graph(dictionary_graph, std::array{1});

    REQUIRE(actual_list[0].has_value());
    REQUIRE(actual_list[0]->size() == 1);
    CHECK(&(*actual_list[0])[0].type() == &equity.type());
    CHECK(bg::scalar_cast<bg::symbol>((*actual_list[0])[0][0]).value().text() == "ABC");

    REQUIRE(actual_dictionary[0].has_value());
    REQUIRE(actual_dictionary[0]->size() == 1);
    CHECK(&actual_dictionary[0]->at(1).type() == &equity.type());
    const auto stored_equity = bg::scalar_cast<bg::Bundle<Equity>>(actual_dictionary[0]->at(1));
    CHECK(bg::scalar_cast<bg::i64>(stored_equity[1]).value() == 100);
}

TEST_CASE("fixed time series lists can contain runtime-sized time series",
          "[evaluation][tsl][series:fixed-tsl][tsb][series:tsb][feature:collection-change]"
          "[scalar:integer][scalar:floating-point][representation:native][declaration:concrete]"
          "[binding:direct][schema:named]") {
    const auto source = bg::node<bundle_list_source_definition>();
    const auto value = bg::node([](bg::In<bg::TSL<bg::TSB<DerivedBundle>, 2>> input, bg::Out<bg::TS<float>> output) {
        output.set(*input[1]->value);
    });
    const auto graph = [&] { return value(source()); };

    const auto actual = bg::testing::test_graph(graph);

    CHECK(actual.equals({4.5F}));
}

TEST_CASE("node code selects a named bundle field",
          "[evaluation][tsb][feature:getitem][series:tsb][schema:named][scalar:integer][scalar:floating-point]"
          "[representation:native][declaration:concrete][binding:direct]") {
    const auto source = bg::node<bundle_source_definition>();
    const auto count =
        bg::node([](bg::In<bg::TSB<DerivedBundle>> input, bg::Out<bg::TS<bg::i64>> output) { output.set(*input->count); });
    const auto graph = [&] { return count(source()); };

    const auto actual = bg::testing::test_graph(graph);

    CHECK(actual.equals({2}));
}

TEST_CASE("a base bundle input accepts a derived bundle",
          "[evaluation][tsb][series:tsb][inheritance][feature:collection-change][scalar:integer]"
          "[scalar:floating-point][representation:native][declaration:concrete][binding:covariant]"
          "[schema:named]") {
    const auto source = bg::node<bundle_source_definition>();
    const auto value = bg::node<base_bundle_value_definition>();
    const auto graph = [&] { return value(source()); };

    const auto actual = bg::testing::test_graph(graph);

    CHECK(actual.equals({2}));
}

TEST_CASE("a typed TSB wiring point selects an inherited field",
          "[evaluation][series:tsb][feature:getattr][schema:named][representation:native]"
          "[transition:update]") {
    const auto source = bg::node<bundle_source_definition>();
    const auto graph = [&] { return source()->count; };

    const auto actual = bg::testing::test_graph(graph);

    CHECK(actual.equals({2}));
}

template <template <typename, auto...> typename Field> struct ReferenceBundle {
    Field<bg::TS<bg::i64>> value;
    Field<bg::REF<bg::TS<bg::i64>>> selected;
};

TEST_CASE("getattr follows a reference field in a referenced native bundle",
          "[evaluation][series:tsb][series:reference][feature:getattr][schema:named]"
          "[representation:native][binding:reference-to-reference][transition:update]") {
    const auto select = bg::node(
        [](bg::In<bg::TS<bool>> use_second, bg::In<bg::REF<bg::TS<bg::i64>>> first,
           bg::In<bg::REF<bg::TS<bg::i64>>> second, bg::Out<bg::REF<bg::TS<bg::i64>>> output) {
            output.copy(*use_second ? second : first);
        });
    const auto make_bundle = bg::node(
        [](bg::In<bg::TS<bg::i64>> value, bg::In<bg::REF<bg::TS<bg::i64>>> selected,
           bg::Out<bg::TSB<ReferenceBundle>> output) {
            output->value.set(*value);
            output->selected.copy(selected);
        });
    const auto reference = bg::node(
        [](bg::In<bg::REF<bg::TSB<ReferenceBundle>>> input,
           bg::Out<bg::REF<bg::TSB<ReferenceBundle>>> output) { output.copy(input); });
    const auto graph = [&](bg::wp<bg::TS<bg::i64>> first, bg::wp<bg::TS<bg::i64>> second,
                           bg::wp<bg::TS<bool>> use_second, bg::wp<bg::TS<bg::i64>> value) {
        const auto selected = select(use_second, first, second);
        const auto bundle_reference = reference(make_bundle(value, selected));
        const auto bundle = bg::wp<bg::TSB<ReferenceBundle>>{bundle_reference};

        return bundle->selected;
    };

    const auto actual =
        bg::testing::test_graph(graph, std::array{10, 20, 30}, std::array{100, 200, 300},
                                std::array<std::optional<bool>, 3>{false, true, std::nullopt}, std::array{1, 2, 3});

    CHECK(actual.equals({10, 200, 300}));
}

TEST_CASE("a typed free TSB wiring point selects its original source",
          "[evaluation][series:tsb][feature:getattr][schema:named][representation:free]"
          "[transition:update]") {
    using namespace bg::literals;

    const auto graph = [](bg::wp<bg::TS<bg::i64>> count, bg::wp<bg::TS<float>> value) {
        return bg::bundle("value"_n = value, "count"_n = count).as<bg::TSB<DerivedBundle>>()->count;
    };

    const auto actual = bg::testing::test_graph(graph, std::vector{1, 2, 3}, std::vector{0.5F, 1.5F, 2.5F});

    CHECK(actual.equals({1, 2, 3}));
}

TEST_CASE("a free bundle follows the receiving named schema",
          "[evaluation][tsb][series:tsb][free][representation:free][feature:collection-change]"
          "[scalar:integer][scalar:floating-point][declaration:concrete][binding:direct]") {
    using namespace bg::literals;
    const auto sum = bg::node<bundle_sum_definition>();
    const auto graph = [&](bg::wp<bg::TS<bg::i64>> count, bg::wp<bg::TS<float>> value) {
        return sum(bg::bundle("value"_n = value, "count"_n = count));
    };

    const auto result = bg::testing::test_graph(graph, std::vector{1, 2}, std::vector{0.5F, 1.5F});

    CHECK(result.equals({1.5F, 3.5F}));
}

TEST_CASE("a free bundle reuses materialization for the same input type",
          "[wiring][tsb][series:tsb][representation:free][feature:wiring][shape:fan-out]"
          "[schema:named][binding:representation]") {
    using namespace bg::literals;
    const auto count = bg::node([](bg::Out<bg::TS<bg::i64>>) {});
    const auto value = bg::node([](bg::Out<bg::TS<float>>) {});
    const auto first_sum = bg::node<bundle_sum_definition>();
    const auto second_sum = bg::node<bundle_sum_definition>();
    auto wiring = bg::GraphWiring{};

    const auto context = bg::WiringContext{wiring};
    const auto bundle = bg::bundle("value"_n = value(), "count"_n = count());
    (void)first_sum(bundle);
    (void)second_sum(bundle);

    REQUIRE(wiring.nodes().size() == 5);
    CHECK(wiring.nodes()[3].inputs()[0] == wiring.nodes()[4].inputs()[0]);
}

TEST_CASE("a free bundle materializes independently for different input schemas",
          "[wiring][tsb][series:tsb][representation:free][feature:wiring][schema:named]"
          "[binding:representation]") {
    using namespace bg::literals;
    const auto count_source = bg::node([](bg::Out<bg::TS<bg::i64>>) {});
    const auto value_source = bg::node([](bg::Out<bg::TS<float>>) {});
    const auto sum = bg::node<bundle_sum_definition>();
    const auto count = bg::node<base_bundle_value_definition>();
    auto wiring = bg::GraphWiring{};

    const auto context = bg::WiringContext{wiring};
    const auto bundle = bg::bundle("value"_n = value_source(), "count"_n = count_source());
    (void)sum(bundle);
    (void)count(bundle);

    REQUIRE(wiring.nodes().size() == 6);
    CHECK(wiring.nodes()[3].inputs()[0] != wiring.nodes()[5].inputs()[0]);
}

TEST_CASE("a free bundle requires every field of the receiving schema",
          "[evaluation][tsb][series:tsb][wiring][scalar:integer][scalar:floating-point]"
          "[representation:native][declaration:concrete][binding:direct][schema:named][feature:wiring]") {
    using namespace bg::literals;
    const auto count = bg::node([](bg::Out<bg::TS<bg::i64>>) {});
    const auto sum = bg::node<bundle_sum_definition>();

    CHECK_THROWS(bg::run_graph([&] { return sum(bg::bundle("count"_n = count())); }));
}

TEST_CASE("a free bundle requires compatible field types",
          "[evaluation][tsb][series:tsb][wiring][scalar:integer][scalar:floating-point]"
          "[representation:native][declaration:concrete][binding:direct][schema:named][feature:wiring]") {
    using namespace bg::literals;
    const auto count = bg::node([](bg::Out<bg::TS<bg::i64>>) {});
    const auto value = bg::node([](bg::Out<bg::TS<float>>) {});
    const auto sum = bg::node<bundle_sum_definition>();

    CHECK_THROWS(bg::run_graph([&] { return sum(bg::bundle("count"_n = value(), "value"_n = count())); }));
}

TEST_CASE("a free bundle rejects duplicate field names",
          "[evaluation][tsb][series:tsb][wiring][scalar:integer][scalar:floating-point]"
          "[representation:native][declaration:concrete][binding:direct][schema:named][feature:wiring]") {
    using namespace bg::literals;
    const auto count = bg::node([](bg::Out<bg::TS<bg::i64>>) {});
    const auto value = bg::node([](bg::Out<bg::TS<float>>) {});
    const auto sum = bg::node<bundle_sum_definition>();

    CHECK_THROWS_AS(
        bg::run_graph([&] { return sum(bg::bundle("count"_n = count(), "count"_n = count(), "value"_n = value())); }),
        bg::WiringError);
}

TEST_CASE("a named bundle input accepts extra free bundle fields",
          "[evaluation][tsb][series:tsb][wiring][schema:named][scalar:integer][scalar:floating-point]"
          "[representation:native][declaration:concrete][binding:direct][feature:wiring]") {
    using namespace bg::literals;
    const auto sum = bg::node<bundle_sum_definition>();
    const auto graph = [&](bg::wp<bg::TS<bg::i64>> count, bg::wp<bg::TS<float>> value) {
        return sum(bg::bundle("extra"_n = count, "value"_n = value, "count"_n = count));
    };

    const auto result = bg::testing::test_graph(graph, std::vector{1, 2}, std::vector{0.5F, 1.5F});

    CHECK(result.equals({1.5F, 3.5F}));
}

namespace {

template <template <typename, auto...> typename Field> struct RedefinedBundle : bg::bundle_extends<BaseBundle, Field> {
    Field<bg::TS<bg::i64>> count;
    Field<bg::TS<float>> value;
};

struct redefined_bundle_source_definition {
    bg::Out<bg::TSB<RedefinedBundle>> output;

    void start() const {
        output->count.set(4);
        output->value.set(2.5F);
    }
};

} // namespace

TEST_CASE("a redefined bundle field shares its inherited storage",
          "[evaluation][tsb][series:tsb][inheritance][feature:collection-change][schema:named]"
          "[scalar:integer][scalar:floating-point][representation:native][declaration:concrete]"
          "[binding:direct]") {
    const auto source = bg::node<redefined_bundle_source_definition>();
    const auto value = bg::node<base_bundle_value_definition>();
    const auto graph = [&] { return value(source()); };

    const auto actual = bg::testing::test_graph(graph);

    CHECK(actual.equals({4}));
}

namespace {

template <template <typename, auto...> typename Field> struct NestedBundle {
    Field<bg::TSB<BaseBundle>> nested;
    Field<bg::TS<float>> value;
};

struct nested_bundle_source_definition {
    bg::Out<bg::TSB<NestedBundle>> output;

    void start() const {
        output->nested->count.set(3);
        output->value.set(1.5F);
    }
};

struct nested_bundle_sum_definition {
    bg::In<bg::TSB<NestedBundle>> input;
    bg::Out<bg::TS<float>> output;

    void evaluate() const { output.set(static_cast<float>(*input->nested->count) + *input->value); }
};

} // namespace

TEST_CASE("a bundle field can contain another bundle",
          "[evaluation][tsb][series:tsb][nested][feature:collection-change][schema:named]"
          "[scalar:integer][scalar:floating-point][representation:native][declaration:concrete]"
          "[binding:direct]") {
    const auto source = bg::node<nested_bundle_source_definition>();
    const auto sum = bg::node<nested_bundle_sum_definition>();
    const auto graph = [&] { return sum(source()); };

    const auto actual = bg::testing::test_graph(graph);

    CHECK(actual.equals({4.5F}));
}

namespace {

struct invalidating_bundle_source_definition {
    static constexpr bool uses_timed_scheduling = true;

    bg::Out<bg::TSB<DerivedBundle>> output;

    void start(bg::EvaluationContext *context) const {
        output->count.set(2);
        output->value.set(3.5F);
        context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
    }

    void evaluate() const { output.invalidate(); }
};

struct optional_bundle_sink_definition {
    bg::In<bg::TSB<DerivedBundle>, bg::optional> input;

    void evaluate() const { validity.push_back({input.valid(), input->count.valid(), input->value.valid()}); }

    inline static std::vector<std::array<bool, 3>> validity;
};

} // namespace

TEST_CASE("invalidating a bundle invalidates every field",
          "[evaluation][tsb][series:tsb][validity][feature:collection-change][transition:invalidation]"
          "[scalar:integer][scalar:floating-point][representation:native][declaration:concrete]"
          "[binding:direct][schema:named]") {
    const auto source = bg::node<invalidating_bundle_source_definition>();
    const auto sink = bg::node<optional_bundle_sink_definition>();
    optional_bundle_sink_definition::validity.clear();

    bg::run_graph([&] { sink(source()); });

    CHECK(optional_bundle_sink_definition::validity ==
          std::vector<std::array<bool, 3>>{{true, true, true}, {false, false, false}});
}
