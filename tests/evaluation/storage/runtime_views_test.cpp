#include <bg/graph/compiled_graph.hpp>
#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/runtime_graph.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/node.hpp>
#include <bg/node/input.hpp>
#include <bg/run_graph.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <vector>

using namespace bg;

namespace {

struct float_sink_definition {
    In<TS<float>> input;

    void evaluate() const {
        ++evaluation_count;
        last_value = *input;
    }

    inline static int evaluation_count{};
    inline static float last_value{};
};

[[nodiscard]] EvaluationTime time(i64 microseconds) {
    return MIN_DT + std::chrono::microseconds{microseconds};
}

} // namespace

TEST_CASE("a scalar list passes values through compiled wiring", "[evaluation][views][feature:time-series-view]") {
    struct list_source_definition {
        Scalar<List<float>> values;
        Out<TS<float>> output;

        void start() const { output.set(values[0].value()); }

        void evaluate() const {}
    };

    const auto list_source = bg::node<list_source_definition>();

    const auto float_sink = bg::node<float_sink_definition>();
    float_sink_definition::evaluation_count = 0;
    auto values = std::vector{2.5F};
    const auto list = ScalarValue<List<float>>{values};

    GraphWiring wiring;
    {
        WiringContext context{wiring};
        float_sink(list_source(list));
    }
    values[0] = 4.0F;

    wiring.finalize();
    const CompiledGraph compiled{wiring};
    auto runtime_global_state = GlobalState{};
    RuntimeGraph runtime{compiled, runtime_global_state};
    runtime.start(time(1));
    runtime.evaluate(time(1));

    CHECK(float_sink_definition::evaluation_count == 1);
    CHECK(float_sink_definition::last_value == 2.5F);
}

namespace {

struct generic_list_source_definition {
    Scalar<List<SCALAR>> values;
    Out<TS<SCALAR>> output;

    void start() const { output.set_value(values[0]); }

    void evaluate() const {}
};

} // namespace

TEST_CASE("a generic scalar list resolves its item view",
          "[evaluation][views][feature:time-series-view][declaration:generic]") {
    const auto generic_list_source = bg::node<generic_list_source_definition>();
    const auto values = std::vector{2.5F};
    const auto list = ScalarValue<List<float>>{values};
    const auto graph = [&] { return generic_list_source(list); };

    const auto actual = bg::run_graph(graph);

    CHECK(actual.equals<float>({2.5F}));
}

namespace {

struct starting_source_definition {
    float value;
    Out<TS<float>> output;

    void start() const { output.set(value); }

    void evaluate() const {}
};

struct generic_value_copy_definition {
    In<TS<SCALAR>> input;
    Out<TS<SCALAR>> output;

    void evaluate() const { output.set_value(input.value_view()); }
};

} // namespace

TEST_CASE("a generic scalar time-series view preserves its resolved scalar type",
          "[evaluation][views][feature:time-series-view][declaration:generic]") {
    const auto starting_source = bg::node<starting_source_definition>();

    const auto generic_value_copy = bg::node<generic_value_copy_definition>();
    const auto graph = [&] { return generic_value_copy(starting_source(2.5F)); };

    const auto actual = bg::run_graph(graph);

    CHECK(actual.equals<float>({2.5F}));
}

namespace {

struct generic_copy_definition {
    In<TS_T> input;
    Out<TS<double>> output;

    void evaluate() const {
        input->visit([&](const In<TS<SCALAR>> *value) { output.set_value(value->value_view()); },
                     [](const In<TSL<TS_T, bg::SIZE>> *) {}, ddv::noop);
    }
};

struct double_source_definition {
    Out<TS<double>> output;

    void start(EvaluationContext *context) const { context->schedule_now(); }
    void evaluate() const { output.set(1.5); }
};

} // namespace

TEST_CASE("a generic time-series view preserves its resolved time-series type",
          "[evaluation][views][feature:time-series-view][declaration:generic]") {
    const auto generic_copy = bg::node<generic_copy_definition>();

    const auto double_source = bg::node<double_source_definition>();
    const auto graph = [&] { return generic_copy(double_source()); };

    const auto actual = bg::run_graph(graph);

    CHECK(actual.equals({1.5}));
}

namespace {

struct generic_list_view_copy_definition {
    In<TSL<TS_T, bg::SIZE>> input;
    Out<TSL<TS_T, bg::SIZE>> output;

    void evaluate() const {
        input_size = input.size();
        output_size = output.size();

        input[1].visit(
            [&](const In<TS<SCALAR>> *source) {
                output[0].visit(
                    [&](const Out<TS<SCALAR>> *destination) { destination->set_value(source->value_view()); },
                    ddv::noop);
            },
            ddv::noop);
    }

    inline static std::size_t input_size{};
    inline static std::size_t output_size{};
};

struct generic_list_view_value_definition {
    In<TSL<TS<i64>, 2>> input;
    Out<TS<i64>> output;

    void evaluate() const { output.set(*input[0]); }
};

} // namespace

TEST_CASE("generic time-series list views expose their resolved size and items",
          "[evaluation][views][tsl][series:fixed-tsl][feature:time-series-view][declaration:generic]") {
    const auto copy = bg::node<generic_list_view_copy_definition>();

    const auto value = bg::node<generic_list_view_value_definition>();
    generic_list_view_copy_definition::input_size = 0;
    generic_list_view_copy_definition::output_size = 0;

    const auto graph = [&](bg::wp<TSL<TS<i64>, 2>> input) { return value(copy(input)); };

    const auto actual = bg::run_graph(graph, {{{0, 3}, {1, 7}}});

    CHECK(generic_list_view_copy_definition::input_size == 2);
    CHECK(generic_list_view_copy_definition::output_size == 2);
    CHECK(actual.equals<i64>({7}));
}

namespace {

template <template <typename, auto...> typename Field> struct RuntimeViewBundle {
    Field<TS<i64>> count;
    Field<TS<float>> value;
};

struct generic_bundle_view_copy_definition {
    In<TSB<bg::SCHEMA>> input;
    Out<TSB<bg::SCHEMA>> output;

    void evaluate() const {
        input_size = input.size();
        output_size = output.size();
        first_name = input.item_name(0);
        second_name = output.item_name(1);

        input[0].visit(
            [&](const In<TS<SCALAR>> *source) {
                output[0].visit(
                    [&](const Out<TS<SCALAR>> *destination) { destination->set_value(source->value_view()); },
                    ddv::noop);
            },
            ddv::noop);
    }

    inline static std::size_t input_size{};
    inline static std::size_t output_size{};
    inline static bg::util::symbol first_name{""};
    inline static bg::util::symbol second_name{""};
};

struct generic_bundle_view_value_definition {
    In<TSB<RuntimeViewBundle>> input;
    Out<TS<i64>> output;

    void evaluate() const { output.set(*input->count); }
};

} // namespace

TEST_CASE("generic bundle views expose their resolved schema and items",
          "[evaluation][views][tsb][series:tsb][feature:time-series-view][declaration:generic]"
          "[schema:generic]") {
    const auto copy = bg::node<generic_bundle_view_copy_definition>();

    const auto value = bg::node<generic_bundle_view_value_definition>();
    generic_bundle_view_copy_definition::input_size = 0;
    generic_bundle_view_copy_definition::output_size = 0;
    generic_bundle_view_copy_definition::first_name = bg::util::symbol{""};
    generic_bundle_view_copy_definition::second_name = bg::util::symbol{""};

    const auto graph = [&](bg::wp<TSB<RuntimeViewBundle>> input) { return value(copy(input)); };

    const auto actual = bg::run_graph(graph, {{{"count", 4}, {"value", 2.5F}}});

    CHECK(generic_bundle_view_copy_definition::input_size == 2);
    CHECK(generic_bundle_view_copy_definition::output_size == 2);
    CHECK(generic_bundle_view_copy_definition::first_name.text() == "count");
    CHECK(generic_bundle_view_copy_definition::second_name.text() == "value");
    CHECK(actual.equals<i64>({4}));
}
