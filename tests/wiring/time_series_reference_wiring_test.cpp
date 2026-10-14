#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/node.hpp>
#include <bg/type/time_series_reference_type.hpp>

#include <catch2/catch_test_macros.hpp>

namespace {

struct reference_wiring_source {
    bg::Out<bg::TS<bg::i64>> output;
    void evaluate() const {}
};

struct reference_wiring_pass {
    bg::In<bg::REF<bg::TS<bg::i64>>> input;
    bg::Out<bg::REF<bg::TS<bg::i64>>> output;
    void evaluate() const { output.copy(input); }
};

} // namespace

TEST_CASE("wiring inserts and reuses a reference-making node",
          "[wiring][reference][feature:wiring][binding:value-to-reference]") {
    const auto source = bg::node<reference_wiring_source>();
    const auto pass = bg::node<reference_wiring_pass>();
    auto wiring = bg::GraphWiring{};

    {
        const auto context = bg::WiringContext{wiring};
        const auto value = source();
        (void)pass(value);
        (void)pass(value);
        wiring.finalize();
    }

    REQUIRE(wiring.nodes().size() == 3);
    CHECK(wiring.nodes()[2].inputs().front().index() == 1);
    CHECK(wiring.nodes()[1].signature().name == bg::util::symbol{"reference"});
    const auto &reference = dynamic_cast<const bg::TimeSeriesReferenceType &>(*wiring.nodes()[1].signature().output);
    CHECK(reference.guarantee().value() == bg::TimeSeriesGuarantee::const_);
}

TEST_CASE("wiring preserves an existing reference",
          "[wiring][reference][feature:wiring][binding:reference-to-reference]") {
    const auto source = bg::node<reference_wiring_source>();
    const auto pass = bg::node<reference_wiring_pass>();
    auto wiring = bg::GraphWiring{};

    {
        const auto context = bg::WiringContext{wiring};
        const auto reference = pass(source());
        (void)pass(reference);
        wiring.finalize();

        REQUIRE(wiring.nodes().size() == 4);
        const auto input = wiring.nodes().back().inputs().front().index();
        CHECK(wiring.outputs()[input].node_index == 2);
    }
}

TEST_CASE("wiring rejects materializing a borrowed input as a reference",
          "[wiring][reference][feature:borrowed-input]") {
    const auto pass = bg::node<reference_wiring_pass>();
    auto wiring = bg::GraphWiring{};
    const auto context = bg::WiringContext{wiring};
    const auto input = wiring.borrowed_input(bg::TS<bg::i64>::type());

    (void)pass(input);
    CHECK_THROWS_AS(wiring.finalize(), bg::WiringError);
    CHECK(wiring.nodes().empty());
}

TEST_CASE("wiring rejects reference-typed borrowed input slots",
          "[wiring][reference][feature:borrowed-input]") {
    auto wiring = bg::GraphWiring{};

    CHECK_THROWS_AS(wiring.borrowed_input(bg::REF<bg::TS<bg::i64>>::type()), bg::WiringError);
}
