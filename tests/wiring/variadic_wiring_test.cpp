#include <bg/graph.hpp>
#include <bg/graph/details/normalized_wiring_arguments.hpp>
#include <bg/graph/details/wiring_record_overlay.hpp>
#include <bg/nested.hpp>
#include <bg/node.hpp>
#include <bg/operator.hpp>
#include <bg/operators.hpp>
#include <bg/testing/test_graph.hpp>
#include <bg/testing/test_wiring.hpp>
#include <bg/time_series/tsb/tsb.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>

namespace {

using namespace bg;
using namespace bg::literals;

using VARIADIC_RESULT = ScalarVariable<"VARIADIC_RESULT">;

template <template <typename, auto...> typename Field>
using ARGS = BundleSchemaVariableDeclaration<"ARGS", Field>;

template <template <typename, auto...> typename Field>
using KWARGS = BundleSchemaVariableDeclaration<"KWARGS", Field>;

struct AggregateVariadicGraph {
    wp<TSB<SCHEMA>, var_args> values;

    void wire() const { REQUIRE(values.size() == 2); }
};

struct AggregateVariadicNode {
    In<TSB<SCHEMA>, var_args> values;

    void evaluate() const {}
};

struct AggregateDualVariadicGraph {
    wp<TSB<ARGS>, var_args> positional;
    wp<TSB<KWARGS>, var_kwargs> named;

    void wire() const {
        CHECK(positional.size() == 2);
        CHECK(named.size() == 2);
    }
};

struct AggregateDualVariadicNode {
    In<TSB<ARGS>, var_args> positional;
    In<TSB<KWARGS>, var_kwargs> named;

    void evaluate() const {}
};

struct AggregateDualVariadicValue {
    In<TSB<ARGS>, var_args> positional;
    In<TSB<KWARGS>, var_kwargs> named;
    Out<TS<std::int64_t>> output;

    void evaluate() const {
        auto left = std::int64_t{};
        positional[0].visit([&](const In<TS<SCALAR>> *value) {
            left = scalar_cast<std::int64_t>(value->value_view()).value();
        }, ddv::noop);
        named[0].visit(
            [&](const In<TS<SCALAR>> *value) {
                output.set(left + scalar_cast<std::int64_t>(value->value_view()).value());
            },
            ddv::noop);
    }
};

struct MaterializedVariadicValue {
    In<TSB<SCHEMA>, var_args> values;
    Out<TS<std::int64_t>> output;

    void evaluate() const {
        auto scalar = std::int64_t{};
        values[0].visit([&](const In<TS<SCALAR>> *value) {
            scalar = scalar_cast<std::int64_t>(value->value_view()).value();
        }, ddv::noop);
        values[1].visit(
            [&](const In<TS<SCALAR>> *value) {
                output.set(scalar + scalar_cast<std::int64_t>(value->value_view()).value());
            },
            ddv::noop);
    }
};

struct EmptyVariadicValue {
    In<TSB<SCHEMA>, var_args> values;
    Out<TS<bool>> output;

    void evaluate() const { output.set(values.valid() && values.size() == 0); }
};

struct CachedNamedVariadicNode {
    In<TSB<SCHEMA>, var_kwargs> values;
    Out<TS<VARIADIC_RESULT>> output;

    inline static auto requirement_calls = 0;
    inline static auto resolver_calls = 0;

    static RequirementResult require(const WiringArguments &) {
        ++requirement_calls;
        return accept;
    }

    static const ScalarType &resolve(VARIADIC_RESULT, const WiringArguments &) {
        ++resolver_calls;
        return int64_type();
    }

    void evaluate() const {}
};

struct ZeroModeNode {
    int mode;
    Out<TS<std::int64_t>> output;

    inline static auto requirement_calls = 0;

    static RequirementResult require(const WiringArguments &arguments) {
        ++requirement_calls;
        return scalar_cast<std::int64_t>(*arguments.scalar("mode")).value() == 0 ? accept : reject();
    }

    void evaluate() const {}
};

struct VariadicModeFallbackNode {
    In<TSB<SCHEMA>, var_args> values;
    Out<TS<std::int64_t>> output;

    inline static auto requirement_calls = 0;

    static RequirementResult require(const WiringArguments &) {
        ++requirement_calls;
        return accept;
    }

    void evaluate() const {}
};

struct ScalarDependentRejectingVariadicNode {
    In<TSB<SCHEMA>, var_args> values;
    Out<TS<std::int64_t>> output;

    inline static auto requirement_calls = 0;

    static RequirementResult require(const WiringArguments &arguments) {
        ++requirement_calls;
        (void)arguments.scalar(0);
        return reject();
    }

    void evaluate() const {}
};

struct OptionalFixedVariadicNode {
    std::optional<int> fixed;
    In<TSB<SCHEMA>, var_args> values;
    Out<TS<std::int64_t>> output;

    inline static auto requirement_calls = 0;

    static RequirementResult require(const WiringArguments &) {
        ++requirement_calls;
        return accept;
    }

    void evaluate() const {}
};

struct NamedFixedVariadicGraph {
    int fixed;
    wp<TSB<SCHEMA>, var_kwargs> values;

    void wire() const {}
};

struct RejectingVariadicNode {
    In<TSB<SCHEMA>, var_args> values;

    static RequirementResult require(const WiringArguments &) { return reject(); }

    void evaluate() const {}
};

} // namespace

TEST_CASE("a positional variadic graph preserves its arguments",
          "[feature:wiring][feature:variadic-wiring][series:tsb][schema:generic][definition:function]") {
    const auto sink = node([](In<TSB<SCHEMA>, var_args>) {});
    const auto tested = graph("name"_n = "variadic", [&](wp<TSB<SCHEMA>, var_args> values) {
        REQUIRE(values.size() == 2);
        CHECK(values[0].is_scalar());
        CHECK(values[1].is_scalar());
        const auto &type = static_cast<const TimeSeriesBundleType &>(values.type());
        CHECK(&type.item_type(0) == &TS<std::int64_t, TimeSeriesGuarantee::const_>::type());
        sink(values);
    });

    testing::test_wiring([&] { tested(1, 2); });
}

TEST_CASE("a resolution overlay owns variadic wiring records used by probing",
          "[feature:wiring][feature:variadic-wiring][series:tsb][schema:generic]") {
    const auto source = node([](Out<TS<std::int64_t>>) {});
    const auto candidate = graph("name"_n = "candidate", [](wp<TSB<SCHEMA>, var_args>) {});
    auto wiring = GraphWiring{};
    const auto context = WiringContext{wiring};
    const auto supplied = std::array{WiringArgument{source()}, WiringArgument{source()}};
    auto records = details::WiringRecordOverlay{wiring};

    const auto normalized = details::probe_wiring_arguments(candidate.signature(), supplied, records);

    REQUIRE(normalized.arguments.size() == 1);
    const auto point = normalized.arguments.front().time_series();
    CHECK(&point.graph() == &wiring);
    CHECK(candidate.signature().parameter(0).type->accepts(point.type()));
}

TEST_CASE("a named variadic graph finds its arguments",
          "[feature:wiring][feature:variadic-wiring][series:tsb][schema:generic][definition:function]") {
    const auto tested = graph("name"_n = "variadic", [&](wp<TSB<SCHEMA>, var_kwargs> values) {
        REQUIRE(values.size() == 2);
        REQUIRE(values.find("window"));
        CHECK(values.find("window")->name()->text() == "window");
        CHECK(values["limit"].is_scalar());
        CHECK_FALSE(values.find("missing"));
        CHECK_THROWS_AS(values["missing"], WiringError);
    });

    testing::test_wiring([&] { tested("window"_n = 1, "limit"_n = 2); });
    CHECK_THROWS_AS(testing::test_wiring([&] { tested(1); }), WiringError);
    CHECK_THROWS_AS(testing::test_wiring([&] { tested("window"_n = 1, "window"_n = 2); }), WiringError);
    CHECK_THROWS_AS(
        testing::test_wiring([&] { tested("window"_n = std::optional<int>{}, "window"_n = 2); }), WiringError);
}

TEST_CASE("a variadic graph forwards a complete pack only",
          "[feature:wiring][feature:variadic-wiring][series:tsb][schema:generic][definition:function]") {
    const auto receiver = graph("name"_n = "receiver", [&](wp<TSB<SCHEMA>, var_args> values) {
        REQUIRE(values.size() == 1);
        CHECK(&values[0].scalar().type() == &cpp_scalar_type<std::int64_t>());
    });
    const auto sender = graph("name"_n = "sender", [&](wp<TSB<SCHEMA>, var_args> values) { receiver(values); });
    const auto selected = graph("name"_n = "selected", [&](wp<TSB<SCHEMA>, var_args> values) {
        receiver(values[0]);
    });
    const auto combined = graph("name"_n = "combined", [&](wp<TSB<SCHEMA>, var_args> values) {
        receiver(values, 2);
    });
    const auto named = graph("name"_n = "named", [&](wp<TSB<SCHEMA>, var_args> values) {
        receiver("values"_n = values);
    });
    const auto fixed = graph("name"_n = "fixed", [](int) {});
    const auto renamed = graph("name"_n = "renamed", [&](wp<TSB<SCHEMA>, var_args> values) {
        fixed("value"_n = values[0]);
    });

    testing::test_wiring([&] { sender(1); });
    testing::test_wiring([&] { selected(1); });
    CHECK_THROWS_AS(testing::test_wiring([&] { combined(1); }), WiringError);
    CHECK_THROWS_WITH(testing::test_wiring([&] { named(1); }),
                      Catch::Matchers::ContainsSubstring("a forwarded variadic wiring record cannot be named"));
    CHECK_THROWS_AS(testing::test_wiring([&] { renamed(1); }), WiringError);
}

TEST_CASE("named variadic arguments are validated before absent values are removed",
          "[feature:wiring][feature:variadic-wiring][series:tsb][schema:generic]"
          "[definition:aggregate]") {
    const auto tested = graph<NamedFixedVariadicGraph>();

    CHECK_THROWS_WITH(
        testing::test_wiring([&] { tested(1, "fixed"_n = std::optional<int>{}); }),
        "a variadic argument name conflicts with a fixed parameter");
}

namespace {

template <template <typename, auto...> typename Field> struct forwarded_named_bundle {
    Field<TS<i64>> fixed;
    Field<TS<float>> value;
};

} // namespace

TEST_CASE("forwarded named variadic arguments cannot conflict with a fixed parameter",
          "[feature:wiring][feature:variadic-wiring][series:tsb][schema:generic][definition:function]") {
    const auto bundle_source = node([](Out<TSB<forwarded_named_bundle>>) {});
    const auto leaf_source = node([](Out<TS<i64>>) {});
    const auto receiver = graph("name"_n = "receiver", [](wp<TS<i64>, "fixed"_n> fixed,
                                                               wp<TSB<SCHEMA>, var_kwargs>) { return fixed; });
    const auto sender = graph("name"_n = "sender", [&](wp<TSB<SCHEMA>, "values"_n, var_kwargs> values) {
        return receiver(leaf_source(), values);
    });

    CHECK_THROWS_WITH(testing::test_wiring([&] { sender("values"_n = bundle_source()); }),
                      Catch::Matchers::ContainsSubstring("a variadic argument name conflicts with a fixed parameter") &&
                          Catch::Matchers::ContainsSubstring("while callback sender at sender[1]"));
}

TEST_CASE("a forwarded variadic pack retains its capture mode",
          "[feature:wiring][feature:variadic-wiring][series:tsb][schema:generic][definition:function]") {
    const auto receiver = graph("name"_n = "receiver", [](wp<TSB<SCHEMA>, var_kwargs>) {});
    const auto sender = graph("name"_n = "sender", [&](wp<TSB<SCHEMA>, var_args> values) {
        receiver(values);
    });

    CHECK_THROWS_AS(testing::test_wiring([&] { sender(1); }), WiringError);
}

TEST_CASE("an item selected from one pack remains an ordinary argument for a single receiver pack",
          "[feature:wiring][feature:variadic-wiring][series:tsb][schema:generic][definition:function]") {
    const auto positional_receiver = graph(
        "name"_n = "positional_receiver",
        [](wp<TSB<SCHEMA>, var_args> values) { CHECK(values.size() == 2); });
    const auto named_receiver = graph(
        "name"_n = "named_receiver",
        [](wp<TSB<SCHEMA>, var_kwargs> values) { CHECK(values.size() == 2); });
    const auto from_named = graph(
        "name"_n = "from_named",
        [&](wp<TSB<SCHEMA>, var_kwargs> values) { positional_receiver(values[0], 2); });
    const auto from_positional = graph(
        "name"_n = "from_positional",
        [&](wp<TSB<SCHEMA>, var_args> values) { named_receiver(values[0], "other"_n = 2); });

    testing::test_wiring([&] {
        from_named("value"_n = 1);
        from_positional(1);
    });
}

TEST_CASE("aggregate graph and node declarations support variadic parameters",
          "[feature:wiring][feature:variadic-wiring][series:tsb][schema:generic][definition:aggregate]") {
    const auto graph_template = graph<AggregateVariadicGraph>();
    const auto node_template = node<AggregateVariadicNode>();

    testing::test_wiring([&] {
        (void)graph_template(1, 2);
        (void)node_template(1, 2);
    });
}

TEST_CASE("callable and aggregate definitions support positional and named packs together",
          "[feature:wiring][feature:variadic-wiring][series:tsb][schema:generic]"
          "[definition:function][definition:aggregate]") {
    const auto callable_graph = graph(
        "name"_n = "callable_graph",
        [](wp<TSB<ARGS>, var_args> positional, wp<TSB<KWARGS>, var_kwargs> named) {
            CHECK(positional.size() == 2);
            CHECK(named.size() == 2);
        });
    const auto aggregate_graph = graph<AggregateDualVariadicGraph>();
    const auto callable_node = node(
        [](In<TSB<ARGS>, var_args>, In<TSB<KWARGS>, var_kwargs>) {});
    const auto aggregate_node = node<AggregateDualVariadicNode>();

    testing::test_wiring([&] {
        callable_graph(1, 2, "left"_n = 3, "right"_n = 4);
        aggregate_graph(1, 2, "left"_n = 3, "right"_n = 4);
        callable_node(1, 2, "left"_n = 3, "right"_n = 4);
        aggregate_node(1, 2, "left"_n = 3, "right"_n = 4);
    });
}

TEST_CASE("two variadic packs materialize independently for callable and aggregate nodes",
          "[feature:wiring][feature:variadic-wiring][series:tsb][schema:generic]"
          "[definition:function][definition:aggregate][representation:free]") {
    const auto callable = node(
        [](In<TSB<ARGS>, var_args> positional, In<TSB<KWARGS>, var_kwargs> named,
           Out<TS<std::int64_t>> output) {
            auto left = std::int64_t{};
            positional[0].visit([&](const In<TS<SCALAR>> *value) {
                left = scalar_cast<std::int64_t>(value->value_view()).value();
            }, ddv::noop);
            named[0].visit(
                [&](const In<TS<SCALAR>> *value) {
                    output.set(left + scalar_cast<std::int64_t>(value->value_view()).value());
                },
                ddv::noop);
        });
    const auto aggregate = node<AggregateDualVariadicValue>();
    const auto left = std::array<std::int64_t, 2>{1, 2};
    const auto right = std::array<std::int64_t, 2>{3, 4};

    const auto callable_result = testing::test_graph(
        [&](wp<TS<std::int64_t>> first, wp<TS<std::int64_t>> second) {
            return callable(first, "right"_n = second);
        },
        left, right);
    const auto aggregate_result = testing::test_graph(
        [&](wp<TS<std::int64_t>> first, wp<TS<std::int64_t>> second) {
            return aggregate(first, "right"_n = second);
        },
        left, right);

    CHECK(callable_result.equals({4, 6}));
    CHECK(aggregate_result.equals({4, 6}));
}

TEST_CASE("positional and named packs may both be empty",
          "[feature:wiring][feature:variadic-wiring][series:tsb][schema:generic][definition:function]") {
    const auto tested = graph(
        "name"_n = "empty",
        [](wp<TSB<ARGS>, var_args> positional, wp<TSB<KWARGS>, var_kwargs> named) {
            CHECK(positional.size() == 0);
            CHECK(named.size() == 0);
        });

    testing::test_wiring([&] { tested(); });
}

TEST_CASE("two variadic packs forward independently",
          "[feature:wiring][feature:variadic-wiring][series:tsb][schema:generic][definition:function]") {
    auto calls = 0;
    const auto receiver = graph(
        "name"_n = "receiver",
        [&](wp<TSB<ARGS>, var_args> positional, wp<TSB<KWARGS>, var_kwargs> named) {
            ++calls;
            CHECK(positional.size() == 2);
            CHECK(named.size() == 2);
        });
    const auto forward_both = graph(
        "name"_n = "forward_both",
        [&](wp<TSB<ARGS>, var_args> positional, wp<TSB<KWARGS>, var_kwargs> named) {
            receiver(positional, named);
        });
    const auto forward_positional = graph(
        "name"_n = "forward_positional",
        [&](wp<TSB<SCHEMA>, var_args> positional) {
            receiver(positional, "left"_n = 3, "right"_n = 4);
        });
    const auto forward_named = graph(
        "name"_n = "forward_named",
        [&](wp<TSB<SCHEMA>, var_kwargs> named) { receiver(1, 2, named); });

    testing::test_wiring([&] {
        forward_both(1, 2, "left"_n = 3, "right"_n = 4);
        forward_positional(1, 2);
        forward_named("left"_n = 3, "right"_n = 4);
    });

    CHECK(calls == 3);
}

TEST_CASE("a forwarded pack rejects additional arguments only in the same pack",
          "[feature:wiring][feature:variadic-wiring][series:tsb][schema:generic][definition:function]") {
    const auto receiver = graph(
        "name"_n = "receiver",
        [](wp<TSB<ARGS>, var_args>, wp<TSB<KWARGS>, var_kwargs>) {});
    const auto invalid_positional = graph(
        "name"_n = "invalid_positional",
        [&](wp<TSB<SCHEMA>, var_args> positional) { receiver(positional, 2); });
    const auto invalid_named = graph(
        "name"_n = "invalid_named",
        [&](wp<TSB<SCHEMA>, var_kwargs> named) { receiver("extra"_n = 1, named); });

    CHECK_THROWS_WITH(testing::test_wiring([&] { invalid_positional(1); }),
                      Catch::Matchers::ContainsSubstring(
                          "a forwarded variadic wiring record cannot have additional arguments in its pack"));
    CHECK_THROWS_WITH(testing::test_wiring([&] { invalid_named("value"_n = 1); }),
                      Catch::Matchers::ContainsSubstring(
                          "a forwarded variadic wiring record cannot have additional arguments in its pack"));
}

TEST_CASE("a positional argument cannot follow a named argument",
          "[feature:wiring][feature:variadic-wiring][series:tsb][schema:generic][definition:function]") {
    const auto tested = graph(
        "name"_n = "ordered",
        [](wp<TSB<ARGS>, var_args>, wp<TSB<KWARGS>, var_kwargs>) {});

    CHECK_THROWS_WITH(testing::test_wiring([&] { tested("named"_n = 1, 2); }),
                      "a positional argument cannot follow a named argument");
}

TEST_CASE("variadic parameter order is validated",
          "[feature:wiring][feature:variadic-wiring][series:tsb][schema:generic][definition:function]") {
    CHECK_THROWS_WITH(
        graph("name"_n = "reversed",
              [](wp<TSB<SCHEMA>, var_kwargs>, wp<TSB<SCHEMA>, var_args>) {}),
        "var_args cannot follow var_kwargs");
    CHECK_THROWS_WITH(
        graph("name"_n = "duplicate_positional",
              [](wp<TSB<SCHEMA>, var_args>, wp<TSB<SCHEMA>, var_args>) {}),
        "a signature cannot have more than one var_args parameter");
    CHECK_THROWS_WITH(
        graph("name"_n = "duplicate_named",
              [](wp<TSB<SCHEMA>, var_kwargs>, wp<TSB<SCHEMA>, var_kwargs>) {}),
        "a signature cannot have more than one var_kwargs parameter");
    CHECK_THROWS_WITH(
        graph("name"_n = "fixed_last", [](wp<TSB<SCHEMA>, var_args>, int) {}),
        "a fixed parameter cannot follow a variadic parameter");
}

TEST_CASE("requirements and resolvers see fixed, positional, then named arguments",
          "[feature:wiring][feature:variadic-wiring]"
          "[series:tsb][schema:generic][definition:function]") {
    auto requirement_calls = 0;
    auto resolver_calls = 0;
    const auto tested = node(
        eval = [](int, In<TSB<ARGS>, var_args>, In<TSB<KWARGS>, var_kwargs>,
                  Out<TS<VARIADIC_RESULT>>) {},
        resolve<VARIADIC_RESULT> = [&](const WiringArguments &arguments) -> const ScalarType & {
            ++resolver_calls;
            REQUIRE(arguments.size() == 4);
            CHECK(arguments.argument_name(1).text() == "1");
            CHECK(arguments.argument_name(2).text() == "2");
            CHECK(arguments.argument_name(3).text() == "named");
            return int64_type();
        },
        require = [&](const WiringArguments &arguments) {
            ++requirement_calls;
            REQUIRE(arguments.size() == 4);
            CHECK(scalar_cast<std::int64_t>(*arguments.scalar(0)).value() == 0);
            CHECK(scalar_cast<std::int64_t>(*arguments.scalar(1)).value() == 1);
            CHECK(scalar_cast<std::int64_t>(*arguments.scalar(2)).value() == 2);
            CHECK(scalar_cast<std::int64_t>(*arguments.scalar("named")).value() == 3);
            return accept;
        });

    testing::test_wiring([&] { (void)tested(0, 1, 2, "named"_n = 3); });

    CHECK(requirement_calls == 1);
    CHECK(resolver_calls == 1);
}

TEST_CASE("an output runtime hook sees flattened named variadic arguments",
          "[feature:wiring][feature:variadic-wiring][series:tsb][schema:generic][definition:function]") {
    auto selector_calls = 0;
    const auto tested = node(
        eval = [](int, In<TSB<ARGS>, var_args>, In<TSB<KWARGS>, var_kwargs>, Out<TS<VARIADIC_RESULT>>) {},
        resolve<VARIADIC_RESULT> = [](const WiringArguments &) -> const ScalarType & { return int64_type(); },
        output_runtime_type = [&](const WiringArguments &arguments) -> const TimeSeriesRuntimeType & {
            ++selector_calls;
            REQUIRE(arguments.size() == 4);
            CHECK(arguments.argument_name(1).text() == "1");
            CHECK(arguments.argument_name(2).text() == "2");
            CHECK(arguments.argument_name(3).text() == "named");
            CHECK(scalar_cast<std::int64_t>(*arguments.scalar(0)).value() == 0);
            CHECK(scalar_cast<std::int64_t>(*arguments.scalar(1)).value() == 1);
            CHECK(scalar_cast<std::int64_t>(*arguments.scalar(2)).value() == 2);
            CHECK(scalar_cast<std::int64_t>(*arguments.scalar(3)).value() == 3);
            return cpp_time_series_runtime_type<TS<i64>>(*arguments.resolved_output_type());
        });

    testing::test_wiring([&] { (void)tested(0, 1, 2, "named"_n = 3); });

    CHECK(selector_calls == 1);
}

TEST_CASE("a fixed parameter cannot follow a variadic parameter",
          "[feature:wiring][feature:variadic-wiring][definition:function][series:tsb][schema:generic]") {
    const auto graph_function = [](wp<TSB<SCHEMA>, var_args>, int) {};
    const auto node_function = [](In<TSB<SCHEMA>, var_args>, int) {};

    CHECK_THROWS_AS(graph("name"_n = "invalid", graph_function), std::invalid_argument);
    CHECK_THROWS_AS(node(node_function), std::invalid_argument);
}

TEST_CASE("a variadic call requires every fixed argument",
          "[feature:wiring][feature:variadic-wiring][series:tsb][schema:generic][definition:function]") {
    const auto tested = graph("name"_n = "fixed", [](int, wp<TSB<SCHEMA>, var_args>) {});

    CHECK_THROWS_AS(testing::test_wiring([&] { tested(); }), WiringError);
}

TEST_CASE("wiring-template span invocation does not mutate variadic arguments",
          "[feature:wiring][feature:variadic-wiring][series:tsb][schema:generic][definition:function]") {
    const auto tested = graph("name"_n = "variadic", [](wp<TSB<SCHEMA>, var_args>) {});
    const auto arguments = std::array<WiringArgument, 2>{WiringArgument{1}, WiringArgument{2}};

    testing::test_wiring([&] {
        (void)static_cast<const WiringTemplate &>(tested)(std::span<const WiringArgument>{arguments});
    });

    CHECK(arguments[0].is_scalar());
    CHECK(arguments[1].is_scalar());
    CHECK_FALSE(arguments[0].name());
    CHECK_FALSE(arguments[1].name());
}

TEST_CASE("a rejected direct variadic call retains no captured scalar",
          "[feature:wiring][feature:variadic-wiring][series:tsb][schema:generic][scalar:opaque]"
          "[definition:aggregate]") {
    const auto receiver = node<RejectingVariadicNode>();
    const auto value = ScalarValue<Opaque<std::shared_ptr<int>>>{std::make_shared<int>(7)};
    const auto baseline = value.view().value().use_count();
    auto wiring = GraphWiring{};

    auto context = WiringContext{wiring};
    CHECK_THROWS_AS(receiver(value), WiringError);
    CHECK(value.view().value().use_count() == baseline);
}

TEST_CASE("a nested graph reconstructs a positional variadic parameter",
          "[feature:wiring][feature:variadic-wiring][feature:nested-graph][series:tsb][schema:generic]"
          "[definition:function][representation:free][shape:nested]") {
    const auto source = node([](Out<TS<i64>>) {});
    const auto run_source = node([](Out<TS<bool>>) {});
    const auto sink = node([](In<TSB<SCHEMA>, var_args>) {});
    const auto inner = graph(
        "name"_n = "inner", [&](wp<TS<i64>> input, wp<TSB<SCHEMA>, var_args> values) {
            REQUIRE(values.size() == 2);
            CHECK(values[0].is_scalar());
            CHECK(values[1].is_scalar());
            sink(values);
            return input;
        });
    const auto outer = graph("name"_n = "outer",
                             [&](wp<TS<i64>> input, wp<TS<bool>> run, wp<TSB<SCHEMA>, var_args> values) {
        return nested(inner, run, input, values);
    });

    testing::test_wiring([&] { (void)outer(source(), run_source(), 1, 2); });
}

TEST_CASE("a nested graph reconstructs a forwarded named variadic parameter",
          "[feature:wiring][feature:variadic-wiring][feature:nested-graph][series:tsb][schema:generic]"
          "[definition:function][shape:nested]") {
    const auto source = node([](Out<TS<i64>>) {});
    const auto run_source = node([](Out<TS<bool>>) {});
    const auto inner = graph(
        "name"_n = "inner", [&](wp<TS<i64>> input, wp<TSB<SCHEMA>, var_kwargs> values) {
            REQUIRE(values.size() == 2);
            CHECK(values["first"].is_scalar());
            CHECK(values["second"].is_scalar());
            return input;
        });
    const auto outer = graph("name"_n = "outer",
                             [&](wp<TS<i64>> input, wp<TS<bool>> run, wp<TSB<SCHEMA>, var_kwargs> values) {
        return nested(inner, run, input, values);
    });

    testing::test_wiring([&] {
        (void)outer(source(), run_source(), "first"_n = 1, "second"_n = 2);
    });
}

TEST_CASE("a nested graph preserves positional and named packs together",
          "[feature:wiring][feature:variadic-wiring][feature:nested-graph][series:tsb][schema:generic]"
          "[definition:function][representation:free][shape:nested]") {
    const auto source = node([](Out<TS<i64>>) {});
    const auto run_source = node([](Out<TS<bool>>) {});
    const auto inner = graph(
        "name"_n = "inner",
        [](wp<TS<i64>> input, wp<TSB<ARGS>, var_args> positional,
           wp<TSB<KWARGS>, var_kwargs> named) {
            REQUIRE(positional.size() == 2);
            REQUIRE(named.size() == 2);
            CHECK(positional[0].is_scalar());
            CHECK(positional[1].is_time_series());
            CHECK(named["scalar"].is_scalar());
            CHECK(named["series"].is_time_series());
            return input;
        });
    const auto outer = graph(
        "name"_n = "outer",
        [&](wp<TS<i64>> input, wp<TS<bool>> run, wp<TSB<ARGS>, var_args> positional,
            wp<TSB<KWARGS>, var_kwargs> named) {
            return nested(inner, run, input, positional, named);
        });

    testing::test_wiring([&] {
        (void)outer(source(), run_source(), 1, source(), "scalar"_n = 2, "series"_n = source());
    });
}

TEST_CASE("a positional variadic graph omits absent scalars without sparse field names",
          "[feature:wiring][feature:variadic-wiring][series:tsb][schema:generic][scalar:integer]"
          "[definition:function]") {
    const auto tested = graph("name"_n = "variadic", [&](wp<TSB<SCHEMA>, var_args> values) {
        REQUIRE(values.size() == 1);
        CHECK(values[0].name()->text() == "0");
        CHECK(scalar_cast<std::int64_t>(values[0].scalar()).value() == 42);
    });

    testing::test_wiring([&] { tested(std::optional<int>{}, 42); });
}

TEST_CASE("an operator selects a fixed overload before a variadic overload",
          "[feature:wiring][feature:variadic-wiring][feature:operator][series:tsb][schema:generic]"
          "[definition:function]") {
    auto selected = std::string_view{};
    auto operation = operator_<auto(TS_T) -> TS_T>{"choose", {"value"}};
    operation.overload(graph("name"_n = "fixed", [&](wp<TS<i64>> value) {
        selected = "fixed";
        return value;
    }));
    operation.overload(graph("name"_n = "variadic", [&](wp<TSB<SCHEMA>, var_args> values) {
        selected = "variadic";
        return operators::const_(values[0]).as<TS<std::int64_t, TimeSeriesGuarantee::const_>>();
    }));
    const auto source = node([](Out<TS<i64>>) {});

    testing::test_wiring([&] { (void)operation(source()); });
    CHECK(selected == "fixed");

    testing::test_wiring([&] { (void)operation(1, 2); });
    CHECK(selected == "variadic");
}

TEST_CASE("a variadic operator prefers the longer compatible fixed prefix",
          "[feature:wiring][feature:variadic-wiring][feature:operator][series:tsb][schema:generic]"
          "[definition:function]") {
    auto selected = std::string_view{};
    auto operation = operator_<auto(TS_T, TS_T) -> TS_T>{"choose", {"first", "second"}};
    operation.overload(graph("name"_n = "short", [&](wp<TS_T> first, wp<TSB<SCHEMA>, var_args>) {
        selected = "short";
        return first;
    }));
    operation.overload(
        graph("name"_n = "long", [&](wp<TS_T> first, wp<TS_T>, wp<TSB<SCHEMA>, var_args>) {
            selected = "long";
            return first;
        }));
    const auto source = node([](Out<TS<i64>>) {});

    testing::test_wiring([&] { (void)operation(source(), source()); });

    CHECK(selected == "long");
}

TEST_CASE("a variadic operator compares reference affinity on fixed parameters",
          "[feature:wiring][feature:variadic-wiring][feature:operator][series:tsb][schema:generic]"
          "[binding:value-to-reference][binding:reference-to-reference][definition:function]") {
    auto selected = std::string_view{};
    auto operation = operator_<auto(TS_T, TS_T) -> TS_T>{"choose", {"first", "second"}};
    operation.overload(graph("name"_n = "value", [&](wp<TS<i64>> first, wp<TSB<SCHEMA>, var_args>) {
        selected = "value";
        return first;
    }));
    operation.overload(
        graph("name"_n = "reference", [&](wp<REF<TS<i64>>> first, wp<TSB<SCHEMA>, var_args>) {
            selected = "reference";
            return first;
        }));
    const auto reference = node([](Out<REF<TS<i64>>>) {});
    const auto value = node([](Out<TS<i64>>) {});

    testing::test_wiring([&] { (void)operation(reference(), value()); });

    CHECK(selected == "reference");
}

TEST_CASE("equally specific variadic overloads are ambiguous",
          "[feature:wiring][feature:variadic-wiring][feature:operator][series:tsb][schema:generic]"
          "[definition:function]") {
    auto operation = operator_<auto(TS_T) -> TS_T>{"choose", {"value"}};
    operation.overload(graph("name"_n = "left", [](wp<TSB<SCHEMA>, var_args> values) {
        return operators::const_(values[0]);
    }));
    operation.overload(graph("name"_n = "right", [](wp<TSB<SCHEMA>, var_args> values) {
        return operators::const_(values[0]);
    }));

    testing::test_wiring([&] { CHECK_THROWS_AS(operation(1), WiringError); });
}

TEST_CASE("variadic prefix length and fixed-parameter specificity can be unordered",
          "[feature:wiring][feature:variadic-wiring][feature:operator][series:tsb][schema:generic]"
          "[definition:function]") {
    auto operation = operator_<auto(TS_T, TS_T) -> TS_T>{"choose", {"first", "second"}};
    operation.overload(
        graph("name"_n = "specific", [](wp<TS<i64>> first, wp<TSB<SCHEMA>, var_args>) { return first; }));
    operation.overload(
        graph("name"_n = "long", [](wp<TS_T> first, wp<TS_T>, wp<TSB<SCHEMA>, var_args>) { return first; }));
    const auto source = node([](Out<TS<i64>>) {});

    testing::test_wiring([&] { CHECK_THROWS_AS(operation(source(), source()), WiringError); });
}

TEST_CASE("a variadic operator cache distinguishes named pack schemas",
          "[feature:wiring][feature:variadic-wiring][feature:operator][series:tsb][schema:generic]"
          "[definition:aggregate]") {
    CachedNamedVariadicNode::requirement_calls = 0;
    CachedNamedVariadicNode::resolver_calls = 0;

    auto operation = operator_<auto(TS_T) -> TS<std::int64_t>>{"named", {"arguments"}};
    operation.overload(node<CachedNamedVariadicNode>());

    testing::test_wiring([&] {
        (void)operation("value"_n = 1);
        (void)operation("value"_n = 2);
        (void)operation("other"_n = 3);
    });

    CHECK(CachedNamedVariadicNode::requirement_calls == 2);
    CHECK(CachedNamedVariadicNode::resolver_calls == 2);
}

TEST_CASE("an operator caches requirements over both variadic packs",
          "[feature:wiring][feature:variadic-wiring][feature:operator]"
          "[series:tsb][schema:generic][definition:function]") {
    auto requirement_calls = 0;
    const auto implementation = node(
        eval = [](In<TSB<ARGS>, var_args>, In<TSB<KWARGS>, var_kwargs>,
                  Out<TS<std::int64_t>>) {},
        require = [&](const WiringArguments &arguments) {
            ++requirement_calls;
            (void)arguments.scalar("named");
            return accept;
        });
    auto operation = operator_<auto(TS_T) -> TS<std::int64_t>>{"mixed", {"arguments"}};
    operation.overload(implementation);

    testing::test_wiring([&] {
        (void)operation(1, "named"_n = 2);
        (void)operation(1, "named"_n = 2);
        (void)operation(1, "named"_n = 3);
    });

    CHECK(requirement_calls == 2);
}

TEST_CASE("a variadic operator cache distinguishes forwarded named pack schemas",
          "[feature:wiring][feature:variadic-wiring][feature:operator][series:tsb][schema:generic]"
          "[definition:aggregate]") {
    CachedNamedVariadicNode::requirement_calls = 0;
    CachedNamedVariadicNode::resolver_calls = 0;

    auto operation = operator_<auto(TS_T) -> TS<std::int64_t>>{"named", {"arguments"}};
    operation.overload(node<CachedNamedVariadicNode>());
    const auto sender = graph(
        "name"_n = "sender", [&](wp<TSB<SCHEMA>, var_kwargs> values) { (void)operation(values); });

    testing::test_wiring([&] {
        sender("value"_n = 1);
        sender("value"_n = 2);
        sender("other"_n = 3);
    });

    CHECK(CachedNamedVariadicNode::requirement_calls == 2);
    CHECK(CachedNamedVariadicNode::resolver_calls == 2);
}

TEST_CASE("an operator cache includes scalar reads from rejected fixed overloads",
          "[feature:wiring][feature:variadic-wiring][feature:operator][series:tsb][schema:generic]"
          "[definition:aggregate]") {
    ZeroModeNode::requirement_calls = 0;
    VariadicModeFallbackNode::requirement_calls = 0;

    auto operation = operator_<auto(std::int64_t) -> TS<std::int64_t>>{"mode", {"mode"}};
    operation.overload(node<ZeroModeNode>());
    operation.overload(node<VariadicModeFallbackNode>());

    testing::test_wiring([&] {
        (void)operation(1);
        (void)operation(1);
        (void)operation(0);
    });

    CHECK(ZeroModeNode::requirement_calls == 2);
    CHECK(VariadicModeFallbackNode::requirement_calls == 1);
}

TEST_CASE("a variadic operator cache includes scalar reads from rejected overloads",
          "[feature:wiring][feature:variadic-wiring][feature:operator][series:tsb][schema:generic]"
          "[definition:aggregate]") {
    ScalarDependentRejectingVariadicNode::requirement_calls = 0;
    VariadicModeFallbackNode::requirement_calls = 0;

    auto operation = operator_<auto(std::int64_t) -> TS<std::int64_t>>{"mode", {"mode"}};
    operation.overload(node<ScalarDependentRejectingVariadicNode>());
    operation.overload(node<VariadicModeFallbackNode>());

    testing::test_wiring([&] {
        (void)operation(1);
        (void)operation(1);
        (void)operation(2);
    });

    CHECK(ScalarDependentRejectingVariadicNode::requirement_calls == 2);
    CHECK(VariadicModeFallbackNode::requirement_calls == 2);
}

TEST_CASE("a forwarded variadic pack cache rechecks flattened scalar reads",
          "[feature:wiring][feature:variadic-wiring][feature:operator][series:tsb][schema:generic]"
          "[definition:aggregate]") {
    ScalarDependentRejectingVariadicNode::requirement_calls = 0;
    VariadicModeFallbackNode::requirement_calls = 0;

    auto operation = operator_<auto(std::int64_t) -> TS<std::int64_t>>{"mode", {"mode"}};
    operation.overload(node<ScalarDependentRejectingVariadicNode>());
    operation.overload(node<VariadicModeFallbackNode>());
    const auto sender = graph(
        "name"_n = "sender", [&](wp<TSB<SCHEMA>, var_args> values) { (void)operation(values); });

    testing::test_wiring([&] {
        sender(1);
        sender(1);
        sender(2);
    });

    CHECK(ScalarDependentRejectingVariadicNode::requirement_calls == 2);
    CHECK(VariadicModeFallbackNode::requirement_calls == 2);
}

TEST_CASE("a variadic operator cache recreates each candidate's absent arguments",
          "[feature:wiring][feature:variadic-wiring][feature:operator][series:tsb][schema:generic]"
          "[definition:aggregate]") {
    ScalarDependentRejectingVariadicNode::requirement_calls = 0;
    OptionalFixedVariadicNode::requirement_calls = 0;

    auto operation = operator_<auto(std::int64_t) -> TS<std::int64_t>>{"mode", {"mode"}};
    operation.overload(node<ScalarDependentRejectingVariadicNode>());
    operation.overload(node<OptionalFixedVariadicNode>());

    testing::test_wiring([&] {
        (void)operation(std::optional<int>{}, 1);
        (void)operation(std::optional<int>{}, 1);
        (void)operation(std::optional<int>{}, 2);
    });

    CHECK(ScalarDependentRejectingVariadicNode::requirement_calls == 2);
    CHECK(OptionalFixedVariadicNode::requirement_calls == 2);
}

TEST_CASE("a materialized pack keeps scalar and time series fields",
          "[feature:wiring][feature:variadic-wiring][series:tsb][schema:generic][scalar:integer]"
          "[definition:aggregate][representation:free]") {
    const auto materialize = node<MaterializedVariadicValue>();
    const auto packed = graph(
        "name"_n = "materialize", [&](wp<TSB<SCHEMA>, var_args> values) -> wp<TS<std::int64_t>> {
            const auto &type = static_cast<const TimeSeriesBundleType &>(values.type());
            REQUIRE(type.item_count() == 2);
            CHECK(&type.item_type(0) == &TS<std::int64_t, TimeSeriesGuarantee::const_>::type());
            CHECK(&type.item_type(1) == &TS<std::int64_t>::type());

            return materialize(values).as<TS<std::int64_t>>();
        });
    const auto tested = [&](wp<TS<std::int64_t>> input) { return packed(7, input); };

    const auto actual = testing::test_graph(tested, std::array<std::int64_t, 3>{1, 2, 3});
    CHECK(actual.equals({8, 9, 10}));
}

TEST_CASE("equivalent variadic node calls reuse materialization and the receiver",
          "[feature:wiring][feature:variadic-wiring][series:tsb][schema:generic][scalar:integer]"
          "[definition:aggregate][representation:free]") {
    const auto source = node([](Out<TS<std::int64_t>>) {});
    const auto receiver = node<MaterializedVariadicValue>();
    auto wiring = GraphWiring{};

    auto context = WiringContext{wiring};
    const auto input = source();
    (void)receiver(7, input);
    (void)wiring.complete_pending_wiring();
    const auto node_count = wiring.nodes().size();
    const auto output_count = wiring.outputs().size();
    REQUIRE(node_count == 4);
    REQUIRE(output_count == 4);

    (void)receiver(7, input);
    (void)wiring.complete_pending_wiring();

    CHECK(wiring.nodes().size() == node_count);
    CHECK(wiring.outputs().size() == output_count);
}

TEST_CASE("an empty materialized pack is valid at graph start",
          "[feature:wiring][feature:variadic-wiring][feature:scheduling][series:tsb][schema:generic]"
          "[transition:start][definition:aggregate][representation:free]") {
    const auto inspect = node<EmptyVariadicValue>();
    const auto packed = graph("name"_n = "empty", [&](wp<TSB<SCHEMA>, var_args> values) {
        return inspect(values).as<TS<bool>>();
    });
    const auto tested = [&] { return packed(); };

    const auto actual = testing::test_graph(tested);

    CHECK(actual.equals({true}));
}

TEST_CASE("a nested graph preserves an empty variadic parameter",
          "[feature:wiring][feature:variadic-wiring][feature:nested-graph][feature:scheduling][series:tsb]"
          "[schema:generic][transition:start][definition:aggregate][representation:free][shape:nested]") {
    const auto inspect = node<EmptyVariadicValue>();
    const auto inner = graph("name"_n = "empty", [&](wp<TSB<SCHEMA>, var_args> values) {
        return inspect(values).as<TS<bool>>();
    });
    const auto tested = [&](wp<TS<bool>> run) { return nested(inner, run); };

    const auto actual = testing::test_graph(tested, std::array{true});

    CHECK(actual.equals({true}));
}

TEST_CASE("a nested graph keeps scalar pack entries inside and crosses time series entries by reference",
          "[feature:wiring][feature:variadic-wiring][feature:nested-graph][series:tsb][schema:generic]"
          "[binding:value-to-reference][binding:reference-to-value][definition:aggregate][representation:free]"
          "[shape:nested]") {
    const auto materialize = node<MaterializedVariadicValue>();
    const auto inner = graph("name"_n = "inner", [&](wp<TSB<SCHEMA>, var_args> values) {
        return materialize(values).as<TS<std::int64_t>>();
    });
    const auto tested = [&](wp<TS<std::int64_t>> input, wp<TS<bool>> run) {
        return nested(inner, run, 7, input);
    };

    const auto actual = testing::test_graph(tested, std::array<std::int64_t, 3>{1, 2, 3},
                                            std::array{true, true, true});

    CHECK(actual.equals({8, 9, 10}));

    auto wiring = GraphWiring{};
    const auto value_source = node([](Out<TS<std::int64_t>>) {});
    const auto run_source = node([](Out<TS<bool>>) {});
    {
        auto context = WiringContext{wiring};
        (void)nested(inner, run_source(), 7, value_source());
    }

    (void)wiring.complete_pending_wiring();
    const auto &nested_node = wiring.nodes().back();
    REQUIRE(nested_node.signature().parameters.size() == 2);
    CHECK(nested_node.scalar_values().empty());
}

TEST_CASE("a nested variadic scalar is owned only by the inner graph",
          "[feature:wiring][feature:variadic-wiring][feature:nested-graph][series:tsb][schema:generic]"
          "[scalar:opaque][definition:function][shape:nested]") {
    const auto inner = graph("name"_n = "inner", [](wp<TS<i64>> input, wp<TSB<SCHEMA>, var_args>) {
        return input;
    });
    const auto value = ScalarValue<Opaque<std::shared_ptr<int>>>{std::make_shared<int>(7)};
    const auto baseline = value.view().value().use_count();
    auto wiring = GraphWiring{};

    {
        auto context = WiringContext{wiring};
        const auto source = node([](Out<TS<i64>>) {});
        const auto run = node([](Out<TS<bool>>) {});
        (void)nested(inner, run(), source(), value);
    }

    CHECK(value.view().value().use_count() == baseline + 1);
}

TEST_CASE("a variadic graph preserves fixed parameters and time series entries",
          "[feature:wiring][feature:variadic-wiring][series:tsb][schema:generic][definition:function]") {
    const auto source = node([](Out<TS<std::int64_t>>) {});
    const auto tested = graph("name"_n = "fixed_then_variadic",
                              [&](int fixed, wp<TSB<SCHEMA>, var_args> values) {
                                  CHECK(fixed == 3);
                                  REQUIRE(values.size() == 2);
                                  CHECK(values[0].is_time_series());
                                  CHECK(&values[0].type() == &TS<std::int64_t>::type());
                                  CHECK(&values[0].time_series().type() == &TS<std::int64_t>::type());
                                  CHECK(values[1].is_scalar());
                              });

    testing::test_wiring([&] { tested(3, source(), 4); });
}

TEST_CASE("nested expands scalar and time-series arguments from TSB wiring records",
          "[feature:wiring][feature:variadic-wiring][feature:nested-graph][series:tsb][schema:named]"
          "[definition:function][representation:free][shape:nested]") {
    const auto inner = graph(
        "name"_n = "inner",
        [](i64 fixed, wp<TS<i64>, "value"_n> value, wp<TSB<SCHEMA>, var_args> extras) {
            CHECK(fixed == 7);
            CHECK(TS<i64>::type().accepts(value.type()));
            REQUIRE(extras.size() == 2);
            CHECK(extras[0].is_scalar());
            CHECK(scalar_cast<i64>(extras[0].scalar()).value() == 23);
            CHECK(extras[1].is_time_series());
            CHECK(&extras[1].type() == &TS<i64>::type());
            return value;
        });
    const auto run_source = node([](Out<TS<bool>>) {});
    const auto series_source = node([](Out<TS<i64>>) {});

    testing::test_wiring([&] {
        auto named_fields = std::vector<WiringArgument>{};
        named_fields.emplace_back(util::symbol{"0"}, i64{7});
        named_fields.emplace_back(util::symbol{"value"}, series_source());
        const auto named = bundle(std::span<const WiringArgument>{named_fields});

        auto positional_fields = std::vector<WiringArgument>{};
        positional_fields.emplace_back(util::symbol{"scalar"}, i64{23});
        positional_fields.emplace_back(util::symbol{"series"}, series_source());
        const auto positional = bundle(std::span<const WiringArgument>{positional_fields});

        (void)nested(inner, run_source(), kwargs(named), args(positional));
    });
}

TEST_CASE("free TSB bundle syntax binds identically to a named variadic parameter",
          "[feature:wiring][feature:variadic-wiring][series:tsb][schema:named][definition:function]") {
    const auto source = node([](Out<TS<i64>>) {});
    const auto receiver = graph("name"_n = "receiver", [](wp<TSB<SCHEMA>, "values"_n, var_kwargs> values) {
        REQUIRE(values.size() == 1);
        REQUIRE(values[0].name());
        CHECK(values[0].name()->text() == "field");
        CHECK(values[0].is_time_series());
        CHECK(&values[0].type() == &TS<i64>::type());
    });

    testing::test_wiring([&] {
        auto fields = std::vector<WiringArgument>{};
        fields.emplace_back(util::symbol{"field"}, source());
        receiver("values"_n = bundle(std::span<const WiringArgument>{fields}));
    });
    testing::test_wiring([&] { receiver("values"_n = bundle("field"_n = source())); });
}

TEST_CASE("kwargs preserves optional qualifiers from a TSB wiring record",
          "[feature:wiring][feature:variadic-wiring][validity:optional][series:tsb][schema:named]") {
    const auto source = node([](Out<TS<i64>>) {});
    const auto optional_receiver =
        graph("name"_n = "optional_receiver", [](wp<TS<i64>, "value"_n, optional>) {});
    const auto required_receiver = graph("name"_n = "required_receiver", [](wp<TS<i64>, "value"_n>) {});

    const auto pack = [&] {
        auto fields = std::vector<WiringArgument>{};
        fields.emplace_back(util::symbol{"value"}, opt(source()));
        return bundle(std::span<const WiringArgument>{fields});
    };

    testing::test_wiring([&] { optional_receiver(kwargs(pack())); });
    CHECK_THROWS_AS(testing::test_wiring([&] { required_receiver(kwargs(pack())); }), TypeResolutionError);
}
