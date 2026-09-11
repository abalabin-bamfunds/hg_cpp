#include <bg/node/input.hpp>
#include <bg/scalar/cpp_runtime_type.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <memory_resource>
#include <string>
#include <string_view>
#include <type_traits>

namespace {

using namespace bg::literals;

using FloatInput = bg::In<bg::TS<float>, "value"_n>;
using UnnamedFloatInput = bg::In<bg::TS<float>>;
using PassiveOptionalFloatInput = bg::In<bg::TS<float>, bg::optional, "value"_n, bg::passive>;
using FinalFloatInput = bg::In<bg::TS<float, bg::TimeSeriesGuarantee::final>>;

template <typename T>
concept HasArrowOperator = requires(const T &value) { value.operator->(); };

static_assert(sizeof(FloatInput) == 3 * sizeof(void *));
static_assert(std::is_trivially_copyable_v<FloatInput>);
static_assert(FloatInput::name == "value");
static_assert(UnnamedFloatInput::name.empty());
static_assert(FloatInput::activity == bg::active);
static_assert(FloatInput::validity == bg::required);
static_assert(PassiveOptionalFloatInput::activity == bg::passive);
static_assert(PassiveOptionalFloatInput::validity == bg::optional);
static_assert(FinalFloatInput::time_series_type::guarantee == bg::TimeSeriesGuarantee::final);
static_assert(!HasArrowOperator<FloatInput>);

} // namespace

TEST_CASE("a typed input reads its time-series value", "[node_api][feature:node-api]") {
    const bg::EvaluationTime updated = bg::MIN_DT + std::chrono::microseconds{1};
    const float value = 1.25F;
    const FloatInput input{updated, &value};

    REQUIRE(input.valid());
    CHECK(input.last_updated() == updated);
    CHECK(input.value() == value);
    CHECK(*input == value);
    const auto read_generic = [](const FloatInput &view) { return view.valid(); };
    CHECK(read_generic(input));

    const auto value_view = input.value_view();
    CHECK(&value_view.type() == &bg::float_type());
    CHECK(*value_view == 1.25F);

    const auto generic_input = bg::In<bg::TS<bg::SCALAR>>{updated, &value, input.runtime_type()};
    const auto generic_value = generic_input.value_view();
    CHECK(&generic_value.type() == &bg::float_type());

    const auto visited_valid = input.visit([](const bg::In<bg::TS<bg::SCALAR>> *view) { return view->valid(); },
                                           [](const auto *) { return false; });

    REQUIRE(visited_valid);
    CHECK(*visited_valid);
}

TEST_CASE("an input reports whether its visible value was modified",
          "[node_api][feature:node-api][feature:time-series-view]") {
    const auto updated = bg::MIN_DT + std::chrono::microseconds{1};
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
    const auto updated = bg::MIN_DT + std::chrono::microseconds{1};
    const auto value = std::pmr::string{"value"};
    const auto input = bg::In<bg::TS<bg::string>>{updated, &value};

    STATIC_CHECK(HasArrowOperator<decltype(input)>);
    STATIC_CHECK(std::same_as<decltype(*input), std::string_view>);
    CHECK(*input == value);
    CHECK(input->size() == value.size());
    CHECK(input->starts_with("val"));
}

TEST_CASE("a default typed input is invalid", "[node_api][feature:node-api]") {
    const FloatInput input;

    CHECK_FALSE(input.valid());
}

TEST_CASE("a generic value input returns an erased scalar value view",
          "[node_api][feature:node-api][declaration:generic]") {
    using Input = bg::In<bg::TS<bg::SCALAR>>;
    const auto &runtime_type = bg::cpp_time_series_runtime_type<bg::TS<float>>();
    const auto updated = bg::MIN_DT + std::chrono::microseconds{1};
    const auto value = 1.25F;
    const auto input = Input{updated, &value, runtime_type};
    const auto value_view = input.value_view();

    STATIC_CHECK(std::same_as<std::remove_cv_t<decltype(value_view)>, bg::Scalar<bg::SCALAR>>);
    CHECK(&value_view.type() == &bg::float_type());
}

TEST_CASE("an arbitrary time-series input provides its category view", "[node_api][feature:node-api]") {
    using Input = bg::In<bg::TS_T>;
    const auto &runtime_type = bg::cpp_time_series_runtime_type<bg::TS<float>>();
    const auto updated = bg::MIN_DT + std::chrono::microseconds{1};
    const auto value = 1.25F;
    const auto input = Input{updated, &value, runtime_type};
    const auto value_view =
        input.visit([](const bg::In<bg::TS<bg::SCALAR>> *value_input) { return value_input->value_view(); }, ddv::noop);

    REQUIRE(value_view);
    CHECK(&value_view->type() == &bg::float_type());
}
