#include <bg/graph.hpp>
#include <bg/map.hpp>
#include <bg/mesh.hpp>
#include <bg/nested.hpp>
#include <bg/node.hpp>
#include <bg/switch.hpp>
#include <bg/run_graph.hpp>
#include <bg/time_series/tsb/tsb.hpp>
#include <bg/time_series/tsl/tsl.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace {

using namespace bg;
using namespace bg::literals;

const auto copy_context = node([](In<TS<i64>, "value"_n, context> value) { return *value; });

const auto optional_value = node([](i64 value, bool valid, Out<TS<i64>> output) {
    if (valid) {
        output.set(value);
    } else {
        output.invalidate();
    }
});

const auto add_contexts = node([](In<TS<i64>, "left"_n, context> left, In<TS<i64>, "right"_n, context> right) {
    return *left + *right;
});

const auto context_reference_is_valid = node([](In<REF<TS<i64>>, "value"_n, context> value) { return value.valid(); });

} // namespace

TEST_CASE("a context time series crosses a nested graph boundary",
          "[feature:context-time-series][feature:nested-graph][shape:nested]") {
    const auto inner = graph("name"_n = "nested_context", []() -> wp<TS<i64>> { return copy_context(); });

    const auto tested = [&](wp<TS<i64>> value, wp<TS<bool>> run) {
        return with_context("value"_n = value, [&] { return nested(inner, run); });
    };

    const auto actual = bg::run_graph(tested, {2, 4, 8}, {true, true, true});

    CHECK(actual.equals({2, 4, 8}));
}

TEST_CASE("a nested graph can return a context time series directly",
          "[feature:context-time-series][feature:nested-graph][shape:nested]") {
    const auto inner = graph("name"_n = "returned_context",
                             [](wp<TS<i64>, "value"_n, context> value) { return value; });

    const auto tested = [&](wp<TS<i64>> value, wp<TS<bool>> run) {
        return with_context("value"_n = value, [&] { return nested(inner, run); });
    };

    const auto actual = bg::run_graph(tested, {2, 4, 8}, {true, true, true});

    CHECK(actual.equals({2, 4, 8}));
}

TEST_CASE("a nested graph can return a changing reference context directly",
          "[feature:context-time-series][feature:nested-graph][feature:switch][series:reference]") {
    const auto inner = graph("name"_n = "returned_reference_context",
                             [](wp<TS<i64>, "value"_n, context> value) { return value; });

    const auto tested = [&](wp<TS<i64>> selector, wp<TS<i64>> first, wp<TS<i64>> second, wp<TS<bool>> run) {
        const auto selected = switch_(selector,
                                      cases{
                                          case_(i64{1}, [](auto value, auto) { return value; }),
                                          case_(i64{2}, [](auto, auto value) { return value; }),
                                      },
                                      first, second);
        return with_context("value"_n = selected, [&] { return nested(inner, run); });
    };

    const auto actual = bg::run_graph(tested, {1, 2, 1, 2}, {10, 11, 12, 13},
                                            {20, 21, 22, 23}, {true, true, true, true});

    CHECK(actual.equals({10, 21, 12, 23}));
}

TEST_CASE("a nested graph can select an item from a context TSL during wiring",
          "[feature:context-time-series][feature:nested-graph][feature:getitem][series:fixed-tsl]") {
    const auto inner =
        graph("name"_n = "nested_context_list_item",
              [](wp<TSL<TS<i64>, 2>, "values"_n, context> values) -> wp<TS<i64>> { return values[0].as<TS<i64>>(); });

    const auto tested = [&](wp<TS<i64>> first, wp<TS<i64>> second, wp<TS<bool>> run) {
        return with_context("values"_n = tsl(first, second), [&] { return nested(inner, run); });
    };

    const auto actual = bg::run_graph(tested, {1, 2}, {10, 20}, {true, true});

    CHECK(actual.equals({1, 2}));
}

template <template <typename, auto...> typename Field> struct ContextBundle {
    Field<TS<i64>> value;
};

TEST_CASE("a nested graph can select a field from a context TSB during wiring",
          "[feature:context-time-series][feature:nested-graph][feature:getattr][series:tsb][schema:named]") {
    const auto inner =
        graph("name"_n = "nested_context_bundle_field",
              [](wp<TSB<ContextBundle>, "values"_n, context> values) -> wp<TS<i64>> { return values->value; });

    const auto tested = [&](wp<TS<i64>> value, wp<TS<bool>> run) {
        const auto values = bundle("value"_n = value);
        return with_context("values"_n = values, [&] { return nested(inner, run); });
    };

    const auto actual = bg::run_graph(tested, {3, 6}, {true, true});

    CHECK(actual.equals({3, 6}));
}

TEST_CASE("a nested graph context parameter inherits lexical context",
          "[feature:context-time-series][feature:nested-graph][definition:function]") {
    const auto inner =
        graph("name"_n = "declared_nested_context", [](wp<TS<i64>, "value"_n, context>) { return copy_context(); });

    const auto tested = [&](wp<TS<i64>> value, wp<TS<bool>> run) {
        return with_context("value"_n = value, [&] { return nested(inner, run); });
    };

    const auto actual = bg::run_graph(tested, {2, 4, 8}, {true, true, true});

    CHECK(actual.equals({2, 4, 8}));
}

TEST_CASE("an explicit nested graph context parameter overrides lexical context",
          "[feature:context-time-series][feature:nested-graph][definition:function]") {
    const auto inner =
        graph("name"_n = "overridden_nested_context", [](wp<TS<i64>, "value"_n, context>) { return copy_context(); });

    const auto tested = [&](wp<TS<i64>> outer, wp<TS<i64>> inner_value, wp<TS<bool>> run) {
        return with_context("value"_n = outer, [&] { return nested(inner, run, "value"_n = inner_value); });
    };

    const auto actual = bg::run_graph(tested, {1, 2}, {10, 20}, {true, true});

    CHECK(actual.equals({10, 20}));
}

TEST_CASE("a context time series crosses two nested graph boundaries",
          "[feature:context-time-series][feature:nested-graph][shape:doubly-nested]") {
    const auto leaf = graph("name"_n = "leaf_context", []() -> wp<TS<i64>> { return copy_context(); });

    const auto middle = graph("name"_n = "middle_context",
                              [&](wp<TS<bool>> run) -> wp<TS<i64>> { return wp<TS<i64>>{nested(leaf, run)}; });

    const auto tested = [&](wp<TS<i64>> value, wp<TS<bool>> run) {
        return with_context("value"_n = value, [&] { return nested(middle, run, run); });
    };

    const auto actual = bg::run_graph(tested, {3, 6, 9}, {true, true, true});

    CHECK(actual.equals({3, 6, 9}));
}

TEST_CASE("a collection context retains its semantic type across two nested graph boundaries",
          "[feature:context-time-series][feature:nested-graph][feature:getitem][series:fixed-tsl]"
          "[shape:doubly-nested]") {
    const auto leaf =
        graph("name"_n = "leaf_context_list_item",
              [](wp<TSL<TS<i64>, 2>, "values"_n, context> values) -> wp<TS<i64>> { return values[0].as<TS<i64>>(); });

    const auto middle =
        graph("name"_n = "middle_context_list_item", [&](wp<TS<bool>> run) { return nested(leaf, run).as<TS<i64>>(); });

    const auto tested = [&](wp<TS<i64>> first, wp<TS<i64>> second, wp<TS<bool>> run) {
        return with_context("values"_n = tsl(first, second), [&] { return nested(middle, run, run); });
    };

    const auto actual = bg::run_graph(tested, {1, 2},
                                            {10, 20}, {true, true});

    CHECK(actual.equals({1, 2}));
}

TEST_CASE("a context time series follows scalar nested graph arguments",
          "[feature:context-time-series][feature:nested-graph]") {
    const auto inner = graph("name"_n = "scalar_and_context", [](i64) -> wp<TS<i64>> { return copy_context(); });

    const auto tested = [&](wp<TS<i64>> value, wp<TS<bool>> run) {
        return with_context("value"_n = value, [&] { return nested(inner, run, i64{1}); });
    };

    const auto actual = bg::run_graph(tested, {3, 6, 9}, {true, true, true});

    CHECK(actual.equals({3, 6, 9}));
}

TEST_CASE("multiple context time series retain distinct nested graph bindings",
          "[feature:context-time-series][feature:nested-graph]") {
    const auto inner = graph("name"_n = "multiple_nested_contexts", []() -> wp<TS<i64>> { return add_contexts(); });

    const auto tested = [&](wp<TS<i64>> left, wp<TS<i64>> right, wp<TS<bool>> run) {
        auto left_scope = with_context("left"_n = left);
        auto right_scope = with_context("right"_n = right);
        return nested(inner, run);
    };

    const auto actual = bg::run_graph(tested, {1, 2, 3}, {10, 20, 30}, {true, true, true});

    CHECK(actual.equals({11, 22, 33}));
}

TEST_CASE("a reference context input retains its reference across a nested graph boundary",
          "[feature:context-time-series][feature:nested-graph][series:reference]") {
    const auto inner =
        graph("name"_n = "reference_context", []() -> wp<TS<bool>> { return context_reference_is_valid(); });

    const auto tested = [&](wp<TS<i64>> value, wp<TS<bool>> run) {
        return with_context("value"_n = value, [&] { return nested(inner, run); });
    };

    const auto actual = bg::run_graph(tested, {1, 2}, {true, true});

    CHECK(actual.equals({true, std::nullopt}));
}

TEST_CASE("an explicitly supplied context reference propagates target ticks",
          "[feature:context-time-series][feature:nested-graph][series:reference]") {
    const auto make_reference = node([](In<REF<TS<i64>>> input, Out<REF<TS<i64>>> output) {
        output.copy(input);
    });

    const auto inner = graph("name"_n = "ticking_reference_context", []() -> wp<TS<i64>> { return copy_context(); });

    const auto tested = [&](wp<TS<i64>> value, wp<TS<bool>> run) {
        const auto reference = make_reference(value);
        CHECK(reference.type().contains_reference());
        return with_context("value"_n = reference, [&] { return nested(inner, run); });
    };

    const auto actual = bg::run_graph(tested, {1, 2, 3}, {true, true, true});

    CHECK(actual.equals({1, 2, 3}));
}

TEST_CASE("a context time series is available to map branches",
          "[feature:context-time-series][feature:map][feature:nested-graph]") {
    const auto tested = [&](wp<TSD<i64, TS<i64>>> mapped, wp<TS<i64>> value) {
        return with_context("value"_n = value,
                            [&] { return map([](auto) { return copy_context(); }, mapped)[i64{1}]; });
    };

    const auto actual = bg::run_graph(tested, {{{1, 1}}, {{1, 2}}, {{1, 3}}}, {10, 20, 30});

    CHECK(actual.equals({10, 20, 30}));
}

TEST_CASE("a changing reference context evaluates only consuming map switch branches",
          "[feature:context-time-series][feature:map][feature:switch][feature:reference]"
          "[series:tss][series:reference][shape:doubly-nested][transition:update]") {
    constexpr auto branch_count = std::size_t{100};
    auto evaluations = std::array<std::size_t, branch_count>{};

    const auto keys = node([](In<TS<bool>> initialize, Out<TSS<i64>> output) {
        if (!*initialize) {
            return;
        }
        for (auto key = i64{}; key < static_cast<i64>(branch_count); ++key) {
            output.insert(key);
        }
    });

    const auto consuming = node(
        [&](In<TS<i64>, "value"_n, context> value, In<TS<i64, TimeSeriesGuarantee::const_>> key, Out<TS<i64>> output) {
            ++evaluations[static_cast<std::size_t>(*key)];
            output.set(*value);
        });

    const auto non_consuming = node([&](In<TS<i64, TimeSeriesGuarantee::const_>> key, Out<TS<i64>> output) {
        ++evaluations[static_cast<std::size_t>(*key)];
        output.set(-1);
    });

    const auto tested = [&](wp<TS<i64>> selector, wp<TS<i64>> first, wp<TS<i64>> second, wp<TS<bool>> initialize) {
        const auto selected = switch_(selector,
                                      cases{
                                          case_(i64{1}, [](auto value, auto) { return value; }),
                                          case_(i64{2}, [](auto, auto value) { return value; }),
                                      },
                                      first, second);
        return with_context("value"_n = selected, [&] {
            return map(
                       [&](auto key) -> wp<TS<i64>> {
                           return switch_(
                                      key,
                                      cases{
                                          case_(i64{0}, [&](auto branch_key) { return consuming(branch_key); }),
                                          case_(DEFAULT, [&](auto branch_key) { return non_consuming(branch_key); }),
                                      },
                                      key)
                               .template as<TS<i64>>();
                       },
                       keys(initialize))[i64{0}];
        });
    };

    const auto actual = bg::run_graph(tested, {1, 2, 1}, {10, literal::SKIP, literal::SKIP},
                                            {20, literal::SKIP, literal::SKIP},
                                            {true, literal::SKIP, literal::SKIP});

    CHECK(actual.equals({10, 20, 10}));
    auto expected = std::array<std::size_t, branch_count>{};
    expected.fill(1);
    expected[0] = 3;
    CHECK(evaluations == expected);
}

TEST_CASE("a context time series is available to switch branches",
          "[feature:context-time-series][feature:switch][feature:nested-graph]") {
    const auto tested = [&](wp<TS<i64>> key, wp<TS<i64>> value) {
        return with_context("value"_n = value, [&] {
            return switch_(key,
                           cases{
                               case_(i64{1}, [] { return copy_context(); }),
                               case_(i64{2}, [] { return copy_context(); }),
                           });
        });
    };

    const auto actual = bg::run_graph(tested, {1, 1, 2, 2}, {10, 20, 30, 40});

    CHECK(actual.equals({10, 20, 30, 40}));
}

TEST_CASE("a context time series is available to mesh branches",
          "[feature:context-time-series][feature:mesh][feature:nested-graph]") {
    const auto tested = [&](wp<TSD<i64, TS<i64>>> mapped, wp<TS<i64>> value) {
        return with_context("value"_n = value,
                            [&] { return mesh([](auto) { return copy_context(); }, mapped)[i64{1}]; });
    };

    const auto actual = bg::run_graph(tested, {{{1, 1}}, {{1, 2}}, {{1, 3}}}, {10, 20, 30});

    CHECK(actual.equals({10, 20, 30}));
}

TEST_CASE("a context time series follows a reference that changes target",
          "[feature:context-time-series][feature:switch][feature:nested-graph][series:reference]") {
    const auto inner = graph("name"_n = "reference_context_consumer", []() -> wp<TS<i64>> { return copy_context(); });

    const auto tested = [&](wp<TS<i64>> selector, wp<TS<i64>> first, wp<TS<i64>> second, wp<TS<bool>> run) {
        const auto selected = switch_(selector,
                                      cases{
                                          case_(i64{1}, [](auto value, auto) { return value; }),
                                          case_(i64{2}, [](auto, auto value) { return value; }),
                                      },
                                      first, second);
        return with_context("value"_n = selected, [&] { return nested(inner, run); });
    };
    const auto actual = bg::run_graph(tested, {1, 1, 2, 1}, {10, 11, 12, 13},
                                            {20, 21, 22, 23}, {true, true, true, true});

    CHECK(actual.equals({10, 11, 22, 13}));
}

TEST_CASE("a changing reference context updates through two nested boundaries in the same cycle",
          "[feature:context-time-series][feature:nested-graph][series:reference][shape:doubly-nested]") {
    const auto leaf = graph("name"_n = "reference_context_leaf", []() -> wp<TS<i64>> { return copy_context(); });

    const auto middle =
        graph("name"_n = "reference_context_middle", [&](wp<TS<bool>> run) { return nested(leaf, run).as<TS<i64>>(); });

    const auto tested = [&](wp<TS<i64>> selector, wp<TS<i64>> first, wp<TS<i64>> second, wp<TS<bool>> run) {
        const auto selected = switch_(selector,
                                      cases{
                                          case_(i64{1}, [](auto value, auto) { return value; }),
                                          case_(i64{2}, [](auto, auto value) { return value; }),
                                      },
                                      first, second);
        return with_context("value"_n = selected, [&] { return nested(middle, run, run); });
    };

    const auto actual = bg::run_graph(tested, {1, 2, 1, 2}, {10, literal::SKIP, literal::SKIP, literal::SKIP},
                                            {20, literal::SKIP, literal::SKIP, literal::SKIP},
                                            {true, true, true, true});

    CHECK(actual.equals({10, 20, 10, 20}));
}

namespace {

const auto publish_context_reference = node([](In<REF<TS<i64>>> value,
                                               Out<TSD<i64, REF<TS<i64>>>> output) {
    output[1].copy(value);
});

} // namespace

TEST_CASE("a TSD item reference context reacts to selector and target changes",
          "[feature:context-time-series][feature:switch][feature:reference][series:tsd]"
          "[transition:update][shape:nested]") {
    const auto add_context_offset = node([](In<TS<i64>> value, In<TS<i64>, "offset"_n, context> offset) {
        return *value + *offset;
    });

    const auto subtract_context_offset = node([](In<TS<i64>> value, In<TS<i64>, "offset"_n, context> offset) {
        return *value - *offset;
    });

    const auto tested = [&](wp<TS<i64>> selector, wp<TS<i64>> value, wp<TS<i64>> offset) {
        const auto reference_offset = publish_context_reference(offset)[i64{1}];
        return with_context("offset"_n = reference_offset, [&] {
            return switch_(selector,
                           cases{
                               case_(i64{1}, [&](auto input) { return add_context_offset(input); }),
                               case_(i64{2}, [&](auto input) { return subtract_context_offset(input); }),
                           },
                           value);
        });
    };

    const auto actual = bg::run_graph(tested,
                                      {1, 1, 1, 2, 2},
                                      {1, literal::SKIP, literal::SKIP, literal::SKIP, literal::SKIP},
                                      {10, literal::SKIP, literal::SKIP, literal::SKIP, 20});

    CHECK(actual.equals<i64>({11, std::nullopt, std::nullopt, -9, -19}));
}

TEST_CASE("a switch does not retick a TSD item reference argument for an unchanged selector",
          "[feature:switch][feature:reference][series:tsd][shape:nested]") {
    const auto read_value = node([](i64 value) { return value; });

    const auto tested = [&](wp<TS<i64>> selector, wp<TS<i64>> value) {
        const auto reference_value = publish_context_reference(value)[i64{1}];
        return switch_(selector,
                       cases{
                           case_(i64{1}, [&](auto input) { return read_value(input); }),
                           case_(i64{2}, [&](auto input) { return read_value(input); }),
                       },
                       reference_value);
    };

    const auto actual = bg::run_graph(tested,
                                      {1, 1, 2, 2},
                                      {7, literal::SKIP, literal::SKIP, literal::SKIP});

    CHECK(actual.equals<i64>({7, std::nullopt, 7, std::nullopt}));
}

TEST_CASE("a context time series preserves invalidation and revalidation",
          "[feature:context-time-series][feature:nested-graph][feature:validity]") {
    const auto inner = graph("name"_n = "validity_context_consumer", []() -> wp<TS<i64>> { return copy_context(); });

    const auto tested = [&](wp<TS<i64>> value, wp<TS<bool>> valid, wp<TS<bool>> run) {
        return with_context("value"_n = optional_value(value, valid), [&] { return nested(inner, run); });
    };

    const auto actual = bg::run_graph(tested, {1, 2, 3}, {true, false, true}, {true, true, true});

    CHECK(actual.equals({1, std::nullopt, 3}));
}

TEST_CASE("a context time series can become valid after an inner graph starts",
          "[feature:context-time-series][feature:nested-graph][feature:validity][transition:revalidation]") {
    const auto inner = graph("name"_n = "initially_invalid_context", []() -> wp<TS<i64>> { return copy_context(); });

    const auto tested = [&](wp<TS<i64>> value, wp<TS<bool>> valid, wp<TS<bool>> run) {
        return with_context("value"_n = optional_value(value, valid), [&] { return nested(inner, run); });
    };

    const auto actual =
        bg::run_graph(tested, {1, 2}, {false, true}, {true, true});

    CHECK(actual.equals({std::nullopt, 2}));
}

TEST_CASE("a restarted nested graph samples the current context value",
          "[feature:context-time-series][feature:nested-graph][feature:lifecycle][feature:sampling]"
          "[transition:restart]") {
    const auto inner = graph("name"_n = "restarted_context_consumer", []() -> wp<TS<i64>> { return copy_context(); });

    const auto tested = [&](wp<TS<i64>> value, wp<TS<bool>> run) {
        return with_context("value"_n = value, [&] { return nested(inner, run); });
    };

    const auto actual =
        bg::run_graph(tested, {1, 2, 3, 4}, {true, false, true, true});

    CHECK(actual.equals({1, std::nullopt, 3, 4}));
}

TEST_CASE("a new map branch samples an existing context value",
          "[feature:context-time-series][feature:map][feature:nested-graph][feature:sampling]") {
    const auto tested = [](wp<TSD<i64, TS<i64>>> values) {
        return with_context("value"_n = i64{42},
                            [&] { return map([](auto) { return copy_context(); }, values)[i64{1}]; });
    };

    const auto actual = bg::run_graph(tested, {literal::SKIP, {{1, 1}}});

    CHECK(actual.equals<i64>({std::nullopt, 42}));
}

TEST_CASE("a collection context preserves item change notifications",
          "[feature:context-time-series][feature:nested-graph][series:tsd]") {
    const auto copy_context_item = node([](In<TSD<i64, TS<i64>>, "values"_n, context> values) { return *values[1]; });

    const auto inner =
        graph("name"_n = "dictionary_context_consumer", [&]() -> wp<TS<i64>> { return copy_context_item(); });

    const auto tested = [&](wp<TSD<i64, TS<i64>>> values, wp<TS<bool>> run) {
        return with_context("values"_n = values, [&] { return nested(inner, run); });
    };

    const auto actual = bg::run_graph(tested, {{{1, 7}}, {{1, 8}}, {{1, 9}}}, {true, true, true});

    CHECK(actual.equals({7, 8, 9}));
}
