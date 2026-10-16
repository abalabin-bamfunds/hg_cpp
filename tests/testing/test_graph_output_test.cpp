#include <bg/graph.hpp>
#include <bg/testing/test_graph.hpp>

#include <catch2/catch_test_macros.hpp>

#include <span>
#include <string>

namespace {

template <template <typename, auto...> typename Field> struct OutputBundle {
    Field<bg::TS<bg::i64>> value;
    Field<bg::TSD<bg::i64, bg::TS<bg::i64>>> nested;
};

template <template <typename, auto...> typename Field> struct OutputScalarBundle {
    Field<bg::i64> value;
};

TEST_CASE("test_graph captures TSL and TSS output deltas",
          "[testing][evaluation][feature:test-graph][series:fixed-tsl][series:tss][transition:update]") {
    const auto list_identity = [](bg::wp<bg::TSL<bg::TS<bg::i64>, 2>> input) { return input; };
    const auto list = bg::testing::test_graph(
        list_identity, {{{0, 1}, {1, 10}}, {{1, 20}}, literal::INVALID, {{0, 3}}});

    const auto set_identity = [](bg::wp<bg::TSS<bg::i64>> input) { return input; };
    const auto set = bg::testing::test_graph(set_identity, {{1, 2}, {literal::REMOVED(1), 3}, literal::SKIP});
    const auto empty_set = bg::testing::test_graph(set_identity, {{}});

    CHECK(list.equals({{{0, 1}, {1, 10}}, {{1, 20}}, literal::INVALID, {{0, 3}}}));
    CHECK_FALSE(list.equals({{{0, 1}, {1, 10}}, {{0, 20}}, literal::INVALID, {{0, 3}}}));
    CHECK_FALSE(list.equals({{{0, 1}, {1, 10}}, {{1, 20}}, literal::SKIP, {{0, 3}}}));
    CHECK(set.equals({{2, 1}, {3, literal::REMOVED(1)}, literal::SKIP}));
    CHECK_FALSE(set.equals({{1, 2}, {3, literal::REMOVED(2)}, literal::SKIP}));
    CHECK(empty_set.equals({{}}));
}

TEST_CASE("test_graph compares runtime-typed scalar bundle literals",
          "[testing][evaluation][feature:test-graph][series:value][scalar:hierarchical][declaration:generic]") {
    const auto identity = [](bg::wp<> input) { return bg::wp<>{input}; };
    const auto actual = bg::testing::test_graph(
        identity,
        bg::testing::input<bg::TS<bg::Bundle<OutputScalarBundle>>>({{{"value", 42}}}));

    CHECK(actual.equals({{{"value", 42}}}));
}

TEST_CASE("test_graph preserves missing and empty collection output ticks",
          "[testing][evaluation][feature:test-graph][series:tss][transition:first-tick]") {
    const auto identity = [](bg::wp<bg::TSS<bg::i64>> input) { return input; };

    const auto skipped = bg::testing::test_graph(
        identity, {literal::SKIP, literal::SKIP, literal::SKIP});
    const auto no_ticks = bg::testing::test_graph(
        identity, bg::testing::TestGraphLiteralInput(std::span<const literal::TimeSeriesLiteral>{}));
    const auto empty = bg::testing::test_graph(identity, {{}});

    CHECK(skipped.equals({literal::SKIP, literal::SKIP, literal::SKIP}));
    CHECK(no_ticks.equals({literal::SKIP}));
    CHECK(empty.equals({{}}));
}

TEST_CASE("test_graph captures nested TSD and TSB output deltas",
          "[testing][evaluation][feature:test-graph][series:tsd][series:tsb][shape:nested]") {
    const auto bundle_identity = [](bg::wp<bg::TSB<OutputBundle>> input) { return input; };
    const auto bundle = bg::testing::test_graph(
        bundle_identity,
        {{{"value", 1}, {"nested", {{2, 20}, {3, 30}}}},
         {{"value", 4}, {"nested", {{3, 31}}}},
         literal::SKIP});

    const auto nested_identity = [](bg::wp<bg::TSD<bg::i64, bg::TSB<OutputBundle>>> input) { return input; };
    const auto nested = bg::testing::test_graph(
        nested_identity,
        {{{1, {{"value", 1}, {"nested", {{2, 20}}}}}},
         {{1, {{"nested", {{2, literal::REMOVE}}}}}},
         {{1, literal::REMOVE}}});
    const auto dictionary_identity = [](bg::wp<bg::TSD<bg::i64, bg::TS<bg::i64>>> input) { return input; };
    const auto dictionary = bg::testing::test_graph(dictionary_identity, {{{1, 10}, {2, 20}}});

    CHECK(bundle.equals({{{"value", 1}, {"nested", {{2, 20}, {3, 30}}}},
                         {{"value", 4}, {"nested", {{3, 31}}}}, literal::SKIP}));
    CHECK(nested.equals({{{1, {{"value", 1}, {"nested", {{2, 20}}}}}},
                         {{1, {{"nested", {{2, literal::REMOVE}}}}}},
                         {{1, literal::REMOVE}}}));
    CHECK_FALSE(nested.equals({{{1, {{"value", 1}, {"nested", {{2, 20}}}}}},
                               {{1, {{"nested", {{2, literal::REMOVE}}}}}},
                               {{2, literal::REMOVE}}}));
    CHECK(dictionary.equals({{{2, 20}, {1, 10}}}));
    CHECK_FALSE(dictionary.equals({{{1, 10}, {1, 10}, {2, 20}}}));
    CHECK_FALSE(dictionary.equals({{{1, 10}}}));
}

TEST_CASE("test_graph compares invalid nested collection values",
          "[testing][evaluation][feature:test-graph][series:tsd][shape:nested][transition:invalidation]") {
    const auto invalidate_set = bg::node([](bg::Out<bg::TSD<bg::i64, bg::TSS<bg::i64>>> output) {
        output[1].invalidate();
    });
    const auto invalidate_dictionary = bg::node(
        [](bg::Out<bg::TSD<bg::i64, bg::TSD<bg::i64, bg::TS<bg::i64>>>> output) { output[1].invalidate(); });
    const auto set_child_graph = [&] { return invalidate_set(); };
    const auto dictionary_child_graph = [&] { return invalidate_dictionary(); };
    const auto set_child = bg::testing::test_graph(set_child_graph);
    const auto dictionary_child = bg::testing::test_graph(dictionary_child_graph);
    const auto scalar_child_identity =
        [](bg::wp<bg::TSD<bg::i64, bg::TS<bg::i64>>> input) { return input; };
    const auto scalar_child = bg::testing::test_graph(scalar_child_identity, {{{1, literal::INVALID}}});

    CHECK(set_child.equals({{{1, literal::INVALID}}}));
    CHECK_FALSE(set_child.equals({{{1, {}}}}));
    CHECK(dictionary_child.equals({{{1, literal::INVALID}}}));
    CHECK_FALSE(dictionary_child.equals({{{1, {}}}}));
    CHECK(scalar_child.equals({{{1, literal::INVALID}}}));
    CHECK_FALSE(scalar_child.equals({{{1, 1}}}));
}

TEST_CASE("test_graph captures a top-level reference to a collection",
          "[testing][evaluation][feature:test-graph][feature:reference][series:tsd]"
          "[binding:reference-to-value][transition:update]") {
    const auto select = bg::node([](bg::In<bg::TS<bool>> choose_second,
                                    bg::In<bg::REF<bg::TSD<bg::i64, bg::TS<bg::i64>>>> first,
                                    bg::In<bg::REF<bg::TSD<bg::i64, bg::TS<bg::i64>>>> second,
                                    bg::Out<bg::REF<bg::TSD<bg::i64, bg::TS<bg::i64>>>> output) {
        output.copy(*choose_second ? second : first);
    });
    const auto graph = [&](bg::wp<bg::TS<bool>> choose_second,
                           bg::wp<bg::TSD<bg::i64, bg::TS<bg::i64>>> first,
                           bg::wp<bg::TSD<bg::i64, bg::TS<bg::i64>>> second) {
        return select(choose_second, first, second);
    };
    const auto actual = bg::testing::test_graph(graph, {false, true}, {{{1, 10}}}, {{{2, 20}}});

    CHECK(actual.equals({{{1, 10}}, {{1, literal::REMOVE}, {2, 20}}}));
}

TEST_CASE("test_graph rejects nested references in collection output capture",
          "[testing][evaluation][feature:test-graph][feature:reference][series:tsd][series:tss]"
          "[binding:reference-to-value][shape:nested]") {
    const auto make_dictionary = bg::node([](bg::In<bg::REF<bg::TSS<bg::i64>>> item,
                                             bg::Out<bg::TSD<bg::i64, bg::REF<bg::TSS<bg::i64>>>> output) {
        output[1].copy(item);
    });
    const auto graph = [&](bg::wp<bg::TSS<bg::i64>> input) { return make_dictionary(input); };

    CHECK_THROWS_AS(bg::testing::test_graph(graph, {{1, 2}}), std::invalid_argument);
}

TEST_CASE("test_graph owns string keys and values in captured collection deltas",
          "[testing][evaluation][feature:test-graph][series:tsd][scalar:string][feature:scalar-storage]") {
    const auto actual = [] {
        const auto key = std::string(128, 'k');
        const auto first_value = std::string(160, 'a');
        const auto replacement_value = std::string(192, 'b');
        const auto identity = [](bg::wp<bg::TSD<bg::string, bg::TS<bg::string>>> input) { return input; };
        return bg::testing::test_graph(identity, {{{key, first_value}}, {{key, replacement_value}}});
    }();

    const auto key = std::string(128, 'k');
    const auto first_value = std::string(160, 'a');
    const auto replacement_value = std::string(192, 'b');
    CHECK(actual.equals({{{key, first_value}}, {{key, replacement_value}}}));
}

TEST_CASE("test_graph rejects duplicate and extra expected collection removals",
          "[testing][evaluation][feature:test-graph][series:tss][series:tsd][transition:removal]") {
    const auto set_identity = [](bg::wp<bg::TSS<bg::i64>> input) { return input; };
    const auto set = bg::testing::test_graph(set_identity, {{1}, {literal::REMOVED(1)}});
    const auto dictionary_identity =
        [](bg::wp<bg::TSD<bg::i64, bg::TS<bg::i64>>> input) { return input; };
    const auto dictionary = bg::testing::test_graph(dictionary_identity, {{{1, 10}}, {{1, literal::REMOVE}}});

    CHECK(set.equals({{1}, {literal::REMOVED(1)}}));
    CHECK_FALSE(set.equals({{1}, {literal::REMOVED(1), literal::REMOVED(1)}}));
    CHECK_FALSE(set.equals({{1}, {literal::REMOVED(1), literal::REMOVED(2)}}));
    CHECK(dictionary.equals({{{1, 10}}, {{1, literal::REMOVE}}}));
    CHECK_FALSE(dictionary.equals({{{1, 10}}, {{1, literal::REMOVE}, {1, literal::REMOVE}}}));
    CHECK_FALSE(dictionary.equals({{{1, 10}}, {{1, literal::REMOVE}, {2, literal::REMOVE}}}));
}

TEST_CASE("test_graph scalar result access rejects collection ticks",
          "[testing][evaluation][feature:test-graph][series:tsd]") {
    const auto identity = [](bg::wp<bg::TSD<bg::i64, bg::TS<bg::i64>>> input) { return input; };
    const auto actual = bg::testing::test_graph(identity, {{{1, 10}}});

    CHECK_THROWS_AS(actual[0], std::invalid_argument);
}

} // namespace
