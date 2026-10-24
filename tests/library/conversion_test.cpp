#include <bg/library.hpp>
#include <bg/nested.hpp>
#include <bg/node.hpp>
#include <bg/scalar/bundle/scalar.hpp>
#include <bg/scalar/bundle/value.hpp>
#include <bg/run_graph.hpp>
#include <bg/testing/test_wiring.hpp>
#include <bg/time_series/tsb/tsb.hpp>
#include <bg/time_series/tsl/tsl.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <array>

using namespace bg;
using namespace bg::literals;
using namespace bg::util::literals;

template <template <typename, auto...> typename Field> struct ConversionScalarRecord {
    Field<i64> count;
    Field<double> ratio;
};

template <template <typename, auto...> typename Field>
struct ConversionDerivedScalarRecord : schema_extends<ConversionScalarRecord, Field> {
    Field<bool> extra;
};

template <template <typename, auto...> typename Field> struct ConversionOptionalScalarRecord {
    Field<i64> count;
    Field<double, optional> ratio;
};

template <template <typename, auto...> typename Field> struct ConversionOptionalSeriesRecord {
    Field<TS<double>, optional> ratio;
    Field<TS<i64>> count;
};

template <template <typename, auto...> typename Field> struct ConversionSeriesRecord {
    Field<TS<double>> ratio;
    Field<TS<i64>> count;
};

template <template <typename, auto...> typename Field> struct ConversionOtherSeriesRecord {
    Field<TS<i64>> count;
    Field<TS<double>> different;
};

TEST_CASE("convert copies scalar lists into fixed TSL snapshots",
          "[feature:operator][series:fixed-tsl][scalar:list]") {
    const auto list_to_tsl_sum = [] {
        const auto values = ScalarValue<List<i64>>{std::array<i64, 3>{1, 2, 3}};
        const auto source = operators::const_(values);
        const auto snapshot = operators::convert[type<TSL<TS<i64, TimeSeriesGuarantee::const_>, 3,
                                                         TimeSeriesGuarantee::const_>>](source);
        return operators::sum_(snapshot);
    };

    CHECK(bg::run_graph(list_to_tsl_sum).equals<i64>({6}));
}

TEST_CASE("convert copies scalar lists into TSS snapshots",
          "[feature:operator][series:tss][scalar:list]") {
    const auto list_to_tss_sum = [] {
        const auto values = ScalarValue<List<i64>>{std::array<i64, 3>{2, 4, 6}};
        const auto source = operators::const_(values);
        const auto snapshot = operators::convert[type<TSS<i64, TimeSeriesGuarantee::const_>>](source);
        return operators::sum_(snapshot);
    };

    CHECK(bg::run_graph(list_to_tss_sum).equals<i64>({12}));
}

TEST_CASE("convert copies scalar sets into TSS snapshots",
          "[feature:operator][series:tss][scalar:set]") {
    const auto set_to_tss_sum = [] {
        const auto values = ScalarValue<Set<i64>>{std::array<i64, 3>{2, 4, 6}};
        const auto source = operators::const_(values);
        const auto snapshot = operators::convert[type<TSS<i64, TimeSeriesGuarantee::const_>>](source);
        return operators::sum_(snapshot);
    };

    CHECK(bg::run_graph(set_to_tss_sum).equals<i64>({12}));
}

TEST_CASE("convert copies fixed TSL snapshots into scalar lists",
          "[feature:operator][series:fixed-tsl][scalar:list]") {
    const auto sum_list = node([](In<TS<List<i64>>> values) {
        const auto items = values.value_view();
        return items[0].value() + items[1].value();
    });
    const auto convert_and_sum = [&](wp<TSL<TS<i64>, 2>> values) {
        return sum_list(operators::convert[type<TS<List<i64>>>](values));
    };

    CHECK(bg::run_graph(convert_and_sum,
                              bg::input<TSL<TS<i64>, 2>>({{{0, 1}, {1, 2}}, {{0, 5}}}))
              .equals<i64>({3, 7}));
}

TEST_CASE("convert supports fixed TSL sizes",
          "[feature:operator][feature:type-system][series:fixed-tsl]") {
    const auto list = node([](Out<TS<List<i64>>>) {});
    const auto empty = node([](Out<TSL<TS<i64>, 0>>) {});
    const auto pair = node([](Out<TSL<TS<i64>, 2>>) {});

    testing::test_wiring([&] { (void)operators::convert[type<TSL<TS<i64>, 0>>](list()); });
    testing::test_wiring([&] { (void)operators::convert[type<TSL<TS<i64>, 2>>](list()); });
    testing::test_wiring([&] { (void)operators::convert[type<TS<List<i64>>>](empty()); });
    testing::test_wiring([&] { (void)operators::convert[type<TS<List<i64>>>](pair()); });

    CHECK_THROWS_WITH(
        testing::test_wiring([&] { (void)operators::convert[type<TSL<TS<double>, 2>>](list()); }),
        Catch::Matchers::ContainsSubstring("convert requires a compatible explicit time-series result type"));
    CHECK_THROWS_WITH(
        testing::test_wiring(
            [&] { (void)operators::convert[type<TS<List<i64>, TimeSeriesGuarantee::final>>](pair()); }),
        Catch::Matchers::ContainsSubstring("convert requires a compatible explicit time-series result type"));
}

TEST_CASE("convert copies TSS snapshots into scalar sets",
          "[feature:operator][series:tss][scalar:set]") {
    const auto sum_set = node([](In<TS<Set<i64>>> values) {
        auto sum = i64{};
        values.value_view().for_each([&](const auto &item) { sum += item.value(); });
        return sum;
    });
    const auto convert_and_sum = [&](wp<TSS<i64>> values) {
        return sum_set(operators::convert[type<TS<Set<i64>>>](values));
    };

    CHECK(bg::run_graph(convert_and_sum, bg::input<TSS<i64>>({{1, 2}, {literal::REMOVED(1), 3}}))
              .equals<i64>({3, 5}));
}

TEST_CASE("convert copies TSD snapshots into scalar dictionaries",
          "[feature:operator][series:tsd][scalar:dict]") {
    const auto sum_dict = node([](In<TS<Dict<i64, i64>>> values) {
        auto sum = i64{};
        values.value_view().for_each([&](const auto &, const auto &item) { sum += item.value(); });
        return sum;
    });
    const auto convert_and_sum = [&](wp<TSD<i64, TS<i64>>> values) {
        return sum_dict(operators::convert[type<TS<Dict<i64, i64>>>](values));
    };

    CHECK(bg::run_graph(convert_and_sum,
                              bg::input<TSD<i64, TS<i64>>>({{{1, 10}, {2, 20}},
                                                                 {{1, literal::REMOVE}, {2, 7}},
                                                                 {{2, literal::REMOVE}}}))
              .equals<i64>({30, 7, 0}));
}

TEST_CASE("convert maps Bundle and TSB fields by name",
          "[feature:operator][series:tsb][scalar:hierarchical][schema:named]") {
    const auto bundle_to_tsb = [](wp<TS<Bundle<ConversionScalarRecord>>> source) {
        return operators::convert[type<TSB<ConversionSeriesRecord>>](source);
    };

    CHECK(bg::run_graph(bundle_to_tsb,
                              bg::input<TS<Bundle<ConversionScalarRecord>>>(
                                  {{{"count", 7}, {"ratio", 2.5}}}))
              .equals({{{"ratio", 2.5}, {"count", 7}}}));

    const auto tsb_to_bundle = [](wp<TSB<ConversionSeriesRecord>> source) {
        return operators::convert[type<TS<Bundle<ConversionScalarRecord>>>](source);
    };

    CHECK(bg::run_graph(tsb_to_bundle,
                              bg::input<TSB<ConversionSeriesRecord>>({{{"ratio", 2.5}, {"count", 7}}}))
              .equals({{{"count", 7}, {"ratio", 2.5}}}));
}

TEST_CASE("convert maps fields from a derived Bundle through its declared base schema",
          "[feature:operator][series:tsb][scalar:hierarchical][schema:inheritance]") {
    auto memory = allocator{};
    const auto derived = bundle<ConversionDerivedScalarRecord>(memory, i64{7}, 2.5, true);
    const auto base = ScalarValue<Bundle<ConversionScalarRecord>>{derived.view()};
    const auto publish_base = node([](In<TS<i64>>, Scalar<Bundle<ConversionScalarRecord>> value,
                                      Out<TS<Bundle<ConversionScalarRecord>>> output) { output.set(value); });
    const auto convert_derived = [&](wp<TS<i64>> trigger) {
        const auto source = publish_base(trigger, base);
        return operators::convert[type<TSB<ConversionSeriesRecord>>](source);
    };

    CHECK(bg::run_graph(convert_derived, bg::input<TS<i64>>({0}))
              .equals({{{"ratio", 2.5}, {"count", 7}}}));
}

TEST_CASE("convert maps optional Bundle fields through present and absent updates",
          "[feature:operator][feature:validity][series:tsb][scalar:hierarchical][schema:optional]"
          "[transition:update][transition:invalidation][transition:revalidation]") {
    const auto bundle_to_tsb = [](wp<TS<Bundle<ConversionOptionalScalarRecord>>> source) {
        return operators::convert[type<TSB<ConversionOptionalSeriesRecord>>](source);
    };

    CHECK(bg::run_graph(
              bundle_to_tsb,
              bg::input<TS<Bundle<ConversionOptionalScalarRecord>>>(
                  {{{"count", 1}, {"ratio", 2.5}}, {{"count", 2}}, {{"count", 3}, {"ratio", 4.5}}}))
              .equals({{{"ratio", 2.5}, {"count", 1}}, {{"ratio", literal::INVALID}, {"count", 2}},
                       {{"ratio", 4.5}, {"count", 3}}}));
}

TEST_CASE("convert rejects a non-TSB result for a Bundle input",
          "[feature:operator][feature:wiring][series:tsb][scalar:hierarchical]") {
    const auto invalid_result = [](wp<TS<Bundle<ConversionScalarRecord>>> source) {
        return operators::convert[type<TS<List<i64>>>](source);
    };

    CHECK_THROWS_AS(bg::run_graph(invalid_result,
                                        bg::input<TS<Bundle<ConversionScalarRecord>>>(
                                            {{{"count", 7}, {"ratio", 2.5}}})),
                    WiringError);
}

TEST_CASE("convert rejects non-Bundle results for a TSB input",
          "[feature:operator][feature:wiring][series:tsb][series:fixed-tsl]") {
    const auto to_scalar_series = [](wp<TSB<ConversionSeriesRecord>> source) {
        return operators::convert[type<TS<double>>](source);
    };
    const auto to_fixed_list = [](wp<TSB<ConversionSeriesRecord>> source) {
        return operators::convert[type<TSL<TS<double>, 2>>](source);
    };
    const auto expect_resolver_rejection = [&](const auto &graph) {
        try {
            (void)bg::run_graph(graph, bg::input<TSB<ConversionSeriesRecord>>({{{"ratio", 2.5}, {"count", 7}}}));
            FAIL("expected a rejected TSB conversion candidate");
        } catch (const WiringError &error) {
            REQUIRE(error.diagnostic());
            CHECK_FALSE(error.cause());
            CHECK(std::ranges::any_of(error.diagnostic()->candidates, [](const auto &candidate) {
                return candidate.stage == WiringFailureStage::resolver;
            }));
            CHECK_FALSE(std::ranges::any_of(error.diagnostic()->candidates, [](const auto &candidate) {
                return candidate.stage == WiringFailureStage::callback;
            }));
        }
    };

    expect_resolver_rejection(to_scalar_series);
    expect_resolver_rejection(to_fixed_list);
}

TEST_CASE("convert rejects mismatched Bundle and TSB field names",
          "[feature:operator][series:tsb][scalar:hierarchical][schema:named]") {
    const auto mismatched_graph = [](wp<TS<Bundle<ConversionScalarRecord>>> source) {
        return operators::convert[type<TSB<ConversionOtherSeriesRecord>>](source);
    };
    CHECK_THROWS_AS(bg::run_graph(mismatched_graph,
                                        bg::input<TS<Bundle<ConversionScalarRecord>>>(
                                            {{{"count", 7}, {"ratio", 2.5}}})),
                    WiringError);
}

TEST_CASE("convert reconciles scalar dictionaries into TSD snapshots",
          "[feature:operator][series:tsd][scalar:dict]") {
    const auto dict_to_tsd_sum = [](wp<TS<Dict<i64, i64>>> values) {
        const auto snapshot = operators::convert[type<TSD<i64, TS<i64>>>](values);
        return operators::sum_(snapshot);
    };

    CHECK(bg::run_graph(dict_to_tsd_sum,
                              {{{1, 10}, {2, 20}}, {{2, 7}}, {}})
              .equals<i64>({30, 7, 0}));
}

TEST_CASE("convert reconciles repeated set snapshots without publishing false changes",
          "[feature:operator][feature:collection-change][series:tss][scalar:set]") {
    const auto observe_changes = node([](In<TSS<i64>> values) {
        auto changes = i64{};
        values.for_each_added([&](const auto &) { changes += 10; });
        values.for_each_removed([&](const auto &) { ++changes; });
        return changes;
    });
    const auto convert_and_observe = [&](wp<TS<Set<i64>>> values) {
        return observe_changes(operators::convert[type<TSS<i64>>](values));
    };

    CHECK(bg::run_graph(convert_and_observe,
                              bg::input<TS<Set<i64>>>({{1, 2}, {1, 2}, {2, 3}}))
              .equals<i64>({20, std::nullopt, 11}));
}

TEST_CASE("convert preserves a TSS snapshot when its scalar collection input is invalid",
          "[feature:operator][feature:validity][series:tss][scalar:list][guarantee:deduplicated]"
          "[transition:update][transition:invalidation][transition:revalidation]") {
    const auto observe = node([](In<TS<List<i64>, TimeSeriesGuarantee::deduplicated>, optional> values,
                                 In<TSS<i64, TimeSeriesGuarantee::deduplicated>, optional> snapshot) {
        return snapshot.valid() && (!values.valid() || values.value_view().empty());
    });
    const auto convert_and_observe = [&](wp<TS<List<i64>>> values) {
        const auto deduplicated = operators::dedup(values);
        const auto snapshot = operators::convert[type<TSS<i64, TimeSeriesGuarantee::deduplicated>>](deduplicated);
        return observe(deduplicated, snapshot);
    };

    CHECK(bg::run_graph(convert_and_observe, bg::input<TS<List<i64>>>({{}, {}, literal::INVALID, {}}))
              .equals<bool>({true, std::nullopt, true, true}));
}

TEST_CASE("combine captures homogeneous time-series arguments into a TSL",
          "[feature:operator][series:fixed-tsl][scalar:integer]") {
    const auto combine_and_sum = [](wp<TS<i64>> first, wp<TS<i64>> second) {
        const auto values = operators::combine[type<TSL<TS<i64>, 2>>](first, second);
        return operators::sum_(values);
    };

    CHECK(bg::run_graph(combine_and_sum, {1, 10}, {2, 20}).equals<i64>({3, 30}));
}

TEST_CASE("combine keeps TSL item validity for unmarked inputs",
          "[feature:operator][feature:validity][series:fixed-tsl][scalar:integer][validity:all-valid]"
          "[transition:update][transition:invalidation][transition:revalidation]") {
    const auto combine_values = [](wp<TS<i64>> first, wp<TS<i64>> second) {
        const auto values = operators::combine[type<TSL<TS<i64>, 2>>](first, second);
        CHECK(values.type().guarantee().value() == TimeSeriesGuarantee::ordinary);
        return values;
    };

    CHECK(bg::run_graph(combine_values, {1, 2, literal::INVALID, 3}, {10, literal::SKIP, literal::SKIP,
                                                                           literal::SKIP})
              .equals({{{0, 1}, {1, 10}}, {{0, 2}}, {{0, literal::INVALID}}, {{0, 3}}}));
}

TEST_CASE("combine keeps named TSB item validity for unmarked inputs",
          "[feature:operator][feature:validity][series:tsb][schema:named][validity:all-valid]"
          "[transition:update][transition:invalidation][transition:revalidation]") {
    const auto combine_values = [](wp<TS<i64>> count, wp<TS<double>> ratio) {
        const auto values = operators::combine[type<TSB<ConversionSeriesRecord>>]("ratio"_n = ratio,
                                                                                   "count"_n = count);
        CHECK(values.type().guarantee().value() == TimeSeriesGuarantee::ordinary);
        return values;
    };

    CHECK(bg::run_graph(combine_values, {7, 8, literal::INVALID, 9}, {2.5, literal::SKIP,
                                                                           literal::SKIP, literal::SKIP})
              .equals({{{"ratio", 2.5}, {"count", 7}}, {{"count", 8}}, {{"count", literal::INVALID}},
                       {{"count", 9}}}));
}

TEST_CASE("combine publishes initially partial TSL and TSB values from unmarked inputs",
          "[feature:operator][feature:validity][series:fixed-tsl][series:tsb][schema:named]"
          "[transition:first-tick][transition:update]") {
    const auto combine_tsl = [](wp<TS<i64>> count, wp<TS<i64>> ratio) {
        return operators::combine[type<TSL<TS<i64>, 2>>](count, ratio);
    };
    const auto combine_tsb = [](wp<TS<i64>> count, wp<TS<double>> ratio) {
        return operators::combine[type<TSB<ConversionSeriesRecord>>]("ratio"_n = ratio, "count"_n = count);
    };

    CHECK(bg::run_graph(combine_tsl, {literal::INVALID, 7}, {2, literal::SKIP})
              .equals({{{1, 2}}, {{0, 7}}}));
    CHECK(bg::run_graph(combine_tsb, {literal::INVALID, 7}, {2.5, literal::SKIP})
              .equals({{{"ratio", 2.5}}, {{"count", 7}}}));
}

TEST_CASE("combine publishes partial optional named TSB updates and item validity changes",
          "[feature:operator][feature:validity][series:tsb][schema:named][validity:optional]"
          "[transition:first-tick][transition:update][transition:invalidation][transition:revalidation]") {
    const auto combine_values = [](wp<TS<i64>> count, wp<TS<double>> ratio) {
        return operators::combine[type<TSB<ConversionSeriesRecord>>]("count"_n = opt(count),
                                                                      "ratio"_n = opt(ratio));
    };

    CHECK(bg::run_graph(combine_values,
                              {literal::INVALID, 7, literal::INVALID, literal::SKIP, 9},
                              {2.5, literal::SKIP, literal::SKIP, literal::INVALID, 3.5})
              .equals({{{"ratio", 2.5}}, {{"count", 7}}, {{"count", literal::INVALID}}, literal::INVALID,
                       {{"count", 9}, {"ratio", 3.5}}}));
}

TEST_CASE("combine forwards a free named TSB with a referenced field",
          "[feature:operator][feature:variadic-wiring][series:tsb][schema:named]"
          "[binding:reference-to-value]") {
    const auto sum = node([](In<TSB<ConversionSeriesRecord>> input) { return *input->count + *input->ratio; });
    const auto combine_values = [&](wp<TS<i64>> count, wp<TS<double>> ratio) {
        const auto values = bundle("count"_n = count.as<REF<TS<i64>>>(), "ratio"_n = ratio);
        const auto combined = operators::combine[type<TSB<ConversionSeriesRecord>>](kwargs(values));
        return sum(combined);
    };

    CHECK(bg::run_graph(combine_values, {7, 8}, {2.5, literal::SKIP}).equals<double>({9.5, 10.5}));
}

TEST_CASE("combine rejects explicit result guarantees stronger than its free collection",
          "[feature:operator][feature:wiring][series:fixed-tsl][series:tsb]"
          "[guarantee:deduplicated][guarantee:final][guarantee:const]") {
    const auto final_tsl = [](wp<TS<i64>> first, wp<TS<i64>> second) {
        return operators::combine[type<TSL<TS<i64>, 2, TimeSeriesGuarantee::final>>](first, second);
    };
    const auto const_tsb = [](wp<TS<i64>> count, wp<TS<double>> ratio) {
        return operators::combine[type<TSB<ConversionSeriesRecord, TimeSeriesGuarantee::const_>>](
            "count"_n = count, "ratio"_n = ratio);
    };
    const auto deduplicated_tsl = [](wp<TS<i64>> first, wp<TS<i64>> second) {
        return operators::combine[type<TSL<TS<i64, TimeSeriesGuarantee::deduplicated>, 2,
                                          TimeSeriesGuarantee::deduplicated>>](operators::dedup(first),
                                                                              operators::dedup(second));
    };

    CHECK_THROWS_AS(bg::run_graph(final_tsl, {1}, {2}), WiringError);
    CHECK_THROWS_AS(bg::run_graph(const_tsb, {1}, {2.0}), WiringError);
    CHECK_THROWS_AS(bg::run_graph(deduplicated_tsl, {1}, {2}), WiringError);
}

TEST_CASE("combine preserves partial optional TSL updates",
          "[feature:operator][feature:validity][series:fixed-tsl][scalar:integer][validity:optional]"
          "[transition:update][transition:invalidation]") {
    const auto combine_values = [](wp<TS<i64>> first, wp<TS<i64>> second) {
        return operators::combine[type<TSL<TS<i64>, 2>>](opt(first), opt(second));
    };

    CHECK(bg::run_graph(combine_values, {1, literal::INVALID, 3}, {10, literal::SKIP, literal::SKIP})
              .equals({{{0, 1}, {1, 10}}, {{0, literal::INVALID}}, {{0, 3}}}));
}

TEST_CASE("combine starts invalid when every TSL item is invalid",
          "[feature:operator][feature:validity][series:fixed-tsl][scalar:integer][validity:all-valid]"
          "[transition:first-tick]") {
    const auto combine_values = [](wp<TS<i64>> first, wp<TS<i64>> second) {
        return operators::combine[type<TSL<TS<i64>, 2>>](first, second);
    };

    CHECK(bg::run_graph(combine_values, {literal::INVALID, 2}, {literal::SKIP, 10})
              .equals({literal::SKIP, {{0, 2}, {1, 10}}}));
}

TEST_CASE("combine publishes an initially partial optional TSL",
          "[feature:operator][feature:validity][series:fixed-tsl][scalar:integer][validity:optional]"
          "[transition:first-tick][transition:update]") {
    const auto combine_values = [](wp<TS<i64>> first, wp<TS<i64>> second) {
        return operators::combine[type<TSL<TS<i64>, 2>>](opt(first), opt(second));
    };

    CHECK(bg::run_graph(combine_values, {literal::INVALID, 2}, {10, literal::SKIP})
              .equals({{{1, 10}}, {{0, 2}}}));
}

TEST_CASE("combine forwards a free TSL with a referenced item",
          "[feature:operator][feature:variadic-wiring][series:fixed-tsl][scalar:integer]"
          "[binding:reference-to-value]") {
    const auto combine_values = [](wp<TS<i64>> first, wp<TS<i64>> second) {
        const auto values = tsl(first.as<REF<TS<i64>>>(), second);
        const auto combined = operators::combine[type<TSL<TS<i64>, 2>>]("values"_n = values);
        return operators::sum_(combined);
    };

    CHECK(bg::run_graph(combine_values, {1, 2}, {10, literal::SKIP}).equals<i64>({11, 12}));
}

TEST_CASE("combine exposes partial items to all_valid consumers",
          "[feature:operator][feature:validity][validity:all-valid][series:fixed-tsl]"
          "[transition:first-tick][transition:update]") {
    const auto all_items_valid = [](wp<TS<i64>> first, wp<TS<i64>> second) {
        const auto values = operators::combine[type<TSL<TS<i64>, 2>>](opt(first), opt(second));
        return operators::all_valid(values);
    };

    CHECK(bg::run_graph(all_items_valid, {literal::INVALID, 2}, {10, literal::SKIP})
              .equals<bool>({false, true}));
}

TEST_CASE("combine gates TSL and TSB consumers that require all items valid",
          "[feature:operator][feature:validity][validity:all-valid][series:fixed-tsl][series:tsb]"
          "[transition:first-tick][transition:update]") {
    const auto sum_tsl = node([](In<TSL<TS<i64>, 2>, all_valid> values) { return *values[0] + *values[1]; });
    const auto sum_tsb = node([](In<TSB<ConversionSeriesRecord>, all_valid> values) {
        return *values->count + *values->ratio;
    });
    const auto combined_tsl = [&](wp<TS<i64>> count, wp<TS<i64>> ratio) {
        return sum_tsl(operators::combine[type<TSL<TS<i64>, 2>>](count, ratio));
    };
    const auto combined_tsb = [&](wp<TS<i64>> count, wp<TS<double>> ratio) {
        return sum_tsb(operators::combine[type<TSB<ConversionSeriesRecord>>]("count"_n = count,
                                                                                "ratio"_n = ratio));
    };

    CHECK(bg::run_graph(combined_tsl, {literal::INVALID, 2}, {10, literal::SKIP})
              .equals<i64>({std::nullopt, 12}));
    CHECK(bg::run_graph(combined_tsb, {literal::INVALID, 7}, {2.5, literal::SKIP})
              .equals<double>({std::nullopt, 9.5}));
}

TEST_CASE("combine preserves child guarantees under an ordinary collection root",
          "[feature:operator][feature:variadic-wiring][series:fixed-tsl]"
          "[guarantee:ordinary][guarantee:deduplicated][guarantee:final]") {
    testing::test_wiring([] {
        const auto final_source = node([](Out<TS<i64, TimeSeriesGuarantee::final>>) {});
        const auto final_values = operators::combine[type<TSL<TS<i64, TimeSeriesGuarantee::final>, 2>>](
            final_source(), final_source());
        const auto &final_type = type_cast<TimeSeriesListType>(final_values.type());
        CHECK(final_values.type().guarantee().value() == TimeSeriesGuarantee::ordinary);
        CHECK(final_type.item_type().guarantee().value() == TimeSeriesGuarantee::final);

        const auto deduplicated_source = node([](Out<TS<i64, TimeSeriesGuarantee::deduplicated>>) {});
        const auto deduplicated_values =
            operators::combine[type<TSL<TS<i64, TimeSeriesGuarantee::deduplicated>, 2>>](
                deduplicated_source(), deduplicated_source());
        const auto &deduplicated_type = type_cast<TimeSeriesListType>(deduplicated_values.type());
        CHECK(deduplicated_values.type().guarantee().value() == TimeSeriesGuarantee::ordinary);
        CHECK(deduplicated_type.item_type().guarantee().value() == TimeSeriesGuarantee::deduplicated);
    });
}

TEST_CASE("adapted variadic collection views preserve stronger source guarantees",
          "[feature:wiring][feature:variadic-wiring][series:fixed-tsl][series:tsb]"
          "[guarantee:ordinary][guarantee:final][representation:free]") {
    const auto final_list = node([](Out<TSL<TS<i64, TimeSeriesGuarantee::final>, 2, TimeSeriesGuarantee::final>>) {});
    const auto inspect_list = graph(
        "name"_n = "inspect_list",
        [](wp<TSL<TS<i64, TimeSeriesGuarantee::final>, 2, TimeSeriesGuarantee::final>, "values"_n, var_args> values) {
            const auto adapted = values.as<TSL<TS<i64>, 2>>();
            CHECK(&adapted.type() == &values.type());
            CHECK(adapted.type().guarantee().value() == TimeSeriesGuarantee::final);
            CHECK(adapted[0].type().guarantee().value() == TimeSeriesGuarantee::final);
        });

    const auto final_bundle =
        node([](Out<TSB<ConversionSeriesRecord, TimeSeriesGuarantee::final>>) {});
    const auto inspect_bundle = graph(
        "name"_n = "inspect_bundle",
        [](wp<TSB<ConversionSeriesRecord, TimeSeriesGuarantee::final>, "values"_n, var_kwargs> values) {
            const auto adapted = values.as<TSB<ConversionSeriesRecord>>();
            CHECK(&adapted.type() == &values.type());
            CHECK(adapted.type().guarantee().value() == TimeSeriesGuarantee::final);
            const auto &source_type = type_cast<TimeSeriesBundleType>(values.type());
            const auto count_index = source_type.find_item_index(util::symbol{"count"});
            REQUIRE(count_index);
            CHECK(&adapted->count.type() == &source_type.item_type(*count_index));
            CHECK(adapted->count.type().guarantee().value() == TimeSeriesGuarantee::final);
        });

    testing::test_wiring([&] { inspect_list("values"_n = final_list()); });
    testing::test_wiring([&] { inspect_bundle("values"_n = final_bundle()); });
}

TEST_CASE("combine reuses free TSL wiring and keeps item selection lazy",
          "[feature:operator][feature:variadic-wiring][series:fixed-tsl][representation:free]") {
    const auto source = node([](Out<TS<i64>>) {});
    auto wiring = GraphWiring{};
    const auto context = WiringContext{wiring};
    const auto values = operators::combine[type<TSL<TS<i64>, 2>>](source(), source()).as<TSL<TS<i64>, 2>>();

    (void)wiring.complete_pending_wiring(static_cast<WiringPoint>(values[0]));
    CHECK(wiring.nodes().size() == 1);
}

TEST_CASE("combine constructs an empty TSL from its requested item type",
          "[feature:operator][feature:variadic-wiring][series:fixed-tsl]") {
    testing::test_wiring([] {
        const auto empty = tsl(TS<i64>::type());
        const auto values = operators::combine[type<TSL<TS<i64>, 0>>]("values"_n = empty);
        const auto *list = type_cast<TimeSeriesListType>(&values.type());
        REQUIRE(list != nullptr);
        CHECK(list->item_count() == 0);
        CHECK(&list->item_type() == &TS<i64>::type());
    });
}

TEST_CASE("combine matches free bundle materialization for named TSB field selection",
          "[feature:operator][feature:variadic-wiring][series:tsb][schema:named][representation:free]") {
    const auto count_source = node([](Out<TS<i64>>) {});
    const auto ratio_source = node([](Out<TS<double>>) {});

    auto bundle_wiring = GraphWiring{};
    {
        const auto context = WiringContext{bundle_wiring};
        const auto values = bundle("count"_n = count_source(), "ratio"_n = ratio_source());
        (void)bundle_wiring.complete_pending_wiring(static_cast<WiringPoint>(operators::getattr(values, "count"_sym)));
    }

    auto wiring = GraphWiring{};
    {
        const auto context = WiringContext{wiring};
        const auto values = operators::combine[type<TSB<ConversionSeriesRecord>>]("count"_n = count_source(),
                                                                                    "ratio"_n = ratio_source())
                                .as<TSB<ConversionSeriesRecord>>();

        (void)wiring.complete_pending_wiring(static_cast<WiringPoint>(operators::getattr(values, "count"_sym)));
    }
    CHECK(bundle_wiring.nodes().size() == 2);
    CHECK(wiring.nodes().size() == bundle_wiring.nodes().size());
}

TEST_CASE("an adapted combined TSL supplies a nested reference consumer",
          "[feature:operator][feature:variadic-wiring][feature:nested-graph][feature:reference]"
          "[series:fixed-tsl][representation:free][binding:value-to-reference]") {
    const auto sum = node([](In<TSL<TS<i64>, 2>> values) { return *values[0] + *values[1]; });
    const auto inner = graph("name"_n = "inner", [&](wp<TSL<TS<i64>, 2>> values) { return sum(values); });
    const auto outer = [&](wp<TS<i64>> first, wp<TS<i64>> second, wp<TS<bool>> run) {
        const auto values = operators::combine[type<TSL<TS<i64>, 2>>](first, second).as<TSL<TS<i64>, 2>>();
        return nested(inner, run, values);
    };

    CHECK(bg::run_graph(outer, {1, 2, 3, 4}, {10, 20, 30, 40}, {true, true, false, true})
              .equals({11, 22, std::nullopt, 44}));
}
