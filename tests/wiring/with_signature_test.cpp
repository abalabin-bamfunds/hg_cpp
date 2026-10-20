#include <bg/graph.hpp>
#include <bg/node.hpp>
#include <bg/testing/test_graph.hpp>
#include <bg/time_series/tsb/tsb.hpp>
#include <bg/with_signature.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <type_traits>
#include <utility>

namespace {

using namespace bg;
using namespace bg::literals;

template <template <typename, auto...> typename Field>
using VALUES = BundleSchemaVariableDeclaration<"WITH_SIGNATURE_VALUES", Field>;

template <template <typename, auto...> typename Field>
using POSITIONAL = BundleSchemaVariableDeclaration<"WITH_SIGNATURE_POSITIONAL", Field>;

} // namespace

TEST_CASE("with_signature gives a positional variadic graph a fixed call signature",
          "[feature:wiring][feature:variadic-wiring][definition:function][series:value]") {
    const auto add = node([](i64 left, i64 right) { return left + right; });
    const auto variadic = graph("name"_n = "variadic_add", [=](wp<TSB<VALUES>, var_args> values) -> wp<TS<i64>> {
        return add(values[0].time_series(), values[1].time_series());
    });
    const auto signature = Signature{util::symbol{"add"},
                                     {{util::symbol{"left"}, &TS<i64>::type()},
                                      {util::symbol{"right"}, &TS<i64>::type()}},
                                     TS<i64>::type()};
    const auto callable = with_signature(variadic, signature);

    static_assert(std::same_as<decltype(callable(std::declval<wp<TS<i64>>>(), std::declval<wp<TS<i64>>>())),
                               wp<TS<i64>>>);
    CHECK(callable.signature().parameter_count() == 2);
    CHECK(callable.signature().parameter(0).name.text() == "left");
    CHECK(callable.signature().parameter(1).name.text() == "right");

    const auto actual = testing::test_graph(callable, std::array<i64, 2>{1, 2}, std::array<i64, 2>{10, 20});
    CHECK(actual.equals<i64>({11, 22}));
}

TEST_CASE("with_signature forwards fixed parameter names to a named variadic graph",
          "[feature:wiring][feature:variadic-wiring][definition:function][schema:named]") {
    const auto add = node([](i64 left, i64 right) { return left + right; });
    const auto variadic = graph("name"_n = "named_add",
                                [=](wp<TSB<POSITIONAL>, var_args> positional,
                                    wp<TSB<VALUES>, var_kwargs> values) -> wp<TS<i64>> {
                                    CHECK(positional.size() == 0);
                                    return add(values["0"].time_series(), values["right"].time_series());
                                });
    const auto signature = Signature{util::symbol{"add"},
                                     {{util::symbol{"0"}, &TS<i64>::type()},
                                      {util::symbol{"right"}, &TS<i64>::type()}},
                                     TS<i64>::type()};
    const auto callable = with_signature(variadic, signature);

    const auto actual = testing::test_graph(callable, std::array<i64, 2>{1, 2}, std::array<i64, 2>{10, 20});
    CHECK(actual.equals<i64>({11, 22}));
}

TEST_CASE("with_signature gives a variadic node a fixed call signature",
          "[feature:wiring][feature:variadic-wiring][definition:function][series:tsb]") {
    const auto count = node([](In<TSB<VALUES>, var_args> values, Out<TS<i64>> output) {
        output.set(static_cast<i64>(values.size()));
    });
    const auto signature = Signature{util::symbol{"count"},
                                     {{util::symbol{"first"}, &TS<i64>::type()},
                                      {util::symbol{"second"}, &TS<i64>::type()}},
                                     TS<i64>::type()};
    const auto callable = with_signature(count, signature);

    const auto actual = testing::test_graph(callable, std::array<i64, 2>{1, 2}, std::array<i64, 2>{10, 20});
    CHECK(actual.equals<i64>({2, 2}));
}

TEST_CASE("with_signature checks its declared output type",
          "[feature:wiring][feature:variadic-wiring][definition:function][series:value]") {
    const auto source = node([](Out<TS<i64>> output) { output.set(1); });
    const auto variadic = graph("name"_n = "wrong_output", [=](wp<TSB<VALUES>, var_args>) { return source(); });
    const auto signature = Signature{util::symbol{"wrong_output"},
                                     {{util::symbol{"input"}, &TS<i64>::type()}}, TS<float>::type()};
    const auto callable = with_signature(variadic, signature);

    auto wiring = GraphWiring{};
    const auto context = WiringContext{wiring};
    CHECK_THROWS_AS(callable(source()), WiringError);
}
