#include <bg/library.hpp>
#include <bg/nested.hpp>
#include <bg/run_graph.hpp>
#include <bg/testing/test_wiring.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <optional>

using namespace bg;
using namespace bg::literals;

TEST_CASE("gate retains closed input ticks and drains them in order",
          "[feature:operator][feature:timed-scheduling][series:value][transition:update]") {
    const auto gate = [](wp<TS<bool>> condition, wp<TS<i64>> value) {
        return operators::gate(condition, value);
    };

    CHECK(bg::run_graph(gate, {false, true}, {1, 2}).equals<i64>({std::nullopt, 1, 2}));
}

TEST_CASE("gate handles a full buffer when opening coincides with an input tick",
          "[feature:operator][feature:timed-scheduling][series:value][transition:update]") {
    const auto gate = [](wp<TS<bool>> condition, wp<TS<i64>> value) {
        return operators::gate(condition, value, 1);
    };

    CHECK(bg::run_graph(gate, {false, true}, {1, 2}).equals<i64>({std::nullopt, 1, 2}));
}

TEST_CASE("gate pauses and resumes its queue when the condition closes or becomes invalid",
          "[feature:operator][feature:timed-scheduling][series:value][transition:update]"
          "[transition:invalidation]") {
    const auto gate = [](wp<TS<bool>> condition, wp<TS<i64>> value) {
        return operators::gate(condition, value);
    };

    CHECK(bg::run_graph(gate, {false, true, literal::INVALID, true}, {1, 2, 3, 4})
              .equals<i64>({std::nullopt, 1, std::nullopt, 2, 3, 4}));
}

TEST_CASE("gate retains value invalidation as an ordered event",
          "[feature:operator][feature:timed-scheduling][series:value][validity:optional]"
          "[transition:invalidation][transition:revalidation]") {
    const auto gate = [](wp<TS<bool>> condition, wp<TS<i64>> value) {
        return operators::gate(condition, value);
    };

    CHECK(bg::run_graph(gate, {false, false, true}, {1, literal::INVALID, 2})
              .equals({literal::SKIP, literal::SKIP, 1, literal::INVALID, 2}));
}

TEST_CASE("gate queues values while its initial condition is invalid",
          "[feature:operator][feature:timed-scheduling][series:value][validity:optional]"
          "[transition:first-tick][transition:invalidation]") {
    const auto gate = [](wp<TS<bool>> condition, wp<TS<i64>> value) {
        return operators::gate(condition, value);
    };

    CHECK(bg::run_graph(gate, {literal::INVALID, true}, {1, 2}).equals<i64>({std::nullopt, 1, 2}));
}

TEST_CASE("gate reset discards closed events before retaining the current tick",
          "[feature:operator][feature:timed-scheduling][series:value][transition:update]") {
    const auto gate = [](wp<TS<bool>> condition, wp<TS<i64>> value, wp<TS<bool>> reset) {
        return operators::gate(condition, value, "reset"_n = reset);
    };

    CHECK(bg::run_graph(gate, input<TS<bool>>({false, false, false, true}), input<TS<i64>>({1, 2, 3, 4}),
                        input<TS<bool>>({literal::SKIP, literal::SKIP, true, literal::SKIP}))
              .equals<i64>({std::nullopt, std::nullopt, std::nullopt, 3, 4}));
}

TEST_CASE("gate reset does not publish or invalidate the previous output",
          "[feature:operator][feature:timed-scheduling][series:value][transition:update]") {
    const auto gate = [](wp<TS<bool>> condition, wp<TS<i64>> value, wp<TS<i64>> reset) {
        return operators::gate(condition, value, "reset"_n = reset);
    };

    CHECK(bg::run_graph(gate, input<TS<bool>>({true, true, true}),
                        input<TS<i64>>({1, literal::SKIP, literal::SKIP}),
                        input<TS<i64>>({literal::SKIP, 1, literal::SKIP}))
              .equals<i64>({1, std::nullopt, std::nullopt}));

    const auto remains_valid = [](wp<TS<bool>> condition, wp<TS<i64>> value, wp<TS<i64>> reset) {
        return operators::sample(reset, operators::valid(operators::gate(condition, value, "reset"_n = reset)));
    };
    CHECK(bg::run_graph(remains_valid, input<TS<bool>>({true, true, true}),
                        input<TS<i64>>({1, literal::SKIP, literal::SKIP}),
                        input<TS<i64>>({literal::SKIP, 1, literal::SKIP}))
              .equals<bool>({std::nullopt, true, std::nullopt}));
}

TEST_CASE("gate reset during drain discards queued events before the current tick",
          "[feature:operator][feature:timed-scheduling][series:value][transition:update]") {
    const auto gate = [](wp<TS<bool>> condition, wp<TS<i64>> value, wp<TS<i64>> reset) {
        return operators::gate(condition, value, "reset"_n = reset);
    };

    CHECK(bg::run_graph(gate, input<TS<bool>>({false, true, true}), input<TS<i64>>({1, 2, 3}),
                        input<TS<i64>>({literal::SKIP, literal::SKIP, 1}))
              .equals<i64>({std::nullopt, 1, 3}));
}

TEST_CASE("gate retains set and dictionary deltas", "[feature:operator][feature:timed-scheduling]"
          "[series:tss][series:tsd][scalar:i64][transition:update]") {
    const auto gate_set = [](wp<TS<bool>> condition, wp<TSS<i64>> value) {
        return operators::gate(condition, value);
    };
    const auto gate_dictionary = [](wp<TS<bool>> condition, wp<TSD<i64, TS<i64>>> value) {
        return operators::gate(condition, value);
    };

    CHECK(bg::run_graph(gate_set, {false, true}, {{1}, {2}}).equals({literal::SKIP, {1}, {2}}));
    CHECK(bg::run_graph(gate_dictionary, {false, true}, {{{1, 10}}, {{2, 20}}})
              .equals({literal::SKIP, {{1, 10}}, {{2, 20}}}));
}

TEST_CASE("gate retains fixed-list deltas", "[feature:operator][feature:timed-scheduling][series:fixed-tsl]"
          "[transition:update]") {
    const auto gate = [](wp<TS<bool>> condition, wp<TSL<TS<i64>, 2>> value) {
        return operators::gate(condition, value);
    };

    CHECK(bg::run_graph(gate, input<TS<bool>>({false, true}),
                        input<TSL<TS<i64>, 2>>({{{0, 10}, {1, 20}}, {{0, 30}}}))
              .equals({literal::SKIP, {{0, 10}, {1, 20}}, {{0, 30}}}));
}

TEST_CASE("gate retains collection invalidation and revalidation",
          "[feature:operator][feature:timed-scheduling][series:fixed-tsl][validity:optional]"
          "[transition:invalidation][transition:revalidation]") {
    const auto gate = [](wp<TS<bool>> condition, wp<TSL<TS<i64>, 1>> value) {
        return operators::gate(condition, value);
    };

    CHECK(bg::run_graph(gate, input<TS<bool>>({false, false, false, true}),
                        input<TSL<TS<i64>, 1>>({{{0, 10}}, literal::INVALID, {{0, 20}}, literal::SKIP}))
              .equals({literal::SKIP, literal::SKIP, literal::SKIP, {{0, 10}}, literal::INVALID, {{0, 20}}}));
}

TEST_CASE("gate retains bundle events from dictionary values",
          "[feature:operator][feature:timed-scheduling][series:tsb][scalar:string][scalar:i64]"
          "[transition:update]") {
    const auto read_value = node([](In<TSB<operators::key_value>> entry,
                                    Out<TS<operators::KEY_VALUE_VALUE>> output) { output.copy(entry->value); });
    const auto gate = [&](wp<TS<bool>> condition, wp<TS<Dict<string, i64>>> value) {
        return read_value(operators::gate(condition, operators::emit(value)));
    };

    CHECK(bg::run_graph(gate, {false, true}, {{{"first", 10}}, {{"second", 20}}})
              .equals<i64>({std::nullopt, 10, 20}));
}

TEST_CASE("gate dereferences contained references before retaining fixed-list ticks",
          "[feature:operator][feature:timed-scheduling][feature:reference][series:fixed-tsl][series:reference]"
          "[binding:reference-to-value][transition:update]") {
    const auto make_reference_list = node([](In<REF<TS<i64>>> value, Out<TSL<REF<TS<i64>>, 1>> output) {
        output[0].copy(value);
    });
    const auto observe = node([](In<TSL<TS<i64>, 1>> values) { return *values[0]; });
    const auto gate = [&](wp<TS<bool>> condition, wp<TS<i64>> value) {
        const auto values = make_reference_list(value.as<REF<TS<i64>>>());
        CHECK(values.type().contains_reference());

        const auto gated_values = operators::gate(condition, values);
        CHECK_FALSE(gated_values.type().contains_reference());
        return observe(gated_values);
    };

    CHECK(bg::run_graph(gate, {false, false, true}, {10, 20, 30})
              .equals<i64>({std::nullopt, std::nullopt, 10, 20, 30}));
}

TEST_CASE("gate releases unread owning events when a nested graph stops",
          "[feature:operator][feature:timed-scheduling][feature:nested-graph][transition:stop]"
          "[series:value][scalar:string]") {
    const auto inner = graph(
        "name"_n = "gate_inner",
        [](wp<TS<bool>> condition, wp<TS<string>> value) -> wp<TS<string>> {
            return operators::gate(condition, value).as<TS<string>>();
        });
    const auto gate = [&](wp<TS<bool>> run, wp<TS<bool>> condition, wp<TS<string>> value) {
        return nested(inner, run, condition, value);
    };

    CHECK(bg::run_graph(gate, {true, true, false, true}, {false, false, true, true},
                        {"old", "queued", "discarded", "new"})
              .equals<string>({std::nullopt, std::nullopt, std::nullopt, "new"}));
}

TEST_CASE("gate rejects a nonpositive buffer length during wiring", "[feature:operator][feature:wiring]") {
    const auto gate = [](wp<TS<bool>> condition, wp<TS<i64>> value) {
        return operators::gate(condition, value, 0);
    };

    CHECK_THROWS_AS(bg::run_graph(gate, {true}, {1}), WiringError);
}

TEST_CASE("batch flushes values retained while closed when the condition opens",
          "[feature:operator][feature:timed-scheduling][series:value][scalar:list][transition:update]") {
    const auto batch = [](wp<TS<bool>> condition, wp<TS<i64>> value) {
        return operators::batch(condition, value, std::chrono::microseconds{2});
    };

    CHECK(bg::run_graph(batch, {false, false, true}, {1, 2, 3}).equals({literal::SKIP, literal::SKIP, {1, 2, 3}}));
}

TEST_CASE("batch does not append an invalidated value",
          "[feature:operator][feature:timed-scheduling][series:value][scalar:list]"
          "[transition:invalidation][transition:revalidation]") {
    const auto batch = [](wp<TS<bool>> condition, wp<TS<i64>> value) {
        return operators::batch(condition, value, std::chrono::microseconds{2});
    };

    CHECK(bg::run_graph(batch, {false, false, false, true}, {1, literal::INVALID, 2, literal::SKIP})
              .equals({literal::SKIP, literal::SKIP, literal::SKIP, {1, 2}}));
}

TEST_CASE("batch groups repeated true ticks when its timer expires",
          "[feature:operator][feature:timed-scheduling][series:value][scalar:list]"
          "[transition:update]") {
    const auto batch = [](wp<TS<bool>> condition, wp<TS<i64>> value) {
        return operators::batch(condition, value, std::chrono::microseconds{2});
    };

    CHECK(bg::run_graph(batch, {true, true, true, true}, {1, 2, 3, 4})
              .equals({{1}, literal::SKIP, literal::SKIP, {2, 3, 4}}));
}

TEST_CASE("batch cancels a timer while closed and retains owning strings",
          "[feature:operator][feature:timed-scheduling][series:value][scalar:list][scalar:string]"
          "[transition:update]") {
    const auto batch = [](wp<TS<bool>> condition, wp<TS<string>> value) {
        return operators::batch(condition, value, std::chrono::microseconds{2});
    };

    CHECK(bg::run_graph(batch, {true, true, false, true}, {"first", "second", "third", "fourth"})
              .equals({{"first"}, literal::SKIP, literal::SKIP, {"second", "third", "fourth"}}));
}

TEST_CASE("gate rejects queue overflow", "[feature:operator][feature:timed-scheduling][series:value][scalar:i64]") {
    const auto gate = [](wp<TS<bool>> condition, wp<TS<i64>> value) {
        return operators::gate(condition, value, 1);
    };

    CHECK_THROWS_AS(bg::run_graph(gate, {false, false}, {1, 2}), std::runtime_error);
}

TEST_CASE("batch rejects queue overflow",
          "[feature:operator][feature:timed-scheduling][series:value][scalar:i64][scalar:list]") {
    const auto batch = [](wp<TS<bool>> condition, wp<TS<i64>> value) {
        return operators::batch(condition, value, std::chrono::microseconds{1}, 1);
    };

    CHECK_THROWS_AS(bg::run_graph(batch, {false, false}, {1, 2}), std::runtime_error);
}

TEST_CASE("batch rejects a nonpositive delay and buffer length during wiring", "[feature:operator][feature:wiring]") {
    const auto zero_delay = [](wp<TS<bool>> condition, wp<TS<i64>> value) {
        return operators::batch(condition, value, std::chrono::microseconds{0});
    };
    const auto zero_capacity = [](wp<TS<bool>> condition, wp<TS<i64>> value) {
        return operators::batch(condition, value, std::chrono::microseconds{1}, 0);
    };

    CHECK_THROWS_AS(bg::run_graph(zero_delay, {true}, {1}), WiringError);
    CHECK_THROWS_AS(bg::run_graph(zero_capacity, {true}, {1}), WiringError);
}

TEST_CASE("gate limits const output guarantees to final and preserves ordinary guarantees",
          "[feature:operator][feature:wiring][guarantee:const][guarantee:ordinary][guarantee:final]") {
    testing::test_wiring([] {
        const auto condition = operators::const_(true);
        const auto constant = operators::const_(i64{1});
        const auto ordinary = operators::merge(operators::const_(i64{1}), operators::const_(i64{2}));

        CHECK(&operators::gate(condition, constant).type() == &TS<i64, TimeSeriesGuarantee::final>::type());
        CHECK(&operators::gate(condition, ordinary).type() == &TS<i64>::type());
    });
}

TEST_CASE("batch produces ordinary output", "[feature:operator][feature:wiring][guarantee:ordinary]") {
    testing::test_wiring([] {
        const auto condition = operators::const_(true);
        const auto value = operators::merge(operators::const_(i64{1}), operators::const_(i64{2}));

        CHECK(&operators::batch(condition, value, std::chrono::microseconds{1}).type() == &TS<List<i64>>::type());
    });
}
