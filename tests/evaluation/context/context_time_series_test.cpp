#include <bg/graph.hpp>
#include <bg/map.hpp>
#include <bg/mesh.hpp>
#include <bg/nested.hpp>
#include <bg/node.hpp>
#include <bg/switch.hpp>
#include <bg/testing/test_graph.hpp>
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

const auto copy_context =
    node([](In<TS<std::int64_t>, "value"_n, context> value, Out<TS<std::int64_t>> output) { output.set(*value); });

const auto optional_value = node([](In<TS<std::int64_t>> value, In<TS<bool>> valid, Out<TS<std::int64_t>> output) {
    if (*valid) {
        output.set(*value);
    } else {
        output.invalidate();
    }
});

const auto copy_context_item = node([](In<TSD<std::int64_t, TS<std::int64_t>>, "values"_n, context> values,
                                       Out<TS<std::int64_t>> output) { output.set(*values[1]); });

const auto add_contexts =
    node([](In<TS<std::int64_t>, "left"_n, context> left, In<TS<std::int64_t>, "right"_n, context> right,
            Out<TS<std::int64_t>> output) { output.set(*left + *right); });

const auto context_reference_is_valid =
    node([](In<REF<TS<std::int64_t>>, "value"_n, context> value, Out<TS<bool>> output) { output.set(value.valid()); });

template <template <typename, auto...> typename Field> struct ContextBundle {
    Field<TS<std::int64_t>> value;
};

} // namespace

TEST_CASE("a context time series crosses a nested graph boundary",
          "[feature:context-time-series][feature:nested-graph][shape:nested]") {
    const auto inner = graph("name"_n = "nested_context", []() -> wp<TS<std::int64_t>> { return copy_context(); });
    const auto tested = [&](wp<TS<std::int64_t>> value, wp<TS<bool>> run) {
        return with_context("value"_n = value, [&] { return nested(inner, run); });
    };

    const auto actual = testing::test_graph(tested, std::array<std::int64_t, 3>{2, 4, 8}, std::array{true, true, true});

    CHECK(actual.equals({2, 4, 8}));
}

TEST_CASE("a nested graph can return a context time series directly",
          "[feature:context-time-series][feature:nested-graph][shape:nested]") {
    const auto inner = graph("name"_n = "returned_context",
                             [](wp<TS<std::int64_t>, "value"_n, context> value) -> wp<TS<std::int64_t>> {
                                 return value.as<TS<std::int64_t>>();
                             });
    const auto tested = [&](wp<TS<std::int64_t>> value, wp<TS<bool>> run) {
        return with_context("value"_n = value, [&] { return nested(inner, run); });
    };

    const auto actual = testing::test_graph(tested, std::array<std::int64_t, 3>{2, 4, 8}, std::array{true, true, true});

    CHECK(actual.equals({2, 4, 8}));
}

TEST_CASE("a nested graph can return a changing reference context directly",
          "[feature:context-time-series][feature:nested-graph][feature:switch][series:reference]") {
    const auto inner = graph("name"_n = "returned_reference_context",
                             [](wp<TS<std::int64_t>, "value"_n, context> value) -> wp<TS<std::int64_t>> {
                                 return value.as<TS<std::int64_t>>();
                             });
    const auto tested = [&](wp<TS<std::int64_t>> selector, wp<TS<std::int64_t>> first,
                            wp<TS<std::int64_t>> second, wp<TS<bool>> run) {
        const auto selected = switch_(selector,
                                      cases{
                                          case_(std::int64_t{1}, [](auto value, auto) { return value; }),
                                          case_(std::int64_t{2}, [](auto, auto value) { return value; }),
                                      },
                                      first, second);
        return with_context("value"_n = selected, [&] { return nested(inner, run); });
    };

    const auto actual = testing::test_graph(tested, std::array<std::int64_t, 4>{1, 2, 1, 2},
                                            std::array<std::int64_t, 4>{10, 11, 12, 13},
                                            std::array<std::int64_t, 4>{20, 21, 22, 23},
                                            std::array{true, true, true, true});

    CHECK(actual.equals({10, 21, 12, 23}));
}

TEST_CASE("a nested graph can select an item from a context TSL during wiring",
          "[feature:context-time-series][feature:nested-graph][feature:getitem][series:fixed-tsl]") {
    const auto inner = graph(
        "name"_n = "nested_context_list_item",
        [](wp<TSL<TS<std::int64_t>, 2>, "values"_n, context> values)
            -> wp<TS<std::int64_t>> { return values[0].as<TS<std::int64_t>>(); });
    const auto tested = [&](wp<TS<std::int64_t>> first, wp<TS<std::int64_t>> second, wp<TS<bool>> run) {
        return with_context("values"_n = tsl(first, second), [&] { return nested(inner, run); });
    };

    const auto actual = testing::test_graph(tested, std::array<std::int64_t, 2>{1, 2},
                                            std::array<std::int64_t, 2>{10, 20}, std::array{true, true});

    CHECK(actual.equals({1, 2}));
}

TEST_CASE("a nested graph can select a field from a context TSB during wiring",
          "[feature:context-time-series][feature:nested-graph][feature:getattr][series:tsb][schema:named]") {
    const auto inner =
        graph("name"_n = "nested_context_bundle_field",
              [](wp<TSB<ContextBundle>, "values"_n, context> values) -> wp<TS<std::int64_t>> { return values->value; });
    const auto tested = [&](wp<TS<std::int64_t>> value, wp<TS<bool>> run) {
        const auto values = bundle("value"_n = value).as<TSB<ContextBundle>>();
        return with_context("values"_n = values, [&] { return nested(inner, run); });
    };

    const auto actual = testing::test_graph(tested, std::array<std::int64_t, 2>{3, 6}, std::array{true, true});

    CHECK(actual.equals({3, 6}));
}

TEST_CASE("a nested graph context parameter inherits lexical context",
          "[feature:context-time-series][feature:nested-graph][definition:function]") {
    const auto inner = graph("name"_n = "declared_nested_context",
                             [](wp<TS<std::int64_t>, "value"_n, context>) { return copy_context(); });
    const auto tested = [&](wp<TS<std::int64_t>> value, wp<TS<bool>> run) {
        return with_context("value"_n = value, [&] { return nested(inner, run); });
    };

    const auto actual = testing::test_graph(tested, std::array<std::int64_t, 3>{2, 4, 8}, std::array{true, true, true});

    CHECK(actual.equals({2, 4, 8}));
}

TEST_CASE("an explicit nested graph context parameter overrides lexical context",
          "[feature:context-time-series][feature:nested-graph][definition:function]") {
    const auto inner = graph("name"_n = "overridden_nested_context",
                             [](wp<TS<std::int64_t>, "value"_n, context>) { return copy_context(); });
    const auto tested = [&](wp<TS<std::int64_t>> outer, wp<TS<std::int64_t>> inner_value, wp<TS<bool>> run) {
        return with_context("value"_n = outer, [&] { return nested(inner, run, "value"_n = inner_value); });
    };

    const auto actual = testing::test_graph(tested, std::array<std::int64_t, 2>{1, 2},
                                            std::array<std::int64_t, 2>{10, 20}, std::array{true, true});

    CHECK(actual.equals({10, 20}));
}

TEST_CASE("a context time series crosses two nested graph boundaries",
          "[feature:context-time-series][feature:nested-graph][shape:doubly-nested]") {
    const auto leaf = graph("name"_n = "leaf_context", []() -> wp<TS<std::int64_t>> { return copy_context(); });
    const auto middle = graph("name"_n = "middle_context", [&](wp<TS<bool>> run) -> wp<TS<std::int64_t>> {
        return wp<TS<std::int64_t>>{nested(leaf, run)};
    });
    const auto tested = [&](wp<TS<std::int64_t>> value, wp<TS<bool>> run) {
        return with_context("value"_n = value, [&] { return nested(middle, run, run); });
    };

    const auto actual = testing::test_graph(tested, std::array<std::int64_t, 3>{3, 6, 9}, std::array{true, true, true});

    CHECK(actual.equals({3, 6, 9}));
}

TEST_CASE("a collection context retains its semantic type across two nested graph boundaries",
          "[feature:context-time-series][feature:nested-graph][feature:getitem][series:fixed-tsl]"
          "[shape:doubly-nested]") {
    const auto leaf = graph(
        "name"_n = "leaf_context_list_item",
        [](wp<TSL<TS<std::int64_t>, 2>, "values"_n, context> values)
            -> wp<TS<std::int64_t>> { return values[0].as<TS<std::int64_t>>(); });
    const auto middle = graph("name"_n = "middle_context_list_item",
                              [&](wp<TS<bool>> run) { return nested(leaf, run).as<TS<std::int64_t>>(); });
    const auto tested = [&](wp<TS<std::int64_t>> first, wp<TS<std::int64_t>> second, wp<TS<bool>> run) {
        return with_context("values"_n = tsl(first, second), [&] { return nested(middle, run, run); });
    };

    const auto actual = testing::test_graph(tested, std::array<std::int64_t, 2>{1, 2},
                                            std::array<std::int64_t, 2>{10, 20}, std::array{true, true});

    CHECK(actual.equals({1, 2}));
}

TEST_CASE("a context time series follows scalar nested graph arguments",
          "[feature:context-time-series][feature:nested-graph]") {
    const auto inner =
        graph("name"_n = "scalar_and_context", [](std::int64_t) -> wp<TS<std::int64_t>> { return copy_context(); });
    const auto tested = [&](wp<TS<std::int64_t>> value, wp<TS<bool>> run) {
        return with_context("value"_n = value, [&] { return nested(inner, run, std::int64_t{1}); });
    };

    const auto actual = testing::test_graph(tested, std::array<std::int64_t, 3>{3, 6, 9}, std::array{true, true, true});

    CHECK(actual.equals({3, 6, 9}));
}

TEST_CASE("multiple context time series retain distinct nested graph bindings",
          "[feature:context-time-series][feature:nested-graph]") {
    const auto inner =
        graph("name"_n = "multiple_nested_contexts", []() -> wp<TS<std::int64_t>> { return add_contexts(); });
    const auto tested = [&](wp<TS<std::int64_t>> left, wp<TS<std::int64_t>> right, wp<TS<bool>> run) {
        auto left_scope = with_context("left"_n = left);
        auto right_scope = with_context("right"_n = right);
        return nested(inner, run);
    };

    const auto actual = testing::test_graph(tested, std::array<std::int64_t, 3>{1, 2, 3},
                                            std::array<std::int64_t, 3>{10, 20, 30}, std::array{true, true, true});

    CHECK(actual.equals({11, 22, 33}));
}

TEST_CASE("a reference context input retains its reference across a nested graph boundary",
          "[feature:context-time-series][feature:nested-graph][series:reference]") {
    const auto inner =
        graph("name"_n = "reference_context", []() -> wp<TS<bool>> { return context_reference_is_valid(); });
    const auto tested = [&](wp<TS<std::int64_t>> value, wp<TS<bool>> run) {
        return with_context("value"_n = value, [&] { return nested(inner, run); });
    };

    const auto actual = testing::test_graph(tested, std::array<std::int64_t, 2>{1, 2}, std::array{true, true});

    CHECK(actual.equals({true, std::nullopt}));
}

TEST_CASE("an explicitly supplied context reference propagates target ticks",
          "[feature:context-time-series][feature:nested-graph][series:reference]") {
    const auto inner =
        graph("name"_n = "ticking_reference_context", []() -> wp<TS<std::int64_t>> { return copy_context(); });
    const auto tested = [&](wp<TS<std::int64_t>> value, wp<TS<bool>> run) {
        const auto reference = value.as<REF<TS<std::int64_t>>>();
        return with_context("value"_n = reference, [&] { return nested(inner, run); });
    };

    const auto actual = testing::test_graph(tested, std::array<std::int64_t, 3>{1, 2, 3}, std::array{true, true, true});

    CHECK(actual.equals({1, 2, 3}));
}

TEST_CASE("a context time series is available to map branches",
          "[feature:context-time-series][feature:map][feature:nested-graph]") {
    const auto tested = [&](wp<TSD<std::int64_t, TS<std::int64_t>>> mapped, wp<TS<std::int64_t>> value) {
        return with_context("value"_n = value, [&] {
            return map([](auto) { return copy_context(); }, mapped)[std::int64_t{1}]
                .as<TS<std::int64_t>>();
        });
    };

    const auto actual = testing::test_graph(tested, {{{1, 1}}, {{1, 2}}, {{1, 3}}},
                                            std::array<std::int64_t, 3>{10, 20, 30});

    CHECK(actual.equals({10, 20, 30}));
}

TEST_CASE("a changing reference context evaluates only consuming map switch branches",
          "[feature:context-time-series][feature:map][feature:switch][feature:reference]"
          "[series:tss][series:reference][shape:doubly-nested][transition:update]") {
    constexpr auto branch_count = std::size_t{100};
    auto evaluations = std::array<std::size_t, branch_count>{};
    const auto keys = node([](In<TS<bool>> initialize, Out<TSS<std::int64_t>> output) {
        if (!*initialize) {
            return;
        }
        for (auto key = std::int64_t{}; key < static_cast<std::int64_t>(branch_count); ++key) {
            output.insert(key);
        }
    });
    const auto consuming = node([&](In<TS<std::int64_t>, "value"_n, context> value,
                                    In<TS<std::int64_t, TimeSeriesGuarantee::const_>> key,
                                    Out<TS<std::int64_t>> output) {
        ++evaluations[static_cast<std::size_t>(*key)];
        output.set(*value);
    });
    const auto non_consuming = node([&](In<TS<std::int64_t, TimeSeriesGuarantee::const_>> key,
                                        Out<TS<std::int64_t>> output) {
        ++evaluations[static_cast<std::size_t>(*key)];
        output.set(-1);
    });
    const auto tested = [&](wp<TS<std::int64_t>> selector, wp<TS<std::int64_t>> first,
                            wp<TS<std::int64_t>> second, wp<TS<bool>> initialize) {
        const auto selected = switch_(selector,
                                      cases{
                                          case_(std::int64_t{1}, [](auto value, auto) { return value; }),
                                          case_(std::int64_t{2}, [](auto, auto value) { return value; }),
                                      },
                                      first, second);
        return with_context("value"_n = selected, [&] {
            return map([&](auto key) -> wp<TS<std::int64_t>> {
                       return switch_(key,
                                      cases{
                                          case_(std::int64_t{0}, [&](auto branch_key) { return consuming(branch_key); }),
                                          case_(DEFAULT, [&](auto branch_key) { return non_consuming(branch_key); }),
                                      },
                                      key)
                           .template as<TS<std::int64_t>>();
                   },
                   keys(initialize))[std::int64_t{0}]
                .as<TS<std::int64_t>>();
        });
    };

    const auto actual = testing::test_graph(
        tested, std::array<std::int64_t, 3>{1, 2, 1},
        std::array<std::optional<std::int64_t>, 3>{10, std::nullopt, std::nullopt},
        std::array<std::optional<std::int64_t>, 3>{20, std::nullopt, std::nullopt},
        std::array<std::optional<bool>, 3>{true, std::nullopt, std::nullopt});

    CHECK(actual.equals({10, 20, 10}));
    auto expected = std::array<std::size_t, branch_count>{};
    expected.fill(1);
    expected[0] = 3;
    CHECK(evaluations == expected);
}

TEST_CASE("a context time series is available to switch branches",
          "[feature:context-time-series][feature:switch][feature:nested-graph]") {
    const auto tested = [&](wp<TS<std::int64_t>> key, wp<TS<std::int64_t>> value) {
        return with_context("value"_n = value, [&] {
            return switch_(key,
                           cases{
                               case_(std::int64_t{1}, [] { return copy_context(); }),
                               case_(std::int64_t{2}, [] { return copy_context(); }),
                           })
                .as<TS<std::int64_t>>();
        });
    };

    const auto actual = testing::test_graph(tested, std::array<std::int64_t, 4>{1, 1, 2, 2},
                                            std::array<std::int64_t, 4>{10, 20, 30, 40});

    CHECK(actual.equals({10, 20, 30, 40}));
}

TEST_CASE("a context time series is available to mesh branches",
          "[feature:context-time-series][feature:mesh][feature:nested-graph]") {
    const auto tested = [&](wp<TSD<std::int64_t, TS<std::int64_t>>> mapped, wp<TS<std::int64_t>> value) {
        return with_context("value"_n = value, [&] {
            return mesh([](auto) { return copy_context(); }, mapped)[std::int64_t{1}]
                .as<TS<std::int64_t>>();
        });
    };

    const auto actual = testing::test_graph(tested, {{{1, 1}}, {{1, 2}}, {{1, 3}}},
                                            std::array<std::int64_t, 3>{10, 20, 30});

    CHECK(actual.equals({10, 20, 30}));
}

TEST_CASE("a context time series follows a reference that changes target",
          "[feature:context-time-series][feature:switch][feature:nested-graph][series:reference]") {
    const auto inner =
        graph("name"_n = "reference_context_consumer", []() -> wp<TS<std::int64_t>> { return copy_context(); });
    const auto tested = [&](wp<TS<std::int64_t>> selector, wp<TS<std::int64_t>> first, wp<TS<std::int64_t>> second,
                            wp<TS<bool>> run) {
        const auto selected = switch_(selector,
                                      cases{
                                          case_(std::int64_t{1}, [](auto value, auto) { return value; }),
                                          case_(std::int64_t{2}, [](auto, auto value) { return value; }),
                                      },
                                      first, second);
        return with_context("value"_n = selected, [&] { return nested(inner, run); });
    };
    const auto actual = testing::test_graph(
        tested, std::array<std::int64_t, 4>{1, 1, 2, 1}, std::array<std::int64_t, 4>{10, 11, 12, 13},
        std::array<std::int64_t, 4>{20, 21, 22, 23}, std::array{true, true, true, true});

    CHECK(actual.equals({10, 11, 22, 13}));
}

TEST_CASE("a changing reference context updates through two nested boundaries in the same cycle",
          "[feature:context-time-series][feature:nested-graph][series:reference][shape:doubly-nested]") {
    const auto leaf =
        graph("name"_n = "reference_context_leaf", []() -> wp<TS<std::int64_t>> { return copy_context(); });
    const auto middle = graph("name"_n = "reference_context_middle", [&](wp<TS<bool>> run) {
        return nested(leaf, run).as<TS<std::int64_t>>();
    });
    const auto tested = [&](wp<TS<std::int64_t>> selector, wp<TS<std::int64_t>> first,
                            wp<TS<std::int64_t>> second, wp<TS<bool>> run) {
        const auto selected = switch_(selector,
                                      cases{
                                          case_(std::int64_t{1}, [](auto value, auto) { return value; }),
                                          case_(std::int64_t{2}, [](auto, auto value) { return value; }),
                                      },
                                      first, second);
        return with_context("value"_n = selected, [&] { return nested(middle, run, run); });
    };

    const auto actual = testing::test_graph(
        tested, std::array<std::int64_t, 4>{1, 2, 1, 2},
        std::array<std::optional<std::int64_t>, 4>{10, std::nullopt, std::nullopt, std::nullopt},
        std::array<std::optional<std::int64_t>, 4>{20, std::nullopt, std::nullopt, std::nullopt},
        std::array{true, true, true, true});

    CHECK(actual.equals({10, 20, 10, 20}));
}

TEST_CASE("a context time series preserves invalidation and revalidation",
          "[feature:context-time-series][feature:nested-graph][feature:validity]") {
    const auto inner =
        graph("name"_n = "validity_context_consumer", []() -> wp<TS<std::int64_t>> { return copy_context(); });
    const auto tested = [&](wp<TS<std::int64_t>> value, wp<TS<bool>> valid, wp<TS<bool>> run) {
        return with_context("value"_n = optional_value(value, valid), [&] { return nested(inner, run); });
    };

    const auto actual = testing::test_graph(tested, std::array<std::int64_t, 3>{1, 2, 3}, std::array{true, false, true},
                                            std::array{true, true, true});

    CHECK(actual.equals({1, std::nullopt, 3}));
}

TEST_CASE("a context time series can become valid after an inner graph starts",
          "[feature:context-time-series][feature:nested-graph][feature:validity][transition:revalidation]") {
    const auto inner =
        graph("name"_n = "initially_invalid_context", []() -> wp<TS<std::int64_t>> { return copy_context(); });
    const auto tested = [&](wp<TS<std::int64_t>> value, wp<TS<bool>> valid, wp<TS<bool>> run) {
        return with_context("value"_n = optional_value(value, valid), [&] { return nested(inner, run); });
    };

    const auto actual =
        testing::test_graph(tested, std::array<std::int64_t, 2>{1, 2}, std::array{false, true}, std::array{true, true});

    CHECK(actual.equals({std::nullopt, 2}));
}

TEST_CASE("a restarted nested graph samples the current context value",
          "[feature:context-time-series][feature:nested-graph][feature:lifecycle][feature:sampling]"
          "[transition:restart]") {
    const auto inner =
        graph("name"_n = "restarted_context_consumer", []() -> wp<TS<std::int64_t>> { return copy_context(); });
    const auto tested = [&](wp<TS<std::int64_t>> value, wp<TS<bool>> run) {
        return with_context("value"_n = value, [&] { return nested(inner, run); });
    };

    const auto actual =
        testing::test_graph(tested, std::array<std::int64_t, 4>{1, 2, 3, 4}, std::array{true, false, true, true});

    CHECK(actual.equals({1, std::nullopt, 3, 4}));
}

TEST_CASE("a new map branch samples an existing context value",
          "[feature:context-time-series][feature:map][feature:nested-graph][feature:sampling]") {
    const auto tested = [](wp<TSD<std::int64_t, TS<std::int64_t>>> values) {
        return with_context("value"_n = std::int64_t{42}, [&] {
            return map([](auto) { return copy_context(); }, values)[std::int64_t{1}]
                .as<TS<std::int64_t>>();
        });
    };

    const auto actual = testing::test_graph(tested, {{}, {{1, 1}}});

    CHECK(actual.equals({std::nullopt, 42}));
}

TEST_CASE("a collection context preserves item change notifications",
          "[feature:context-time-series][feature:nested-graph][series:tsd]") {
    const auto inner =
        graph("name"_n = "dictionary_context_consumer", []() -> wp<TS<std::int64_t>> { return copy_context_item(); });
    const auto tested = [&](wp<TSD<std::int64_t, TS<std::int64_t>>> values, wp<TS<bool>> run) {
        return with_context("values"_n = values, [&] { return nested(inner, run); });
    };

    const auto actual = testing::test_graph(tested, {{{1, 7}}, {{1, 8}}, {{1, 9}}}, std::array{true, true, true});

    CHECK(actual.equals({7, 8, 9}));
}
