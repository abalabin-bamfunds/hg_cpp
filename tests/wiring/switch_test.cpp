#include <bg/graph.hpp>
#include <bg/node.hpp>
#include <bg/switch.hpp>
#include <bg/testing/test_wiring.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>

#include <cstdint>
#include <tuple>

namespace {

using namespace bg;
using namespace bg::literals;

const auto integer_source = node([](Out<TS<i64>>) {});
const auto float_source = node([](Out<TS<float>>) {});
const auto final_integer_source = node([](Out<TS<i64, TimeSeriesGuarantee::final>>) {});
const auto integer_transform = node([](In<TS<i64>>, Out<TS<i64>>) {});

class SwitchCacheOverload final : public WiringTemplate {
  public:
    explicit SwitchCacheOverload(int &requirement_calls)
        : signature_{util::symbol{"cached_switch"},
                     {{util::symbol{"key"}, &TS<std::int64_t>::type()},
                      {util::symbol{"cases"}, &GraphArgument::type()}},
                     TS<i64>::type()},
          requirement_calls_{&requirement_calls} {}

    const Signature &signature() const final { return signature_; }

    RequirementResult requirement(const WiringArguments &) const final {
        ++*requirement_calls_;
        return accept;
    }

  private:
    std::optional<WiringPoint> wire(GraphWiring &, ResolvedSignature, const TypeMap &,
                                    std::span<const WiringArgument>) const final {
        return integer_source();
    }

    Signature signature_;
    int *requirement_calls_;
};

TEST_CASE("switch validates its case set",
          "[feature:switch][feature:wiring][feature:operator][series:value]"
          "[definition:function]") {
    const auto key = node([](Out<TS<std::int64_t>>) {});
    const auto value = node([](Out<TS<i64>>) {});
    const auto identity = graph("name"_n = "switch_identity",
                                [](wp<TS<i64>> input) { return input; });

    testing::test_wiring([&] {
        CHECK_THROWS_WITH((void)switch_(key(), cases{}, value()),
                          "switch_ requires at least one case");
        CHECK_THROWS_WITH(
            (void)switch_(key(),
                          cases{case_(std::int64_t{1}, identity),
                                case_(std::int64_t{1}, identity)},
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

TEST_CASE("switch rejects incompatible case output shapes",
          "[feature:switch][feature:wiring][feature:operator][series:value]"
          "[definition:function]") {
    const auto key = node([](Out<TS<std::int64_t>>) {});
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
                          cases{case_(std::int64_t{1}, integers),
                                case_(std::int64_t{2}, floats)}),
            "switch case output types are incompatible");
        CHECK_THROWS_WITH(
            (void)switch_(key(),
                          cases{case_(std::int64_t{1}, integers),
                                case_(std::int64_t{2}, sink)}),
            "switch_ cannot mix sink and output cases");
        CHECK_THROWS_WITH(
            (void)switch_(key(),
                          cases{case_(DEFAULT, integers),
                                case_(std::int64_t{1}, sink)}),
            "switch_ cannot mix sink and output cases");
    });
}

TEST_CASE("switch output is a deduplicated reference to the common case output",
          "[feature:switch][feature:wiring][feature:operator][feature:reference]"
          "[series:value][binding:value-to-reference][guarantee:deduplicated]"
          "[definition:function]") {
    const auto key = node([](Out<TS<std::int64_t>>) {});
    const auto value = node([](Out<TS<i64>>) {});
    const auto identity = graph("name"_n = "typed_switch_identity",
                                [](wp<TS<i64>> input) { return input; });

    testing::test_wiring([&] {
        const auto result = switch_(key(), cases{case_(std::int64_t{1}, identity)}, value());

        CHECK(&result.type() ==
              &REF<TS<i64>, TimeSeriesGuarantee::deduplicated>::type());
    });
}

TEST_CASE("switch uses the weakest compatible case output guarantee",
          "[feature:switch][feature:wiring][feature:operator][feature:type-system]"
          "[series:value][guarantee:ordinary][guarantee:final][definition:function]") {
    const auto key = node([](Out<TS<std::int64_t>>) {});
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

TEST_CASE("switch resolution cache includes every case graph",
          "[feature:switch][feature:wiring][feature:operator][feature:graph-argument]"
          "[series:value][definition:function]") {
    const auto key = node([](Out<TS<std::int64_t>>) {});
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
