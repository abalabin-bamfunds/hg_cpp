#include <bg/graph.hpp>
#include <bg/nested.hpp>
#include <bg/node.hpp>
#include <bg/operator.hpp>
#include <bg/scalar/type.hpp>
#include <bg/testing/test_graph.hpp>
#include <bg/testing/test_wiring.hpp>
#include <bg/time_series/tsb/tsb.hpp>
#include <bg/time_series/tsl/tsl.hpp>
#include <bg/type/scalar_variable.hpp>
#include <bg/type/time_series_bundle_type.hpp>
#include <bg/type/time_series_list_type.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>

#include <array>
#include <cstdint>

namespace {

using namespace bg;
using namespace bg::literals;

using DIRECT_RESULT = ScalarVariable<"DIRECT_RESULT">;

template <template <typename, auto...> typename Field>
using ARGS = BundleSchemaVariableDeclaration<"ARGS", Field>;

template <template <typename, auto...> typename Field>
using KWARGS = BundleSchemaVariableDeclaration<"KWARGS", Field>;

template <template <typename, auto...> typename Field> struct TestBundle {
    Field<TS<std::int64_t>> count;
    Field<TS<float>> value;
};

struct FirstCapturedListItem {
    In<TSL<TS<std::int64_t>, SIZE>, var_args> values;
    Out<TS<std::int64_t>> output;

    void evaluate() const { output.set(*values[0]); }
};

} // namespace

TEST_CASE("a callable variadic TSL captures homogeneous time series and resolves its size",
          "[feature:wiring][feature:variadic-wiring][series:fixed-tsl][declaration:generic]"
          "[definition:function]") {
    const auto source = node([](Out<TS<std::int64_t>>) {});
    const auto tested = graph("name"_n = "capture", [](wp<TSL<TS_T, SIZE>, var_args> values) {
        REQUIRE(values.size() == 2);

        const auto &type = static_cast<const TimeSeriesListType &>(values.type());
        CHECK(type.item_count() == 2);
        CHECK(&type.item_type() == &TS<std::int64_t>::type());
    });

    testing::test_wiring([&] { tested(source(), source()); });
}

TEST_CASE("each variadic pack uses its declared collection operations",
          "[feature:wiring][feature:variadic-wiring][series:fixed-tsl][series:tsb]"
          "[schema:generic][declaration:generic][definition:function]") {
    const auto source = node([](Out<TS<std::int64_t>>) {});
    const auto tested = graph(
        "name"_n = "mixed_collections",
        [](wp<TSL<TS_T, SIZE>, var_args> positional, wp<TSB<KWARGS>, var_kwargs> named) {
            CHECK(positional.size() == 2);
            CHECK(named.size() == 1);

            const auto &list = static_cast<const TimeSeriesListType &>(positional.type());
            CHECK(list.item_count() == 2);
            CHECK(&list.item_type() == &TS<std::int64_t>::type());
            CHECK(named["limit"].is_scalar());
        });

    testing::test_wiring([&] { tested(source(), source(), "limit"_n = 3); });
}

TEST_CASE("an aggregate variadic TSL materializes as one runtime-sized list",
          "[feature:wiring][feature:variadic-wiring][series:fixed-tsl][declaration:generic]"
          "[definition:aggregate][representation:free]") {
    const auto first = node<FirstCapturedListItem>();
    const auto tested = [&](wp<TS<std::int64_t>> left, wp<TS<std::int64_t>> right) {
        return first(left, right).as<TS<std::int64_t>>();
    };

    const auto actual = testing::test_graph(tested, std::array<std::int64_t, 3>{1, 2, 3},
                                            std::array<std::int64_t, 3>{4, 5, 6});

    CHECK(actual.equals({1, 2, 3}));
}

namespace {

struct AggregateListSink {
    In<TSL<TS_T, SIZE>, var_args> values;

    void evaluate() const {}
};

} // namespace

TEST_CASE("a variadic TSL rejects scalars and incompatible item types",
          "[feature:wiring][feature:variadic-wiring][series:fixed-tsl][declaration:generic]"
          "[definition:aggregate]") {
    const auto integer = node([](Out<TS<std::int64_t>>) {});
    const auto floating = node([](Out<TS<float>>) {});
    const auto sink = node<AggregateListSink>();

    testing::test_wiring([&] {
        CHECK_THROWS_AS(sink(1), WiringError);
        CHECK_THROWS_WITH(sink(integer(), floating()),
                          "a time series list requires compatible item types");
    });
}

namespace {

struct EmptyConcreteListGraph {
    wp<TSL<TS<std::int64_t, TimeSeriesGuarantee::final>, SIZE,
           TimeSeriesGuarantee::ordinary>,
       var_args>
        values;

    void wire() const {
        CHECK(values.size() == 0);
        CHECK(&values.type() ==
              &TSL<TS<std::int64_t, TimeSeriesGuarantee::final>, 0,
                   TimeSeriesGuarantee::ordinary>::type());
    }
};

inline constexpr auto EMPTY_G = guarantee_variable<"EMPTY_G">;

} // namespace

TEST_CASE("an empty variadic TSL requires a concrete item type and collection guarantee",
          "[feature:wiring][feature:variadic-wiring][series:fixed-tsl][declaration:concrete]"
          "[definition:aggregate]") {
    const auto concrete = graph<EmptyConcreteListGraph>();
    const auto generic_item = graph("name"_n = "generic_item", [](wp<TSL<TS_T, SIZE>, var_args>) {});
    const auto generic_collection =
        graph("name"_n = "generic_collection", [](wp<TSL<TS<std::int64_t>, SIZE, EMPTY_G>, var_args>) {});

    testing::test_wiring([&] {
        concrete();
        CHECK_THROWS_AS(generic_item(), WiringError);
        CHECK_THROWS_AS(generic_collection(), WiringError);
    });
}

namespace {

struct EmptyListValidity {
    In<TSL<TS<std::int64_t, TimeSeriesGuarantee::ordinary>, SIZE,
           TimeSeriesGuarantee::ordinary>,
       var_args, optional>
        values;
    Out<TS<bool>> output;

    void start() const { output.set(values.valid()); }
};

} // namespace

TEST_CASE("an empty materialized variadic TSL becomes valid at graph start",
          "[feature:validity][feature:variadic-wiring][series:fixed-tsl][declaration:concrete]"
          "[definition:aggregate][representation:free][validity:optional][transition:start]") {
    const auto validity = node<EmptyListValidity>();

    const auto actual = testing::test_graph([&] { return validity(); });

    CHECK(actual.equals({true}));
}

TEST_CASE("a concrete variadic TSL rejects a different capture size",
          "[feature:wiring][feature:variadic-wiring][series:fixed-tsl][declaration:concrete]"
          "[definition:function]") {
    const auto source = node([](Out<TS<std::int64_t>>) {});
    const auto tested = graph("name"_n = "fixed", [](wp<TSL<TS<std::int64_t>, 2>, var_args>) {});

    testing::test_wiring([&] {
        CHECK_THROWS(tested(source()));
        CHECK_THROWS(tested(source(), source(), source()));
    });
}

namespace {

struct DirectListGraph {
    wp<TSL<TS_T, SIZE>, var_args> values;

    void wire() const {
        REQUIRE(values.size() == 2);
        CHECK(values[0].is_time_series());
        CHECK(&values[0].type() == &TS<std::int64_t>::type());
    }
};

} // namespace

TEST_CASE("an exact named TSL binds as one collection and supports lazy item selection",
          "[feature:wiring][feature:variadic-wiring][feature:getitem][series:fixed-tsl]"
          "[binding:direct][definition:aggregate][representation:native]") {
    const auto source = node([](Out<TSL<TS<std::int64_t>, 2>>) {});
    const auto tested = graph<DirectListGraph>();
    auto wiring = GraphWiring{};

    const auto context = WiringContext{wiring};
    tested("values"_n = source());

    CHECK(wiring.nodes().size() == 1);
    CHECK(wiring.outputs().size() == 1);
}

namespace {

struct DirectBundleGraph {
    wp<TSB<SCHEMA>, var_kwargs> values;

    void wire() const {
        REQUIRE(values.size() == 2);
        CHECK(values["count"].is_time_series());
        CHECK(&values["count"].type() == &TS<std::int64_t>::type());
    }
};

struct DirectDualBundleGraph {
    wp<TSB<ARGS>, var_args> positional;
    wp<TSB<KWARGS>, var_kwargs> named;

    void wire() const {
        REQUIRE(positional.size() == 2);
        REQUIRE(named.size() == 2);
        CHECK(&positional[0].type() == &TS<std::int64_t>::type());
        CHECK(&named["value"].type() == &TS<float>::type());
    }
};

} // namespace

TEST_CASE("an exact named TSB binds as one collection and supports lazy field selection",
          "[feature:wiring][feature:variadic-wiring][feature:getattr][series:tsb][schema:named]"
          "[binding:direct][definition:aggregate][representation:native]") {
    const auto source = node([](Out<TSB<TestBundle>>) {});
    const auto tested = graph<DirectBundleGraph>();
    auto wiring = GraphWiring{};

    const auto context = WiringContext{wiring};
    tested("values"_n = source());

    CHECK(wiring.nodes().size() == 1);
    CHECK(wiring.outputs().size() == 1);
}

TEST_CASE("positional and named packs accept direct collections by their parameter names",
          "[feature:wiring][feature:variadic-wiring][feature:getattr][series:tsb][schema:named]"
          "[binding:direct][definition:aggregate][representation:native]") {
    const auto source = node([](Out<TSB<TestBundle>>) {});
    const auto tested = graph<DirectDualBundleGraph>();
    auto wiring = GraphWiring{};

    const auto context = WiringContext{wiring};
    tested("positional"_n = source(), "named"_n = source());

    CHECK(wiring.nodes().size() == 1);
    CHECK(wiring.outputs().size() == 1);
}

TEST_CASE("a nonmatching TSB name remains a captured bundle field",
          "[feature:wiring][feature:variadic-wiring][series:tsb][schema:generic]"
          "[definition:function][representation:free]") {
    const auto source = node([](Out<TSB<TestBundle>>) {});
    const auto tested = graph("name"_n = "capture", [](wp<TSB<SCHEMA>, var_kwargs> values) {
        REQUIRE(values.size() == 1);
        CHECK(values[0].name()->text() == "other");
        CHECK(&values[0].type() == &TSB<TestBundle>::type());
    });

    testing::test_wiring([&] { tested("other"_n = source()); });
}

TEST_CASE("direct collection items are flat resolver and requirement arguments",
          "[feature:wiring][feature:variadic-wiring][feature:operator][series:fixed-tsl]"
          "[binding:direct][definition:function]") {
    auto requirement_calls = 0;
    auto resolver_calls = 0;
    const auto receiver = node(
        eval = [](In<TSL<TS_T, SIZE>, "values"_n, var_args>, Out<TS<DIRECT_RESULT>>) {},
        resolve<DIRECT_RESULT> = [&](const WiringArguments &arguments) -> const ScalarType & {
            ++resolver_calls;
            REQUIRE(arguments.size() == 2);
            CHECK(&arguments.resolved_type(0) == &TS<std::int64_t>::type());
            CHECK(&arguments.resolved_type(1) == &TS<std::int64_t>::type());
            return int_type();
        },
        require = [&](const WiringArguments &arguments) {
            ++requirement_calls;
            REQUIRE(arguments.size() == 2);
            CHECK(arguments.argument_name(0).text() == "0");
            CHECK(arguments.argument_name(1).text() == "1");
            CHECK(&arguments.resolved_type(0) == &TS<std::int64_t>::type());
            CHECK(&arguments.resolved_type(1) == &TS<std::int64_t>::type());
            return accept;
        });
    auto operation = operator_<auto(TS_T) -> TS<std::int64_t>>{"first", {"values"}};
    operation.overload(receiver);

    testing::test_wiring([&] {
        const auto first_source = node([](Out<TSL<TS<std::int64_t>, 2>>) {});
        const auto second_source = node([](Out<TSL<TS<std::int64_t>, 2>>) {});
        (void)operation("values"_n = first_source());
        (void)operation("values"_n = second_source());
    });

    CHECK(resolver_calls == 1);
    CHECK(requirement_calls == 1);
}

namespace {

struct NestedDirectListGraph {
    wp<TSL<TS<std::int64_t>, SIZE>, var_args> values;

    inline static const auto first = node<FirstCapturedListItem>();

    wp<TS<std::int64_t>> wire() const { return first(values).as<TS<std::int64_t>>(); }
};

} // namespace

TEST_CASE("a direct variadic TSL crosses a nested boundary as one reference",
          "[feature:wiring][feature:variadic-wiring][feature:nested-graph][series:fixed-tsl]"
          "[binding:value-to-reference][binding:reference-to-value][shape:nested]"
          "[definition:aggregate]") {
    const auto inner = graph<NestedDirectListGraph>();
    const auto tested = [&](wp<TS<std::int64_t>> left, wp<TS<std::int64_t>> right, wp<TS<bool>> run) {
        return nested(inner, run, "values"_n = tsl(left, right));
    };

    const auto actual = testing::test_graph(tested, std::array<std::int64_t, 2>{1, 2},
                                            std::array<std::int64_t, 2>{3, 4}, std::array{true, true});
    CHECK(actual.equals({1, 2}));

    auto wiring = GraphWiring{};
    const auto value = node([](Out<TS<std::int64_t>>) {});
    const auto run = node([](Out<TS<bool>>) {});
    {
        const auto context = WiringContext{wiring};
        (void)nested(inner, run(), "values"_n = tsl(value(), value()));
    }

    const auto &nested_node = wiring.nodes().back();
    REQUIRE(nested_node.signature().parameters.size() == 2);
    CHECK(nested_node.input_output_indices().size() == 2);
}

namespace {

struct ReadBundleCount {
    In<TSB<TestBundle>> values;
    Out<TS<std::int64_t>> output;

    void evaluate() const { output.set(*values->count); }
};

struct MakeBundle {
    In<TS<std::int64_t>> count;
    In<TS<float>> value;
    Out<TSB<TestBundle>> output;

    void evaluate() const {
        output->count.set(*count);
        output->value.set(*value);
    }
};

struct NestedDirectBundleGraph {
    wp<TSB<TestBundle>, var_kwargs> values;

    inline static const auto count = node<ReadBundleCount>();

    wp<TS<std::int64_t>> wire() const { return count(values).as<TS<std::int64_t>>(); }
};

} // namespace

TEST_CASE("a direct variadic TSB crosses a nested boundary as one reference",
          "[feature:wiring][feature:variadic-wiring][feature:nested-graph][series:tsb][schema:named]"
          "[binding:value-to-reference][binding:reference-to-value][shape:nested]"
          "[definition:aggregate]") {
    const auto inner = graph<NestedDirectBundleGraph>();
    const auto make_bundle = node<MakeBundle>();
    const auto tested = [&](wp<TS<std::int64_t>> count, wp<TS<float>> value, wp<TS<bool>> run) {
        return nested(inner, run, "values"_n = make_bundle(count, value));
    };

    const auto actual = testing::test_graph(tested, std::array<std::int64_t, 2>{1, 2},
                                            std::array<float, 2>{3.0F, 4.0F}, std::array{true, true});
    CHECK(actual.equals({1, 2}));

    auto wiring = GraphWiring{};
    const auto count = node([](Out<TS<std::int64_t>>) {});
    const auto value = node([](Out<TS<float>>) {});
    const auto run = node([](Out<TS<bool>>) {});
    {
        const auto context = WiringContext{wiring};
        (void)nested(inner, run(), "values"_n = make_bundle(count(), value()));
    }

    const auto &nested_node = wiring.nodes().back();
    REQUIRE(nested_node.signature().parameters.size() == 2);
    CHECK(nested_node.input_output_indices().size() == 2);
}
