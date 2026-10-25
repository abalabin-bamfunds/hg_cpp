#include <bg/library.hpp>
#include <bg/run_graph.hpp>
#include <bg/time_series/tsl/tsl.hpp>

#include <catch2/catch_test_macros.hpp>

#include <span>
#include <vector>

using namespace bg;
using namespace bg::literals;

TEST_CASE("Boolean TSL reductions read current item values",
          "[feature:operator][series:fixed-tsl][scalar:boolean][shape:single-node][transition:update]") {
    const auto all = [](wp<TS<bool>> first, wp<TS<bool>> second) {
        return operators::all_("values"_n = tsl(first, second));
    };

    const auto any = [](wp<TS<bool>> first, wp<TS<bool>> second) {
        return operators::any_("values"_n = tsl(first, second));
    };

    CHECK(bg::run_graph(all, {true, false, false, true}, {true, true, false, true})
              .equals<bool>({true, false, false, true}));
    CHECK(bg::run_graph(any, {true, false, false, true}, {true, true, false, true})
              .equals<bool>({true, true, false, true}));
}

TEST_CASE("optional Boolean TSL reductions skip invalid items and use empty identities",
          "[feature:operator][series:fixed-tsl][scalar:boolean][validity:optional]"
          "[transition:invalidation][transition:revalidation]") {
    const auto all = [](wp<TS<bool>> first, wp<TS<bool>> second) {
        return operators::all_("values"_n = opt(tsl(first, second)));
    };

    const auto any = [](wp<TS<bool>> first, wp<TS<bool>> second) {
        return operators::any_("values"_n = opt(tsl(first, second)));
    };

    CHECK(bg::run_graph(all,
                              {false, literal::INVALID, literal::INVALID, true},
                              {true, false, literal::INVALID, literal::INVALID})
              .equals<bool>({false, false, true, true}));
    CHECK(bg::run_graph(any,
                              {false, literal::INVALID, literal::INVALID, true},
                              {true, false, literal::INVALID, literal::INVALID})
              .equals<bool>({true, false, false, true}));
}

TEST_CASE("required Boolean TSL reductions wait for every immediate item",
          "[feature:operator][series:fixed-tsl][scalar:boolean][validity:all-valid]"
          "[transition:invalidation][transition:revalidation]") {
    const auto all = [](wp<TS<bool>> first, wp<TS<bool>> second) {
        return operators::all_("values"_n = tsl(first, second));
    };

    const auto any = [](wp<TS<bool>> first, wp<TS<bool>> second) {
        return operators::any_("values"_n = tsl(first, second));
    };

    CHECK(bg::run_graph(all, {true, literal::INVALID, true}, {true, true, false})
              .equals<bool>({true, std::nullopt, false}));
    CHECK(bg::run_graph(any, {false, literal::INVALID, false}, {true, true, false})
              .equals<bool>({true, std::nullopt, false}));
}

TEST_CASE("Boolean reductions read direct TSL inputs and report required output validity",
          "[feature:operator][series:fixed-tsl][scalar:boolean][shape:single-node]"
          "[validity:all-valid][validity:optional][transition:invalidation][transition:revalidation]") {
    const auto all = [](wp<TSL<TS<bool>, 2>> values) { return operators::all_(values); };

    const auto any = [](wp<TSL<TS<bool>, 2>> values) { return operators::any_(values); };

    const auto optional_all = [](wp<TSL<TS<bool>, 2>> values) { return operators::all_(opt(values)); };

    const auto optional_any = [](wp<TSL<TS<bool>, 2>> values) { return operators::any_(opt(values)); };

    const auto all_valid = [](wp<TSL<TS<bool>, 2>> values) {
        return operators::valid(operators::all_(values));
    };

    CHECK(bg::run_graph(all, {literal::SKIP, {{0, true}}, {{1, true}}, {{1, literal::INVALID}},
                                    {{1, false}}})
              .equals<bool>({std::nullopt, std::nullopt, true, std::nullopt, false}));
    CHECK(bg::run_graph(any, {literal::SKIP, {{0, true}}, {{1, true}}, {{1, literal::INVALID}},
                                    {{1, false}}})
              .equals<bool>({std::nullopt, std::nullopt, true, std::nullopt, true}));
    CHECK(bg::run_graph(optional_all, {literal::SKIP, {{0, true}}, {{1, true}},
                                            {{1, literal::INVALID}}, {{1, false}}})
              .equals<bool>({std::nullopt, true, true, true, false}));
    CHECK(bg::run_graph(optional_any, {literal::SKIP, {{0, true}}, {{1, true}},
                                            {{1, literal::INVALID}}, {{1, false}}})
              .equals<bool>({std::nullopt, true, true, true, true}));
    CHECK(bg::run_graph(all_valid, {literal::SKIP, {{0, true}}, {{1, true}},
                                         {{1, literal::INVALID}}, {{1, false}}})
              .equals<bool>({false, std::nullopt, true, false, true}));
}

TEST_CASE("optional Boolean TSL reductions treat an invalid root as empty",
          "[feature:operator][series:fixed-tsl][scalar:boolean][validity:optional]"
          "[transition:invalidation][transition:revalidation]") {
    const auto all = [](wp<TSL<TS<bool>, 2>> values) { return operators::all_(opt(values)); };

    const auto any = [](wp<TSL<TS<bool>, 2>> values) { return operators::any_(opt(values)); };

    CHECK(bg::run_graph(all, {literal::SKIP, {{0, false}}, {{0, literal::INVALID}}, {{1, true}}})
              .equals<bool>({std::nullopt, false, true, true}));
    CHECK(bg::run_graph(any, {literal::SKIP, {{0, false}}, {{0, literal::INVALID}}, {{1, true}}})
              .equals<bool>({std::nullopt, false, false, true}));
}

TEST_CASE("Boolean TSD reductions track updates, invalid items, removals, and empty membership",
          "[feature:operator][series:tsd][scalar:boolean][shape:single-node][validity:all-valid]"
          "[validity:optional][transition:update][transition:invalidation][transition:removal]") {
    const auto optional_all = [](wp<TSD<i64, TS<bool>>> values) {
        return operators::all_("values"_n = opt(values));
    };

    const auto optional_any = [](wp<TSD<i64, TS<bool>>> values) {
        return operators::any_("values"_n = opt(values));
    };

    CHECK(bg::run_graph(operators::all_, bg::input<TSD<i64, TS<bool>>>(
                                                {{{1, true}, {2, true}},
                                                 {{1, false}, {2, literal::REMOVE}},
                                                 {{1, literal::INVALID}},
                                                 {{1, literal::REMOVE}}}))
              .equals<bool>({true, false, std::nullopt, true}));
    CHECK(bg::run_graph(operators::any_, bg::input<TSD<i64, TS<bool>>>(
                                                {{{1, true}, {2, true}},
                                                 {{1, false}, {2, literal::REMOVE}},
                                                 {{1, literal::INVALID}},
                                                 {{1, literal::REMOVE}}}))
              .equals<bool>({true, false, std::nullopt, false}));
    CHECK(bg::run_graph(optional_all, {{{1, true}, {2, true}},
                                             {{1, false}, {2, literal::REMOVE}},
                                             {{1, literal::INVALID}},
                                             {{1, literal::REMOVE}}})
              .equals<bool>({true, false, true, true}));
    CHECK(bg::run_graph(optional_any, {{{1, true}, {2, true}},
                                             {{1, false}, {2, literal::REMOVE}},
                                             {{1, literal::INVALID}},
                                             {{1, literal::REMOVE}}})
              .equals<bool>({true, false, false, false}));
}

TEST_CASE("Boolean TSS reductions use membership and empty identities",
          "[feature:operator][series:tss][scalar:boolean][transition:addition][transition:removal]") {
    CHECK(bg::run_graph(operators::all_, bg::input<TSS<bool>>(
                                                {{}, {false}, {literal::REMOVED(false), true},
                                                 {literal::REMOVED(true)}}))
              .equals<bool>({true, false, true, true}));
    CHECK(bg::run_graph(operators::any_, bg::input<TSS<bool>>(
                                                {{}, {false}, {literal::REMOVED(false), true},
                                                 {literal::REMOVED(true)}}))
              .equals<bool>({false, false, true, false}));
}

TEST_CASE("optional Boolean TSS reductions treat an invalid selected target as empty",
          "[feature:operator][series:tss][series:reference][scalar:boolean][validity:optional]"
          "[transition:invalidation][transition:revalidation]") {
    const auto all = [](wp<TS<bool>> select_invalid, wp<TSS<bool>> populated, wp<TSS<bool>> invalid) {
        const auto values = operators::if_then_else(select_invalid, invalid, populated);
        return operators::all_(opt(values));
    };

    const auto any = [](wp<TS<bool>> select_invalid, wp<TSS<bool>> populated, wp<TSS<bool>> invalid) {
        const auto values = operators::if_then_else(select_invalid, invalid, populated);
        return operators::any_(opt(values));
    };

    CHECK(bg::run_graph(all, {false, true, false}, {{false}, literal::SKIP, literal::SKIP},
                              {literal::SKIP, literal::SKIP, literal::SKIP})
              .equals<bool>({false, true, false}));
    CHECK(bg::run_graph(any, {false, true, false}, {{true}, literal::SKIP, literal::SKIP},
                              {literal::SKIP, literal::SKIP, literal::SKIP})
              .equals<bool>({true, false, true}));
}

TEST_CASE("Boolean reductions preserve const, final, and deduplicated guarantees",
          "[feature:operator][series:fixed-tsl][series:tsd][series:tss][scalar:boolean]"
          "[guarantee:const][guarantee:final][guarantee:deduplicated]") {
    const auto const_values = node([](Out<TSL<TS<bool, TimeSeriesGuarantee::const_>, 2,
                                               TimeSeriesGuarantee::const_>> output) {
        output[0].set(true);
        output[1].set(true);
    });

    const auto check_const_tsl = [&] {
        const auto values = const_values();
        const auto required = operators::all_("values"_n = values);
        const auto optional = operators::any_("values"_n = opt(values));
        CHECK(&required.type() == &TS<bool, TimeSeriesGuarantee::const_>::type());
        CHECK(&optional.type() == &TS<bool>::type());
        return required;
    };

    const auto final_values = node([](Out<TSL<TS<bool, TimeSeriesGuarantee::final>, 2,
                                               TimeSeriesGuarantee::final>> output) {
        output[0].set(false);
        output[1].set(true);
    });

    const auto check_final_tsl = [&] {
        const auto items = final_values();
        const auto required = operators::any_("values"_n = items);
        const auto optional = operators::all_("values"_n = opt(items));
        CHECK(&required.type() == &TS<bool, TimeSeriesGuarantee::final>::type());
        CHECK(&optional.type() == &TS<bool>::type());
        return required;
    };

    const auto deduplicated_values = node([](Out<TSL<TS<bool, TimeSeriesGuarantee::deduplicated>, 2,
                                                      TimeSeriesGuarantee::deduplicated>> output) {
        output[0].set(true);
        output[1].set(true);
    });

    const auto check_deduplicated_tsl = [&] {
        const auto items = deduplicated_values();
        const auto required = operators::all_("values"_n = items);
        const auto optional = operators::any_("values"_n = opt(items));
        CHECK(&required.type() == &TS<bool, TimeSeriesGuarantee::deduplicated>::type());
        CHECK(&optional.type() == &TS<bool>::type());
        return required;
    };

    const auto final_dictionary = node([](Out<TSD<i64, TS<bool, TimeSeriesGuarantee::final>,
                                                     TimeSeriesGuarantee::final>> output) {
        output[1].set(true);
    });

    const auto check_final_tsd = [&] {
        const auto values = final_dictionary();
        const auto required = operators::all_("values"_n = values);
        CHECK(&required.type() == &TS<bool, TimeSeriesGuarantee::final>::type());
        return required;
    };

    const auto deduplicated_dictionary = node([](Out<TSD<i64, TS<bool, TimeSeriesGuarantee::deduplicated>,
                                                                TimeSeriesGuarantee::deduplicated>> output) {
        output[1].set(true);
    });

    const auto check_deduplicated_tsd = [&] {
        const auto values = deduplicated_dictionary();
        const auto required = operators::any_("values"_n = values);
        CHECK(&required.type() == &TS<bool, TimeSeriesGuarantee::deduplicated>::type());
        return required;
    };

    const auto const_set = node([](Out<TSS<bool, TimeSeriesGuarantee::const_>> output) { output.apply({true}); });

    const auto check_const_tss = [&] {
        const auto values = const_set();
        const auto required = operators::all_("values"_n = values);
        CHECK(&required.type() == &TS<bool, TimeSeriesGuarantee::const_>::type());
        return required;
    };

    const auto final_set = node([](Out<TSS<bool, TimeSeriesGuarantee::final>> output) { output.apply({false}); });

    const auto check_final_tss = [&] {
        const auto values = final_set();
        const auto required = operators::any_("values"_n = values);
        CHECK(&required.type() == &TS<bool, TimeSeriesGuarantee::final>::type());
        return required;
    };

    const auto deduplicated_set = node([](Out<TSS<bool, TimeSeriesGuarantee::deduplicated>> output) {
        output.apply({true});
    });

    const auto check_deduplicated_tss = [&] {
        const auto values = deduplicated_set();
        const auto required = operators::all_("values"_n = values);
        CHECK(&required.type() == &TS<bool, TimeSeriesGuarantee::deduplicated>::type());
        return required;
    };

    CHECK(bg::run_graph(check_const_tsl).equals<bool>({true}));
    CHECK(bg::run_graph(check_final_tsl).equals<bool>({true}));
    CHECK(bg::run_graph(check_deduplicated_tsl).equals<bool>({true}));
    CHECK(bg::run_graph(check_final_tsd).equals<bool>({true}));
    CHECK(bg::run_graph(check_deduplicated_tsd).equals<bool>({true}));
    CHECK(bg::run_graph(check_const_tss).equals<bool>({true}));
    CHECK(bg::run_graph(check_final_tss).equals<bool>({false}));
    CHECK(bg::run_graph(check_deduplicated_tss).equals<bool>({true}));
}

TEST_CASE("deduplicated Boolean reductions suppress equal aggregate results",
          "[feature:operator][series:tss][scalar:boolean][guarantee:deduplicated][transition:addition]") {
    const auto add_value = node([](In<TS<bool>> value,
                                   Out<TSS<bool, TimeSeriesGuarantee::deduplicated>> output) {
        output.apply({*value});
    });

    const auto all = [&](wp<TS<bool>> values) { return operators::all_(add_value(values)); };
    const auto any = [&](wp<TS<bool>> values) { return operators::any_(add_value(values)); };

    CHECK(bg::run_graph(all, {false, true}).equals<bool>({false, std::nullopt}));
    CHECK(bg::run_graph(any, {true, false}).equals<bool>({true, std::nullopt}));
}

TEST_CASE("Boolean reductions accept free TSL inputs and nested operator results",
          "[feature:operator][series:fixed-tsl][scalar:boolean][representation:free][shape:chain]") {
    const auto all = [](wp<TS<bool>> first, wp<TS<bool>> second) {
        const auto items = std::vector<WiringPoint>{first, second};
        return operators::all_("values"_n = tsl(std::span<const WiringPoint>{items}));
    };

    const auto not_any = [](wp<TS<bool>> first, wp<TS<bool>> second) {
        return operators::not_(operators::any_("values"_n = tsl(first, second)));
    };

    CHECK(bg::run_graph(all, {true, false, true}, {true, true, false}).equals<bool>({true, false, false}));
    CHECK(bg::run_graph(not_any, {false, false, true}, {false, true, false})
              .equals<bool>({true, false, false}));
}

TEST_CASE("Boolean reductions support valid-items views of TSD inputs",
          "[feature:operator][series:tsd][scalar:boolean][input-guarantee:valid-items]"
          "[transition:invalidation][transition:removal]") {
    const auto all_filtered = [](wp<TSD<i64, TS<bool>>> values) {
        return operators::all_("values"_n = valid_items(values));
    };

    const auto any_filtered = [](wp<TSD<i64, TS<bool>>> values) {
        return operators::any_("values"_n = valid_items(values));
    };

    const auto optional_all_filtered = [](wp<TSD<i64, TS<bool>>> values) {
        return operators::all_("values"_n = opt(valid_items(values)));
    };

    const auto optional_any_filtered = [](wp<TSD<i64, TS<bool>>> values) {
        return operators::any_("values"_n = opt(valid_items(values)));
    };
    CHECK(bg::run_graph(all_filtered, {{{1, true}, {2, false}},
                                            {{1, literal::REMOVE}, {2, literal::INVALID}},
                                            {{2, literal::REMOVE}}})
              .equals<bool>({false, true, true}));
    CHECK(bg::run_graph(any_filtered, {{{1, true}, {2, false}},
                                            {{1, literal::REMOVE}, {2, literal::INVALID}},
                                            {{2, literal::REMOVE}}})
              .equals<bool>({true, false, false}));
    CHECK(bg::run_graph(optional_all_filtered, {{{1, true}, {2, false}},
                                                     {{1, literal::REMOVE}, {2, literal::INVALID}},
                                                     {{2, literal::REMOVE}}})
              .equals<bool>({false, true, true}));
    CHECK(bg::run_graph(optional_any_filtered, {{{1, true}, {2, false}},
                                                     {{1, literal::REMOVE}, {2, literal::INVALID}},
                                                     {{2, literal::REMOVE}}})
              .equals<bool>({true, false, false}));
}

TEST_CASE("Boolean TSD reductions follow reference target updates and retargeting",
          "[feature:operator][series:tsd][series:reference][scalar:boolean][binding:reference-to-value]"
          "[transition:update]") {
    const auto all = [](wp<TS<bool>> select_first, wp<TSD<i64, TS<bool>>> first,
                        wp<TSD<i64, TS<bool>>> second) {
        return operators::all_(operators::if_then_else(select_first, first, second));
    };

    const auto any = [](wp<TS<bool>> select_first, wp<TSD<i64, TS<bool>>> first,
                        wp<TSD<i64, TS<bool>>> second) {
        return operators::any_(operators::if_then_else(select_first, first, second));
    };

    CHECK(bg::run_graph(all, {true, true, false, literal::SKIP},
                              {{{1, false}}, {{1, true}}, literal::SKIP, {{1, false}}},
                              {{{1, true}}, literal::SKIP, {{1, false}}, literal::SKIP})
              .equals<bool>({false, true, false, std::nullopt}));
    CHECK(bg::run_graph(any, {true, true, false, literal::SKIP},
                              {{{1, false}}, {{1, true}}, literal::SKIP, {{1, false}}},
                              {{{1, true}}, literal::SKIP, {{1, false}}, literal::SKIP})
              .equals<bool>({false, true, false, std::nullopt}));
}

TEST_CASE("optional Boolean TSD reductions use empty identities for invalid reference targets",
          "[feature:operator][series:tsd][series:reference][scalar:boolean][validity:optional]"
          "[transition:invalidation][transition:revalidation]") {
    const auto all = [](wp<TS<bool>> choose_invalid, wp<TSD<i64, TS<bool>>> populated,
                        wp<TSD<i64, TS<bool>>> invalid) {
        const auto values = operators::if_then_else(choose_invalid, invalid, populated);
        return operators::all_("values"_n = opt(values));
    };

    const auto any = [](wp<TS<bool>> choose_invalid, wp<TSD<i64, TS<bool>>> populated,
                        wp<TSD<i64, TS<bool>>> invalid) {
        const auto values = operators::if_then_else(choose_invalid, invalid, populated);
        return operators::any_("values"_n = opt(values));
    };

    CHECK(bg::run_graph(all, {false, true, false}, {{{1, false}}, literal::SKIP, literal::SKIP},
                              {literal::SKIP, literal::SKIP, literal::SKIP})
              .equals<bool>({false, true, false}));
    CHECK(bg::run_graph(any, {false, true, false}, {{{1, true}}, literal::SKIP, literal::SKIP},
                              {literal::SKIP, literal::SKIP, literal::SKIP})
              .equals<bool>({true, false, true}));
}

TEST_CASE("Boolean reductions return identities for empty collections",
          "[feature:operator][series:fixed-tsl][series:tsd][series:tss][scalar:boolean]") {
    const auto empty_tsl = [&] {
        return operators::all_("values"_n = tsl<TS<bool>>());
    };

    const auto empty_tsl_any = [&] {
        return operators::any_("values"_n = tsl<TS<bool>>());
    };
    CHECK(bg::run_graph(empty_tsl).equals<bool>({true}));
    CHECK(bg::run_graph(empty_tsl_any).equals<bool>({false}));
    CHECK(bg::run_graph(operators::all_, bg::input<TSD<i64, TS<bool>>>({{}})).equals<bool>({true}));
    CHECK(bg::run_graph(operators::any_, bg::input<TSD<i64, TS<bool>>>({{}})).equals<bool>({false}));
    CHECK(bg::run_graph(operators::all_, bg::input<TSS<bool>>({{}})).equals<bool>({true}));
    CHECK(bg::run_graph(operators::any_, bg::input<TSS<bool>>({{}})).equals<bool>({false}));
}
