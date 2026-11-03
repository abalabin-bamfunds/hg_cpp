#include <hg/graph.hpp>
#include <hg/map.hpp>
#include <hg/nested.hpp>
#include <hg/node.hpp>
#include <hg/operators.hpp>
#include <hg/run_graph.hpp>
#include <hg/scalar/bundle/value.hpp>
#include <hg/scalar/dict/type.hpp>
#include <hg/scalar/opaque.hpp>
#include <hg/switch.hpp>
#include <hg/testing/test_wiring.hpp>
#include <hg/time_series/tsb/tsb.hpp>
#include <hg/time_series/tsd/tsd_type.hpp>
#include <hg/time_series/tss/tss_type.hpp>
#include <hg/type/time_series_dict_type.hpp>
#include <hg/type/time_series_reference_type.hpp>
#include <hg/type/time_series_value_type.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

namespace {

using namespace hg;
using namespace hg::literals;

TEST_CASE("map applies a graph to each scalar Dict entry",
          "[feature:map][feature:nested-graph][series:value][series:tsd][scalar:dict]"
          "[transition:addition][transition:update][transition:removal]") {
    const auto increment_value = node([](i64 value) { return value + 1; });
    const auto materialize = node([](In<TSD<i64, TS<i64>>> input, Out<TSD<i64, TS<i64>>> output) {
        output.copy_tick(input);
    });

    const auto tested = [&](wp<TS<Dict<i64, i64>>> values) { return materialize(map(increment_value, values)); };

    CHECK(hg::run_graph(tested,
                        {{{1, 10}, {2, 20}},
                         {{1, 11}, {2, 20}, {3, 30}},
                         {{1, 11}, {3, 30}},
                         {{1, 12}, {2, 20}, {3, 30}},
                         {}})
              .equals({{{1, 11}, {2, 21}},
                       {{1, 12}, {3, 31}},
                       {{2, literal::REMOVE}},
                       {{1, 13}, {2, 21}},
                       {{1, literal::REMOVE}, {2, literal::REMOVE}, {3, literal::REMOVE}}}));
}

TEST_CASE("map combines entries from a TSD and scalar Dict",
          "[feature:map][feature:nested-graph][series:tsd][scalar:dict][transition:addition][shape:nested]") {
    const auto combine = node([](In<TS<i64>, optional> value, In<TS<i64>, optional> dictionary_value) {
        return (value.valid() ? *value : 0) + (dictionary_value.valid() ? *dictionary_value : 0);
    });
    const auto materialize = node([](In<TSD<i64, TS<i64>>> input, Out<TSD<i64, TS<i64>>> output) {
        output.copy_tick(input);
    });

    const auto tested = [&](wp<TSD<i64, TS<i64>>> values, wp<TS<Dict<i64, i64>>> dictionary) {
        return materialize(map(combine, values, dictionary));
    };

    CHECK(hg::run_graph(tested, {{{1, 10}, {2, 20}}}, {{{2, 200}, {3, 300}}})
              .equals({{{1, 10}, {2, 220}, {3, 300}}}));
}

TEST_CASE("map schedules scalar Dict branches only when an entry changes",
          "[feature:map][feature:nested-graph][series:value][scalar:dict]"
          "[transition:addition][transition:update][transition:removal]") {
    auto evaluations = std::vector<std::pair<i64, i64>>{};
    const auto observe = node([&](i64 key, i64 value) { evaluations.emplace_back(key, value); });
    const auto tested = [&](wp<TS<Dict<i64, i64>>> values) {
        map([&](auto key, auto value) { observe(key, value); }, values, "__key_arg__"_n = 0);
        return values;
    };

    hg::run_graph(tested,
                  {{{1, 10}, {2, 20}},
                   {{1, 11}, {2, 20}, {3, 30}},
                   {{1, 11}, {3, 30}},
                   {{1, 12}, {2, 20}, {3, 30}},
                   {}});

    std::ranges::sort(evaluations);
    CHECK(evaluations == std::vector<std::pair<i64, i64>>{{1, 10}, {1, 11}, {1, 12}, {2, 20}, {2, 20}, {3, 30}});
}

TEST_CASE("map binds a named scalar argument beside a scalar Dict source",
          "[feature:map][feature:nested-graph][series:value][scalar:dict][scalar:i64]"
          "[shape:nested][definition:aggregate]") {
    struct AddOffset {
        wp<TS<i64>> value;
        i64 offset;

        auto wire() const {
            static const auto add = node([](i64 value, i64 offset) { return value + offset; });
            return add(value, offset);
        }
    };
    const auto materialize = node([](In<TSD<i64, TS<i64>>> input, Out<TSD<i64, TS<i64>>> output) {
        output.copy_tick(input);
    });

    const auto tested = [&](wp<TS<Dict<i64, i64>>> values) {
        return materialize(map(graph<AddOffset>(), "value"_n = values, "offset"_n = i64{5}));
    };

    CHECK(hg::run_graph(tested, {{{1, 10}, {2, 20}}, {{1, 11}}})
              .equals({{{1, 15}, {2, 25}}, {{1, 16}, {2, literal::REMOVE}}}));
}

TEST_CASE("map reads a scalar Dict through a reference",
          "[feature:map][feature:nested-graph][feature:reference][series:value][series:reference][scalar:dict]"
          "[binding:value-to-reference][transition:update]") {
    const auto increment_value = node([](i64 value) { return value + 1; });
    const auto materialize = node([](In<TSD<i64, TS<i64>>> input, Out<TSD<i64, TS<i64>>> output) {
        output.copy_tick(input);
    });

    const auto tested = [&](wp<TS<Dict<i64, i64>>> values) {
        const auto reference = wp<REF<TS<Dict<i64, i64>>>>{values};
        return materialize(map(increment_value, reference));
    };

    CHECK(hg::run_graph(tested, {{{1, 10}}, {{1, 11}, {2, 20}}})
              .equals({{{1, 11}}, {{1, 12}, {2, 21}}}));
}

TEST_CASE("map removes and restores branches when a scalar Dict source is invalidated",
          "[feature:map][feature:nested-graph][feature:validity][series:value][series:tsd][scalar:dict]"
          "[transition:invalidation][transition:revalidation]") {
    const auto increment_value = node([](i64 value) { return value + 1; });
    const auto materialize = node([](In<TSD<i64, TS<i64>>> input, Out<TSD<i64, TS<i64>>> output) {
        output.copy_tick(input);
    });

    const auto tested = [&](wp<TS<Dict<i64, i64>>> values) { return materialize(map(increment_value, values)); };

    CHECK(hg::run_graph(tested, {{{1, 10}}, literal::INVALID, {{1, 11}}})
              .equals({{{1, 11}}, {{1, literal::REMOVE}}, {{1, 12}}}));
}

TEST_CASE("map uses a scalar Dict as the selected key set",
          "[feature:map][feature:nested-graph][series:value][series:tsd][scalar:dict]"
          "[transition:addition][transition:removal]") {
    const auto select_value = graph("name"_n = "read_value_for_selected_dictionary_key",
                                    [](wp<TS<i64>> value, wp<TS<bool>>) { return value; });
    const auto materialize = node([](In<TSD<i64, TS<i64>>> input, Out<TSD<i64, TS<i64>>> output) {
        output.copy_tick(input);
    });

    const auto tested = [&](wp<TS<Dict<i64, i64>>> values, wp<TS<Dict<i64, bool>>> selected) {
        return materialize(map(select_value, values, selected, "__key_set_arg__"_n = 1));
    };

    CHECK(hg::run_graph(tested,
                        {{{1, 10}, {2, 20}},
                         {{1, 11}, {2, 21}},
                         {{1, 11}, {2, 21}},
                         {{1, 11}, {2, 21}}},
                        {{{1, true}},
                         {{1, true}},
                         {{1, true}, {2, true}},
                         {{2, true}}})
              .equals({{{1, 10}},
                       {{1, 11}},
                       {{2, 21}},
                       {{1, literal::REMOVE}}}));
}

TEST_CASE("map keeps ordinary item guarantees for scalar Dict values without equality",
          "[feature:map][feature:wiring][series:value][series:tsd][scalar:opaque][guarantee:ordinary]") {
    const auto values = node([](Out<TS<Dict<i64, Opaque<i64>>>>) {});
    const auto identity = graph("name"_n = "identity_opaque_dictionary_item",
                                [](wp<TS<Opaque<i64>>> value) { return value; });

    testing::test_wiring([&] {
        const auto result = map(identity, values());
        const auto &dictionary = type_cast<TimeSeriesDictionaryType>(result.type());
        const auto &item_reference = type_cast<TimeSeriesReferenceType>(dictionary.value_type());
        const auto &item = type_cast<TimeSeriesValueType>(item_reference.target_type());

        CHECK(item.guarantee().value() == TimeSeriesGuarantee::ordinary);
    });
}

} // namespace
