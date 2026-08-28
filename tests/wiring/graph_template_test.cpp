#include <bg/graph.hpp>
#include <bg/graph/compiled_graph.hpp>
#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/node.hpp>
#include <bg/type/scalar_type.hpp>
#include <bg/type/time_series_value_type.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <concepts>
#include <memory>
#include <string>
#include <utility>

namespace {

using namespace bg::literals;

struct source_definition {
    bg::Out<bg::TS<float>> output;

    void evaluate() const { output.set(1.0F); }
};

struct scale_definition {
    bg::In<bg::TS<float>> input;
    float factor;
    bg::Out<bg::TS<float>> output;

    void evaluate() const { output.set(input.value() * factor); }
};

const auto source = bg::node<source_definition>();
const auto scale = bg::node<scale_definition>(
    [](bg::WiringPoint, float factor) { return std::string{"node-factor="} + std::to_string(factor); });

const auto scale_graph = bg::graph(
    "name"_m = "scale_graph",
    "label"_m = [](bg::wp<bg::TS<float>>, float factor) { return std::string{"factor="} + std::to_string(factor); },
    [](bg::wp<bg::TS<float>> input, float factor) -> bg::wp<bg::TS<float>> { return scale(input, factor); });

using GraphScalar = bg::ScalarVariable<"GraphScalar">;

static_assert(std::same_as<decltype(scale_graph(std::declval<bg::WiringPoint>(), 2.0F)), bg::wp<bg::TS<float>>>);

} // namespace

TEST_CASE("a lambda graph function supplies its signature", "[graph_template]") {
    const auto &time_series = bg::time_series_value_type(bg::float_type(), bg::TimeSeriesGuarantee::ordinary);
    const bg::ResolvedSignature resolved =
        scale_graph.signature().resolve(std::array<const bg::Type *, 2>{&time_series, &bg::float_type()});

    CHECK(resolved.name.text() == "scale_graph");
    REQUIRE(resolved.parameters.size() == 2);
    CHECK(resolved.parameters[0].name.text() == "0");
    CHECK(resolved.parameters[1].name.text() == "1");
    CHECK(resolved.output == &time_series);
}

TEST_CASE("a graph template expands into the active graph", "[graph_template]") {
    bg::GraphWiring wiring;

    {
        bg::WiringContext context{wiring};
        const bg::WiringPoint input = source();
        (void)scale_graph(input, 2.0F);
    }

    const auto nodes = wiring.nodes();
    REQUIRE(nodes.size() == 2);
    CHECK(nodes[1].signature().name.text() == "scale_definition");
    CHECK(nodes[0].path().label.text() == "0");

    const bg::WiringPath &node_path = nodes[1].path();
    REQUIRE(node_path.parent != nullptr);
    CHECK(node_path.name.text() == "scale_definition");
    CHECK(node_path.label.text() == "node-factor=2.000000");
    CHECK(node_path.parent->name.text() == "scale_graph");
    CHECK(node_path.parent->label.text() == "factor=2.000000");
}

TEST_CASE("a graph template can omit its output", "[graph_template]") {
    const auto scale_sink_graph = bg::graph(
        "name"_m = "scale_sink_graph", [](bg::wp<bg::TS<float>> input, float factor) { (void)scale(input, factor); });
    bg::GraphWiring wiring;

    {
        bg::WiringContext context{wiring};
        scale_sink_graph(source(), 2.0F);
    }

    const auto nodes = wiring.nodes();
    REQUIRE(nodes.size() == 2);
    REQUIRE(nodes[1].path().parent != nullptr);
    CHECK(nodes[1].path().parent->name.text() == "scale_sink_graph");
}

TEST_CASE("a graph signature resolves a scalar variable declaration", "[graph_template]") {
    const auto generic_identity_graph =
        bg::graph("name"_m = "generic_identity_graph",
                  [](bg::wp<bg::TS<GraphScalar>> input) -> bg::wp<bg::TS<GraphScalar>> { return input; });
    const auto &time_series = bg::time_series_value_type(bg::double_type(), bg::TimeSeriesGuarantee::ordinary);
    const std::array<const bg::Type *, 1> arguments{&time_series};

    const auto signature = generic_identity_graph.signature().resolve(arguments);

    CHECK(signature.parameters[0].type == &time_series);
    CHECK(signature.output == &time_series);
}

namespace {

struct struct_scale_graph {
    bg::wp<bg::TS<float>> input;
    float factor;

    [[nodiscard]] bg::wp<bg::TS<float>> wire() const { return scale(input, factor); }
};

} // namespace

TEST_CASE("a struct graph definition supplies its signature", "[graph_template]") {
    const auto reflected_scale_graph = bg::graph<struct_scale_graph>();
    const auto &signature = reflected_scale_graph.signature();
    const auto &time_series = bg::time_series_value_type(bg::float_type(), bg::TimeSeriesGuarantee::ordinary);
    const bg::ResolvedSignature resolved =
        signature.resolve(std::array<const bg::Type *, 2>{&time_series, &bg::float_type()});

    CHECK(resolved.name.text() == "struct_scale_graph");
    REQUIRE(resolved.parameters.size() == 2);
    CHECK(resolved.parameters[0].name.text() == "input");
    CHECK(resolved.parameters[1].name.text() == "factor");

    bg::GraphWiring wiring;
    {
        bg::WiringContext context{wiring};
        (void)reflected_scale_graph(source(), 2.0F);
    }

    REQUIRE(wiring.nodes().size() == 2);
    REQUIRE(wiring.nodes()[1].path().parent != nullptr);
    CHECK(wiring.nodes()[1].path().parent->name.text() == "struct_scale_graph");
}

TEST_CASE("node and graph templates share the wiring interface", "[graph_template]") {
    bg::GraphWiring wiring;

    {
        bg::WiringContext context{wiring};
        const bg::WiringPoint input = source();
        const bg::WiringTemplate &node_template = scale;
        const bg::WiringTemplate &graph_template = scale_graph;

        REQUIRE(node_template(input, 2.0F));
        REQUIRE(graph_template(input, 3.0F));
    }

    CHECK(wiring.nodes().size() == 3);
}

TEST_CASE("custom wiring labels can repeat", "[graph_template]") {
    bg::GraphWiring wiring;

    {
        bg::WiringContext context{wiring};
        const bg::WiringPoint input = source();
        (void)scale_graph(input, 2.0F);
        (void)scale_graph(input, 2.0F);
    }

    const auto nodes = wiring.nodes();
    REQUIRE(nodes.size() == 3);
    REQUIRE(nodes[1].path().parent != nullptr);
    REQUIRE(nodes[2].path().parent != nullptr);
    CHECK(nodes[1].path().parent->label == nodes[2].path().parent->label);
}

TEST_CASE("a compiled graph owns its wiring paths", "[graph_template]") {
    std::unique_ptr<bg::CompiledGraph> compiled;

    {
        bg::GraphWiring wiring;
        bg::WiringContext context{wiring};
        const bg::WiringPoint input = source();
        (void)scale_graph(input, 2.0F);

        compiled = std::make_unique<bg::CompiledGraph>(wiring);
    }

    const bg::WiringPath &path = compiled->wiring_path(1);
    REQUIRE(path.parent != nullptr);
    CHECK(path.name.text() == "scale_definition");
    CHECK(path.parent->name.text() == "scale_graph");
    CHECK(path.parent->label.text() == "factor=2.000000");
}
