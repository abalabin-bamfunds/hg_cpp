#include <bg/scalar/value.hpp>

#include <catch2/catch_test_macros.hpp>

#include <type_traits>

namespace {

static_assert(sizeof(bg::Scalar<float>) == sizeof(bg::details::ScalarData));
static_assert(std::is_trivially_copyable_v<bg::details::ScalarData>);

} // namespace

TEST_CASE("a scalar input view reads a typed value", "[scalar]") {
    const float value = 1.25F;
    const auto view = bg::Scalar<float>::from(bg::cpp_scalar_runtime_type<float>(), &value);

    CHECK(view.data() == &value);
    CHECK(view.value() == value);
}

TEST_CASE("a scalar input view supports constant-size polymorphism", "[scalar]") {
    const float value = 1.25F;
    const auto view = bg::Scalar<>::from(bg::cpp_scalar_runtime_type<float>(), &value);

    CHECK(view.data() == &value);
    const auto visited = view.visit([](const bg::Scalar<> *input) { return input->data(); }, ddv::noop);

    REQUIRE(visited);
    CHECK(*visited == &value);
}
