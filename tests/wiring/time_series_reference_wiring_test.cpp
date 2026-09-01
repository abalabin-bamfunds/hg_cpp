#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/node.hpp>

#include <catch2/catch_test_macros.hpp>

namespace {

struct reference_wiring_source {
    bg::Out<bg::TS<int>> output;
    void evaluate() const {}
};

struct reference_wiring_pass {
    bg::In<bg::REF<bg::TS<int>>> input;
    bg::Out<bg::REF<bg::TS<int>>> output;
    void evaluate() const { output.set(input); }
};

} // namespace

TEST_CASE("wiring inserts and reuses a reference-making node", "[wiring][reference]") {
    const auto source = bg::node<reference_wiring_source>();
    const auto pass = bg::node<reference_wiring_pass>();
    auto wiring = bg::GraphWiring{};

    {
        const auto context = bg::WiringContext{wiring};
        const auto value = source();
        (void)pass(value);
        (void)pass(value);
    }

    REQUIRE(wiring.nodes().size() == 4);
    CHECK(wiring.nodes()[2].input_sources().front() == wiring.nodes()[3].input_sources().front());
    CHECK(wiring.nodes()[1].signature().name == bg::util::symbol{"reference"});
}

TEST_CASE("wiring preserves an existing reference", "[wiring][reference]") {
    const auto source = bg::node<reference_wiring_source>();
    const auto pass = bg::node<reference_wiring_pass>();
    auto wiring = bg::GraphWiring{};

    {
        const auto context = bg::WiringContext{wiring};
        const auto reference = pass(source());
        (void)pass(reference);

        REQUIRE(wiring.nodes().size() == 4);
        const auto input = wiring.nodes().back().input_sources().front();
        const bg::WiringPoint reference_point = reference;
        CHECK(wiring.wiring_points()[input].node_index == reference_point.node_index());
    }
}
