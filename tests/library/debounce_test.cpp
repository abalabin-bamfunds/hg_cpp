#include <bg/graph.hpp>
#include <bg/library.hpp>
#include <bg/node.hpp>
#include <bg/run_graph.hpp>
#include <bg/testing/test_wiring.hpp>
#include <bg/time_series/tsb/tsb.hpp>
#include <bg/time_series/tsd/tsd_type.hpp>
#include <bg/time_series/tss/tss_type.hpp>
#include <bg/time_series/tsl/tsl.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>

using namespace bg;
using namespace bg::literals;

template <template <typename, auto...> typename Field> struct debounce_pair {
    Field<TS<i64>> left;
    Field<TS<i64>> right;
};

template <template <typename, auto...> typename Field> struct debounce_reference_pair {
    Field<REF<TS<i64>>> left;
    Field<REF<TS<i64>>> right;
};

TEST_CASE("debounce publishes the latest value after input silence",
          "[feature:operator][feature:timed-scheduling][transition:update]") {
    const auto debounce = [](wp<TS<i64>> value) { return operators::debounce(value, timedelta_t{2}); };

    CHECK(bg::run_graph(debounce, {1, literal::SKIP, literal::SKIP, 2, 3})
              .equals<i64>({std::nullopt, std::nullopt, 1, std::nullopt, std::nullopt, std::nullopt, 3}));

    const auto isolated = [](wp<TS<i64>> value) { return operators::debounce(value, timedelta_t{1}); };
    CHECK(bg::run_graph(isolated, {7}).equals<i64>({std::nullopt, 7}));
}

TEST_CASE("debounce restarts for equal ticks, invalidation, and revalidation",
          "[feature:operator][feature:timed-scheduling][transition:update]"
          "[transition:invalidation][transition:revalidation]") {
    const auto debounce = [](wp<TS<i64>> value) { return operators::debounce(value, timedelta_t{2}); };
    const auto valid = [](wp<TS<i64>> value) { return operators::valid(operators::debounce(value, timedelta_t{2})); };

    CHECK(bg::run_graph(debounce, {1, 1}).equals<i64>({std::nullopt, std::nullopt, std::nullopt, 1}));
    CHECK(bg::run_graph(valid, {1, literal::SKIP, literal::SKIP, literal::INVALID,
                                literal::SKIP, literal::SKIP, 2})
              .equals<bool>({false, std::nullopt, true, std::nullopt, std::nullopt, false,
                             std::nullopt, std::nullopt, true}));
    CHECK(bg::run_graph(valid, {1, literal::INVALID, 2})
              .equals<bool>({false, std::nullopt, std::nullopt, std::nullopt, true}));
}

TEST_CASE("debounce reconciles collection removals from the complete current value",
          "[feature:operator][feature:timed-scheduling][series:tsd][series:tss]"
          "[transition:removal]") {
    const auto debounce_dictionary = [](wp<TSD<i64, TS<i64>>> value) {
        return operators::sum_(operators::debounce(value, timedelta_t{2}));
    };
    const auto debounce_set = [](wp<TSS<i64>> value) {
        return operators::sum_(operators::debounce(value, timedelta_t{2}));
    };

    CHECK(bg::run_graph(debounce_dictionary, {{{1, 10}}, {{1, literal::REMOVE}}})
              .equals<i64>({std::nullopt, std::nullopt, std::nullopt, 0}));
    CHECK(bg::run_graph(debounce_set, {{1, 2}, {literal::REMOVED(1), literal::REMOVED(2)}})
              .equals<i64>({std::nullopt, std::nullopt, std::nullopt, 0}));

    CHECK(bg::run_graph(debounce_dictionary,
                        {{{1, 10}}, literal::SKIP, literal::SKIP, {{1, literal::REMOVE}}})
              .equals<i64>({std::nullopt, std::nullopt, 10, std::nullopt, std::nullopt, 0}));
    CHECK(bg::run_graph(debounce_set, {{1, 2}, literal::SKIP, literal::SKIP,
                                       {literal::REMOVED(1), literal::REMOVED(2)}})
              .equals<i64>({std::nullopt, std::nullopt, 3, std::nullopt, std::nullopt, 0}));
}

TEST_CASE("debounce copies fixed lists, bundles, and contained reference values",
          "[feature:operator][feature:timed-scheduling][series:fixed-tsl][series:tsb]"
          "[series:reference][binding:reference-to-value]") {
    const auto sum_list = node([](In<TSL<TS<i64>, 2>, all_valid> input) { return *input[0] + *input[1]; });
    const auto debounced_list = [&](wp<TSL<TS<i64>, 2>> value) {
        return sum_list(operators::debounce(value, timedelta_t{2}));
    };
    CHECK(bg::run_graph(debounced_list, {{{0, 1}, {1, 10}}, {{0, 2}, {1, 20}}})
              .equals<i64>({std::nullopt, std::nullopt, std::nullopt, 22}));

    const auto sum_bundle = node([](In<TSB<debounce_pair>, all_valid> input) { return *input->left + *input->right; });
    const auto debounced_bundle = [&](wp<TS<i64>> left, wp<TS<i64>> right) {
        return sum_bundle(operators::debounce(bundle<debounce_pair>("left"_n = left, "right"_n = right),
                                              timedelta_t{2}));
    };
    CHECK(bg::run_graph(debounced_bundle, {1, 2}, {10, 20})
              .equals<i64>({std::nullopt, std::nullopt, std::nullopt, 22}));

    const auto observe_values = node([](In<TSB<debounce_pair>, all_valid> input) {
        return *input->left + *input->right;
    });
    const auto debounced_references = [&](wp<TS<i64>> left, wp<TS<i64>> right) {
        const auto references = bundle<debounce_reference_pair>("left"_n = left.as<REF<TS<i64>>>(),
                                                                "right"_n = right.as<REF<TS<i64>>>());
        const auto value = operators::debounce(references, timedelta_t{2});
        CHECK_FALSE(value.type().contains_reference());
        return observe_values(value);
    };
    CHECK(bg::run_graph(debounced_references, {1, 2}, {10, 20})
              .equals<i64>({std::nullopt, std::nullopt, std::nullopt, 22}));
}

namespace {

struct debounce_coincident_source {
    static constexpr bool uses_timed_scheduling = true;

    Out<TS<i64>> output;

    void start(EvaluationContext *context) const {
        output.set(10);
        context->schedule_at(context->evaluation_time() + std::chrono::microseconds{2});
    }

    void evaluate(EvaluationContext *) const { output.set(20); }
};

} // namespace

TEST_CASE("debounce restarts rather than publishing when an event coincides with expiry",
          "[feature:operator][feature:timed-scheduling][transition:update]") {
    const auto source = node<debounce_coincident_source>();
    const auto graph = [&] { return operators::debounce(source(), timedelta_t{2}); };

    const auto result = bg::run_graph(graph);
    CHECK(result.equals<i64>({std::nullopt, std::nullopt, std::nullopt, std::nullopt, 20}));
}

TEST_CASE("debounce validates its period and caps the output guarantee",
          "[feature:operator][feature:wiring][feature:timed-scheduling][guarantee:final]") {
    testing::test_wiring([] {
        const auto constant = operators::const_(i64{1});
        CHECK(&operators::debounce(constant, timedelta_t{1}).type() ==
              &TS<i64, TimeSeriesGuarantee::final>::type());
        CHECK_THROWS_AS(operators::debounce(constant, timedelta_t{}), WiringError);
        CHECK_THROWS_AS(operators::debounce(constant, timedelta_t{-1}), WiringError);

        const auto constant_bundle = bundle<debounce_pair>("left"_n = operators::const_(i64{1}),
                                                           "right"_n = operators::const_(i64{2}));
        const auto debounced_bundle = operators::debounce(constant_bundle, timedelta_t{1});
        const auto &output_type = type_cast<TimeSeriesBundleType>(debounced_bundle.type());
        CHECK(output_type.guarantee().value() == TimeSeriesGuarantee::ordinary);
        CHECK(output_type.item_type(0).guarantee().value() == TimeSeriesGuarantee::final);
        CHECK(output_type.item_type(1).guarantee().value() == TimeSeriesGuarantee::final);
    });
}

TEST_CASE("debounce does not publish an initial invalid input or outlive a stopped branch",
          "[feature:operator][feature:timed-scheduling][feature:nested-graph]"
          "[transition:stop][validity:optional]") {
    const auto invalid = [](wp<TS<i64>> value) {
        return operators::debounce(value, timedelta_t{2});
    };
    CHECK(bg::run_graph(invalid, {literal::INVALID}).equals<i64>({std::nullopt}));

    const auto debounce = [](wp<TS<i64>> value) { return operators::debounce(value, timedelta_t{2}); };
    const auto passthrough = [](wp<TS<i64>> value) { return value; };
    const auto selected = [&](wp<TS<i64>> selector, wp<TS<i64>> value) {
        return switch_(selector, cases{case_(i64{1}, debounce), case_(i64{0}, passthrough)}, value);
    };
    CHECK(bg::run_graph(selected, {1, 0}, {10, 20}).equals<i64>({std::nullopt, 20}));

    CHECK(bg::run_graph(selected, {1, 0, 1}, {10, 20, 30})
              .equals<i64>({std::nullopt, 20, std::nullopt, std::nullopt, 30}));
}

