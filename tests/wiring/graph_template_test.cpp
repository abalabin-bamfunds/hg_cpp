#include <bg/graph.hpp>
#include <bg/graph/compiled_graph.hpp>
#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/nested.hpp>
#include <bg/node.hpp>
#include <bg/operator.hpp>
#include <bg/testing/test_graph.hpp>
#include <bg/type/scalar_type.hpp>
#include <bg/type/time_series_list_type.hpp>
#include <bg/type/time_series_value_type.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <concepts>
#include <memory>
#include <string>
#include <string_view>
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

using NESTED_OUTPUT = bg::TimeSeriesVariable<"NESTED_OUTPUT">;

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

TEST_CASE("a graph function can receive wiring arguments without adding a graph parameter",
          "[graph_template][feature:wiring]") {
    auto observed_argument_count = std::size_t{};
    auto observed_scalar = bg::i64{};
    auto observed_output_type = static_cast<const bg::TimeSeriesType *>(nullptr);
    const auto inspect = bg::graph(
        "name"_n = "inspect_wiring_arguments",
        [&](const bg::WiringArguments &arguments, bg::i64 value,
            bg::wp<bg::TS<float>> input) -> bg::wp<bg::TS<float>> {
            observed_argument_count = arguments.size();
            observed_scalar = bg::scalar_cast<bg::i64>(*arguments.scalar(0)).value();
            observed_output_type = arguments.resolved_output_type();
            CHECK(&arguments.resolved_type(0) == &bg::cpp_scalar_type<bg::i64>());
            return input;
        });

    auto wiring = bg::GraphWiring{};
    {
        const auto context = bg::WiringContext{wiring};
        (void)inspect(42, source());
    }

    CHECK(inspect.signature().parameter_count() == 2);
    CHECK(observed_argument_count == 2);
    CHECK(observed_scalar == 42);
    CHECK(observed_output_type == &bg::TS<float>::type());
}

TEST_CASE("a graph can map its output type into a nested implementation",
          "[graph_template][feature:wiring][feature:type-system]") {
    const auto integer_source = bg::node([](bg::Out<bg::TS<bg::i64>>) {});
    const auto generic_output = bg::graph<NESTED_OUTPUT>(
        "name"_n = "generic_output",
        [=]() -> bg::wp<NESTED_OUTPUT> { return bg::wp<NESTED_OUTPUT>{integer_source()}; });
    const auto run = bg::node([](bg::Out<bg::TS<bool>>) {});
    const auto wrapper = bg::graph<NESTED_OUTPUT>(
        "name"_n = "generic_wrapper",
        [=](const bg::WiringArguments &arguments,
            bg::wp<bg::TS<bool>> run_input) -> bg::wp<bg::REF<NESTED_OUTPUT>> {
            const auto mapped_output = generic_output[arguments.type_map()];
            return bg::nested(mapped_output, run_input);
        });

    auto wiring = bg::GraphWiring{};
    const auto context = bg::WiringContext{wiring};
    const auto output = wrapper[bg::type<NESTED_OUTPUT> = bg::type<bg::TS<bg::i64>>](run());

    auto output_name = std::string{};
    output.type().format_name(output_name);
    CHECK(output_name == "REF<TS<i64>, deduplicated>");
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

TEST_CASE("callable graph requirements reject before wiring the body",
          "[graph_template][feature:wiring][feature:operator]") {
    auto requirement_calls = 0;
    auto body_calls = 0;
    const auto positive_only = bg::graph(
        "name"_n = "positive_only",
        bg::require = [&](const bg::WiringArguments &arguments) {
            ++requirement_calls;
            return arguments.resolved_output_type() == &bg::TS<float>::type() &&
                   bg::scalar_cast<bg::i64>(*arguments.scalar(0)).value() > 0;
        },
        [&](bg::i64) {
            ++body_calls;
            return source();
        });

    auto wiring = bg::GraphWiring{};
    {
        const auto context = bg::WiringContext{wiring};
        (void)positive_only(1);
        CHECK_THROWS_AS(positive_only(-1), bg::WiringError);
    }
    CHECK(requirement_calls == 2);
    CHECK(body_calls == 1);

    const auto sink = bg::graph(
        "name"_n = "requirement_sink",
        bg::require = [](const bg::WiringArguments &arguments) { return arguments.resolved_output_type() == nullptr; },
        [](bg::i64) {});
    {
        const auto context = bg::WiringContext{wiring};
        (void)sink(1);
    }

    auto operation = bg::operator_<auto(bg::SCALAR) -> bg::TS<float>>{"requirement_fallback", {"value"}};
    auto selected = std::string_view{};
    operation.overload(bg::graph(
        "name"_n = "positive",
        bg::require = [](const bg::WiringArguments &arguments) {
            return bg::scalar_cast<bg::i64>(*arguments.scalar(0)).value() > 0;
        },
        [&](bg::i64) {
            selected = "positive";
            return source();
        }));
    operation.overload(bg::graph(
        "name"_n = "fallback",
        bg::require = [](const bg::WiringArguments &arguments) {
            return bg::scalar_cast<bg::i64>(*arguments.scalar(0)).value() <= 0;
        },
        [&](bg::i64) {
            selected = "fallback";
            return source();
        }));

    {
        const auto context = bg::WiringContext{wiring};
        (void)operation(2);
        CHECK(selected == "positive");
        (void)operation(-2);
        CHECK(selected == "fallback");
    }
}

TEST_CASE("callable graph requirements return rejection diagnostics with graph labels",
          "[graph_template][feature:wiring]") {
    const auto guarded = bg::graph(
        "name"_n = "guarded",
        "label"_n = [](bg::i64 value) { return std::string{"value="} + std::to_string(value); },
        bg::require = [](const bg::WiringArguments &) { return bg::reject("value is not accepted"); },
        [](bg::i64) { return source(); });

    auto wiring = bg::GraphWiring{};
    const auto context = bg::WiringContext{wiring};
    try {
        (void)guarded(3);
        FAIL("expected graph requirement rejection");
    } catch (const bg::WiringError &error) {
        CHECK(std::string{error.what()}.contains("value is not accepted"));
    }

    const auto accepted = bg::graph(
        "name"_n = "accepted_label",
        "label"_n = [](bg::i64 value) { return std::string{"value="} + std::to_string(value); },
        bg::require = [](const bg::WiringArguments &) { return bg::accept; },
        [](bg::i64) { return source(); });
    auto accepted_wiring = bg::GraphWiring{};
    {
        const auto accepted_context = bg::WiringContext{accepted_wiring};
        (void)accepted(4);
    }
    (void)accepted_wiring.complete_pending_wiring();
    REQUIRE(accepted_wiring.nodes().size() == 1);
    REQUIRE(accepted_wiring.nodes()[0].path().parent != nullptr);
    CHECK(accepted_wiring.nodes()[0].path().parent->label.text() == "value=4");
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
