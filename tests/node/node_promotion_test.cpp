#include <bg/node.hpp>
#include <bg/operators.hpp>
#include <bg/testing/test_graph.hpp>
#include <bg/time_series/tsl/tsl.hpp>
#include <bg/type/time_series_value_type.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <optional>

namespace {

using namespace bg;
using namespace bg::literals;

struct promoted_aggregate {
    int offset;
    In<TS<i64>> input;
    Scalar<i64> scalar;
    std::optional<int> extra;
    Out<TS<i64>> output;

    void evaluate() const {
        output.set(*input + offset + scalar.value() + extra.value_or(0));
    }
};

struct lifecycle_aggregate {
    int offset;
    In<TS<i64>> input;
    State<i64> state;
    Out<TS<i64>> output;

    void start() const { started_with = offset; }
    void evaluate() const { output.set(*input + offset); }
    void stop() const noexcept { ++stopped; }
    void finalize() const noexcept { ++finalized; }

    inline static int started_with{};
    inline static int stopped{};
    inline static int finalized{};
};

struct promoted_invalidation_aggregate {
    int offset;
    int scalar_offset;
    std::optional<int> optional_offset;
    In<TS<i64>> input;
    State<i64> state;
    Out<TS<i64>> output;

    void evaluate() const {
        ++*state;
        output.set(*input + offset + scalar_offset + optional_offset.value_or(0));
    }
    void on_invalidation() const {
        observed_offset = scalar_offset;
        observed_optional_has_value = optional_offset.has_value();
        output.set(-1);
    }

    inline static int observed_offset{};
    inline static bool observed_optional_has_value{};
};

struct promoted_dynamic_reference_aggregate {
    int offset;
    In<TS<i64>> input;
    In<REF<TSL<TS<i64, TimeSeriesGuarantee::ordinary>, 1, TimeSeriesGuarantee::ordinary>,
           TimeSeriesGuarantee::ordinary>>
        collection;
    In<REF<TS<i64, TimeSeriesGuarantee::ordinary>, TimeSeriesGuarantee::ordinary>, dynamic_binding, optional,
       "selected"_n> selected;
    Out<TS<i64>> output;

    void evaluate() const {
        selected.bind(collection[0]);
        output.set(*input + offset);
    }
};

struct compiled_runtime_metadata_aggregate {
    int offset;
    In<TS<i64>> input;
    Scalar<SCALAR> factor;
    In<TS<bool>> select_second;
    In<REF<TSL<TS<i64, TimeSeriesGuarantee::ordinary>, 2, TimeSeriesGuarantee::ordinary>,
           TimeSeriesGuarantee::ordinary>>
        collection;
    In<REF<TS<i64, TimeSeriesGuarantee::ordinary>, TimeSeriesGuarantee::ordinary>, dynamic_binding, optional,
       "selected"_n>
        selected;
    Out<REF<TS<SCALAR>>> output;

    void evaluate() const {
        const auto factor_value = scalar_cast<i64>(factor).value();
        const auto input_value = *input;
        const auto use_second = ((*select_second) != ((offset + input_value * factor_value) % 2 == 0));
        selected.bind(collection[use_second ? 1 : 0]);
        output.set_value(selected.value());
    }
};

} // namespace

TEST_CASE("callable plain parameters accept scalar and time-series arguments",
          "[feature:node-api][definition:function][series:value][scalar:integer]") {
    const auto add = node([](int lhs, int rhs) { return lhs + rhs; });
    REQUIRE(add.signature().output() == &TS<i64>::type());

    const auto scalar_graph = [=] { return add(2, 3); };
    CHECK(testing::test_graph(scalar_graph).equals({5}));

    const auto time_series_graph = [=](wp<TS<i64>> input) { return add(input, input); };
    CHECK(testing::test_graph(time_series_graph, {1, 2, 3}).equals({2, 4, 6}));

    const auto mixed_graph = [=](wp<TS<i64>> input) { return add(input, 3); };
    CHECK(testing::test_graph(mixed_graph, {1, 2, 3}).equals({4, 5, 6}));
}

TEST_CASE("callable optional arguments and return values preserve absence",
          "[feature:node-api][definition:function][series:value][scalar:integer][validity:optional]") {
    const auto select = node([](int value) -> std::optional<int> {
        if (value == 2) {
            return std::nullopt;
        }
        return value;
    });

    const auto graph = [=](wp<TS<i64>> input) { return select(input); };
    CHECK(testing::test_graph(graph, {1, 2, 3}).equals({1, std::nullopt, 3}));

    static const std::optional<int> result{7};
    const auto reference_result = node([]() -> const std::optional<int> & { return result; });
    const auto constant_graph = [=] { return reference_result(); };
    CHECK(testing::test_graph(constant_graph).equals({7}));

    const auto optional_input = node(
        eval = [](std::optional<int> value) { return value.value_or(-1); }, schedule_on_start = true);
    const auto optional_graph = [=](wp<TS<i64>> input) { return optional_input(input); };
    CHECK(testing::test_graph(optional_graph, {std::nullopt, 1, 2}).equals({-1, 1, 2}));
}

TEST_CASE("required promoted callable inputs invalidate and recover their outputs",
          "[feature:node-api][definition:function][series:value][scalar:integer][validity:required]") {
    const auto twice = node([](int value) { return value * 2; });
    const auto gate = node([](In<TS<i64>> input, Out<TS<i64>> output) {
        if (*input == -1) {
            output.invalidate();
        } else {
            output.set(*input);
        }
    });
    const auto graph = [=](wp<TS<i64>> input) {
        return operators::default_(twice(gate(input)), -1).as<TS<i64>>();
    };

    CHECK(testing::test_graph(graph, {1, -1, 3}).equals({2, -1, 6}));
}

TEST_CASE("callable bool returns produce bool time series outputs",
          "[feature:node-api][definition:function][series:value][scalar:boolean]") {
    const auto negate = node([](bool value) { return !value; });
    const auto graph = [=](wp<TS<bool>> input) { return negate(input); };

    CHECK(testing::test_graph(graph, {true, false, true}).equals({false, true, false}));
}

TEST_CASE("callable start scheduling can be disabled for scalar-only nodes",
          "[feature:node-api][definition:function][series:value][scalar:integer][feature:scheduling]") {
    const auto source = node([](int value) { return value; });
    const auto disabled = node(eval = [](int value) { return value; }, schedule_on_start = false);

    CHECK(testing::test_graph([=] { return source(5); }).equals({5}));
    const auto result = testing::test_graph([=] { return disabled(5); });
    REQUIRE(result.size() == 1);
    CHECK_FALSE(result[0].has_value());
}

TEST_CASE("callable scalar-or-time-series inputs accept constant and default bindings",
          "[feature:node-api][definition:function][series:value][scalar:integer][guarantee:const]") {
    const auto add = node([](int lhs, int rhs) { return lhs + rhs; });
    const auto graph = [=](wp<TS<i64>> input) {
        return add(operators::const_(3), operators::default_(input, 7));
    };

    CHECK(testing::test_graph(graph, {std::nullopt, 1, 2}).equals({10, 4, 5}));
}

TEST_CASE("callable scalar-or-time-series inputs read references to values",
          "[feature:node-api][feature:reference][definition:function][series:value]") {
    const auto identity = node([](int value) { return value; });
    const auto graph = [=](wp<TS<i64>> input) { return identity(input.as<REF<TS<i64>>>()); };

    CHECK(testing::test_graph(graph, {1, 2, 3}).equals({1, 2, 3}));
}

TEST_CASE("callable scalar-or-time-series inputs preserve stronger guarantees",
          "[feature:node-api][definition:function][series:value][scalar:integer][guarantee:final][guarantee:const]") {
    const auto identity = node([](int value) { return value; });
    const std::array<const Type *, 1> final_input{&TS<i64, TimeSeriesGuarantee::final>::type()};
    const std::array<const Type *, 1> const_input{&TS<i64, TimeSeriesGuarantee::const_>::type()};

    CHECK(identity.signature().resolve(final_input).parameters[0].type == final_input[0]);
    CHECK(identity.signature().resolve(const_input).parameters[0].type == const_input[0]);
}

TEST_CASE("promoted callable parameters coexist with explicit input, scalar, and output fields",
          "[feature:node-api][definition:function][series:value][scalar:integer]") {
    const auto combine = node([](int offset, In<TS<i64>> input, Scalar<i64> factor, Out<TS<i64>> output) {
        output.set(offset + *input * factor.value());
    });

    const auto scalar_graph = [=](wp<TS<i64>> input) { return combine(3, input, 2); };
    CHECK(testing::test_graph(scalar_graph, {1, 2, 3}).equals({5, 7, 9}));

    const auto time_series_graph = [=](wp<TS<i64>> input) { return combine(input, input, 2); };
    CHECK(testing::test_graph(time_series_graph, {1, 2, 3}).equals({3, 6, 9}));
}

TEST_CASE("aggregate plain fields accept scalar or time-series bindings beside explicit fields",
          "[feature:node-api][definition:aggregate][series:value][scalar:integer]") {
    const auto sum = node<promoted_aggregate>();
    const auto graph = [=](wp<TS<i64>> input) { return sum(2, input, 3, std::optional<int>{4}); };

    CHECK(testing::test_graph(graph, {1, 2, 3}).equals({10, 11, 12}));

    const auto time_series_graph = [=](wp<TS<i64>> input) {
        return sum(input, input, 3, std::optional<int>{});
    };
    CHECK(testing::test_graph(time_series_graph, {1, 2, 3}).equals({5, 7, 9}));

    const auto &signature = sum.signature();
    CHECK(signature.parameter(2).type == &int64_type());
}

TEST_CASE("aggregate lifecycle functions reject promoted time-series fields",
          "[feature:node-api][definition:aggregate][series:value][scalar:integer][feature:lifecycle]") {
    const auto node_template = node<lifecycle_aggregate>();
    const auto valid_graph = [=](wp<TS<i64>> input) { return node_template(3, input); };
    lifecycle_aggregate::started_with = 0;
    lifecycle_aggregate::stopped = 0;
    lifecycle_aggregate::finalized = 0;
    CHECK(testing::test_graph(valid_graph, {1}).equals({4}));
    CHECK(lifecycle_aggregate::started_with == 3);
    CHECK(lifecycle_aggregate::stopped == 1);
    CHECK(lifecycle_aggregate::finalized == 1);

    const auto invalid_graph = [=](wp<TS<i64>> input) { return node_template(input, input); };
    CHECK_THROWS(testing::test_graph(invalid_graph, {1}));
}

TEST_CASE("aggregate invalidation receives promoted scalar and empty optional fields",
          "[feature:node-api][definition:aggregate][series:value][scalar:integer][feature:lifecycle]") {
    const auto node_template = node<promoted_invalidation_aggregate>();
    const auto gate = node([](In<TS<i64>> input, Out<TS<i64>> output) {
        if (*input == -1) {
            output.invalidate();
        } else {
            output.set(*input);
        }
    });
    const auto graph = [=](wp<TS<i64>> input) {
        const auto value = gate(input);
        return node_template(value, 5, value, value);
    };
    promoted_invalidation_aggregate::observed_offset = 0;
    promoted_invalidation_aggregate::observed_optional_has_value = true;

    CHECK(testing::test_graph(graph, {1, -1, 2}).equals({8, -1, 11}));
    CHECK(promoted_invalidation_aggregate::observed_offset == 5);
    CHECK_FALSE(promoted_invalidation_aggregate::observed_optional_has_value);
}

TEST_CASE("promoted fields coexist with aggregate dynamic reference inputs",
          "[feature:node-api][definition:aggregate][series:value][feature:reference]") {
    const auto node_template = node<promoted_dynamic_reference_aggregate>();
    const auto scalar_graph = [=](wp<TS<i64>> input) {
        const auto collection = tsl(input);
        return node_template(5, input, collection);
    };
    const auto time_series_graph = [=](wp<TS<i64>> input) {
        const auto collection = tsl(input);
        return node_template(input, input, collection);
    };

    CHECK(testing::test_graph(scalar_graph, {1, 2}).equals({6, 7}));
    CHECK(testing::test_graph(time_series_graph, {1, 2}).equals({2, 4}));
}

TEST_CASE("compiled runtime metadata locates interleaved parameters, output, and dynamic reference slots",
          "[feature:node-api][definition:aggregate][series:value][scalar:integer][feature:reference]") {
    const auto combine = node<compiled_runtime_metadata_aggregate>();
    const auto read = node([](i64 value) { return value; });
    const auto graph = [=](wp<TS<i64>> input, wp<TS<bool>> select_second, wp<TS<i64>> first,
                           wp<TS<i64>> second) {
        return read(combine(5, input, 3, select_second, tsl(first, second)));
    };

    const auto input = std::array<std::optional<int>, 5>{1, 2, std::nullopt, std::nullopt, std::nullopt};
    const auto select_second = std::array<std::optional<bool>, 5>{false, false, std::nullopt, true, std::nullopt};
    const auto first = std::array<std::optional<int>, 5>{10, 20, 30, 40, std::nullopt};
    const auto second = std::array<std::optional<int>, 5>{100, 200, 300, std::nullopt, 400};

    CHECK(testing::test_graph(graph, input, select_second, first, second).equals({100, 20, 30, 300, 400}));
}
