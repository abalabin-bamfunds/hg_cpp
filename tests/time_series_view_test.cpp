#include <bg/time_series/view.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <type_traits>

namespace {

class TestView : public bg::TimeSeriesView {
  public:
    void bind(const bg::EvaluationTime &last_updated, const void *value) noexcept {
        TimeSeriesView::bind(last_updated, value);
    }
};

} // namespace

static_assert(std::is_trivially_copyable_v<bg::TimeSeriesView>);

TEST_CASE("a time-series view reads validity and update time", "[time_series]") {
    TestView view;

    CHECK_FALSE(view.valid());

    const bg::EvaluationTime updated = bg::MIN_DT + std::chrono::microseconds{1};
    const float value = 1.25F;
    view.bind(updated, &value);

    CHECK(view.valid());
    CHECK(view.last_updated() == updated);

    view.bind(bg::MIN_DT, nullptr);

    CHECK_FALSE(view.valid());
}
