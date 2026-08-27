#include <bg/graph.hpp>
#include <bg/node.hpp>
#include <bg/testing/test_graph.hpp>

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <vector>

namespace {

using namespace bg::literals;

struct Add {
    bg::In<bg::TS<float>> lhs;
    bg::In<bg::TS<float>> rhs;
    bg::Out<bg::TS<float>> output;

    void evaluate() const { output.set(lhs.value() + rhs.value()); }
};

inline const auto add = bg::node<Add>();
inline const auto add_graph = bg::graph(
    "name"_m = "add_graph",
    [](bg::wp<bg::TS<float>> lhs, bg::wp<bg::TS<float>> rhs) -> bg::wp<bg::TS<float>> { return add(lhs, rhs); });

} // namespace

TEST_CASE("test_graph evaluates a node over input ticks", "[testing][evaluation]") {
    const auto lhs = std::vector{1.0F, 2.0F, 3.0F};
    const auto rhs = std::vector{10.0F, 20.0F, 30.0F};

    const auto actual = bg::testing::test_graph(add, lhs, rhs);

    REQUIRE(actual.size() == 3);
    CHECK(bg::scalar_cast<float>(actual[0])->value() == 11.0F);
    CHECK(bg::scalar_cast<float>(actual[1])->value() == 22.0F);
    CHECK(bg::scalar_cast<float>(actual[2])->value() == 33.0F);
}

TEST_CASE("test_graph supports skipped input ticks", "[testing][evaluation]") {
    const auto lhs = std::vector<std::optional<float>>{1.0F, std::nullopt, 3.0F};
    const auto rhs = std::vector{10.0F, 20.0F, 30.0F};

    const auto actual = bg::testing::test_graph(add_graph, lhs, rhs);

    REQUIRE(actual.size() == 3);
    CHECK(bg::scalar_cast<float>(actual[0])->value() == 11.0F);
    CHECK(bg::scalar_cast<float>(actual[1])->value() == 21.0F);
    CHECK(bg::scalar_cast<float>(actual[2])->value() == 33.0F);
}
