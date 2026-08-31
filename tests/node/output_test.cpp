#include <bg/node/output.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <type_traits>

namespace {

using FloatOutput = bg::Out<bg::TS<float>>;

static_assert(sizeof(FloatOutput) == sizeof(void *));
static_assert(sizeof(bg::Out<bg::TS_T>) == sizeof(void *));
static_assert(sizeof(bg::Out<bg::TS<bg::SCALAR>>) == sizeof(void *));
static_assert(sizeof(bg::Out<bg::REF<bg::TS<int>>>) == sizeof(void *));
static_assert(sizeof(bg::Out<bg::TSL<bg::TS<int>, 2>>) == sizeof(void *));
static_assert(sizeof(bg::Out<bg::TSS<int>>) == sizeof(void *));
static_assert(sizeof(bg::Out<bg::TSD<int, bg::TS<int>>>) == sizeof(void *));
static_assert(!std::is_constructible_v<FloatOutput, bg::TypedTimeSeriesValueOutputView<float> &>);
static_assert(!std::is_copy_constructible_v<FloatOutput>);
static_assert(std::is_move_constructible_v<FloatOutput>);

template <typename Output>
concept ScalarWritableOutput = requires(const Output &output, const bg::Scalar<> &value) { output.set_value(value); };

static_assert(!ScalarWritableOutput<bg::Out<bg::TS_T>>);
static_assert(ScalarWritableOutput<bg::Out<bg::TS<bg::SCALAR>>>);

} // namespace

TEST_CASE("a value output publishes at the evaluation time", "[node_api]") {
    bg::EvaluationTime last_updated = bg::MIN_DT;
    float value{};
    const bg::EvaluationTime evaluation_time = bg::MIN_DT + std::chrono::microseconds{1};
    auto view = bg::util::polymorphic_value<bg::TypedTimeSeriesValueOutputView<float>>::make<
        bg::TypedTimeSeriesValueOutputView<float>>();
    view->bind(last_updated, &value, evaluation_time);
    auto output = bg::details::output_factory::make<bg::TS<float>>(view.get());

    output.set(1.25F);

    REQUIRE(output.valid());
    CHECK(output.last_updated() == evaluation_time);
    CHECK(output.value() == 1.25F);
}

TEST_CASE("invalidating a value output retains its storage", "[node_api]") {
    bg::EvaluationTime last_updated = bg::MIN_DT + std::chrono::microseconds{1};
    float value = 1.25F;
    const bg::EvaluationTime evaluation_time = bg::MIN_DT + std::chrono::microseconds{2};
    auto view = bg::util::polymorphic_value<bg::TypedTimeSeriesValueOutputView<float>>::make<
        bg::TypedTimeSeriesValueOutputView<float>>();
    view->bind(last_updated, &value, evaluation_time);
    auto output = bg::details::output_factory::make<bg::TS<float>>(view.get());

    output.invalidate();

    CHECK_FALSE(output.valid());
    CHECK(last_updated == bg::MIN_DT);
    CHECK(value == 1.25F);
}

TEST_CASE("a value output accepts a compatible scalar view", "[node_api]") {
    bg::EvaluationTime last_updated = bg::MIN_DT;
    float value{};
    const bg::EvaluationTime evaluation_time = bg::MIN_DT + std::chrono::microseconds{1};
    auto view = bg::util::polymorphic_value<bg::TypedTimeSeriesValueOutputView<float>>::make<
        bg::TypedTimeSeriesValueOutputView<float>>();
    view->bind(last_updated, &value, evaluation_time);
    auto output = bg::details::output_factory::make<bg::TS<float>>(view.get());
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
    auto view = bg::util::polymorphic_value<bg::TypedTimeSeriesValueOutputView<float>>::make<
        bg::TypedTimeSeriesValueOutputView<float>>();
    view->bind(last_updated, &value, evaluation_time);
    auto output = bg::details::output_factory::make<bg::TS<float>>(view.get());
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
    auto view = bg::util::polymorphic_value<bg::TimeSeriesOutputView>::copy(*prototype);
    view->bind(last_updated, &value, evaluation_time);
    const auto output = bg::details::output_factory::make<bg::TS<bg::SCALAR>>(
        static_cast<bg::TimeSeriesValueOutputView &>(view.get()));

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
    auto view = bg::util::polymorphic_value<bg::TimeSeriesOutputView>::copy(*prototype);
    view->bind(last_updated, &value, evaluation_time);
    const auto output = bg::details::output_factory::make<bg::TS<bg::SCALAR>>(
        static_cast<bg::TimeSeriesValueOutputView &>(view.get()));

    CHECK_THROWS_AS(output.set_value(source), std::invalid_argument);
    CHECK_FALSE(output.valid());
}
