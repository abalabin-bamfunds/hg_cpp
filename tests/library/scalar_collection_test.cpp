#include <hg/library.hpp>
#include <hg/run_graph.hpp>
#include <hg/testing/test_wiring.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <limits>
#include <optional>
#include <utility>

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
          "[feature:operator][series:value][scalar:list][scalar:set][scalar:dict][scalar:i64]") {
    const auto list_sum = [](wp<TS<List<i64>>> values) { return operators::sum_(values); };
    const auto set_sum = [](wp<TS<Set<i64>>> values) { return operators::sum_(values); };
    const auto dictionary_sum = [](wp<TS<Dict<i64, i64>>> values) { return operators::sum_(values); };

    CHECK(hg::run_graph(list_sum, hg::input<TS<List<i64>>>({{std::numeric_limits<i64>::max(), 1}}))
              .equals<i64>({std::numeric_limits<i64>::min()}));
    CHECK(hg::run_graph(set_sum, hg::input<TS<Set<i64>>>({{std::numeric_limits<i64>::max(), 1}}))
              .equals<i64>({std::numeric_limits<i64>::min()}));
    CHECK(hg::run_graph(dictionary_sum,
                        hg::input<TS<Dict<i64, i64>>>({{{100, std::numeric_limits<i64>::max()}, {200, 1}}}))
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

TEST_CASE("scalar dictionary reductions read values and define empty results",
          "[feature:operator][series:value][scalar:dict][scalar:i64][validity:optional]"
          "[transition:invalidation][transition:revalidation]") {
    const auto sum = [](wp<TS<Dict<i64, i64>>> values) { return operators::sum_(values); };
    const auto minimum = [](wp<TS<Dict<i64, i64>>> values) { return operators::min_(values); };
    const auto maximum = [](wp<TS<Dict<i64, i64>>> values) { return operators::max_(values); };
    const auto mean = [](wp<TS<Dict<i64, i64>>> values) { return operators::mean(values); };
    const auto optional_sum = [](wp<TS<Dict<i64, i64>>> values) { return operators::sum_(opt(values)); };
    const auto optional_minimum = [](wp<TS<Dict<i64, i64>>> values) { return operators::min_(opt(values)); };
    const auto optional_maximum = [](wp<TS<Dict<i64, i64>>> values) { return operators::max_(opt(values)); };
    const auto optional_mean = [](wp<TS<Dict<i64, i64>>> values) { return operators::mean(opt(values)); };
    const auto values = hg::input<TS<Dict<i64, i64>>>(
        {{{101, 7}, {202, 2}}, {}, literal::INVALID, {{303, -4}, {404, 8}}});

    CHECK(hg::run_graph(sum, values).equals<i64>({9, 0, std::nullopt, 4}));
    CHECK(hg::run_graph(minimum, values).equals<i64>({2, std::nullopt, std::nullopt, -4}));
    CHECK(hg::run_graph(maximum, values).equals<i64>({7, std::nullopt, std::nullopt, 8}));
    CHECK(hg::run_graph(mean, values).equals<double>({4.5, std::nullopt, std::nullopt, 2.0}));

    CHECK(hg::run_graph(optional_sum, values).equals<i64>({9, 0, 0, 4}));
    CHECK(hg::run_graph(optional_minimum, values).equals<i64>({2, std::nullopt, std::nullopt, -4}));
    CHECK(hg::run_graph(optional_maximum, values).equals<i64>({7, std::nullopt, std::nullopt, 8}));
    CHECK(hg::run_graph(optional_mean, values).equals<double>({4.5, std::nullopt, std::nullopt, 2.0}));
}

TEST_CASE("scalar dictionary floating-point reductions use dictionary values",
          "[feature:operator][series:value][scalar:dict][scalar:floating-point]") {
    const auto sum = [](wp<TS<Dict<string, double>>> values) { return operators::sum_(values); };
    const auto minimum = [](wp<TS<Dict<string, double>>> values) { return operators::min_(values); };
    const auto maximum = [](wp<TS<Dict<string, double>>> values) { return operators::max_(values); };
    const auto mean = [](wp<TS<Dict<string, double>>> values) { return operators::mean(values); };

    const auto values = hg::input<TS<Dict<string, double>>>({{{"large-key", -1.5}, {"other-key", 2.5}}, {}});
    CHECK(hg::run_graph(sum, values).equals<double>({1.0, 0.0}));
    CHECK(hg::run_graph(minimum, values).equals<double>({-1.5, std::nullopt}));
    CHECK(hg::run_graph(maximum, values).equals<double>({2.5, std::nullopt}));
    CHECK(hg::run_graph(mean, values).equals<double>({0.5, std::nullopt}));
}

TEST_CASE("scalar dictionary Boolean reductions use empty identities",
          "[feature:operator][series:value][scalar:dict][scalar:boolean][validity:optional]") {
    const auto all = [](wp<TS<Dict<i64, bool>>> values) { return operators::all_(values); };
    const auto any = [](wp<TS<Dict<i64, bool>>> values) { return operators::any_(values); };
    const auto optional_all = [](wp<TS<Dict<i64, bool>>> values) { return operators::all_(opt(values)); };
    const auto optional_any = [](wp<TS<Dict<i64, bool>>> values) { return operators::any_(opt(values)); };
    const auto values = hg::input<TS<Dict<i64, bool>>>({{{100, true}, {200, false}}, {}, literal::INVALID,
                                                        {{300, true}}});

    CHECK(hg::run_graph(all, values).equals<bool>({false, true, std::nullopt, true}));
    CHECK(hg::run_graph(any, values).equals<bool>({true, false, std::nullopt, true}));
    CHECK(hg::run_graph(optional_all, values).equals<bool>({false, true, true, true}));
    CHECK(hg::run_graph(optional_any, values).equals<bool>({true, false, false, true}));
}

TEST_CASE("scalar dictionary subtraction republishes equal results for ordinary inputs",
          "[feature:operator][series:value][scalar:dict][guarantee:ordinary][transition:update]") {
    const auto difference = [](wp<TS<Dict<i64, i64>>> lhs, wp<TS<Dict<i64, string>>> rhs) { return lhs - rhs; };

    CHECK(hg::run_graph(difference, hg::input<TS<Dict<i64, i64>, TimeSeriesGuarantee::const_>>({{{100, 5}, {200, 7}}}),
                        hg::input<TS<Dict<i64, string>>>({{{100, "first"}}, {{100, "second"}}}))
              .equals({{{200, 7}}, {{200, 7}}}));
}

TEST_CASE("scalar dictionary subtraction suppresses equal results for deduplicated inputs",
          "[feature:operator][series:value][scalar:dict][guarantee:deduplicated][transition:update]") {
    const auto difference = [](wp<TS<Dict<i64, i64>>> lhs, wp<TS<Dict<i64, string>>> rhs) {
        return operators::sub(operators::dedup(lhs), operators::dedup(rhs));
    };

    CHECK(hg::run_graph(difference, hg::input<TS<Dict<i64, i64>>>({{{100, 5}, {200, 7}}}),
                        hg::input<TS<Dict<i64, string>>>({{{100, "first"}}, {{100, "second"}}, {{100, "second"}}}))
              .equals({{{200, 7}}, literal::SKIP, literal::SKIP}));
}

TEST_CASE("scalar dictionary reductions and subtraction resolve guarantees",
          "[feature:operator][series:value][scalar:dict][guarantee:const][guarantee:final][validity:optional]") {
    const auto const_dictionary = node([](Out<TS<Dict<i64, i64>, TimeSeriesGuarantee::const_>> output) {
        output.set(std::array<std::pair<i64, i64>, 1>{{{100, 1}}});
    });
    const auto final_dictionary = node([](Out<TS<Dict<i64, i64>, TimeSeriesGuarantee::final>> output) {
        output.set(std::array<std::pair<i64, i64>, 1>{{{200, 2}}});
    });
    const auto const_flags = node([](Out<TS<Dict<i64, bool>, TimeSeriesGuarantee::const_>> output) {
        output.set(std::array<std::pair<i64, bool>, 1>{{{100, true}}});
    });

    testing::test_wiring([&] {
        CHECK(&operators::sum_(const_dictionary()).type() == &TS<i64, TimeSeriesGuarantee::const_>::type());
        CHECK(&operators::sum_(opt(const_dictionary())).type() == &TS<i64>::type());
        CHECK(&operators::min_(const_dictionary()).type() == &TS<i64, TimeSeriesGuarantee::final>::type());
        CHECK(&operators::mean(const_dictionary()).type() == &TS<double, TimeSeriesGuarantee::final>::type());
        CHECK(&operators::all_(const_flags()).type() == &TS<bool, TimeSeriesGuarantee::const_>::type());
        CHECK(&operators::any_(opt(const_flags())).type() == &TS<bool>::type());
        CHECK(&operators::sub(const_dictionary(), final_dictionary()).type() ==
              &TS<Dict<i64, i64>, TimeSeriesGuarantee::final>::type());
        CHECK(&operators::sub(opt(const_dictionary()), final_dictionary()).type() ==
              &TS<Dict<i64, i64>>::type());
    });
}

TEST_CASE("scalar dictionary subtraction selects left entries by key",
          "[feature:operator][series:value][scalar:dict][scalar:string][validity:optional]"
          "[transition:invalidation][transition:revalidation]") {
    const auto difference = [](wp<TS<Dict<string, string>>> lhs, wp<TS<Dict<string, i64>>> rhs) {
        return lhs - rhs;
    };
    const auto optional_difference = [](wp<TS<Dict<string, string>>> lhs, wp<TS<Dict<string, i64>>> rhs) {
        return opt(lhs) - rhs;
    };
    const auto lhs = hg::input<TS<Dict<string, string>>>(
        {{{"remove", "first"}, {"keep", "left-value"}}, {{"remove", "second"}}, literal::INVALID,
         {{"keep", "last"}}});
    const auto rhs = hg::input<TS<Dict<string, i64>>>({{{"remove", 1}}, {{"keep", 2}}, {}, literal::INVALID});

    const auto required_result = hg::run_graph(difference, lhs, rhs);
    CHECK(required_result.equals(
        {{{"keep", "left-value"}}, {{"remove", "second"}}, literal::INVALID, literal::SKIP}));
    const auto optional_result = hg::run_graph(optional_difference, lhs, rhs);
    CHECK(optional_result.equals({{{"keep", "left-value"}}, {{"remove", "second"}}, {}, {{"keep", "last"}}}));

    const auto optional_rhs_difference = [](wp<TS<Dict<string, string>>> lhs,
                                            wp<TS<Dict<string, i64>>> rhs) { return lhs - opt(rhs); };
    CHECK(hg::run_graph(optional_rhs_difference, lhs, rhs)
              .equals({{{"keep", "left-value"}}, {{"remove", "second"}}, {}, {{"keep", "last"}}}));
}

TEST_CASE("scalar dictionary reductions follow reference retargeting",
          "[feature:operator][series:value][series:reference][scalar:dict][binding:reference-to-value]"
          "[transition:update]") {
    const auto graph = [](wp<TS<bool>> select_lhs, wp<TS<Dict<i64, i64>>> lhs, wp<TS<Dict<i64, i64>>> rhs) {
        return operators::sum_(operators::if_then_else(select_lhs, lhs, rhs));
    };

    CHECK(hg::run_graph(graph, hg::input<TS<bool>>({false, true, false}),
                        hg::input<TS<Dict<i64, i64>>>({{{100, 2}}, {{100, 4}}, {{100, 6}}}),
                        hg::input<TS<Dict<i64, i64>>>({{{200, 3}}, {{200, 5}}, {{200, 7}}}))
              .equals<i64>({3, 4, 7}));
}

TEST_CASE("scalar dictionary reductions work in nested maps and switches",
          "[feature:operator][feature:map][feature:switch][feature:nested-graph][series:value][series:tsd]"
          "[scalar:dict][shape:nested]") {
    const auto sum_values = graph([](wp<TS<Dict<i64, i64>>> values) { return operators::sum_(values); });
    const auto minimum = graph([](wp<TS<Dict<i64, i64>>> values) { return operators::min_(values); });
    const auto materialize = node([](In<TSD<i64, TS<i64>>> input, Out<TSD<i64, TS<i64>>> output) {
        output.copy_tick(input);
    });
    const auto map_values = [&](wp<TSD<i64, TS<Dict<i64, i64>>>> values) {
        return materialize(map(sum_values, values));
    };
    const auto switch_values = [&](wp<TS<i64>> selector, wp<TS<Dict<i64, i64>>> values) {
        return switch_(selector, cases{case_(i64{0}, sum_values), case_(i64{1}, minimum)}, values);
    };

    CHECK(hg::run_graph(map_values,
                        hg::input<TSD<i64, TS<Dict<i64, i64>>>>({{{1, {{100, 3}, {200, 4}}}, {2, {{300, 7}}}},
                                                                 {{1, {{100, 5}}}}}))
              .equals({{{1, 7}, {2, 7}}, {{1, 5}}}));
    CHECK(hg::run_graph(switch_values, hg::input<TS<i64>>({0, 1, 0}),
                        hg::input<TS<Dict<i64, i64>>>({{{100, 3}, {200, 4}}, {{300, 7}}, {{400, 2}, {500, 5}}}))
              .equals<i64>({7, 7, 7}));
}
