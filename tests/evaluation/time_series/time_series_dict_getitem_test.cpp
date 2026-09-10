#include <bg/graph.hpp>
#include <bg/nested.hpp>
#include <bg/node.hpp>
#include <bg/operators.hpp>
#include <bg/testing/test_graph.hpp>
#include <bg/time_series/tsd/tsd_type.hpp>
#include <bg/type/time_series_reference_type.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <optional>

TEST_CASE("dictionary getitem follows one scalar key without reacting to unrelated items",
          "[evaluation][series:tsd][series:value][feature:getitem][feature:operator]"
          "[binding:value-to-reference][binding:reference-to-value][transition:update]"
          "[transition:removal][transition:resurrection]") {
    const auto make_dictionary =
        bg::node([](bg::In<bg::TS<int>> command, bg::Out<bg::TSD<std::int64_t, bg::TS<int>>> output) {
            if (*command < 0) {
                output.erase(1);
            } else if (*command >= 100) {
                output[2].set(*command);
            } else {
                output[1].set(*command);
            }
        });
    const auto read = bg::node([](bg::In<bg::TS<int>> input, bg::Out<bg::TS<int>> output) { output.set(*input); });
    const auto graph = [&](bg::wp<bg::TS<int>> command) { return read(make_dictionary(command)[std::int64_t{1}]); };

    const auto actual = bg::testing::test_graph(graph, std::array{100, 101, 10, 11, -1, 12});

    CHECK(actual.equals({std::nullopt, std::nullopt, 10, 11, std::nullopt, 12}));
}

TEST_CASE("dictionary getitem changes its subscription with a time series key",
          "[evaluation][series:tsd][series:value][feature:getitem][feature:operator]"
          "[binding:value-to-reference][binding:reference-to-value][transition:update]") {
    const auto make_dictionary =
        bg::node([](bg::In<bg::TS<int>> input, bg::Out<bg::TSD<std::int64_t, bg::TS<int>>> output) {
            output[1].set(*input);
            output[2].set(*input * 10);
        });
    const auto read = bg::node([](bg::In<bg::TS<int>> input, bg::Out<bg::TS<int>> output) { output.set(*input); });
    const auto graph = [&](bg::wp<bg::TS<int>> input, bg::wp<bg::TS<std::int64_t>> key) {
        return read(make_dictionary(input)[key]);
    };

    const auto actual = bg::testing::test_graph(graph, std::array{10, 11, 12}, std::array<std::int64_t, 3>{1, 2, 1});

    CHECK(actual.equals({10, 110, 12}));
}

TEST_CASE("dictionary getitem releases its subscription while a time series key is invalid",
          "[evaluation][series:tsd][series:value][feature:getitem][feature:operator]"
          "[binding:value-to-reference][binding:reference-to-value]"
          "[transition:invalidation][transition:revalidation]") {
    const auto make_dictionary =
        bg::node([](bg::In<bg::TS<int>> input, bg::Out<bg::TSD<std::int64_t, bg::TS<int>>> output) {
            output[1].set(*input);
            output[2].set(*input * 10);
        });
    const auto make_key = bg::node([](bg::In<bg::TS<int>> input, bg::Out<bg::TS<std::int64_t>> output) {
        if (*input == 0) {
            output.invalidate();
        } else {
            output.set(*input);
        }
    });
    const auto read = bg::node([](bg::In<bg::TS<int>> input, bg::Out<bg::TS<int>> output) { output.set(*input); });
    const auto graph = [&](bg::wp<bg::TS<int>> input, bg::wp<bg::TS<int>> key) {
        return read(make_dictionary(input)[make_key(key)]);
    };

    const auto actual = bg::testing::test_graph(graph, std::array{10, 11, 12, 13}, std::array{1, 0, 2, 1});

    CHECK(actual.equals({10, std::nullopt, 120, 13}));
}

TEST_CASE("dictionary getitem selects an item from a dictionary of references",
          "[evaluation][series:tsd][series:value][reference][feature:getitem][feature:operator]"
          "[binding:reference-to-reference][binding:reference-to-value]") {
    const auto increment =
        bg::node([](bg::In<bg::TS<int>> input, bg::Out<bg::TS<int>> output) { output.set(*input + 1); });
    const auto scale =
        bg::node([](bg::In<bg::TS<int>> input, bg::Out<bg::TS<int>> output) { output.set(*input * 10); });
    const auto make_dictionary = bg::node([](bg::In<bg::REF<bg::TS<int>>> first, bg::In<bg::REF<bg::TS<int>>> second,
                                             bg::Out<bg::TSD<std::int64_t, bg::REF<bg::TS<int>>>> output) {
        output[1].set(first);
        output[2].set(second);
    });
    const auto read = bg::node([](bg::In<bg::TS<int>> input, bg::Out<bg::TS<int>> output) { output.set(*input); });
    const auto graph = [&](bg::wp<bg::TS<int>> input) {
        return read(make_dictionary(increment(input), scale(input))[std::int64_t{2}]);
    };

    const auto actual = bg::testing::test_graph(graph, std::array{1, 2, 3});

    CHECK(actual.equals({10, 20, 30}));
}

TEST_CASE("a value dictionary provides final item references",
          "[evaluation][series:tsd][series:value][reference][feature:reference][feature:sampling]"
          "[binding:value-to-reference][guarantee:final]") {
    const auto make_dictionary =
        bg::node([](bg::In<bg::TS<int>> input, bg::Out<bg::TSD<std::int64_t, bg::TS<int>>> output) {
            output[1].set(*input);
    });
    const auto inspect = bg::node([](bg::In<bg::TSD<std::int64_t, bg::REF<bg::TS<int>>>> input,
                                     bg::Out<bg::TS<bool>> output) {
        const auto item = input[1];
        const auto &type = static_cast<const bg::TimeSeriesReferenceType &>(item.runtime_type().type());
        output.set(type.guarantee().value() == bg::TimeSeriesGuarantee::final);
    });
    const auto graph = [&](bg::wp<bg::TS<int>> input) { return inspect(make_dictionary(input)); };

    const auto actual = bg::testing::test_graph(graph, std::array{1});

    CHECK(actual.equals({true}));
}

TEST_CASE("a dereferenced dictionary follows a target that is initially invalid",
          "[evaluation][series:tsd][series:value][reference][feature:reference][feature:sampling]"
          "[binding:value-to-reference][binding:reference-to-value]"
          "[transition:revalidation]") {
    const auto make_target = bg::node([](bg::In<bg::TS<int>> command, bg::Out<bg::TS<int>> output) {
        if (*command == 0) {
            output.invalidate();
        } else {
            output.set(*command);
        }
    });
    const auto make_dictionary = bg::node(
        [](bg::In<bg::REF<bg::TS<int>>> target, bg::Out<bg::TSD<std::int64_t, bg::REF<bg::TS<int>>>> output) {
            output[1].set(target);
        });
    const auto read = bg::node([](bg::In<bg::TSD<std::int64_t, bg::TS<int>>> input,
                                  bg::Out<bg::TS<int>> output) {
        if (input[1].valid()) {
            output.set(*input[1]);
        } else {
            output.invalidate();
        }
    });
    const auto graph = [&](bg::wp<bg::TS<int>> command) {
        return read(make_dictionary(make_target(command)));
    };

    const auto actual = bg::testing::test_graph(graph, std::array{0, 10});

    CHECK(actual.equals({std::nullopt, 10}));
}

TEST_CASE("a dereferenced dictionary follows target invalidation and revalidation",
          "[evaluation][series:tsd][series:value][reference][feature:reference]"
          "[binding:value-to-reference][binding:reference-to-value]"
          "[transition:invalidation][transition:revalidation]") {
    const auto make_target = bg::node([](bg::In<bg::TS<int>> command, bg::Out<bg::TS<int>> output) {
        if (*command == 0) {
            output.invalidate();
        } else {
            output.set(*command);
        }
    });
    const auto make_dictionary = bg::node(
        [](bg::In<bg::REF<bg::TS<int>>> target, bg::Out<bg::TSD<std::int64_t, bg::REF<bg::TS<int>>>> output) {
            output[1].set(target);
        });
    const auto read = bg::node([](bg::In<bg::TSD<std::int64_t, bg::TS<int>>> input,
                                  bg::Out<bg::TS<int>> output) {
        if (input[1].valid()) {
            output.set(*input[1]);
        } else {
            output.invalidate();
        }
    });
    const auto graph = [&](bg::wp<bg::TS<int>> command) {
        return read(make_dictionary(make_target(command)));
    };

    const auto actual = bg::testing::test_graph(graph, std::array{10, 0, 20});

    CHECK(actual.equals({10, std::nullopt, 20}));
}

TEST_CASE("a dictionary reference representation crosses into a nested graph",
          "[evaluation][nested][series:tsd][series:value][reference][feature:reference]"
          "[feature:nested-graph][boundary][binding:value-to-reference][binding:reference-to-value]"
          "[transition:update]") {
    using namespace bg::literals;

    const auto make_dictionary =
        bg::node([](bg::In<bg::TS<int>> input, bg::Out<bg::TSD<std::int64_t, bg::TS<int>>> output) {
            output[1].set(*input);
        });
    const auto read = bg::node([](bg::In<bg::TSD<std::int64_t, bg::TS<int>>> input,
                                  bg::Out<bg::TS<int>> output) { output.set(*input[1]); });
    const auto inner =
        bg::graph("name"_n = "consume_dictionary_references",
                  [&](bg::wp<bg::TSD<std::int64_t, bg::REF<bg::TS<int>>>> input) -> bg::wp<bg::TS<int>> {
                      return read(input);
                  });
    const auto outer = [&](bg::wp<bg::TS<int>> input, bg::wp<bg::TS<bool>> run) {
        return bg::nested(inner, run, make_dictionary(input));
    };

    const auto actual =
        bg::testing::test_graph(outer, std::array{10, 20, 30}, std::array{true, false, true});

    CHECK(actual.equals({10, std::nullopt, 30}));
}

TEST_CASE("an inner dictionary reference representation crosses into its outer graph",
          "[evaluation][nested][series:tsd][series:value][reference][feature:reference]"
          "[feature:nested-graph][boundary][binding:value-to-reference][binding:reference-to-reference]"
          "[binding:reference-to-value][transition:update]") {
    using namespace bg::literals;

    const auto make_dictionary =
        bg::node([](bg::In<bg::TS<int>> input, bg::Out<bg::TSD<std::int64_t, bg::TS<int>>> output) {
            output[1].set(*input);
        });
    const auto select = bg::node([](bg::In<bg::TSD<std::int64_t, bg::REF<bg::TS<int>>>> input,
                                    bg::Out<bg::REF<bg::TS<int>>> output) { output.set(input[1]); });
    const auto read =
        bg::node([](bg::In<bg::TS<int>> input, bg::Out<bg::TS<int>> output) { output.set(*input); });
    const auto inner =
        bg::graph("name"_n = "produce_dictionary",
                  [&](bg::wp<bg::TS<int>> input) -> bg::wp<bg::TSD<std::int64_t, bg::TS<int>>> {
                      return make_dictionary(input);
                  });
    const auto outer = [&](bg::wp<bg::TS<int>> input, bg::wp<bg::TS<bool>> run) {
        return read(select(bg::nested(inner, run, input)));
    };

    const auto actual =
        bg::testing::test_graph(outer, std::array{10, 20, 30}, std::array{true, false, true});

    CHECK(actual.equals({10, std::nullopt, 30}));
}

TEST_CASE("a dictionary reference representation connects two nested graphs",
          "[evaluation][nested][series:tsd][series:value][reference][feature:reference]"
          "[feature:nested-graph][boundary][shape:nested][binding:value-to-reference]"
          "[binding:reference-to-reference][binding:reference-to-value][transition:update]") {
    using namespace bg::literals;

    const auto make_dictionary =
        bg::node([](bg::In<bg::TS<int>> input, bg::Out<bg::TSD<std::int64_t, bg::TS<int>>> output) {
            output[1].set(*input);
        });
    const auto select = bg::node([](bg::In<bg::TSD<std::int64_t, bg::REF<bg::TS<int>>>> input,
                                    bg::Out<bg::REF<bg::TS<int>>> output) { output.set(input[1]); });
    const auto read =
        bg::node([](bg::In<bg::TS<int>> input, bg::Out<bg::TS<int>> output) { output.set(*input); });
    const auto producer =
        bg::graph("name"_n = "produce_dictionary",
                  [&](bg::wp<bg::TS<int>> input) -> bg::wp<bg::TSD<std::int64_t, bg::TS<int>>> {
                      return make_dictionary(input);
                  });
    const auto consumer =
        bg::graph("name"_n = "consume_dictionary_references",
                  [&](bg::wp<bg::TSD<std::int64_t, bg::REF<bg::TS<int>>>> input) -> bg::wp<bg::TS<int>> {
                      return read(select(input));
                  });
    const auto outer = [&](bg::wp<bg::TS<int>> input, bg::wp<bg::TS<bool>> run) {
        const auto dictionary = bg::nested(producer, run, input);
        return bg::nested(consumer, run, dictionary);
    };

    const auto actual =
        bg::testing::test_graph(outer, std::array{10, 20, 30}, std::array{true, false, true});

    CHECK(actual.equals({10, std::nullopt, 30}));
}

TEST_CASE("dictionary getitem ignores changes to another contained dictionary",
          "[evaluation][series:tsd][series:value][reference][feature:getitem][feature:operator]"
          "[binding:value-to-reference][binding:reference-to-value][shape:nested]"
          "[transition:update]") {
    const auto make_dictionary = bg::node(
        [](bg::In<bg::TS<int>> command,
           bg::Out<bg::TSD<std::int64_t, bg::TSD<std::int64_t, bg::TS<int>>>> output) {
            if (*command >= 100) {
                output[2][2].set(*command);
            } else {
                output[1][2].set(*command);
            }
        });
    const auto read = bg::node(
        [](bg::In<bg::TS<int>> input, bg::Out<bg::TS<int>> output) { output.set(*input); });
    const auto graph = [&](bg::wp<bg::TS<int>> command) {
        return read(make_dictionary(command)[std::int64_t{1}][std::int64_t{2}]);
    };

    const auto actual = bg::testing::test_graph(graph, std::array{10, 100, 11});

    CHECK(actual.equals({10, std::nullopt, 11}));
}

TEST_CASE("dictionary getitem follows removal and restoration of a contained dictionary",
          "[evaluation][series:tsd][series:value][reference][feature:getitem][feature:operator]"
          "[binding:value-to-reference][binding:reference-to-value][shape:nested]"
          "[transition:removal][transition:resurrection]") {
    const auto make_dictionary = bg::node(
        [](bg::In<bg::TS<int>> command,
           bg::Out<bg::TSD<std::int64_t, bg::TSD<std::int64_t, bg::TS<int>>>> output) {
            if (*command < 0) {
                output.erase(1);
            } else {
                output[1][2].set(*command);
            }
        });
    const auto read = bg::node(
        [](bg::In<bg::TS<int>> input, bg::Out<bg::TS<int>> output) { output.set(*input); });
    const auto graph = [&](bg::wp<bg::TS<int>> command) {
        return read(make_dictionary(command)[std::int64_t{1}][std::int64_t{2}]);
    };

    const auto actual = bg::testing::test_graph(graph, std::array{10, -1, 20});

    CHECK(actual.equals({10, std::nullopt, 20}));
}

TEST_CASE("a contained dictionary reference representation follows removal and restoration",
          "[evaluation][series:tsd][series:value][reference][feature:getitem][feature:operator]"
          "[binding:value-to-reference][binding:reference-to-reference][binding:reference-to-value][shape:nested]"
          "[transition:removal][transition:resurrection]") {
    const auto make_dictionary = bg::node(
        [](bg::In<bg::TS<int>> command,
           bg::Out<bg::TSD<std::int64_t, bg::TSD<std::int64_t, bg::TS<int>>>> output) {
            if (*command < 0) {
                output.erase(1);
            } else {
                output[1][2].set(*command);
            }
        });
    const auto select = bg::node([](bg::In<bg::TSD<std::int64_t, bg::REF<bg::TS<int>>>> input,
                                    bg::Out<bg::REF<bg::TS<int>>> output) { output.set(input[2]); });
    const auto read =
        bg::node([](bg::In<bg::TS<int>> input, bg::Out<bg::TS<int>> output) { output.set(*input); });
    const auto graph = [&](bg::wp<bg::TS<int>> command) {
        return read(select(make_dictionary(command)[std::int64_t{1}]));
    };

    const auto actual = bg::testing::test_graph(graph, std::array{10, -1, 20});

    CHECK(actual.equals({10, std::nullopt, 20}));
}

TEST_CASE("dictionary getitem preserves a collection item's runtime representation",
          "[evaluation][series:tsd][series:fixed-tsl][feature:getitem][feature:operator]"
          "[binding:value-to-reference][binding:reference-to-value]") {
    const auto make_dictionary =
        bg::node([](bg::In<bg::TS<int>> input, bg::Out<bg::TSD<std::int64_t, bg::TSL<bg::TS<int>, 2>>> output) {
            output[1][0].set(*input);
            output[1][1].set(*input * 10);
        });
    const auto sum = bg::node(
        [](bg::In<bg::TSL<bg::TS<int>, 2>> input, bg::Out<bg::TS<int>> output) { output.set(*input[0] + *input[1]); });
    const auto graph = [&](bg::wp<bg::TS<int>> input) { return sum(make_dictionary(input)[std::int64_t{1}]); };

    const auto actual = bg::testing::test_graph(graph, std::array{1, 2, 3});

    CHECK(actual.equals({11, 22, 33}));
}

TEST_CASE("dereferencing a dictionary item samples a new dictionary target",
          "[evaluation][series:tsd][series:value][reference][feature:reference]"
          "[binding:reference-to-value][shape:nested][transition:update]") {
    const auto make_first = bg::node(
        [](bg::In<bg::TS<int>>, bg::Out<bg::TSD<std::int64_t, bg::TS<int>>> output) {
            output[1].set(10);
            output[2].set(20);
        });
    const auto make_second = bg::node(
        [](bg::In<bg::TS<int>>, bg::Out<bg::TSD<std::int64_t, bg::TS<int>>> output) {
            output[2].set(200);
            output[3].set(300);
        });
    const auto select = bg::node(
        [](bg::In<bg::TS<int>> selection,
           bg::In<bg::REF<bg::TSD<std::int64_t, bg::TS<int>>>> first,
           bg::In<bg::REF<bg::TSD<std::int64_t, bg::TS<int>>>> second,
           bg::Out<bg::TSD<std::int64_t, bg::REF<bg::TSD<std::int64_t, bg::TS<int>>>>> output) {
            if (*selection == 0) {
                output[0].set(first);
            } else if (*selection == 1) {
                output[0].set(second);
            } else {
                output[0].invalidate();
            }
        });
    const auto inspect = bg::node(
        [](bg::In<bg::TSD<std::int64_t, bg::TSD<std::int64_t, bg::TS<int>>>> input,
           bg::Out<bg::TS<int>> output) {
            const auto item = input[0];
            auto added = 0;
            auto removed = 0;
            auto modified = 0;
            item.for_each_added([&](const auto &key) { added |= 1 << *key; });
            item.for_each_removed([&](const auto &key) { removed |= 1 << *key; });
            item.for_each_modified([&](const auto &key) { modified |= 1 << *key; });
            output.set(added * 100 + removed * 10 + modified);
        });
    const auto graph = [&](bg::wp<bg::TS<int>> selection) {
        const auto first = make_first(selection);
        const auto second = make_second(selection);
        return inspect(select(selection, first, second));
    };

    const auto actual = bg::testing::test_graph(graph, std::array{0, 1, 2});

    CHECK(actual.equals({600, 824, 120}));
}

TEST_CASE("dictionary getitem follows a sampled dictionary item",
          "[evaluation][series:tsd][series:value][reference][feature:reference][feature:sampling]"
          "[feature:getitem][binding:reference-to-reference][binding:reference-to-value]"
          "[shape:nested][transition:update]") {
    const auto make_first = bg::node(
        [](bg::In<bg::TS<bool>>, bg::Out<bg::TSD<std::int64_t, bg::TS<int>>> output) { output[2].set(20); });
    const auto make_second = bg::node(
        [](bg::In<bg::TS<bool>>, bg::Out<bg::TSD<std::int64_t, bg::TS<int>>> output) { output[2].set(200); });
    const auto select = bg::node(
        [](bg::In<bg::TS<bool>> use_second,
           bg::In<bg::REF<bg::TSD<std::int64_t, bg::TS<int>>>> first,
           bg::In<bg::REF<bg::TSD<std::int64_t, bg::TS<int>>>> second,
           bg::Out<bg::TSD<std::int64_t, bg::REF<bg::TSD<std::int64_t, bg::TS<int>>>>> output) {
            output[0].set(*use_second ? second : first);
        });
    const auto read = bg::node(
        [](bg::In<bg::TS<int>> input, bg::Out<bg::TS<int>> output) { output.set(*input); });
    const auto graph = [&](bg::wp<bg::TS<bool>> use_second) {
        return read(select(use_second, make_first(use_second), make_second(use_second))[std::int64_t{0}]
                                                                                        [std::int64_t{2}]);
    };

    const auto actual = bg::testing::test_graph(graph, std::array{false, true, true});

    CHECK(actual.equals({20, 200, 200}));
}

TEST_CASE("all consumers see the same dictionary target sample",
          "[evaluation][series:tsd][series:value][reference][feature:reference]"
          "[binding:reference-to-value][shape:nested][transition:update]") {
    const auto make_first = bg::node(
        [](bg::In<bg::TS<bool>>, bg::Out<bg::TSD<std::int64_t, bg::TS<int>>> output) {
            output[1].set(10);
        });
    const auto make_second = bg::node(
        [](bg::In<bg::TS<bool>>, bg::Out<bg::TSD<std::int64_t, bg::TS<int>>> output) {
            output[2].set(20);
        });
    const auto select = bg::node(
        [](bg::In<bg::TS<bool>> use_second,
           bg::In<bg::REF<bg::TSD<std::int64_t, bg::TS<int>>>> first,
           bg::In<bg::REF<bg::TSD<std::int64_t, bg::TS<int>>>> second,
           bg::Out<bg::TSD<std::int64_t, bg::REF<bg::TSD<std::int64_t, bg::TS<int>>>>> output) {
            output[0].set(*use_second ? second : first);
        });
    const auto inspect = bg::node(
        [](bg::In<bg::TSD<std::int64_t, bg::TSD<std::int64_t, bg::TS<int>>>> input,
           bg::Out<bg::TS<int>> output) {
            auto result = 0;
            input[0].for_each_added([&](const auto &key) { result += static_cast<int>(*key); });
            input[0].for_each_removed([&](const auto &key) { result += static_cast<int>(*key) * 10; });
            output.set(result);
        });
    const auto sum = bg::node([](bg::In<bg::TS<int>> left, bg::In<bg::TS<int>> right,
                                 bg::Out<bg::TS<int>> output) { output.set(*left + *right); });
    const auto graph = [&](bg::wp<bg::TS<bool>> use_second) {
        const auto dictionary = select(use_second, make_first(use_second), make_second(use_second));
        return sum(inspect(dictionary), inspect(dictionary));
    };

    const auto actual = bg::testing::test_graph(graph, std::array{false, true});

    CHECK(actual.equals({2, 24}));
}

TEST_CASE("a passive consumer does not replay an old dictionary target sample",
          "[evaluation][series:tsd][series:value][reference][feature:reference][feature:sampling]"
          "[binding:reference-to-value][shape:nested][activity:passive][transition:update]") {
    const auto make_first = bg::node(
        [](bg::In<bg::TS<int>>, bg::Out<bg::TSD<std::int64_t, bg::TS<int>>> output) { output[1].set(10); });
    const auto make_second = bg::node(
        [](bg::In<bg::TS<int>>, bg::Out<bg::TSD<std::int64_t, bg::TS<int>>> output) { output[2].set(20); });
    const auto select = bg::node(
        [](bg::In<bg::TS<int>> selection,
           bg::In<bg::REF<bg::TSD<std::int64_t, bg::TS<int>>>> first,
           bg::In<bg::REF<bg::TSD<std::int64_t, bg::TS<int>>>> second,
           bg::Out<bg::TSD<std::int64_t, bg::REF<bg::TSD<std::int64_t, bg::TS<int>>>>> output) {
            output[0].set(*selection == 0 ? first : second);
        });
    const auto inspect = bg::node(
        [](bg::In<bg::TSD<std::int64_t, bg::TSD<std::int64_t, bg::TS<int>>>, bg::passive> input,
           bg::In<bg::TS<int>> trigger, bg::Out<bg::TS<int>> output) {
            auto result = 0;
            input[0].for_each_added([&](const auto &key) { result += static_cast<int>(*key); });
            input[0].for_each_removed([&](const auto &key) { result += static_cast<int>(*key) * 10; });
            output.set(result + *trigger * 0);
        });
    const auto graph = [&](bg::wp<bg::TS<int>> selection, bg::wp<bg::TS<int>> trigger) {
        const auto dictionary = select(selection, make_first(selection), make_second(selection));
        return inspect(dictionary, trigger);
    };

    const auto actual = bg::testing::test_graph(
        graph, std::array<std::optional<int>, 3>{0, 1, std::nullopt},
        std::array<std::optional<int>, 3>{1, std::nullopt, 2});

    CHECK(actual.equals({1, std::nullopt, 0}));
}

TEST_CASE("a contained dictionary target sample recurses into modified items",
          "[evaluation][series:tsd][series:value][reference][feature:reference][feature:sampling]"
          "[binding:reference-to-value][shape:nested][transition:update]") {
    const auto make_first = bg::node(
        [](bg::In<bg::TS<bool>>,
           bg::Out<bg::TSD<std::int64_t, bg::TSD<std::int64_t, bg::TS<int>>>> output) {
            output[2][10].set(10);
            output[2][20].set(20);
        });
    const auto make_second = bg::node(
        [](bg::In<bg::TS<bool>>,
           bg::Out<bg::TSD<std::int64_t, bg::TSD<std::int64_t, bg::TS<int>>>> output) {
            output[2][20].set(200);
            output[2][30].set(300);
        });
    const auto select = bg::node(
        [](bg::In<bg::TS<bool>> use_second,
           bg::In<bg::REF<bg::TSD<std::int64_t, bg::TSD<std::int64_t, bg::TS<int>>>>> first,
           bg::In<bg::REF<bg::TSD<std::int64_t, bg::TSD<std::int64_t, bg::TS<int>>>>> second,
           bg::Out<bg::TSD<std::int64_t,
                           bg::REF<bg::TSD<std::int64_t, bg::TSD<std::int64_t, bg::TS<int>>>>>> output) {
            output[0].set(*use_second ? second : first);
        });
    const auto inspect = bg::node(
        [](bg::In<bg::TSD<std::int64_t,
                           bg::TSD<std::int64_t, bg::TSD<std::int64_t, bg::TS<int>>>>> input,
           bg::Out<bg::TS<int>> output) {
            const auto nested = input[0][2];
            auto added = 0;
            auto removed = 0;
            auto modified = 0;
            nested.for_each_added([&](const auto &key) { added += static_cast<int>(*key); });
            nested.for_each_removed([&](const auto &key) { removed += static_cast<int>(*key); });
            nested.for_each_modified([&](const auto &key) { modified += static_cast<int>(*key); });
            output.set(added * 10'000 + removed * 100 + modified);
        });
    const auto graph = [&](bg::wp<bg::TS<bool>> use_second) {
        return inspect(select(use_second, make_first(use_second), make_second(use_second)));
    };

    const auto actual = bg::testing::test_graph(graph, std::array{false, true});

    CHECK(actual.equals({300'000, 301'020}));
}

TEST_CASE("a dictionary item samples nested graph stop and restart",
          "[evaluation][nested][series:tsd][series:value][reference][feature:reference][feature:sampling]"
          "[feature:nested-graph][binding:reference-to-value][shape:nested]"
          "[transition:stop][transition:restart]") {
    using namespace bg::literals;

    const auto make_dictionary = bg::node(
        [](bg::In<bg::TS<int>> input, bg::Out<bg::TSD<std::int64_t, bg::TS<int>>> output) {
            output[1].set(*input);
            output[2].set(*input * 10);
        });
    const auto producer =
        bg::graph("name"_n = "sampled_dictionary_target",
                  [&](bg::wp<bg::TS<int>> input) -> bg::wp<bg::TSD<std::int64_t, bg::TS<int>>> {
                      return make_dictionary(input);
                  });
    const auto wrap = bg::node(
        [](bg::In<bg::REF<bg::TSD<std::int64_t, bg::TS<int>>>, bg::optional> target,
           bg::Out<bg::TSD<std::int64_t, bg::REF<bg::TSD<std::int64_t, bg::TS<int>>>>> output) {
            if (target.valid()) {
                output[0].set(target);
            } else {
                output[0].invalidate();
            }
        });
    const auto inspect = bg::node(
        [](bg::In<bg::TSD<std::int64_t, bg::TSD<std::int64_t, bg::TS<int>>>> input,
           bg::Out<bg::TS<int>> output) {
            const auto item = input[0];
            auto added = 0;
            auto removed = 0;
            item.for_each_added([&](const auto &key) { added |= 1 << *key; });
            item.for_each_removed([&](const auto &key) { removed |= 1 << *key; });
            output.set(added * 100 + removed * 10);
        });
    const auto graph = [&](bg::wp<bg::TS<int>> input, bg::wp<bg::TS<bool>> run) {
        return inspect(wrap(bg::nested(producer, run, input)));
    };

    const auto actual =
        bg::testing::test_graph(graph, std::array{1, 2, 3}, std::array{true, false, true});

    CHECK(actual.equals({600, 60, 600}));
}

TEST_CASE("dictionary getitem releases and restores its subscription across a nested graph lifecycle",
          "[evaluation][nested][series:tsd][series:value][reference][feature:getitem][feature:operator]"
          "[feature:nested-graph][binding:reference-to-reference][binding:reference-to-value]"
          "[transition:stop][transition:restart]") {
    using namespace bg::literals;

    const auto make_dictionary = bg::node(
        [](bg::In<bg::TS<int>> input, bg::Out<bg::TSD<std::int64_t, bg::TS<int>>> output) { output[1].set(*input); });
    const auto read = bg::node([](bg::In<bg::TS<int>> input, bg::Out<bg::TS<int>> output) { output.set(*input); });
    const auto inner = bg::graph("name"_n = "dictionary",
                                 [&](bg::wp<bg::TS<int>> input) -> bg::wp<bg::TSD<std::int64_t, bg::TS<int>>> {
                                     return make_dictionary(input);
                                 });
    const auto outer = [&](bg::wp<bg::TS<int>> input, bg::wp<bg::TS<bool>> run) {
        return read(bg::nested(inner, run, input)[std::int64_t{1}]);
    };

    const auto actual =
        bg::testing::test_graph(outer, std::array{1, 2, 3, 4, 5}, std::array{true, true, false, true, true});

    CHECK(actual.equals({1, 2, std::nullopt, 4, 5}));
}
