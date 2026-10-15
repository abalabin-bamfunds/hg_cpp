#include <bg/graph/compiled_graph.hpp>
#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/node.hpp>
#include <bg/graph/runtime_graph.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/node.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
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

[[nodiscard]] bg::EvaluationTime time(bg::i64 microseconds) {
    return bg::MIN_DT + std::chrono::microseconds{microseconds};
}

} // namespace

TEST_CASE("a runtime node locates output and input data", "[evaluation][storage][node][feature:graph-compilation]") {
    const auto source = bg::node<runtime_node_source>();
    const auto reference = bg::node<runtime_node_reference>();
    const auto sink = bg::node<runtime_node_sink>();
    auto wiring = bg::GraphWiring{};

    auto direct_sink = bg::NodeCallIdentity{};
    auto reference_sink = bg::NodeCallIdentity{};
    [&] {
        const auto context = bg::WiringContext{wiring};
        const auto result = source();
        const auto direct_arguments = std::array{bg::WiringArgument{result}};
        direct_sink = wiring.invoke_node(sink, direct_arguments).identity;

        const auto referenced = reference(result);
        const auto reference_arguments = std::array{bg::WiringArgument{referenced}};
        reference_sink = wiring.invoke_node(sink, reference_arguments).identity;
    }();

    wiring.finalize();
    const auto direct_sink_index = wiring.node_index(direct_sink);
    const auto reference_sink_index = wiring.node_index(reference_sink);
    const auto compiled = bg::CompiledGraph{wiring};
    auto runtime_global_state = bg::GlobalState{};
    auto runtime = bg::RuntimeGraph{compiled, runtime_global_state};
    runtime.start(time(1));
    runtime.evaluate(time(1));

    const auto source_node = runtime.node(0);
    CHECK(source_node.output_last_updated().time() == time(1));
    CHECK(*static_cast<const double *>(source_node.output_value()) == 1.5);

    const auto recovered = bg::Node::from_output(compiled.node(0), source_node.output_value());
    CHECK(recovered.output_value() == source_node.output_value());
    CHECK(&recovered.output_last_updated() == &source_node.output_last_updated());

    const auto direct_input = runtime.node(direct_sink_index).time_series_input(0);
    CHECK(direct_input.data() == source_node.output_value());
    CHECK(direct_input.last_updated() == time(1));

    const auto reference_input = runtime.node(reference_sink_index).time_series_input(0);
    CHECK(reference_input.data() == source_node.output_value());
    CHECK(reference_input.last_updated() == time(1));
    CHECK(&reference_input.runtime_type() == &direct_input.runtime_type());
}
