#include <hg/library.hpp>
#include <hg/run_graph.hpp>
#include <hg/testing/test_wiring.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <limits>
#include <optional>

using namespace hg;

TEST_CASE("scalar set operations calculate complete set results",
          "[feature:operator][series:value][scalar:set][transition:update]") {
    const auto input_lhs = hg::input<TS<Set<i64>>>({{1, 2}, {1}, literal::INVALID, {3}, {3}});
    const auto input_rhs = hg::input<TS<Set<i64>>>({{2, 3}, {1, 4}, {2}, {3, 4}, {3, 4}});
    const auto union_graph = [](wp<TS<Set<i64>>> lhs, wp<TS<Set<i64>>> rhs) {
        return lhs | rhs;
    };
    const auto intersection = [](wp<TS<Set<i64>>> lhs, wp<TS<Set<i64>>> rhs) {
        return lhs & rhs;
    };
    const auto difference = [](wp<TS<Set<i64>>> lhs, wp<TS<Set<i64>>> rhs) {
        return lhs - rhs;
    };
    const auto symmetric_difference = [](wp<TS<Set<i64>>> lhs, wp<TS<Set<i64>>> rhs) {
        return lhs ^ rhs;
    };

    CHECK(hg::run_graph(union_graph, input_lhs, input_rhs)
              .equals({{1, 2, 3}, {1, 4}, literal::INVALID, {3, 4}, {3, 4}}));
    CHECK(hg::run_graph(intersection, input_lhs, input_rhs)
              .equals({{2}, {1}, literal::INVALID, {3}, {3}}));
    CHECK(hg::run_graph(difference, input_lhs, input_rhs)
              .equals({{1}, {}, literal::INVALID, {}, {}}));
    CHECK(hg::run_graph(symmetric_difference, input_lhs, input_rhs)
              .equals({{1, 3}, {4}, literal::INVALID, {4}, {4}}));
}

TEST_CASE("optional scalar set operations treat invalid operands as empty",
          "[feature:operator][series:value][scalar:set][validity:optional]"
          "[transition:invalidation][transition:revalidation]") {
    const auto run = [](auto operation) {
        const auto graph = [operation](wp<TS<Set<i64>>> lhs, wp<TS<Set<i64>>> rhs) {
            return operation(opt(lhs), rhs);
        };
        return hg::run_graph(graph,
                             hg::input<TS<Set<i64>>>({{1, 4}, literal::INVALID, {2}, literal::INVALID}),
                             hg::input<TS<Set<i64>>>({{2}, {3}, literal::INVALID, literal::INVALID}));
    };

    CHECK(run([](auto lhs, auto rhs) { return operators::bit_or(lhs, rhs); })
              .equals({{1, 2, 4}, {3}, {2}, {}}));
    CHECK(run([](auto lhs, auto rhs) { return operators::bit_and(lhs, rhs); }).equals({{}, {}, {}, {}}));
    CHECK(run([](auto lhs, auto rhs) { return operators::sub(lhs, rhs); }).equals({{1, 4}, {}, {2}, {}}));
    CHECK(run([](auto lhs, auto rhs) { return operators::bit_xor(lhs, rhs); })
              .equals({{1, 2, 4}, {3}, {2}, {}}));
}

TEST_CASE("scalar list and set reductions support numeric values and empty collections",
          "[feature:operator][series:value][scalar:list][scalar:set][scalar:i64]"
          "[validity:optional][transition:invalidation][transition:revalidation]") {
    const auto list_sum = [](wp<TS<List<i64>>> values) { return operators::sum_(values); };
    const auto set_sum = [](wp<TS<Set<i64>>> values) { return operators::sum_(values); };
    const auto optional_sum = [](wp<TS<Set<i64>>> values) { return operators::sum_(opt(values)); };
    const auto minimum = [](wp<TS<List<i64>>> values) { return operators::min_(values); };
    const auto maximum = [](wp<TS<Set<i64>>> values) { return operators::max_(values); };
    const auto mean = [](wp<TS<List<i64>>> values) { return operators::mean(values); };
    const auto optional_minimum = [](wp<TS<List<i64>>> values) { return operators::min_(opt(values)); };
    const auto optional_maximum = [](wp<TS<Set<i64>>> values) { return operators::max_(opt(values)); };
    const auto optional_mean = [](wp<TS<List<i64>>> values) { return operators::mean(opt(values)); };
    const auto empty_or_invalid = hg::input<TS<List<i64>>>({{}, literal::INVALID, {4}});

    CHECK(hg::run_graph(list_sum, hg::input<TS<List<i64>>>({{1, 2, 3}, {}, {4, 5}}))
              .equals<i64>({6, 0, 9}));
    CHECK(hg::run_graph(set_sum, hg::input<TS<Set<i64>>>({{1, 2, 3}, {}, {4, 5}}))
              .equals<i64>({6, 0, 9}));
    CHECK(hg::run_graph(optional_sum,
                        hg::input<TS<Set<i64>>>({{1, 2}, {}, literal::INVALID, {3}}))
              .equals<i64>({3, 0, 0, 3}));
    CHECK(hg::run_graph(minimum, hg::input<TS<List<i64>>>({{1, 2, 3}, {}, {4, 5}}))
              .equals<i64>({1, std::nullopt, 4}));
    CHECK(hg::run_graph(maximum, hg::input<TS<Set<i64>>>({{1, 2, 3}, {}, {4, 5}}))
              .equals<i64>({3, std::nullopt, 5}));
    CHECK(hg::run_graph(mean, hg::input<TS<List<i64>>>({{1, 2, 3}, {}, {4, 5}}))
              .equals<double>({2.0, std::nullopt, 4.5}));
    CHECK(hg::run_graph(optional_minimum, empty_or_invalid).equals<i64>({std::nullopt, std::nullopt, 4}));
    CHECK(hg::run_graph(optional_maximum, hg::input<TS<Set<i64>>>({{}, literal::INVALID, {4}}))
              .equals<i64>({std::nullopt, std::nullopt, 4}));
    CHECK(hg::run_graph(optional_mean, empty_or_invalid).equals<double>({std::nullopt, std::nullopt, 4.0}));
}

TEST_CASE("scalar list and set reductions support double values",
          "[feature:operator][series:value][scalar:list][scalar:set][scalar:double]") {
    const auto list_sum = [](wp<TS<List<double>>> values) { return operators::sum_(values); };
    const auto list_min = [](wp<TS<List<double>>> values) { return operators::min_(values); };
    const auto list_max = [](wp<TS<List<double>>> values) { return operators::max_(values); };
    const auto list_mean = [](wp<TS<List<double>>> values) { return operators::mean(values); };
    const auto set_sum = [](wp<TS<Set<double>>> values) { return operators::sum_(values); };
    const auto set_min = [](wp<TS<Set<double>>> values) { return operators::min_(values); };
    const auto set_max = [](wp<TS<Set<double>>> values) { return operators::max_(values); };
    const auto set_mean = [](wp<TS<Set<double>>> values) { return operators::mean(values); };
    const auto list_input = hg::input<TS<List<double>>>({{1.5, 2.5}, {}, {-3.0, 4.0}});
    const auto set_input = hg::input<TS<Set<double>>>({{1.5, 2.5}, {}, {-3.0, 4.0}});

    CHECK(hg::run_graph(list_sum, list_input).equals<double>({4.0, 0.0, 1.0}));
    CHECK(hg::run_graph(list_min, list_input).equals<double>({1.5, std::nullopt, -3.0}));
    CHECK(hg::run_graph(list_max, list_input).equals<double>({2.5, std::nullopt, 4.0}));
    CHECK(hg::run_graph(list_mean, list_input).equals<double>({2.0, std::nullopt, 0.5}));
    CHECK(hg::run_graph(set_sum, set_input).equals<double>({4.0, 0.0, 1.0}));
    CHECK(hg::run_graph(set_min, set_input).equals<double>({1.5, std::nullopt, -3.0}));
    CHECK(hg::run_graph(set_max, set_input).equals<double>({2.5, std::nullopt, 4.0}));
    CHECK(hg::run_graph(set_mean, set_input).equals<double>({2.0, std::nullopt, 0.5}));
}

TEST_CASE("scalar list and set i64 sums wrap at the scalar width",
          "[feature:operator][series:value][scalar:list][scalar:set][scalar:i64]") {
    const auto list_sum = [](wp<TS<List<i64>>> values) { return operators::sum_(values); };
    const auto set_sum = [](wp<TS<Set<i64>>> values) { return operators::sum_(values); };

    CHECK(hg::run_graph(list_sum, hg::input<TS<List<i64>>>({{std::numeric_limits<i64>::max(), 1}}))
              .equals<i64>({std::numeric_limits<i64>::min()}));
    CHECK(hg::run_graph(set_sum, hg::input<TS<Set<i64>>>({{std::numeric_limits<i64>::max(), 1}}))
              .equals<i64>({std::numeric_limits<i64>::min()}));
}

TEST_CASE("scalar list and set Boolean reductions use empty identities",
          "[feature:operator][series:value][scalar:list][scalar:set][scalar:boolean]"
          "[validity:optional][transition:invalidation][transition:revalidation]") {
    const auto all_list = [](wp<TS<List<bool>>> values) { return operators::all_(values); };
    const auto any_list = [](wp<TS<List<bool>>> values) { return operators::any_(values); };
    const auto all_set = [](wp<TS<Set<bool>>> values) { return operators::all_(values); };
    const auto any_set = [](wp<TS<Set<bool>>> values) { return operators::any_(values); };
    const auto optional_all = [](wp<TS<Set<bool>>> values) { return operators::all_(opt(values)); };
    const auto optional_any_list = [](wp<TS<List<bool>>> values) { return operators::any_(opt(values)); };
    const auto optional_all_list = [](wp<TS<List<bool>>> values) { return operators::all_(opt(values)); };
    const auto optional_any_set = [](wp<TS<Set<bool>>> values) { return operators::any_(opt(values)); };

    const auto list_input = hg::input<TS<List<bool>>>({{true, false}, {}, {true}, literal::INVALID});
    CHECK(hg::run_graph(all_list, list_input).equals<bool>({false, true, true, std::nullopt}));
    CHECK(hg::run_graph(any_list, list_input).equals<bool>({true, false, true, std::nullopt}));

    const auto set_input = hg::input<TS<Set<bool>>>({{true, false}, {}, {true}, literal::INVALID});
    CHECK(hg::run_graph(all_set, set_input).equals<bool>({false, true, true, std::nullopt}));
    CHECK(hg::run_graph(any_set, set_input).equals<bool>({true, false, true, std::nullopt}));
    CHECK(hg::run_graph(optional_all, set_input).equals<bool>({false, true, true, true}));
    CHECK(hg::run_graph(optional_any_list, list_input).equals<bool>({true, false, true, false}));
    CHECK(hg::run_graph(optional_all_list, list_input).equals<bool>({false, true, true, true}));
    CHECK(hg::run_graph(optional_any_set, set_input).equals<bool>({true, false, true, false}));
}

TEST_CASE("scalar set operations support string and nested set items",
          "[feature:operator][series:value][scalar:set][scalar:string]") {
    const auto string_union = [](wp<TS<Set<string>>> lhs, wp<TS<Set<string>>> rhs) {
        return lhs | rhs;
    };
    const auto nested_union = [](wp<TS<Set<Set<i64>>>> lhs, wp<TS<Set<Set<i64>>>> rhs) {
        return operators::bit_or(lhs, rhs);
    };

    CHECK(hg::run_graph(string_union, hg::input<TS<Set<string>>>({{"a", "b"}}),
                        hg::input<TS<Set<string>>>({{"b", "c"}}))
              .equals({{"a", "b", "c"}}));
    CHECK(hg::run_graph(nested_union,
                        hg::input<TS<Set<Set<i64>>>>({{{1, 2}, {3}}}),
                        hg::input<TS<Set<Set<i64>>>>({{{2}, {4}}}))
              .equals({{{1, 2}, {2}, {3}, {4}}}));
}

TEST_CASE("scalar set operations follow reference retargeting",
          "[feature:operator][series:value][series:reference][scalar:set][transition:retargeting]") {
    const auto graph = [](wp<TS<bool>> select_lhs, wp<TS<Set<i64>>> lhs, wp<TS<Set<i64>>> rhs) {
        return operators::bit_or(lhs, operators::if_then_else(select_lhs, lhs, rhs));
    };

    CHECK(hg::run_graph(graph, hg::input<TS<bool>>({false, true, false}),
                        hg::input<TS<Set<i64>>>({{1}, {1, 3}, {2}}),
                        hg::input<TS<Set<i64>>>({{2}, {4}, {2, 4}}))
              .equals({{1, 2}, {1, 3}, {2, 4}}));
}

TEST_CASE("scalar set operations preserve and enforce output guarantees",
          "[feature:operator][series:value][scalar:set][guarantee:const][guarantee:final]") {
    const auto const_source = node([](Out<TS<Set<i64>, TimeSeriesGuarantee::const_>> output) {
        output.set(std::array<i64, 2>{1, 2});
    });
    const auto final_source = node([](Out<TS<Set<i64>, TimeSeriesGuarantee::final>> output) {
        output.set(std::array<i64, 2>{1, 2});
    });

    testing::test_wiring([&] {
        const auto const_result = operators::bit_or(const_source(), const_source());
        const auto final_result = operators::bit_or(final_source(), final_source());
        CHECK(&const_result.type() == &TS<Set<i64>, TimeSeriesGuarantee::const_>::type());
        CHECK(&final_result.type() == &TS<Set<i64>, TimeSeriesGuarantee::final>::type());
    });
}

TEST_CASE("deduplicated scalar set inputs suppress equal set operation results",
          "[feature:operator][series:value][scalar:set][guarantee:deduplicated]") {
    const auto left = node([](In<TS<i64>> cycle, Out<TS<Set<i64>, TimeSeriesGuarantee::deduplicated>> output) {
        if (*cycle == 0) {
            output.set(std::array<i64, 1>{1});
        } else {
            output.set(std::array<i64, 2>{1, 2});
        }
    });
    const auto right = node([](Out<TS<Set<i64>, TimeSeriesGuarantee::deduplicated>> output) {
        output.set(std::array<i64, 2>{1, 2});
    });
    const auto graph = [&](wp<TS<i64>> cycle) {
        return operators::bit_or(left(cycle), right());
    };

    CHECK(hg::run_graph(graph, hg::input<TS<i64>>({0, 1, 2}))
              .equals({{1, 2}, literal::SKIP, literal::SKIP}));
}

TEST_CASE("scalar collection reductions resolve output guarantees",
          "[feature:operator][series:value][scalar:list][scalar:set][scalar:boolean][guarantee:const]"
          "[validity:optional]") {
    const auto const_list = node([](Out<TS<List<i64>, TimeSeriesGuarantee::const_>> output) {
        output.set(std::array<i64, 2>{1, 2});
    });
    const auto const_set = node([](Out<TS<Set<i64>, TimeSeriesGuarantee::const_>> output) {
        output.set(std::array<i64, 2>{1, 2});
    });
    const auto const_flags = node([](Out<TS<List<bool>, TimeSeriesGuarantee::const_>> output) {
        output.set(std::array<bool, 2>{true, false});
    });

    testing::test_wiring([&] {
        CHECK(&operators::sum_(const_list()).type() == &TS<i64, TimeSeriesGuarantee::const_>::type());
        CHECK(&operators::sum_(opt(const_list())).type() == &TS<i64>::type());
        CHECK(&operators::min_(const_set()).type() == &TS<i64, TimeSeriesGuarantee::final>::type());
        CHECK(&operators::max_(const_list()).type() == &TS<i64, TimeSeriesGuarantee::final>::type());
        CHECK(&operators::mean(const_set()).type() == &TS<double, TimeSeriesGuarantee::final>::type());
        CHECK(&operators::all_(const_flags()).type() == &TS<bool, TimeSeriesGuarantee::const_>::type());
        CHECK(&operators::any_(opt(const_flags())).type() == &TS<bool>::type());
    });
}
