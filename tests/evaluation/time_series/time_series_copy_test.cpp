#include <bg/node.hpp>
#include <bg/operators.hpp>
#include <bg/scalar/details/string_storage.hpp>
#include <bg/scalar/details/value_access.hpp>
#include <bg/scalar/value.hpp>
#include <bg/run_graph.hpp>
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

    const auto actual = bg::run_graph(graph, input, std::array{0, 1, 2});

    CHECK(actual.equals({1, 1, 2}));
}

TEST_CASE("a generic output copies and copies a tick from a time series value",
          "[feature:node-api][feature:test-graph][series:generic][series:value][transition:update]") {
    const auto copy = node([](In<TS_T> input, Out<TS_T> output) { output.copy(input); });

    const auto copy_tick = node([](In<TS_T> input, Out<TS_T> output) { output.copy_tick(input); });

    const auto copy_graph = [&](wp<TS<i64>> input) { return copy(input); };

    const auto copy_tick_graph = [&](wp<TS<i64>> input) { return copy_tick(input); };

    const auto copied = bg::run_graph(copy_graph, std::array{3, 5});

    const auto copied_tick = bg::run_graph(copy_tick_graph, std::array{3, 5});

    CHECK(copied.equals<i64>({3, 5}));
    CHECK(copied_tick.equals<i64>({3, 5}));
}

TEST_CASE("copy_tick applies a current invalidation but ignores its stale state",
          "[feature:node-api][feature:test-graph][series:value]"
          "[transition:update][transition:invalidation][transition:revalidation]") {
    const auto copy_tick = node([](In<TS<i64>, passive, optional> input, In<TS<i64>> trigger,
                                   Out<TS<i64>> output) {
        if (*trigger == 2) {
            output.set(99);
        }
        output.copy_tick(input);
    });

    const auto graph = [&](wp<TS<i64>> input, wp<TS<i64>> trigger) {
        return copy_tick(input, trigger);
    };

    const auto actual = bg::run_graph(graph, {1, literal::INVALID, literal::SKIP, 2}, {0, 1, 2, 3});

    CHECK(actual.equals({1, std::nullopt, 99, 2}));
}

TEST_CASE("copy propagates value invalidation and revalidation",
          "[feature:node-api][feature:test-graph][series:value]"
          "[transition:invalidation][transition:revalidation]") {
    const auto copy = node([](In<TS<i64>, optional> input, Out<TS<i64>> output) { output.copy(input); });

    const auto graph = [&](wp<TS<i64>> input) { return copy(input); };

    const auto actual = bg::run_graph(graph, {1, literal::INVALID, 2});

    CHECK(actual.equals({1, std::nullopt, 2}));
}

TEST_CASE("reference copy accepts independent declarations and propagates invalidation",
          "[feature:node-api][feature:test-graph][series:reference]"
          "[transition:invalidation][transition:revalidation]") {
    const auto value = node([](Out<TS<i64>> output) { output.set(10); });

    const auto reference = node([](In<TS<i64>> cycle, In<REF<TS<i64>>> input, Out<REF<TS<i64>>> output) {
        if (*cycle == 1) {
            output.invalidate();
        } else {
            output.copy(input);
        }
    });

    const auto copy = node([](In<REF<TS_T>, optional> input,
                              Out<REF<TS<i64>>> output) { output.copy(input); });

    const auto copy_tick = node([](In<REF<TS_T>, optional> input,
                                   Out<REF<TS<i64>>> output) { output.copy_tick(input); });

    const auto read = node([](In<TS<i64>, optional> input, Out<TS<i64>> output) { output.copy(input); });

    const auto observe_validity = node([](In<TS<i64>, optional> input, Out<TS<bool>> output) {
        output.set(input.valid());
    });

    const auto copy_graph = [&](wp<TS<i64>> cycle) {
        return read(copy(reference(cycle, value())));
    };

    const auto copy_validity_graph = [&](wp<TS<i64>> cycle) {
        return observe_validity(copy(reference(cycle, value())));
    };

    const auto copy_tick_graph = [&](wp<TS<i64>> cycle) {
        return read(copy_tick(reference(cycle, value())));
    };

    const auto copy_tick_validity_graph = [&](wp<TS<i64>> cycle) {
        return observe_validity(copy_tick(reference(cycle, value())));
    };

    const auto cycles = std::array{0, 1, 2};

    CHECK(bg::run_graph(copy_graph, cycles).equals<i64>({10, std::nullopt, 10}));
    CHECK(bg::run_graph(copy_validity_graph, cycles).equals<bool>({true, false, true}));
    CHECK(bg::run_graph(copy_tick_graph, cycles).equals<i64>({10, std::nullopt, 10}));
    CHECK(bg::run_graph(copy_tick_validity_graph, cycles).equals<bool>({true, false, true}));
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

    const auto sum = node([](In<TSB<copy_bundle>> input) { return *input->first + *input->second; });

    const auto graph = [&](wp<TS<i64>> first, wp<TS<i64>> second) {
        return sum(copy(bundle<copy_bundle>(first, second)));
    };

    const auto actual = bg::run_graph(graph, std::array{1, 2}, std::array{10, 20});

    CHECK(actual.equals({11, 22}));
}

TEST_CASE("copy_tick modifies only updated fields of a time series bundle",
          "[feature:node-api][feature:test-graph][series:generic][series:tsb][transition:update]") {
    const auto copy_tick = node([](In<TS_T> input, Out<TS_T> output) { output.copy_tick(input); });

    const auto inspect = node([](In<TSB<copy_bundle>> input) {
        const auto modified = static_cast<i64>(input.item_updated(0)) + 2 * static_cast<i64>(input.item_updated(1));
        return modified * 10000 + *input->first * 100 + *input->second;
    });

    const auto graph = [&](wp<TSB<copy_bundle>> input) { return inspect(copy_tick(input)); };

    const auto actual = bg::run_graph(graph, {{{"first", 1}, {"second", 10}}, {{"first", 2}}});

    CHECK(actual.equals({30110, 10210}));
}

TEST_CASE("copy copies each item of a fixed time series list",
          "[feature:node-api][feature:test-graph][series:fixed-tsl][representation:free]"
          "[representation:native][transition:update]") {
    const auto copy = node([](In<TSL<TS<i64>, 2>> input, Out<TSL<TS<i64>, 2>> output) {
        output.copy(input);
    });

    const auto sum = node([](In<TSL<TS<i64>, 2>> input) { return *input[0] + *input[1]; });

    const auto graph = [&](wp<TS<i64>> first, wp<TS<i64>> second) {
        return sum(copy(tsl(first, second)));
    };

    const auto actual = bg::run_graph(graph, std::array{1, 2}, std::array{10, 20});

    CHECK(actual.equals({11, 22}));
}

TEST_CASE("a generic output copies and copies ticks from a fixed time series list",
          "[feature:node-api][feature:test-graph][series:generic][series:fixed-tsl][transition:update]") {
    const auto copy = node([](In<TS_T> input, Out<TS_T> output) { output.copy(input); });

    const auto copy_tick = node([](In<TS_T> input, Out<TS_T> output) { output.copy_tick(input); });

    const auto sum = node([](In<TSL<TS<i64>, 2>> input) { return *input[0] + *input[1]; });

    const auto copy_graph = [&](wp<TS<i64>> first, wp<TS<i64>> second) {
        return sum(copy(tsl(first, second)));
    };

    const auto copy_tick_graph = [&](wp<TS<i64>> first, wp<TS<i64>> second) {
        return sum(copy_tick(tsl(first, second)));
    };

    const auto copied = bg::run_graph(copy_graph, std::array{2, 4}, std::array{20, 40});

    const auto copied_tick = bg::run_graph(copy_tick_graph, std::array{2, 4}, std::array{20, 40});

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

    const auto sum = node([](In<TSL<TS<i64, TimeSeriesGuarantee::deduplicated>, 2>> input) {
        return *input[0] + *input[1];
    });

    const auto graph = [&](wp<TS<i64>> first, wp<TS<i64>> second) {
        return sum(copy(tsl(first, second)));
    };

    const auto actual = bg::run_graph(graph, std::array{1, 1, 2}, std::array{10, 10, 20});

    CHECK(actual.equals({11, std::nullopt, 22}));
}

TEST_CASE("copy reconciles a time series dictionary snapshot",
          "[feature:node-api][feature:test-graph][series:tsd][transition:update][transition:removal]") {
    const auto copy = node([](In<TSD<i64, TS<i64>>> input, Out<TSD<i64, TS<i64>>> output) {
        output.copy(input);
    });

    const auto summarize = node([](In<TSD<i64, TS<i64>>> input) {
        const auto first = input.contains(1) ? *input[1] : 0;
        const auto second = input.contains(2) ? *input[2] : 0;
        return static_cast<i64>(input.size() * 100) + first * 10 + second;
    });

    const auto graph = [&](wp<TSD<i64, TS<i64>>> input) { return summarize(copy(input)); };

    const auto actual = bg::run_graph(
        graph, {{{1, 1}, {2, 11}}, {{1, 2}, {2, 12}}, {{1, 3}, {2, literal::REMOVE}}});

    CHECK(actual.equals({221, 232, 130}));
}

TEST_CASE("a dictionary copy accepts a different child guarantee",
          "[feature:node-api][feature:test-graph][series:tsd]"
          "[guarantee:ordinary][guarantee:deduplicated][transition:update]") {
    const auto copy = node([](In<TSD<i64, TS<i64>>> input,
                              Out<TSD<i64, TS<i64, TimeSeriesGuarantee::deduplicated>>> output) {
        output.copy(input);
    });

    const auto read = node([](In<TSD<i64, TS<i64, TimeSeriesGuarantee::deduplicated>>> input) { return *input[1]; });

    const auto graph = [&](wp<TSD<i64, TS<i64>>> input) { return read(copy(input)); };

    const auto actual = bg::run_graph(graph, {{{1, 1}}, {{1, 1}}, {{1, 2}}});

    CHECK(actual.equals({1, std::nullopt, 2}));
}

TEST_CASE("copy_tick publishes an initially empty time series dictionary",
          "[feature:node-api][feature:test-graph][series:tsd][transition:first-tick]"
          "[transition:addition]") {
    const auto copy = node([](In<TSD<i64, TS<i64>>> input, Out<TSD<i64, TS<i64>>> output) {
        output.copy_tick(input);
    });

    const auto size = node([](In<TSD<i64, TS<i64>>> input) { return static_cast<i64>(input.size()); });

    const auto graph = [&](wp<TSD<i64, TS<i64>>> input) { return size(copy(input)); };

    const auto actual = bg::run_graph(graph, {{}, {{1, 1}}});

    CHECK(actual.equals({0, 1}));
}

TEST_CASE("copy handles an invalid erased input without runtime type metadata",
          "[feature:node-api][feature:test-graph][series:generic][series:tsd][declaration:generic]"
          "[transition:invalidation]") {
    const auto copy_invalid = node([](In<TS_T> trigger, Out<TS_T> output) {
        output.copy(trigger);
        output.copy(In<TS_T>{TimeSeriesInputData{}});
    });

    const auto observe_validity = node([](In<TSD<i64, TS<i64>>, optional> input) { return input.valid(); });

    const auto graph = [&](wp<TSD<i64, TS<i64>>> input) { return observe_validity(copy_invalid(input)); };

    const auto actual = bg::run_graph(graph, {{{1, 1}}, {{1, 2}}});

    CHECK(actual.equals({false, false}));
}

TEST_CASE("copy reconciles a time series set snapshot",
          "[feature:node-api][feature:test-graph][series:tss][transition:addition][transition:removal]") {
    const auto copy = node([](In<TSS<i64>> input, Out<TSS<i64>> output) {
        output.copy(input);
    });

    const auto summarize = node([](In<TSS<i64>> input) {
        const auto membership = static_cast<int>(input.contains(1)) + 2 * static_cast<int>(input.contains(2)) +
                                3 * static_cast<int>(input.contains(3));
        return static_cast<i64>(input.size() * 10 + membership);
    });

    const auto graph = [&](wp<TSS<i64>> input) { return summarize(copy(input)); };

    const auto actual = bg::run_graph(
        graph, {{1, 2}, {literal::REMOVED(2), 3}, {literal::REMOVED(3)}});

    CHECK(actual.equals({23, 24, 11}));
}

TEST_CASE("a generic output copies time series set snapshots and changes",
          "[feature:node-api][feature:test-graph][series:generic][series:tss]"
          "[transition:addition][transition:removal]") {
    const auto copy = node([](In<TS_T> input, Out<TS_T> output) { output.copy(input); });

    const auto copy_tick = node([](In<TS_T> input, Out<TS_T> output) { output.copy_tick(input); });

    const auto summary = node([](In<TSS<i64>> input) {
        const auto membership = static_cast<int>(input.contains(1)) + 2 * static_cast<int>(input.contains(2)) +
                                3 * static_cast<int>(input.contains(3));
        return static_cast<i64>(input.size() * 10 + membership);
    });

    const auto copy_graph = [&](wp<TSS<i64>> input) { return summary(copy(input)); };

    const auto copy_tick_graph = [&](wp<TSS<i64>> input) { return summary(copy_tick(input)); };

    const auto copied = bg::run_graph(copy_graph, {{1, 2}, {literal::REMOVED(2), 3}, {literal::REMOVED(3)}});

    const auto copied_tick = bg::run_graph(
        copy_tick_graph, {{1, 2}, {literal::REMOVED(2), 3}, {literal::REMOVED(3)}});

    CHECK(copied.equals({23, 24, 11}));
    CHECK(copied_tick.equals({23, 24, 11}));
}

TEST_CASE("copy_tick copies a reference-bound time series set",
          "[feature:node-api][feature:sampling][series:tss][binding:reference-to-value]"
          "[transition:addition][transition:removal]") {
    const auto preserve_reference = node([](In<REF<TSS<i64>>> input, Out<REF<TSS<i64>>> output) {
        output.copy(input);
    });

    const auto copy = node([](In<TSS<i64>> input, Out<TSS<i64>> output) { output.copy_tick(input); });

    const auto summary = node([](In<TSS<i64>> input) {
        const auto membership = static_cast<int>(input.contains(1)) + 2 * static_cast<int>(input.contains(2)) +
                                3 * static_cast<int>(input.contains(3));
        return static_cast<i64>(input.size() * 10 + membership);
    });

    const auto graph = [&](wp<TSS<i64>> input) {
        return summary(copy(preserve_reference(input)));
    };

    const auto actual = bg::run_graph(graph, {{1, 2}, {literal::REMOVED(2), 3}, {literal::REMOVED(3)}});

    CHECK(actual.equals({23, 24, 11}));
}

TEST_CASE("collection copies remove items when a reference selects an invalid TSS and revalidate",
          "[feature:node-api][feature:test-graph][feature:sampling][series:tss]"
          "[binding:reference-to-value][transition:invalidation][transition:revalidation]"
          "[transition:removal][transition:addition]") {
    const auto select = node([](In<TS<i64>> cycle, In<REF<TSS<i64>>, passive> populated,
                                In<REF<TSS<i64>>, passive> invalid, Out<REF<TSS<i64>>> output) {
        output.copy(*cycle == 1 ? invalid : populated);
    });

    const auto copy_tick = node([](In<TSS<i64>, optional> input, Out<TSS<i64>> output) {
        if (!output.valid()) {
            output.insert(9);
        }
        output.copy_tick(input);
    });

    const auto copy = node([](In<TSS<i64>, optional> input, Out<TSS<i64>> output) { output.copy(input); });

    const auto generic_copy_tick = node([](In<TS_T, optional> input, Out<TS_T> output) {
        output.copy_tick(input);
    });

    const auto generic_copy = node([](In<TS_T, optional> input, Out<TS_T> output) { output.copy(input); });

    const auto inspect = node([](In<TSS<i64>> tick_copy, In<TSS<i64>> full_copy,
                                 In<TSS<i64>> generic_tick_copy, In<TSS<i64>> generic_full_copy) {
        const auto tick_members = static_cast<i64>(tick_copy.contains(1)) + 2 * tick_copy.contains(2) +
                                  9 * tick_copy.contains(9);
        const auto full_members = static_cast<i64>(full_copy.contains(1)) + 2 * full_copy.contains(2);
        const auto generic_tick_members = static_cast<i64>(generic_tick_copy.contains(1)) +
                                          2 * generic_tick_copy.contains(2);
        const auto generic_full_members = static_cast<i64>(generic_full_copy.contains(1)) +
                                          2 * generic_full_copy.contains(2);
        const auto first_key = i64{1};
        const auto second_key = i64{2};
        const auto reports_removals = tick_copy.was_removed(Scalar<i64>::from(first_key)) &&
                                      tick_copy.was_removed(Scalar<i64>::from(second_key)) &&
                                      full_copy.was_removed(Scalar<i64>::from(first_key)) &&
                                      full_copy.was_removed(Scalar<i64>::from(second_key)) &&
                                      generic_tick_copy.was_removed(Scalar<i64>::from(first_key)) &&
                                      generic_tick_copy.was_removed(Scalar<i64>::from(second_key)) &&
                                      generic_full_copy.was_removed(Scalar<i64>::from(first_key)) &&
                                      generic_full_copy.was_removed(Scalar<i64>::from(second_key));
        return tick_members + 10 * full_members + 100 * generic_tick_members +
               1000 * generic_full_members + 10000 * reports_removals;
    });

    const auto graph = [&](wp<TS<i64>> cycle, wp<TSS<i64>> populated, wp<TSS<i64>> invalid) {
        const auto selected = select(cycle, populated, invalid);
        return inspect(copy_tick(selected), copy(selected), generic_copy_tick(selected), generic_copy(selected));
    };

    const auto actual = bg::run_graph(graph, {0, 1, 2}, {{1, 2}, literal::SKIP, literal::SKIP},
                                            {});

    CHECK(actual.equals({3342, 10009, 3342}));
}

TEST_CASE("generic collection copy_tick removes nested items from an invalid TSD target",
          "[feature:node-api][feature:test-graph][feature:sampling][series:tsd][series:generic]"
          "[binding:reference-to-value][shape:nested][transition:invalidation]"
          "[transition:revalidation][transition:removal][transition:addition]") {
    const auto select = node([](In<TS<i64>> cycle, In<REF<TSD<i64, TSD<i64, TS<i64>>>>, passive> populated,
                                In<REF<TSD<i64, TSD<i64, TS<i64>>>>, passive> invalid,
                                Out<REF<TSD<i64, TSD<i64, TS<i64>>>>> output) {
        output.copy(*cycle == 1 ? invalid : populated);
    });

    const auto copy_tick = node([](In<TS_T, optional> input, Out<TS_T> output) { output.copy_tick(input); });

    const auto copy = node([](In<TSD<i64, TSD<i64, TS<i64>>>, optional> input,
                              Out<TSD<i64, TSD<i64, TS<i64>>>> output) { output.copy(input); });

    const auto inspect = node([](In<TSD<i64, TSD<i64, TS<i64>>>> tick_copy,
                                 In<TSD<i64, TSD<i64, TS<i64>>>> full_copy) {
        const auto value = [](const auto &input) {
            auto result = i64{};
            if (input.contains(1)) {
                const auto nested = input[1];
                result = static_cast<i64>(input.size() * 100 + nested.size() * 10);
                if (nested.contains(10)) {
                    result += *nested[10];
                }
                if (nested.contains(11)) {
                    result += *nested[11];
                }
            }
            return result;
        };
        const auto removed_key = i64{1};
        const auto reports_removal =
            tick_copy.find_removed_id(Scalar<i64>::from(removed_key)) != util::occupancy_index::npos &&
            full_copy.find_removed_id(Scalar<i64>::from(removed_key)) != util::occupancy_index::npos;
        return value(tick_copy) + 10 * value(full_copy) + 1000 * reports_removal;
    });

    const auto graph = [&](wp<TS<i64>> cycle, wp<TSD<i64, TSD<i64, TS<i64>>>> populated,
                           wp<TSD<i64, TSD<i64, TS<i64>>>> invalid) {
        const auto selected = select(cycle, populated, invalid);
        return inspect(copy_tick(selected), copy(selected));
    };

    const auto actual = bg::run_graph(graph, {0, 1, 2},
                                            {{{1, {{10, 10}, {11, 20}}}}, literal::SKIP, literal::SKIP}, {});

    CHECK(actual.equals({1650, 1000, 1650}));
}

TEST_CASE("copy_tick does not reconstruct TSS changes missed in an earlier cycle",
          "[feature:node-api][feature:test-graph][series:tss][activity:passive]"
          "[transition:addition][transition:update]") {
    const auto copy_tick = node([](In<TSS<i64>, passive, optional> input, In<TS<i64>> trigger,
                                   Out<TSS<i64>> output) {
        static_cast<void>(trigger);
        output.copy_tick(input);
    });

    const auto sum = node([](In<TSS<i64>, passive> input, In<TS<i64>> trigger) {
        static_cast<void>(trigger);
        auto value = i64{};
        input.for_each([&](const auto &item) { value += *item; });
        return value;
    });

    const auto graph = [&](wp<TSS<i64>> input, wp<TS<i64>> trigger) {
        return sum(copy_tick(input, trigger), trigger);
    };

    const auto actual = bg::run_graph(graph, {{1}, {2}, literal::SKIP}, {1, literal::SKIP, 2});

    CHECK(actual.equals({1, std::nullopt, 1}));
}

TEST_CASE("copy and copy_tick leave initially invalid collection outputs invalid",
          "[feature:node-api][feature:test-graph][series:tss][binding:reference-to-value]"
          "[transition:first-tick][validity:optional]") {
    const auto invalid_set = node([](Out<TSS<i64>>) {});

    const auto copy_tick = node([](In<TSS<i64>, passive, optional> input, In<TS<i64>> trigger,
                                   Out<TSS<i64>> output) {
        static_cast<void>(trigger);
        output.copy_tick(input);
    });

    const auto copy = node([](In<TSS<i64>, passive, optional> input, In<TS<i64>> trigger,
                              Out<TSS<i64>> output) {
        static_cast<void>(trigger);
        output.copy(input);
    });

    const auto inspect = node([](In<TS<i64>> cycle, In<TSS<i64>, passive, optional> tick_copy,
                                 In<TSS<i64>, passive, optional> full_copy) {
        static_cast<void>(cycle);
        return static_cast<i64>(tick_copy.valid()) + 10 * full_copy.valid();
    });

    const auto graph = [&](wp<TS<i64>> cycle) {
        const auto invalid = invalid_set();
        return inspect(cycle, copy_tick(invalid, cycle), copy(invalid, cycle));
    };

    const auto actual = bg::run_graph(graph, std::array{0, 1});

    CHECK(actual.equals({0, 0}));
}

TEST_CASE("copy copies a reference-bound time series dictionary",
          "[feature:node-api][feature:sampling][series:tsd][binding:reference-to-value]"
          "[transition:update][transition:removal]") {
    const auto preserve_reference = node([](In<REF<TSD<i64, TS<i64>>>> input,
                                            Out<REF<TSD<i64, TS<i64>>>> output) { output.copy(input); });

    const auto copy = node([](In<TSD<i64, TS<i64>>> input, Out<TSD<i64, TS<i64>>> output) {
        output.copy(input);
    });

    const auto summary = node([](In<TSD<i64, TS<i64>>> input) {
        const auto first = input.contains(1) ? *input[1] : 0;
        const auto second = input.contains(2) ? *input[2] : 0;
        const auto third = input.contains(3) ? *input[3] : 0;
        return static_cast<i64>(input.size() * 100) + first * 10 + second + third;
    });

    const auto graph = [&](wp<TSD<i64, TS<i64>>> input) { return summary(copy(preserve_reference(input))); };

    const auto actual = bg::run_graph(graph, {{{1, 10}, {2, 20}}, {{1, 11}, {2, literal::REMOVE}},
                                                    {{1, 12}, {3, 30}}});

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

    const auto summary = node([](In<TSD<i64, TS<i64>>> input) {
        return static_cast<i64>(input.size() * 10 + input.contains(2));
    });

    const auto graph = [&](wp<TS<i64>> value) { return summary(copy(source(value))); };

    const auto actual = bg::run_graph(graph, std::array{1, 2});

    CHECK(actual.equals({10, 10}));
}

TEST_CASE("copy_tick publishes an initially empty time series set",
          "[feature:node-api][feature:test-graph][series:tss][transition:first-tick]"
          "[transition:addition]") {
    const auto copy = node([](In<TSS<i64>> input, Out<TSS<i64>> output) { output.copy_tick(input); });

    const auto size = node([](In<TSS<i64>> input) { return static_cast<i64>(input.size()); });

    const auto graph = [&](wp<TSS<i64>> input) { return size(copy(input)); };

    const auto actual = bg::run_graph(graph, {{}, {1}});

    CHECK(actual.equals({0, 1}));
}

TEST_CASE("a nested dictionary copy reconciles nested item changes",
          "[feature:node-api][feature:test-graph][series:tsd][shape:nested]"
          "[transition:update][transition:removal]") {
    const auto copy = node([](In<TSD<i64, TSD<i64, TS<i64>>>> input,
                              Out<TSD<i64, TSD<i64, TS<i64>>>> output) {
        output.copy(input);
    });

    const auto summarize = node([](In<TSD<i64, TSD<i64, TS<i64>>>> input) {
        const auto nested = input[1];
        const auto second = nested.contains(11) ? *nested[11] : 0;
        return static_cast<i64>(nested.size() * 100) + *nested[10] + second;
    });

    const auto graph = [&](wp<TSD<i64, TSD<i64, TS<i64>>>> input) { return summarize(copy(input)); };

    const auto actual = bg::run_graph(
        graph, {{{1, {{10, 1}, {11, 11}}}}, {{1, {{10, 2}, {11, literal::REMOVE}}}}});

    CHECK(actual.equals({212, 102}));
}

TEST_CASE("copy_tick forwards a nested dictionary delta to its changed leaves",
          "[feature:node-api][feature:test-graph][series:tsd][shape:nested]"
          "[transition:update][transition:removal]") {
    const auto copy = node([](In<TSD<i64, TSD<i64, TS<i64>>>> input,
                              Out<TSD<i64, TSD<i64, TS<i64>>>> output) {
        output.copy_tick(input);
    });

    const auto summary = node([](In<TSD<i64, TSD<i64, TS<i64>>>> full,
                                 In<TSD<i64, TSD<i64, TS<i64>>>> delta) {
        const auto full_inner = full[1];
        const auto delta_inner = delta[1];
        const auto full_value = *full_inner[10] + (full_inner.contains(11) ? *full_inner[11] : 0);
        const auto delta_value = *delta_inner[10] + (delta_inner.contains(11) ? *delta_inner[11] : 0);
        const auto unchanged_item = *full_inner[10] != 2 || !delta_inner.item_modified(11);
        return static_cast<i64>(full_inner.size() == delta_inner.size() && full_value == delta_value &&
                                unchanged_item);
    });

    const auto full_copy = node([](In<TSD<i64, TSD<i64, TS<i64>>>> input,
                                   Out<TSD<i64, TSD<i64, TS<i64>>>> output) {
        output.copy(input);
    });

    const auto graph = [&](wp<TSD<i64, TSD<i64, TS<i64>>>> input) {
        return summary(full_copy(input), copy(input));
    };

    const auto actual = bg::run_graph(
        graph, {{{1, {{10, 1}, {11, 11}}}}, {{1, {{10, 2}}}}, {{1, {{10, 3}, {11, literal::REMOVE}}}}});

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

    const auto actual = bg::run_graph(graph, std::array{1, 2, 3});

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

    const auto actual = bg::run_graph(graph, std::array{0, 1, 2, 0});

    CHECK(actual.equals({10, 20, std::nullopt, 10}));
}

TEST_CASE("copy preserves the destination guarantee",
          "[feature:node-api][feature:test-graph][series:value][guarantee:deduplicated]"
          "[transition:update]") {
    const auto copy = node([](In<TS<i64>> input,
                              Out<TS<i64, TimeSeriesGuarantee::deduplicated>> output) { output.copy(input); });

    const auto graph = [&](wp<TS<i64>> input) { return copy(input); };

    const auto actual = bg::run_graph(graph, std::array{1, 1, 2});

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
