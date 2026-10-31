#include <bg/node/output.hpp>
#include <bg/time_series/ref/ref_storage.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <type_traits>

using namespace bg;

namespace {

using FloatOutput = Out<TS<float>>;

static_assert(sizeof(FloatOutput) == sizeof(void *));
static_assert(sizeof(Out<TS_T>) == sizeof(void *));
static_assert(sizeof(Out<TS<SCALAR>>) == sizeof(void *));
static_assert(sizeof(Out<REF<TS<i64>>>) == sizeof(void *));
static_assert(sizeof(Out<TSL<TS<i64>, 2>>) == sizeof(void *));
static_assert(sizeof(Out<TSS<i64>>) == sizeof(void *));
static_assert(sizeof(Out<TSD<i64, TS<i64>>>) == sizeof(void *));
static_assert(!std::is_constructible_v<FloatOutput, details::OutputState &>);
static_assert(!std::is_copy_constructible_v<FloatOutput>);
static_assert(std::is_move_constructible_v<FloatOutput>);

template <typename Output>
concept ScalarWritableOutput = requires(const Output &output, const Scalar<> &value) { output.set_value(value); };

static_assert(!ScalarWritableOutput<Out<TS_T>>);
static_assert(ScalarWritableOutput<Out<TS<SCALAR>>>);

} // namespace

TEST_CASE("a value output publishes at the evaluation time", "[node_api][feature:node-api]") {
    TimeSeriesState last_updated;
    float value{};
    const EvaluationTime evaluation_time = MIN_DT + std::chrono::microseconds{1};
    auto state = details::OutputState{cpp_time_series_runtime_type<TS<float>>()};
    state.bind(last_updated, &value, evaluation_time);
    auto output = details::output_factory::make<Out<TS<float>>>(state);

    output.set(1.25F);

    REQUIRE(output.valid());
    CHECK(output.last_updated() == evaluation_time);
    CHECK(output.value() == 1.25F);
}

TEST_CASE("an erased output applies a root INVALID marker through its runtime type",
          "[node_api][feature:node-api][declaration:generic][transition:invalidation]") {
    TimeSeriesState last_updated;
    i64 value{};
    const auto evaluation_time = MIN_DT + std::chrono::microseconds{1};
    auto state = details::OutputState{cpp_time_series_runtime_type<TS<i64>>()};
    state.bind(last_updated, &value, evaluation_time);
    auto typed_output = details::output_factory::make<Out<TS<i64>>>(state);
    typed_output.set(5);
    const auto erased_output = Out<TS_T>{TimeSeriesOutputBinding{state}};

    erased_output.apply(literal::INVALID);

    CHECK_FALSE(erased_output.valid());
    CHECK(last_updated.became_invalid());
    CHECK(last_updated.time() == evaluation_time);
    CHECK(value == 5);
}

TEST_CASE("invalidating a value output retains its storage", "[node_api][feature:node-api]") {
    TimeSeriesState last_updated{MIN_DT + std::chrono::microseconds{1}};
    float value = 1.25F;
    const EvaluationTime evaluation_time = MIN_DT + std::chrono::microseconds{2};
    auto state = details::OutputState{cpp_time_series_runtime_type<TS<float>>()};
    state.bind(last_updated, &value, evaluation_time);
    auto output = details::output_factory::make<Out<TS<float>>>(state);

    output.invalidate();

    CHECK_FALSE(output.valid());
    CHECK(last_updated.became_invalid());
    CHECK(last_updated.time() == evaluation_time);
    CHECK(value == 1.25F);
}

TEST_CASE("a value output accepts a compatible scalar view", "[node_api][feature:node-api]") {
    TimeSeriesState last_updated;
    float value{};
    const EvaluationTime evaluation_time = MIN_DT + std::chrono::microseconds{1};
    auto state = details::OutputState{cpp_time_series_runtime_type<TS<float>>()};
    state.bind(last_updated, &value, evaluation_time);
    auto output = details::output_factory::make<Out<TS<float>>>(state);
    const auto source_value = 1.25F;
    const auto source = Scalar<SCALAR>::from<float>(source_value);

    output.set_value(source);

    CHECK(output.value() == 1.25F);
    CHECK(output.last_updated() == evaluation_time);
}

TEST_CASE("a value output rejects an incompatible scalar view", "[node_api][feature:node-api]") {
    TimeSeriesState last_updated;
    float value{};
    const EvaluationTime evaluation_time = MIN_DT + std::chrono::microseconds{1};
    auto state = details::OutputState{cpp_time_series_runtime_type<TS<float>>()};
    state.bind(last_updated, &value, evaluation_time);
    auto output = details::output_factory::make<Out<TS<float>>>(state);
    const auto source_value = 1.25;
    const auto source = Scalar<double>::from(source_value);

    CHECK_THROWS_AS(output.set_value(source), std::invalid_argument);
    CHECK_FALSE(output.valid());
}

TEST_CASE("a generic value output accepts a compatible erased scalar view",
          "[node_api][feature:node-api][declaration:generic]") {
    TimeSeriesState last_updated;
    float value{};
    const auto evaluation_time = MIN_DT + std::chrono::microseconds{1};
    const auto source_value = 1.25F;
    const auto source = Scalar<SCALAR>::from<float>(source_value);
    auto state = details::OutputState{cpp_time_series_runtime_type<TS<float>>()};
    state.bind(last_updated, &value, evaluation_time);
    const auto output = details::output_factory::make<Out<TS<SCALAR>>>(state);

    output.set_value(source);

    CHECK(value == 1.25F);
    CHECK(last_updated.time() == evaluation_time);
}

TEST_CASE("a generic value output rejects an incompatible scalar view",
          "[node_api][feature:node-api][declaration:generic]") {
    TimeSeriesState last_updated;
    float value{};
    const auto evaluation_time = MIN_DT + std::chrono::microseconds{1};
    const auto source_value = 1.25;
    const auto source = Scalar<SCALAR>::from<double>(source_value);
    auto state = details::OutputState{cpp_time_series_runtime_type<TS<float>>()};
    state.bind(last_updated, &value, evaluation_time);
    const auto output = details::output_factory::make<Out<TS<SCALAR>>>(state);

    CHECK_THROWS_AS(output.set_value(source), std::invalid_argument);
    CHECK_FALSE(output.valid());
}

TEST_CASE("a deduplicated value output suppresses an equal update",
          "[node_api][feature:node-api][transition:update][series:value][scalar:floating-point]"
          "[guarantee:deduplicated]") {
    const auto previous_update = MIN_DT + std::chrono::microseconds{1};
    auto last_updated = TimeSeriesState{previous_update};
    const auto evaluation_time = previous_update + std::chrono::microseconds{1};
    auto value = 1.25F;
    auto state = details::OutputState{
        cpp_time_series_runtime_type<TS<float, TimeSeriesGuarantee::deduplicated>>()};
    state.bind(last_updated, &value, evaluation_time);
    const auto output = details::output_factory::make<Out<TS<float>>>(state);

    output.set(1.25F);

    CHECK(last_updated.time() == previous_update);
}

TEST_CASE("a deduplicated value output publishes revalidation",
          "[node_api][feature:node-api][transition:revalidation][series:value]"
          "[scalar:floating-point][guarantee:deduplicated]") {
    auto last_updated = TimeSeriesState{};
    const auto evaluation_time = MIN_DT + std::chrono::microseconds{2};
    auto value = 1.25F;
    auto state = details::OutputState{
        cpp_time_series_runtime_type<TS<float, TimeSeriesGuarantee::deduplicated>>()};
    state.bind(last_updated, &value, evaluation_time);
    const auto output = details::output_factory::make<Out<TS<float>>>(state);

    output.set(1.25F);

    CHECK(last_updated.became_valid());
    CHECK(last_updated.time() == evaluation_time);
}

TEST_CASE("a generic deduplicated value output compares through its scalar runtime type",
          "[node_api][feature:node-api][declaration:generic][transition:update][series:value]"
          "[scalar:floating-point][guarantee:deduplicated]") {
    const auto previous_update = MIN_DT + std::chrono::microseconds{1};
    auto last_updated = TimeSeriesState{previous_update};
    const auto evaluation_time = previous_update + std::chrono::microseconds{1};
    auto value = 1.25F;
    const auto source = Scalar<SCALAR>::from<float>(value);
    auto state = details::OutputState{
        cpp_time_series_runtime_type<TS<float, TimeSeriesGuarantee::deduplicated>>()};
    state.bind(last_updated, &value, evaluation_time);
    const auto output = details::output_factory::make<Out<TS<SCALAR>>>(state);

    output.set_value(source);

    CHECK(last_updated.time() == previous_update);
}

TEST_CASE("a default reference output suppresses an equal address",
          "[node_api][reference][feature:node-api][transition:update][series:value]"
          "[guarantee:deduplicated]") {
    auto root = details::RootReference{};
    auto address = Reference{&root};
    const auto previous_update = MIN_DT + std::chrono::microseconds{1};
    auto last_updated = TimeSeriesState{previous_update};
    const auto evaluation_time = previous_update + std::chrono::microseconds{1};
    auto state = details::OutputState{cpp_time_series_runtime_type<
        REF<TS<i64>>, TimeSeriesTypeRole::output>()};
    state.bind(last_updated, &address, evaluation_time);
    const auto output = details::output_factory::make<Out<REF<TS<i64>>>>(state);

    output.set_value(address);

    CHECK(last_updated.time() == previous_update);
}

TEST_CASE("an ordinary reference output publishes an equal address",
          "[node_api][reference][feature:node-api][transition:update][series:value][guarantee:ordinary]") {
    using OrdinaryReference = REF<TS<i64>, TimeSeriesGuarantee::ordinary>;

    auto root = details::RootReference{};
    auto address = Reference{&root};
    const auto previous_update = MIN_DT + std::chrono::microseconds{1};
    auto last_updated = TimeSeriesState{previous_update};
    const auto evaluation_time = previous_update + std::chrono::microseconds{1};
    auto state = details::OutputState{cpp_time_series_runtime_type<OrdinaryReference>()};
    state.bind(last_updated, &address, evaluation_time);
    const auto output = details::output_factory::make<Out<OrdinaryReference>>(state);

    output.set_value(address);

    CHECK(last_updated.time() == evaluation_time);
}
