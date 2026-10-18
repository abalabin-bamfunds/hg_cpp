#include <bg/library.hpp>
#include <bg/node.hpp>
#include <bg/testing/test_graph.hpp>
#include <bg/testing/test_wiring.hpp>
#include <bg/time_series/tsb/tsb.hpp>
#include <bg/time_series/tsl/tsl.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <string_view>

using namespace bg;
using namespace bg::literals;

namespace {

template <template <typename, auto...> typename Field> struct length_bundle {
    Field<TS<i64>> first;
    Field<TS<double>> second;
};

const auto final_keys = node([](Out<TSS<i64, TimeSeriesGuarantee::final>> output) { output.apply({1, 2}); });

} // namespace

TEST_CASE("len_ of a TSS follows key membership", "[feature:operator][series:tss][scalar:i64][shape:single-node]") {
    const auto graph = [](wp<TSS<i64>> value) { return operators::len_("value"_n = value); };

    CHECK(testing::test_graph(graph, {{1}, {2, literal::REMOVED(1)}}).equals<i64>({1, 1}));
}

TEST_CASE("len_ of a fixed TSL is a wiring-time constant",
          "[feature:operator][series:fixed-tsl][scalar:i64][shape:single-node][guarantee:const]") {
    const auto graph = [] {
        const auto values = std::array{operators::const_(i64{1}), operators::const_(i64{2}), operators::const_(i64{3})};
        return operators::len_("value"_n = tsl(values));
    };

    CHECK(testing::test_graph(graph).equals<i64>({3}));

    auto wiring = GraphWiring{};
    auto result = WiringPoint{};
    {
        auto context = WiringContext{wiring};
        result = graph();
    }
    result = wiring.complete_pending_wiring(result);
    wiring.finalize();
    const auto compiled = CompiledGraph{wiring};
    CHECK(compiled.node_count() == 0);
}

TEST_CASE("len_ rejects a TSL with sparse item metadata",
          "[feature:operator][feature:wiring][series:fixed-tsl][scalar:i64][validity:required]") {
    testing::test_wiring([] {
        auto &wiring = WiringContext::current().wiring();
        const auto &sparse_type = time_series_list_type(TS<i64>::type(), std::dynamic_extent,
                                                        TimeSeriesGuarantee::ordinary);
        const auto sparse_input = wiring.borrowed_input(sparse_type);

        CHECK_THROWS_AS(operators::len_("value"_n = sparse_input), WiringError);
    });
}

TEST_CASE("len_ of a fixed TSB is a wiring-time constant",
          "[feature:operator][series:tsb][schema:named][scalar:i64][shape:single-node][guarantee:const]") {
    const auto graph = [] {
        const auto values = bundle<TSB<length_bundle>>(i64{1}, 2.0);
        return operators::len_("value"_n = values);
    };

    CHECK(testing::test_graph(graph).equals<i64>({2}));
}

TEST_CASE("len_ does not turn an invalid TSS into a valid empty set",
          "[feature:operator][series:tss][scalar:i64][validity:required][transition:first-tick]") {
    const auto graph = [](wp<TSS<i64>> values) { return operators::len_("value"_n = values); };

    CHECK(testing::test_graph(graph, {literal::SKIP, {}}).equals<i64>({std::nullopt, 0}));
}

TEST_CASE("contains_ on a TSS queries the current key set",
          "[feature:operator][series:tss][scalar:i64][shape:single-node][guarantee:ordinary]") {
    const auto graph = [](wp<TSS<i64>> collection) {
        return operators::contains_("collection"_n = collection, "key"_n = i64{1});
    };

    CHECK(testing::test_graph(graph, {{1}, {2, literal::REMOVED(1)}, {1}}).equals<bool>({true, false, true}));
}

TEST_CASE("contains_ distinguishes an invalid TSS from a valid empty TSS",
          "[feature:operator][series:tss][scalar:i64][validity:required][transition:first-tick]") {
    const auto graph = [](wp<TSS<i64>> collection) {
        return operators::contains_("collection"_n = collection, "key"_n = i64{1});
    };

    CHECK(testing::test_graph(graph, {literal::SKIP, {}}).equals<bool>({std::nullopt, false}));
}

TEST_CASE("contains_ on a TSS accepts a time-series key",
          "[feature:operator][series:tss][scalar:i64][shape:single-node][guarantee:ordinary]") {
    const auto graph = [](wp<TSS<i64>> collection, wp<TS<i64>> key) {
        return operators::contains_("collection"_n = collection, "key"_n = key);
    };

    CHECK(testing::test_graph(graph, {{1}, {2}}, {1, 2}).equals<bool>({true, true}));
}

TEST_CASE("is_empty observes required and optional TSS membership",
          "[feature:operator][series:tss][scalar:i64][validity:required][validity:optional]"
          "[transition:first-tick][transition:invalidation][transition:revalidation]") {
    const auto required = [](wp<TSS<i64>> values) { return operators::is_empty(values); };
    const auto optional = [](wp<TSS<i64>> values) { return operators::is_empty(opt(values)); };
    CHECK(testing::test_graph(required, testing::input<TSS<i64>>(
                                           {literal::SKIP, {}, {1}, {literal::REMOVED(1)}}))
              .equals<bool>({std::nullopt, true, false, true}));
    CHECK(testing::test_graph(optional, testing::input<TSS<i64>>(
                                           {literal::SKIP, {}, {1}, {literal::REMOVED(1)}}))
              .equals<bool>({std::nullopt, true, false, true}));
}

TEST_CASE("TSS equality accounts for simultaneous membership changes",
          "[feature:operator][series:tss][scalar:i64][guarantee:ordinary][transition:addition]"
          "[transition:removal]") {
    const auto equal = [](wp<TSS<i64>> lhs, wp<TSS<i64>> rhs) { return lhs == rhs; };
    const auto not_equal = [](wp<TSS<i64>> lhs, wp<TSS<i64>> rhs) { return operators::ne(lhs, rhs); };

    const auto equal_result = testing::test_graph(
        equal, testing::input<TSS<i64>>({{1}, {2}, {3}, {literal::REMOVED(2)}}),
        testing::input<TSS<i64>>({{1}, {3}, {2}, {literal::REMOVED(2)}}));
    const auto not_equal_result = testing::test_graph(
        not_equal, testing::input<TSS<i64>>({{1}, {2}, {3}, {literal::REMOVED(2)}}),
        testing::input<TSS<i64>>({{1}, {3}, {2}, {literal::REMOVED(2)}}));
    CHECK(equal_result.equals<bool>({true, false, true, true}));
    CHECK(not_equal_result.equals<bool>({false, true, false, false}));
}

TEST_CASE("TSS equality preserves mismatch counts across same-cycle key changes",
          "[feature:operator][series:tss][scalar:i64][transition:addition][transition:removal]") {
    const auto equal = [](wp<TSS<i64>> lhs, wp<TSS<i64>> rhs) { return lhs == rhs; };

    CHECK(testing::test_graph(
              equal,
              testing::input<TSS<i64>>({{}, {1}, {2}, {literal::REMOVED(1)}, {literal::REMOVED(2)}, {2},
                                        {literal::REMOVED(2)}, literal::SKIP, {3}}),
              testing::input<TSS<i64>>({{}, {1}, literal::SKIP, {literal::REMOVED(1)}, {2}, literal::SKIP,
                                        {literal::REMOVED(2)}, {3}, literal::SKIP}))
              .equals<bool>({true, true, false, false, false, true, true, false, true}));
}

TEST_CASE("TSS equality reconciles after required input invalidation",
          "[feature:operator][series:tss][scalar:i64][validity:required][transition:invalidation]"
          "[transition:revalidation]") {
    const auto equal = [](wp<TS<bool>> condition, wp<TSS<i64>> never_valid, wp<TSS<i64>> values) {
        return operators::if_then_else(condition, never_valid, values) == values;
    };

    const auto result = testing::test_graph(
        equal, testing::input<TS<bool>>({false, true, true, false}),
        testing::input<TSS<i64>>({literal::SKIP, literal::SKIP, literal::SKIP, literal::SKIP}),
        testing::input<TSS<i64>>({{1}, {1}, {2}, {2}}));
    CHECK(result.equals<bool>({true, std::nullopt, std::nullopt, true}));
}

TEST_CASE("optional TSS equality treats invalid inputs as empty with ordinary outputs",
          "[feature:operator][series:tss][scalar:i64][guarantee:ordinary][validity:optional]"
          "[transition:invalidation][transition:revalidation]") {
    const auto equal = [](wp<TSS<i64>> lhs, wp<TSS<i64>> rhs) {
        return operators::eq("lhs"_n = opt(lhs), "rhs"_n = opt(rhs));
    };

    CHECK(testing::test_graph(equal, testing::input<TSS<i64>>({literal::SKIP, {}, {1}}),
                              testing::input<TSS<i64>>({{}, {}, {2}}))
              .equals<bool>({true, true, false}));
}

TEST_CASE("TSS equality preserves required guarantees and weakens optional guarantees",
          "[feature:operator][series:tss][scalar:i64][guarantee:final][guarantee:deduplicated]"
          "[validity:required][validity:optional]") {
    const auto check_final = [](wp<TSS<i64, TimeSeriesGuarantee::final>> lhs,
                                wp<TSS<i64, TimeSeriesGuarantee::final>> rhs) {
        const auto required = operators::eq(lhs, rhs);
        const auto optional = operators::eq("lhs"_n = opt(lhs), "rhs"_n = opt(rhs));
        const auto empty = operators::is_empty("value"_n = opt(lhs));
        CHECK(&required.type() == &TS<bool, TimeSeriesGuarantee::final>::type());
        CHECK(&optional.type() == &TS<bool>::type());
        CHECK(&empty.type() == &TS<bool>::type());
        return required;
    };
    const auto check_deduplicated = [](wp<TSS<i64, TimeSeriesGuarantee::deduplicated>> lhs,
                                       wp<TSS<i64, TimeSeriesGuarantee::deduplicated>> rhs) {
        const auto required = operators::eq(lhs, rhs);
        const auto optional = operators::ne("lhs"_n = opt(lhs), "rhs"_n = opt(rhs));
        CHECK(&required.type() == &TS<bool, TimeSeriesGuarantee::deduplicated>::type());
        CHECK(&optional.type() == &TS<bool>::type());
        return required;
    };

    CHECK(testing::test_graph(check_final,
                              testing::input<TSS<i64, TimeSeriesGuarantee::final>>({{1}}),
                              testing::input<TSS<i64, TimeSeriesGuarantee::final>>({{1}}))
              .equals<bool>({true}));
    CHECK(testing::test_graph(check_deduplicated,
                              testing::input<TSS<i64, TimeSeriesGuarantee::deduplicated>>({{1}, {2}}),
                              testing::input<TSS<i64, TimeSeriesGuarantee::deduplicated>>({{1}, {3}}))
              .equals<bool>({true, false}));
}

TEST_CASE("TSS set operators publish only changes to their result",
          "[feature:operator][series:tss][scalar:i64][transition:addition][transition:removal]") {
    const auto set_union = [](wp<TSS<i64>> lhs, wp<TSS<i64>> rhs) { return lhs | rhs; };
    const auto intersection = [](wp<TSS<i64>> lhs, wp<TSS<i64>> rhs) { return lhs & rhs; };
    const auto difference = [](wp<TSS<i64>> lhs, wp<TSS<i64>> rhs) { return lhs - rhs; };
    const auto symmetric_difference = [](wp<TSS<i64>> lhs, wp<TSS<i64>> rhs) { return lhs ^ rhs; };
    CHECK(testing::test_graph(set_union,
                              testing::input<TSS<i64>>({{1, 2}, {3, literal::REMOVED(1)}, literal::SKIP,
                                                        {literal::REMOVED(2)}}),
                              testing::input<TSS<i64>>({{2, 3}, {1, literal::REMOVED(2)}, {2, literal::REMOVED(3)},
                                                        literal::SKIP}))
              .equals({{1, 2, 3}, literal::SKIP, literal::SKIP, literal::SKIP}));
    CHECK(testing::test_graph(intersection,
                              testing::input<TSS<i64>>({{1, 2}, {3, literal::REMOVED(1)}, literal::SKIP,
                                                        {literal::REMOVED(2)}}),
                              testing::input<TSS<i64>>({{2, 3}, {1, literal::REMOVED(2)}, {2, literal::REMOVED(3)},
                                                        literal::SKIP}))
              .equals({{2}, {3, literal::REMOVED(2)}, {2, literal::REMOVED(3)}, {literal::REMOVED(2)}}));
    CHECK(testing::test_graph(difference,
                              testing::input<TSS<i64>>({{1, 2}, {3, literal::REMOVED(1)}, literal::SKIP,
                                                        {literal::REMOVED(2)}}),
                              testing::input<TSS<i64>>({{2, 3}, {1, literal::REMOVED(2)}, {2, literal::REMOVED(3)},
                                                        literal::SKIP}))
              .equals({{1}, {2, literal::REMOVED(1)}, {3, literal::REMOVED(2)}, literal::SKIP}));
    CHECK(testing::test_graph(symmetric_difference,
                              testing::input<TSS<i64>>({{1, 2}, {3, literal::REMOVED(1)}, literal::SKIP,
                                                        {literal::REMOVED(2)}}),
                              testing::input<TSS<i64>>({{2, 3}, {1, literal::REMOVED(2)}, {2, literal::REMOVED(3)},
                                                        literal::SKIP}))
              .equals({{1, 3}, {2, literal::REMOVED(3)}, {3, literal::REMOVED(2)}, {2}}));
}

TEST_CASE("TSS set operations handle same-cycle additions, removals, and key moves",
          "[feature:operator][series:tss][scalar:i64][transition:addition][transition:removal]") {
    const auto set_union = [](wp<TSS<i64>> lhs, wp<TSS<i64>> rhs) { return lhs | rhs; };
    const auto intersection = [](wp<TSS<i64>> lhs, wp<TSS<i64>> rhs) { return lhs & rhs; };
    const auto difference = [](wp<TSS<i64>> lhs, wp<TSS<i64>> rhs) { return lhs - rhs; };
    const auto symmetric_difference = [](wp<TSS<i64>> lhs, wp<TSS<i64>> rhs) { return lhs ^ rhs; };
    CHECK(testing::test_graph(set_union, testing::input<TSS<i64>>({{1}, {2}, {literal::REMOVED(2)}, {3}}),
                              testing::input<TSS<i64>>({{}, {2}, {3, literal::REMOVED(2)},
                                                        {literal::REMOVED(3)}}))
              .equals({{1}, {2}, {3, literal::REMOVED(2)}, literal::SKIP}));
    CHECK(testing::test_graph(intersection, testing::input<TSS<i64>>({{1}, {2}, {literal::REMOVED(2)}, {3}}),
                              testing::input<TSS<i64>>({{}, {2}, {3, literal::REMOVED(2)},
                                                        {literal::REMOVED(3)}}))
              .equals({{}, {2}, {literal::REMOVED(2)}, literal::SKIP}));
    CHECK(testing::test_graph(difference, testing::input<TSS<i64>>({{1}, {2}, {literal::REMOVED(2)}, {3}}),
                              testing::input<TSS<i64>>({{}, {2}, {3, literal::REMOVED(2)},
                                                        {literal::REMOVED(3)}}))
              .equals({{1}, literal::SKIP, literal::SKIP, {3}}));
    CHECK(testing::test_graph(
              symmetric_difference, testing::input<TSS<i64>>({{1}, {2}, {literal::REMOVED(2)}, {3}}),
              testing::input<TSS<i64>>({{}, {2}, {3, literal::REMOVED(2)}, {literal::REMOVED(3)}}))
              .equals({{1}, literal::SKIP, {3}, literal::SKIP}));
}

TEST_CASE("TSS set operators propagate guarantees and weaken optional inputs",
          "[feature:operator][series:tss][scalar:i64][guarantee:final][guarantee:deduplicated]"
          "[guarantee:const][validity:optional]") {
    const auto check_final = [](wp<TSS<i64, TimeSeriesGuarantee::final>> lhs,
                                wp<TSS<i64, TimeSeriesGuarantee::final>> rhs) {
        const auto result = lhs | rhs;
        const auto optional = opt(lhs) | rhs;
        CHECK(&result.type() == &TSS<i64, TimeSeriesGuarantee::final>::type());
        CHECK(&optional.type() == &TSS<i64>::type());
        return result;
    };
    const auto check_deduplicated = [](wp<TSS<i64, TimeSeriesGuarantee::deduplicated>> lhs,
                                       wp<TSS<i64, TimeSeriesGuarantee::deduplicated>> rhs) {
        const auto result = lhs ^ rhs;
        CHECK(&result.type() == &TSS<i64, TimeSeriesGuarantee::deduplicated>::type());
        return result;
    };
    const auto check_const = [](wp<TSS<i64, TimeSeriesGuarantee::const_>> lhs,
                                wp<TSS<i64, TimeSeriesGuarantee::const_>> rhs) {
        const auto result = lhs & rhs;
        CHECK(&result.type() == &TSS<i64, TimeSeriesGuarantee::const_>::type());
        return result;
    };

    CHECK(testing::test_graph(check_final, testing::input<TSS<i64, TimeSeriesGuarantee::final>>({{1}}),
                              testing::input<TSS<i64, TimeSeriesGuarantee::final>>({{2}}))
              .equals({{1, 2}}));
    CHECK(testing::test_graph(
              check_deduplicated,
              testing::input<TSS<i64, TimeSeriesGuarantee::deduplicated>>({{1}}),
              testing::input<TSS<i64, TimeSeriesGuarantee::deduplicated>>({{2}}))
              .equals({{1, 2}}));
    CHECK(testing::test_graph(check_const, testing::input<TSS<i64, TimeSeriesGuarantee::const_>>({{1, 2}}),
                              testing::input<TSS<i64, TimeSeriesGuarantee::const_>>({{2, 3}}))
              .equals({{2}}));
}

TEST_CASE("TSS set operations accept erased wiring points and treat optional invalid sets as empty",
          "[feature:operator][series:tss][scalar:i64][validity:optional][transition:first-tick]"
          "[declaration:generic]") {
    const auto set_union = [](wp<> lhs, wp<> rhs) { return lhs | rhs; };
    const auto difference = [](wp<> lhs, wp<> rhs) {
        return operators::sub("lhs"_n = opt(lhs), "rhs"_n = opt(rhs));
    };
    CHECK(testing::test_graph(set_union, testing::input<TSS<i64>>({literal::SKIP, {1, 2}}),
                              testing::input<TSS<i64>>({{1}, {2}}))
              .equals({literal::SKIP, {1, 2}}));
    CHECK(testing::test_graph(difference, testing::input<TSS<i64>>({literal::SKIP, {1, 2}}),
                              testing::input<TSS<i64>>({{1}, {2}}))
              .equals({{}, literal::SKIP}));

    const auto one_optional = [](wp<TSS<i64>> lhs, wp<TSS<i64>> rhs) { return opt(lhs) | rhs; };
    CHECK(testing::test_graph(one_optional, testing::input<TSS<i64>>({literal::SKIP, {1, 2}}),
                              testing::input<TSS<i64>>({{1}, {2}}))
              .equals({{1}, {2}}));
}

TEST_CASE("TSS set operations retain members while a required reference is invalid",
          "[feature:operator][series:tss][series:reference][scalar:i64][validity:required]"
          "[transition:invalidation][transition:revalidation][transition:addition][transition:removal]") {
    const auto select = node([](In<TS<i64>> phase, In<REF<TSS<i64>>, passive> populated,
                                In<REF<TSS<i64>>, passive> invalid, Out<REF<TSS<i64>>> output) {
        output.copy(*phase == 1 || *phase == 2 ? invalid : populated);
    });
    const auto graph = [&](wp<TS<i64>> phase, wp<TSS<i64>> populated, wp<TSS<i64>> invalid,
                           wp<TSS<i64>> rhs) {
        return select(phase, populated, invalid) | rhs;
    };

    CHECK(testing::test_graph(graph, {0, 1, 2, 3}, {{1}, literal::SKIP, literal::SKIP, literal::SKIP}, {},
                              {{2}, {3, literal::REMOVED(2)}, {4, literal::REMOVED(3)}, literal::SKIP})
              .equals({{1, 2}, literal::SKIP, literal::SKIP, {4, literal::REMOVED(2)}}));
}

TEST_CASE("optional TSS set operations remove an invalid reference contribution",
          "[feature:operator][series:tss][series:reference][scalar:i64][validity:optional]"
          "[transition:invalidation][transition:revalidation]") {
    const auto set_union = [](wp<TS<bool>> condition, wp<TSS<i64>> populated, wp<TSS<i64>> invalid,
                              wp<TSS<i64>> rhs) {
        const auto selected = operators::if_then_else(condition, populated, invalid);
        return opt(selected) | rhs;
    };
    const auto intersection = [](wp<TS<bool>> condition, wp<TSS<i64>> populated, wp<TSS<i64>> invalid,
                                 wp<TSS<i64>> rhs) {
        const auto selected = operators::if_then_else(condition, populated, invalid);
        return opt(selected) & rhs;
    };
    const auto difference = [](wp<TS<bool>> condition, wp<TSS<i64>> populated, wp<TSS<i64>> invalid,
                               wp<TSS<i64>> rhs) {
        const auto selected = operators::if_then_else(condition, populated, invalid);
        return opt(selected) - rhs;
    };
    const auto symmetric_difference = [](wp<TS<bool>> condition, wp<TSS<i64>> populated,
                                          wp<TSS<i64>> invalid, wp<TSS<i64>> rhs) {
        const auto selected = operators::if_then_else(condition, populated, invalid);
        return opt(selected) ^ rhs;
    };

    CHECK(testing::test_graph(set_union, {true, false, true}, {{1, 2}, literal::SKIP, literal::SKIP}, {},
                              {{2}, literal::SKIP, literal::SKIP})
              .equals({{1, 2}, {literal::REMOVED(1)}, {1}}));
    CHECK(testing::test_graph(intersection, {true, false, true}, {{1, 2}, literal::SKIP, literal::SKIP}, {},
                              {{2}, literal::SKIP, literal::SKIP})
              .equals({{2}, {literal::REMOVED(2)}, {2}}));
    CHECK(testing::test_graph(difference, {true, false, true}, {{1, 2}, literal::SKIP, literal::SKIP}, {},
                              {{2}, literal::SKIP, literal::SKIP})
              .equals({{1}, {literal::REMOVED(1)}, {1}}));
    CHECK(testing::test_graph(symmetric_difference, {true, false, true},
                              {{1, 2}, literal::SKIP, literal::SKIP}, {}, {{2}, literal::SKIP, literal::SKIP})
              .equals({{1}, {2, literal::REMOVED(1)}, {1, literal::REMOVED(2)}}));
}

TEST_CASE("TSS set operations do not publish when a valid reference selects equal membership",
          "[feature:operator][series:tss][series:reference][scalar:i64][transition:update]") {
    const auto graph = [](wp<TS<bool>> condition, wp<TSS<i64>> first, wp<TSS<i64>> second,
                          wp<TSS<i64>> rhs) {
        return operators::if_then_else(condition, first, second) | rhs;
    };

    CHECK(testing::test_graph(graph, {true, false}, {{1}, literal::SKIP}, {{1}, literal::SKIP},
                              {{2}, literal::SKIP})
              .equals({{1, 2}, literal::SKIP}));
}

TEST_CASE("TSS set operations include the old target's same-cycle change when a reference is retargeted",
          "[feature:operator][series:tss][series:reference][transition:update][transition:retarget]") {
    const auto graph = [](wp<TS<bool>> condition, wp<TSS<i64>> lhs, wp<TSS<i64>> old_target,
                          wp<TSS<i64>> new_target) {
        return lhs | operators::if_then_else(condition, old_target, new_target);
    };

    CHECK(testing::test_graph(graph, {true, false}, {{1}, literal::SKIP}, {{2}, {3, literal::REMOVED(2)}},
                              {{3}, literal::SKIP})
              .equals({{1, 2}, {3, literal::REMOVED(2)}}));
}

TEST_CASE("TSS set operations do not replay a stale old-target delta when a reference is retargeted",
          "[feature:operator][series:tss][series:reference][transition:retarget]") {
    const auto graph = [](wp<TS<bool>> condition, wp<TSS<i64>> lhs, wp<TSS<i64>> old_target,
                          wp<TSS<i64>> new_target) {
        return lhs | operators::if_then_else(condition, old_target, new_target);
    };

    CHECK(testing::test_graph(graph, {true, false}, {{1}, literal::SKIP}, {{2}, literal::SKIP},
                              {{3}, literal::SKIP})
              .equals({{1, 2}, {3, literal::REMOVED(2)}}));
}

TEST_CASE("TSS equality sees same-cycle removals from a sampled previous target",
          "[feature:operator][series:tss][series:reference][transition:update][transition:retarget]") {
    const auto graph = [](wp<TS<bool>> condition, wp<TSS<i64>> old_target, wp<TSS<i64>> new_target) {
        return operators::if_then_else(condition, old_target, new_target) == new_target;
    };

    CHECK(testing::test_graph(graph, {true, false},
                              testing::input<TSS<i64>>({{2}, {3, literal::REMOVED(2)}}),
                              testing::input<TSS<i64>>({{3}, literal::SKIP}))
              .equals<bool>({false, true}));
}

TEST_CASE("TSS set operators support string keys",
          "[feature:operator][series:tss][scalar:string][transition:addition][transition:removal]") {
    const auto graph = [](wp<TSS<string>> lhs, wp<TSS<string>> rhs) { return lhs | rhs; };

    CHECK(testing::test_graph(graph, {{"alpha", "beta"}, {literal::REMOVED("alpha"), "gamma"}}, {{"beta"}})
              .equals({{"alpha", "beta"}, {"gamma", literal::REMOVED("alpha")}}));
}

TEST_CASE("contains_ follows a time-series key through invalidation and revalidation",
          "[feature:operator][series:tss][scalar:i64][shape:single-node][validity:required]"
          "[transition:invalidation][transition:revalidation]") {
    const auto graph = [](wp<TSS<i64>> collection, wp<TS<i64>> key) {
        return operators::contains_("collection"_n = collection, "key"_n = key);
    };

    CHECK(testing::test_graph(graph, {{1, 2}, {1, 2}, {1, 2}}, testing::input<TS<i64>>({1, literal::INVALID, 2}))
              .equals<bool>({true, std::nullopt, true}));
}

TEST_CASE("contains_ follows a selected TSS reference through retargeting and invalidation",
          "[feature:operator][feature:reference][series:tss][scalar:i64][binding:reference-to-value]"
          "[transition:update][transition:invalidation][transition:revalidation]") {
    const auto query = [](wp<TS<bool>> condition, wp<TSS<i64>> left, wp<TSS<i64>> right) {
        const auto selected = operators::if_then_else(condition, left, right);
        return operators::contains_("collection"_n = selected, "key"_n = i64{1});
    };

    CHECK(testing::test_graph(query, testing::input<TS<bool>>({true, false, literal::INVALID, true}),
                              {{1}, {2}, {3}, {1}}, {{2}, {2}, {3}, {3}})
              .equals<bool>({true, false, std::nullopt, true}));

    const auto observe_validity = [](wp<TS<bool>> condition, wp<TSS<i64>> left, wp<TSS<i64>> right) {
        const auto selected = operators::if_then_else(condition, left, right);
        return operators::valid(operators::contains_("collection"_n = selected, "key"_n = i64{1}));
    };
    CHECK(testing::test_graph(observe_validity, testing::input<TS<bool>>({true, false, literal::INVALID, true}),
                              {{1}, {2}, {3}, {1}}, {{2}, {2}, {3}, {3}})
              .equals<bool>({true, std::nullopt, false, true}));
}

TEST_CASE("contains_ propagates a final TSS guarantee",
          "[feature:operator][series:tss][scalar:i64][shape:single-node][guarantee:final]") {
    const auto graph = [] {
        const auto result = operators::contains_("collection"_n = final_keys(), "key"_n = i64{1});
        CHECK(&result.type() == &TS<bool, TimeSeriesGuarantee::final>::type());
        return result;
    };

    CHECK(testing::test_graph(graph).equals<bool>({true}));
}

TEST_CASE("contains_ combines a final TSS with an ordinary time-series key",
          "[feature:operator][series:tss][scalar:i64][shape:single-node][guarantee:ordinary][transition:update]") {
    const auto graph = [](wp<TS<i64>> key) {
        const auto result = operators::contains_("collection"_n = final_keys(), "key"_n = key);
        CHECK(&result.type() == &TS<bool>::type());
        return result;
    };

    CHECK(testing::test_graph(graph, {1, 2, 3}).equals<bool>({true, true, false}));
}

TEST_CASE("len_ forwards equal sizes for ordinary inputs and suppresses them for deduplicated inputs",
          "[feature:operator][series:tss][scalar:i64][guarantee:ordinary][guarantee:deduplicated]"
          "[transition:update]") {
    const auto ordinary = [](wp<TSS<i64>> values) { return operators::len_("value"_n = values); };
    const auto deduplicated = [](wp<TSS<i64, TimeSeriesGuarantee::deduplicated>> values) {
        return operators::len_("value"_n = values);
    };

    CHECK(testing::test_graph(ordinary,
                              {{1, 2}, {literal::REMOVED(1), 3}, {literal::REMOVED(3), 1}})
              .equals<i64>({2, 2, 2}));
    CHECK(testing::test_graph(
              deduplicated,
              testing::input<TSS<i64, TimeSeriesGuarantee::deduplicated>>(
                  {{1, 2}, {literal::REMOVED(1), 3}, {literal::REMOVED(3), 1}}))
              .equals<i64>({2, std::nullopt, std::nullopt}));
}

TEST_CASE("len_ and contains_ query scalar time-series lists",
          "[feature:operator][series:value][scalar:list][scalar:i64][shape:single-node]") {
    const auto length = [](wp<TS<List<i64>>> values) { return operators::len_("value"_n = values); };
    const auto contains = [](wp<TS<List<i64>>> values) {
        return operators::contains_("collection"_n = values, "key"_n = i64{2});
    };
    const auto contains_key_series = [](wp<TS<List<i64>>> values, wp<TS<i64>> key) {
        return operators::contains_("collection"_n = values, "key"_n = key);
    };

    CHECK(testing::test_graph(length, {{1, 2, 3}, {}}).equals<i64>({3, 0}));
    CHECK(testing::test_graph(contains, {{1, 2, 3}, {1, 3}, {}}).equals<bool>({true, false, false}));
    CHECK(testing::test_graph(contains_key_series, {{1, 2, 3}, {1, 3}, {}}, {2, 3, 1})
              .equals<bool>({true, true, false}));
}

TEST_CASE("len_ and contains_ query scalar time-series sets",
          "[feature:operator][series:value][scalar:set][scalar:i64][shape:single-node]") {
    const auto length = [](wp<TS<Set<i64>>> values) { return operators::len_("value"_n = values); };
    const auto contains = [](wp<TS<Set<i64>>> values) {
        return operators::contains_("collection"_n = values, "key"_n = i64{2});
    };
    const auto contains_key_series = [](wp<TS<Set<i64>>> values, wp<TS<i64>> key) {
        return operators::contains_("collection"_n = values, "key"_n = key);
    };

    CHECK(testing::test_graph(length, {{1, 2, 3}, {2, 3}}).equals<i64>({3, 2}));
    CHECK(testing::test_graph(contains, {{1, 2, 3}, {1, 3}, {}}).equals<bool>({true, false, false}));
    CHECK(testing::test_graph(contains_key_series, {{1, 2, 3}, {1, 3}, {}}, {2, 3, 1})
              .equals<bool>({true, true, false}));
}

TEST_CASE("len_ and contains_ query scalar time-series dictionaries",
          "[feature:operator][series:value][scalar:dict][scalar:i64][shape:single-node]") {
    const auto length = [](wp<TS<Dict<i64, i64>>> values) { return operators::len_("value"_n = values); };
    const auto contains = [](wp<TS<Dict<i64, i64>>> values) {
        return operators::contains_("collection"_n = values, "key"_n = i64{2});
    };
    const auto contains_key_series = [](wp<TS<Dict<i64, i64>>> values, wp<TS<i64>> key) {
        return operators::contains_("collection"_n = values, "key"_n = key);
    };

    CHECK(testing::test_graph(length, {{{1, 10}, {2, 20}}, {{2, 20}}}).equals<i64>({2, 1}));
    CHECK(testing::test_graph(contains, {{{1, 10}, {2, 20}}, {{1, 10}}, {}}).equals<bool>({true, false, false}));
    CHECK(testing::test_graph(contains_key_series, {{{1, 10}, {2, 20}}, {{1, 10}}, {}}, {2, 1, 3})
              .equals<bool>({true, true, false}));
}

TEST_CASE("len_ and contains_ query strings and symbols without copying text",
          "[feature:operator][series:value][scalar:string][scalar:symbol][shape:single-node]") {
    const auto string_length = [](wp<TS<string>> value) { return operators::len_("value"_n = value); };
    const auto symbol_length = [](wp<TS<symbol>> value) { return operators::len_("value"_n = value); };
    const auto string_contains = [](wp<TS<string>> value, wp<TS<string>> item) {
        return operators::contains_("collection"_n = value, "key"_n = item);
    };
    const auto symbol_contains = [](wp<TS<symbol>> value, wp<TS<symbol>> item) {
        return operators::contains_("collection"_n = value, "key"_n = item);
    };
    const auto strings = std::array{std::string_view{"a\0bc", 4}, std::string_view{"abc"}};
    const auto string_items = std::array{std::string_view{"\0b", 2}, std::string_view{"d"}};
    const auto symbols = std::array{symbol{"alphabet"}, symbol{"gamma"}};
    const auto symbol_items = std::array{symbol{"pha"}, symbol{"am"}};

    CHECK(testing::test_graph(string_length, strings).equals<i64>({4, 3}));
    CHECK(testing::test_graph(symbol_length, symbols).equals<i64>({8, 5}));
    CHECK(testing::test_graph(string_contains, strings, string_items).equals<bool>({true, false}));
    CHECK(testing::test_graph(symbol_contains, symbols, symbol_items).equals<bool>({true, true}));
}
