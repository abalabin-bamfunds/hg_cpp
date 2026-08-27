#include <bg/scalar/value.hpp>
#include <bg/util/polymorphic_value.hpp>

#include <catch2/catch_test_macros.hpp>

#include <type_traits>

namespace {

using FloatView = bg::details::BorrowedScalarValue<float>;
using GenericView = bg::util::polymorphic_value<bg::ScalarValueInputView>;

static_assert(sizeof(FloatView) == sizeof(bg::ScalarValueInputView));
static_assert(std::is_trivially_copyable_v<GenericView>);

} // namespace

TEST_CASE("a scalar input view reads a typed value", "[scalar]") {
    const float value = 1.25F;
    FloatView view;
    view.bind(&value);

    CHECK(view.data() == &value);
    CHECK(view.value() == value);
}

TEST_CASE("a scalar input view supports constant-size polymorphism", "[scalar]") {
    const auto prototype = GenericView::make<FloatView>();
    const float value = 1.25F;

    auto view = prototype;
    view->bind(&value);

    CHECK(view->data() == &value);
    const auto visited = view->visit([](const bg::ScalarValueInputView *input) { return input->data(); });

    REQUIRE(visited);
    CHECK(*visited == &value);
}
