#include <bg/node/input.hpp>
#include <bg/scalar/cpp_storage_representation.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <type_traits>

namespace {

using namespace bg::literals;

using FloatInput = bg::In<bg::TS<float>, "value"_n>;
using UnnamedFloatInput = bg::In<bg::TS<float>>;
using PassiveOptionalFloatInput = bg::In<bg::TS<float>, bg::optional, "value"_n, bg::passive>;
using FinalFloatInput = bg::In<bg::TS<float, bg::TimeSeriesGuarantee::final>>;

static_assert(sizeof(FloatInput) == sizeof(bg::TimeSeriesValueInputView));
static_assert(sizeof(FloatInput) == 3 * sizeof(void *));
static_assert(std::is_trivially_copyable_v<FloatInput>);
static_assert(FloatInput::name == "value");
static_assert(UnnamedFloatInput::name.empty());
static_assert(FloatInput::activity == bg::active);
static_assert(FloatInput::validity == bg::required);
static_assert(PassiveOptionalFloatInput::activity == bg::passive);
static_assert(PassiveOptionalFloatInput::validity == bg::optional);
static_assert(FinalFloatInput::time_series_type::guarantee == bg::TimeSeriesGuarantee::final);

} // namespace

TEST_CASE("a typed input reads its time-series value", "[node_api]") {
    const bg::EvaluationTime updated = bg::MIN_DT + std::chrono::microseconds{1};
    const float value = 1.25F;
    const FloatInput input{updated, &value};

    REQUIRE(input.valid());
    CHECK(input.last_updated() == updated);
    CHECK(input.value() == value);
    const auto read_generic = [](const bg::TimeSeriesValueInputView &view) { return view.valid(); };
    CHECK(read_generic(input));
    CHECK(input->valid());
    CHECK(input->last_updated() == updated);

    const auto value_view = input->value_view();
    CHECK(&value_view->type() == &bg::float_type());
    CHECK(value_view->value() == 1.25F);

    const bg::TimeSeriesValueInputView &generic_input = *input.operator->();
    const auto generic_value = generic_input.value_view();
    CHECK(&generic_value->type() == &bg::float_type());

    const bg::TimeSeriesInputView &any_input = generic_input;
    const auto any_value = any_input.value_view();
    CHECK(&any_value->type() == &bg::float_type());

    const auto visited_valid = input->visit([](const bg::TimeSeriesValueInputView *view) { return view->valid(); });

    REQUIRE(visited_valid);
    CHECK(*visited_valid);
}

TEST_CASE("a default typed input is invalid", "[node_api]") {
    const FloatInput input;

    CHECK_FALSE(input.valid());
}

TEST_CASE("a generic value input returns an erased scalar value view", "[node_api]") {
    using Input = bg::In<bg::TS<bg::SCALAR>>;
    const auto *prototype = bg::cpp_scalar_storage_representation<float>().input_view_prototype();
    const auto updated = bg::MIN_DT + std::chrono::microseconds{1};
    const auto value = 1.25F;
    const auto input = Input{updated, &value, *prototype};
    const auto value_view = input.value_view();

    STATIC_CHECK(std::same_as<std::remove_cv_t<decltype(value_view)>, bg::Scalar<bg::SCALAR>>);
    CHECK(&value_view->type() == &bg::float_type());
}

TEST_CASE("an arbitrary time-series input returns an erased scalar view", "[node_api]") {
    using Input = bg::In<bg::TS_T>;
    const auto *prototype = bg::cpp_scalar_storage_representation<float>().input_view_prototype();
    const auto updated = bg::MIN_DT + std::chrono::microseconds{1};
    const auto value = 1.25F;
    const auto input = Input{updated, &value, *prototype};
    const auto value_view = input.value_view();

    STATIC_CHECK(std::same_as<std::remove_cv_t<decltype(value_view)>, bg::Scalar<>>);
    CHECK(&value_view->type() == &bg::float_type());
}
