#include <bg/graph.hpp>
#include <bg/nested.hpp>
#include <bg/node.hpp>
#include <bg/operators.hpp>
#include <bg/testing/test_graph.hpp>
#include <bg/time_series/tsd/tsd_type.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <optional>

TEST_CASE("dictionary getitem follows one scalar key without reacting to unrelated items",
          "[evaluation][series:tsd][series:value][feature:getitem][feature:operator]"
          "[binding:value-to-reference][binding:reference-to-value][transition:update]"
          "[transition:removal][transition:resurrection]") {
    using Dictionary = bg::TSD<std::int64_t, bg::TS<int>>;

    const auto make_dictionary = bg::node([](bg::In<bg::TS<int>> command, bg::Out<Dictionary> output) {
        if (*command < 0) {
            output.erase(1);
        } else if (*command >= 100) {
            output[2].set(*command);
        } else {
            output[1].set(*command);
        }
    });
    const auto read = bg::node(
        [](bg::In<bg::TS<int>> input, bg::Out<bg::TS<int>> output) { output.set(*input); });
    const auto graph = [&](bg::wp<bg::TS<int>> command) {
        return read(make_dictionary(command)[std::int64_t{1}]);
    };

    const auto actual = bg::testing::test_graph(graph, std::array{100, 101, 10, 11, -1, 12});

    CHECK(actual.equals({std::nullopt, std::nullopt, 10, 11, std::nullopt, 12}));
}

TEST_CASE("dictionary getitem changes its subscription with a time series key",
          "[evaluation][series:tsd][series:value][feature:getitem][feature:operator]"
          "[binding:value-to-reference][binding:reference-to-value][transition:update]") {
    using Dictionary = bg::TSD<std::int64_t, bg::TS<int>>;

    const auto make_dictionary = bg::node([](bg::In<bg::TS<int>> input, bg::Out<Dictionary> output) {
        output[1].set(*input);
        output[2].set(*input * 10);
    });
    const auto read = bg::node(
        [](bg::In<bg::TS<int>> input, bg::Out<bg::TS<int>> output) { output.set(*input); });
    const auto graph = [&](bg::wp<bg::TS<int>> input, bg::wp<bg::TS<std::int64_t>> key) {
        return read(make_dictionary(input)[key]);
    };

    const auto actual = bg::testing::test_graph(
        graph, std::array{10, 11, 12}, std::array<std::int64_t, 3>{1, 2, 1});

    CHECK(actual.equals({10, 110, 12}));
}

TEST_CASE("dictionary getitem releases its subscription while a time series key is invalid",
          "[evaluation][series:tsd][series:value][feature:getitem][feature:operator]"
          "[binding:value-to-reference][binding:reference-to-value]"
          "[transition:invalidation][transition:revalidation]") {
    using Dictionary = bg::TSD<std::int64_t, bg::TS<int>>;

    const auto make_dictionary = bg::node([](bg::In<bg::TS<int>> input, bg::Out<Dictionary> output) {
        output[1].set(*input);
        output[2].set(*input * 10);
    });
    const auto make_key = bg::node(
        [](bg::In<bg::TS<int>> input, bg::Out<bg::TS<std::int64_t>> output) {
            if (*input == 0) {
                output.invalidate();
            } else {
                output.set(*input);
            }
        });
    const auto read = bg::node(
        [](bg::In<bg::TS<int>> input, bg::Out<bg::TS<int>> output) { output.set(*input); });
    const auto graph = [&](bg::wp<bg::TS<int>> input, bg::wp<bg::TS<int>> key) {
        return read(make_dictionary(input)[make_key(key)]);
    };

    const auto actual =
        bg::testing::test_graph(graph, std::array{10, 11, 12, 13}, std::array{1, 0, 2, 1});

    CHECK(actual.equals({10, std::nullopt, 120, 13}));
}

TEST_CASE("dictionary getitem selects an item from a dictionary of references",
          "[evaluation][series:tsd][series:value][reference][feature:getitem][feature:operator]"
          "[binding:reference-to-reference][binding:reference-to-value]") {
    using ReferenceDictionary = bg::TSD<std::int64_t, bg::REF<bg::TS<int>>>;

    const auto increment = bg::node(
        [](bg::In<bg::TS<int>> input, bg::Out<bg::TS<int>> output) { output.set(*input + 1); });
    const auto scale = bg::node(
        [](bg::In<bg::TS<int>> input, bg::Out<bg::TS<int>> output) { output.set(*input * 10); });
    const auto make_dictionary = bg::node(
        [](bg::In<bg::REF<bg::TS<int>>> first, bg::In<bg::REF<bg::TS<int>>> second,
           bg::Out<ReferenceDictionary> output) {
            output[1].set(first);
            output[2].set(second);
        });
    const auto read = bg::node(
        [](bg::In<bg::TS<int>> input, bg::Out<bg::TS<int>> output) { output.set(*input); });
    const auto graph = [&](bg::wp<bg::TS<int>> input) {
        return read(make_dictionary(increment(input), scale(input))[std::int64_t{2}]);
    };

    const auto actual = bg::testing::test_graph(graph, std::array{1, 2, 3});

    CHECK(actual.equals({10, 20, 30}));
}

TEST_CASE("dictionary getitem preserves a collection item's runtime representation",
          "[evaluation][series:tsd][series:fixed-tsl][feature:getitem][feature:operator]"
          "[binding:value-to-reference][binding:reference-to-value]") {
    using IntList = bg::TSL<bg::TS<int>, 2>;
    using Dictionary = bg::TSD<std::int64_t, IntList>;

    const auto make_dictionary = bg::node([](bg::In<bg::TS<int>> input, bg::Out<Dictionary> output) {
        output[1][0].set(*input);
        output[1][1].set(*input * 10);
    });
    const auto sum = bg::node(
        [](bg::In<IntList> input, bg::Out<bg::TS<int>> output) { output.set(*input[0] + *input[1]); });
    const auto graph = [&](bg::wp<bg::TS<int>> input) {
        return sum(make_dictionary(input)[std::int64_t{1}]);
    };

    const auto actual = bg::testing::test_graph(graph, std::array{1, 2, 3});

    CHECK(actual.equals({11, 22, 33}));
}

TEST_CASE("dictionary getitem releases and restores its subscription across a nested graph lifecycle",
          "[evaluation][nested][series:tsd][series:value][reference][feature:getitem][feature:operator]"
          "[feature:nested-graph][binding:reference-to-reference][binding:reference-to-value]"
          "[transition:stop][transition:restart]") {
    using Dictionary = bg::TSD<std::int64_t, bg::TS<int>>;
    using namespace bg::literals;

    const auto make_dictionary = bg::node([](bg::In<bg::TS<int>> input, bg::Out<Dictionary> output) {
        output[1].set(*input);
    });
    const auto read = bg::node(
        [](bg::In<bg::TS<int>> input, bg::Out<bg::TS<int>> output) { output.set(*input); });
    const auto inner = bg::graph(
        "name"_n = "dictionary",
        [&](bg::wp<bg::TS<int>> input) -> bg::wp<Dictionary> { return make_dictionary(input); });
    const auto outer = [&](bg::wp<bg::TS<int>> input, bg::wp<bg::TS<bool>> run) {
        return read(bg::nested(inner, run, input)[std::int64_t{1}]);
    };

    const auto actual = bg::testing::test_graph(
        outer, std::array{1, 2, 3, 4, 5}, std::array{true, true, false, true, true});

    CHECK(actual.equals({1, 2, std::nullopt, 4, 5}));
}
