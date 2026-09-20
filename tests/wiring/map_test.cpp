#include <bg/graph.hpp>
#include <bg/graph/graph_argument.hpp>
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

namespace {

using namespace bg;
using namespace bg::literals;

class PatternMapOverload final : public WiringTemplate {
  public:
    template <typename Pattern>
    PatternMapOverload(util::symbol name, const Pattern &pattern,
                       const TimeSeriesType &output_type, std::size_t forwarded_argument,
                       int &requirement_calls)
        : graph_type_{std::make_shared<GraphArgumentType>(
              std::make_shared<const Pattern>(pattern))},
          signature_{make_signature(name, *graph_type_, output_type)},
          forwarded_argument_{forwarded_argument}, requirement_calls_{&requirement_calls} {}

    const Signature &signature() const final { return signature_; }

    RequirementResult requirement(const WiringArguments &) const final {
        ++*requirement_calls_;
        return accept;
    }

  private:
    static Signature make_signature(util::symbol name, const GraphArgumentType &graph_type,
                                    const TimeSeriesType &output_type) {
        return {
            name,
            {{util::symbol{"function"}, &graph_type},
             {util::symbol{"integers"}, &TSD<std::int64_t, TS<int>>::type()},
             {util::symbol{"floats"}, &TSD<std::int64_t, TS<float>>::type()}},
            output_type,
        };
    }

    std::optional<WiringPoint> wire(GraphWiring &, ResolvedSignature, const TypeMap &,
                                    std::span<const WiringArgument> arguments) const final {
        return arguments[forwarded_argument_].time_series();
    }

    std::shared_ptr<const GraphArgumentType> graph_type_;
    Signature signature_;
    std::size_t forwarded_argument_;
    int *requirement_calls_;
};

TEST_CASE("map overloads distinguish graph structure and reuse alpha-equivalent structure",
          "[feature:map][feature:operator][feature:graph-argument][feature:nested-graph]"
          "[feature:wiring][series:tsd]") {
    const auto integers = node([](Out<TSD<std::int64_t, TS<int>>>) {});
    const auto floats = node([](Out<TSD<std::int64_t, TS<float>>>) {});
    const auto add_values = node([](In<TS<int>>, In<TS<float>>, Out<TS<int>>) {});
    const auto multiply_values = node([](In<TS<int>>, In<TS<float>>, Out<TS<int>>) {});

    const auto add = graph(
        "name"_n = "add",
        [add_values](wp<TS<int>> left, wp<TS<float>> right) {
            return add_values(left, right);
        });
    const auto renamed_add = graph(
        "name"_n = "renamed_add",
        [add_values](wp<TS<int>> x, wp<TS<float>> y) {
            return add_values(x, y);
        });
    const auto multiply = graph(
        "name"_n = "multiply",
        [multiply_values](wp<TS<int>> left, wp<TS<float>> right) {
            return multiply_values(left, right);
        });

    auto add_requirements = 0;
    auto multiply_requirements = 0;
    auto operation = MapOperator{};
    operation.overload(PatternMapOverload{
        util::symbol{"add_map"}, add, TSD<std::int64_t, TS<int>>::type(), 1,
        add_requirements});
    operation.overload(PatternMapOverload{
        util::symbol{"multiply_map"}, multiply, TSD<std::int64_t, TS<float>>::type(), 2,
        multiply_requirements});

    testing::test_wiring([&] {
        const auto integer_values = integers();
        const auto float_values = floats();

        CHECK(&operation(add, integer_values, float_values).type() ==
              &TSD<std::int64_t, TS<int>>::type());
        CHECK(&operation(renamed_add, integer_values, float_values).type() ==
              &TSD<std::int64_t, TS<int>>::type());
        CHECK(&operation(multiply, integer_values, float_values).type() ==
              &TSD<std::int64_t, TS<float>>::type());
    });

    CHECK(add_requirements == 1);
    CHECK(multiply_requirements == 1);
}

TEST_CASE("map accepts a cacheable capturing graph function",
          "[feature:map][feature:operator][feature:graph-argument][feature:nested-graph]"
          "[feature:wiring][series:tsd][definition:function]") {
    const auto values = node([](Out<TSD<std::int64_t, TS<int>>>) {});
    const auto scale_value = node([](In<TS<int>>, int, Out<TS<int>>) {});
    const auto scale = 3;
    const auto function = [scale_value, scale](auto value) {
        return scale_value(value, scale);
    };

    testing::test_wiring([&] {
        const auto first = map(function, values());
        const auto second = map(function, values());
        const auto &expected = TSD<
            std::int64_t,
            REF<TS<int>, TimeSeriesGuarantee::deduplicated>>::type();

        CHECK(&first.type() == &expected);
        CHECK(&second.type() == &expected);
    });
}

TEST_CASE("map preserves the mapped item guarantee",
          "[feature:map][feature:operator][feature:type-system][feature:wiring][series:tsd]"
          "[guarantee:final]") {
    const auto values = node([](Out<TSD<std::int64_t, TS<int, TimeSeriesGuarantee::final>>>) {});

    testing::test_wiring([&] {
        const auto result = map([](auto value) { return value; }, values());
        const auto &expected = TSD<
            std::int64_t,
            REF<TS<int, TimeSeriesGuarantee::final>,
                TimeSeriesGuarantee::deduplicated>>::type();

        CHECK(&result.type() == &expected);
    });
}

TEST_CASE("map resolves an untyped wiring point from its semantic type",
          "[feature:map][feature:operator][feature:wiring][series:tsd][series:tss]"
          "[declaration:runtime][guarantee:const]") {
    const auto values = node([](Out<TSD<std::int64_t, TS<int>>>) {});
    const auto keys = node([](Out<TSS<std::int64_t>>) {});
    const auto identity = graph(
        "name"_n = "identity",
        [](wp<TS<int>> value) { return value; });
    const auto key_source = graph(
        "name"_n = "key_source",
        [](wp<TS<std::int64_t, TimeSeriesGuarantee::const_>> key) { return key; });

    testing::test_wiring([&] {
        const auto values_result = map(identity, wp<>{values()});
        const auto &expected_values = TSD<
            std::int64_t,
            REF<TS<int>, TimeSeriesGuarantee::deduplicated>>::type();
        CHECK(&values_result.type() == &expected_values);

        const auto keys_result = map(key_source, wp<>{keys()}, "__key_arg__"_n = 0);
        const auto &expected_keys = TSD<
            std::int64_t,
            REF<TS<std::int64_t, TimeSeriesGuarantee::const_>,
                TimeSeriesGuarantee::deduplicated>>::type();
        CHECK(&keys_result.type() == &expected_keys);
    });
}

TEST_CASE("a map graph function requires typed collection arguments",
          "[feature:map][feature:wiring][series:tsd][declaration:runtime][definition:function]") {
    const auto values = node([](Out<TSD<std::int64_t, TS<int>>>) {});

    testing::test_wiring([&] {
        CHECK_THROWS_WITH(
            map([](auto value) { return value; }, wp<>{values()}),
            "a map graph function requires typed TSD or TSS arguments; use a graph template with wp<>");
    });
}

TEST_CASE("map reports invalid selectors and incompatible key types",
          "[feature:map][feature:operator][feature:wiring][series:tsd]") {
    const auto integers = node([](Out<TSD<std::int64_t, TS<int>>>) {});
    const auto floating_point_keys = node([](Out<TSD<float, TS<int>>>) {});
    const auto identity = graph(
        "name"_n = "identity",
        [](wp<TS<int>> value) { return value; });

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
            (void)map([](auto left, auto right) { return left; }, integers(),
                      floating_point_keys());
        }),
        "all map TSD and TSS arguments must have the same key type");
}

TEST_CASE("a map graph function requires an explicit selector when leading key use is ambiguous",
          "[feature:map][feature:operator][feature:wiring][series:tsd][guarantee:const]") {
    const auto values = node([](Out<TSD<std::int64_t, TS<int>>>) {});
    const auto function = [](auto first, auto...) { return first; };

    testing::test_wiring([&] {
        CHECK_THROWS_WITH(
            map(function, values()),
            "a map graph function accepts arguments with and without a leading key; provide __key_arg__");

        const auto result = map(function, values(), "__key_arg__"_n = 0);
        const auto &expected = TSD<
            std::int64_t,
            REF<TS<std::int64_t, TimeSeriesGuarantee::const_>,
                TimeSeriesGuarantee::deduplicated>>::type();

        CHECK(&result.type() == &expected);
    });
}

} // namespace
