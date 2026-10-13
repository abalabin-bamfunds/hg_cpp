#include <bg/graph.hpp>
#include <bg/graph/compiled_graph.hpp>
#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/node.hpp>
#include <bg/testing/test_graph.hpp>
#include <bg/type/scalar_type.hpp>
#include <bg/type/time_series_list_type.hpp>
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

    void evaluate() const { output.set(*input * factor); }
};

const auto source = bg::node<source_definition>();
const auto scale = bg::node<scale_definition>(
    [](bg::WiringPoint, float factor) { return std::string{"node-factor="} + std::to_string(factor); });

const auto scale_graph = bg::graph(
    "name"_n = "scale_graph",
    "label"_n = [](bg::wp<bg::TS<float>>, float factor) { return std::string{"factor="} + std::to_string(factor); },
    [](bg::wp<bg::TS<float>> input, float factor) -> bg::wp<bg::TS<float>> { return scale(input, factor); });

static_assert(std::same_as<decltype(scale_graph(std::declval<bg::WiringPoint>(), 2.0F)), bg::wp<bg::TS<float>>>);

} // namespace

TEST_CASE("a lambda graph function supplies its signature", "[graph_template][feature:wiring][definition:function]") {
    const auto &time_series = bg::TS<float>::type();
    const bg::ResolvedSignature resolved =
        scale_graph.signature().resolve(std::array<const bg::Type *, 2>{&time_series, &bg::float_type()});

    CHECK(resolved.name.text() == "scale_graph");
    REQUIRE(resolved.parameters.size() == 2);
    CHECK(resolved.parameters[0].name.text() == "0");
    CHECK(resolved.parameters[1].name.text() == "1");
    CHECK(resolved.output == &time_series);
}

TEST_CASE("a graph template expands into the active graph", "[graph_template][feature:wiring]") {
    bg::GraphWiring wiring;

    {
        bg::WiringContext context{wiring};
        const bg::WiringPoint input = source();
        (void)scale_graph(input, 2.0F);
    }

    (void)wiring.complete_pending_wiring();
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

TEST_CASE("a graph template can omit its output", "[graph_template][feature:wiring]") {
    const auto scale_sink_graph = bg::graph(
        "name"_n = "scale_sink_graph", [](bg::wp<bg::TS<float>> input, float factor) { (void)scale(input, factor); });
    bg::GraphWiring wiring;

    {
        bg::WiringContext context{wiring};
        scale_sink_graph(source(), 2.0F);
    }

    (void)wiring.complete_pending_wiring();
    const auto nodes = wiring.nodes();
    REQUIRE(nodes.size() == 2);
    REQUIRE(nodes[1].path().parent != nullptr);
    CHECK(nodes[1].path().parent->name.text() == "scale_sink_graph");
}

TEST_CASE("callable output guarantee inference is local to its node signature",
          "[graph_template][feature:wiring][feature:node-api][definition:function][series:value]") {
    const auto increment = bg::node([](double value) { return value + 1.0; });
    const auto inferred_output_graph = bg::graph("name"_n = "inferred_callable_output",
                                                 [=](bg::wp<bg::TS<double>> input) { return increment(input); });
    const auto forwarding_graph = bg::graph("name"_n = "forward_inferred_callable_output",
                                            [=](bg::wp<bg::TS<double>> input) { return inferred_output_graph(input); });

    const auto inputs = std::array{1.0, 2.0, 3.0};
    CHECK(bg::testing::test_graph(inferred_output_graph, inputs).equals<double>({2.0, 3.0, 4.0}));
    CHECK(bg::testing::test_graph(forwarding_graph, inputs).equals<double>({2.0, 3.0, 4.0}));
}

TEST_CASE("a graph reference output defaults to deduplicated",
          "[graph_template][reference][feature:wiring][definition:function][guarantee:deduplicated]") {
    const auto pass =
        bg::graph("name"_n = "reference_pass",
                  [](bg::wp<bg::REF<bg::TS<bg::i64>>> input) -> bg::wp<bg::REF<bg::TS<bg::i64>>> { return input; });

    CHECK(pass.signature().output()->guarantee().value() == bg::TimeSeriesGuarantee::deduplicated);
    CHECK_FALSE(static_cast<const bg::TimeSeriesType &>(*pass.signature().parameter(0).type).guarantee().is_resolved());
}

TEST_CASE("graph output re-annotation is checked at the cast", "[graph_template][feature:wiring]") {
    const auto invalid =
        bg::graph("name"_n = "invalid_output",
                  [](bg::wp<bg::TS<float>> input) -> bg::wp<bg::TS<double>> { return bg::wp<bg::TS<double>>{input}; });
    auto wiring = bg::GraphWiring{};
    const auto context = bg::WiringContext{wiring};

    CHECK_THROWS_AS(invalid(source()), bg::WiringError);
}

TEST_CASE("a graph signature resolves a scalar variable declaration", "[graph_template][feature:wiring]") {
    using GRAPH_SCALAR = bg::ScalarVariable<"GRAPH_SCALAR">;

    const auto generic_identity_graph =
        bg::graph("name"_n = "generic_identity_graph",
                  [](bg::wp<bg::TS<GRAPH_SCALAR>> input) -> bg::wp<bg::TS<GRAPH_SCALAR>> { return input; });
    const auto &time_series = bg::TS<double>::type();
    const std::array<const bg::Type *, 1> arguments{&time_series};

    const auto signature = generic_identity_graph.signature().resolve(arguments);

    CHECK(signature.parameters[0].type == &time_series);
    CHECK(signature.output == &time_series);
}

TEST_CASE("a graph signature resolves a time series variable declaration",
          "[graph_template][type_system][feature:wiring]") {
    using T = bg::TimeSeriesVariable<"T">;
    constexpr auto G = bg::guarantee_variable<"G">;
    const auto identity = bg::graph("name"_n = "generic_time_series_identity",
                                    [](bg::wp<T::with<G>> input) -> bg::wp<T::with<G>> { return input; });
    const auto &list =
        bg::TSL<bg::TS<double, bg::TimeSeriesGuarantee::ordinary>, 2, bg::TimeSeriesGuarantee::final>::type();
    const auto arguments = std::array<const bg::Type *, 1>{&list};

    const auto signature = identity.signature().resolve(arguments);

    CHECK(signature.parameters[0].type == &list);
    CHECK(signature.output == &list);
}

namespace {

struct struct_scale_graph {
    bg::wp<bg::TS<float>> input;
    float factor;

    [[nodiscard]] bg::wp<bg::TS<float>> wire() const { return scale(input, factor); }
};

} // namespace

TEST_CASE("a struct graph definition supplies its signature", "[graph_template][feature:wiring]") {
    const auto reflected_scale_graph = bg::graph<struct_scale_graph>();
    const auto &signature = reflected_scale_graph.signature();
    const auto &time_series = bg::TS<float>::type();
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

    (void)wiring.complete_pending_wiring();
    REQUIRE(wiring.nodes().size() == 2);
    REQUIRE(wiring.nodes()[1].path().parent != nullptr);
    CHECK(wiring.nodes()[1].path().parent->name.text() == "struct_scale_graph");
}

TEST_CASE("node and graph templates share the wiring interface", "[graph_template][feature:wiring]") {
    bg::GraphWiring wiring;

    {
        bg::WiringContext context{wiring};
        const bg::WiringPoint input = source();
        const bg::WiringTemplate &node_template = scale;
        const bg::WiringTemplate &graph_template = scale_graph;

        REQUIRE(node_template(input, 2.0F));
        REQUIRE(graph_template(input, 3.0F));
    }

    (void)wiring.complete_pending_wiring();
    CHECK(wiring.nodes().size() == 3);
}

TEST_CASE("deduplicated graph calls retain their first wiring path", "[graph_template][feature:wiring]") {
    bg::GraphWiring wiring;

    {
        bg::WiringContext context{wiring};
        const bg::WiringPoint input = source();
        (void)scale_graph(input, 2.0F);
        (void)scale_graph(input, 2.0F);
    }

    (void)wiring.complete_pending_wiring();
    const auto nodes = wiring.nodes();
    REQUIRE(nodes.size() == 2);
    REQUIRE(nodes[1].path().parent != nullptr);
    CHECK(nodes[1].path().parent->label.text() == "factor=2.000000");
}

TEST_CASE("a compiled graph owns its wiring paths", "[graph_template][feature:wiring]") {
    std::unique_ptr<bg::CompiledGraph> compiled;

    {
        bg::GraphWiring wiring;
        bg::WiringContext context{wiring};
        const bg::WiringPoint input = source();
        (void)scale_graph(input, 2.0F);

        wiring.finalize();
        compiled = std::make_unique<bg::CompiledGraph>(wiring);
    }

    const bg::WiringPath &path = compiled->wiring_path(1);
    REQUIRE(path.parent != nullptr);
    CHECK(path.name.text() == "scale_definition");
    CHECK(path.parent->name.text() == "scale_graph");
    CHECK(path.parent->label.text() == "factor=2.000000");
}
