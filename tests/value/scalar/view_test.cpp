#include <bg/scalar/value.hpp>

#include <catch2/catch_test_macros.hpp>

#include <concepts>
#include <string_view>
#include <type_traits>

using namespace bg;

namespace {

template <typename T>
concept HasArrowOperator = requires(const T &value) { value.operator->(); };

static_assert(sizeof(Scalar<float>) == sizeof(details::ScalarData));
static_assert(std::is_trivially_copyable_v<details::ScalarData>);
static_assert(!std::convertible_to<const Scalar<float> &, const details::ScalarData &>);
static_assert(!std::convertible_to<const ScalarValue<float> &, const details::ScalarData &>);
static_assert(!HasArrowOperator<Scalar<float>>);
static_assert(HasArrowOperator<Scalar<string>>);

} // namespace

TEST_CASE("a scalar input view reads a typed value", "[scalar][feature:scalar-api]") {
    const float value = 1.25F;
    const auto view = Scalar<float>::from(cpp_scalar_runtime_type<float>(), &value);

    CHECK(view.data() == &value);
    CHECK(view.value() == value);
    CHECK(*view == value);
}

TEST_CASE("a scalar string view provides value member access", "[scalar][feature:scalar-api][scalar:string]") {
    const auto value = ScalarValue{std::string_view{"value"}};
    const auto view = value.view();

    CHECK(view->size() == 5);
    CHECK(view->starts_with("val"));
}

TEST_CASE("a scalar input view supports constant-size polymorphism", "[scalar][feature:scalar-api]") {
    const float value = 1.25F;
    const auto view = Scalar<>::from(cpp_scalar_runtime_type<float>(), &value);

    CHECK(view.data() == &value);
    const auto visited = view.visit([](const Scalar<> *input) { return input->data(); }, ddv::noop);

    REQUIRE(visited);
    CHECK(*visited == &value);
}
