#include <bg/node/input.hpp>
#include <bg/scalar/cpp_runtime_type.hpp>
#include <bg/scalar/details/string_storage.hpp>
#include <bg/scalar/value.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <memory_resource>
#include <string>
#include <string_view>
#include <type_traits>

using namespace bg;

namespace {

using namespace bg::literals;

using FloatInput = In<TS<float>, "value"_n>;
using UnnamedFloatInput = In<TS<float>>;
using PassiveOptionalFloatInput = In<TS<float>, optional, "value"_n, passive>;
using AllValidFloatInput = In<TS<float>, InputValidity::all_valid>;
using FinalFloatInput = In<TS<float, TimeSeriesGuarantee::final>>;

template <typename T>
concept HasArrowOperator = requires(const T &value) { value.operator->(); };

static_assert(sizeof(FloatInput) == 3 * sizeof(void *));
static_assert(std::is_trivially_copyable_v<FloatInput>);
static_assert(FloatInput::name == "value");
static_assert(UnnamedFloatInput::name.empty());
static_assert(FloatInput::activity == active);
static_assert(FloatInput::validity == required);
static_assert(PassiveOptionalFloatInput::activity == passive);
static_assert(PassiveOptionalFloatInput::validity == optional);
static_assert(AllValidFloatInput::validity == InputValidity::all_valid);
static_assert(FinalFloatInput::time_series_type::guarantee == TimeSeriesGuarantee::final);
static_assert(!HasArrowOperator<FloatInput>);

} // namespace

TEST_CASE("a typed input reads its time-series value", "[node_api][feature:node-api]") {
    const auto updated_time = MIN_DT + std::chrono::microseconds{1};
    auto updated = TimeSeriesState{};
    updated.publish(updated_time);
    const float value = 1.25F;
    const FloatInput input{updated, &value};

    REQUIRE(input.valid());
    CHECK(input.last_updated() == updated_time);
    CHECK(input.value() == value);
    CHECK(*input == value);
    const auto read_generic = [](const FloatInput &view) { return view.valid(); };
    CHECK(read_generic(input));

    const auto value_view = input.value_view();
    CHECK(&value_view.type() == &float_type());
    CHECK(*value_view == 1.25F);

    const auto generic_input = In<TS<SCALAR>>{updated, &value, input.runtime_type()};
    const auto generic_value = generic_input.value_view();
    CHECK(&generic_value.type() == &float_type());

    const auto visited_valid = input.visit([](const In<TS<SCALAR>> *view) { return view->valid(); },
                                           [](const auto *) { return false; });

    REQUIRE(visited_valid);
    CHECK(*visited_valid);
}

TEST_CASE("an input reports whether its visible value was modified",
          "[node_api][feature:node-api][feature:time-series-view]") {
    auto updated = TimeSeriesState{};
    updated.publish(MIN_DT + std::chrono::microseconds{1});
    const auto value = 1.25F;
    const auto modified = FloatInput{updated, &value, true};
    const auto unchanged = FloatInput{updated, &value};

    CHECK(modified.modified());
    CHECK_FALSE(unchanged.modified());

    const auto visited = modified.visit([](const auto *input) { return input->modified(); });
    REQUIRE(visited);
    CHECK(*visited);
}

TEST_CASE("a typed value input provides member access to its value", "[node_api][feature:node-api]") {
    auto updated = TimeSeriesState{};
    updated.publish(MIN_DT + std::chrono::microseconds{1});
    const auto value = ScalarValue<string>{"value"};
    auto *stored = details::StringStorage::from(details::scalar_data(value.view()));
    const auto input = In<TS<string>>{updated, &stored};

    STATIC_CHECK(HasArrowOperator<decltype(input)>);
    STATIC_CHECK(std::same_as<decltype(*input), std::string_view>);
    CHECK(*input == "value");
    CHECK(input->size() == std::string_view{"value"}.size());
    CHECK(input->starts_with("val"));
}

TEST_CASE("a default typed input is invalid", "[node_api][feature:node-api]") {
    const FloatInput input;

    CHECK_FALSE(input.valid());
}

TEST_CASE("a generic value input returns an erased scalar value view",
          "[node_api][feature:node-api][declaration:generic]") {
    using Input = In<TS<SCALAR>>;
    const auto &runtime_type = cpp_time_series_runtime_type<TS<float>>();
    auto updated = TimeSeriesState{};
    updated.publish(MIN_DT + std::chrono::microseconds{1});
    const auto value = 1.25F;
    const auto input = Input{updated, &value, runtime_type};
    const auto value_view = input.value_view();

    STATIC_CHECK(std::same_as<std::remove_cv_t<decltype(value_view)>, Scalar<SCALAR>>);
    CHECK(&value_view.type() == &float_type());
}

TEST_CASE("an arbitrary time-series input provides its category view", "[node_api][feature:node-api]") {
    using Input = In<TS_T>;
    const auto &runtime_type = cpp_time_series_runtime_type<TS<float>>();
    auto updated = TimeSeriesState{};
    updated.publish(MIN_DT + std::chrono::microseconds{1});
    const auto value = 1.25F;
    const auto input = Input{updated, &value, runtime_type};
    const auto value_view =
        input.visit([](const In<TS<SCALAR>> *value_input) { return value_input->value_view(); }, ddv::noop);

    REQUIRE(value_view);
    CHECK(&value_view->type() == &float_type());
}
