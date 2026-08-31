#include <bg/time_series/time_series_view.hpp>
#include <bg/time_series/tsd/tsd_input_view.hpp>
#include <bg/time_series/tsd/tsd_storage_representation.hpp>
#include <bg/time_series/tss/tss_input_view.hpp>
#include <bg/time_series/tss/tss_storage_representation.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <type_traits>

static_assert(std::is_trivially_copyable_v<bg::TimeSeriesView>);

TEST_CASE("a time-series view reads validity and update time", "[time_series]") {
    class test_view final : public bg::TimeSeriesView {
      public:
        void bind(const bg::EvaluationTime &last_updated, const void *value) noexcept {
            TimeSeriesView::bind(last_updated, value);
        }
    };

    test_view view;

    CHECK_FALSE(view.valid());

    const bg::EvaluationTime updated = bg::MIN_DT + std::chrono::microseconds{1};
    const float value = 1.25F;
    view.bind(updated, &value);

    CHECK(view.valid());
    CHECK(view.last_updated() == updated);

    view.bind(bg::MIN_DT, nullptr);

    CHECK_FALSE(view.valid());
}

TEST_CASE("time series collection views support generic visitor dispatch", "[time_series][visitor]") {
    const auto &set = bg::cpp_time_series_storage_representation<bg::TSS<std::int64_t>>().input_view_prototype();
    const auto &dict =
        bg::cpp_time_series_storage_representation<bg::TSD<std::int64_t, bg::TS<int>>>().input_view_prototype();
    auto set_visited = false;
    auto dict_visited = false;

    set.visit([&](const bg::TimeSeriesSetInputView *) { set_visited = true; }, ddv::noop);
    dict.visit([&](const bg::TimeSeriesDictionaryInputView *) { dict_visited = true; }, ddv::noop);

    CHECK(set_visited);
    CHECK(dict_visited);
}
