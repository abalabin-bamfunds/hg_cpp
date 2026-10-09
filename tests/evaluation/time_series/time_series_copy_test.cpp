#include <bg/node.hpp>
#include <bg/operators.hpp>
#include <bg/scalar/details/string_storage.hpp>
#include <bg/scalar/details/value_access.hpp>
#include <bg/scalar/value.hpp>
#include <bg/testing/test_graph.hpp>
#include <bg/time_series/cpp_runtime_type.hpp>
#include <bg/time_series/details/input_data.hpp>
#include <bg/time_series/details/output_state.hpp>
#include <bg/time_series/output_binding.hpp>
#include <bg/time_series/tsb/tsb.hpp>
#include <bg/time_series/tsl/tsl.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <optional>
#include <stdexcept>
#include <string_view>

using namespace bg;

namespace {

template <template <typename, auto...> typename Field> struct copy_bundle {
    Field<TS<i64>> first;
    Field<TS<i64>> second;
};

template <template <typename, auto...> typename Field>
using COPY_SOURCE = BundleSchemaVariableDeclaration<"COPY_SOURCE", Field>;

static_assert(requires(const Out<TSB<SCHEMA>> &output, const In<TSB<COPY_SOURCE>> &input) {
    output.copy(input);
    output.copy_tick(input);
});

} // namespace

TEST_CASE("copy copies the latest value when the node runs",
          "[feature:node-api][feature:test-graph][series:value][transition:update]") {
    const auto copy = node([](In<TS<i64>, passive> input, In<TS<i64>> trigger, Out<TS<i64>> output) {
        static_cast<void>(trigger);
        output.copy(input);
    });
    const auto graph = [&](wp<TS<i64>> input, wp<TS<i64>> trigger) {
        return copy(input, trigger);
    };

    const auto input = std::array<std::optional<i64>, 3>{1, std::nullopt, 2};
    const auto actual = testing::test_graph(graph, input, std::array{0, 1, 2});

    CHECK(actual.equals({1, 1, 2}));
}

TEST_CASE("a generic output copies and copies a tick from a time series value",
          "[feature:node-api][feature:test-graph][series:generic][series:value][transition:update]") {
    const auto copy = node([](In<TS_T> input, Out<TS_T> output) { output.copy(input); });
    const auto copy_tick = node([](In<TS_T> input, Out<TS_T> output) { output.copy_tick(input); });
    const auto copy_graph = [&](wp<TS<i64>> input) { return copy(input); };
    const auto copy_tick_graph = [&](wp<TS<i64>> input) { return copy_tick(input); };

    const auto copied = testing::test_graph(copy_graph, std::array{3, 5});
    const auto copied_tick = testing::test_graph(copy_tick_graph, std::array{3, 5});

    CHECK(copied.equals<i64>({3, 5}));
    CHECK(copied_tick.equals<i64>({3, 5}));
}

TEST_CASE("copy_tick applies a current invalidation but ignores its stale state",
          "[feature:node-api][feature:test-graph][series:value]"
          "[transition:update][transition:invalidation][transition:revalidation]") {
    const auto source = node([](In<TS<i64>> input, Out<TS<i64>> output) {
        if (*input == 0) {
            output.invalidate();
            return;
        }
        output.set(*input);
    });
    const auto copy_tick = node([](In<TS<i64>, passive, optional> input, In<TS<i64>> trigger,
                                   Out<TS<i64>> output) {
        if (*trigger == 2) {
            output.set(99);
        }
        output.copy_tick(input);
    });
    const auto graph = [&](wp<TS<i64>> input, wp<TS<i64>> trigger) {
        return copy_tick(source(input), trigger);
    };

    const auto input = std::array<std::optional<i64>, 4>{1, 0, std::nullopt, 2};
    const auto actual = testing::test_graph(graph, input, std::array{0, 1, 2, 3});

    CHECK(actual.equals({1, std::nullopt, 99, 2}));
}

TEST_CASE("copy propagates value invalidation and revalidation",
          "[feature:node-api][feature:test-graph][series:value]"
          "[transition:invalidation][transition:revalidation]") {
    const auto source = node([](In<TS<i64>> input, Out<TS<i64>> output) {
        if (*input == 0) {
            output.invalidate();
            return;
        }
        output.set(*input);
    });
    const auto copy = node([](In<TS<i64>, optional> input, Out<TS<i64>> output) { output.copy(input); });
    const auto graph = [&](wp<TS<i64>> input) { return copy(source(input)); };

    const auto actual = testing::test_graph(graph, std::array{1, 0, 2});

    CHECK(actual.equals({1, std::nullopt, 2}));
}

TEST_CASE("copy of a string time series retains its storage",
          "[feature:node-api][series:value][scalar:string][transition:update]") {
    const auto time = MIN_DT + std::chrono::microseconds{1};
    const auto &runtime_type = cpp_time_series_runtime_type<TS<string>>();
    auto output_last_updated = TimeSeriesState{};
    auto *output_value = static_cast<details::StringStorage *>(nullptr);
    auto output_state = OutputState{runtime_type};
    output_state.bind(output_last_updated, &output_value, time);
    auto output = Out<TS<string>>{TimeSeriesOutputBinding{output_state}};
    auto *source_storage = static_cast<details::StringStorage *>(nullptr);

    {
        auto source = ScalarValue<string>{"shared string"};
        auto source_view = source.view();
        source_storage = details::StringStorage::from(details::scalar_data(source_view));
        const auto input_last_updated = TimeSeriesState{time};
        const auto input = In<TS<string>>{TimeSeriesInputData{
            runtime_type, input_last_updated, source_view.data(), true}};

        output.copy(input);
    }

    CHECK(output_value == source_storage);
    CHECK(output.value() == "shared string");

    auto memory = allocator{};
    cpp_scalar_runtime_type<string>().destroy(&output_value, memory);
}

TEST_CASE("copy copies every field of a time series bundle",
          "[feature:node-api][feature:test-graph][series:tsb][schema:named]"
          "[representation:free][representation:native][transition:update]") {
    const auto copy = node([](In<TSB<copy_bundle>> input, Out<TSB<copy_bundle>> output) {
        output.copy(input);
    });
    const auto sum = node([](In<TSB<copy_bundle>> input, Out<TS<i64>> output) {
        output.set(*input->first + *input->second);
    });
    const auto graph = [&](wp<TS<i64>> first, wp<TS<i64>> second) {
        return sum(copy(bundle<copy_bundle>(first, second)));
    };

    const auto actual = testing::test_graph(graph, std::array{1, 2}, std::array{10, 20});

    CHECK(actual.equals({11, 22}));
}

TEST_CASE("copy_tick modifies only updated fields of a time series bundle",
          "[feature:node-api][feature:test-graph][series:generic][series:tsb][transition:update]") {
    const auto source = node([](In<TS<i64>> cycle, Out<TSB<copy_bundle>> output) {
        if (*cycle == 0) {
            output->first.set(1);
            output->second.set(10);
            return;
        }
        output->first.set(2);
    });
    const auto copy_tick = node([](In<TS_T> input, Out<TS_T> output) { output.copy_tick(input); });
    const auto inspect = node([](In<TSB<copy_bundle>> input, Out<TS<i64>> output) {
        const auto modified = static_cast<i64>(input.item_updated(0)) + 2 * static_cast<i64>(input.item_updated(1));
        output.set(modified * 10000 + *input->first * 100 + *input->second);
    });
    const auto graph = [&](wp<TS<i64>> cycle) { return inspect(copy_tick(source(cycle))); };

    const auto actual = testing::test_graph(graph, std::array{0, 1});

    CHECK(actual.equals({30110, 10210}));
}

TEST_CASE("copy copies each item of a fixed time series list",
          "[feature:node-api][feature:test-graph][series:fixed-tsl][representation:free]"
          "[representation:native][transition:update]") {
    const auto copy = node([](In<TSL<TS<i64>, 2>> input, Out<TSL<TS<i64>, 2>> output) {
        output.copy(input);
    });
    const auto sum = node([](In<TSL<TS<i64>, 2>> input, Out<TS<i64>> output) {
        output.set(*input[0] + *input[1]);
    });
    const auto graph = [&](wp<TS<i64>> first, wp<TS<i64>> second) {
        return sum(copy(tsl(first, second)));
    };

    const auto actual = testing::test_graph(graph, std::array{1, 2}, std::array{10, 20});

    CHECK(actual.equals({11, 22}));
}

TEST_CASE("a generic output copies and copies ticks from a fixed time series list",
          "[feature:node-api][feature:test-graph][series:generic][series:fixed-tsl][transition:update]") {
    const auto copy = node([](In<TS_T> input, Out<TS_T> output) { output.copy(input); });
    const auto copy_tick = node([](In<TS_T> input, Out<TS_T> output) { output.copy_tick(input); });
    const auto sum = node([](In<TSL<TS<i64>, 2>> input, Out<TS<i64>> output) {
        output.set(*input[0] + *input[1]);
    });
    const auto copy_graph = [&](wp<TS<i64>> first, wp<TS<i64>> second) {
        return sum(copy(tsl(first, second)));
    };
    const auto copy_tick_graph = [&](wp<TS<i64>> first, wp<TS<i64>> second) {
        return sum(copy_tick(tsl(first, second)));
    };

    const auto copied = testing::test_graph(copy_graph, std::array{2, 4}, std::array{20, 40});
    const auto copied_tick = testing::test_graph(copy_tick_graph, std::array{2, 4}, std::array{20, 40});

    CHECK(copied.equals({22, 44}));
    CHECK(copied_tick.equals({22, 44}));
}

TEST_CASE("a fixed list copy accepts a different child guarantee",
          "[feature:node-api][feature:test-graph][series:fixed-tsl]"
          "[guarantee:ordinary][guarantee:deduplicated][transition:update]") {
    const auto copy = node([](In<TSL<TS<i64>, 2>> input,
                              Out<TSL<TS<i64, TimeSeriesGuarantee::deduplicated>, 2>> output) {
        output.copy(input);
    });
    const auto sum = node([](In<TSL<TS<i64, TimeSeriesGuarantee::deduplicated>, 2>> input,
                             Out<TS<i64>> output) { output.set(*input[0] + *input[1]); });
    const auto graph = [&](wp<TS<i64>> first, wp<TS<i64>> second) {
        return sum(copy(tsl(first, second)));
    };

    const auto actual = testing::test_graph(graph, std::array{1, 1, 2}, std::array{10, 10, 20});

    CHECK(actual.equals({11, std::nullopt, 22}));
}

TEST_CASE("copy reconciles a time series dictionary snapshot",
          "[feature:node-api][feature:test-graph][series:tsd][transition:update][transition:removal]") {
    const auto source = node([](In<TS<i64>> value, Out<TSD<i64, TS<i64>>> output) {
        output[1].set(*value);
        if (*value == 3) {
            output.erase(2);
        } else {
            output[2].set(*value + 10);
        }
    });
    const auto copy = node([](In<TSD<i64, TS<i64>>> input, Out<TSD<i64, TS<i64>>> output) {
        output.copy(input);
    });
    const auto summarize = node([](In<TSD<i64, TS<i64>>> input, Out<TS<i64>> output) {
        const auto first = input.contains(1) ? *input[1] : 0;
        const auto second = input.contains(2) ? *input[2] : 0;
        output.set(static_cast<i64>(input.size() * 100) + first * 10 + second);
    });
    const auto graph = [&](wp<TS<i64>> value) { return summarize(copy(source(value))); };

    const auto actual = testing::test_graph(graph, std::array{1, 2, 3});

    CHECK(actual.equals({221, 232, 130}));
}

TEST_CASE("a dictionary copy accepts a different child guarantee",
          "[feature:node-api][feature:test-graph][series:tsd]"
          "[guarantee:ordinary][guarantee:deduplicated][transition:update]") {
    const auto source = node([](In<TS<i64>> value, Out<TSD<i64, TS<i64>>> output) {
        output[1].set(*value);
    });
    const auto copy = node([](In<TSD<i64, TS<i64>>> input,
                              Out<TSD<i64, TS<i64, TimeSeriesGuarantee::deduplicated>>> output) {
        output.copy(input);
    });
    const auto read = node([](In<TSD<i64, TS<i64, TimeSeriesGuarantee::deduplicated>>> input,
                              Out<TS<i64>> output) { output.set(*input[1]); });
    const auto graph = [&](wp<TS<i64>> value) { return read(copy(source(value))); };

    const auto actual = testing::test_graph(graph, std::array{1, 1, 2});

    CHECK(actual.equals({1, std::nullopt, 2}));
}

TEST_CASE("copy_tick publishes an initially empty time series dictionary",
          "[feature:node-api][feature:test-graph][series:tsd][transition:first-tick]"
          "[transition:addition]") {
    const auto source = node([](In<TS<i64>> cycle, Out<TSD<i64, TS<i64>>> output) {
        if (*cycle == 0) {
            output.clear();
            return;
        }
        output[1].set(*cycle);
    });
    const auto copy = node([](In<TSD<i64, TS<i64>>> input, Out<TSD<i64, TS<i64>>> output) {
        output.copy_tick(input);
    });
    const auto size = node([](In<TSD<i64, TS<i64>>> input, Out<TS<i64>> output) {
        output.set(static_cast<i64>(input.size()));
    });
    const auto graph = [&](wp<TS<i64>> cycle) { return size(copy(source(cycle))); };

    const auto actual = testing::test_graph(graph, std::array{0, 1});

    CHECK(actual.equals({0, 1}));
}

TEST_CASE("copy handles an invalid erased input without runtime type metadata",
          "[feature:node-api][feature:test-graph][series:generic][series:tsd][declaration:generic]"
          "[transition:invalidation]") {
    const auto source = node([](In<TS<i64>> value, Out<TSD<i64, TS<i64>>> output) {
        output[1].set(*value);
    });
    const auto copy_invalid = node([](In<TS_T> trigger, Out<TS_T> output) {
        output.copy(trigger);
        output.copy(In<TS_T>{TimeSeriesInputData{}});
    });
    const auto size = node([](In<TSD<i64, TS<i64>>> input, Out<TS<i64>> output) {
        output.set(static_cast<i64>(input.size()));
    });
    const auto graph = [&](wp<TS<i64>> value) { return size(copy_invalid(source(value))); };

    const auto actual = testing::test_graph(graph, std::array{1, 2});

    CHECK(actual.equals({1, 1}));
}

TEST_CASE("copy reconciles a time series set snapshot",
          "[feature:node-api][feature:test-graph][series:tss][transition:addition][transition:removal]") {
    const auto source = node([](In<TS<i64>> value, Out<TSS<i64>> output) {
        if (*value == 0) {
            output.insert(1);
            output.insert(2);
        } else if (*value == 1) {
            output.erase(2);
            output.insert(3);
        } else {
            output.erase(3);
        }
    });
    const auto copy = node([](In<TSS<i64>> input, Out<TSS<i64>> output) {
        output.copy(input);
    });
    const auto summarize = node([](In<TSS<i64>> input, Out<TS<i64>> output) {
        const auto membership = static_cast<int>(input.contains(1)) + 2 * static_cast<int>(input.contains(2)) +
                                3 * static_cast<int>(input.contains(3));
        output.set(static_cast<i64>(input.size() * 10 + membership));
    });
    const auto graph = [&](wp<TS<i64>> value) { return summarize(copy(source(value))); };

    const auto actual = testing::test_graph(graph, std::array{0, 1, 2});

    CHECK(actual.equals({23, 24, 11}));
}

TEST_CASE("a generic output copies time series set snapshots and changes",
          "[feature:node-api][feature:test-graph][series:generic][series:tss]"
          "[transition:addition][transition:removal]") {
    const auto source = node([](In<TS<i64>> cycle, Out<TSS<i64>> output) {
        if (*cycle == 0) {
            output.insert(1);
            output.insert(2);
        } else if (*cycle == 1) {
            output.erase(2);
            output.insert(3);
        } else {
            output.erase(3);
        }
    });
    const auto copy = node([](In<TS_T> input, Out<TS_T> output) { output.copy(input); });
    const auto copy_tick = node([](In<TS_T> input, Out<TS_T> output) { output.copy_tick(input); });
    const auto summary = node([](In<TSS<i64>> input, Out<TS<i64>> output) {
        const auto membership = static_cast<int>(input.contains(1)) + 2 * static_cast<int>(input.contains(2)) +
                                3 * static_cast<int>(input.contains(3));
        output.set(static_cast<i64>(input.size() * 10 + membership));
    });
    const auto copy_graph = [&](wp<TS<i64>> cycle) { return summary(copy(source(cycle))); };
    const auto copy_tick_graph = [&](wp<TS<i64>> cycle) { return summary(copy_tick(source(cycle))); };

    const auto copied = testing::test_graph(copy_graph, std::array{0, 1, 2});
    const auto copied_tick = testing::test_graph(copy_tick_graph, std::array{0, 1, 2});

    CHECK(copied.equals({23, 24, 11}));
    CHECK(copied_tick.equals({23, 24, 11}));
}

TEST_CASE("copy_tick copies a reference-bound time series set",
          "[feature:node-api][feature:sampling][series:tss][binding:reference-to-value]"
          "[transition:addition][transition:removal]") {
    const auto source = node([](In<TS<i64>> cycle, Out<TSS<i64>> output) {
        if (*cycle == 0) {
            output.insert(1);
            output.insert(2);
        } else if (*cycle == 1) {
            output.erase(2);
            output.insert(3);
        } else {
            output.erase(3);
        }
    });
    const auto preserve_reference = node([](In<REF<TSS<i64>>> input, Out<REF<TSS<i64>>> output) {
        output.copy(input);
    });
    const auto copy = node([](In<TSS<i64>> input, Out<TSS<i64>> output) { output.copy_tick(input); });
    const auto summary = node([](In<TSS<i64>> input, Out<TS<i64>> output) {
        const auto membership = static_cast<int>(input.contains(1)) + 2 * static_cast<int>(input.contains(2)) +
                                3 * static_cast<int>(input.contains(3));
        output.set(static_cast<i64>(input.size() * 10 + membership));
    });
    const auto graph = [&](wp<TS<i64>> cycle) {
        return summary(copy(preserve_reference(source(cycle))));
    };

    const auto actual = testing::test_graph(graph, std::array{0, 1, 2});

    CHECK(actual.equals({23, 24, 11}));
}

TEST_CASE("copy copies a reference-bound time series dictionary",
          "[feature:node-api][feature:sampling][series:tsd][binding:reference-to-value]"
          "[transition:update][transition:removal]") {
    const auto source = node([](In<TS<i64>> cycle, Out<TSD<i64, TS<i64>>> output) {
        output[1].set(*cycle + 10);
        if (*cycle == 0) {
            output[2].set(20);
        } else if (*cycle == 1) {
            output.erase(2);
        } else {
            output[3].set(30);
        }
    });
    const auto preserve_reference = node([](In<REF<TSD<i64, TS<i64>>>> input,
                                            Out<REF<TSD<i64, TS<i64>>>> output) { output.copy(input); });
    const auto copy = node([](In<TSD<i64, TS<i64>>> input, Out<TSD<i64, TS<i64>>> output) {
        output.copy(input);
    });
    const auto summary = node([](In<TSD<i64, TS<i64>>> input, Out<TS<i64>> output) {
        const auto first = input.contains(1) ? *input[1] : 0;
        const auto second = input.contains(2) ? *input[2] : 0;
        const auto third = input.contains(3) ? *input[3] : 0;
        output.set(static_cast<i64>(input.size() * 100) + first * 10 + second + third);
    });
    const auto graph = [&](wp<TS<i64>> cycle) { return summary(copy(preserve_reference(source(cycle)))); };

    const auto actual = testing::test_graph(graph, std::array{0, 1, 2});

    CHECK(actual.equals({320, 210, 350}));
}

TEST_CASE("copy uses the valid_items view of a dictionary input",
          "[feature:node-api][feature:test-graph][series:tsd][input-guarantee:valid-items]"
          "[transition:update]") {
    const auto source = node([](In<TS<i64>> value, Out<TSD<i64, TS<i64>>> output) {
        output[1].set(*value);
        output[2].invalidate();
    });
    const auto copy = node([](In<TSD<i64, TS<i64>>, valid_items> input,
                              Out<TSD<i64, TS<i64>>> output) { output.copy(input); });
    const auto summary = node([](In<TSD<i64, TS<i64>>> input, Out<TS<i64>> output) {
        output.set(static_cast<i64>(input.size() * 10 + input.contains(2)));
    });
    const auto graph = [&](wp<TS<i64>> value) { return summary(copy(source(value))); };

    const auto actual = testing::test_graph(graph, std::array{1, 2});

    CHECK(actual.equals({10, 10}));
}

TEST_CASE("copy_tick publishes an initially empty time series set",
          "[feature:node-api][feature:test-graph][series:tss][transition:first-tick]"
          "[transition:addition]") {
    const auto source = node([](In<TS<i64>> cycle, Out<TSS<i64>> output) {
        if (*cycle == 0) {
            output.clear();
            return;
        }
        output.insert(*cycle);
    });
    const auto copy = node([](In<TSS<i64>> input, Out<TSS<i64>> output) { output.copy_tick(input); });
    const auto size = node([](In<TSS<i64>> input, Out<TS<i64>> output) {
        output.set(static_cast<i64>(input.size()));
    });
    const auto graph = [&](wp<TS<i64>> cycle) { return size(copy(source(cycle))); };

    const auto actual = testing::test_graph(graph, std::array{0, 1});

    CHECK(actual.equals({0, 1}));
}

TEST_CASE("a nested dictionary copy reconciles nested item changes",
          "[feature:node-api][feature:test-graph][series:tsd][shape:nested]"
          "[transition:update][transition:removal]") {
    const auto source = node([](In<TS<i64>> cycle, Out<TSD<i64, TSD<i64, TS<i64>>>> output) {
        output[1][10].set(*cycle);
        if (*cycle == 2) {
            output[1].erase(11);
        } else {
            output[1][11].set(*cycle + 10);
        }
    });
    const auto copy = node([](In<TSD<i64, TSD<i64, TS<i64>>>> input,
                              Out<TSD<i64, TSD<i64, TS<i64>>>> output) {
        output.copy(input);
    });
    const auto summarize = node([](In<TSD<i64, TSD<i64, TS<i64>>>> input, Out<TS<i64>> output) {
        const auto nested = input[1];
        const auto second = nested.contains(11) ? *nested[11] : 0;
        output.set(static_cast<i64>(nested.size() * 100) + *nested[10] + second);
    });
    const auto graph = [&](wp<TS<i64>> cycle) { return summarize(copy(source(cycle))); };

    const auto actual = testing::test_graph(graph, std::array{1, 2});

    CHECK(actual.equals({212, 102}));
}

TEST_CASE("copy_tick forwards a nested dictionary delta to its changed leaves",
          "[feature:node-api][feature:test-graph][series:tsd][shape:nested]"
          "[transition:update][transition:removal]") {
    const auto source = node([](In<TS<i64>> cycle, Out<TSD<i64, TSD<i64, TS<i64>>>> output) {
        output[1][10].set(*cycle);
        if (*cycle == 1) {
            output[1][11].set(11);
        } else if (*cycle == 3) {
            output[1].erase(11);
        }
    });
    const auto copy = node([](In<TSD<i64, TSD<i64, TS<i64>>>> input,
                              Out<TSD<i64, TSD<i64, TS<i64>>>> output) {
        output.copy_tick(input);
    });
    const auto summary = node([](In<TSD<i64, TSD<i64, TS<i64>>>> full,
                                 In<TSD<i64, TSD<i64, TS<i64>>>> delta,
                                 Out<TS<i64>> output) {
        const auto full_inner = full[1];
        const auto delta_inner = delta[1];
        const auto full_value = *full_inner[10] + (full_inner.contains(11) ? *full_inner[11] : 0);
        const auto delta_value = *delta_inner[10] + (delta_inner.contains(11) ? *delta_inner[11] : 0);
        const auto unchanged_item = *full_inner[10] != 2 || !delta_inner.item_modified(11);
        output.set(static_cast<i64>(full_inner.size() == delta_inner.size() && full_value == delta_value &&
                                    unchanged_item));
    });
    const auto full_copy = node([](In<TSD<i64, TSD<i64, TS<i64>>>> input,
                                   Out<TSD<i64, TSD<i64, TS<i64>>>> output) {
        output.copy(input);
    });
    const auto graph = [&](wp<TS<i64>> cycle) {
        const auto nested = source(cycle);
        return summary(full_copy(nested), copy(nested));
    };

    const auto actual = testing::test_graph(graph, std::array{1, 2, 3});

    CHECK(actual.equals({1, 1, 1}));
}

TEST_CASE("copy_tick forwards a reference to its target",
          "[feature:node-api][feature:test-graph][feature:reference][series:reference]"
          "[transition:first-tick][transition:update][binding:value-to-reference]"
          "[binding:reference-to-value]") {
    const auto copy_reference = node([](In<REF<TS<i64>>> input, Out<REF<TS<i64>>> output) {
        output.copy_tick(input);
    });
    const auto read = node([](In<TS<i64>> input, Out<TS<i64>> output) { output.set(*input); });
    const auto graph = [&](wp<TS<i64>> input) { return read(copy_reference(input)); };

    const auto actual = testing::test_graph(graph, std::array{1, 2, 3});

    CHECK(actual.equals({1, 2, 3}));
}

TEST_CASE("copy_tick forwards reference selection changes and invalidation",
          "[feature:node-api][feature:test-graph][feature:reference][series:reference]"
          "[transition:update][transition:invalidation][transition:revalidation]") {
    const auto first = node([](Out<TS<i64>> output) { output.set(10); });
    const auto second = node([](Out<TS<i64>> output) { output.set(20); });
    const auto select = node([](In<TS<i64>> command, In<REF<TS<i64>>, passive> first,
                                In<REF<TS<i64>>, passive> second, Out<REF<TS<i64>>> output) {
        if (*command == 2) {
            output.invalidate();
        } else {
            output.copy(*command == 1 ? second : first);
        }
    });
    const auto copy_tick = node([](In<REF<TS<i64>>, optional> input, Out<REF<TS<i64>>> output) {
        output.copy_tick(input);
    });
    const auto read = node([](In<TS<i64>, optional> input, Out<TS<i64>> output) {
        if (input.valid()) {
            output.set(*input);
        } else {
            output.invalidate();
        }
    });
    const auto graph = [&](wp<TS<i64>> command) { return read(copy_tick(select(command, first(), second()))); };

    const auto actual = testing::test_graph(graph, std::array{0, 1, 2, 0});

    CHECK(actual.equals({10, 20, std::nullopt, 10}));
}

TEST_CASE("copy preserves the destination guarantee",
          "[feature:node-api][feature:test-graph][series:value][guarantee:deduplicated]"
          "[transition:update]") {
    const auto copy = node([](In<TS<i64>> input,
                              Out<TS<i64, TimeSeriesGuarantee::deduplicated>> output) { output.copy(input); });
    const auto graph = [&](wp<TS<i64>> input) { return copy(input); };

    const auto actual = testing::test_graph(graph, std::array{1, 1, 2});

    CHECK(actual.equals({1, std::nullopt, 2}));
}

TEST_CASE("dictionary copy rejects a valid input from another time series family",
          "[feature:node-api][series:tsd][series:value][declaration:runtime]") {
    const auto &input_runtime = cpp_time_series_runtime_type<TS<i64>>();
    const auto &output_runtime = cpp_time_series_runtime_type<TSD<i64, TS<i64>>>();
    const auto last_updated = TimeSeriesState{EvaluationTime{std::chrono::microseconds{1}}};
    const auto input_value = i64{42};
    const auto input = TimeSeriesInputData{input_runtime, last_updated, &input_value};
    auto output = OutputState{output_runtime};

    CHECK_THROWS_AS(output_runtime.copy(input, output), std::invalid_argument);
}
