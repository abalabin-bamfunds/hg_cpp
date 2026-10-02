#include <bg/node/input.hpp>
#include <bg/node/output.hpp>
#include <bg/util/reflection.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>

namespace {

struct add {
    bg::In<bg::TS<float>> lhs;
    bg::In<bg::TS<float>> rhs;
    bg::Out<bg::TS<float>> output;

    void evaluate() const { output.set(*lhs + *rhs); }
};

static_assert(bg::util::unqualified_type_name<add>() == "add");
static_assert(bg::util::aggregate_member_names<add>[0] == "lhs");
static_assert(bg::util::aggregate_member_names<add>[1] == "rhs");
static_assert(bg::util::aggregate_member_names<add>[2] == "output");

} // namespace

TEST_CASE("an aggregate node declaration evaluates through its fields",
          "[node_api][feature:node-api][definition:aggregate]") {
    auto input_state = bg::TimeSeriesState{};
    input_state.publish(bg::MIN_DT + std::chrono::microseconds{1});
    const bg::EvaluationTime evaluation_time = bg::MIN_DT + std::chrono::microseconds{2};
    const float lhs = 1.25F;
    const float rhs = 2.5F;

    auto output_state_value = bg::TimeSeriesState{};
    float output_value{};
    auto output_state = bg::details::OutputState{bg::cpp_time_series_runtime_type<bg::TS<float>>()};
    output_state.bind(output_state_value, &output_value, evaluation_time);

    add value{{input_state, &lhs}, {input_state, &rhs}, bg::details::output_factory::make<bg::TS<float>>(output_state)};

    value.evaluate();

    CHECK(output_state_value.time() == evaluation_time);
    CHECK(output_value == 3.75F);
}
