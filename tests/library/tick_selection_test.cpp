#include <bg/graph.hpp>
#include <bg/library.hpp>
#include <bg/node.hpp>
#include <bg/run_graph.hpp>
#include <bg/testing/test_wiring.hpp>
#include <bg/time_series/tsb/tsb.hpp>
#include <bg/time_series/tsb/tsb_type.hpp>
#include <bg/time_series/tsd/tsd_type.hpp>
#include <bg/time_series/tsl/tsl.hpp>
#include <bg/time_series/tsl/tsl_type.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>

using namespace bg;
using namespace bg::literals;

TEST_CASE("take drop and step count valid events and preserve skipped ticks",
          "[feature:operator][series:value][transition:update]"
          "[transition:invalidation][transition:revalidation]") {
    const auto take = bg::graph([](wp<TS<i64>> value) { return operators::take(value, 2).as<TS<i64>>(); });
    const auto drop = bg::graph([](wp<TS<i64>> value) { return operators::drop(value, 2).as<TS<i64>>(); });
    const auto step = bg::graph([](wp<TS<i64>> value) { return operators::step(value, 2).as<TS<i64>>(); });

    CHECK(bg::run_graph(take, {1, literal::INVALID, 2, 3, literal::INVALID, 4})
              .equals({1, literal::INVALID, 2, literal::SKIP, literal::SKIP, literal::SKIP}));
    CHECK(bg::run_graph(drop, {1, literal::INVALID, 2, 3, literal::INVALID, 4})
              .equals({literal::SKIP, literal::SKIP, literal::SKIP, 3, literal::INVALID, 4}));
    CHECK(bg::run_graph(step, {1, 1, 2, literal::INVALID, 3, 4, 5})
              .equals({1, literal::SKIP, 2, literal::INVALID, literal::SKIP, 4, literal::SKIP}));
}

TEST_CASE("counted operators validate their counts and handle zero",
          "[feature:operator][feature:wiring]") {
    const auto take_none = bg::graph([](wp<TS<i64>> value) { return operators::take(value, 0).as<TS<i64>>(); });
    const auto drop_none = bg::graph([](wp<TS<i64>> value) { return operators::drop(value, 0).as<TS<i64>>(); });
    const auto step_every_event =
        bg::graph([](wp<TS<i64>> value) { return operators::step(value, 1).as<TS<i64>>(); });

    CHECK(bg::run_graph(take_none, {1, 2}).equals({literal::SKIP, literal::SKIP}));
    CHECK(bg::run_graph(drop_none, {1, 2}).equals({1, 2}));
    CHECK(bg::run_graph(step_every_event, {1, 2}).equals({1, 2}));
    CHECK_THROWS_AS(bg::run_graph(
                        [](wp<TS<i64>> value) { return operators::take(value, -1); }, {1}),
                    WiringError);
    CHECK_THROWS_AS(bg::run_graph(
                        [](wp<TS<i64>> value) { return operators::drop(value, -1); }, {1}),
                    WiringError);
    CHECK_THROWS_AS(bg::run_graph(
                        [](wp<TS<i64>> value) { return operators::step(value, 0); }, {1}),
                    WiringError);
}

TEST_CASE("initial skips and invalidations do not consume selected events",
          "[feature:operator][feature:validity][series:value]"
          "[transition:first-tick][transition:invalidation]") {
    const auto take = bg::graph([](wp<TS<i64>> value) { return operators::take(value, 2).as<TS<i64>>(); });
    const auto drop = bg::graph([](wp<TS<i64>> value) { return operators::drop(value, 1).as<TS<i64>>(); });
    const auto step = bg::graph([](wp<TS<i64>> value) { return operators::step(value, 2).as<TS<i64>>(); });
    CHECK(bg::run_graph(take, {literal::SKIP, literal::INVALID, 10, literal::SKIP, 20, 30})
              .equals({literal::SKIP, literal::SKIP, 10, literal::SKIP, 20, literal::SKIP}));
    CHECK(bg::run_graph(drop, {literal::SKIP, literal::INVALID, 10, literal::SKIP, 20, 30})
              .equals({literal::SKIP, literal::SKIP, literal::SKIP, literal::SKIP, 20, 30}));
    CHECK(bg::run_graph(step, {literal::SKIP, literal::INVALID, 10, literal::SKIP, 20, 30})
              .equals({literal::SKIP, literal::SKIP, 10, literal::SKIP, literal::SKIP, 30}));
}

TEST_CASE("drop and step reconcile skipped dictionary changes before publishing",
          "[feature:operator][series:tsd]"
          "[transition:addition][transition:removal]") {
    const auto drop_sum = bg::graph([](wp<TSD<i64, TS<i64>>> value) {
        return operators::sum_(operators::drop(value, 1)).as<TS<i64>>();
    });
    const auto step_sum = bg::graph([](wp<TSD<i64, TS<i64>>> value) {
        return operators::sum_(operators::step(value, 2)).as<TS<i64>>();
    });

    CHECK(bg::run_graph(drop_sum, {{{1, 10}, {2, 20}}, {{2, 21}, {3, 30}}, {{2, literal::REMOVE}, {4, 40}}})
              .equals({literal::SKIP, 61, 80}));
    CHECK(bg::run_graph(step_sum, {{{1, 10}, {2, 20}}, {{2, 21}, {3, 30}}, {{2, literal::REMOVE}, {4, 40}}})
              .equals({30, literal::SKIP, 80}));
}

template <template <typename, auto...> typename Field> struct changing_pair {
    Field<TS<i64>> first;
    Field<TS<i64>> second;
};

TEST_CASE("step copies the current free TSL and TSB after skipped updates",
          "[feature:operator][series:fixed-tsl][series:tsb][transition:update]") {
    const auto sum_bundle = node([](In<TSB<changing_pair>, all_valid> values) {
        return *values->first + *values->second;
    });
    const auto step_list = bg::graph([](wp<TS<i64>> first, wp<TS<i64>> second) {
        const auto values = tsl(std::array{first, second});
        return operators::sum_(operators::step(values, 2)).as<TS<i64>>();
    });
    const auto step_bundle = bg::graph([&](wp<TS<i64>> first, wp<TS<i64>> second) {
        const auto values = bundle<changing_pair>(first, second);
        return sum_bundle(operators::step(values, 2));
    });

    CHECK(bg::run_graph(step_list, {1, 2}, {10, literal::SKIP, 20})
              .equals({11, literal::SKIP, 22}));
    CHECK(bg::run_graph(step_bundle, {1, 2}, {10, literal::SKIP, 20})
              .equals({11, literal::SKIP, 22}));
}

TEST_CASE("step follows TSS and TSD references through invalid targets and revalidation",
          "[feature:operator][series:reference][series:tss][series:tsd]"
          "[transition:invalidation][transition:revalidation][transition:update]") {
    const auto select_set = node([](In<TS<i64>> cycle, In<REF<TSS<i64>>, passive> populated,
                                    In<REF<TSS<i64>>, passive> invalid, Out<REF<TSS<i64>>> output) {
        output.copy(*cycle == 1 ? invalid : populated);
    });
    const auto set_sum = bg::graph([&](wp<TS<i64>> cycle, wp<TSS<i64>> populated, wp<TSS<i64>> invalid) {
        return operators::sum_(operators::step(select_set(cycle, populated, invalid), 2)).as<TS<i64>>();
    });

    CHECK(bg::run_graph(set_sum,
                              std::array{bg::input<TS<i64>>({0, 1, 2, 3}),
                                         bg::input<TSS<i64>>({{1}, literal::SKIP, {2}, {3}}),
                                         bg::input<TSS<i64>>(ticks({}))})
              .equals({1, 0, literal::SKIP, 6}));

    const auto select_dictionary = node([](In<TS<i64>> cycle, In<REF<TSD<i64, TS<i64>>>, passive> populated,
                                           In<REF<TSD<i64, TS<i64>>>, passive> invalid,
                                           Out<REF<TSD<i64, TS<i64>>>> output) {
        output.copy(*cycle == 1 ? invalid : populated);
    });
    const auto dictionary_sum = bg::graph([&](wp<TS<i64>> cycle, wp<TSD<i64, TS<i64>>> populated,
                                              wp<TSD<i64, TS<i64>>> invalid) {
        return operators::sum_(operators::step(select_dictionary(cycle, populated, invalid), 2))
            .as<TS<i64>>();
    });

    CHECK(bg::run_graph(dictionary_sum,
                              std::array{bg::input<TS<i64>>({0, 1, 2, 3}),
                                         bg::input<TSD<i64, TS<i64>>>(
                                             {{{1, 10}}, literal::SKIP, {{2, 20}}, {{3, 30}}}),
                                         bg::input<TSD<i64, TS<i64>>>(ticks({}))})
              .equals({10, 0, literal::SKIP, 60}));
}

template <template <typename, auto...> typename Field> struct changing_and_constant {
    Field<TS<i64>> changing;
    Field<TS<i64, TimeSeriesGuarantee::const_>> constant;
};

TEST_CASE("take preserves item guarantees and forwards only changed bundle items",
          "[feature:operator][feature:type-system]"
          "[series:tsb][guarantee:const][guarantee:final]") {
    const auto observe_constant = node([](In<TSB<changing_and_constant>> values) {
        return values.item_updated(1);
    });
    const auto graph = bg::graph([&](wp<TS<i64>> value) {
        return observe_constant(operators::take(
            bundle<changing_and_constant>(value, operators::const_(i64{1})), 2));
    });

    CHECK(bg::run_graph(graph, {10, 20, 30}).equals({true, false, literal::SKIP}));

    testing::test_wiring([] {
        auto &wiring = WiringContext::current().wiring();
        const auto &constant = wiring.borrowed_input(TS<i64, TimeSeriesGuarantee::const_>::type());
        const auto &constant_list = wiring.borrowed_input(
            TSL<TS<i64, TimeSeriesGuarantee::const_>, 2, TimeSeriesGuarantee::const_>::type());

        CHECK(&operators::take(constant, 1).type() == &TS<i64, TimeSeriesGuarantee::const_>::type());
        CHECK(&operators::take(constant, 0).type() == &TS<i64, TimeSeriesGuarantee::final>::type());

        const auto &dropped = type_cast<TimeSeriesListType>(operators::drop(constant_list, 1).type());
        const auto &stepped = type_cast<TimeSeriesListType>(operators::step(constant_list, 2).type());
        CHECK(dropped.guarantee().value() == TimeSeriesGuarantee::final);
        CHECK(dropped.item_type().guarantee().value() == TimeSeriesGuarantee::final);
        CHECK(stepped.guarantee().value() == TimeSeriesGuarantee::final);
        CHECK(stepped.item_type().guarantee().value() == TimeSeriesGuarantee::final);

        const auto &deduplicated_list = wiring.borrowed_input(
            TSL<TS<i64, TimeSeriesGuarantee::deduplicated>, 2, TimeSeriesGuarantee::deduplicated>::type());
        const auto &limited_deduplicated =
            type_cast<TimeSeriesListType>(operators::drop(deduplicated_list, 1).type());
        CHECK(limited_deduplicated.guarantee().value() == TimeSeriesGuarantee::deduplicated);
        CHECK(limited_deduplicated.item_type().guarantee().value() == TimeSeriesGuarantee::deduplicated);

        const auto &strong_child_list = wiring.borrowed_input(
            TSL<TS<i64, TimeSeriesGuarantee::const_>, 2, TimeSeriesGuarantee::ordinary>::type());
        const auto &limited_strong_child =
            type_cast<TimeSeriesListType>(operators::step(strong_child_list, 2).type());
        CHECK(limited_strong_child.guarantee().value() == TimeSeriesGuarantee::ordinary);
        CHECK(limited_strong_child.item_type().guarantee().value() == TimeSeriesGuarantee::ordinary);

        const auto &final_list = wiring.borrowed_input(
            TSL<TS<i64, TimeSeriesGuarantee::final>, 2, TimeSeriesGuarantee::final>::type());
        const auto &limited_final = type_cast<TimeSeriesListType>(operators::step(final_list, 2).type());
        CHECK(limited_final.guarantee().value() == TimeSeriesGuarantee::final);
        CHECK(limited_final.item_type().guarantee().value() == TimeSeriesGuarantee::final);
    });
}

TEST_CASE("take follows ordinary reference target updates",
          "[feature:operator][series:reference]"
          "[binding:reference-to-value][transition:update]") {
    const auto bind_reference = node([](In<REF<TS<i64>>> value, Out<REF<TS<i64>>> output) {
        output.copy(value);
    });
    const auto graph = bg::graph([&](wp<TS<i64>> value) {
        return operators::take(bind_reference(value), 2).as<TS<i64>>();
    });

    CHECK(bg::run_graph(graph, {10, 20, 30, 40})
              .equals({10, 20, literal::SKIP, literal::SKIP}));
}

TEST_CASE("take state is independent for switch branches and map keys",
          "[feature:operator][feature:map][feature:switch][feature:nested-graph]"
          "[feature:lifecycle][series:tsd][shape:nested]") {
    const auto switched = bg::graph([](wp<TS<i64>> selector, wp<TS<i64>> value) {
        const auto take_branch = [](wp<TS<i64>> input) { return operators::take(input, 2).as<TS<i64>>(); };
        const auto pass_branch = [](wp<TS<i64>> input) { return input; };
        return switch_(selector, cases{case_(i64{1}, take_branch), case_(i64{0}, pass_branch)}, value)
            .as<TS<i64>>();
    });
    CHECK(bg::run_graph(switched, {1, 1, 0, 1, 1, 1}, {10, 20, 30, 40, 50, 60})
              .equals({10, 20, 30, 40, 50, literal::SKIP}));

    const auto mapped_sum = bg::graph([](wp<TSD<i64, TS<i64>>> values) {
        const auto selected = map([](wp<TS<i64>> input) { return operators::take(input, 2).as<TS<i64>>(); }, values);
        return operators::sum_(selected).as<TS<i64>>();
    });
    CHECK(bg::run_graph(mapped_sum,
                              {{{1, 10}, {2, 100}}, {{1, 11}}, {{1, 12}, {2, 101}}, {{2, 102}}, {{2, 103}}})
              .equals({110, 111, 112, literal::SKIP, literal::SKIP}));

    CHECK(bg::run_graph(mapped_sum,
                              {{{1, 10}}, {{1, literal::REMOVE}}, {{1, 20}}, {{1, 21}}, {{1, 22}}})
              .equals({10, 0, 20, 21, literal::SKIP}));
}
