#include <bg/graph.hpp>
#include <bg/graph/graph_argument.hpp>
#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/map.hpp>
#include <bg/node.hpp>
#include <bg/testing/test_wiring.hpp>
#include <bg/time_series/tsd/tsd_type.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace {

using namespace bg;
using namespace bg::literals;

using MAP_LEFT = ScalarVariable<"MAP_LEFT">;
using MAP_RIGHT = ScalarVariable<"MAP_RIGHT">;
using RESULT = TimeSeriesVariable<"RESULT">;

inline constexpr auto first_argument_pattern = graph_pattern(
    [](auto first, auto) {
        return first;
    });

class PatternMapOverload final : public WiringTemplate {
  public:
    template <typename Pattern>
    PatternMapOverload(util::symbol name, const Pattern &pattern,
                       const TimeSeriesType &output_type, std::size_t forwarded_argument,
                       int &requirement_calls)
        : graph_constraint_{std::make_shared<GraphArgumentConstraint>(
              std::make_shared<const Pattern>(pattern))},
          signature_{make_signature(name, graph_constraint_, output_type)},
          forwarded_argument_{forwarded_argument}, requirement_calls_{&requirement_calls} {}

    const Signature &default_signature() const final { return signature_; }

    RequirementResult requirement(const WiringArguments &) const final {
        ++*requirement_calls_;
        return accept;
    }

  private:
    static Signature make_signature(util::symbol name,
                                    std::shared_ptr<const GraphArgumentConstraint> graph_constraint,
                                    const TimeSeriesType &output_type) {
        auto function = Parameter{util::symbol{"function"}, &GraphArgument::type()};
        function.graph_constraint = std::move(graph_constraint);
        return Signature{name,
                         {std::move(function),
                          {util::symbol{"integers"}, &TSD<i64, TS<i64>>::type()},
                          {util::symbol{"floats"}, &TSD<i64, TS<float>>::type()}},
                         output_type};
    }

    std::optional<WiringPoint> wire(GraphWiring &, ResolvedSignature, const WiringArguments &,
                                    std::span<const WiringArgument> arguments) const final {
        return arguments[forwarded_argument_].time_series();
    }

    std::shared_ptr<const GraphArgumentConstraint> graph_constraint_;
    Signature signature_;
    std::size_t forwarded_argument_;
    int *requirement_calls_;
};

class MapOptionOverload final : public WiringTemplate {
  public:
    template <typename Pattern>
    MapOptionOverload(const Pattern &pattern, bool &selected)
        : graph_constraint_{std::make_shared<GraphArgumentConstraint>(
              std::make_shared<const Pattern>(pattern))},
          signature_{make_signature(graph_constraint_)},
          selected_{&selected} {}

    const Signature &default_signature() const final { return signature_; }

    RequirementResult requirement(const WiringArguments &arguments) const final {
        const auto key_set_argument = arguments.scalar("__key_set_arg__");
        if (!key_set_argument || scalar_cast<i64>(*key_set_argument).value() != 0) {
            return reject("the key selector was not forwarded");
        }

        return accept;
    }

  private:
    static Signature make_signature(std::shared_ptr<const GraphArgumentConstraint> graph_constraint) {
        auto graph = Parameter{util::symbol{"graph"}, &GraphArgument::type()};
        graph.graph_constraint = std::move(graph_constraint);
        return Signature{
            util::symbol{"map_option"},
            {std::move(graph),
             {util::symbol{"values"}, &TSD<i64, TS<i64>>::type()},
             {util::symbol{"options"}, &TSB<details::MAP_NAMED>::type(), active, required, false,
              details::VariadicMode::named,
              details::variadic_wiring_operations<TSB<details::MAP_NAMED>>()}},
            TSD<i64, TS<i64>>::type()};
    }

    std::optional<WiringPoint> wire(GraphWiring &, ResolvedSignature, const WiringArguments &,
                                    std::span<const WiringArgument> arguments) const final {
        *selected_ = true;
        return arguments[1].time_series();
    }

    std::shared_ptr<const GraphArgumentConstraint> graph_constraint_;
    Signature signature_;
    bool *selected_;
};

TEST_CASE("map overloads distinguish graph structure and reuse alpha-equivalent structure",
          "[feature:map][feature:operator][feature:graph-argument][feature:nested-graph]"
          "[feature:wiring][series:tsd]") {
    const auto integers = node([](Out<TSD<i64, TS<i64>>>) {});

    const auto floats = node([](Out<TSD<i64, TS<float>>>) {});

    const auto add_values = node([](In<TS<i64>>, In<TS<float>>, Out<TS<i64>>) {});

    const auto multiply_values = node([](In<TS<i64>>, In<TS<float>>, Out<TS<i64>>) {});

    const auto add = graph(
        "name"_n = "add",
        [add_values](wp<TS<i64>> left, wp<TS<float>> right) {
            return add_values(left, right);
        });

    const auto renamed_add = graph(
        "name"_n = "renamed_add",
        [add_values](wp<TS<i64>> x, wp<TS<float>> y) {
            return add_values(x, y);
        });

    const auto multiply = graph(
        "name"_n = "multiply",
        [multiply_values](wp<TS<i64>> left, wp<TS<float>> right) {
            return multiply_values(left, right);
        });

    auto add_requirements = 0;
    auto multiply_requirements = 0;
    auto operation = std::remove_cvref_t<decltype(map)>{"map", {"graph", "args", "kwargs"}};
    operation.overload(PatternMapOverload{
        util::symbol{"add_map"}, add, TSD<i64, TS<i64>>::type(), 1,
        add_requirements});
    operation.overload(PatternMapOverload{
        util::symbol{"multiply_map"}, multiply, TSD<i64, TS<float>>::type(), 2,
        multiply_requirements});

    testing::test_wiring([&] {
        const auto integer_values = integers();
        const auto float_values = floats();

        CHECK(&operation(add, integer_values, float_values).type() ==
              &TSD<i64, TS<i64>>::type());
        CHECK(&operation(renamed_add, integer_values, float_values).type() ==
              &TSD<i64, TS<i64>>::type());
        CHECK(&operation(multiply, integer_values, float_values).type() ==
              &TSD<i64, TS<float>>::type());

        const auto runtime_arguments = std::vector<MapArgument>{
            {WiringArgument{integer_values}},
            {WiringArgument{float_values}},
        };
        const WiringTemplate &runtime_add = add;
        CHECK(&operation(runtime_add, std::span<const MapArgument>{runtime_arguments}).type() ==
              &TSD<i64, TS<i64>>::type());
    });

    CHECK(add_requirements == 1);
    CHECK(multiply_requirements == 1);
}

TEST_CASE("map runtime declarations preserve valid_items inputs",
          "[feature:map][feature:wiring][feature:validity][series:tsd][input-guarantee:valid-items]") {
    const auto identity = graph(
        "name"_n = "valid_items_map_identity",
        [](wp<TS<i64>> value, wp<TSD<i64, TS<i64>>, valid_items>) { return value; });

    const auto values = node([](Out<TSD<i64, TS<i64>>>) {});

    const auto dictionary = node([](Out<TSD<i64, TS<i64>>>) {});
    auto wiring = GraphWiring{};

    {
        auto context = WiringContext{wiring};
        (void)map(identity, values(), pass_through(valid_items(dictionary())));
    }

    (void)wiring.complete_pending_wiring();
    const auto mapped = std::ranges::find_if(wiring.nodes(), [](const auto &candidate) {
        return std::ranges::any_of(candidate.signature().parameters,
                                   [](const auto &parameter) { return parameter.valid_items; });
    });
    REQUIRE(mapped != wiring.nodes().end());
    CHECK(std::ranges::any_of(mapped->signature().parameters,
                              [](const auto &parameter) { return parameter.valid_items; }));
}

TEST_CASE("map overloads receive map options",
          "[feature:map][feature:operator][feature:graph-argument][feature:variadic-wiring]"
          "[feature:wiring][series:tsd]") {
    const auto values = node([](Out<TSD<i64, TS<i64>>>) {});

    const auto identity = graph(
        "name"_n = "identity_with_key_option",
        [](wp<TS<i64>> value) { return value; });

    auto selected = false;
    auto operation = std::remove_cvref_t<decltype(map)>{"map", {"graph", "args", "kwargs"}};
    operation.overload(MapOptionOverload{identity, selected});

    testing::test_wiring([&] {
        const auto input = values();
        const auto result = operation(identity, input, "__key_set_arg__"_n = 0);

        CHECK(selected);
        CHECK(&result.type() == &input.type());

        selected = false;
        const auto runtime_arguments = std::vector<MapArgument>{
            {WiringArgument{input}},
            {WiringArgument{util::symbol{"__key_set_arg__"}, i64{0}}},
        };
        const WiringTemplate &runtime_identity = identity;
        const auto runtime_result =
            operation(runtime_identity, std::span<const MapArgument>{runtime_arguments});

        CHECK(selected);
        CHECK(&runtime_result.type() == &input.type());
    });
}

TEST_CASE("map accepts fully runtime graph and argument values",
          "[feature:map][feature:operator][feature:graph-argument][feature:wiring]"
          "[series:tsd][definition:runtime]") {
    const auto values = node([](Out<TSD<i64, TS<i64>>>) {});

    const auto other_keys = node([](Out<TSD<bool, TS<float>>>) {});

    const auto identity = graph(
        "name"_n = "runtime_identity",
        [](wp<TS<i64>> value, wp<TSD<bool, TS<float>>>) { return value; });

    const auto sink = graph(
        "name"_n = "runtime_sink",
        [](wp<TS<i64>>, wp<TSD<bool, TS<float>>>) {});

    testing::test_wiring([&] {
        const auto arguments = std::vector<MapArgument>{
            {WiringArgument{values()}},
            {WiringArgument{other_keys()}, true},
        };
        const WiringTemplate &runtime_identity = identity;
        const WiringTemplate &runtime_sink = sink;

        const auto result = map(runtime_identity, std::span<const MapArgument>{arguments});
        const auto &expected = TSD<
            i64,
            REF<TS<i64>, TimeSeriesGuarantee::const_>>::type();

        CHECK(&result.type() == &expected);
        CHECK_FALSE(map(runtime_sink, std::span<const MapArgument>{arguments}).has_output());
    });
}

TEST_CASE("an explicit type mapping resolves a raw map lambda output",
          "[feature:map][feature:operator][feature:type-system][feature:wiring]"
          "[declaration:generic][series:tsd]") {
    const auto values = node([](Out<TSD<i64, TS<i64>>>) {});

    testing::test_wiring([&] {
        const auto result = map[type<RESULT> = type<TS<i64>>](
            [](auto value) -> wp<RESULT> { return wp<RESULT>{value}; }, values());

        CHECK(&result.type() ==
              &TSD<i64, REF<TS<i64>, TimeSeriesGuarantee::const_>>::type());
    });
}

TEST_CASE("map overloads declare graph signatures and patterns in their parameters",
          "[feature:map][feature:operator][feature:type-system][feature:graph-argument]"
          "[feature:wiring][series:tsd][definition:function]") {
    const auto integers = node([](Out<TSD<i64, TS<i64>>>) {});

    const auto floats = node([](Out<TSD<i64, TS<float>>>) {});

    const auto first = graph(
        "name"_n = "first",
        [](wp<TS<i64>> left, wp<TS<float>>) {
            return left;
        });

    auto selected = false;

    const auto implementation = graph(
        "name"_n = "first_map",
        [&selected](GRAPH<auto(TS<MAP_LEFT>, TS<MAP_RIGHT>) -> TS<MAP_LEFT>, first_argument_pattern>,
                    wp<TSD<i64, TS<i64>>> integer_values,
                    wp<TSD<i64, TS<float>>>) {
            selected = true;
            return integer_values;
        });
    auto operation = std::remove_cvref_t<decltype(map)>{"map", {"graph", "args", "kwargs"}};
    operation.overload(implementation);

    testing::test_wiring([&] {
        const auto result = operation(first, integers(), floats());

        CHECK(selected);
        CHECK(&result.type() == &TSD<i64, TS<i64>>::type());
    });
}

TEST_CASE("map accepts a cacheable capturing graph function",
          "[feature:map][feature:operator][feature:graph-argument][feature:nested-graph]"
          "[feature:wiring][series:tsd][definition:function]") {
    const auto values = node([](Out<TSD<i64, TS<i64>>>) {});

    const auto scale_value = node([](In<TS<i64>>, int, Out<TS<i64>>) {});
    const auto scale = 3;
    const auto function = [scale_value, scale](auto value) {
        return scale_value(value, scale);
    };

    testing::test_wiring([&] {
        const auto first = map(function, values());
        const auto second = map(function, values());
        const auto &expected = TSD<
            i64,
            REF<TS<i64>, TimeSeriesGuarantee::const_>>::type();

        CHECK(&first.type() == &expected);
        CHECK(&second.type() == &expected);
    });
}

TEST_CASE("map preserves the mapped item guarantee",
          "[feature:map][feature:operator][feature:type-system][feature:wiring][series:tsd]"
          "[guarantee:final]") {
    const auto values = node([](Out<TSD<i64, TS<i64, TimeSeriesGuarantee::final>>>) {});

    testing::test_wiring([&] {
        const auto result = map([](auto value) { return value; }, values());
        const auto &expected = TSD<
            i64,
            REF<TS<i64, TimeSeriesGuarantee::final>,
                TimeSeriesGuarantee::const_>>::type();

        CHECK(&result.type() == &expected);
    });
}

TEST_CASE("a sink map returns a wiring point without an output",
          "[feature:map][feature:operator][feature:wiring][series:tsd]") {
    const auto values = node([](Out<TSD<i64, TS<i64>>>) {});

    const auto sink = graph(
        "name"_n = "sink",
        [](wp<TS<i64>>) {});

    testing::test_wiring([&] {
        const auto result = map(sink, values());

        CHECK_FALSE(result.has_output());
        CHECK_THROWS_WITH(result.type(), "a wiring point has no output");
        CHECK_THROWS_WITH(result.graph(), "a wiring point has no output");
        CHECK_THROWS_WITH(result.as<TS<i64>>(), "a wiring point has no output");
    });
}

TEST_CASE("map resolves an untyped wiring point from its semantic type",
          "[feature:map][feature:operator][feature:wiring][series:tsd][series:tss]"
          "[declaration:runtime][guarantee:const]") {
    const auto values = node([](Out<TSD<i64, TS<i64>>>) {});

    const auto keys = node([](Out<TSS<i64>>) {});

    const auto identity = graph(
        "name"_n = "identity",
        [](wp<TS<i64>> value) { return value; });

    const auto key_source = graph(
        "name"_n = "key_source",
        [](wp<TS<i64, TimeSeriesGuarantee::const_>> key) { return key; });

    testing::test_wiring([&] {
        const auto values_result = map(identity, wp<>{values()});
        const auto &expected_values = TSD<
            i64,
            REF<TS<i64>, TimeSeriesGuarantee::const_>>::type();
        CHECK(&values_result.type() == &expected_values);

        const auto keys_result = map(key_source, wp<>{keys()}, "__key_arg__"_n = 0);
        const auto &expected_keys = TSD<
            i64,
            REF<TS<i64, TimeSeriesGuarantee::const_>,
                TimeSeriesGuarantee::const_>>::type();
        CHECK(&keys_result.type() == &expected_keys);
    });
}

TEST_CASE("a map graph function requires typed collection arguments",
          "[feature:map][feature:wiring][series:tsd][declaration:runtime][definition:function]") {
    const auto values = node([](Out<TSD<i64, TS<i64>>>) {});

    testing::test_wiring([&] {
        CHECK_THROWS_WITH(
            map([](auto value) { return value; }, wp<>{values()}),
            "a map graph function requires typed TSD or TSS arguments; use a graph template with wp<>");
    });
}

TEST_CASE("map reports invalid selectors and incompatible key types",
          "[feature:map][feature:operator][feature:wiring][series:tsd]") {
    const auto integers = node([](Out<TSD<i64, TS<i64>>>) {});

    const auto floating_point_keys = node([](Out<TSD<float, TS<i64>>>) {});

    const auto identity = graph(
        "name"_n = "identity",
        [](wp<TS<i64>> value) { return value; });

    CHECK_THROWS_WITH(
        testing::test_wiring([&] {
            (void)map(identity, integers(), "__key_arg__"_n = 2);
        }),
        "__key_arg__ selects a graph parameter that is out of range");
    CHECK_THROWS_WITH(
        testing::test_wiring([&] {
            (void)map([](auto value) { return value; }, integers(),
                      "__key_set_arg__"_n = 2);
        }),
        "a map argument selector index is out of range");
    CHECK_THROWS_WITH(
        testing::test_wiring([&] {
            (void)map([](auto left, auto) { return left; }, integers(),
                      floating_point_keys());
        }),
        "all map TSD and TSS arguments must have the same key type");
}

TEST_CASE("a map graph function requires an explicit selector when leading key use is ambiguous",
          "[feature:map][feature:operator][feature:wiring][series:tsd][guarantee:const]") {
    const auto values = node([](Out<TSD<i64, TS<i64>>>) {});
    const auto function = [](auto first, auto...) { return first; };

    testing::test_wiring([&] {
        CHECK_THROWS_WITH(
            map(function, values()),
            "a map graph function accepts arguments with and without a leading key; provide __key_arg__");

        const auto result = map(function, values(), "__key_arg__"_n = 0);
        const auto &expected = TSD<
            i64,
            REF<TS<i64, TimeSeriesGuarantee::const_>,
                TimeSeriesGuarantee::const_>>::type();

        CHECK(&result.type() == &expected);
    });
}

} // namespace
