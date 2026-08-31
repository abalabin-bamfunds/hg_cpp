#include <bg/graph/compiled_graph.hpp>
#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/runtime_graph.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/node.hpp>
#include <bg/run_graph.hpp>
#include <bg/time_series/tsl/tsl_input_view.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <vector>

namespace {

struct list_source_definition {
    bg::Scalar<bg::List<float>> values;
    bg::Out<bg::TS<float>> output;

    void start() const { output.set(values[0].value()); }

    void evaluate() const {}
};

struct float_sink_definition {
    bg::In<bg::TS<float>> input;

    void evaluate() const {
        ++evaluation_count;
        last_value = input.value();
    }

    inline static int evaluation_count{};
    inline static float last_value{};
};

[[nodiscard]] bg::EvaluationTime time(std::int64_t microseconds) {
    return bg::MIN_DT + std::chrono::microseconds{microseconds};
}

} // namespace

TEST_CASE("a scalar list passes values through compiled wiring", "[evaluation][views]") {
    const auto list_source = bg::node<list_source_definition>();
    const auto float_sink = bg::node<float_sink_definition>();
    float_sink_definition::evaluation_count = 0;
    auto values = std::vector{2.5F};
    const auto list = bg::ScalarValue<bg::List<float>>{values};

    bg::GraphWiring wiring;
    {
        bg::WiringContext context{wiring};
        float_sink(list_source(list));
    }
    values[0] = 4.0F;

    const bg::CompiledGraph compiled{wiring};
    bg::RuntimeGraph runtime{compiled};
    runtime.start(time(1));
    runtime.evaluate(time(1));

    CHECK(float_sink_definition::evaluation_count == 1);
    CHECK(float_sink_definition::last_value == 2.5F);
}

namespace {

struct generic_list_source_definition {
    bg::Scalar<bg::List<bg::SCALAR>> values;
    bg::Out<bg::TS<bg::SCALAR>> output;

    void start() const { output.set_value(values[0]); }

    void evaluate() const {}
};

} // namespace

TEST_CASE("a generic scalar list resolves its item view", "[evaluation][views]") {
    const auto generic_list_source = bg::node<generic_list_source_definition>();
    const auto float_sink = bg::node<float_sink_definition>();
    float_sink_definition::evaluation_count = 0;
    const auto values = std::vector{2.5F};
    const auto list = bg::ScalarValue<bg::List<float>>{values};

    bg::run_graph([&] { float_sink(generic_list_source(list)); });

    CHECK(float_sink_definition::evaluation_count == 1);
    CHECK(float_sink_definition::last_value == 2.5F);
}

namespace {

struct starting_source_definition {
    float value;
    bg::Out<bg::TS<float>> output;

    void start() const { output.set(value); }

    void evaluate() const {}
};

struct generic_value_copy_definition {
    bg::In<bg::TS<bg::SCALAR>> input;
    bg::Out<bg::TS<bg::SCALAR>> output;

    void evaluate() const { output.set_value(input.value_view()); }
};

} // namespace

TEST_CASE("a generic scalar time-series view preserves its resolved scalar type", "[evaluation][views]") {
    const auto starting_source = bg::node<starting_source_definition>();
    const auto generic_value_copy = bg::node<generic_value_copy_definition>();
    const auto float_sink = bg::node<float_sink_definition>();
    float_sink_definition::evaluation_count = 0;

    bg::run_graph([&] { float_sink(generic_value_copy(starting_source(2.5F))); });

    CHECK(float_sink_definition::evaluation_count == 1);
    CHECK(float_sink_definition::last_value == 2.5F);
}

namespace {

struct generic_copy_definition {
    bg::In<bg::TS_T> input;
    bg::Out<bg::TS_T> output;

    void evaluate() const {
        input->visit(
            [&](const bg::TimeSeriesValueInputView *value) {
                const auto scalar = value->erased_value_view();
                static_cast<const bg::TimeSeriesValueOutputView *>(output.operator->())->set_value(scalar.input_view());
            },
            [](const bg::TimeSeriesListInputView *) {}, ddv::noop);
    }
};

struct double_source_definition {
    bg::Out<bg::TS<double>> output;

    void evaluate() const { output.set(1.5); }
};

struct double_sink_definition {
    bg::In<bg::TS<double>> input;

    void evaluate() const {
        ++evaluation_count;
        last_value = input.value();
    }

    inline static int evaluation_count{};
    inline static double last_value{};
};

} // namespace

TEST_CASE("a generic time-series view preserves its resolved time-series type", "[evaluation][views]") {
    const auto generic_copy = bg::node<generic_copy_definition>();
    const auto double_source = bg::node<double_source_definition>();
    const auto double_sink = bg::node<double_sink_definition>();
    double_sink_definition::evaluation_count = 0;

    bg::GraphWiring wiring;
    {
        bg::WiringContext context{wiring};
        double_sink(generic_copy(double_source()));
    }

    const bg::CompiledGraph compiled{wiring};
    bg::RuntimeGraph runtime{compiled};
    runtime.start(time(1));
    runtime.schedule(0);
    runtime.evaluate(time(1));

    CHECK(double_sink_definition::evaluation_count == 1);
    CHECK(double_sink_definition::last_value == 1.5);
}
