#include <bg/graph.hpp>
#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/node.hpp>
#include <bg/operators.hpp>
#include <bg/switch.hpp>
#include <bg/testing/test_graph.hpp>
#include <bg/testing/test_wiring.hpp>
#include <bg/time_series/tsd/tsd_type.hpp>
#include <bg/time_series/tsb/tsb_type.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>

#include <cstdint>
#include <tuple>

namespace {

using namespace bg;
using namespace bg::literals;

template <template <typename, auto...> typename Field> struct switch_bundle {
    Field<TS<i64>> value;
};

const auto integer_source = node([](Out<TS<i64>>) {});

const auto float_source = node([](Out<TS<float>>) {});

const auto final_integer_source = node([](Out<TS<i64, TimeSeriesGuarantee::final>>) {});

const auto integer_transform = node([](In<TS<i64>>, Out<TS<i64>>) {});

class SwitchCacheOverload final : public WiringTemplate {
  public:
    explicit SwitchCacheOverload(int &requirement_calls)
        : signature_{util::symbol{"cached_switch"},
                     {{util::symbol{"key"}, &TS<i64>::type()},
                      {util::symbol{"cases"}, &GraphArgument::type()}},
                     TS<i64>::type()},
          requirement_calls_{&requirement_calls} {}

    const Signature &default_signature() const final { return signature_; }

    RequirementResult requirement(const WiringArguments &) const final {
        ++*requirement_calls_;
        return accept;
    }

  private:
    std::optional<WiringPoint> wire(GraphWiring &, ResolvedSignature, const WiringArguments &,
                                    std::span<const WiringArgument>) const final {
        return integer_source();
    }

    Signature signature_;
    int *requirement_calls_;
};

TEST_CASE("switch validates its case set",
          "[feature:switch][feature:wiring][feature:operator][series:value]"
          "[definition:function]") {
    const auto key = node([](Out<TS<i64>>) {});

    const auto value = node([](Out<TS<i64>>) {});

    const auto identity = graph("name"_n = "switch_identity",
                                [](wp<TS<i64>> input) { return input; });

    testing::test_wiring([&] {
        CHECK_THROWS_WITH((void)switch_(key(), cases{}, value()),
                          "switch_ requires at least one case");
        CHECK_THROWS_WITH(
            (void)switch_(key(),
                          cases{case_(i64{1}, identity),
                                case_(i64{1}, identity)},
                          value()),
            "switch_ has duplicate case keys");
        CHECK_THROWS_WITH(
            (void)switch_(key(),
                          cases{case_(DEFAULT, identity), case_(DEFAULT, identity)},
                          value()),
            "switch_ accepts one DEFAULT case");
        CHECK_THROWS_WITH((void)switch_(key(), cases{case_(true, identity)}, value()),
                          "switch case keys must match the switch key type");
        CHECK_THROWS_WITH(
            (void)switch_(key(), cases{case_(std::tuple{}, identity)}, value()),
            "a switch case key tuple cannot be empty");
        CHECK_THROWS_WITH(
            (void)switch_(key(), cases{case_(std::tuple{1, 1}, identity)}, value()),
            "switch_ has duplicate case keys");
        CHECK_THROWS_WITH(
            (void)switch_(key(),
                          cases{case_(std::tuple{1, 2}, identity), case_(2, identity)},
                          value()),
            "switch_ has duplicate case keys");
        CHECK_THROWS_WITH(
            (void)switch_(key(), cases{case_(std::tuple{1, true}, identity)}, value()),
            "switch case keys must match the switch key type");
        CHECK_THROWS_WITH(
            (void)switch_(key(), cases{case_(1, identity)}, value(), value()),
            "switch case graph does not accept every positional argument");
    });
}

TEST_CASE("switch preserves named arguments around an omitted case default",
          "[feature:switch][feature:wiring][feature:wiring-defaults][series:value]") {
    const auto combine = node([](In<TS<i64>> x, In<TS<i64>> y, In<TS<i64>> z, Out<TS<i64>> output) {
        output.set(*x + *y * 10 + *z * 100);
    });
    const auto selected = graph(
        {"x"_n, "y"_n = i64{2}, "z"_n},
        [&](wp<TS<i64>> x, wp<TS<i64>> y, wp<TS<i64>> z) { return combine(x, y, z); });
    const auto run = [&] {
        return switch_(operators::const_(i64{1}), cases{case_(i64{1}, selected)},
                       "x"_n = operators::const_(i64{3}), "z"_n = operators::const_(i64{4}));
    };

    CHECK(testing::test_graph(run).equals({423}));
}

TEST_CASE("switch runtime declarations preserve valid_items inputs",
          "[feature:switch][feature:wiring][feature:validity][series:tsd][input-guarantee:valid-items]") {
    const auto key = node([](Out<TS<i64>>) {});

    const auto source = node([](Out<TSD<i64, TS<i64>>>) {});

    const auto identity = graph(
        "name"_n = "valid_items_switch_identity",
        [](wp<TSD<i64, TS<i64>>, valid_items> input) { return input; });
    auto wiring = GraphWiring{};

    {
        auto context = WiringContext{wiring};
        (void)switch_(key(), cases{case_(i64{1}, identity)}, valid_items(source()));
    }

    (void)wiring.complete_pending_wiring();
    const auto switched = std::ranges::find_if(wiring.nodes(), [](const auto &candidate) {
        return std::ranges::any_of(candidate.signature().parameters,
                                   [](const auto &parameter) { return parameter.valid_items; });
    });
    REQUIRE(switched != wiring.nodes().end());
    CHECK(std::ranges::any_of(switched->signature().parameters,
                              [](const auto &parameter) { return parameter.valid_items; }));
}

TEST_CASE("switch rejects incompatible case output shapes",
          "[feature:switch][feature:wiring][feature:operator][series:value]"
          "[definition:function]") {
    const auto key = node([](Out<TS<i64>>) {});

    const auto integers = graph(
        "name"_n = "integer_switch_source",
        []() -> wp<TS<i64>> { return integer_source(); });

    const auto floats = graph(
        "name"_n = "float_switch_source",
        []() -> wp<TS<float>> { return float_source(); });

    const auto sink = graph("name"_n = "switch_sink", [] {});

    testing::test_wiring([&] {
        CHECK_THROWS_WITH(
            (void)switch_(key(),
                          cases{case_(i64{1}, integers),
                                case_(i64{2}, floats)}),
            "switch case output types are incompatible");
        CHECK_THROWS_WITH(
            (void)switch_(key(),
                          cases{case_(i64{1}, integers),
                                case_(i64{2}, sink)}),
            "switch_ cannot mix sink and output cases");
        CHECK_THROWS_WITH(
            (void)switch_(key(),
                          cases{case_(DEFAULT, integers),
                                case_(i64{1}, sink)}),
            "switch_ cannot mix sink and output cases");
    });
}

TEST_CASE("switch output is a deduplicated reference to the common case output",
          "[feature:switch][feature:wiring][feature:operator][feature:reference]"
          "[series:value][binding:value-to-reference][guarantee:deduplicated]"
          "[definition:function]") {
    const auto key = node([](Out<TS<i64>>) {});

    const auto value = node([](Out<TS<i64>>) {});

    const auto identity = graph("name"_n = "typed_switch_identity",
                                [](wp<TS<i64>> input) { return input; });

    testing::test_wiring([&] {
        const auto result = switch_(key(), cases{case_(i64{1}, identity)}, value());

        CHECK(&result.type() ==
              &REF<TS<i64>, TimeSeriesGuarantee::deduplicated>::type());
    });
}

TEST_CASE("switch uses the weakest compatible case output guarantee",
          "[feature:switch][feature:wiring][feature:operator][feature:type-system]"
          "[series:value][guarantee:ordinary][guarantee:final][definition:function]") {
    const auto key = node([](Out<TS<i64>>) {});

    const auto final_source = graph(
        "name"_n = "final_integer_switch_source",
        []() -> wp<TS<i64, TimeSeriesGuarantee::final>> { return final_integer_source(); });

    const auto ordinary_source = graph(
        "name"_n = "ordinary_integer_switch_source",
        []() -> wp<TS<i64>> { return integer_source(); });

    testing::test_wiring([&] {
        const auto output = switch_(
            key(), cases{case_(1, final_source), case_(2, ordinary_source)});

        CHECK(&output.type() ==
              &REF<TS<i64>, TimeSeriesGuarantee::deduplicated>::type());
    });
}

TEST_CASE("switch resolves bundles whose matching fields have different guarantees",
          "[feature:switch][feature:wiring][feature:operator][feature:type-system]"
          "[series:tsb][schema:named][guarantee:ordinary][guarantee:final][definition:function]") {
    const auto key = node([](Out<TS<i64>>) {});

    const auto final_source = node([](Out<TSB<switch_bundle, TimeSeriesGuarantee::final>>) {});

    const auto ordinary_source = node([](Out<TSB<switch_bundle>>) {});

    const auto final_graph = graph(
        "name"_n = "final_bundle_switch_source",
        [&]() -> wp<TSB<switch_bundle, TimeSeriesGuarantee::final>> { return final_source(); });

    const auto ordinary_graph = graph(
        "name"_n = "ordinary_bundle_switch_source",
        [&]() -> wp<TSB<switch_bundle>> { return ordinary_source(); });

    testing::test_wiring([&] {
        const auto output = switch_(key(), cases{case_(1, final_graph), case_(2, ordinary_graph)});
        CHECK(&output.type() == &REF<TSB<switch_bundle>, TimeSeriesGuarantee::deduplicated>::type());
    });
}

TEST_CASE("switch resolution cache includes every case graph",
          "[feature:switch][feature:wiring][feature:operator][feature:graph-argument]"
          "[series:value][definition:function]") {
    const auto key = node([](Out<TS<i64>>) {});

    const auto first = graph("name"_n = "first_switch_source",
                             [] { return integer_source(); });

    const auto second = graph("name"_n = "second_switch_source",
                              [] { return integer_transform(integer_source()); });

    auto requirement_calls = 0;
    auto operation = std::remove_cvref_t<decltype(switch_)>{
        "cached_switch", {"key", "cases", "args", "kwargs"}};
    operation.overload(SwitchCacheOverload{requirement_calls});

    testing::test_wiring([&] {
        (void)operation(key(), cases{case_(1, first), case_(2, first)});
        (void)operation(key(), cases{case_(1, first), case_(2, first)});
        (void)operation(key(), cases{case_(1, first), case_(2, second)});
    });

    CHECK(requirement_calls == 2);
}

} // namespace
