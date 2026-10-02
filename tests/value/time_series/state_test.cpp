#include <bg/time_series/time_series_state.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>

TEST_CASE("a time series state records validity transitions",
          "[feature:validity][series:generic][transition:update][transition:invalidation]"
          "[transition:revalidation]") {
    const auto first_time = bg::MIN_DT + std::chrono::microseconds{1};
    const auto second_time = first_time + std::chrono::microseconds{1};
    const auto third_time = second_time + std::chrono::microseconds{1};
    const auto fourth_time = third_time + std::chrono::microseconds{1};

    auto state = bg::TimeSeriesState{};
    CHECK_FALSE(state.valid());
    CHECK(state.never_valid());
    CHECK_FALSE(state.became_valid());
    CHECK_FALSE(state.became_invalid());
    CHECK(state.time() == bg::MIN_DT);

    const auto initial_state = bg::TimeSeriesState{bg::MIN_DT};
    CHECK_FALSE(initial_state.valid());
    CHECK(initial_state.never_valid());
    CHECK_FALSE(initial_state.became_valid());
    CHECK_FALSE(initial_state.became_invalid());

    state.publish(first_time);
    CHECK(state.valid());
    CHECK(state.became_valid());
    CHECK(state.time() == first_time);

    state.publish(first_time);
    CHECK(state.became_valid());

    state.publish(second_time);
    CHECK(state.valid());
    CHECK_FALSE(state.became_valid());
    CHECK_FALSE(state.became_invalid());
    CHECK(state.time() == second_time);

    state.invalidate(third_time);
    CHECK_FALSE(state.valid());
    CHECK(state.became_invalid());
    CHECK(state.time() == third_time);

    state.publish(fourth_time);
    CHECK(state.valid());
    CHECK(state.became_valid());
    CHECK(state.time() == fourth_time);

    state.publish(fourth_time);
    CHECK(state.became_valid());

    state.publish(fourth_time + std::chrono::microseconds{1});
    CHECK_FALSE(state.became_valid());
    CHECK_FALSE(state.became_invalid());
}

TEST_CASE("a time series state represents supported timestamps", "[feature:validity][series:generic]") {
    const auto first_time = bg::MIN_DT + std::chrono::microseconds{1};
    const auto largest_time =
        bg::EvaluationTime{bg::EvaluationTime::duration{(std::int64_t{1} << 62) - 1}};
    const auto too_large = largest_time + std::chrono::microseconds{1};

    CHECK_FALSE(bg::TimeSeriesState::can_represent(bg::MIN_DT));
    CHECK(bg::TimeSeriesState::can_represent(first_time));
    CHECK(bg::TimeSeriesState::can_represent(largest_time));
    CHECK_FALSE(bg::TimeSeriesState::can_represent(too_large));
}

TEST_CASE("a time series state orders change times", "[feature:validity][series:generic]") {
    const auto early_time = bg::MIN_DT + std::chrono::microseconds{1};
    const auto time = early_time + std::chrono::microseconds{1};
    const auto later_time = time + std::chrono::microseconds{1};
    const auto largest_time =
        bg::EvaluationTime{bg::EvaluationTime::duration{(std::int64_t{1} << 62) - 1}};

    const auto never_valid = bg::TimeSeriesState{};
    const auto ordinary_early = bg::TimeSeriesState{early_time};
    const auto ordinary = bg::TimeSeriesState{time};
    const auto ordinary_later = bg::TimeSeriesState{later_time};
    const auto largest = bg::TimeSeriesState{largest_time};

    auto became_valid = bg::TimeSeriesState{};
    became_valid.publish(time);

    auto became_invalid = bg::TimeSeriesState{time};
    became_invalid.invalidate(time);

    CHECK(ordinary_early.later_than(never_valid));
    CHECK(ordinary.later_than(ordinary_early));
    CHECK(ordinary_later.later_than(became_valid));
    CHECK(ordinary_later.later_than(became_invalid));
    CHECK(largest.later_than(ordinary_later));

    CHECK_FALSE(ordinary.later_than(became_valid));
    CHECK_FALSE(became_valid.later_than(ordinary));
    CHECK_FALSE(ordinary.later_than(became_invalid));
    CHECK_FALSE(became_invalid.later_than(ordinary));
    CHECK_FALSE(became_valid.later_than(became_invalid));
    CHECK_FALSE(became_invalid.later_than(became_valid));
}
