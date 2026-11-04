#include <hg/graph.hpp>
#include <hg/library.hpp>
#include <hg/run_graph.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <limits>
#include <stdexcept>

using namespace hg;
using namespace hg::util::literals;

namespace {

date_time_t date_time(int year, unsigned month, unsigned day) {
    return std::chrono::time_point_cast<timedelta_t>(
        std::chrono::sys_days{std::chrono::year{year} / std::chrono::month{month} / std::chrono::day{day}});
}

} // namespace

TEST_CASE("date_time fields extract calendar and clock components",
          "[feature:operator][series:value][scalar:evaluation-time]") {
    const auto year = [](wp<TS<date_time_t>> value) {
        return operators::getattr(value, "year"_sym).as<TS<i64>>();
    };
    const auto month = [](wp<TS<date_time_t>> value) {
        return operators::getattr(value, "month"_sym).as<TS<i64>>();
    };
    const auto day = [](wp<TS<date_time_t>> value) {
        return operators::getattr(value, "day"_sym).as<TS<i64>>();
    };
    const auto hour = [](wp<TS<date_time_t>> value) {
        return operators::getattr(value, "hour"_sym).as<TS<i64>>();
    };
    const auto minute = [](wp<TS<date_time_t>> value) {
        return operators::getattr(value, "minute"_sym).as<TS<i64>>();
    };
    const auto second = [](wp<TS<date_time_t>> value) {
        return operators::getattr(value, "second"_sym).as<TS<i64>>();
    };
    const auto microsecond = [](wp<TS<date_time_t>> value) {
        return operators::getattr(value, "microsecond"_sym).as<TS<i64>>();
    };
    const auto weekday = [](wp<TS<date_time_t>> value) {
        return operators::getattr(value, "weekday"_sym).as<TS<i64>>();
    };
    const auto isoweekday = [](wp<TS<date_time_t>> value) {
        return operators::getattr(value, "isoweekday"_sym).as<TS<i64>>();
    };

    const auto leap_day = date_time(2024, 2, 29) + std::chrono::hours{23} + std::chrono::minutes{58} +
                          std::chrono::seconds{57} + std::chrono::microseconds{123456};
    const auto timestamps = std::array{leap_day, date_time_t{timedelta_t{-1}}};

    CHECK(hg::run_graph(year, timestamps).equals<i64>({2024, 1969}));
    CHECK(hg::run_graph(month, timestamps).equals<i64>({2, 12}));
    CHECK(hg::run_graph(day, timestamps).equals<i64>({29, 31}));
    CHECK(hg::run_graph(hour, timestamps).equals<i64>({23, 23}));
    CHECK(hg::run_graph(minute, timestamps).equals<i64>({58, 59}));
    CHECK(hg::run_graph(second, timestamps).equals<i64>({57, 59}));
    CHECK(hg::run_graph(microsecond, timestamps).equals<i64>({123456, 999999}));
    CHECK(hg::run_graph(weekday, timestamps).equals<i64>({3, 2}));
    CHECK(hg::run_graph(isoweekday, timestamps).equals<i64>({4, 3}));
    CHECK(hg::run_graph(year, {leap_day, literal::INVALID, date_time(1969, 12, 31)})
              .equals<i64>({2024, std::nullopt, 1969}));
}

TEST_CASE("date_time calendar fields reject years outside the chrono calendar range",
          "[feature:operator][feature:wiring][series:value][scalar:evaluation-time]") {
    const auto year = [](wp<TS<date_time_t>> value) {
        return operators::getattr(value, "year"_sym).as<TS<i64>>();
    };
    const auto outside_calendar = date_time_t{timedelta_t{std::numeric_limits<i64>::max()}};

    CHECK_THROWS_AS(hg::run_graph(year, {outside_calendar}), std::out_of_range);
}

TEST_CASE("date_time clock fields are safe at the i64 microsecond limits",
          "[feature:operator][series:value][scalar:evaluation-time]") {
    const auto hour = [](wp<TS<date_time_t>> value) {
        return operators::getattr(value, "hour"_sym).as<TS<i64>>();
    };
    const auto minute = [](wp<TS<date_time_t>> value) {
        return operators::getattr(value, "minute"_sym).as<TS<i64>>();
    };
    const auto second = [](wp<TS<date_time_t>> value) {
        return operators::getattr(value, "second"_sym).as<TS<i64>>();
    };
    const auto microsecond = [](wp<TS<date_time_t>> value) {
        return operators::getattr(value, "microsecond"_sym).as<TS<i64>>();
    };
    const auto timestamps = std::array{date_time_t{timedelta_t::min()}, date_time_t{timedelta_t::max()}};

    CHECK(hg::run_graph(hour, timestamps).equals<i64>({19, 4}));
    CHECK(hg::run_graph(minute, timestamps).equals<i64>({59, 0}));
    CHECK(hg::run_graph(second, timestamps).equals<i64>({5, 54}));
    CHECK(hg::run_graph(microsecond, timestamps).equals<i64>({224'192, 775'807}));
}

TEST_CASE("timedelta fields use normalized components for negative values",
          "[feature:operator][series:value][scalar:timedelta]") {
    const auto days = [](wp<TS<timedelta_t>> value) {
        return operators::getattr(value, "days"_sym).as<TS<i64>>();
    };
    const auto seconds = [](wp<TS<timedelta_t>> value) {
        return operators::getattr(value, "seconds"_sym).as<TS<i64>>();
    };
    const auto microseconds = [](wp<TS<timedelta_t>> value) {
        return operators::getattr(value, "microseconds"_sym).as<TS<i64>>();
    };
    const auto total_seconds = [](wp<TS<timedelta_t>> value) {
        return operators::getattr(value, "total_seconds"_sym).as<TS<double>>();
    };

    const auto durations = std::array{timedelta_t{-1}, timedelta_t{}, timedelta_t{86'400'000'001}};

    CHECK(hg::run_graph(days, durations).equals<i64>({-1, 0, 1}));
    CHECK(hg::run_graph(seconds, durations).equals<i64>({86'399, 0, 0}));
    CHECK(hg::run_graph(microseconds, durations).equals<i64>({999'999, 0, 1}));
    CHECK(hg::run_graph(total_seconds, durations).equals<double>({-0.000001, 0.0, 86'400.000001}));
}

TEST_CASE("timedelta fields handle the full i64 microsecond range",
          "[feature:operator][series:value][scalar:timedelta]") {
    const auto days = [](wp<TS<timedelta_t>> value) {
        return operators::getattr(value, "days"_sym).as<TS<i64>>();
    };
    const auto seconds = [](wp<TS<timedelta_t>> value) {
        return operators::getattr(value, "seconds"_sym).as<TS<i64>>();
    };
    const auto microseconds = [](wp<TS<timedelta_t>> value) {
        return operators::getattr(value, "microseconds"_sym).as<TS<i64>>();
    };

    constexpr auto minimum = std::numeric_limits<i64>::min();
    constexpr auto maximum = std::numeric_limits<i64>::max();
    const auto values = std::array{timedelta_t{minimum}, timedelta_t{maximum}};
    CHECK(hg::run_graph(days, values).equals<i64>({-106'751'992, 106'751'991}));
    CHECK(hg::run_graph(seconds, values).equals<i64>({71'945, 14'454}));
    CHECK(hg::run_graph(microseconds, values).equals<i64>({224'192, 775'807}));
}

TEST_CASE("date_time and timedelta getattr reject unknown fields during wiring",
          "[feature:operator][feature:wiring][scalar:evaluation-time][scalar:timedelta]") {
    const auto unknown_date_time_field = [](wp<TS<date_time_t>> value) {
        return operators::getattr(value, "timezone"_sym).as<TS<i64>>();
    };
    const auto unknown_timedelta_field = [](wp<TS<timedelta_t>> value) {
        return operators::getattr(value, "weeks"_sym).as<TS<i64>>();
    };

    CHECK_THROWS_AS(hg::run_graph(unknown_date_time_field, {date_time(2024, 1, 1)}), WiringError);
    CHECK_THROWS_AS(hg::run_graph(unknown_timedelta_field, {timedelta_t{86'400'000'000}}), WiringError);
}
