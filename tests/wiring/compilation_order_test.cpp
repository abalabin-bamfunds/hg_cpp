#include <bg/graph.hpp>
#include <bg/graph/compiled_graph.hpp>
#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/nested.hpp>
#include <bg/node.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cstdint>
#include <string_view>
#include <vector>

namespace {

using namespace bg;
using namespace bg::literals;

struct OrderSource {
    std::int64_t value;
    Out<TS<std::int64_t>> output;

    void evaluate() const { output.set(value); }
};

struct OrderConsumer {
    In<TS<std::int64_t>> input;

    void evaluate() const { (void)*input; }
};

struct OrderJoin {
    In<TS<std::int64_t>> request;
    In<TS<std::int64_t>> response;
    Out<TS<std::int64_t>> output;

    void evaluate() const { output.set(*response); }
};

struct RunSource {
    bool value;
    Out<TS<bool>> output;

    void evaluate() const { output.set(value); }
};

} // namespace

TEST_CASE("compilation orders a node after every direct consumer of a wiring point",
          "[feature:compilation-order][feature:wiring][feature:compilation]") {
    const auto identity = 0;
    const auto requirement_identity = details::NestedBindingIdentity::object(&identity);
    const auto source = node<OrderSource>([](std::int64_t value) { return value == 1 ? "collector" : "provider"; });
    const auto consume_before = node<OrderConsumer>([](wp<TS<std::int64_t>>) { return "client_before"; });
    const auto consume_after = node<OrderConsumer>([](wp<TS<std::int64_t>>) { return "client_after"; });
    auto wiring = GraphWiring{};
    const auto wiring_context = WiringContext{wiring};
    const auto requirement =
        wiring.require_inherited_time_series(requirement_identity, util::symbol{"input"}, TS<std::int64_t>::type());
    (void)consume_before(requirement);
    const auto collector = source(1);
    (void)consume_after(requirement);
    const auto provider = source(2);
    wiring.provide_inherited_time_series(requirement_identity, TS<std::int64_t>::type(), provider);

    wiring.order_after_consumers(requirement, collector);
    wiring.order_after_consumers(requirement, collector);
    REQUIRE(wiring.ordering_constraints().size() == 1);

    const auto compiled = CompiledGraph{wiring};
    const auto index_of_label = [&](std::string_view label) {
        for (auto index = std::size_t{}; index < compiled.node_count(); ++index) {
            if (compiled.wiring_path(index).label.text() == label) {
                return index;
            }
        }
        return compiled.node_count();
    };
    const auto client_before_index = index_of_label("client_before");
    const auto client_after_index = index_of_label("client_after");
    const auto collector_index = index_of_label("collector");

    REQUIRE(client_before_index < compiled.node_count());
    REQUIRE(client_after_index < compiled.node_count());
    REQUIRE(collector_index < compiled.node_count());
    CHECK(client_before_index < collector_index);
    CHECK(client_after_index < collector_index);
}

TEST_CASE("ordering constraints include consumers wired after an inherited provider",
          "[feature:compilation-order][feature:wiring][feature:compilation]") {
    const auto identity = 0;
    const auto requirement_identity = details::NestedBindingIdentity::object(&identity);
    const auto source = node<OrderSource>([](std::int64_t value) { return value == 1 ? "collector" : "provider"; });
    const auto consume = node<OrderConsumer>([](wp<TS<std::int64_t>>) { return "client"; });
    auto wiring = GraphWiring{};
    const auto wiring_context = WiringContext{wiring};
    const auto requirement =
        wiring.require_inherited_time_series(requirement_identity, util::symbol{"input"}, TS<std::int64_t>::type());
    const auto collector = source(1);
    const auto provider = source(2);
    wiring.provide_inherited_time_series(requirement_identity, TS<std::int64_t>::type(), provider);
    (void)consume(requirement);
    wiring.order_after_consumers(requirement, collector);

    const auto compiled = CompiledGraph{wiring};
    auto client_index = compiled.node_count();
    auto collector_index = compiled.node_count();
    for (auto index = std::size_t{}; index < compiled.node_count(); ++index) {
        const auto label = compiled.wiring_path(index).label.text();
        if (label == "client") {
            client_index = index;
        }
        if (label == "collector") {
            collector_index = index;
        }
    }

    REQUIRE(client_index < compiled.node_count());
    REQUIRE(collector_index < compiled.node_count());
    CHECK(client_index < collector_index);
}

TEST_CASE("ordering constraints include consumers across nested graph boundaries",
          "[feature:compilation-order][feature:nested-graph][feature:compilation]") {
    const auto identity = 0;
    const auto requirement_identity = details::NestedBindingIdentity::object(&identity);
    const auto copy = node([](In<TS<std::int64_t>> input, Out<TS<std::int64_t>> output) { output.set(*input); });
    const auto inner = graph("name"_n = "service_client", [&] {
        auto &wiring = WiringContext::current().wiring();
        const auto requirement =
            wiring.require_inherited_time_series(requirement_identity, util::symbol{"input"}, TS<std::int64_t>::type());
        return copy(requirement);
    });
    const auto source = node<OrderSource>([](std::int64_t value) { return value == 1 ? "collector" : "provider"; });
    const auto run_source = node<RunSource>([](bool value) { return value ? "first_run" : "second_run"; });
    auto wiring = GraphWiring{};
    const auto wiring_context = WiringContext{wiring};
    const auto collector = source(1);
    const auto first_run = run_source(true);
    const auto second_run = run_source(false);
    (void)nested(inner, first_run);
    (void)nested(inner, second_run);
    const auto requirement =
        wiring.require_inherited_time_series(requirement_identity, util::symbol{"input"}, TS<std::int64_t>::type());
    const auto provider = source(2);
    wiring.provide_inherited_time_series(requirement_identity, TS<std::int64_t>::type(), provider);
    wiring.order_after_consumers(requirement, collector);

    const auto compiled = CompiledGraph{wiring};
    auto collector_index = compiled.node_count();
    auto nested_indices = std::vector<std::size_t>{};
    for (auto index = std::size_t{}; index < compiled.node_count(); ++index) {
        const auto &path = compiled.wiring_path(index);
        if (path.label.text() == "collector") {
            collector_index = index;
        }
        if (path.name.text() == "nested") {
            nested_indices.push_back(index);
        }
    }

    REQUIRE(collector_index < compiled.node_count());
    REQUIRE(nested_indices.size() == 2);
    for (const auto index : nested_indices) {
        CHECK(index < collector_index);
    }
}

TEST_CASE("mixed data and ordering dependencies report every wiring path in a cycle",
          "[feature:compilation-order][feature:wiring][feature:compilation]") {
    const auto identity = 0;
    const auto requirement_identity = details::NestedBindingIdentity::object(&identity);
    const auto producer = node<OrderSource>([](std::int64_t) { return "collector"; });
    const auto client = node<OrderJoin>([](wp<TS<std::int64_t>>, wp<TS<std::int64_t>>) { return "client"; });
    auto wiring = GraphWiring{};
    const auto wiring_context = WiringContext{wiring};
    const auto requirement =
        wiring.require_inherited_time_series(requirement_identity, util::symbol{"input"}, TS<std::int64_t>::type());
    const auto producer_output = producer(1);
    (void)client(requirement, producer_output);
    wiring.provide_inherited_time_series(requirement_identity, TS<std::int64_t>::type(), producer_output);
    wiring.order_after_consumers(requirement, producer_output);

    CHECK_THROWS_WITH(CompiledGraph{wiring}, Catch::Matchers::ContainsSubstring("OrderJoin[client]") &&
                                                 Catch::Matchers::ContainsSubstring("OrderSource[collector]"));
}
