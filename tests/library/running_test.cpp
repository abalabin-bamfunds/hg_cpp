#include <bg/library.hpp>
#include <bg/nested.hpp>
#include <bg/run_graph.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <limits>
#include <optional>

using namespace bg;
using namespace bg::literals;

TEST_CASE("running integer sums include every valid update and wrap at i64 limits",
          "[feature:operator][series:value][scalar:i64][guarantee:ordinary]"
          "[transition:first-tick][transition:update][transition:invalidation][transition:revalidation]") {
    const auto sums = [](wp<TS<i64>> value) { return operators::sums(value); };
    const auto minimum = std::numeric_limits<i64>::min();
    const auto maximum = std::numeric_limits<i64>::max();

    CHECK(bg::run_graph(sums, {literal::SKIP, 1, 1, 0, -1, literal::INVALID,
                                     literal::SKIP, 5})
              .equals<i64>({std::nullopt, 1, 2, 2, 1, std::nullopt, std::nullopt, 5}));
    CHECK(bg::run_graph(sums, {maximum, 1, -1})
              .equals<i64>({maximum, minimum, maximum}));
}

TEST_CASE("running integer sums reset only on a valid reset tick",
          "[feature:operator][series:value][scalar:i64][validity:optional]"
          "[transition:first-tick][transition:update][transition:invalidation][transition:revalidation]") {
    const auto sums = [](wp<TS<i64>> value, wp<TS<bool>> reset) {
        return operators::sums(value, "reset"_n = reset);
    };

    const auto actual = bg::run_graph(
        sums,
        {1, literal::SKIP, 2, literal::SKIP, 3, literal::SKIP, literal::INVALID, 5},
        {literal::SKIP, false, literal::SKIP, true, literal::SKIP, literal::INVALID,
         literal::SKIP, literal::SKIP});
    CHECK(actual.equals<i64>({1, std::nullopt, 2, std::nullopt, 3, std::nullopt, std::nullopt, 5}));
}

TEST_CASE("running integer sums handle reset before data and simultaneous reset and data",
          "[feature:operator][series:value][scalar:i64][validity:optional]"
          "[transition:first-tick][transition:update][transition:invalidation]") {
    const auto sums = [](wp<TS<i64>> value, wp<TS<bool>> reset) {
        return operators::sums(value, "reset"_n = reset);
    };

    CHECK(bg::run_graph(sums,
                              {literal::SKIP, 10, literal::SKIP, 4, 7},
                              {false, literal::SKIP, true, false, literal::SKIP})
              .equals<i64>({std::nullopt, 10, std::nullopt, 4, 11}));
}

TEST_CASE("running sums accept any reset time series and reset on its valid ticks",
          "[feature:operator][series:value][scalar:i64][scalar:string][validity:optional]"
          "[transition:update][transition:invalidation]") {
    const auto sums = [](wp<TS<i64>> value, wp<TS<string>> reset) {
        return operators::sums(value, "reset"_n = reset);
    };

    CHECK(bg::run_graph(sums,
                              {1, 2, 3, 4, literal::INVALID, 5, 6},
                              {literal::SKIP, literal::SKIP, "reset", literal::SKIP,
                               "reset", literal::SKIP, literal::SKIP})
              .equals<i64>({1, 3, 3, 7, std::nullopt, 5, 11}));
}

TEST_CASE("reset invalidation does not clear running sums",
          "[feature:operator][series:value][scalar:i64][scalar:string][validity:optional]"
          "[transition:update][transition:invalidation]") {
    const auto sums = [](wp<TS<i64>> value, wp<TS<string>> reset) {
        return operators::sums(value, "reset"_n = reset);
    };

    CHECK(bg::run_graph(sums,
                              {1, 2, literal::SKIP, 3},
                              {literal::SKIP, "reset", literal::INVALID, literal::SKIP})
              .equals<i64>({1, 2, std::nullopt, 5}));
}

TEST_CASE("a reset-only tick invalidates the running output",
          "[feature:operator][series:value][scalar:i64][validity:optional][transition:invalidation]") {
    const auto observe = [](wp<TS<i64>> value, wp<TS<bool>> reset) {
        return operators::valid(operators::sums(value, "reset"_n = reset));
    };

    CHECK(bg::run_graph(observe,
                              {5, literal::SKIP, literal::SKIP, 3},
                              {literal::SKIP, false, literal::INVALID,
                               literal::SKIP})
              .equals<bool>({true, false, std::nullopt, true}));
}

TEST_CASE("all running aggregates reset on reset-only and simultaneous data ticks",
          "[feature:operator][series:value][scalar:i64][validity:optional]"
          "[transition:update][transition:invalidation]") {
    const auto sums = [](wp<TS<i64>> value, wp<TS<bool>> reset) {
        return operators::sums(value, "reset"_n = reset);
    };

    const auto mins = [](wp<TS<i64>> value, wp<TS<bool>> reset) {
        return operators::mins(value, "reset"_n = reset);
    };

    const auto maxs = [](wp<TS<i64>> value, wp<TS<bool>> reset) {
        return operators::maxs(value, "reset"_n = reset);
    };

    const auto means = [](wp<TS<i64>> value, wp<TS<bool>> reset) {
        return operators::means(value, "reset"_n = reset);
    };
    const auto run = [](const auto &operation) {
        return bg::run_graph(operation,
                                   {5, 4, 6, 8, 3, 9, literal::SKIP, 7},
                                   {literal::SKIP, false, true, literal::SKIP,
                                    false, true, true, literal::SKIP});
    };

    CHECK(run(sums).equals<i64>({5, 4, 6, 14, 3, 9, std::nullopt, 7}));
    CHECK(run(mins).equals<i64>({5, 4, 6, std::nullopt, 3, 9, std::nullopt, 7}));
    CHECK(run(maxs).equals<i64>({5, 4, 6, 8, 3, 9, std::nullopt, 7}));
    CHECK(run(means).equals<double>({5.0, 4.0, 6.0, 7.0, 3.0, 9.0, std::nullopt, 7.0}));
}

TEST_CASE("reset invalidation preserves each running history",
          "[feature:operator][series:value][scalar:i64][scalar:string][validity:optional]"
          "[transition:update][transition:invalidation]") {
    const auto sums = [](wp<TS<i64>> value, wp<TS<string>> reset) {
        return operators::sums(value, "reset"_n = reset);
    };

    const auto mins = [](wp<TS<i64>> value, wp<TS<string>> reset) {
        return operators::mins(value, "reset"_n = reset);
    };

    const auto maxs = [](wp<TS<i64>> value, wp<TS<string>> reset) {
        return operators::maxs(value, "reset"_n = reset);
    };

    const auto means = [](wp<TS<i64>> value, wp<TS<string>> reset) {
        return operators::means(value, "reset"_n = reset);
    };
    const auto run = [](const auto &operation) {
        return bg::run_graph(operation, {5, 4, literal::SKIP, 3},
                                   {literal::SKIP, "reset", literal::INVALID, literal::SKIP});
    };

    CHECK(run(sums).equals<i64>({5, 4, std::nullopt, 7}));
    CHECK(run(mins).equals<i64>({5, 4, std::nullopt, 3}));
    CHECK(run(maxs).equals<i64>({5, 4, std::nullopt, std::nullopt}));
    CHECK(run(means).equals<double>({5.0, 4.0, std::nullopt, 3.5}));
}

TEST_CASE("required value invalidation restarts every reset aggregate",
          "[feature:operator][series:value][scalar:i64][validity:optional]"
          "[transition:invalidation][transition:revalidation]") {
    const auto sums = [](wp<TS<i64>> value, wp<TS<bool>> reset) {
        return operators::sums(value, "reset"_n = reset);
    };

    const auto mins = [](wp<TS<i64>> value, wp<TS<bool>> reset) {
        return operators::mins(value, "reset"_n = reset);
    };

    const auto maxs = [](wp<TS<i64>> value, wp<TS<bool>> reset) {
        return operators::maxs(value, "reset"_n = reset);
    };

    const auto means = [](wp<TS<i64>> value, wp<TS<bool>> reset) {
        return operators::means(value, "reset"_n = reset);
    };
    const auto run = [](const auto &operation) {
        return bg::run_graph(operation,
                                   {5, 4, literal::INVALID, 3, 2},
                                   {literal::SKIP, literal::SKIP, literal::SKIP,
                                    literal::SKIP, literal::SKIP});
    };

    CHECK(run(sums).equals<i64>({5, 9, std::nullopt, 3, 5}));
    CHECK(run(mins).equals<i64>({5, 4, std::nullopt, 3, 2}));
    CHECK(run(maxs).equals<i64>({5, std::nullopt, std::nullopt, 3, std::nullopt}));
    CHECK(run(means).equals<double>({5.0, 4.5, std::nullopt, 3.0, 2.5}));
}

TEST_CASE("running minimum and maximum publish only new extrema",
          "[feature:operator][series:value][scalar:i64][guarantee:deduplicated]"
          "[transition:first-tick][transition:update]") {
    const auto mins = [](wp<TS<i64>> value) { return operators::mins(value); };
    const auto maxs = [](wp<TS<i64>> value) { return operators::maxs(value); };
    const auto values = std::array<i64, 7>{4, 4, 6, 3, 3, 5, 2};

    CHECK(bg::run_graph(mins, values).equals<i64>({4, std::nullopt, std::nullopt, 3,
                                                         std::nullopt, std::nullopt, 2}));
    CHECK(bg::run_graph(maxs, values).equals<i64>({4, std::nullopt, 6, std::nullopt,
                                                         std::nullopt, std::nullopt, std::nullopt}));
}

TEST_CASE("running extrema restart after input invalidation",
          "[feature:operator][series:value][scalar:i64][guarantee:deduplicated]"
          "[transition:invalidation][transition:revalidation]") {
    const auto mins = [](wp<TS<i64>> value) { return operators::mins(value); };
    const auto maxs = [](wp<TS<i64>> value) { return operators::maxs(value); };
    CHECK(bg::run_graph(mins, {5, 3, literal::INVALID, 7, 6})
              .equals<i64>({5, 3, std::nullopt, 7, 6}));
    CHECK(bg::run_graph(maxs, {5, 3, literal::INVALID, 7, 6})
              .equals<i64>({5, std::nullopt, std::nullopt, 7, std::nullopt}));
}

TEST_CASE("running extrema keep an initial NaN until input invalidation",
          "[feature:operator][series:value][scalar:floating-point][guarantee:deduplicated]"
          "[transition:invalidation][transition:revalidation]") {
    const auto mins = [](wp<TS<double>> value) { return operators::mins(value); };
    const auto maxs = [](wp<TS<double>> value) { return operators::maxs(value); };
    const auto mins_valid = [](wp<TS<double>> value) { return operators::valid(operators::mins(value)); };
    const auto maxs_valid = [](wp<TS<double>> value) { return operators::valid(operators::maxs(value)); };
    const auto nan = std::numeric_limits<double>::quiet_NaN();
    const auto check_sticky_nan = [nan](const auto &operation) {
        const auto result = bg::run_graph(
            operation, {nan, 1.0, nan, 0.0, literal::INVALID,
                        literal::SKIP, 3.0});
        REQUIRE(result.size() == 7);
        const auto first_value = result[0];
        const auto last_value = result[6];
        REQUIRE(first_value.has_value());
        CHECK(std::isnan(scalar_cast<double>(*first_value).value()));
        for (const auto index : {1, 2, 3, 4, 5}) {
            CHECK_FALSE(result[index].has_value());
        }
        REQUIRE(last_value.has_value());
        CHECK(scalar_cast<double>(*last_value).value() == 3.0);
    };

    check_sticky_nan(mins);
    check_sticky_nan(maxs);
    CHECK(bg::run_graph(mins_valid,
                              {nan, 1.0, nan, 0.0, literal::INVALID,
                               literal::SKIP, 3.0})
              .equals<bool>({true, std::nullopt, std::nullopt, std::nullopt, false, std::nullopt,
                             true}));
    CHECK(bg::run_graph(maxs_valid,
                              {nan, 1.0, nan, 0.0, literal::INVALID,
                               literal::SKIP, 3.0})
              .equals<bool>({true, std::nullopt, std::nullopt, std::nullopt, false, std::nullopt,
                             true}));
}

TEST_CASE("running extrema ignore later NaNs without invalidating output",
          "[feature:operator][series:value][scalar:floating-point][guarantee:deduplicated]") {
    const auto mins = [](wp<TS<double>> value) { return operators::mins(value); };
    const auto maxs = [](wp<TS<double>> value) { return operators::maxs(value); };
    const auto mins_valid = [](wp<TS<double>> value) { return operators::valid(operators::mins(value)); };
    const auto maxs_valid = [](wp<TS<double>> value) { return operators::valid(operators::maxs(value)); };
    const auto nan = std::numeric_limits<double>::quiet_NaN();
    const auto values = std::array<double, 4>{2.0, nan, 3.0, 1.0};

    CHECK(bg::run_graph(mins, values).equals<double>({2.0, std::nullopt, std::nullopt, 1.0}));
    CHECK(bg::run_graph(maxs, values).equals<double>({2.0, std::nullopt, 3.0, std::nullopt}));
    CHECK(bg::run_graph(mins_valid, values)
              .equals<bool>({true, std::nullopt, std::nullopt, std::nullopt}));
    CHECK(bg::run_graph(maxs_valid, values)
              .equals<bool>({true, std::nullopt, std::nullopt, std::nullopt}));
}

TEST_CASE("running extrema clear NaN history on reset-only and simultaneous ticks",
          "[feature:operator][series:value][scalar:floating-point][scalar:boolean][validity:optional]"
          "[transition:update][transition:invalidation]") {
    const auto mins = [](wp<TS<double>> value, wp<TS<bool>> reset) {
        return operators::mins(value, "reset"_n = reset);
    };

    const auto maxs = [](wp<TS<double>> value, wp<TS<bool>> reset) {
        return operators::maxs(value, "reset"_n = reset);
    };

    const auto mins_valid = [](wp<TS<double>> value, wp<TS<bool>> reset) {
        return operators::valid(operators::mins(value, "reset"_n = reset));
    };

    const auto maxs_valid = [](wp<TS<double>> value, wp<TS<bool>> reset) {
        return operators::valid(operators::maxs(value, "reset"_n = reset));
    };
    const auto nan = std::numeric_limits<double>::quiet_NaN();
    const auto check_reset_history = [nan](const auto &operation,
                                           std::optional<double> expected_last_update) {
        const auto result = bg::run_graph(
            operation, {nan, 1.0, literal::SKIP, 4.0,
                        nan, 5.0, 2.0, 3.0},
            {literal::SKIP, literal::SKIP, true, literal::SKIP,
             false, literal::SKIP, true, literal::SKIP});
        REQUIRE(result.size() == 8);
        const auto first_value = result[0];
        const auto value_after_first_reset = result[3];
        const auto value_after_second_nan = result[4];
        const auto value_after_second_reset = result[6];
        const auto last_value = result[7];
        REQUIRE(first_value.has_value());
        CHECK(std::isnan(scalar_cast<double>(*first_value).value()));
        CHECK_FALSE(result[1].has_value());
        CHECK_FALSE(result[2].has_value());
        REQUIRE(value_after_first_reset.has_value());
        CHECK(scalar_cast<double>(*value_after_first_reset).value() == 4.0);
        REQUIRE(value_after_second_nan.has_value());
        CHECK(std::isnan(scalar_cast<double>(*value_after_second_nan).value()));
        CHECK_FALSE(result[5].has_value());
        REQUIRE(value_after_second_reset.has_value());
        CHECK(scalar_cast<double>(*value_after_second_reset).value() == 2.0);
        CHECK(last_value.has_value() == expected_last_update.has_value());
        if (expected_last_update) {
            REQUIRE(last_value.has_value());
            CHECK(scalar_cast<double>(*last_value).value() == *expected_last_update);
        }
    };

    check_reset_history(mins, std::nullopt);
    check_reset_history(maxs, 3.0);
    CHECK(bg::run_graph(mins_valid,
                              {nan, 1.0, literal::SKIP, 4.0,
                               nan, 5.0, 2.0, 3.0},
                              {literal::SKIP, literal::SKIP, true,
                               literal::SKIP, false, literal::SKIP,
                               true, literal::SKIP})
              .equals<bool>({true, std::nullopt, false, true, std::nullopt, std::nullopt,
                             std::nullopt, std::nullopt}));
    CHECK(bg::run_graph(maxs_valid,
                              {nan, 1.0, literal::SKIP, 4.0,
                               nan, 5.0, 2.0, 3.0},
                              {literal::SKIP, literal::SKIP, true,
                               literal::SKIP, false, literal::SKIP,
                               true, literal::SKIP})
              .equals<bool>({true, std::nullopt, false, true, std::nullopt, std::nullopt,
                             std::nullopt, std::nullopt}));
}

TEST_CASE("running extrema retain final and const guarantees",
          "[feature:operator][series:value][scalar:i64][guarantee:final][guarantee:const]") {
    const auto check_final = [](wp<TS<i64, TimeSeriesGuarantee::final>> value) {
        const auto minimum = operators::mins(value);
        const auto maximum = operators::maxs(value);
        CHECK(&minimum.type() == &TS<i64, TimeSeriesGuarantee::final>::type());
        CHECK(&maximum.type() == &TS<i64, TimeSeriesGuarantee::final>::type());
        return operators::add(minimum, maximum);
    };
    const auto check_const = [] {
        const auto minimum = operators::mins(operators::const_(i64{3}));
        const auto maximum = operators::maxs(operators::const_(i64{3}));
        CHECK(&minimum.type() == &TS<i64, TimeSeriesGuarantee::const_>::type());
        CHECK(&maximum.type() == &TS<i64, TimeSeriesGuarantee::const_>::type());
        return minimum + maximum;
    };

    CHECK(bg::run_graph(check_final, bg::input<TS<i64, TimeSeriesGuarantee::final>>({4}))
              .equals<i64>({8}));
    CHECK(bg::run_graph(check_const).equals<i64>({6}));
}

TEST_CASE("running floating aggregates calculate sums, extrema, and means",
          "[feature:operator][series:value][scalar:floating-point][guarantee:ordinary]"
          "[transition:first-tick][transition:update]") {
    const auto sums = [](wp<TS<double>> value) { return operators::sums(value); };
    const auto mins = [](wp<TS<double>> value) { return operators::mins(value); };
    const auto maxs = [](wp<TS<double>> value) { return operators::maxs(value); };
    const auto means = [](wp<TS<double>> value) { return operators::means(value); };
    const auto values = std::array<double, 6>{2.0, 2.0, -1.0, 4.0, 3.0, 1.0};

    CHECK(bg::run_graph(sums, values).equals<double>({2.0, 4.0, 3.0, 7.0, 10.0, 11.0}));
    CHECK(bg::run_graph(mins, values).equals<double>({2.0, std::nullopt, -1.0, std::nullopt,
                                                            std::nullopt, std::nullopt}));
    CHECK(bg::run_graph(maxs, values).equals<double>({2.0, std::nullopt, std::nullopt, 4.0,
                                                            std::nullopt, std::nullopt}));
    CHECK(bg::run_graph(means, values).equals<double>({2.0, 2.0, 1.0, 1.75, 2.0, 11.0 / 6.0}));
}

TEST_CASE("running means use double output and restart after invalidation",
          "[feature:operator][series:value][scalar:i64][scalar:floating-point][validity:optional]"
          "[transition:update][transition:invalidation][transition:revalidation]") {
    const auto integer_mean = [](wp<TS<i64>> value) {
        const auto result = operators::means(value);
        CHECK(&result.type() == &TS<double>::type());
        return result;
    };
    const auto floating_mean = [](wp<TS<double>> value) { return operators::means(value); };

    CHECK(bg::run_graph(integer_mean, {1, 2, 3})
              .equals<double>({1.0, 1.5, 2.0}));
    CHECK(bg::run_graph(floating_mean,
                              {2.0, 4.0, literal::INVALID, 10.0, 20.0})
              .equals<double>({2.0, 3.0, std::nullopt, 10.0, 15.0}));
}

TEST_CASE("running sums and means retain the input guarantee",
          "[feature:operator][series:value][scalar:i64][scalar:floating-point]"
          "[guarantee:ordinary][guarantee:deduplicated][guarantee:final][guarantee:const]") {
    const auto check_deduplicated = [](wp<TS<i64, TimeSeriesGuarantee::deduplicated>> value) {
        const auto sum = operators::sums(value);
        const auto mean = operators::means(value);
        CHECK(&sum.type() == &TS<i64, TimeSeriesGuarantee::deduplicated>::type());
        CHECK(&mean.type() == &TS<double, TimeSeriesGuarantee::deduplicated>::type());
        return sum;
    };
    const auto check_final = [](wp<TS<i64, TimeSeriesGuarantee::final>> value) {
        const auto sum = operators::sums(value);
        const auto mean = operators::means(value);
        CHECK(&sum.type() == &TS<i64, TimeSeriesGuarantee::final>::type());
        CHECK(&mean.type() == &TS<double, TimeSeriesGuarantee::final>::type());
        return sum;
    };
    const auto check_const = [] {
        const auto value = operators::const_(i64{7});
        const auto sum = operators::sums(value);
        const auto mean = operators::means(value);
        CHECK(&sum.type() == &TS<i64, TimeSeriesGuarantee::const_>::type());
        CHECK(&mean.type() == &TS<double, TimeSeriesGuarantee::const_>::type());
        return sum;
    };

    CHECK(bg::run_graph(check_deduplicated,
                              bg::input<TS<i64, TimeSeriesGuarantee::deduplicated>>({2, 3}))
              .equals<i64>({2, 5}));
    CHECK(bg::run_graph(check_final,
                              bg::input<TS<i64, TimeSeriesGuarantee::final>>({literal::SKIP, 4}))
              .equals<i64>({std::nullopt, 4}));
    CHECK(bg::run_graph(check_const).equals<i64>({7}));
}

TEST_CASE("reset operators weaken final and const guarantees",
          "[feature:operator][series:value][scalar:i64][scalar:boolean]"
          "[guarantee:final][guarantee:const][transition:update]") {
    const auto check_final = [](wp<TS<i64, TimeSeriesGuarantee::final>> value,
                                wp<TS<bool, TimeSeriesGuarantee::final>> reset) {
        const auto sum = operators::sums(value, "reset"_n = reset);
        const auto minimum = operators::mins(value, "reset"_n = reset);
        const auto maximum = operators::maxs(value, "reset"_n = reset);
        const auto mean = operators::means(value, "reset"_n = reset);
        CHECK(&sum.type() == &TS<i64>::type());
        CHECK(&minimum.type() == &TS<i64, TimeSeriesGuarantee::deduplicated>::type());
        CHECK(&maximum.type() == &TS<i64, TimeSeriesGuarantee::deduplicated>::type());
        CHECK(&mean.type() == &TS<double>::type());
        return sum;
    };
    const auto check_const = [] {
        const auto value = operators::const_(i64{4});
        const auto reset = operators::const_(true);
        const auto sum = operators::sums(value, "reset"_n = reset);
        const auto minimum = operators::mins(value, "reset"_n = reset);
        const auto maximum = operators::maxs(value, "reset"_n = reset);
        const auto mean = operators::means(value, "reset"_n = reset);
        CHECK(&sum.type() == &TS<i64>::type());
        CHECK(&minimum.type() == &TS<i64, TimeSeriesGuarantee::deduplicated>::type());
        CHECK(&maximum.type() == &TS<i64, TimeSeriesGuarantee::deduplicated>::type());
        CHECK(&mean.type() == &TS<double>::type());
        return sum;
    };

    CHECK(bg::run_graph(check_final,
                              bg::input<TS<i64, TimeSeriesGuarantee::final>>({4}),
                              bg::input<TS<bool, TimeSeriesGuarantee::final>>({false}))
              .equals<i64>({4}));
    CHECK(bg::run_graph(check_const).equals<i64>({4}));
}

TEST_CASE("running means avoid integer overflow and use ordinary floating arithmetic",
          "[feature:operator][series:value][scalar:i64][scalar:floating-point][transition:update]") {
    const auto integer_mean = [](wp<TS<i64>> value) { return operators::means(value); };
    const auto floating_mean = [](wp<TS<double>> value) { return operators::means(value); };
    const auto maximum = std::numeric_limits<i64>::max();
    const auto infinity = std::numeric_limits<double>::infinity();

    CHECK(bg::run_graph(integer_mean, {maximum, maximum})
              .equals<double>({static_cast<double>(maximum), static_cast<double>(maximum)}));
    CHECK(bg::run_graph(floating_mean, {infinity, 1.0})
              .equals<double>({infinity, infinity}));
}

TEST_CASE("running integer sums have independent nested graph state",
          "[feature:operator][series:value][scalar:i64][shape:nested]") {
    const auto running_sum = graph("name"_n = "running_sum", [](wp<TS<i64>> value) {
        return wp<TS<i64>>{operators::sums(value)};
    });
    const auto combine = [&](wp<TS<i64>> lhs, wp<TS<i64>> rhs, wp<TS<bool>> run) {
        return operators::add(nested(running_sum, run, lhs), nested(running_sum, run, rhs));
    };

    CHECK(bg::run_graph(combine, {1, 2, 3}, {10, 20, 30},
                              {true, true, true})
              .equals<i64>({11, 33, 66}));
}

TEST_CASE("running integer sums follow selected reference ticks",
          "[feature:operator][series:value][series:reference][scalar:i64]"
          "[binding:reference-to-value][transition:update]") {
    const auto accumulate_selected = [](wp<TS<bool>> condition, wp<TS<i64>> value) {
        const auto selected = operators::if_then_else(condition, value, operators::const_(i64{100}));
        return operators::sums(selected);
    };

    CHECK(bg::run_graph(accumulate_selected, {true, true, true},
                              {1, 2, 3})
              .equals<i64>({1, 3, 6}));

    const auto retarget = [](wp<TS<bool>> condition) {
        const auto selected = operators::if_then_else(condition, operators::const_(i64{10}),
                                                       operators::const_(i64{20}));
        return operators::sums(selected);
    };
    CHECK(bg::run_graph(retarget, {true, true, false, true})
              .equals<i64>({10, std::nullopt, 30, 40}));
}
