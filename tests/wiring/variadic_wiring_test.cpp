#include <bg/graph.hpp>
#include <bg/nested.hpp>
#include <bg/node.hpp>
#include <bg/operator.hpp>
#include <bg/operators.hpp>
#include <bg/testing/test_graph.hpp>
#include <bg/testing/test_wiring.hpp>
#include <bg/time_series/tsb/tsb.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>

namespace {

using namespace bg;
using namespace bg::literals;

using VARIADIC_RESULT = ScalarVariable<"VARIADIC_RESULT">;

struct AggregateVariadicGraph {
    wp<TSB<SCHEMA>, var_args> values;

    void wire() const { REQUIRE(values.size() == 2); }
};

struct AggregateVariadicNode {
    In<TSB<SCHEMA>, var_args> values;

    void evaluate() const {}
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
        return int_type();
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
                      "a forwarded variadic wiring record cannot be named");
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

TEST_CASE("a forwarded variadic pack retains its capture mode",
          "[feature:wiring][feature:variadic-wiring][series:tsb][schema:generic][definition:function]") {
    const auto receiver = graph("name"_n = "receiver", [](wp<TSB<SCHEMA>, var_kwargs>) {});
    const auto sender = graph("name"_n = "sender", [&](wp<TSB<SCHEMA>, var_args> values) {
        receiver(values);
    });

    CHECK_THROWS_AS(testing::test_wiring([&] { sender(1); }), WiringError);
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

TEST_CASE("a variadic parameter must be final",
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
    const auto source = node([](Out<TS<int>>) {});
    const auto run_source = node([](Out<TS<bool>>) {});
    const auto sink = node([](In<TSB<SCHEMA>, var_args>) {});
    const auto inner = graph(
        "name"_n = "inner", [&](wp<TS<int>> input, wp<TSB<SCHEMA>, var_args> values) {
            REQUIRE(values.size() == 2);
            CHECK(values[0].is_scalar());
            CHECK(values[1].is_scalar());
            sink(values);
            return input;
        });
    const auto outer = graph("name"_n = "outer",
                             [&](wp<TS<int>> input, wp<TS<bool>> run, wp<TSB<SCHEMA>, var_args> values) {
        return nested(inner, run, input, values);
    });

    testing::test_wiring([&] { (void)outer(source(), run_source(), 1, 2); });
}

TEST_CASE("a nested graph reconstructs a forwarded named variadic parameter",
          "[feature:wiring][feature:variadic-wiring][feature:nested-graph][series:tsb][schema:generic]"
          "[definition:function][shape:nested]") {
    const auto source = node([](Out<TS<int>>) {});
    const auto run_source = node([](Out<TS<bool>>) {});
    const auto inner = graph(
        "name"_n = "inner", [&](wp<TS<int>> input, wp<TSB<SCHEMA>, var_kwargs> values) {
            REQUIRE(values.size() == 2);
            CHECK(values["first"].is_scalar());
            CHECK(values["second"].is_scalar());
            return input;
        });
    const auto outer = graph("name"_n = "outer",
                             [&](wp<TS<int>> input, wp<TS<bool>> run, wp<TSB<SCHEMA>, var_kwargs> values) {
        return nested(inner, run, input, values);
    });

    testing::test_wiring([&] {
        (void)outer(source(), run_source(), "first"_n = 1, "second"_n = 2);
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
    auto selected = "";
    auto operation = operator_<auto(TS_T) -> TS_T>{"choose", {"value"}};
    operation.overload(graph("name"_n = "fixed", [&](wp<TS<int>> value) {
        selected = "fixed";
        return value;
    }));
    operation.overload(graph("name"_n = "variadic", [&](wp<TSB<SCHEMA>, var_args> values) {
        selected = "variadic";
        return operators::const_(values[0]).as<TS<std::int64_t, TimeSeriesGuarantee::const_>>();
    }));
    const auto source = node([](Out<TS<int>>) {});

    testing::test_wiring([&] { (void)operation(source()); });
    CHECK(selected == "fixed");

    testing::test_wiring([&] { (void)operation(1, 2); });
    CHECK(selected == "variadic");
}

TEST_CASE("a variadic operator prefers the longer compatible fixed prefix",
          "[feature:wiring][feature:variadic-wiring][feature:operator][series:tsb][schema:generic]"
          "[definition:function]") {
    auto selected = "";
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
    const auto source = node([](Out<TS<int>>) {});

    testing::test_wiring([&] { (void)operation(source(), source()); });

    CHECK(selected == "long");
}

TEST_CASE("a variadic operator compares reference affinity on fixed parameters",
          "[feature:wiring][feature:variadic-wiring][feature:operator][series:tsb][schema:generic]"
          "[binding:value-to-reference][binding:reference-to-reference][definition:function]") {
    auto selected = "";
    auto operation = operator_<auto(TS_T, TS_T) -> TS_T>{"choose", {"first", "second"}};
    operation.overload(graph("name"_n = "value", [&](wp<TS<int>> first, wp<TSB<SCHEMA>, var_args>) {
        selected = "value";
        return first;
    }));
    operation.overload(
        graph("name"_n = "reference", [&](wp<REF<TS<int>>> first, wp<TSB<SCHEMA>, var_args>) {
            selected = "reference";
            return first;
        }));
    const auto reference = node([](Out<REF<TS<int>>>) {});
    const auto value = node([](Out<TS<int>>) {});

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
        graph("name"_n = "specific", [](wp<TS<int>> first, wp<TSB<SCHEMA>, var_args>) { return first; }));
    operation.overload(
        graph("name"_n = "long", [](wp<TS_T> first, wp<TS_T>, wp<TSB<SCHEMA>, var_args>) { return first; }));
    const auto source = node([](Out<TS<int>>) {});

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
    const auto node_count = wiring.nodes().size();
    const auto output_count = wiring.outputs().size();
    REQUIRE(node_count == 4);
    REQUIRE(output_count == 4);

    (void)receiver(7, input);

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

    const auto &nested_node = wiring.nodes().back();
    REQUIRE(nested_node.signature().parameters.size() == 2);
    CHECK(nested_node.scalar_values().empty());
}

TEST_CASE("a nested variadic scalar is owned only by the inner graph",
          "[feature:wiring][feature:variadic-wiring][feature:nested-graph][series:tsb][schema:generic]"
          "[scalar:opaque][definition:function][shape:nested]") {
    const auto inner = graph("name"_n = "inner", [](wp<TS<int>> input, wp<TSB<SCHEMA>, var_args>) {
        return input;
    });
    const auto value = ScalarValue<Opaque<std::shared_ptr<int>>>{std::make_shared<int>(7)};
    const auto baseline = value.view().value().use_count();
    auto wiring = GraphWiring{};

    {
        auto context = WiringContext{wiring};
        const auto source = node([](Out<TS<int>>) {});
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
