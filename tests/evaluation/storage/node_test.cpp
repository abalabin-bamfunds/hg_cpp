#include <bg/graph/compiled_graph.hpp>
#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/node.hpp>
#include <bg/graph/runtime_graph.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/node.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>

namespace {

struct runtime_node_source {
    bg::Out<bg::TS<double>> output;

    void start() const { output.set(1.5); }
    void evaluate() const {}
};

struct runtime_node_reference {
    bg::In<bg::REF<bg::TS<double>>> input;
    bg::Out<bg::REF<bg::TS<double>>> output;

    void evaluate() const { output.copy(input); }
};

struct runtime_node_sink {
    bg::In<bg::TS_T> input;

    void evaluate() const {}
};

[[nodiscard]] bg::EvaluationTime time(std::int64_t microseconds) {
    return bg::MIN_DT + std::chrono::microseconds{microseconds};
}

} // namespace

TEST_CASE("a runtime node locates output and input data", "[evaluation][storage][node][feature:graph-compilation]") {
    const auto source = bg::node<runtime_node_source>();
    const auto reference = bg::node<runtime_node_reference>();
    const auto sink = bg::node<runtime_node_sink>();
    auto wiring = bg::GraphWiring{};

    auto direct_sink_index = std::size_t{};
    auto reference_sink_index = std::size_t{};
    [&] {
        const auto context = bg::WiringContext{wiring};
        const auto result = source();

        direct_sink_index = wiring.nodes().size();
        sink(result);

        const auto referenced = reference(result);
        reference_sink_index = wiring.nodes().size();
        sink(referenced);
    }();

    const auto compiled = bg::CompiledGraph{wiring};
    auto runtime = bg::RuntimeGraph{compiled};
    runtime.start(time(1));
    runtime.evaluate(time(1));

    const auto source_node = runtime.node(0);
    CHECK(source_node.output_last_updated().time() == time(1));
    CHECK(*static_cast<const double *>(source_node.output_value()) == 1.5);

    const auto recovered = bg::Node::from_output(compiled.node(0), source_node.output_value());
    CHECK(recovered.output_value() == source_node.output_value());
    CHECK(&recovered.output_last_updated() == &source_node.output_last_updated());

    const auto direct_input = runtime.node(direct_sink_index).time_series_input(0, 0);
    CHECK(direct_input.data() == source_node.output_value());
    CHECK(direct_input.last_updated() == time(1));

    const auto reference_input = runtime.node(reference_sink_index).time_series_input(0, 0);
    CHECK(reference_input.data() == source_node.output_value());
    CHECK(reference_input.last_updated() == time(1));
    CHECK(&reference_input.runtime_type() == &direct_input.runtime_type());
}
