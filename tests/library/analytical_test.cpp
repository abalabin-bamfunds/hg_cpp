#include <bg/graph.hpp>
#include <bg/library.hpp>
#include <bg/nested.hpp>
#include <bg/run_graph.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <limits>
#include <optional>
#include <stdexcept>

using namespace bg;
using namespace bg::literals;

TEST_CASE("diff subtracts the previous valid numeric value",
          "[feature:operator][series:value][scalar:i64][guarantee:ordinary]"
          "[transition:first-tick][transition:update]") {
    const auto diff = [](wp<TS<i64>> value) { return operators::diff(value); };

    CHECK(bg::run_graph(diff, {10, 15, 15, 20})
              .equals<i64>({std::nullopt, 5, 0, 5}));
}

TEST_CASE("diff wraps integer subtraction and accepts deduplicated inputs",
          "[feature:operator][series:value][scalar:i64][guarantee:deduplicated][transition:update]") {
    const auto difference = [](wp<TS<i64>> value) { return operators::diff(value); };
    const auto deduplicated_difference = [](wp<TS<i64>> value) {
        const auto result = operators::diff(operators::dedup(value));
        CHECK(&result.type() == &TS<i64>::type());
        return result;
    };

    CHECK(bg::run_graph(difference,
                              {std::numeric_limits<i64>::max(), std::numeric_limits<i64>::min()})
              .equals<i64>({std::nullopt, 1}));
    CHECK(bg::run_graph(
              deduplicated_difference,
              {1, 2, 3})
              .equals<i64>({std::nullopt, 1, 1}));
}

TEST_CASE("diff skips idle cycles and restarts after invalidation",
          "[feature:operator][feature:invalidation][series:value][scalar:i64]"
          "[transition:update][transition:invalidation][transition:revalidation]") {
    const auto diff = [](wp<TS<i64>> value) { return operators::diff(value); };

    CHECK(bg::run_graph(diff, {10, 15, literal::SKIP, 15, literal::INVALID,
                                     literal::SKIP, 30, 37})
              .equals<i64>({std::nullopt, 5, std::nullopt, 0, std::nullopt,
                            std::nullopt, std::nullopt, 7}));
}

TEST_CASE("diff follows reference retargeting and owns nested lag state",
          "[feature:operator][feature:state][feature:nested-graph][series:value]"
          "[series:reference][binding:reference-to-value][scalar:floating-point]"
          "[shape:nested][transition:update]") {
    const auto follow_reference = [](wp<TS<bool>> use_second, wp<TS<double>> first,
                                     wp<TS<double>> second) {
        return operators::diff(operators::if_then_else(use_second, first, second));
    };
    CHECK(bg::run_graph(follow_reference, {true, true, false, false},
                              {1.0, 3.0, literal::SKIP, literal::SKIP},
                              {10.0, literal::SKIP, 10.0, 12.0})
              .equals<double>({std::nullopt, 2.0, 7.0, 2.0}));

    const auto differ = graph("name"_n = "differ", [](wp<TS<double>> value) {
        return wp<TS<double>>{operators::diff(value)};
    });
    const auto combine = [&](wp<TS<double>> lhs, wp<TS<double>> rhs, wp<TS<bool>> run) {
        return operators::add(nested(differ, run, lhs), nested(differ, run, rhs));
    };
    CHECK(bg::run_graph(combine, {1.0, 3.0, 6.0}, {10.0, 15.0, 25.0},
                              {true, true, true})
              .equals<double>({std::nullopt, 7.0, 13.0}));
}

TEST_CASE("clip accepts scalar bounds for integer and floating inputs",
          "[feature:operator][series:value][scalar:i64][scalar:floating-point]"
          "[guarantee:ordinary]") {
    const auto clip_scalar = [] { return operators::clip(i64{5}, i64{0}, i64{10}); };
    const auto clip_integer = [](wp<TS<i64>> value) { return operators::clip(value, 0, 10); };
    const auto clip_double = [](wp<TS<double>> value) { return operators::clip(value, 0.0, 10.0); };

    CHECK(bg::run_graph(clip_scalar).equals<i64>({5}));
    CHECK(bg::run_graph(clip_integer, {-5, 5, 20}).equals<i64>({0, 5, 10}));
    CHECK(bg::run_graph(clip_double, {-5.0, 5.0, 20.0}).equals<double>({0.0, 5.0, 10.0}));
}

TEST_CASE("clip preserves deduplication and recomputes on a maximum-only bound tick",
          "[feature:operator][series:value][scalar:i64][guarantee:deduplicated][transition:update]") {
    const auto deduplicated_clip = [](wp<TS<i64>> value) {
        const auto result = operators::clip(operators::dedup(value), i64{2}, i64{8});
        CHECK(&result.type() == &TS<i64, TimeSeriesGuarantee::deduplicated>::type());
        return result;
    };
    const auto maximum_clip = [](wp<TS<i64>> value, wp<TS<i64>> maximum) {
        return operators::clip(value, i64{0}, maximum);
    };
    const auto scalar_value_clip = [](wp<TS<i64>> minimum) {
        return operators::clip(i64{5}, minimum, i64{10});
    };

    CHECK(bg::run_graph(
              deduplicated_clip,
              {1, 2, 3})
              .equals<i64>({2, std::nullopt, 3}));
    CHECK(bg::run_graph(maximum_clip, {5, literal::SKIP, literal::SKIP}, {10, 4, 8})
              .equals<i64>({5, 4, 5}));
    CHECK(bg::run_graph(scalar_value_clip, {0, 6, 3}).equals<i64>({5, 6, 5}));
}

TEST_CASE("clip evaluates changing bounds and invalidates with required inputs",
          "[feature:operator][feature:validity][series:value][scalar:floating-point]"
          "[validity:required][transition:update][transition:invalidation][transition:revalidation]") {
    const auto clip = [](wp<TS<double>> value, wp<TS<double>> min_, wp<TS<double>> max_) {
        return operators::clip(value, min_, max_);
    };

    CHECK(bg::run_graph(clip,
                              {5.0, literal::SKIP, literal::SKIP, 5.0},
                              {0.0, 4.0, 6.0, 6.0},
                              {10.0, 10.0, 10.0, 6.0})
              .equals<double>({5.0, 5.0, 6.0, 6.0}));

    CHECK(bg::run_graph(clip,
                              {5.0, literal::SKIP, literal::SKIP, 5.0},
                              {0.0, literal::INVALID, literal::SKIP, 1.0},
                              {10.0, 10.0, 10.0, 10.0})
              .equals<double>({5.0, std::nullopt, std::nullopt, 5.0}));
}

TEST_CASE("clip checks bound order during evaluation and uses ordinary comparisons",
          "[feature:operator][series:value][scalar:floating-point][transition:update]") {
    const auto clip = [](wp<TS<double>> value, wp<TS<double>> min_, wp<TS<double>> max_) {
        return operators::clip(value, min_, max_);
    };

    CHECK_THROWS_AS(bg::run_graph(clip, {1.0}, {2.0}, {0.0}), std::invalid_argument);

    const auto nan = std::numeric_limits<double>::quiet_NaN();
    const auto actual = bg::run_graph(clip, {nan}, {0.0}, {10.0});
    REQUIRE(actual[0].has_value());
    CHECK(std::isnan(scalar_cast<double>(actual[0]->view()).value()));
}

TEST_CASE("clip preserves the weakest input guarantee",
          "[feature:operator][series:value][scalar:floating-point]"
          "[guarantee:final][guarantee:const]") {
    const auto final_value = node([](Out<TS<double, TimeSeriesGuarantee::final>> output) { output.set(5.0); });

    const auto clip_final = [&] {
        const auto result = operators::clip(final_value(), 0.0, 10.0);
        CHECK(&result.type() == &TS<double, TimeSeriesGuarantee::final>::type());
        return result;
    };
    const auto clip_const = [] {
        const auto result = operators::clip(operators::const_(5.0), 0.0, 10.0);
        CHECK(&result.type() == &TS<double, TimeSeriesGuarantee::const_>::type());
        return result;
    };

    CHECK(bg::run_graph(clip_final).equals<double>({5.0}));
    CHECK(bg::run_graph(clip_const).equals<double>({5.0}));
}

TEST_CASE("clip preserves final guarantees and clamps floating infinities",
          "[feature:operator][series:value][scalar:floating-point][guarantee:final][transition:update]") {
    const auto final_value = node([](Out<TS<i64, TimeSeriesGuarantee::final>> output) { output.set(8); });

    const auto final_minimum = node([](Out<TS<i64, TimeSeriesGuarantee::final>> output) { output.set(2); });

    const auto final_maximum = node([](Out<TS<i64, TimeSeriesGuarantee::final>> output) { output.set(6); });

    const auto final_clip = [&] {
        const auto result = operators::clip(final_value(), final_minimum(), final_maximum());
        CHECK(&result.type() == &TS<i64, TimeSeriesGuarantee::final>::type());
        return result;
    };
    const auto floating_clip = [](wp<TS<double>> value) {
        return operators::clip(value, 0.0, 10.0);
    };

    CHECK(bg::run_graph(final_clip).equals<i64>({6}));
    const auto infinity = std::numeric_limits<double>::infinity();
    CHECK(bg::run_graph(floating_clip, {-infinity, infinity}).equals<double>({0.0, 10.0}));
}

TEST_CASE("ewma seeds from the first value and updates on each valid tick",
          "[feature:operator][feature:state][series:value][scalar:floating-point]"
          "[guarantee:ordinary][transition:first-tick][transition:update]") {
    const auto ewma = [](wp<TS<double>> value) { return operators::ewma(value, 0.5); };

    CHECK(bg::run_graph(ewma, {1.0, 3.0, 6.0}).equals<double>({1.0, 2.0, 4.0}));
}

TEST_CASE("ewma alpha endpoints and warmup count valid ticks only",
          "[feature:operator][feature:state][series:value][scalar:floating-point]"
          "[transition:first-tick][transition:update]") {
    const auto alpha_zero = [](wp<TS<double>> value) { return operators::ewma(value, 0.0); };
    const auto alpha_one = [](wp<TS<double>> value) { return operators::ewma(value, 1.0); };
    const auto warmup = [](wp<TS<double>> value) { return operators::ewma(value, 0.5, 2); };

    CHECK(bg::run_graph(alpha_zero, {1.0, 3.0, 6.0}).equals<double>({1.0, 1.0, 1.0}));
    CHECK(bg::run_graph(alpha_one, {1.0, 3.0, 6.0}).equals<double>({1.0, 3.0, 6.0}));
    CHECK(bg::run_graph(warmup, {1.0, literal::SKIP, 3.0, 5.0, 9.0})
              .equals<double>({std::nullopt, std::nullopt, std::nullopt, 3.5, 6.25}));
}

TEST_CASE("ewma clears its state and warmup after input invalidation",
          "[feature:operator][feature:state][feature:invalidation][series:value]"
          "[scalar:floating-point][transition:update][transition:invalidation][transition:revalidation]") {
    const auto ewma = [](wp<TS<double>> value) { return operators::ewma(value, 0.5, 1); };
    const auto valid_ewma = [](wp<TS<double>> value) {
        return operators::valid(operators::ewma(value, 0.5, 1));
    };

    CHECK(bg::run_graph(ewma, {1.0, 3.0, literal::INVALID, literal::SKIP, 10.0, 14.0})
              .equals<double>({std::nullopt, 2.0, std::nullopt, std::nullopt, std::nullopt, 12.0}));
    CHECK(bg::run_graph(valid_ewma,
                              {1.0, 3.0, literal::INVALID, literal::SKIP, 10.0, 14.0})
              .equals<bool>({false, true, false, std::nullopt, std::nullopt, true}));
}

TEST_CASE("ewma restarts an unfinished warmup after input invalidation",
          "[feature:operator][feature:state][feature:invalidation][series:value]"
          "[scalar:floating-point][transition:invalidation][transition:revalidation]") {
    const auto ewma = [](wp<TS<double>> value) { return operators::ewma(value, 0.5, 2); };

    CHECK(bg::run_graph(ewma, {1.0, literal::INVALID, literal::SKIP, 10.0, 14.0, 18.0})
              .equals<double>({std::nullopt, std::nullopt, std::nullopt, std::nullopt,
                               std::nullopt, 15.0}));
}

TEST_CASE("ewma validates scalar configuration during wiring",
          "[feature:operator][feature:wiring][series:value][scalar:floating-point][scalar:i64]") {
    const auto negative_alpha = [](wp<TS<double>> value) { return operators::ewma(value, -0.1); };
    const auto high_alpha = [](wp<TS<double>> value) { return operators::ewma(value, 1.1); };
    const auto infinite_alpha = [](wp<TS<double>> value) {
        return operators::ewma(value, std::numeric_limits<double>::infinity());
    };
    const auto negative_periods = [](wp<TS<double>> value) { return operators::ewma(value, 0.5, -1); };

    CHECK_THROWS_AS(bg::run_graph(negative_alpha, {1.0}), WiringError);
    CHECK_THROWS_AS(bg::run_graph(high_alpha, {1.0}), WiringError);
    CHECK_THROWS_AS(bg::run_graph(infinite_alpha, {1.0}), WiringError);
    CHECK_THROWS_AS(bg::run_graph(negative_periods, {1.0}), WiringError);
}

TEST_CASE("ewma retains input guarantees and caps const warmup at final",
          "[feature:operator][feature:state][series:value][scalar:floating-point]"
          "[guarantee:deduplicated][guarantee:final][guarantee:const]") {
    const auto check_deduplicated = [](wp<TS<double>> value) {
        const auto result = operators::ewma(operators::dedup(value), 0.5);
        CHECK(&result.type() == &TS<double, TimeSeriesGuarantee::deduplicated>::type());
        return result;
    };
    const auto final_value = node([](Out<TS<double, TimeSeriesGuarantee::final>> output) { output.set(4.0); });

    const auto check_final = [&] {
        const auto result = operators::ewma(final_value(), 0.5);
        CHECK(&result.type() == &TS<double, TimeSeriesGuarantee::final>::type());
        return result;
    };
    const auto check_const = [] {
        const auto result = operators::ewma(operators::const_(2.0), 0.5);
        CHECK(&result.type() == &TS<double, TimeSeriesGuarantee::const_>::type());
        return result;
    };
    const auto check_const_warmup = [] {
        const auto result = operators::ewma(operators::const_(2.0), 0.5, 1);
        CHECK(&result.type() == &TS<double, TimeSeriesGuarantee::final>::type());
        return result;
    };

    CHECK(bg::run_graph(check_deduplicated, {2.0, 4.0}).equals<double>({2.0, 3.0}));
    CHECK(bg::run_graph(check_final).equals<double>({4.0}));
    CHECK(bg::run_graph(check_const).equals<double>({2.0}));
    CHECK(bg::run_graph(check_const_warmup).equals<double>({std::nullopt}));
}

TEST_CASE("ewma state is independent across nested graph instances",
          "[feature:operator][feature:state][feature:nested-graph][series:value]"
          "[scalar:floating-point][guarantee:ordinary][shape:nested]") {
    const auto smooth = graph("name"_n = "smooth", [](wp<TS<double>> value) {
        return wp<TS<double>>{operators::ewma(value, 0.5)};
    });
    const auto combine = [&](wp<TS<double>> lhs, wp<TS<double>> rhs, wp<TS<bool>> run) {
        return operators::add(nested(smooth, run, lhs), nested(smooth, run, rhs));
    };

    CHECK(bg::run_graph(combine, {1.0, 2.0, 3.0}, {10.0, 20.0, 30.0},
                              {true, true, true})
              .equals<double>({11.0, 16.5, 24.75}));
}
