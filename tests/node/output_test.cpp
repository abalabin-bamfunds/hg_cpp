#include <bg/node/output.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <type_traits>

namespace {

using FloatOutput = bg::Out<bg::TS<float>>;

static_assert(sizeof(FloatOutput) == 4 * sizeof(void *));
static_assert(std::is_trivially_copyable_v<FloatOutput>);

} // namespace

TEST_CASE("a value output publishes at the evaluation time", "[node_api]") {
    bg::EvaluationTime last_updated = bg::MIN_DT;
    float value{};
    const bg::EvaluationTime evaluation_time = bg::MIN_DT + std::chrono::microseconds{1};
    FloatOutput output{last_updated, value, evaluation_time};

    output.set(1.25F);

    REQUIRE(output.valid());
    CHECK(output.last_updated() == evaluation_time);
    CHECK(output.value() == 1.25F);
}

TEST_CASE("invalidating a value output retains its storage", "[node_api]") {
    bg::EvaluationTime last_updated = bg::MIN_DT + std::chrono::microseconds{1};
    float value = 1.25F;
    const bg::EvaluationTime evaluation_time = bg::MIN_DT + std::chrono::microseconds{2};
    FloatOutput output{last_updated, value, evaluation_time};

    output.invalidate();

    CHECK_FALSE(output.valid());
    CHECK(last_updated == bg::MIN_DT);
    CHECK(value == 1.25F);
}

TEST_CASE("a value output accepts a compatible scalar view", "[node_api]") {
    bg::EvaluationTime last_updated = bg::MIN_DT;
    float value{};
    const bg::EvaluationTime evaluation_time = bg::MIN_DT + std::chrono::microseconds{1};
    FloatOutput output{last_updated, value, evaluation_time};
    const auto source_value = 1.25F;
    const auto source = bg::Scalar<bg::SCALAR>::from<float>(source_value);

    output.set_value(source);

    CHECK(output.value() == 1.25F);
    CHECK(output.last_updated() == evaluation_time);
}

TEST_CASE("a value output rejects an incompatible scalar view", "[node_api]") {
    bg::EvaluationTime last_updated = bg::MIN_DT;
    float value{};
    const bg::EvaluationTime evaluation_time = bg::MIN_DT + std::chrono::microseconds{1};
    FloatOutput output{last_updated, value, evaluation_time};
    const auto source_value = 1.25;
    const auto source = bg::Scalar<double>::from(source_value);

    CHECK_THROWS_AS(output.set_value(source), std::invalid_argument);
    CHECK_FALSE(output.valid());
}

TEST_CASE("a generic value output accepts a compatible erased scalar view", "[node_api]") {
    bg::EvaluationTime last_updated = bg::MIN_DT;
    float value{};
    const auto evaluation_time = bg::MIN_DT + std::chrono::microseconds{1};
    const auto source_value = 1.25F;
    const auto source = bg::Scalar<bg::SCALAR>::from<float>(source_value);
    const auto *prototype = bg::cpp_scalar_storage_representation<float>().output_view_prototype();
    const auto output = bg::Out<bg::TS<bg::SCALAR>>{last_updated, &value, *prototype, evaluation_time};

    output.set_value(source);

    CHECK(value == 1.25F);
    CHECK(last_updated == evaluation_time);
}

TEST_CASE("an arbitrary time-series output accepts a compatible scalar view", "[node_api]") {
    bg::EvaluationTime last_updated = bg::MIN_DT;
    float value{};
    const auto evaluation_time = bg::MIN_DT + std::chrono::microseconds{1};
    const auto source_value = 1.25F;
    const auto source = bg::Scalar<>::from<float>(source_value);
    const auto *prototype = bg::cpp_scalar_storage_representation<float>().output_view_prototype();
    const auto output = bg::Out<bg::TS_T>{last_updated, &value, *prototype, evaluation_time};

    output.set_value(source);

    CHECK(value == 1.25F);
    CHECK(last_updated == evaluation_time);
}

TEST_CASE("a generic value output rejects an incompatible scalar view", "[node_api]") {
    bg::EvaluationTime last_updated = bg::MIN_DT;
    float value{};
    const auto evaluation_time = bg::MIN_DT + std::chrono::microseconds{1};
    const auto source_value = 1.25;
    const auto source = bg::Scalar<bg::SCALAR>::from<double>(source_value);
    const auto *prototype = bg::cpp_scalar_storage_representation<float>().output_view_prototype();
    const auto output = bg::Out<bg::TS<bg::SCALAR>>{last_updated, &value, *prototype, evaluation_time};

    CHECK_THROWS_AS(output.set_value(source), std::invalid_argument);
    CHECK_FALSE(output.valid());
}
