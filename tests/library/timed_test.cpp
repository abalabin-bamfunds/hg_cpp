#include <bg/graph.hpp>
#include <bg/library.hpp>
#include <bg/node.hpp>
#include <bg/testing/test_graph.hpp>
#include <bg/testing/test_wiring.hpp>
#include <bg/time_series/tsd/tsd_type.hpp>
#include <bg/time_series/tsb/tsb.hpp>
#include <bg/time_series/tss/tss_type.hpp>
#include <bg/time_series/tsl/tsl_type.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <limits>
#include <stdexcept>

using namespace bg;
using namespace bg::literals;

struct changing_timed_target {
    static constexpr bool uses_timed_scheduling = true;

    Out<TS<i64>> output;

    void start(EvaluationContext *context) const {
        output.set(10);
        context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
    }

    void evaluate(EvaluationContext *) const {
        output.set(20);
    }
};

TEST_CASE("delay starts with an unbound reference input",
          "[feature:operator][feature:timed-scheduling][series:reference][binding:reference-to-value]") {
    const auto source = node([](Out<REF<TS<i64>>>) {});
    const auto delayed = [&] { return operators::delay(source(), timedelta_t{1}); };

    CHECK(testing::test_graph(delayed).equals<i64>({std::nullopt}));
}

TEST_CASE("delay publishes each retained input event after its duration",
          "[feature:operator][feature:timed-scheduling][transition:invalidation][transition:revalidation]") {
    const auto delayed = [](wp<TS<i64>> value) { return operators::delay(value, timedelta_t{2}); };
    const auto delayed_valid = [](wp<TS<i64>> value) {
        return operators::valid(operators::delay(value, timedelta_t{2}));
    };

    CHECK(testing::test_graph(delayed, {1, 2, literal::INVALID, 4})
              .equals<i64>({std::nullopt, std::nullopt, 1, 2, std::nullopt, 4}));
    CHECK(testing::test_graph(delayed_valid, {1, literal::INVALID, literal::SKIP, 4})
              .equals<bool>({false, std::nullopt, true, false, std::nullopt, true}));
}

TEST_CASE("delay preserves separate equal ticks",
          "[feature:operator][feature:timed-scheduling][series:value][transition:update]") {
    const auto delayed_equal = [](wp<TS<i64>> value) { return operators::delay(value, timedelta_t{2}); };

    CHECK(testing::test_graph(delayed_equal, {1, 1}).equals<i64>({std::nullopt, std::nullopt, 1, 1}));
}

TEST_CASE("delay replays dictionary deltas",
          "[feature:operator][feature:timed-scheduling][series:tsd][transition:update][transition:removal]") {
    const auto delayed_sum = [](wp<TSD<i64, TS<i64>>> value) {
        return operators::sum_(operators::delay(value, timedelta_t{2}));
    };

    CHECK(testing::test_graph(delayed_sum, {{{1, 10}}, {{1, 20}}, {{1, literal::REMOVE}}})
              .equals<i64>({std::nullopt, std::nullopt, 10, 20, 0}));
}

TEST_CASE("delay replays set membership changes",
          "[feature:operator][feature:timed-scheduling][series:tss][transition:addition][transition:removal]") {
    const auto delayed_set_sum = [](wp<TSS<i64>> value) {
        return operators::sum_(operators::delay(value, timedelta_t{2}));
    };
    CHECK(testing::test_graph(
              delayed_set_sum,
              {{1, 2}, {3}, {literal::REMOVED(1), 4},
               {literal::REMOVED(2), literal::REMOVED(3), literal::REMOVED(4)}})
              .equals<i64>({std::nullopt, std::nullopt, 3, 6, 9, 0}));
}

TEST_CASE("delay replays fixed-list ticks",
          "[feature:operator][feature:timed-scheduling][series:fixed-tsl][transition:update]") {
    const auto sum_list = node([](In<TSL<TS<i64>, 2>> input) { return *input[0] + *input[1]; });

    const auto delayed_list = [&](wp<TSL<TS<i64>, 2>> value) {
        return sum_list(operators::delay(value, timedelta_t{2}));
    };
    CHECK(testing::test_graph(delayed_list, {{{0, 1}, {1, 10}}, {{0, 2}, {1, 20}}})
              .equals<i64>({std::nullopt, std::nullopt, 11, 22}));
}

template <template <typename, auto...> typename Field> struct TimedPair {
    Field<TS<i64>> left;
    Field<TS<i64>> right;
};

TEST_CASE("delay replays bundle ticks", "[feature:operator][feature:timed-scheduling][series:tsb][transition:update]") {
    const auto observe_bundle = node([](In<TSB<TimedPair>> input) { return *input->left + *input->right; });

    const auto delayed_bundle = [&](wp<TS<i64>> left, wp<TS<i64>> right) {
        const auto value = bundle<TimedPair>("left"_n = left, "right"_n = right);
        return observe_bundle(operators::delay(value, timedelta_t{2}));
    };
    CHECK(testing::test_graph(delayed_bundle, {1, 2}, {10, 20})
              .equals<i64>({std::nullopt, std::nullopt, 11, 22}));
}

TEST_CASE("delay zero preserves the input type and negative delay fails during wiring",
          "[feature:operator][feature:wiring][feature:timed-scheduling][guarantee:const]") {
    testing::test_wiring([] {
        auto &wiring = WiringContext::current().wiring();
        const auto &input = wiring.borrowed_input(TS<i64, TimeSeriesGuarantee::const_>::type());
        const auto unchanged = operators::delay(input, timedelta_t{});
        CHECK(&unchanged.type() == &input.type());
        const auto reference_source = node([](Out<REF<TS<i64>>>) {});
        const auto reference = reference_source();
        CHECK(&operators::delay(reference, timedelta_t{}).type() == &reference.type());
        CHECK_THROWS_AS(operators::delay(input, timedelta_t{-1}), WiringError);
    });
}

TEST_CASE("delay follows reference retargets and target ticks",
          "[feature:operator][feature:timed-scheduling][series:reference][transition:update]"
          "[binding:reference-to-value]") {
    const auto changing_target = node<changing_timed_target>();
    const auto select = node([](In<TS<bool>> use_constant, In<REF<TS<i64>>> changing,
                                In<REF<TS<i64>>> constant, Out<REF<TS<i64>>> output) {
        output.copy(*use_constant ? constant : changing);
    });
    const auto delayed = [&](wp<TS<bool>> use_constant) {
        const auto constant_target = operators::const_(i64{100}).as<TS<i64>>();
        return operators::delay(select(use_constant, changing_target(), constant_target), timedelta_t{1});
    };

    CHECK(testing::test_graph(delayed, std::array{false, false, true, true})
              .equals<i64>({std::nullopt, 10, 20, 100}));
}

TEST_CASE("delay fails when a reference retargets from valid TSS to an invalid target",
          "[feature:operator][feature:timed-scheduling][series:tss][series:reference]"
          "[transition:invalidation][transition:update]") {
    const auto select = node([](In<TS<i64>> cycle, In<REF<TSS<i64>>, passive> populated,
                                In<REF<TSS<i64>>, passive> invalid, Out<REF<TSS<i64>>> output) {
        output.copy(*cycle == 0 ? populated : invalid);
    });
    const auto delayed = [&](wp<TS<i64>> cycle, wp<TSS<i64>> populated, wp<TSS<i64>> invalid) {
        return operators::delay(select(cycle, populated, invalid), timedelta_t{1});
    };

    CHECK_THROWS_AS(testing::test_graph(delayed, {0, 1}, {{1}, literal::SKIP}, {}), std::invalid_argument);
}

TEST_CASE("delay fails when a reference retargets from valid TSD to an invalid target",
          "[feature:operator][feature:timed-scheduling][series:tsd][series:reference]"
          "[transition:invalidation][transition:update]") {
    const auto select = node([](In<TS<i64>> cycle, In<REF<TSD<i64, TS<i64>>>, passive> populated,
                                In<REF<TSD<i64, TS<i64>>>, passive> invalid,
                                Out<REF<TSD<i64, TS<i64>>>> output) {
        output.copy(*cycle == 0 ? populated : invalid);
    });
    const auto delayed = [&](wp<TS<i64>> cycle, wp<TSD<i64, TS<i64>>> populated,
                             wp<TSD<i64, TS<i64>>> invalid) {
        return operators::delay(select(cycle, populated, invalid), timedelta_t{1});
    };

    CHECK_THROWS_AS(testing::test_graph(delayed, {0, 1}, {{{1, 10}}, literal::SKIP}, {}), std::invalid_argument);
}

TEST_CASE("delay replays TSS membership changes when a reference selects another valid set",
          "[feature:operator][feature:timed-scheduling][series:tss][series:reference]"
          "[transition:addition][transition:removal][transition:update]") {
    const auto select = node([](In<TS<bool>> use_second, In<REF<TSS<i64>>> first, In<REF<TSS<i64>>> second,
                                Out<REF<TSS<i64>>> output) { output.copy(*use_second ? second : first); });
    const auto delayed_sum = [&](wp<TS<bool>> use_second, wp<TSS<i64>> first, wp<TSS<i64>> second) {
        return operators::sum_(operators::delay(select(use_second, first, second), timedelta_t{1}));
    };

    CHECK(testing::test_graph(delayed_sum, {false, true}, {{1, 2}, literal::SKIP}, {{3, 4}, literal::SKIP})
              .equals<i64>({std::nullopt, 3, 7}));
}

TEST_CASE("delay replays TSD key changes when a reference selects another valid dictionary",
          "[feature:operator][feature:timed-scheduling][series:tsd][series:reference]"
          "[transition:addition][transition:removal][transition:update]") {
    const auto select = node([](In<TS<bool>> use_second, In<REF<TSD<i64, TS<i64>>>> first,
                                In<REF<TSD<i64, TS<i64>>>> second, Out<REF<TSD<i64, TS<i64>>>> output) {
        output.copy(*use_second ? second : first);
    });
    const auto delayed_sum = [&](wp<TS<bool>> use_second, wp<TSD<i64, TS<i64>>> first,
                                 wp<TSD<i64, TS<i64>>> second) {
        return operators::sum_(operators::delay(select(use_second, first, second), timedelta_t{1}));
    };

    CHECK(testing::test_graph(delayed_sum, {false, true}, {{{1, 10}}, literal::SKIP},
                              {{{2, 20}}, literal::SKIP})
              .equals<i64>({std::nullopt, 10, 20}));
}

TEST_CASE("throttle follows reference retargets and target ticks",
          "[feature:operator][feature:timed-scheduling][series:reference][transition:update]"
          "[binding:reference-to-value]") {
    const auto changing_target = node<changing_timed_target>();
    const auto select = node([](In<TS<bool>> use_constant, In<REF<TS<i64>>> changing,
                                In<REF<TS<i64>>> constant, Out<REF<TS<i64>>> output) {
        output.copy(*use_constant ? constant : changing);
    });
    const auto throttled = [&](wp<TS<bool>> use_constant) {
        const auto constant_target = operators::const_(i64{100}).as<TS<i64>>();
        const auto selected = select(use_constant, changing_target(), constant_target);
        return operators::throttle(selected, timedelta_t{1});
    };

    CHECK(testing::test_graph(throttled, std::array{false, false, true, true})
              .equals<i64>({10, 20, 100, std::nullopt}));
}

TEST_CASE("delay limits const guarantees and rejects contained references",
          "[feature:operator][feature:wiring][feature:timed-scheduling][guarantee:final][series:tsd]"
          "[series:reference]") {
    testing::test_wiring([] {
        auto &wiring = WiringContext::current().wiring();
        const auto &constant = wiring.borrowed_input(TS<i64, TimeSeriesGuarantee::const_>::type());
        CHECK(&operators::delay(constant, timedelta_t{1}).type() ==
              &TS<i64, TimeSeriesGuarantee::final>::type());

        const auto &constant_dictionary = wiring.borrowed_input(
            TSD<i64, TS<i64, TimeSeriesGuarantee::const_>, TimeSeriesGuarantee::const_>::type());
        const auto &final_dictionary = type_cast<TimeSeriesDictionaryType>(
            operators::delay(constant_dictionary, timedelta_t{1}).type());
        CHECK(final_dictionary.guarantee().value() == TimeSeriesGuarantee::final);
        CHECK(final_dictionary.value_type().guarantee().value() == TimeSeriesGuarantee::final);

        const auto &reference_collection = wiring.borrowed_input(TSD<i64, REF<TS<i64>>>::type());
        CHECK_THROWS_AS(operators::delay(reference_collection, timedelta_t{1}), WiringError);

        const auto &sparse_type = time_series_list_type(TS<i64>::type(), std::dynamic_extent,
                                                        TimeSeriesGuarantee::ordinary);
        const auto &sparse_list = wiring.borrowed_input(sparse_type);
        CHECK_THROWS_AS(operators::delay(sparse_list, timedelta_t{1}), WiringError);

        const auto &nested_sparse_type = time_series_dictionary_type(int64_type(), sparse_type,
                                                                      TimeSeriesGuarantee::ordinary);
        const auto &nested_sparse_list = wiring.borrowed_input(nested_sparse_type);
        CHECK_THROWS_AS(operators::delay(nested_sparse_list, timedelta_t{1}), WiringError);
    });
}

TEST_CASE("delay reports evaluation-time overflow",
          "[feature:operator][feature:timed-scheduling][scalar:evaluation-time]") {
    const auto delayed = [](wp<TS<i64>> value) { return operators::delay(value, timedelta_t::max()); };

    CHECK_THROWS_AS(testing::test_graph(delayed, {1}), std::overflow_error);
}

TEST_CASE("schedule supports delayed and immediate starts and finite tick limits",
          "[feature:operator][feature:timed-scheduling][transition:start]") {
    const auto delayed = [] { return operators::schedule(timedelta_t{1}, true, i64{2}); };
    const auto immediate = [] { return operators::schedule(timedelta_t{1}, false, i64{3}); };
    const auto omitted_initial_delay = [] { return operators::schedule(timedelta_t{1}, "max_ticks"_n = i64{2}); };
    const auto empty = [] { return operators::schedule(timedelta_t{1}, true, i64{0}); };

    CHECK(testing::test_graph(delayed).equals<bool>({std::nullopt, true, true}));
    CHECK(testing::test_graph(immediate).equals<bool>({true, true, true}));
    CHECK(testing::test_graph(omitted_initial_delay).equals<bool>({std::nullopt, true, true}));
    CHECK(testing::test_graph(empty).equals<bool>({std::nullopt}));
}

TEST_CASE("schedule validates its period, limit, and due-time range",
          "[feature:operator][feature:wiring][feature:timed-scheduling][scalar:evaluation-time]") {
    CHECK_THROWS_AS(testing::test_graph([] { return operators::schedule(timedelta_t{}, true, 1); }), WiringError);
    CHECK_THROWS_AS(testing::test_graph([] { return operators::schedule(timedelta_t{1}, true, i64{-1}); }),
                    WiringError);
    CHECK_THROWS_AS(testing::test_graph([] { return operators::schedule(timedelta_t::max(), true, i64{1}); }),
                    std::overflow_error);
}

TEST_CASE("throttle publishes the first event and coalesces later events",
          "[feature:operator][feature:timed-scheduling][transition:update]") {
    const auto immediate = [](wp<TS<i64>> value) { return operators::throttle(value, timedelta_t{2}); };
    const auto delayed_first = [](wp<TS<i64>> value) {
        return operators::throttle(value, timedelta_t{2}, true);
    };

    CHECK(testing::test_graph(immediate, {1, 2, 3, 4}).equals<i64>({1, std::nullopt, 3, std::nullopt, 4}));
    CHECK(testing::test_graph(delayed_first, {1, 2, 3}).equals<i64>({std::nullopt, std::nullopt, 3}));
}

TEST_CASE("throttle applies timing to invalidation and publishes current-cycle changes",
          "[feature:operator][feature:timed-scheduling][transition:invalidation][transition:revalidation]") {
    const auto valid_throttled = [](wp<TS<i64>> value) {
        return operators::valid(operators::throttle(value, timedelta_t{2}, false));
    };
    const auto throttled = [](wp<TS<i64>> value) { return operators::throttle(value, timedelta_t{2}, false); };

    CHECK(testing::test_graph(valid_throttled, {1, literal::INVALID, literal::SKIP, literal::SKIP})
              .equals<bool>({true, std::nullopt, false, std::nullopt}));
    CHECK(testing::test_graph(throttled, {1, literal::INVALID, 3}).equals<i64>({1, std::nullopt, 3}));

    const auto delayed_first_valid = [](wp<TS<i64>> value) {
        return operators::valid(operators::throttle(value, timedelta_t{2}, true));
    };
    CHECK(testing::test_graph(delayed_first_valid,
                              {1, literal::SKIP, literal::SKIP, literal::INVALID, literal::SKIP})
              .equals<bool>({false, std::nullopt, true, std::nullopt, false}));
}

TEST_CASE("throttle reconciles collection state at publication time",
          "[feature:operator][feature:timed-scheduling][series:tsd][transition:addition][transition:removal]") {
    const auto throttled_sum = [](wp<TSD<i64, TS<i64>>> value) {
        return operators::sum_(operators::throttle(value, timedelta_t{2}, false));
    };

    CHECK(testing::test_graph(throttled_sum, {{{1, 10}}, {{1, literal::REMOVE}}})
              .equals<i64>({10, std::nullopt, 0}));
}

TEST_CASE("throttle validates its period, guarantee, and due-time range",
          "[feature:operator][feature:wiring][feature:timed-scheduling][guarantee:final]"
          "[scalar:evaluation-time]") {
    testing::test_wiring([] {
        auto &wiring = WiringContext::current().wiring();
        const auto &constant = wiring.borrowed_input(TS<i64, TimeSeriesGuarantee::const_>::type());
        CHECK(&operators::throttle(constant, timedelta_t{1}, false).type() ==
              &TS<i64, TimeSeriesGuarantee::final>::type());
        CHECK_THROWS_AS(operators::throttle(constant, timedelta_t{}, false), WiringError);
    });

    const auto overflowing = [](wp<TS<i64>> value) {
        return operators::throttle(value, timedelta_t::max(), false);
    };
    CHECK_THROWS_AS(testing::test_graph(overflowing, {1}), std::overflow_error);
}

TEST_CASE("a nested timed source stops with its selected graph",
          "[feature:operator][feature:timed-scheduling][feature:nested-graph][feature:switch]"
          "[transition:stop]") {
    const auto selected_schedule = [](wp<TS<i64>> key) {
        return switch_(key,
                       cases{case_(i64{1}, [] {
                                   return operators::schedule(timedelta_t{2}, true, i64{5}).as<TS<bool>>();
                               }),
                              case_(DEFAULT, [] { return operators::const_(false).as<TS<bool>>(); })})
            .as<TS<bool>>();
    };

    CHECK(testing::test_graph(selected_schedule, {1, 0, 0})
              .equals<bool>({std::nullopt, false, std::nullopt}));
}
