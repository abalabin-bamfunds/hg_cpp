#include <bg/graph.hpp>
#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/library.hpp>
#include <bg/node.hpp>
#include <bg/scalar/cpp_runtime_type.hpp>
#include <bg/scalar/opaque.hpp>
#include <bg/scalar/type.hpp>
#include <bg/time_series/tsd/tsd_type.hpp>
#include <bg/time_series/tsl/tsl.hpp>
#include <bg/time_series/tss/tss_type.hpp>
#include <bg/testing/test_graph.hpp>
#include <bg/type/time_series_value_type.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <optional>
#include <string>

using namespace bg;

namespace stream_test {
struct non_comparable_value {
    i64 value;
};
} // namespace stream_test

namespace {

struct delayed_value_source {
    static constexpr bool uses_timed_scheduling = true;

    Out<TS<i64>> output;

    void start(EvaluationContext *context) const {
        context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
    }

    void evaluate() const { output.set(1); }
};

struct changing_item_source {
    static constexpr bool uses_timed_scheduling = true;

    State<int> step;
    Out<TS<i64>> output;

    void start(EvaluationContext *context) const {
        output.set(1);
        context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
    }

    void evaluate(EvaluationContext *context) const {
        if ((*step)++ == 0) {
            output.invalidate();
            context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
            return;
        }

        output.set(2);
    }
};

struct empty_dictionary_source {
    Out<TSD<i64, TS<i64>>> output;

    void start() const { output.clear(); }
};

struct changing_set_source {
    static constexpr bool uses_timed_scheduling = true;

    State<int> step;
    Out<TSS<i64>> output;

    void start(EvaluationContext *context) const {
        output.clear();
        context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
    }

    void evaluate(EvaluationContext *context) const {
        if ((*step)++ == 0) {
            output.insert(1);
            output.insert(2);
            context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
            return;
        }

        output.erase(2);
    }
};

struct deduplicated_source {
    Out<TS<i64, TimeSeriesGuarantee::deduplicated>> output;

    void start() const { output.set(1); }
};

} // namespace

TEST_CASE("valid publishes the input validity", "[feature:operator][feature:validity][validity:optional][series:value]") {
    const auto source = node<delayed_value_source>();
    const auto query = [&](wp<TS<i64>> value) { return operators::valid(value); };
    const auto graph = [&] { return query(source()); };

    CHECK(testing::test_graph(graph).equals<bool>({false, true}));
}

TEST_CASE("valid observes target validity through a reference",
          "[feature:operator][feature:validity][series:reference][binding:reference-to-value]"
          "[transition:invalidation][transition:revalidation]") {
    const auto source = node<changing_item_source>();
    const auto reference = node([](In<REF<TS<i64>>> input, Out<REF<TS<i64>>> output) { output.copy(input); });
    const auto graph = [&] { return operators::valid(reference(source())); };

    CHECK(testing::test_graph(graph).equals<bool>({true, false, true}));
}

TEST_CASE("all_valid checks every item in a fixed list",
          "[feature:operator][feature:validity][validity:all-valid][series:fixed-tsl]") {
    const auto query = [](wp<TS<i64>> first, wp<TS<i64>> second) {
        return operators::all_valid(tsl(first, second));
    };

    CHECK(testing::test_graph(query, std::array<i64, 2>{1, 2}, std::array<i64, 2>{3, 4})
              .equals<bool>({true, std::nullopt}));
}

TEST_CASE("all_valid tracks immediate item invalidation in a free list",
          "[feature:operator][feature:validity][validity:all-valid][series:fixed-tsl][representation:free]"
          "[transition:invalidation][transition:revalidation]") {
    const auto changing_item = node<changing_item_source>();
    const auto graph = [&](wp<TS<i64>> first) { return operators::all_valid(tsl(first, changing_item())); };

    CHECK(testing::test_graph(graph, std::array<i64, 1>{10}).equals<bool>({true, false, true}));
}

TEST_CASE("all_valid treats a valid empty dictionary as valid",
          "[feature:operator][feature:validity][validity:all-valid][series:tsd]") {
    const auto source = node<empty_dictionary_source>();
    const auto query = [](wp<TSD<i64, TS<i64>>> value) { return operators::all_valid(value); };
    const auto graph = [&] { return query(source()); };

    CHECK(testing::test_graph(graph).equals<bool>({true}));
}

TEST_CASE("sample copies the current value on every signal tick",
          "[feature:operator][series:value][shape:chain]") {
    const auto sample = [](wp<TS<i64>> signal, wp<TS<i64>> value) { return operators::sample(signal, value); };

    CHECK(testing::test_graph(sample, std::array<i64, 3>{0, 1, 2}, std::array<i64, 3>{10, 20, 30})
              .equals<i64>({10, 20, 30}));
}

TEST_CASE("sample uses the latest passive value and preserves output on invalidation",
          "[feature:operator][feature:validity][series:value][activity:passive]"
          "[transition:invalidation][transition:revalidation]") {
    const auto invalidate_on_negative = node([](In<TS<i64>> input, Out<TS<i64>> output) {
        if (*input < 0) {
            output.invalidate();
            return;
        }
        output.set(*input);
    });
    const auto sample = [&](wp<TS<i64>> signal, wp<TS<i64>> value) {
        return operators::sample(signal, invalidate_on_negative(value));
    };

    CHECK(testing::test_graph(sample, std::array<i64, 4>{0, 1, 2, 3}, std::array<i64, 4>{10, -1, 20, 30})
              .equals<i64>({10, std::nullopt, 20, 30}));

    const auto retain_passive_value = [](wp<TS<i64>> signal, wp<TS<i64>> value) {
        return operators::sample(signal, value);
    };
    const auto sparse_signal = std::array<std::optional<i64>, 5>{0, std::nullopt, 2, std::nullopt, 4};
    const auto sparse_value = std::array<std::optional<i64>, 5>{10, 20, std::nullopt, 30, std::nullopt};
    CHECK(testing::test_graph(retain_passive_value, sparse_signal, sparse_value)
              .equals<i64>({10, std::nullopt, 20, std::nullopt, 30}));
}

TEST_CASE("sample observes a reference target only on signal ticks",
          "[feature:operator][feature:sampling][series:reference][activity:passive]"
          "[binding:reference-to-value][transition:invalidation][transition:revalidation]") {
    const auto source = node<changing_item_source>();
    const auto reference = node([](In<REF<TS<i64>>> input, Out<REF<TS<i64>>> output) { output.copy(input); });
    const auto sample = [&](wp<TS<i64>> signal) {
        return operators::sample(signal, reference(source()));
    };

    CHECK(testing::test_graph(sample, std::array<i64, 3>{0, 1, 2}).equals<i64>({1, std::nullopt, 2}));
}

TEST_CASE("sample limits stronger input guarantees to ordinary",
          "[feature:operator][feature:type-system][series:value][guarantee:const]") {
    const auto graph = [](wp<TS<i64>> signal) {
        const auto result = operators::sample(signal, operators::const_(i64{42}));
        CHECK(&result.type() == &TS<i64, TimeSeriesGuarantee::ordinary>::type());
        return result;
    };

    CHECK(testing::test_graph(graph, std::array<i64, 2>{0, 1}).equals<i64>({42, 42}));
}

TEST_CASE("sample republishes const list items on every signal tick",
          "[feature:operator][series:fixed-tsl][guarantee:const][transition:update]") {
    const auto sum = node([](In<TSL<TS<i64>, 2>, all_valid> values, Out<TS<i64>> output) {
        output.set(*values[0] + *values[1]);
    });
    const auto graph = [&](wp<TS<i64>> signal) {
        const auto values = tsl(operators::const_(i64{10}), operators::const_(i64{20}));
        return sum(operators::sample(signal, values));
    };

    CHECK(testing::test_graph(graph, std::array<i64, 2>{0, 1}).equals<i64>({30, 30}));
}

TEST_CASE("sample copies set removals and does not publish unchanged membership",
          "[feature:operator][feature:collection-change][series:tss][activity:passive]"
          "[transition:removal]") {
    const auto set = node<changing_set_source>();
    const auto inspect = node([](In<TSS<i64>> input, Out<TS<i64>> output) {
        output.set(static_cast<i64>(input.size() * 10 + input.contains(2)));
    });
    const auto graph = [&](wp<TS<i64>> signal) { return inspect(operators::sample(signal, set())); };

    CHECK(testing::test_graph(graph, std::array<i64, 4>{0, 1, 2, 3})
              .equals<i64>({0, 21, 10, std::nullopt}));
}

TEST_CASE("dedup suppresses consecutive equal values",
          "[feature:operator][feature:type-system][series:value][guarantee:deduplicated]") {
    const auto deduplicate = [](wp<TS<i64>> value) { return operators::dedup(value); };

    CHECK(testing::test_graph(deduplicate, std::array<i64, 4>{1, 1, 2, 2})
              .equals<i64>({1, std::nullopt, 2, std::nullopt}));
}

TEST_CASE("dedup publishes the same value after invalidation",
          "[feature:operator][feature:validity][series:value][guarantee:deduplicated]"
          "[transition:invalidation][transition:revalidation]") {
    const auto invalidate = node([](In<TS<i64>> value, Out<TS<i64>> output) {
        if (*value < 0) {
            output.invalidate();
            return;
        }
        output.copy(value);
    });
    const auto graph = [&](wp<TS<i64>> value) { return operators::dedup(invalidate(value)); };

    CHECK(testing::test_graph(graph, std::array<i64, 3>{1, -1, 1}).equals<i64>({1, std::nullopt, 1}));
}

TEST_CASE("dedup leaves an already deduplicated input as the same graph node",
          "[feature:operator][feature:type-system][series:value][guarantee:deduplicated]"
          "[shape:chain]") {
    const auto source = node<deduplicated_source>();
    const auto copy = node([](In<TS<i64>> input, Out<TS<i64>> output) { output.copy(input); });
    const auto graph = [&] { return copy(operators::dedup(source())); };
    auto wiring = GraphWiring{};
    {
        auto context = WiringContext{wiring};
        graph();
    }
    wiring.finalize();
    const auto compiled = CompiledGraph{wiring};

    CHECK(compiled.node_count() == 2);
    CHECK(testing::test_graph(graph).equals<i64>({1}));
}

TEST_CASE("dedup on a reference observes retargeted values",
          "[feature:operator][feature:reference][feature:type-system][series:reference]"
          "[guarantee:deduplicated][binding:reference-to-value][transition:update]") {
    const auto first = node([](Out<TS<i64, TimeSeriesGuarantee::deduplicated>> output) { output.set(1); });
    const auto second = node([](Out<TS<i64, TimeSeriesGuarantee::deduplicated>> output) { output.set(2); });
    const auto select = node([](In<TS<i64>> command, In<REF<TS<i64, TimeSeriesGuarantee::deduplicated>>, passive> first,
                                In<REF<TS<i64, TimeSeriesGuarantee::deduplicated>>, passive> second,
                                Out<REF<TS<i64, TimeSeriesGuarantee::deduplicated>>> output) {
        output.copy(*command == 0 ? first : second);
    });
    const auto copy = node([](In<TS<i64>> input, Out<TS<i64>> output) { output.copy(input); });
    const auto graph = [&](wp<TS<i64>> command) { return copy(operators::dedup(select(command, first(), second()))); };

    CHECK(testing::test_graph(graph, std::array<i64, 4>{0, 0, 1, 1})
              .equals<i64>({1, std::nullopt, 2, std::nullopt}));
}

TEST_CASE("dedup rejects scalar types without equality",
          "[feature:operator][feature:type-system][series:value][scalar:opaque]") {
    using OPAQUE_SERIES = TimeSeriesVariable<"OPAQUE_SERIES">;
    using VALUE = Opaque<stream_test::non_comparable_value>;
    (void)cpp_scalar_runtime_type<VALUE>();
    const auto source = node(
        eval = [](Out<OPAQUE_SERIES>) {},
        resolve<OPAQUE_SERIES> = []() -> const TimeSeriesType & {
            return time_series_value_type(cpp_scalar_type<VALUE>(), TimeSeriesGuarantee::ordinary);
        });
    auto wiring = GraphWiring{};

    try {
        auto context = WiringContext{wiring};
        static_cast<void>(operators::dedup(source()));
        FAIL("expected dedup to reject a scalar type without equality");
    } catch (const WiringError &error) {
        REQUIRE(error.diagnostic());
        CHECK(std::ranges::any_of(error.diagnostic()->candidates, [](const auto &candidate) {
            return candidate.message.find("requires a scalar type that supports equality") != std::string::npos;
        }));
    }
}

TEST_CASE("dedup returns final and const opaque inputs without an equality node",
          "[feature:operator][feature:type-system][series:value][scalar:opaque]"
          "[guarantee:final][guarantee:const][shape:single-node]") {
    using OPAQUE_SERIES = TimeSeriesVariable<"OPAQUE_SERIES">;
    using VALUE = Opaque<stream_test::non_comparable_value>;
    (void)cpp_scalar_runtime_type<VALUE>();

    const auto make_source = [](TimeSeriesGuarantee guarantee) {
        return node(
            eval = [](Out<OPAQUE_SERIES>) {},
            resolve<OPAQUE_SERIES> = [guarantee]() -> const TimeSeriesType & {
                return time_series_value_type(cpp_scalar_type<VALUE>(), guarantee);
            });
    };
    const auto check_unchanged = [&](TimeSeriesGuarantee guarantee) {
        const auto source = make_source(guarantee);
        auto wiring = GraphWiring{};
        const TimeSeriesType *result_type;
        {
            auto context = WiringContext{wiring};
            result_type = &operators::dedup(source()).type();
        }
        wiring.finalize();

        CHECK(result_type == &time_series_value_type(cpp_scalar_type<VALUE>(), guarantee));
        CHECK(wiring.nodes().size() == 1);
    };

    check_unchanged(TimeSeriesGuarantee::final);
    check_unchanged(TimeSeriesGuarantee::const_);
}
