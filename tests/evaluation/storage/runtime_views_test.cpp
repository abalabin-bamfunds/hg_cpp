#include <bg/graph/compiled_graph.hpp>
#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/runtime_graph.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/node.hpp>
#include <bg/node/input.hpp>
#include <bg/testing/test_graph.hpp>

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
        last_value = *input;
    }

    inline static int evaluation_count{};
    inline static float last_value{};
};

[[nodiscard]] bg::EvaluationTime time(std::int64_t microseconds) {
    return bg::MIN_DT + std::chrono::microseconds{microseconds};
}

} // namespace

TEST_CASE("a scalar list passes values through compiled wiring", "[evaluation][views][feature:time-series-view]") {
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

TEST_CASE("a generic scalar list resolves its item view",
          "[evaluation][views][feature:time-series-view][declaration:generic]") {
    const auto generic_list_source = bg::node<generic_list_source_definition>();
    const auto values = std::vector{2.5F};
    const auto list = bg::ScalarValue<bg::List<float>>{values};
    const auto graph = [&] { return generic_list_source(list); };

    const auto actual = bg::testing::test_graph(graph);

    CHECK(actual.equals<float>({2.5F}));
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

TEST_CASE("a generic scalar time-series view preserves its resolved scalar type",
          "[evaluation][views][feature:time-series-view][declaration:generic]") {
    const auto starting_source = bg::node<starting_source_definition>();
    const auto generic_value_copy = bg::node<generic_value_copy_definition>();
    const auto graph = [&] { return generic_value_copy(starting_source(2.5F)); };

    const auto actual = bg::testing::test_graph(graph);

    CHECK(actual.equals<float>({2.5F}));
}

namespace {

struct generic_copy_definition {
    bg::In<bg::TS_T> input;
    bg::Out<bg::TS<double>> output;

    void evaluate() const {
        input->visit([&](const bg::In<bg::TS<bg::SCALAR>> *value) { output.set_value(value->value_view()); },
                     [](const bg::In<bg::TSL<bg::TS_T, bg::SIZE>> *) {}, ddv::noop);
    }
};

struct double_source_definition {
    bg::Out<bg::TS<double>> output;

    void start(bg::EvaluationContext *context) const { context->schedule_now(); }
    void evaluate() const { output.set(1.5); }
};

} // namespace

TEST_CASE("a generic time-series view preserves its resolved time-series type",
          "[evaluation][views][feature:time-series-view][declaration:generic]") {
    const auto generic_copy = bg::node<generic_copy_definition>();
    const auto double_source = bg::node<double_source_definition>();
    const auto graph = [&] { return generic_copy(double_source()); };

    const auto actual = bg::testing::test_graph(graph);

    CHECK(actual.equals({1.5}));
}

namespace {

struct generic_list_view_source_definition {
    bg::Out<bg::TSL<bg::TS<int>, 2>> output;

    void start() const {
        output[0].set(3);
        output[1].set(7);
    }
};

struct generic_list_view_copy_definition {
    bg::In<bg::TSL<bg::TS_T, bg::SIZE>> input;
    bg::Out<bg::TSL<bg::TS_T, bg::SIZE>> output;

    void evaluate() const {
        input_size = input.size();
        output_size = output.size();

        input[1].visit(
            [&](const bg::In<bg::TS<bg::SCALAR>> *source) {
                output[0].visit(
                    [&](const bg::Out<bg::TS<bg::SCALAR>> *destination) {
                        destination->set_value(source->value_view());
                    },
                    ddv::noop);
            },
            ddv::noop);
    }

    inline static std::size_t input_size{};
    inline static std::size_t output_size{};
};

struct generic_list_view_value_definition {
    bg::In<bg::TSL<bg::TS<int>, 2>> input;
    bg::Out<bg::TS<int>> output;

    void evaluate() const { output.set(*input[0]); }
};

} // namespace

TEST_CASE("generic time-series list views expose their resolved size and items",
          "[evaluation][views][tsl][series:fixed-tsl][feature:time-series-view][declaration:generic]") {
    const auto source = bg::node<generic_list_view_source_definition>();
    const auto copy = bg::node<generic_list_view_copy_definition>();
    const auto value = bg::node<generic_list_view_value_definition>();
    generic_list_view_copy_definition::input_size = 0;
    generic_list_view_copy_definition::output_size = 0;
    const auto graph = [&] { return value(copy(source())); };

    const auto actual = bg::testing::test_graph(graph);

    CHECK(generic_list_view_copy_definition::input_size == 2);
    CHECK(generic_list_view_copy_definition::output_size == 2);
    CHECK(actual.equals<int>({7}));
}

namespace {

template <template <typename, auto...> typename Field> struct RuntimeViewBundle {
    Field<bg::TS<int>> count;
    Field<bg::TS<float>> value;
};

struct generic_bundle_view_source_definition {
    bg::Out<bg::TSB<RuntimeViewBundle>> output;

    void start() const {
        output->count.set(4);
        output->value.set(2.5F);
    }
};

struct generic_bundle_view_copy_definition {
    bg::In<bg::TSB<bg::SCHEMA>> input;
    bg::Out<bg::TSB<bg::SCHEMA>> output;

    void evaluate() const {
        input_size = input.size();
        output_size = output.size();
        first_name = input.item_name(0);
        second_name = output.item_name(1);

        input[0].visit(
            [&](const bg::In<bg::TS<bg::SCALAR>> *source) {
                output[0].visit(
                    [&](const bg::Out<bg::TS<bg::SCALAR>> *destination) {
                        destination->set_value(source->value_view());
                    },
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
    bg::In<bg::TSB<RuntimeViewBundle>> input;
    bg::Out<bg::TS<int>> output;

    void evaluate() const { output.set(*input->count); }
};

} // namespace

TEST_CASE("generic bundle views expose their resolved schema and items",
          "[evaluation][views][tsb][series:tsb][feature:time-series-view][declaration:generic]"
          "[schema:generic]") {
    const auto source = bg::node<generic_bundle_view_source_definition>();
    const auto copy = bg::node<generic_bundle_view_copy_definition>();
    const auto value = bg::node<generic_bundle_view_value_definition>();
    generic_bundle_view_copy_definition::input_size = 0;
    generic_bundle_view_copy_definition::output_size = 0;
    generic_bundle_view_copy_definition::first_name = bg::util::symbol{""};
    generic_bundle_view_copy_definition::second_name = bg::util::symbol{""};
    const auto graph = [&] { return value(copy(source())); };

    const auto actual = bg::testing::test_graph(graph);

    CHECK(generic_bundle_view_copy_definition::input_size == 2);
    CHECK(generic_bundle_view_copy_definition::output_size == 2);
    CHECK(generic_bundle_view_copy_definition::first_name.text() == "count");
    CHECK(generic_bundle_view_copy_definition::second_name.text() == "value");
    CHECK(actual.equals<int>({4}));
}
