#include <bg/bg.hpp>

#include "../../utils/counting_memory_resource.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstdint>

namespace {

struct inspect_scalar_list {
    bg::Scalar<bg::List<std::int64_t>> values;

    void start(bg::EvaluationContext *context) const {
        context->global_state().write([&](auto &state) {
            state.set(bg::util::symbol{"result"}, bg::ScalarValue{values[0].value() + values[1].value()});
        });
    }

    void evaluate() const {}
};

TEST_CASE("a scalar list is available from packed graph scalar storage",
          "[node][scalar][list][scalar:list][feature:state]") {
    const auto inspect = bg::node<inspect_scalar_list>();
    const auto input = bg::ScalarValue<bg::List<std::int64_t>>{std::array<std::int64_t, 2>{1, 2}};
    auto state = bg::GlobalState{};

    bg::run_graph([&] { inspect(input); }, state);

    state.read([](const auto &values) { CHECK(bg::scalar_cast<std::int64_t>(values.at("result")).value() == 3); });
}

struct inspect_scalar_list_state {
    bg::State<bg::List<std::int64_t>> values;

    void start(bg::EvaluationContext *context) const {
        values->append(1);
        values->append(2);

        context->global_state().write([&](auto &state) {
            state.set(bg::util::symbol{"result"}, bg::ScalarValue{(*values)[0] + (*values)[1]});
        });
    }

    void evaluate() const {}
};

TEST_CASE("scalar list state is available during node start",
          "[node][state][scalar][list][scalar:list][feature:state][transition:start]") {
    const auto inspect = bg::node<inspect_scalar_list_state>();
    auto state = bg::GlobalState{};

    bg::run_graph([&] { inspect(); }, state);

    state.read([](const auto &values) { CHECK(bg::scalar_cast<std::int64_t>(values.at("result")).value() == 3); });
}

struct scalar_list_source {
    bg::Scalar<bg::List<std::int64_t>> value;
    bg::Out<bg::TS<bg::List<std::int64_t>>> output;

    void start() const { output.set_value(value); }
    void evaluate() const {}
};

struct scalar_list_sink {
    bg::In<bg::TS<bg::List<std::int64_t>>> input;

    void evaluate(bg::EvaluationContext *context) const {
        const auto values = input.value_view();
        context->global_state().write([&](auto &state) {
            state.set(bg::util::symbol{"result"}, bg::ScalarValue{values[0].value() + values[1].value()});
        });
    }
};

TEST_CASE("a scalar list propagates through a value time series",
          "[node][time_series][scalar][list][scalar:list][feature:state]") {
    const auto source = bg::node<scalar_list_source>();
    const auto sink = bg::node<scalar_list_sink>();
    const auto input = bg::ScalarValue<bg::List<std::int64_t>>{std::array<std::int64_t, 2>{1, 2}};
    auto state = bg::GlobalState{};

    bg::run_graph([&] { sink(source(input)); }, state);

    state.read([](const auto &values) { CHECK(bg::scalar_cast<std::int64_t>(values.at("result")).value() == 3); });
}

TEST_CASE("a runtime graph releases scalar list time-series storage",
          "[node][time_series][scalar][list][scalar:list][allocation][feature:state]") {
    const auto source = bg::node<scalar_list_source>();
    const auto input = bg::ScalarValue<bg::List<std::int64_t>>{std::array<std::int64_t, 2>{1, 2}};
    auto wiring = bg::GraphWiring{};
    {
        const auto context = bg::WiringContext{wiring};
        (void)source(input);
    }

    wiring.finalize();
    const auto graph = bg::CompiledGraph{wiring};
    auto memory = counting_memory_resource{};
    {
        auto runtime_global_state = bg::GlobalState{bg::allocator{memory}};
        auto runtime = bg::RuntimeGraph{graph, bg::allocator{memory}, runtime_global_state};
        bg::SimulationEvaluationEngine{}.run(runtime, bg::MIN_DT + std::chrono::microseconds{1});
    }

    CHECK(memory.allocations() == 0);
}

} // namespace
