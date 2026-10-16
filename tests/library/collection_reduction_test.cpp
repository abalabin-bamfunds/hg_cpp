#include <bg/library.hpp>
#include <bg/testing/test_graph.hpp>
#include <bg/testing/test_wiring.hpp>
#include <bg/time_series/tsl/tsl.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <limits>

using namespace bg;
using namespace bg::literals;

TEST_CASE("TSS reductions read the complete current membership",
          "[feature:operator][series:tss][scalar:i64][shape:single-node][transition:addition]"
          "[transition:removal]") {
    const auto sum = [](wp<TSS<i64>> values) { return operators::sum_("values"_n = values); };
    const auto minimum = [](wp<TSS<i64>> values) { return operators::min_("values"_n = values); };
    const auto maximum = [](wp<TSS<i64>> values) { return operators::max_("values"_n = values); };
    const auto mean = [](wp<TSS<i64>> values) { return operators::mean("values"_n = values); };
    const auto run = [](auto operation) {
        const auto graph = [operation](wp<TSS<i64>> values) { return operation(values); };
        return testing::test_graph(graph,
                                   {{1, 2}, {3}, {literal::REMOVED(1), 4},
                                    {literal::REMOVED(2), literal::REMOVED(3), literal::REMOVED(4)}});
    };

    CHECK(run(sum).equals<i64>({3, 6, 9, 0}));
    CHECK(run(minimum).equals<i64>({1, 1, 2, std::nullopt}));
    CHECK(run(maximum).equals<i64>({2, 3, 4, std::nullopt}));
    CHECK(run(mean).equals<double>({1.5, 2.0, 3.0, std::nullopt}));
}

TEST_CASE("TSD reductions gate invalid items and optional reductions skip them",
          "[feature:operator][series:tsd][scalar:i64][shape:single-node][validity:all-valid]"
          "[validity:optional][transition:invalidation][transition:revalidation][transition:removal]") {
    const auto sum = [](wp<TSD<i64, TS<i64>>> values) { return operators::sum_("values"_n = values); };
    const auto optional_sum = [](wp<TSD<i64, TS<i64>>> values) {
        return operators::sum_("values"_n = opt(values));
    };
    const auto minimum = [](wp<TSD<i64, TS<i64>>> values) { return operators::min_("values"_n = values); };
    const auto optional_minimum = [](wp<TSD<i64, TS<i64>>> values) {
        return operators::min_("values"_n = opt(values));
    };
    const auto maximum = [](wp<TSD<i64, TS<i64>>> values) { return operators::max_("values"_n = values); };
    const auto optional_maximum = [](wp<TSD<i64, TS<i64>>> values) {
        return operators::max_("values"_n = opt(values));
    };
    const auto mean = [](wp<TSD<i64, TS<i64>>> values) { return operators::mean("values"_n = values); };
    const auto optional_mean = [](wp<TSD<i64, TS<i64>>> values) {
        return operators::mean("values"_n = opt(values));
    };
    const auto run = [](auto operation) {
        const auto graph = [operation](wp<TSD<i64, TS<i64>>> values) { return operation(values); };
        return testing::test_graph(graph, testing::input<TSD<i64, TS<i64>>>(
            {{{1, 10}, {2, 20}}, {{1, literal::INVALID}, {2, 30}}, {{1, literal::REMOVE}, {2, 20}}}));
    };

    CHECK(run(sum).equals<i64>({30, std::nullopt, 20}));
    CHECK(run(optional_sum).equals<i64>({30, 30, 20}));
    CHECK(run(minimum).equals<i64>({10, std::nullopt, 20}));
    CHECK(run(optional_minimum).equals<i64>({10, 30, 20}));
    CHECK(run(maximum).equals<i64>({20, std::nullopt, 20}));
    CHECK(run(optional_maximum).equals<i64>({20, 30, 20}));
    CHECK(run(mean).equals<double>({15.0, std::nullopt, 20.0}));
    CHECK(run(optional_mean).equals<double>({15.0, 30.0, 20.0}));
}

TEST_CASE("TSL reductions scan fixed items and optional reductions skip invalid items",
          "[feature:operator][series:fixed-tsl][scalar:i64][shape:single-node][validity:all-valid]"
          "[validity:optional][transition:invalidation]") {
    const auto sum = [](wp<TS<i64>> first, wp<TS<i64>> second) {
        return operators::sum_("values"_n = tsl(std::array{first, second}));
    };
    const auto optional_sum = [](wp<TS<i64>> first, wp<TS<i64>> second) {
        return operators::sum_("values"_n = opt(tsl(std::array{first, second})));
    };
    const auto minimum = [](wp<TS<i64>> first, wp<TS<i64>> second) {
        return operators::min_("values"_n = tsl(std::array{first, second}));
    };
    const auto optional_minimum = [](wp<TS<i64>> first, wp<TS<i64>> second) {
        return operators::min_("values"_n = opt(tsl(std::array{first, second})));
    };
    const auto maximum = [](wp<TS<i64>> first, wp<TS<i64>> second) {
        return operators::max_("values"_n = tsl(std::array{first, second}));
    };
    const auto optional_maximum = [](wp<TS<i64>> first, wp<TS<i64>> second) {
        return operators::max_("values"_n = opt(tsl(std::array{first, second})));
    };
    const auto mean = [](wp<TS<i64>> first, wp<TS<i64>> second) {
        return operators::mean("values"_n = tsl(std::array{first, second}));
    };
    const auto optional_mean = [](wp<TS<i64>> first, wp<TS<i64>> second) {
        return operators::mean("values"_n = opt(tsl(std::array{first, second})));
    };
    CHECK(testing::test_graph(sum, testing::input<TS<i64>>({1, 2, literal::INVALID, 4}),
                              testing::input<TS<i64>>({10, literal::SKIP, 20, literal::INVALID}))
              .equals<i64>({11, 12, std::nullopt, std::nullopt}));
    CHECK(testing::test_graph(optional_sum, testing::input<TS<i64>>({1, 2, literal::INVALID, 4}),
                              testing::input<TS<i64>>({10, literal::SKIP, 20, literal::INVALID}))
              .equals<i64>({11, 12, 20, 4}));
    CHECK(testing::test_graph(mean, testing::input<TS<i64>>({1, 2, literal::INVALID, 4}),
                              testing::input<TS<i64>>({10, literal::SKIP, 20, literal::INVALID}))
              .equals<double>({5.5, 6.0, std::nullopt, std::nullopt}));
    CHECK(testing::test_graph(optional_mean, testing::input<TS<i64>>({1, 2, literal::INVALID, 4}),
                              testing::input<TS<i64>>({10, literal::SKIP, 20, literal::INVALID}))
              .equals<double>({5.5, 6.0, 20.0, 4.0}));
    CHECK(testing::test_graph(minimum, testing::input<TS<i64>>({1, 2, literal::INVALID, 4}),
                              testing::input<TS<i64>>({10, literal::SKIP, 20, literal::INVALID}))
              .equals<i64>({1, 2, std::nullopt, std::nullopt}));
    CHECK(testing::test_graph(optional_minimum, testing::input<TS<i64>>({1, 2, literal::INVALID, 4}),
                              testing::input<TS<i64>>({10, literal::SKIP, 20, literal::INVALID}))
              .equals<i64>({1, 2, 20, 4}));
    CHECK(testing::test_graph(maximum, testing::input<TS<i64>>({1, 2, literal::INVALID, 4}),
                              testing::input<TS<i64>>({10, literal::SKIP, 20, literal::INVALID}))
              .equals<i64>({10, 10, std::nullopt, std::nullopt}));
    CHECK(testing::test_graph(optional_maximum, testing::input<TS<i64>>({1, 2, literal::INVALID, 4}),
                              testing::input<TS<i64>>({10, literal::SKIP, 20, literal::INVALID}))
              .equals<i64>({10, 10, 20, 4}));
}

TEST_CASE("collection reductions accept native fixed TSL inputs",
          "[feature:operator][series:fixed-tsl][scalar:i64][representation:native]"
          "[validity:all-valid][validity:optional][transition:invalidation]") {
    const auto sum = [](wp<TSL<TS<i64>, 2>> values) { return operators::sum_("values"_n = values); };
    const auto optional_sum = [](wp<TSL<TS<i64>, 2>> values) {
        return operators::sum_("values"_n = opt(values));
    };

    CHECK(testing::test_graph(sum, {{{0, 1}, {1, 10}}, {{0, 2}}, {{0, literal::INVALID}, {1, 20}}})
              .equals<i64>({11, 12, std::nullopt}));
    CHECK(testing::test_graph(optional_sum,
                              {{{0, 1}, {1, 10}}, {{0, 2}}, {{0, literal::INVALID}, {1, 20}}})
              .equals<i64>({11, 12, 20}));
}

TEST_CASE("collection reductions handle valid empty TSS membership",
          "[feature:operator][series:tss][scalar:i64][validity:optional][transition:first-tick]"
          "[transition:removal]") {
    const auto sum = [](auto values) { return operators::sum_("values"_n = opt(values)); };
    const auto minimum = [](auto values) { return operators::min_("values"_n = opt(values)); };
    const auto maximum = [](auto values) { return operators::max_("values"_n = opt(values)); };
    const auto mean = [](auto values) { return operators::mean("values"_n = opt(values)); };
    const auto run = [](auto operation) {
        const auto graph = [operation](wp<TSS<i64>> values) { return operation(values); };
        return testing::test_graph(
            graph, testing::input<TSS<i64>>({literal::SKIP, {}, {4}, {literal::REMOVED(4)}, {5},
                                             {literal::REMOVED(5)}}));
    };

    CHECK(run(sum).equals<i64>({std::nullopt, 0, 4, 0, 5, 0}));
    CHECK(run(minimum).equals<i64>({std::nullopt, std::nullopt, 4, std::nullopt, 5, std::nullopt}));
    CHECK(run(maximum).equals<i64>({std::nullopt, std::nullopt, 4, std::nullopt, 5, std::nullopt}));
    CHECK(run(mean).equals<double>({std::nullopt, std::nullopt, 4.0, std::nullopt, 5.0, std::nullopt}));
}

TEST_CASE("collection extrema invalidate and revalidate on membership changes",
          "[feature:operator][series:tss][scalar:i64][transition:removal][transition:revalidation]") {
    const auto valid_minimum = [](wp<TSS<i64>> values) {
        return operators::valid(operators::min_("values"_n = values));
    };
    const auto valid_maximum = [](wp<TSS<i64>> values) {
        return operators::valid(operators::max_("values"_n = values));
    };
    const auto valid_mean = [](wp<TSS<i64>> values) {
        return operators::valid(operators::mean("values"_n = values));
    };

    CHECK(testing::test_graph(valid_minimum, {{4}, {literal::REMOVED(4)}, {5}}).equals<bool>({true, false, true}));
    CHECK(testing::test_graph(valid_maximum, {{4}, {literal::REMOVED(4)}, {5}}).equals<bool>({true, false, true}));
    CHECK(testing::test_graph(valid_mean, {{4}, {literal::REMOVED(4)}, {5}}).equals<bool>({true, false, true}));
}

TEST_CASE("collection reductions observe empty results and revalidate",
          "[feature:operator][series:tss][scalar:i64][validity:optional][transition:first-tick]"
          "[transition:removal][transition:revalidation]") {
    const auto valid_sum = [](wp<TSS<i64>> values) {
        return operators::valid(operators::sum_("values"_n = opt(values)));
    };
    const auto valid_minimum = [](wp<TSS<i64>> values) {
        return operators::valid(operators::min_("values"_n = opt(values)));
    };
    const auto valid_maximum = [](wp<TSS<i64>> values) {
        return operators::valid(operators::max_("values"_n = opt(values)));
    };
    const auto valid_mean = [](wp<TSS<i64>> values) {
        return operators::valid(operators::mean("values"_n = opt(values)));
    };
    CHECK(testing::test_graph(valid_sum, testing::input<TSS<i64>>(
                                             {literal::SKIP, {}, {4}, {literal::REMOVED(4)}, {5}}))
              .equals<bool>({false, true, std::nullopt, std::nullopt, std::nullopt}));
    CHECK(testing::test_graph(valid_minimum, testing::input<TSS<i64>>(
                                                 {literal::SKIP, {}, {4}, {literal::REMOVED(4)}, {5}}))
              .equals<bool>({false, std::nullopt, true, false, true}));
    CHECK(testing::test_graph(valid_maximum, testing::input<TSS<i64>>(
                                                 {literal::SKIP, {}, {4}, {literal::REMOVED(4)}, {5}}))
              .equals<bool>({false, std::nullopt, true, false, true}));
    CHECK(testing::test_graph(valid_mean, testing::input<TSS<i64>>(
                                               {literal::SKIP, {}, {4}, {literal::REMOVED(4)}, {5}}))
              .equals<bool>({false, std::nullopt, true, false, true}));
}

TEST_CASE("optional collection reductions handle a REF retargeted to an invalid collection",
          "[feature:operator][series:tss][series:reference][scalar:i64][validity:optional]"
          "[transition:invalidation][transition:revalidation]") {
    const auto sum = [](auto values) { return operators::sum_("values"_n = opt(values)); };
    const auto minimum = [](auto values) { return operators::min_("values"_n = opt(values)); };
    const auto maximum = [](auto values) { return operators::max_("values"_n = opt(values)); };
    const auto mean = [](auto values) { return operators::mean("values"_n = opt(values)); };
    const auto run = [](auto operation) {
        const auto select = node([](In<TS<i64>> cycle, In<REF<TSS<i64>>, passive> populated,
                                    In<REF<TSS<i64>>, passive> invalid, Out<REF<TSS<i64>>> output) {
            output.copy(*cycle == 1 ? invalid : populated);
        });
        const auto graph = [&, operation](wp<TS<i64>> cycle, wp<TSS<i64>> populated, wp<TSS<i64>> invalid) {
            return operation(select(cycle, populated, invalid));
        };
        return testing::test_graph(graph, {0, 1, 2}, {{4}, literal::SKIP, literal::SKIP}, {});
    };

    CHECK(run(sum).equals<i64>({4, 0, 4}));
    CHECK(run(minimum).equals<i64>({4, std::nullopt, 4}));
    CHECK(run(maximum).equals<i64>({4, std::nullopt, 4}));
    CHECK(run(mean).equals<double>({4.0, std::nullopt, 4.0}));
}

TEST_CASE("collection reductions follow TSD reference items through updates and retargeting",
          "[feature:operator][series:tsd][series:reference][scalar:i64][binding:reference-to-value]"
          "[transition:update]") {
    const auto retain_reference = node([](In<TS<bool>> select, In<REF<TS<i64>>, passive> first,
                                          In<REF<TS<i64>>, passive> second,
                                          Out<TSD<i64, REF<TS<i64>>>> output) {
        output[1].copy(*select ? first : second);
    });
    const auto graph = [=](wp<TS<i64>> first, wp<TS<i64>> second, wp<TS<bool>> select) {
        const auto values = retain_reference(select, first, second);
        return operators::sum_("values"_n = values);
    };

    CHECK(testing::test_graph(graph, std::array{1, 2, 3, 4, 5}, std::array{10, 20, 30, 40, 50},
                              std::array{true, true, false, false, true})
              .equals<i64>({1, 2, 30, 40, 5}));
}

TEST_CASE("optional TSS sum publishes zero before a final REF target becomes valid",
          "[feature:operator][series:tss][series:reference][scalar:i64][guarantee:ordinary]"
          "[validity:optional][feature:scheduling][transition:revalidation]") {
    const auto retain_reference = node(
        eval = [](In<REF<TSS<i64, TimeSeriesGuarantee::final>>, passive> values,
                  Out<REF<TSS<i64, TimeSeriesGuarantee::final>, TimeSeriesGuarantee::final>> output) {
            output.copy(values);
        },
        schedule_on_start = true);
    const auto graph = [=](wp<TSS<i64, TimeSeriesGuarantee::final>> values) {
        const auto reference = values.as<REF<TSS<i64, TimeSeriesGuarantee::final>>>();
        const auto bound = retain_reference(reference);
        const auto result = operators::sum_("values"_n = opt(bound));
        CHECK(&result.type() == &TS<i64>::type());
        return result;
    };

    const auto result = testing::test_graph(
        graph, testing::input<TSS<i64, TimeSeriesGuarantee::final>>({literal::SKIP, {0}}));
    CHECK(result.equals<i64>({0, 0}));
}

TEST_CASE("optional TSS sum publishes equal results for a deduplicated collection",
          "[feature:operator][series:tss][scalar:i64][guarantee:deduplicated]"
          "[guarantee:ordinary][validity:optional][transition:addition]") {
    const auto graph = [](wp<TSS<i64, TimeSeriesGuarantee::deduplicated>> values) {
        const auto result = operators::sum_("values"_n = opt(values));
        CHECK(&result.type() == &TS<i64>::type());
        return result;
    };

    CHECK(testing::test_graph(
              graph, testing::input<TSS<i64, TimeSeriesGuarantee::deduplicated>>({{1}, {0, 1}}))
              .equals<i64>({1, 1}));
}

TEST_CASE("collection reductions reject a TSL with sparse item metadata",
          "[feature:operator][feature:wiring][series:fixed-tsl][scalar:i64][validity:required]") {
    testing::test_wiring([] {
        auto &wiring = WiringContext::current().wiring();
        const auto &sparse_type = time_series_list_type(TS<i64>::type(), std::dynamic_extent,
                                                        TimeSeriesGuarantee::ordinary);
        const auto sparse_input = wiring.borrowed_input(sparse_type);

        CHECK_THROWS_AS(operators::sum_("values"_n = sparse_input), WiringError);
        CHECK_THROWS_AS(operators::min_("values"_n = sparse_input), WiringError);
        CHECK_THROWS_AS(operators::max_("values"_n = sparse_input), WiringError);
        CHECK_THROWS_AS(operators::mean("values"_n = sparse_input), WiringError);
    });
}

TEST_CASE("collection reductions preserve sound guarantees and weaken const extrema",
          "[feature:operator][series:fixed-tsl][scalar:i64][guarantee:const][guarantee:final]") {
    const auto check_sum = [] {
        const auto values = std::array{operators::const_(i64{2}), operators::const_(i64{4})};
        const auto result = operators::sum_("values"_n = tsl(values));
        const auto optional_result = operators::sum_("values"_n = opt(tsl(values)));
        CHECK(&result.type() == &TS<i64>::type());
        CHECK(&optional_result.type() == &TS<i64>::type());
        return result;
    };
    const auto check_minimum = [] {
        const auto values = std::array{operators::const_(i64{2}), operators::const_(i64{4})};
        const auto result = operators::min_("values"_n = tsl(values));
        const auto maximum = operators::max_("values"_n = tsl(values));
        const auto mean = operators::mean("values"_n = tsl(values));
        const auto optional_minimum = operators::min_("values"_n = opt(tsl(values)));
        const auto optional_maximum = operators::max_("values"_n = opt(tsl(values)));
        const auto optional_mean = operators::mean("values"_n = opt(tsl(values)));
        CHECK(&result.type() == &TS<i64>::type());
        CHECK(&maximum.type() == &TS<i64>::type());
        CHECK(&mean.type() == &TS<double>::type());
        CHECK(&optional_minimum.type() == &TS<i64>::type());
        CHECK(&optional_maximum.type() == &TS<i64>::type());
        CHECK(&optional_mean.type() == &TS<double>::type());
        return result;
    };
    const auto check_optional_minimum = [] {
        const auto values = std::array{operators::const_(i64{2}), operators::const_(i64{4})};
        const auto result = operators::min_("values"_n = opt(tsl(values)));
        CHECK(&result.type() == &TS<i64>::type());
        return result;
    };
    const auto check_empty_sum = [] {
        const auto result = operators::sum_("values"_n = tsl<TS<i64>>());
        CHECK(&result.type() == &TS<i64>::type());
        return result;
    };
    const auto check_empty_minimum = [] {
        const auto result = operators::min_("values"_n = tsl<TS<i64>>());
        CHECK(&result.type() == &TS<i64>::type());
        return result;
    };
    const auto check_const_tsl = [](wp<TSL<TS<i64, TimeSeriesGuarantee::const_>, 2,
                                           TimeSeriesGuarantee::const_>> values) {
        const auto sum = operators::sum_("values"_n = values);
        const auto optional_sum = operators::sum_("values"_n = opt(values));
        const auto minimum = operators::min_("values"_n = values);
        const auto maximum = operators::max_("values"_n = values);
        const auto mean = operators::mean("values"_n = values);
        CHECK(&sum.type() == &TS<i64, TimeSeriesGuarantee::const_>::type());
        CHECK(&optional_sum.type() == &TS<i64>::type());
        CHECK(&minimum.type() == &TS<i64, TimeSeriesGuarantee::final>::type());
        CHECK(&maximum.type() == &TS<i64, TimeSeriesGuarantee::final>::type());
        CHECK(&mean.type() == &TS<double, TimeSeriesGuarantee::final>::type());
        return sum;
    };
    const auto check_final_tsl = [](wp<TSL<TS<i64, TimeSeriesGuarantee::final>, 2,
                                           TimeSeriesGuarantee::final>> values) {
        const auto sum = operators::sum_("values"_n = values);
        const auto optional_sum = operators::sum_("values"_n = opt(values));
        const auto minimum = operators::min_("values"_n = values);
        const auto maximum = operators::max_("values"_n = values);
        const auto mean = operators::mean("values"_n = values);
        CHECK(&sum.type() == &TS<i64, TimeSeriesGuarantee::final>::type());
        CHECK(&optional_sum.type() == &TS<i64>::type());
        CHECK(&minimum.type() == &TS<i64, TimeSeriesGuarantee::final>::type());
        CHECK(&maximum.type() == &TS<i64, TimeSeriesGuarantee::final>::type());
        CHECK(&mean.type() == &TS<double, TimeSeriesGuarantee::final>::type());
        return sum;
    };
    const auto check_const_tsd = [](wp<TSD<i64, TS<i64, TimeSeriesGuarantee::const_>,
                                                TimeSeriesGuarantee::const_>> values) {
        const auto sum = operators::sum_("values"_n = values);
        const auto optional_sum = operators::sum_("values"_n = opt(values));
        CHECK(&sum.type() == &TS<i64, TimeSeriesGuarantee::const_>::type());
        CHECK(&optional_sum.type() == &TS<i64>::type());
        return sum;
    };
    const auto check_const_tss = [](wp<TSS<i64, TimeSeriesGuarantee::const_>> values) {
        const auto sum = operators::sum_("values"_n = values);
        const auto optional_sum = operators::sum_("values"_n = opt(values));
        CHECK(&sum.type() == &TS<i64, TimeSeriesGuarantee::const_>::type());
        CHECK(&optional_sum.type() == &TS<i64>::type());
        return sum;
    };

    CHECK(testing::test_graph(check_sum).equals<i64>({6}));
    CHECK(testing::test_graph(check_minimum).equals<i64>({2}));
    CHECK(testing::test_graph(check_optional_minimum).equals<i64>({2}));
    CHECK(testing::test_graph(check_empty_sum).equals<i64>({0}));
    CHECK(testing::test_graph(check_empty_minimum).equals<i64>({std::nullopt}));
    CHECK(testing::test_graph(check_const_tsl, {{{0, 2}, {1, 4}}}).equals<i64>({6}));
    CHECK(testing::test_graph(check_final_tsl, {{{0, 2}, {1, 4}}}).equals<i64>({6}));
    CHECK(testing::test_graph(check_const_tsd,
                              testing::input<TSD<i64, TS<i64, TimeSeriesGuarantee::const_>,
                                                   TimeSeriesGuarantee::const_>>({{{1, 2}, {2, 4}}}))
              .equals<i64>({6}));
    CHECK(testing::test_graph(check_const_tss,
                              testing::input<TSS<i64, TimeSeriesGuarantee::const_>>({{2, 4}}))
              .equals<i64>({6}));
}

TEST_CASE("collection reductions resolve final and deduplicated guarantees for required and optional inputs",
          "[feature:operator][series:tss][scalar:i64][guarantee:final][guarantee:deduplicated]"
          "[validity:all-valid][validity:optional]") {
    const auto check_final = [](wp<TSS<i64, TimeSeriesGuarantee::final>> values) {
        const auto sum = operators::sum_("values"_n = values);
        const auto minimum = operators::min_("values"_n = values);
        const auto maximum = operators::max_("values"_n = values);
        const auto mean = operators::mean("values"_n = values);
        const auto optional_sum = operators::sum_("values"_n = opt(values));
        const auto optional_minimum = operators::min_("values"_n = opt(values));
        const auto optional_maximum = operators::max_("values"_n = opt(values));
        const auto optional_mean = operators::mean("values"_n = opt(values));
        CHECK(&sum.type() == &TS<i64, TimeSeriesGuarantee::final>::type());
        CHECK(&minimum.type() == &TS<i64, TimeSeriesGuarantee::final>::type());
        CHECK(&maximum.type() == &TS<i64, TimeSeriesGuarantee::final>::type());
        CHECK(&mean.type() == &TS<double, TimeSeriesGuarantee::final>::type());
        CHECK(&optional_sum.type() == &TS<i64>::type());
        CHECK(&optional_minimum.type() == &TS<i64, TimeSeriesGuarantee::final>::type());
        CHECK(&optional_maximum.type() == &TS<i64, TimeSeriesGuarantee::final>::type());
        CHECK(&optional_mean.type() == &TS<double, TimeSeriesGuarantee::final>::type());
        return sum;
    };
    const auto check_deduplicated = [](wp<TSS<i64, TimeSeriesGuarantee::deduplicated>> values) {
        const auto sum = operators::sum_("values"_n = values);
        const auto minimum = operators::min_("values"_n = values);
        const auto maximum = operators::max_("values"_n = values);
        const auto mean = operators::mean("values"_n = values);
        const auto optional_sum = operators::sum_("values"_n = opt(values));
        const auto optional_minimum = operators::min_("values"_n = opt(values));
        const auto optional_maximum = operators::max_("values"_n = opt(values));
        const auto optional_mean = operators::mean("values"_n = opt(values));
        CHECK(&sum.type() == &TS<i64, TimeSeriesGuarantee::deduplicated>::type());
        CHECK(&minimum.type() == &TS<i64, TimeSeriesGuarantee::deduplicated>::type());
        CHECK(&maximum.type() == &TS<i64, TimeSeriesGuarantee::deduplicated>::type());
        CHECK(&mean.type() == &TS<double, TimeSeriesGuarantee::deduplicated>::type());
        CHECK(&optional_sum.type() == &TS<i64>::type());
        CHECK(&optional_minimum.type() == &TS<i64, TimeSeriesGuarantee::deduplicated>::type());
        CHECK(&optional_maximum.type() == &TS<i64, TimeSeriesGuarantee::deduplicated>::type());
        CHECK(&optional_mean.type() == &TS<double, TimeSeriesGuarantee::deduplicated>::type());
        return sum;
    };

    CHECK(testing::test_graph(check_final, testing::input<TSS<i64, TimeSeriesGuarantee::final>>({{4}}))
              .equals<i64>({4}));
    CHECK(testing::test_graph(check_deduplicated,
                              testing::input<TSS<i64, TimeSeriesGuarantee::deduplicated>>({{1}, {2}}))
              .equals<i64>({1, 3}));
}

TEST_CASE("i64 collection sums wrap and collection means accumulate in double",
          "[feature:operator][series:tss][series:fixed-tsl][scalar:i64][scalar:floating-point]"
          "[transition:update][transition:removal]") {
    const auto sum = [](wp<TSS<i64>> values) { return operators::sum_("values"_n = values); };
    const auto mean = [](wp<TS<i64>> first, wp<TS<i64>> second) {
        return operators::mean("values"_n = tsl(std::array{first, second}));
    };
    const auto maximum = std::numeric_limits<i64>::max();

    CHECK(testing::test_graph(sum, {{maximum, 1}, {literal::REMOVED(1)}})
              .equals<i64>({std::numeric_limits<i64>::min(), maximum}));
    CHECK(testing::test_graph(mean, std::array{maximum}, std::array{maximum})
              .equals<double>({static_cast<double>(maximum)}));
}

TEST_CASE("TSD and TSS reductions support double items",
          "[feature:operator][series:tsd][series:tss][scalar:floating-point][transition:update]"
          "[transition:removal]") {
    const auto sum_tsd = [](wp<TSD<i64, TS<double>>> values) {
        return operators::sum_("values"_n = values);
    };
    const auto sum_tss = [](wp<TSS<double>> values) { return operators::sum_("values"_n = values); };

    CHECK(testing::test_graph(sum_tsd, {{{1, 1.25}, {2, 2.5}}, {{1, 2.0}}}).equals<double>({3.75, 4.5}));
    CHECK(testing::test_graph(sum_tss, {{1.25, 2.5}, {2.0}, {literal::REMOVED(1.25)}})
              .equals<double>({3.75, 5.75, 4.5}));
}

TEST_CASE("fixed TSL extrema and means use ordinary floating-point comparisons",
          "[feature:operator][series:fixed-tsl][scalar:floating-point][transition:update]") {
    const auto minimum = [](wp<TS<double>> first, wp<TS<double>> second) {
        return operators::min_("values"_n = tsl(std::array{first, second}));
    };
    const auto maximum = [](wp<TS<double>> first, wp<TS<double>> second) {
        return operators::max_("values"_n = tsl(std::array{first, second}));
    };
    const auto mean = [](wp<TS<double>> first, wp<TS<double>> second) {
        return operators::mean("values"_n = tsl(std::array{first, second}));
    };
    const auto nan = std::numeric_limits<double>::quiet_NaN();
    const auto positive_infinity = std::numeric_limits<double>::infinity();
    const auto negative_infinity = -positive_infinity;
    const auto first = std::array{nan, 1.0, positive_infinity};
    const auto second = std::array{2.0, negative_infinity, nan};

    const auto minimum_result = testing::test_graph(minimum, first, second);
    const auto maximum_result = testing::test_graph(maximum, first, second);
    const auto mean_result = testing::test_graph(mean, first, second);
    REQUIRE(minimum_result[0].has_value());
    REQUIRE(maximum_result[0].has_value());
    REQUIRE(mean_result[0].has_value());
    CHECK(std::isnan(scalar_cast<double>(*minimum_result[0]).value()));
    CHECK(std::isnan(scalar_cast<double>(*maximum_result[0]).value()));
    CHECK(std::isnan(scalar_cast<double>(*mean_result[0]).value()));
    CHECK(scalar_cast<double>(*minimum_result[1]).value() == negative_infinity);
    CHECK(scalar_cast<double>(*minimum_result[2]).value() == positive_infinity);
    CHECK(scalar_cast<double>(*maximum_result[1]).value() == 1.0);
    CHECK(scalar_cast<double>(*maximum_result[2]).value() == positive_infinity);
    CHECK(scalar_cast<double>(*mean_result[1]).value() == negative_infinity);
    CHECK(std::isnan(scalar_cast<double>(*mean_result[2]).value()));
}

TEST_CASE("collection reductions reject non-numeric item types",
          "[feature:operator][series:tss][scalar:string]") {
    const auto sum = [](wp<TSS<string>> values) { return operators::sum_("values"_n = values); };

    CHECK_THROWS_AS(testing::test_graph(sum, {{"value"}}), WiringError);
}
