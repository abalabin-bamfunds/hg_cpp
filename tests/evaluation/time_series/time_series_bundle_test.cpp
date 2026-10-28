#include <bg/graph.hpp>
#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/nested.hpp>
#include <bg/node.hpp>
#include <bg/operators.hpp>
#include <bg/run_graph.hpp>
#include <bg/switch.hpp>
#include <bg/scalar/bundle/value.hpp>
#include <bg/scalar/dict/scalar.hpp>
#include <bg/scalar/list/scalar.hpp>
#include <bg/time_series/tsb/tsb.hpp>
#include <bg/time_series/tsb/tsb_input.hpp>
#include <bg/time_series/tsb/tsb_output.hpp>
#include <bg/time_series/tsl/tsl_input.hpp>
#include <bg/time_series/tsl/tsl_output.hpp>
#include <bg/time_series/tick_format.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <optional>
#include <span>
#include <string>
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

    const auto actual = bg::run_graph(count, {{{"count", 2}, {"value", 3.5F}}});

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
        bg::run_graph(observe, {{{"count", 2}, {"value", 3.5F}}, {{"count", 4}}, {{"value", 6.5F}}});

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

    const auto result = bg::run_graph(graph, {1, 2}, {10.0F, 20.0F});

    CHECK(result.equals({11.0F, 22.0F}));
}

namespace {

template <template <typename, auto...> typename> struct EmptyBundle {};

template <template <typename, auto...> typename Field> struct SharedBundle {
    Field<i64> count;
    Field<double> value;
};

} // namespace

TEST_CASE("a delayed reference in a free TSB preserves an aliased field target identity",
          "[evaluation][tsb][series:reference][representation:free][transition:update]"
          "[binding:value-to-reference][binding:reference-to-reference][binding:reference-to-value]") {
    struct Observation {
        i64 value;
        bool modified;
        EvaluationTime last_updated;

        bool operator==(const Observation &) const = default;
    };

    const auto late_ref = node([](In<TS<i64>> bind, In<REF<TS<i64>>, passive> value,
                                  Out<REF<TS<i64>>> output) {
        if (bind.valid()) {
            output.copy(value);
        }
    });

    const auto select = node([](In<TS<bool>> choose_late, In<REF<TS<i64>>, passive, optional> first,
                                In<REF<TS<i64>>, passive, optional> late, Out<REF<TS<i64>>> output) {
        output.copy(*choose_late ? late : first);
    });

    const auto observe = node([](In<TS<i64>> input, In<TS<i64>>, Out<TS<Observation>> output) {
        output.set(Observation{*input, input.modified(), input.last_updated()});
    });
    const auto tested = [&](wp<TS<i64>> value, wp<TS<i64>> bind, wp<TS<bool>> choose_late,
                             wp<TS<i64>> pulse) {
        const auto first = bundle("value"_n = value);
        const auto late = bundle("value"_n = late_ref(bind, value));
        const auto selected = select(choose_late, operators::getattr(first, util::symbol{"value"}),
                                     operators::getattr(late, util::symbol{"value"}));
        return observe(selected, pulse);
    };

    const auto start_time = MIN_DT + std::chrono::microseconds{1};
    const auto actual = bg::run_graph(
        tested, GraphRunOptions{.start_time = start_time},
        std::array<std::optional<i64>, 4>{10, std::nullopt, std::nullopt, 20},
        std::array<std::optional<i64>, 4>{std::nullopt, 1, std::nullopt, std::nullopt},
        std::array<bool, 4>{false, true, false, true}, std::array<i64, 4>{1, 2, 3, 4});
    const auto expected = std::initializer_list<std::optional<Observation>>{
        Observation{10, true, start_time},
        Observation{10, false, start_time},
        Observation{10, false, start_time},
        Observation{20, true, start_time + std::chrono::microseconds{3}},
    };

    CHECK(actual.equals<Observation>(expected));
}

TEST_CASE("an empty free TSB publishes valid at graph start",
          "[evaluation][tsb][series:tsb][free][feature:wiring-materialisation][representation:free]"
          "[transition:start]") {
    const auto observe = node([](In<TSB<EmptyBundle>> input) { return input.valid() ? 1 : 0; });

    const auto graph = [&] {
        const auto fields = std::vector<WiringArgument>{};
        return observe(bundle(fields));
    };

    CHECK(bg::run_graph(graph).equals({1}));
}

TEST_CASE("a scalar schema lifts mixed scalar and time series fields into a TSB",
          "[feature:wiring][feature:constant][series:tsb][schema:named][scalar:hierarchical]") {
    const auto graph = [](wp<TS<i64>> count) { return bundle<SharedBundle>(count, 2.5); };

    const auto actual = bg::run_graph(graph, {1, 2});

    CHECK(actual.equals({{{"count", 1}, {"value", 2.5}}, {{"count", 2}}}));
}

TEST_CASE("an explicit TSB construction lifts every scalar field",
          "[feature:wiring][feature:constant][series:tsb][schema:named][scalar:hierarchical]") {
    const auto graph = [] { return bundle<TSB<SharedBundle>>(i64{1}, 2.5); };

    const auto actual = bg::run_graph(graph);

    CHECK(actual.equals({{{"count", 1}, {"value", 2.5}}}));
}

TEST_CASE("typed TSB construction uses inherited logical field order",
          "[feature:wiring][feature:constant][series:tsb][schema:named][binding:covariant]") {
    const auto graph = [] { return bundle<TSB<DerivedBundle>>(ScalarValue<i64>{1}, ScalarValue<float>{2.5F})->value; };

    const auto actual = bg::run_graph(graph);

    CHECK(actual.equals({2.5F}));
}

TEST_CASE("an explicit time series schema selects TSB construction for scalar arguments",
          "[feature:wiring][feature:constant][series:tsb][schema:named][declaration:concrete]") {
    const auto graph = [] { return bundle<BaseBundle>(ScalarValue<i64>{1})->count; };

    const auto actual = bg::run_graph(graph);

    CHECK(actual.equals({1}));
}

TEST_CASE("a typed TSB wiring point selects an inherited field",
          "[evaluation][series:tsb][feature:getattr][schema:named][representation:native]"
          "[transition:update]") {
    const auto graph = [](wp<TSB<DerivedBundle>> input) { return input->count; };

    const auto actual = bg::run_graph(graph, {{{"count", 2}, {"value", 3.5F}}});

    CHECK(actual.equals({2}));
}

TEST_CASE("a typed free TSB wiring point selects its original source",
          "[evaluation][series:tsb][feature:getattr][schema:named][representation:free]"
          "[transition:update]") {
    const auto graph = [](wp<TS<i64>> count, wp<TS<float>> value) {
        return bundle("value"_n = value, "count"_n = count).as<TSB<DerivedBundle>>()->count;
    };

    const auto actual = bg::run_graph(graph, {1, 2, 3}, {0.5F, 1.5F, 2.5F});

    CHECK(actual.equals({1, 2, 3}));
}

TEST_CASE("a free bundle follows the receiving named schema",
          "[evaluation][tsb][series:tsb][free][representation:free][feature:collection-change]"
          "[scalar:integer][scalar:floating-point][declaration:concrete][binding:direct]") {
    const auto sum = node(derived_bundle_sum);

    const auto graph = [&](wp<TS<i64>> count, wp<TS<float>> value) {
        return sum(bundle("value"_n = value, "count"_n = count));
    };

    const auto result = bg::run_graph(graph, {1, 2}, {0.5F, 1.5F});

    CHECK(result.equals({1.5F, 3.5F}));
}

TEST_CASE("a free bundle can be observed through a reference",
          "[feature:reference][series:tsb][series:reference][representation:free][shape:chain]"
          "[binding:value-to-reference][transition:update]") {
    const auto observe = node([](In<REF<TSB<DerivedBundle>>> input) { return input.valid(); });

    const auto graph = [&](wp<TS<i64>> count, wp<TS<float>> value) {
        return observe(bundle("count"_n = count, "value"_n = value));
    };

    const auto actual = bg::run_graph(graph, {1, 2}, {0.5F, 1.5F});

    CHECK(actual.equals({true, std::nullopt}));
}

TEST_CASE("a reference to a free bundle samples updates to its fields",
          "[feature:reference][feature:sampling][series:tsb][series:reference][representation:free]"
          "[binding:reference-to-value][transition:update]") {
    const auto sum = node([](In<TSB<DerivedBundle>> input) { return *input->count + *input->value; });

    const auto graph = [&](wp<TS<i64>> count, wp<TS<float>> value) {
        const auto fields = bundle("count"_n = count, "value"_n = value);
        return sum(fields.as<REF<TSB<DerivedBundle>>>());
    };

    const auto actual = bg::run_graph(graph, {1, 2, literal::SKIP}, {0.5F, literal::SKIP, 1.5F});

    CHECK(actual.equals({1.5F, 2.5F, 3.5F}));
}

TEST_CASE("a free bundle reports changes for each field independently",
          "[feature:collection-change][series:tsb][representation:free][schema:named]"
          "[transition:update][scalar:integer][scalar:floating-point][binding:direct]") {
    struct Observation {
        i64 count;
        double value;
        bool count_updated;
        bool value_updated;

        bool operator==(const Observation &) const = default;
    };

    const auto observe = node([](In<TSB<DerivedBundle>> input) {
        return Observation{*input->count, *input->value, input->count.modified(), input->value.modified()};
    });

    const auto graph = [&](wp<TS<i64>> count, wp<TS<float>> value) {
        return observe(bundle("count"_n = count, "value"_n = value));
    };

    const auto actual = bg::run_graph(graph, {1, 2, literal::SKIP}, {0.5F, literal::SKIP, 1.5F});

    CHECK(actual.equals<Observation>({Observation{1, 0.5F, true, true},
                                      Observation{2, 0.5F, true, false},
                                      Observation{2, 1.5F, false, true}}));
}

TEST_CASE("a named bundle input accepts extra free bundle fields",
          "[evaluation][tsb][series:tsb][wiring][schema:named][scalar:integer][scalar:floating-point]"
          "[representation:native][declaration:concrete][binding:direct][feature:wiring]") {
    const auto sum = node(derived_bundle_sum);

    const auto graph = [&](wp<TS<i64>> count, wp<TS<float>> value) {
        return sum(bundle("extra"_n = count, "value"_n = value, "count"_n = count));
    };

    const auto result = bg::run_graph(graph, {1, 2}, {0.5F, 1.5F});

    CHECK(result.equals({1.5F, 3.5F}));
}

TEST_CASE("a base bundle input accepts a derived bundle",
          "[evaluation][tsb][series:tsb][inheritance][feature:collection-change][scalar:integer]"
          "[scalar:floating-point][representation:native][declaration:concrete][binding:covariant]"
          "[schema:named]") {
    const auto value = node(base_bundle_count);

    const auto graph = [&](wp<TSB<DerivedBundle>> input) { return value(input); };

    const auto actual = bg::run_graph(graph, {{{"count", 2}, {"value", 3.5F}}});

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

    const auto actual = bg::run_graph(graph, {{{"count", 4}, {"value", 2.5F}}});

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

    const auto actual = bg::run_graph(sum, {{{"nested", {{"count", 3}}}, {"value", 1.5F}}});

    CHECK(actual.equals({4.5F}));
}

TEST_CASE("fixed time series lists can contain runtime-sized time series",
          "[evaluation][tsl][series:fixed-tsl][tsb][series:tsb][feature:collection-change]"
          "[scalar:integer][scalar:floating-point][representation:native][declaration:concrete]"
          "[binding:direct][schema:named]") {
    const auto value = node([](In<TSL<TSB<DerivedBundle>, 2>> input) { return *input[1]->value; });

    const auto actual = bg::run_graph(value, {{{1, {{"value", 4.5F}}}}});

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

    const auto actual = bg::run_graph(graph);

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

    const auto actual = bg::run_graph(graph, {1, 2, 3});

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

    const auto actual = bg::run_graph(graph, {1});
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

    const auto actual = bg::run_graph(graph, {1});

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

    const auto actual = bg::run_graph(graph);

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

    const auto actual = bg::run_graph(graph, {100, 200}, {true, true});

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

    const auto actual_list = bg::run_graph(list_graph, {1});
    const auto actual_dictionary = bg::run_graph(dictionary_graph, {1});

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

    const auto make_bundle = node([](i64 value, In<REF<TS<i64>>> selected,
                                     Out<TSB<ReferenceBundle, TimeSeriesGuarantee::deduplicated>> output) {
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
        bg::run_graph(graph, std::array{10, 20, 30}, std::array{100, 200, 300},
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

    const auto actual = bg::run_graph(observe, {{{"count", 2}, {"value", 3.5F}}, literal::INVALID});
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

    const auto transitions = bg::run_graph(transition_graph, {{{"count", 2}, {"value", 3.5F}}, literal::INVALID});

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

TEST_CASE("a switch routes a free bundle between its fields and a constant bundle",
          "[.][repro][feature:switch][feature:reference][series:tsb][representation:free][schema:named]"
          "[shape:nested][binding:value-to-reference][binding:reference-to-value][transition:update]") {
    const auto pass_through = graph("name"_n = "pass_free_bundle",
                                    [](wp<TSB<DerivedBundle>> input) { return input; });

    const auto constant = graph("name"_n = "constant_bundle",
                                [](wp<TSB<DerivedBundle>>) { return bundle<DerivedBundle>(i64{1}, 2.0F); });

    const auto tested = [&](wp<TS<bool>> key, wp<TS<i64>> count, wp<TS<float>> value) {
        const auto fields = bundle("count"_n = count, "value"_n = value);
        return switch_(key, cases{case_(true, pass_through), case_(false, constant)}, fields);
    };

    const auto actual = bg::run_graph(tested,
                                      {false, true, false, true},
                                      {1, literal::SKIP, 3, literal::SKIP},
                                      {-1.0F, literal::SKIP, -3.0F, literal::SKIP});

    auto observed = std::vector<std::string>{};
    for (auto tick_index = std::size_t{}; tick_index < actual.size(); ++tick_index) {
        actual.visit_tick(tick_index, [&](const TimeSeriesType &type, bool valid, util::buffer_reader &reader) {
            observed.push_back(fmt::format("{}", tick(type, valid, reader)));
        });
    }
    CAPTURE(observed);

    CHECK(actual.equals({{{"count", 1}, {"value", 2.0F}},
                         {{"count", 1}, {"value", -1.0F}},
                         {{"count", 1}, {"value", 2.0F}},
                         {{"count", 3}, {"value", -3.0F}}}));
}

TEST_CASE("a switch accepts native and free bundle branches",
          "[.][repro][feature:switch][feature:reference][series:tsb][schema:named]"
          "[representation:free][representation:native][shape:nested][transition:update]") {
    const auto pass_through = graph("name"_n = "pass_free_bundle",
                                    [](wp<TSB<DerivedBundle>> input) { return input; });

    const auto pack = node([](i64 count, float value, Out<TSB<DerivedBundle>> output) {
        output->count.set(count);
        output->value.set(value);
    });

    const auto native = graph("name"_n = "pack_native_bundle",
                              [&](wp<TSB<DerivedBundle>> input) { return pack(input->count, input->value); });

    const auto tested = [&](wp<TS<bool>> key, wp<TS<i64>> count, wp<TS<float>> value) {
        const auto fields = bundle("count"_n = count, "value"_n = value);
        return switch_(key, cases{case_(true, pass_through), case_(false, native)}, fields);
    };

    const auto reversed = [&](wp<TS<bool>> key, wp<TS<i64>> count, wp<TS<float>> value) {
        const auto fields = bundle("count"_n = count, "value"_n = value);
        return switch_(key, cases{case_(false, native), case_(true, pass_through)}, fields);
    };

    CHECK(bg::run_graph(tested,
                        {true, false, true, false},
                        {1, literal::SKIP, literal::SKIP, 4},
                        {0.5F, literal::SKIP, literal::SKIP, 3.5F})
              .equals({{{"count", 1}, {"value", 0.5F}},
                       {{"count", 1}, {"value", 0.5F}},
                       {{"count", 1}, {"value", 0.5F}},
                       {{"count", 4}, {"value", 3.5F}}}));
    CHECK(bg::run_graph(reversed,
                        {true, false, true, false},
                        {1, literal::SKIP, literal::SKIP, 4},
                        {0.5F, literal::SKIP, literal::SKIP, 3.5F})
              .equals({{{"count", 1}, {"value", 0.5F}},
                       {{"count", 1}, {"value", 0.5F}},
                       {{"count", 1}, {"value", 0.5F}},
                       {{"count", 4}, {"value", 3.5F}}}));
}

TEST_CASE("reselecting a rebuilt free bundle keeps unchanged field references",
          "[.][repro][feature:switch][feature:reference][feature:sampling][series:tsb][representation:free]"
          "[shape:nested][binding:reference-to-reference][transition:update]") {
    const auto pass_through = graph("name"_n = "pass_free_bundle",
                                    [](wp<TSB<DerivedBundle>> input) { return input; });

    const auto rebuild = graph("name"_n = "rebuild_free_bundle", [](wp<TSB<DerivedBundle>> input) {
        return bundle("count"_n = input->count, "value"_n = input->value);
    });

    const auto count_value = node([](i64 count) { return count; });

    const auto select_count = graph("name"_n = "select_free_bundle_count", [&](wp<TSB<DerivedBundle>> input) {
        return count_value(input->count);
    });

    const auto tested = [&](wp<TS<bool>> key, wp<TS<i64>> count, wp<TS<float>> value) {
        const auto fields = bundle("count"_n = count, "value"_n = value);
        return select_count(switch_(key, cases{case_(true, pass_through), case_(false, rebuild)}, fields));
    };

    const auto actual = bg::run_graph(tested,
                                      {true, false, true, false},
                                      {10, literal::SKIP, literal::SKIP, 20},
                                      {0.5F, 1.5F, 2.5F, 3.5F});

    CHECK(actual.equals({10, std::nullopt, std::nullopt, 20}));
}

TEST_CASE("a nested graph observes independent updates to free bundle fields",
          "[feature:nested-graph][feature:reference][feature:sampling][series:tsb][representation:free]"
          "[shape:nested][binding:value-to-reference][binding:reference-to-value][transition:update]") {
    const auto sum = node([](i64 count, float value) { return count + value; });

    const auto inner = graph("name"_n = "sum_free_bundle_fields", [&](wp<TSB<DerivedBundle>> input) {
        return sum(input->count, input->value);
    });

    const auto tested = [&](wp<TS<i64>> count, wp<TS<float>> value, wp<TS<bool>> run) {
        return nested(inner, run, bundle("count"_n = count, "value"_n = value));
    };

    const auto actual = bg::run_graph(tested, {1, 2, literal::SKIP}, {0.5F, literal::SKIP, 1.5F}, {true, true, true});

    CHECK(actual.equals({1.5F, 2.5F, 3.5F}));
}

TEST_CASE("a nested graph sees partial validity in a free bundle",
          "[feature:nested-graph][feature:reference][feature:validity][series:tsb]"
          "[representation:free][shape:nested][validity:optional][transition:invalidation]"
          "[transition:revalidation]") {
    const auto observe = node([](In<TSB<DerivedBundle>, optional> input) {
        return input.valid() && input->count.valid();
    });

    const auto inner = graph("name"_n = "observe_free_bundle_count", [&](wp<TSB<DerivedBundle>> input) {
        return observe(input);
    });

    const auto tested = [&](wp<TS<i64>> count, wp<TS<float>> value, wp<TS<bool>> run) {
        return nested(inner, run, bundle("count"_n = count, "value"_n = value));
    };

    const auto actual = bg::run_graph(tested, {1, literal::INVALID, 2}, {0.5F, 1.5F, 2.5F}, {true, true, true});

    CHECK(actual.equals({true, false, true}));
}

TEST_CASE("a nested graph passes a free bundle reference to a value consumer",
          "[.][repro][feature:nested-graph][feature:reference][feature:sampling][series:tsb]"
          "[representation:free][shape:nested][binding:value-to-reference]"
          "[binding:reference-to-value][transition:update]") {
    const auto sum = node([](In<TSB<DerivedBundle>> input) { return *input->count + *input->value; });

    const auto inner = graph("name"_n = "reference_free_bundle", [](wp<REF<TSB<DerivedBundle>>> input) {
        return input;
    });

    const auto tested = [&](wp<TS<i64>> count, wp<TS<float>> value, wp<TS<bool>> run) {
        return sum(nested(inner, run, bundle("count"_n = count, "value"_n = value)));
    };

    const auto actual = bg::run_graph(tested, {1, 2, literal::SKIP}, {0.5F, literal::SKIP, 1.5F}, {true, true, true});

    CHECK(actual.equals({1.5F, 2.5F, 3.5F}));
}
