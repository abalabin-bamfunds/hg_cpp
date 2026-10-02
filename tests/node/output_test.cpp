#include <bg/node/output.hpp>
#include <bg/time_series/ref/ref_storage.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <type_traits>

namespace {

using FloatOutput = bg::Out<bg::TS<float>>;

static_assert(sizeof(FloatOutput) == sizeof(void *));
static_assert(sizeof(bg::Out<bg::TS_T>) == sizeof(void *));
static_assert(sizeof(bg::Out<bg::TS<bg::SCALAR>>) == sizeof(void *));
static_assert(sizeof(bg::Out<bg::REF<bg::TS<bg::i64>>>) == sizeof(void *));
static_assert(sizeof(bg::Out<bg::TSL<bg::TS<bg::i64>, 2>>) == sizeof(void *));
static_assert(sizeof(bg::Out<bg::TSS<bg::i64>>) == sizeof(void *));
static_assert(sizeof(bg::Out<bg::TSD<bg::i64, bg::TS<bg::i64>>>) == sizeof(void *));
static_assert(!std::is_constructible_v<FloatOutput, bg::details::OutputState &>);
static_assert(!std::is_copy_constructible_v<FloatOutput>);
static_assert(std::is_move_constructible_v<FloatOutput>);

template <typename Output>
concept ScalarWritableOutput = requires(const Output &output, const bg::Scalar<> &value) { output.set_value(value); };

static_assert(!ScalarWritableOutput<bg::Out<bg::TS_T>>);
static_assert(ScalarWritableOutput<bg::Out<bg::TS<bg::SCALAR>>>);

} // namespace

TEST_CASE("a value output publishes at the evaluation time", "[node_api][feature:node-api]") {
    bg::TimeSeriesState last_updated;
    float value{};
    const bg::EvaluationTime evaluation_time = bg::MIN_DT + std::chrono::microseconds{1};
    auto state = bg::details::OutputState{bg::cpp_time_series_runtime_type<bg::TS<float>>()};
    state.bind(last_updated, &value, evaluation_time);
    auto output = bg::details::output_factory::make<bg::TS<float>>(state);

    output.set(1.25F);

    REQUIRE(output.valid());
    CHECK(output.last_updated() == evaluation_time);
    CHECK(output.value() == 1.25F);
}

TEST_CASE("invalidating a value output retains its storage", "[node_api][feature:node-api]") {
    bg::TimeSeriesState last_updated{bg::MIN_DT + std::chrono::microseconds{1}};
    float value = 1.25F;
    const bg::EvaluationTime evaluation_time = bg::MIN_DT + std::chrono::microseconds{2};
    auto state = bg::details::OutputState{bg::cpp_time_series_runtime_type<bg::TS<float>>()};
    state.bind(last_updated, &value, evaluation_time);
    auto output = bg::details::output_factory::make<bg::TS<float>>(state);

    output.invalidate();

    CHECK_FALSE(output.valid());
    CHECK(last_updated.became_invalid());
    CHECK(last_updated.time() == evaluation_time);
    CHECK(value == 1.25F);
}

TEST_CASE("a value output accepts a compatible scalar view", "[node_api][feature:node-api]") {
    bg::TimeSeriesState last_updated;
    float value{};
    const bg::EvaluationTime evaluation_time = bg::MIN_DT + std::chrono::microseconds{1};
    auto state = bg::details::OutputState{bg::cpp_time_series_runtime_type<bg::TS<float>>()};
    state.bind(last_updated, &value, evaluation_time);
    auto output = bg::details::output_factory::make<bg::TS<float>>(state);
    const auto source_value = 1.25F;
    const auto source = bg::Scalar<bg::SCALAR>::from<float>(source_value);

    output.set_value(source);

    CHECK(output.value() == 1.25F);
    CHECK(output.last_updated() == evaluation_time);
}

TEST_CASE("a value output rejects an incompatible scalar view", "[node_api][feature:node-api]") {
    bg::TimeSeriesState last_updated;
    float value{};
    const bg::EvaluationTime evaluation_time = bg::MIN_DT + std::chrono::microseconds{1};
    auto state = bg::details::OutputState{bg::cpp_time_series_runtime_type<bg::TS<float>>()};
    state.bind(last_updated, &value, evaluation_time);
    auto output = bg::details::output_factory::make<bg::TS<float>>(state);
    const auto source_value = 1.25;
    const auto source = bg::Scalar<double>::from(source_value);

    CHECK_THROWS_AS(output.set_value(source), std::invalid_argument);
    CHECK_FALSE(output.valid());
}

TEST_CASE("a generic value output accepts a compatible erased scalar view",
          "[node_api][feature:node-api][declaration:generic]") {
    bg::TimeSeriesState last_updated;
    float value{};
    const auto evaluation_time = bg::MIN_DT + std::chrono::microseconds{1};
    const auto source_value = 1.25F;
    const auto source = bg::Scalar<bg::SCALAR>::from<float>(source_value);
    auto state = bg::details::OutputState{bg::cpp_time_series_runtime_type<bg::TS<float>>()};
    state.bind(last_updated, &value, evaluation_time);
    const auto output = bg::details::output_factory::make<bg::TS<bg::SCALAR>>(state);

    output.set_value(source);

    CHECK(value == 1.25F);
    CHECK(last_updated.time() == evaluation_time);
}

TEST_CASE("a generic value output rejects an incompatible scalar view",
          "[node_api][feature:node-api][declaration:generic]") {
    bg::TimeSeriesState last_updated;
    float value{};
    const auto evaluation_time = bg::MIN_DT + std::chrono::microseconds{1};
    const auto source_value = 1.25;
    const auto source = bg::Scalar<bg::SCALAR>::from<double>(source_value);
    auto state = bg::details::OutputState{bg::cpp_time_series_runtime_type<bg::TS<float>>()};
    state.bind(last_updated, &value, evaluation_time);
    const auto output = bg::details::output_factory::make<bg::TS<bg::SCALAR>>(state);

    CHECK_THROWS_AS(output.set_value(source), std::invalid_argument);
    CHECK_FALSE(output.valid());
}

TEST_CASE("a deduplicated value output suppresses an equal update",
          "[node_api][feature:node-api][transition:update][series:value][scalar:floating-point]"
          "[guarantee:deduplicated]") {
    const auto previous_update = bg::MIN_DT + std::chrono::microseconds{1};
    auto last_updated = bg::TimeSeriesState{previous_update};
    const auto evaluation_time = previous_update + std::chrono::microseconds{1};
    auto value = 1.25F;
    auto state = bg::details::OutputState{
        bg::cpp_time_series_runtime_type<bg::TS<float, bg::TimeSeriesGuarantee::deduplicated>>()};
    state.bind(last_updated, &value, evaluation_time);
    const auto output = bg::details::output_factory::make<bg::TS<float>>(state);

    output.set(1.25F);

    CHECK(last_updated.time() == previous_update);
}

TEST_CASE("a deduplicated value output publishes revalidation",
          "[node_api][feature:node-api][transition:revalidation][series:value]"
          "[scalar:floating-point][guarantee:deduplicated]") {
    auto last_updated = bg::TimeSeriesState{};
    const auto evaluation_time = bg::MIN_DT + std::chrono::microseconds{2};
    auto value = 1.25F;
    auto state = bg::details::OutputState{
        bg::cpp_time_series_runtime_type<bg::TS<float, bg::TimeSeriesGuarantee::deduplicated>>()};
    state.bind(last_updated, &value, evaluation_time);
    const auto output = bg::details::output_factory::make<bg::TS<float>>(state);

    output.set(1.25F);

    CHECK(last_updated.became_valid());
    CHECK(last_updated.time() == evaluation_time);
}

TEST_CASE("a generic deduplicated value output compares through its scalar runtime type",
          "[node_api][feature:node-api][declaration:generic][transition:update][series:value]"
          "[scalar:floating-point][guarantee:deduplicated]") {
    const auto previous_update = bg::MIN_DT + std::chrono::microseconds{1};
    auto last_updated = bg::TimeSeriesState{previous_update};
    const auto evaluation_time = previous_update + std::chrono::microseconds{1};
    auto value = 1.25F;
    const auto source = bg::Scalar<bg::SCALAR>::from<float>(value);
    auto state = bg::details::OutputState{
        bg::cpp_time_series_runtime_type<bg::TS<float, bg::TimeSeriesGuarantee::deduplicated>>()};
    state.bind(last_updated, &value, evaluation_time);
    const auto output = bg::details::output_factory::make<bg::TS<bg::SCALAR>>(state);

    output.set_value(source);

    CHECK(last_updated.time() == previous_update);
}

TEST_CASE("a default reference output suppresses an equal address",
          "[node_api][reference][feature:node-api][transition:update][series:value]"
          "[guarantee:deduplicated]") {
    auto root = bg::details::RootReference{};
    auto address = bg::Reference{&root};
    const auto previous_update = bg::MIN_DT + std::chrono::microseconds{1};
    auto last_updated = bg::TimeSeriesState{previous_update};
    const auto evaluation_time = previous_update + std::chrono::microseconds{1};
    auto state = bg::details::OutputState{bg::cpp_time_series_runtime_type<bg::REF<bg::TS<bg::i64>>>()};
    state.bind(last_updated, &address, evaluation_time);
    const auto output = bg::details::output_factory::make<bg::REF<bg::TS<bg::i64>>>(state);

    output.set_value(address);

    CHECK(last_updated.time() == previous_update);
}

TEST_CASE("an ordinary reference output publishes an equal address",
          "[node_api][reference][feature:node-api][transition:update][series:value][guarantee:ordinary]") {
    using OrdinaryReference = bg::REF<bg::TS<bg::i64>, bg::TimeSeriesGuarantee::ordinary>;

    auto root = bg::details::RootReference{};
    auto address = bg::Reference{&root};
    const auto previous_update = bg::MIN_DT + std::chrono::microseconds{1};
    auto last_updated = bg::TimeSeriesState{previous_update};
    const auto evaluation_time = previous_update + std::chrono::microseconds{1};
    auto state = bg::details::OutputState{bg::cpp_time_series_runtime_type<OrdinaryReference>()};
    state.bind(last_updated, &address, evaluation_time);
    const auto output = bg::details::output_factory::make<OrdinaryReference>(state);

    output.set_value(address);

    CHECK(last_updated.time() == evaluation_time);
}
