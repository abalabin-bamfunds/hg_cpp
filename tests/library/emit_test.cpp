#include <bg/graph.hpp>
#include <bg/library.hpp>
#include <bg/nested.hpp>
#include <bg/node.hpp>
#include <bg/testing/test_graph.hpp>
#include <bg/testing/test_wiring.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <optional>
#include <span>
#include <string>

using namespace bg;
using namespace bg::literals;

TEST_CASE("emit scalar list items in FIFO order across input updates and invalidation",
          "[feature:operator][feature:timed-scheduling][series:value][scalar:list]"
          "[transition:first-tick][transition:update][transition:invalidation]") {
    const auto emit = [](wp<TS<List<i64>>> value) { return operators::emit(value); };

    CHECK(testing::test_graph(emit, {{1, 1, 2}, {3}, {}, literal::INVALID, {4}})
              .equals<i64>({1, 1, 2, 3, 4}));
}

TEST_CASE("emit empty scalar collections without publishing an empty event",
          "[feature:operator][series:value][scalar:list][transition:first-tick]") {
    const auto emit = [](wp<TS<List<i64>>> value) { return operators::emit(value); };

    CHECK(testing::test_graph(emit, {{}, {7}}).equals<i64>({std::nullopt, 7}));
}

TEST_CASE("emit scalar set members as ordinary output ticks",
          "[feature:operator][feature:timed-scheduling][series:value][scalar:set][guarantee:ordinary]") {
    const auto emit = [](wp<TS<Set<i64>>> value) { return operators::emit(value); };

    CHECK(testing::test_graph(emit, {{1}, {2}}).equals<i64>({1, 2}));
}

TEST_CASE("emit scalar dictionary entries as key and value bundle fields",
          "[feature:operator][feature:timed-scheduling][series:value][scalar:dict][series:tsb]") {
    const auto read_value = node([](In<TSB<operators::key_value>> entry,
                                    Out<TS<operators::KEY_VALUE_VALUE>> output) {
        output.copy(entry->value);
    });
    const auto emit = [&](wp<TS<Dict<string, i64>>> value) {
        const auto entries = operators::emit(value);
        return read_value(entries);
    };

    CHECK(testing::test_graph(emit, {{{"first", 10}}, {{"second", 20}}}).equals<i64>({10, 20}));
}

TEST_CASE("emit TSD entries bind the public key/value schema to mixed scalar types",
          "[feature:operator][feature:timed-scheduling][series:tsd][series:tsb][schema:named]") {
    const auto read_key = node([](In<TSB<operators::key_value>> entry,
                                  Out<TS<operators::KEY_VALUE_KEY>> output) {
        output.copy(entry->key);
    });
    const auto emit = [&](wp<TSD<string, TS<i64>>> value) {
        const auto entries = operators::emit(value);
        return read_key(entries);
    };

    CHECK(testing::test_graph(emit, testing::input<TSD<string, TS<i64>>>({{{"alpha", 1}}, {{"beta", 2}}}))
              .equals<string>({"alpha", "beta"}));
}

TEST_CASE("emit TSS additions and ignore removals",
          "[feature:operator][feature:timed-scheduling][series:tss][transition:addition][transition:removal]") {
    const auto emit = [](wp<TSS<i64>> value) { return operators::emit(value); };

    CHECK(testing::test_graph(emit,
                              {{1}, {literal::REMOVED(1), 2}, {literal::REMOVED(2), 3}})
              .equals<i64>({1, 2, 3}));
}

TEST_CASE("emit modified valid fixed TSL items",
          "[feature:operator][feature:timed-scheduling][series:fixed-tsl]"
          "[transition:update][transition:invalidation][transition:revalidation]") {
    const auto emit = [](wp<TSL<TS<i64>, 2>> value) { return operators::emit(value); };

    CHECK(testing::test_graph(emit,
                              {{{0, 1}, {1, 10}}, {{0, 2}}, {{1, literal::INVALID}}, {{1, 20}}})
              .equals<i64>({1, 10, 2, 20}));
}

TEST_CASE("emit valid TSD additions and modifications while ignoring invalidations and removals",
          "[feature:operator][feature:timed-scheduling][series:tsd][series:tsb]"
          "[transition:addition][transition:update][transition:invalidation][transition:removal]") {
    const auto sum_entry = node([](In<TS<i64>> key, In<TS<i64>> value) { return *key + *value; });
    const auto emit = [&](wp<TSD<i64, TS<i64>>> value) {
        const auto entries = operators::emit(value);
        return sum_entry(operators::getattr(entries, util::symbol{"key"}),
                         operators::getattr(entries, util::symbol{"value"}));
    };

    CHECK(testing::test_graph(emit,
                              testing::input<TSD<i64, TS<i64>>>({{{1, 10}}, {{2, 20}}, {{1, 11}},
                                                                 {{1, literal::INVALID}},
                                                                 {{1, literal::REMOVE}}, {{3, 30}}}))
              .equals<i64>({11, 22, 12, std::nullopt, std::nullopt, 33}));

    const auto simultaneous = testing::test_graph(emit,
                                                  testing::input<TSD<i64, TS<i64>>>({{{1, 10}, {2, 20}}}));
    CHECK((simultaneous.equals<i64>({11, 22}) || simultaneous.equals<i64>({22, 11})));
}

TEST_CASE("emit owns nested string values while later inputs replace their source collection",
          "[feature:operator][feature:timed-scheduling][series:value][scalar:list][scalar:string]"
          "[transition:update]") {
    const auto summarize = node([](In<TS<List<string>>> value) {
        auto result = std::string{};
        const auto items = value.value_view();
        for (auto index = std::size_t{}; index < items.size(); ++index) {
            result += items[index].value();
        }
        return result;
    });
    const auto emit = [&](wp<TS<List<List<string>>>> value) { return summarize(operators::emit(value)); };

    CHECK(testing::test_graph(
              emit, testing::input<TS<List<List<string>>>>({{{"old", "first"}, {"queued"}}, {{"new", "later"}}}))
              .equals<string>({"oldfirst", "queued", "newlater"}));
}

TEST_CASE("emit keeps queued values when its input reference rebinds",
          "[feature:operator][feature:timed-scheduling][feature:reference][series:value][scalar:list]"
          "[binding:reference-to-value][transition:update]") {
    const auto select_reference = node(
        [](In<TS<bool>> use_second, In<REF<TS<List<i64>>>> first, In<REF<TS<List<i64>>>> second,
           Out<REF<TS<List<i64>>>> output) { output.copy(*use_second ? second : first); });
    const auto emit = [&](wp<TS<bool>> use_second) {
        const auto first = operators::const_(ScalarValue<List<i64>>{std::array<i64, 3>{1, 2, 3}});
        const auto second = operators::const_(ScalarValue<List<i64>>{std::array<i64, 2>{10, 20}});
        return operators::emit(select_reference(use_second, first, second));
    };

    CHECK(testing::test_graph(emit, {false, true}).equals<i64>({1, 2, 3, 10, 20}));
}

TEST_CASE("emit releases a pending queue when a nested branch stops",
          "[feature:operator][feature:timed-scheduling][feature:nested-graph][transition:stop]"
          "[series:value][scalar:list]") {
    const auto inner = graph("name"_n = "emit_inner", [](wp<TS<List<i64>>> value) -> wp<TS<i64>> {
        return operators::emit(value).as<TS<i64>>();
    });
    const auto emit = [&](wp<TS<List<i64>>> value, wp<TS<bool>> run) { return nested(inner, run, value); };

    CHECK(testing::test_graph(emit, {{1, 2, 3}, literal::SKIP, {9}}, {true, false, true})
              .equals<i64>({1, std::nullopt, 9}));
}

TEST_CASE("emit rejects an output guarantee that can suppress emitted ticks",
          "[feature:operator][feature:wiring][guarantee:ordinary][guarantee:deduplicated]") {
    testing::test_wiring([] {
        auto &wiring = WiringContext::current().wiring();
        const auto &value = wiring.borrowed_input(TS<List<i64>>::type());
        CHECK(operators::emit(value).type().guarantee().value() == TimeSeriesGuarantee::ordinary);
    });

    testing::test_wiring([] {
        auto &wiring = WiringContext::current().wiring();
        const auto &value = wiring.borrowed_input(TS<List<i64>>::type());
        const auto output = operators::emit[type<TS<i64, TimeSeriesGuarantee::ordinary>>](value);
        CHECK(output.type().guarantee().value() == TimeSeriesGuarantee::ordinary);
    });

    CHECK_THROWS_AS(testing::test_wiring([] {
                         auto &wiring = WiringContext::current().wiring();
                         const auto &value = wiring.borrowed_input(TS<List<i64>>::type());
                         operators::emit[type<TS<i64, TimeSeriesGuarantee::deduplicated>>](value);
                     }),
                     WiringError);

    CHECK_THROWS_AS(testing::test_wiring([] {
                         auto &wiring = WiringContext::current().wiring();
                         const auto &value = wiring.borrowed_input(TS<List<i64>>::type());
                         operators::emit[type<TS<i64, TimeSeriesGuarantee::final>>](value);
                     }),
                     WiringError);

    CHECK_THROWS_AS(testing::test_wiring([] {
                         auto &wiring = WiringContext::current().wiring();
                         const auto &value = wiring.borrowed_input(TS<List<i64>>::type());
                         operators::emit[type<TS<i64, TimeSeriesGuarantee::const_>>](value);
                     }),
                     WiringError);

    auto deduplicated_reference_map = TypeMap{};
    deduplicated_reference_map.set(
        details::type_parameter<OUT>(), &REF<TS<i64>, TimeSeriesGuarantee::deduplicated>::type());
    const auto &deduplicated_reference_type_map = deduplicated_reference_map;
    CHECK_THROWS_AS(testing::test_wiring([&] {
                         auto &wiring = WiringContext::current().wiring();
                         const auto &value = wiring.borrowed_input(TS<List<i64>>::type());
                         const auto arguments = std::array<WiringPoint, 1>{value};
                         const auto points = std::span<const WiringPoint>{arguments};
                         operators::emit(deduplicated_reference_type_map, points);
                     }),
                     WiringError);

    auto final_reference_map = TypeMap{};
    final_reference_map.set(details::type_parameter<OUT>(), &REF<TS<i64>, TimeSeriesGuarantee::final>::type());
    const auto &final_reference_type_map = final_reference_map;
    CHECK_THROWS_AS(testing::test_wiring([&] {
                         auto &wiring = WiringContext::current().wiring();
                         const auto &value = wiring.borrowed_input(TS<List<i64>>::type());
                         const auto arguments = std::array<WiringPoint, 1>{value};
                         const auto points = std::span<const WiringPoint>{arguments};
                         operators::emit(final_reference_type_map, points);
                     }),
                     WiringError);

    auto const_reference_map = TypeMap{};
    const_reference_map.set(details::type_parameter<OUT>(), &REF<TS<i64>, TimeSeriesGuarantee::const_>::type());
    const auto &const_reference_type_map = const_reference_map;
    CHECK_THROWS_AS(testing::test_wiring([&] {
                         auto &wiring = WiringContext::current().wiring();
                         const auto &value = wiring.borrowed_input(TS<List<i64>>::type());
                         const auto arguments = std::array<WiringPoint, 1>{value};
                         const auto points = std::span<const WiringPoint>{arguments};
                         operators::emit(const_reference_type_map, points);
                     }),
                     WiringError);

    const auto empty_fixed = node([](Out<TSL<TS<i64>, 0>>) {});
    testing::test_wiring([&] {
        const auto output = operators::emit(empty_fixed());
        CHECK(output.type().guarantee().value() == TimeSeriesGuarantee::ordinary);
    });
}
