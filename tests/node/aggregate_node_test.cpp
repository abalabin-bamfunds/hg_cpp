#include <bg/graph/compiled_graph.hpp>
#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/wiring_arguments.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/node/input.hpp>
#include <bg/node.hpp>
#include <bg/node/output.hpp>
#include <bg/testing/test_graph.hpp>
#include <bg/util/reflection.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <map>
#include <memory>

using namespace bg;

namespace {

struct alignas(std::max_align_t) PaddedScalarValue {
    i64 value{};
    i64 padding{};
};

struct SelectedOutputRuntimeType final : TimeSeriesValueRuntimeType {
    explicit SelectedOutputRuntimeType(const TimeSeriesType &type)
        : TimeSeriesValueRuntimeType{type, cpp_scalar_runtime_type<i64>(), sizeof(PaddedScalarValue),
                                    alignof(PaddedScalarValue), false, false, true} {}

    void construct_initial_value(void *destination) const noexcept override {
        std::construct_at(static_cast<PaddedScalarValue *>(destination));
    }

    void destroy_value(void *value, allocator &) const noexcept override {
        std::destroy_at(static_cast<PaddedScalarValue *>(value));
    }
};

const TimeSeriesRuntimeType &selected_output_runtime_type(const TimeSeriesType &type) {
    static auto runtime_types = std::map<const TimeSeriesType *, std::unique_ptr<SelectedOutputRuntimeType>>{};
    const auto [entry, inserted] = runtime_types.try_emplace(&type);
    if (inserted) {
        entry->second = std::make_unique<SelectedOutputRuntimeType>(type);
    }
    return *entry->second;
}

using AGGREGATE_SCALAR = ScalarVariable<"AGGREGATE_SCALAR">;

struct selected_runtime_output {
    In<TS<AGGREGATE_SCALAR>> input;
    Out<TS<AGGREGATE_SCALAR>> output;

    inline static int selector_calls{};

    static const TimeSeriesRuntimeType &output_runtime_type(const WiringArguments &arguments) {
        ++selector_calls;
        CHECK(arguments.size() == 1);
        CHECK(&arguments.resolved_type(0) == &TS<i64>::type());
        return selected_output_runtime_type(*arguments.resolved_output_type());
    }

    void evaluate() const {
        CHECK(&output.runtime_type() == &selected_output_runtime_type(output.runtime_type().type()));
        output.set_value(input.value_view());
    }
};

struct selected_concrete_runtime_output {
    In<TS<i64>> input;
    Out<TS<i64>> output;

    static const TimeSeriesRuntimeType &output_runtime_type(const WiringArguments &arguments) {
        return selected_output_runtime_type(*arguments.resolved_output_type());
    }

    void evaluate() const {
        CHECK(&output.runtime_type() == &selected_output_runtime_type(output.runtime_type().type()));
        output.set(input.value());
        CHECK(output.value() == input.value());
    }
};

struct add {
    In<TS<float>> lhs;
    In<TS<float>> rhs;
    Out<TS<float>> output;

    void evaluate() const { output.set(*lhs + *rhs); }
};

static_assert(util::unqualified_type_name<add>() == "add");
static_assert(util::aggregate_member_names<add>[0] == "lhs");
static_assert(util::aggregate_member_names<add>[1] == "rhs");
static_assert(util::aggregate_member_names<add>[2] == "output");

} // namespace

TEST_CASE("an aggregate node declaration evaluates through its fields",
          "[node_api][feature:node-api][definition:aggregate]") {
    auto input_state = TimeSeriesState{};
    input_state.publish(MIN_DT + std::chrono::microseconds{1});
    const EvaluationTime evaluation_time = MIN_DT + std::chrono::microseconds{2};
    const float lhs = 1.25F;
    const float rhs = 2.5F;

    auto output_state_value = TimeSeriesState{};
    float output_value{};
    auto output_state = details::OutputState{cpp_time_series_runtime_type<TS<float>>()};
    output_state.bind(output_state_value, &output_value, evaluation_time);

    add value{{input_state, &lhs}, {input_state, &rhs},
              details::output_factory::make<Out<TS<float>>>(output_state)};

    value.evaluate();

    CHECK(output_state_value.time() == evaluation_time);
    CHECK(output_value == 3.75F);
}

TEST_CASE("an aggregate definition selects its output runtime type during wiring",
          "[feature:node-api][series:value][declaration:generic][definition:aggregate]") {
    selected_runtime_output::selector_calls = 0;
    const auto aggregate = node<selected_runtime_output>();

    const auto graph = [&](wp<TS<i64>> input) -> wp<TS<i64>> {
        return wp<TS<i64>>{aggregate(input)};
    };

    CHECK(testing::test_graph(graph, std::array<i64, 3>{1, 2, 3}).equals<i64>({1, 2, 3}));
    CHECK(selected_runtime_output::selector_calls == 1);
}

TEST_CASE("a concrete aggregate retains its selected output descriptor in the wiring record",
          "[feature:node-api][series:value][declaration:concrete][definition:aggregate]") {
    const auto source = node([](Out<TS<i64>>) {});

    const auto aggregate = node<selected_concrete_runtime_output>();
    auto wiring = GraphWiring{};
    const WiringContext context{wiring};
    const auto graph_output = aggregate(source());

    wiring.finalize();

    REQUIRE(wiring.outputs().size() == 2);
    CHECK(wiring.outputs()[1].runtime_type ==
          &selected_output_runtime_type(TS<i64>::type()));
    const auto compiled = CompiledGraph{wiring};
    CHECK(compiled.node(wiring.output_node_index(graph_output)).has_output_runtime_type());
}

TEST_CASE("a concrete aggregate evaluates using its selected output descriptor",
          "[feature:node-api][series:value][declaration:concrete][definition:aggregate]") {
    const auto aggregate = node<selected_concrete_runtime_output>();

    const auto graph = [&](wp<TS<i64>> input) -> wp<TS<i64>> {
        return wp<TS<i64>>{aggregate(input)};
    };

    CHECK(testing::test_graph(graph, std::array<i64, 3>{1, 2, 3}).equals<i64>({1, 2, 3}));
}
