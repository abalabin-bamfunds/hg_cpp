#include <bg/graph.hpp>
#include <bg/node.hpp>
#include <bg/operators.hpp>
#include <bg/testing/test_graph.hpp>
#include <bg/testing/test_wiring.hpp>
#include <bg/type/type_resolution_error.hpp>

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <span>
#include <stdexcept>
#include <string>

using namespace bg;
using namespace bg::literals;

namespace {

struct defaulted_aggregate_graph {
    wp<TS<i64>> value;

    struct defaults {
        i64 value{6};
    };

    wp<TS<i64>> wire() const { return value; }
};

struct defaulted_aggregate_node {
    In<TS<i64>> value;
    Out<TS<i64>> output;

    struct defaults {
        i64 value{8};
    };

    void evaluate() const { output.set(*value); }
};

struct unknown_default_aggregate_graph {
    wp<TS<i64>> value;

    struct defaults {
        i64 unknown{2};
    };

    wp<TS<i64>> wire() const { return value; }
};

} // namespace

TEST_CASE("callable graph defaults promote scalar values to constant time series",
          "[feature:wiring][feature:wiring-defaults][series:value][definition:function]") {
    const auto with_default = graph({"value"_n = i64{7}}, [](wp<TS<i64>> value) { return value; });
    const auto defaulted = [&] { return with_default(); };
    const auto supplied = [&] {
        return with_default(operators::const_(i64{11}));
    };

    CHECK(testing::test_graph(defaulted).equals<i64>({7}));
    CHECK(testing::test_graph(supplied).equals({11}));
}

TEST_CASE("callable node defaults promote scalar values to constant time series",
          "[feature:wiring][feature:wiring-defaults][series:value][definition:function]") {
    const auto copy = node({"value"_n = i64{8}}, [](In<TS<i64>> value, Out<TS<i64>> output) {
        output.set(*value);
    });
    const auto graph = [&] { return copy(); };

    CHECK(testing::test_graph(graph).equals({8}));
}

TEST_CASE("callable eval declarations support scalar defaults",
          "[feature:wiring][feature:wiring-defaults][scalar:integer][definition:function]") {
    const auto source = node(
        {"value"_n = i64{12}},
        eval = [](i64 value, Out<TS<i64>> output) { output.set(value); });
    const auto graph = [&] { return source(); };

    CHECK(testing::test_graph(graph).equals({12}));
}

TEST_CASE("test_graph literal inputs omit trailing defaulted node arguments",
          "[feature:wiring][feature:wiring-defaults][feature:test-graph][series:value][definition:function]") {
    const auto add = node(
        {"input"_n, "amount"_n = i64{1}},
        eval = [](In<TS<i64>> input, i64 amount, Out<TS<i64>> output) { output.set(*input + amount); });

    CHECK(testing::test_graph(add, {1, 2}).equals<i64>({2, 3}));
}

TEST_CASE("aggregate node and graph defaults use reflected field names",
          "[feature:wiring][feature:wiring-defaults][series:value][definition:aggregate]") {
    const auto copy = node<defaulted_aggregate_node>();
    const auto graph = bg::graph<defaulted_aggregate_graph>();
    const auto defaulted_node = [&] { return copy(); };
    const auto defaulted_graph = [&] { return graph(); };

    CHECK(testing::test_graph(defaulted_node).equals({8}));
    CHECK(testing::test_graph(defaulted_graph).equals({6}));
}

TEST_CASE("an explicitly absent optional scalar overrides a wiring default",
          "[feature:wiring][feature:wiring-defaults][scalar:integer][definition:function]") {
    const auto with_default = graph({"value"_n = i64{9}}, [](std::optional<i64> value) {
        return operators::const_(value.value_or(2)).as<TS<i64, TimeSeriesGuarantee::const_>>();
    });
    const auto explicit_absence = [&] { return with_default(std::optional<i64>{}); };

    CHECK(testing::test_graph(explicit_absence).equals({2}));
}

TEST_CASE("argument binding preserves names around an omitted time-series default",
          "[feature:wiring][feature:wiring-defaults][series:value][definition:function]") {
    const auto combine = node([](In<TS<i64>> x, In<TS<i64>> y, In<TS<i64>> z, Out<TS<i64>> output) {
        output.set(*x + *y * 10 + *z * 100);
    });
    const auto with_default = graph(
        {"x"_n, "y"_n = i64{2}, "z"_n},
        [&](wp<TS<i64>> x, wp<TS<i64>> y, wp<TS<i64>> z) { return combine(x, y, z); });
    const auto run = [&] {
        const auto x = operators::const_(i64{3});
        const auto z = operators::const_(i64{4});
        const auto supplied = details::make_wiring_arguments("x"_n = x, "z"_n = z);
        const auto bound = details::bind_wiring_arguments(with_default.signature(), supplied);
        const auto result = static_cast<const WiringTemplate &>(with_default)(
            std::span<const WiringArgument>{bound});
        return wp<TS<i64>>{*result};
    };

    CHECK(testing::test_graph(run).equals({423}));
}

TEST_CASE("operator defaults precede implementation defaults and remain cacheable",
          "[feature:operator][feature:wiring][feature:wiring-defaults][series:value][definition:function]") {
    auto requirement_calls = 0;
    auto operation = operator_<auto(TS<i64>, i64) -> TS<i64>>{
        "defaulted", {"input"_n, "amount"_n = i64{2}}};
    operation.overload(node(
        {"input"_n, "amount"_n = i64{9}},
        eval = [](In<TS<i64>> input, i64 amount, Out<TS<i64>> output) { output.set(*input + amount); },
        require = [&](const WiringArguments &arguments) {
            ++requirement_calls;
            return scalar_cast<i64>(*arguments.scalar("amount")).value() == 2 ? accept : reject();
        }));

    const auto graph = [&](wp<TS<i64>> input) {
        const auto first = operation(input);
        (void)operation(input);
        return first;
    };

    CHECK(testing::test_graph(graph, {3}).equals({5}));
    CHECK(requirement_calls == 1);
}

TEST_CASE("generic graph defaults resolve their scalar type",
          "[feature:wiring][feature:wiring-defaults][feature:type-system][declaration:generic][series:value]") {
    using VALUE = ScalarVariable<"VALUE">;
    const auto identity = graph<VALUE>(
        {"value"_n = i64{7}}, [](wp<TS<VALUE>> value) { return value; });
    const auto defaulted = [&] { return identity(); };

    CHECK(testing::test_graph(defaulted).equals<i64>({7}));
}

TEST_CASE("callable node defaults own their values and participate in deduplication",
          "[feature:wiring][feature:wiring-defaults][series:value][definition:function]") {
    auto evaluations = 0;
    const auto combine = node([](In<TS<i64>> first, In<TS<i64>> second, Out<TS<i64>> output) {
        output.set(*first + *second);
    });
    const auto make_source = [&] {
        auto value = ScalarValue<i64>{17};
        return node({"value"_n = value.view()},
                    eval = [&](i64 value, Out<TS<i64>> output) {
                        ++evaluations;
                        output.set(value);
                    });
    };
    const auto source = make_source();
    const auto graph = [&] {
        const auto first = source();
        const auto same_value = source(i64{17});
        return combine(first, same_value);
    };

    CHECK(testing::test_graph(graph).equals({34}));
    CHECK(evaluations == 1);
}

TEST_CASE("wiring default declarations reject repeated names",
          "[feature:wiring][feature:wiring-defaults][definition:function]") {
    CHECK_THROWS_AS(
        (graph({"value"_n = i64{1}, "value"_n = i64{2}}, [](i64, i64) {})), std::invalid_argument);
}

TEST_CASE("wiring default declarations validate names and parameter counts",
          "[feature:wiring][feature:wiring-defaults][definition:function]") {
    CHECK_THROWS_AS((graph({std::string_view{}}, [](i64) {})), std::invalid_argument);
    CHECK_THROWS_AS((graph({"value"_n = i64{1}}, [](i64, i64) {})), std::invalid_argument);
    CHECK_THROWS_AS((graph({"other"_n = i64{1}}, [](wp<TS<i64>, "actual"_n>) {})), std::invalid_argument);
    CHECK_THROWS_AS((bg::graph<unknown_default_aggregate_graph>()), std::invalid_argument);
}

TEST_CASE("incompatible wiring defaults fail when the call uses them",
          "[feature:wiring][feature:wiring-defaults][series:value][definition:function]") {
    const auto invalid = graph({"value"_n = std::string{"bad"}}, [](wp<TS<i64>> value) { return value; });

    CHECK_THROWS_AS(testing::test_graph([&] { return invalid(); }), TypeResolutionError);
}
