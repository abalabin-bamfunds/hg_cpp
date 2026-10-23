#include <bg/library.hpp>
#include <bg/map.hpp>
#include <bg/node.hpp>
#include <bg/switch.hpp>
#include <bg/evaluation_context.hpp>
#include <bg/graph/compiled_graph.hpp>
#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/runtime_graph.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/run_graph.hpp>
#include <bg/testing/test_wiring.hpp>
#include <bg/time_series/tsb/tsb.hpp>
#include <bg/time_series/ref/ref_provider.hpp>
#include <bg/time_series/tsd/tsd_type.hpp>
#include <bg/time_series/tsl/tsl.hpp>

#include "../utils/counting_memory_resource.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

using namespace bg;
using namespace bg::literals;

TEST_CASE("len_ of a TSS follows key membership", "[feature:operator][series:tss][scalar:i64][shape:single-node]") {
    const auto graph = [](wp<TSS<i64>> value) { return operators::len_("value"_n = value); };

    CHECK(bg::run_graph(graph, {{1}, {2, literal::REMOVED(1)}}).equals<i64>({1, 1}));
}

TEST_CASE("keys_where_true follows Boolean dictionary items",
          "[feature:operator][series:tsd][series:tss][scalar:boolean][transition:update][transition:removal]") {
    const auto graph = [](wp<TSD<i64, TS<bool>>> mask) { return operators::keys_where_true(mask); };

    const auto actual = bg::run_graph(
        graph, {{{1, true}, {2, false}}, {{1, false}, {2, true}}, {{1, literal::INVALID}},
                {{2, literal::REMOVE}}, {{1, true}}});

    CHECK(actual.equals({{1}, {2, literal::REMOVED(1)}, literal::SKIP, {literal::REMOVED(2)}, {1}}));
}

TEST_CASE("keys_ projects dictionary membership without copying item updates",
          "[feature:operator][series:tsd][series:tss][transition:addition][transition:removal]") {
    const auto graph = [](wp<TSD<i64, TS<i64>>> values) { return operators::keys_(values); };

    const auto input = bg::input<TSD<i64, TS<i64>>>(
        {{}, {{1, 10}, {2, 20}}, {{1, 11}}, {{3, 30}, {2, literal::REMOVE}},
         {{1, literal::REMOVE}, {3, literal::REMOVE}}});
    const auto actual = bg::run_graph(graph, input);

    CHECK(actual.equals({{}, {1, 2}, literal::SKIP, {3, literal::REMOVED(2)},
                         {literal::REMOVED(1), literal::REMOVED(3)}}));
}

TEST_CASE("keys_ projects an ordinary dictionary without adding a runtime node",
          "[feature:operator][feature:graph-compilation][series:tsd][series:tss]") {
    const auto dictionary = node([](Out<TSD<i64, TS<i64>>> output) { output[1].set(1); });
    auto wiring = GraphWiring{};
    auto result = WiringPoint{};
    {
        auto context = WiringContext{wiring};
        result = operators::keys_(dictionary());
    }

    result = wiring.complete_pending_wiring(result);
    wiring.finalize();
    const auto compiled = CompiledGraph{wiring};

    CHECK(compiled.node_count() == 1);
}

TEST_CASE("keys_ follows a selected dictionary reference through switching and invalidation",
          "[feature:operator][series:tsd][series:tss][series:reference][transition:invalidation]"
          "[transition:revalidation]") {
    const auto graph = [](wp<TS<bool>> condition, wp<TSD<i64, TS<i64>>> left,
                          wp<TSD<i64, TS<i64>>> right) {
        return operators::keys_(operators::if_then_else(condition, left, right));
    };
    const auto valid_graph = [](wp<TS<bool>> condition, wp<TSD<i64, TS<i64>>> left,
                                wp<TSD<i64, TS<i64>>> right) {
        return operators::valid(operators::keys_(operators::if_then_else(condition, left, right)));
    };

    const auto actual = bg::run_graph(
        graph, bg::input<TS<bool>>({true, true, false, literal::INVALID, true}),
        bg::input<TSD<i64, TS<i64>>>({{}, {{1, 10}, {2, 20}}, literal::SKIP, literal::SKIP, literal::SKIP}),
        bg::input<TSD<i64, TS<i64>>>({{}, literal::SKIP, {{8, 80}}, literal::SKIP, literal::SKIP}));
    const auto validity = bg::run_graph(
        valid_graph, bg::input<TS<bool>>({true, true, false, literal::INVALID, true}),
        bg::input<TSD<i64, TS<i64>>>({{}, {{1, 10}, {2, 20}}, literal::SKIP, literal::SKIP, literal::SKIP}),
        bg::input<TSD<i64, TS<i64>>>({{}, literal::SKIP, {{8, 80}}, literal::SKIP, literal::SKIP}));

    CHECK(actual.equals({{}, {1, 2}, {8, literal::REMOVED(1), literal::REMOVED(2)},
                         {literal::REMOVED(8)}, {1, 2}}));
    CHECK(validity.equals<bool>({true, std::nullopt, std::nullopt, false, true}));
}

TEST_CASE("keys_ follows an initially invalid selected dictionary as it becomes valid",
          "[feature:operator][series:tsd][series:tss][series:reference][transition:invalidation]"
          "[transition:revalidation]") {
    const auto graph = [](wp<TS<bool>> condition, wp<TSD<i64, TS<i64>>> left,
                          wp<TSD<i64, TS<i64>>> right) {
        return operators::keys_(operators::if_then_else(condition, left, right));
    };
    const auto valid_graph = [](wp<TS<bool>> condition, wp<TSD<i64, TS<i64>>> left,
                                wp<TSD<i64, TS<i64>>> right) {
        return operators::valid(operators::keys_(operators::if_then_else(condition, left, right)));
    };

    const auto actual = bg::run_graph(
        graph, bg::input<TS<bool>>({true, false, false, false}),
        bg::input<TSD<i64, TS<i64>>>({{{1, 10}}, literal::SKIP, literal::SKIP, literal::SKIP}),
        bg::input<TSD<i64, TS<i64>>>({literal::SKIP, literal::SKIP, {}, {{8, 80}}}));
    const auto validity = bg::run_graph(
        valid_graph, bg::input<TS<bool>>({true, false, false, false}),
        bg::input<TSD<i64, TS<i64>>>({{{1, 10}}, literal::SKIP, literal::SKIP, literal::SKIP}),
        bg::input<TSD<i64, TS<i64>>>({literal::SKIP, literal::SKIP, {}, {{8, 80}}}));

    CHECK(actual.equals({{1}, {literal::REMOVED(1)}, {}, {8}}));
    CHECK(validity.equals<bool>({true, false, true, std::nullopt}));
}

TEST_CASE("keys_ preserves the dictionary key-set guarantee",
          "[feature:operator][series:tsd][series:tss][guarantee:final]") {
    const auto final_dictionary = node([](Out<TSD<i64, TS<i64>, TimeSeriesGuarantee::final>> output) {
        output[1].set(1);
    });

    const auto graph = [&] {
        const auto keys = operators::keys_(final_dictionary());
        CHECK(&keys.type() == &TSS<i64, TimeSeriesGuarantee::final>::type());
        return keys;
    };

    CHECK(bg::run_graph(graph).equals({{1}}));
}

TEST_CASE("keys_where_true follows invalidation and revalidation of a referenced mask item",
          "[feature:operator][series:tsd][series:tss][series:reference][transition:invalidation]"
          "[transition:revalidation]") {
    const auto make_mask = node([](In<REF<TS<bool>>> first, In<REF<TS<bool>>> second,
                                   Out<TSD<i64, REF<TS<bool>>>> output) {
        output[1].copy(first);
        output[2].copy(second);
    });
    const auto graph = [&](wp<TS<bool>> first, wp<TS<bool>> second) {
        return operators::keys_where_true(make_mask(first, second));
    };

    const auto actual = bg::run_graph(graph, bg::input<TS<bool>>({true, literal::INVALID, true}),
                                      bg::input<TS<bool>>({false, false, false}));

    CHECK(actual.equals({{1}, {literal::REMOVED(1)}, {1}}));
}

TEST_CASE("keys_where_true clears its result when the mask reference becomes invalid",
          "[feature:operator][series:tsd][series:tss][series:reference][transition:invalidation]"
          "[transition:revalidation]") {
    const auto invalid_mask = node([](Out<TSD<i64, TS<bool>>>) {});
    const auto select_mask = node([](In<TS<bool>> choose_valid, In<REF<TSD<i64, TS<bool>>>, passive> valid,
                                     In<REF<TSD<i64, TS<bool>>>, passive> invalid,
                                     Out<REF<TSD<i64, TS<bool>>>> output) {
        output.copy(*choose_valid ? valid : invalid);
    });
    const auto graph = [&](wp<TS<bool>> choose_valid, wp<TSD<i64, TS<bool>>> mask) {
        return operators::keys_where_true(select_mask(choose_valid, mask, invalid_mask()));
    };

    const auto actual = bg::run_graph(
        graph, bg::input<TS<bool>>({false, true, false, true}),
        bg::input<TSD<i64, TS<bool>>>({{{1, true}}, literal::SKIP, literal::SKIP, literal::SKIP}));

    CHECK(actual.equals({literal::SKIP, {1}, {literal::REMOVED(1)}, {1}}));
}

namespace {

template <template <typename, auto...> typename Field> struct length_bundle {
    Field<TS<i64>> first;
    Field<TS<double>> second;
};

const auto final_keys = node([](Out<TSS<i64, TimeSeriesGuarantee::final>> output) { output.apply({1, 2}); });

template <bool ValidItems, typename TS_T = TS<i64>> struct tsd_membership_evaluation_count_definition {
    static constexpr bool uses_cycle_callbacks = true;

    In<REF<TSD<i64, TS_T>>> collection;
    In<TS<bool>, optional, dynamic_binding> membership;
    Out<TS<bool>> output;

    void evaluate() const {
        ++evaluation_count;
        const auto key_value = i64{7};
        const auto key = Scalar<i64>::from(key_value);
        if constexpr (ValidItems) {
            membership.bind(collection.valid_item_membership(key));
        } else {
            membership.bind(collection.membership(key));
        }
        output.copy(membership);
    }

    void on_invalidation() const {
        membership.unbind();
        output.invalidate();
    }

    inline static std::size_t evaluation_count{};
};

struct mixed_dynamic_input_definition {
    In<REF<TSD<i64, TS<i64>>>> values;
    In<REF<TSS<i64>>> keys;
    In<TS<i64>> key;
    Scalar<SCALAR> seed;
    State<SCALAR> state;
    In<REF<TS<i64>>, dynamic_binding, optional> selected_value;
    In<TS<bool>, optional, dynamic_binding> selected_membership;
    Out<TS<bool>> output;

    void evaluate() const {
        state.set(seed);
        selected_value.bind(values[key.value_view()]);
        selected_membership.bind(keys.membership(key.value_view()));
        output.set(selected_value.valid() && selected_membership.valid() && *selected_membership);
    }

    void on_invalidation() const {
        selected_value.unbind();
        selected_membership.unbind();
        output.invalidate();
    }
};

class FailedDynamicValueBindingType final : public details::DynamicValueBindingType {
  public:
    TimeSeriesInputData input_data(const void *, EvaluationTime) const noexcept override { return {}; }

    void subscribe(void *, RuntimeGraph &, std::size_t, std::uint32_t) const override {
        throw std::runtime_error{"subscription failed"};
    }

    void unsubscribe(void *, RuntimeGraph &, std::size_t) const noexcept override {}
};

struct failed_membership_rebind_definition {
    static constexpr bool uses_cycle_callbacks = true;

    In<REF<TSD<i64, TS<i64>>>> collection;
    In<TS<bool>, optional, dynamic_binding> membership;
    Out<TS<bool>> output;

    void evaluate() const {
        const auto key = i64{7};
        membership.bind(collection.membership(Scalar<i64>::from(key)));
        try {
            static const auto failed_binding_type = FailedDynamicValueBindingType{};
            static int failed_binding_data{};
            membership.bind(details::DynamicValueBinding<bool>{&failed_binding_data, &failed_binding_type});
        } catch (const std::runtime_error &) {
        }
        output.copy(membership);
    }
};

template <template <typename, auto...> typename Field> struct absent_membership_churn_inputs {
    Field<TSD<i64, TS<i64>>> collection;
    Field<TS<i64>> key;
    Field<TS<i64>> unused_key;
};

struct absent_membership_churn_source {
    static constexpr bool schedule_on_start = true;
    static constexpr bool uses_timed_scheduling = true;

    Out<TSB<absent_membership_churn_inputs>> output;

    void evaluate(EvaluationContext *context) const {
        if (cycle == 0) {
            output->collection[0].set(0);
        }
        output->key.set(static_cast<i64>(100 + cycle));
        output->unused_key.set(static_cast<i64>(100'000 + cycle));
        ++cycle;
        context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
    }

    static inline std::size_t cycle{};
};

struct unused_membership_descriptor_definition {
    static constexpr bool uses_cycle_callbacks = true;

    In<REF<TSD<i64, TS<i64>>>> collection;
    In<TS<i64>> key;
    Out<TS<bool>> output;

    void evaluate() const {
        const auto binding = collection.membership(key.value_view());
        output.set(binding.data != nullptr);
    }
};

} // namespace

TEST_CASE("len_ of a fixed TSL is a wiring-time constant",
          "[feature:operator][series:fixed-tsl][scalar:i64][shape:single-node][guarantee:const]") {
    const auto graph = [] {
        const auto values = std::array{operators::const_(i64{1}), operators::const_(i64{2}), operators::const_(i64{3})};
        return operators::len_("value"_n = tsl(values));
    };

    CHECK(bg::run_graph(graph).equals<i64>({3}));

    auto wiring = GraphWiring{};
    auto result = WiringPoint{};
    {
        auto context = WiringContext{wiring};
        result = graph();
    }
    result = wiring.complete_pending_wiring(result);
    wiring.finalize();
    const auto compiled = CompiledGraph{wiring};
    CHECK(compiled.node_count() == 0);
}

TEST_CASE("len_ rejects a TSL with sparse item metadata",
          "[feature:operator][feature:wiring][series:fixed-tsl][scalar:i64][validity:required]") {
    testing::test_wiring([] {
        auto &wiring = WiringContext::current().wiring();
        const auto &sparse_type = time_series_list_type(TS<i64>::type(), std::dynamic_extent,
                                                        TimeSeriesGuarantee::ordinary);
        const auto sparse_input = wiring.borrowed_input(sparse_type);

        CHECK_THROWS_AS(operators::len_("value"_n = sparse_input), WiringError);
    });
}

TEST_CASE("len_ of a fixed TSB is a wiring-time constant",
          "[feature:operator][series:tsb][schema:named][scalar:i64][shape:single-node][guarantee:const]") {
    const auto graph = [] {
        const auto values = bundle<TSB<length_bundle>>(i64{1}, 2.0);
        return operators::len_("value"_n = values);
    };

    CHECK(bg::run_graph(graph).equals<i64>({2}));
}

TEST_CASE("len_ does not turn an invalid TSS into a valid empty set",
          "[feature:operator][series:tss][scalar:i64][validity:required][transition:first-tick]") {
    const auto graph = [](wp<TSS<i64>> values) { return operators::len_("value"_n = values); };

    CHECK(bg::run_graph(graph, {literal::SKIP, {}}).equals<i64>({std::nullopt, 0}));
}

TEST_CASE("contains_ on a TSS queries the current key set",
          "[feature:operator][series:tss][scalar:i64][shape:single-node][guarantee:ordinary]") {
    const auto graph = [](wp<TSS<i64>> collection) {
        return operators::contains_("collection"_n = collection, "key"_n = i64{1});
    };

    CHECK(bg::run_graph(graph, {{1}, {2, literal::REMOVED(1)}, {1}}).equals<bool>({true, false, true}));
}

TEST_CASE("contains_ distinguishes an invalid TSS from a valid empty TSS",
          "[feature:operator][series:tss][scalar:i64][validity:required][transition:first-tick]") {
    const auto graph = [](wp<TSS<i64>> collection) {
        return operators::contains_("collection"_n = collection, "key"_n = i64{1});
    };

    CHECK(bg::run_graph(graph, {literal::SKIP, {}}).equals<bool>({std::nullopt, false}));
}

TEST_CASE("contains_ on a TSS accepts a time-series key",
          "[feature:operator][series:tss][scalar:i64][shape:single-node][guarantee:ordinary]") {
    const auto graph = [](wp<TSS<i64>> collection, wp<TS<i64>> key) {
        return operators::contains_("collection"_n = collection, "key"_n = key);
    };

    CHECK(bg::run_graph(graph, {{1}, {2}}, {1, 2}).equals<bool>({true, true}));
}

TEST_CASE("contains_ on a TSD distinguishes key presence from valid item presence",
          "[feature:operator][series:tsd][scalar:i64][input-guarantee:valid-items][transition:invalidation]"
          "[transition:revalidation]") {
    const auto physical = [](wp<TSD<i64, TS<i64>>> collection) {
        return operators::contains_("collection"_n = collection, "key"_n = i64{7});
    };
    const auto valid_item_contains = [](wp<TSD<i64, TS<i64>>> collection) {
        return operators::contains_("collection"_n = valid_items(collection), "key"_n = i64{7});
    };
    CHECK(bg::run_graph(physical, {{{7, 70}}, {{7, literal::INVALID}}, {{7, 71}}})
              .equals<bool>({true, std::nullopt, std::nullopt}));
    CHECK(bg::run_graph(valid_item_contains, {{{7, 70}}, {{7, literal::INVALID}}, {{7, 71}}})
              .equals<bool>({true, false, true}));
}

TEST_CASE("valid-item TSD membership uses REF item validity, not target validity",
          "[feature:operator][series:tsd][series:reference][input-guarantee:valid-items]"
          "[transition:invalidation][transition:revalidation]") {
    const auto make_dictionary = node([](In<TS<i64>> command, In<REF<TS<i64>>> target,
                                         Out<TSD<i64, REF<TS<i64>>>> output) {
        if (*command == 0) {
            output[7].copy(target);
        } else if (*command == 1) {
            output[7].invalidate();
        } else if (*command == 2) {
            output[7].copy(target);
        }
    });
    const auto run = [&]<bool ValidItems>() {
        using Definition = tsd_membership_evaluation_count_definition<ValidItems, REF<TS<i64>>>;
        const auto observe = node<Definition>();

        const auto graph = [&](wp<TS<i64>> target_value, wp<TS<i64>> command) {
            return observe(make_dictionary(command, target_value));
        };
        Definition::evaluation_count = 0;
        auto actual = bg::run_graph(
            graph,
            bg::input<TS<i64>>({literal::INVALID, i64{10}, i64{20}, literal::INVALID, literal::INVALID, i64{30}}),
            bg::input<TS<i64>>({0, 3, 3, 1, 2, 3}));
        return std::pair{std::move(actual), Definition::evaluation_count};
    };

    const auto [physical, physical_evaluation_count] = run.template operator()<false>();
    CHECK(physical.equals<bool>({true, std::nullopt, std::nullopt, std::nullopt, std::nullopt, std::nullopt}));
    CHECK(physical_evaluation_count == 1);

    const auto [valid_items, valid_items_evaluation_count] = run.template operator()<true>();
    CHECK(valid_items.equals<bool>({true, std::nullopt, std::nullopt, false, true, std::nullopt}));
    CHECK(valid_items_evaluation_count == 3);
}

TEST_CASE("an aggregate can bind a dynamic reference and dynamic value input together",
          "[feature:reference][series:reference][series:value][binding:reference-to-value]"
          "[shape:multiple][definition:aggregate][transition:update]") {
    const auto observe = node<mixed_dynamic_input_definition>();

    const auto graph = [&](wp<TSD<i64, TS<i64>>> values, wp<TSS<i64>> keys, wp<TS<i64>> key) {
        return observe(values, keys, key, i64{0});
    };

    const auto actual = bg::run_graph(graph, {{{1, 10}}, {{1, 10}}, {{1, 10}}},
                                            {{1}, {literal::REMOVED(1)}, {1}}, {1, 1, 1});

    CHECK(actual.equals<bool>({true, false, true}));
}

TEST_CASE("a TSD membership binding ignores unrelated keys and item updates",
          "[evaluation][series:tsd][series:reference][feature:collection-change]"
          "[binding:reference-to-value][transition:addition][transition:invalidation][transition:revalidation]") {
    using Definition = tsd_membership_evaluation_count_definition<false>;
    const auto observe = node<Definition>();

    const auto graph = [&](wp<TSD<i64, TS<i64>>> input) { return observe(input); };
    Definition::evaluation_count = 0;

    const auto actual = bg::run_graph(
        graph, {{{7, 70}}, {{8, 80}}, {{7, literal::INVALID}}, {{7, literal::REMOVE}}, {{7, 71}}, {{8, 81}}});

    CHECK(actual.equals<bool>({true, std::nullopt, std::nullopt, false, true, std::nullopt}));
    CHECK(Definition::evaluation_count == 3);
}

namespace {

struct shared_tsd_membership_definition {
    static constexpr bool uses_cycle_callbacks = true;

    In<REF<TSD<i64, TS<i64>>>> collection;
    In<TS<i64>> key;
    In<TS<bool>> first_active;
    In<TS<bool>> second_active;
    In<TS<bool>, optional, dynamic_binding> first_membership;
    In<TS<bool>, optional, dynamic_binding> second_membership;
    Out<TS<bool>> output;

    void evaluate() const {
        if (*first_active) {
            first_membership.bind(collection.membership(key.value_view()));
        } else {
            first_membership.unbind();
        }
        if (*second_active) {
            second_membership.bind(collection.membership(key.value_view()));
        } else {
            second_membership.unbind();
        }

        const auto first_contains_key = *first_active && first_membership.valid() && *first_membership;
        const auto second_contains_key = *second_active && second_membership.valid() && *second_membership;
        output.set(first_contains_key || second_contains_key);
    }

    void on_invalidation() const {
        first_membership.unbind();
        second_membership.unbind();
        output.invalidate();
    }
};

} // namespace

TEST_CASE("TSD membership supports shared consumers and release followed by rebinding",
          "[evaluation][series:tsd][series:reference][feature:collection-change]"
          "[shape:multiple][definition:aggregate][transition:removal][transition:addition]") {
    const auto observe = node<shared_tsd_membership_definition>();

    const auto graph = [&](wp<TSD<i64, TS<i64>>> collection, wp<TS<i64>> key, wp<TS<bool>> first_active,
                           wp<TS<bool>> second_active) {
        return observe(collection, key, first_active, second_active);
    };

    const auto actual = bg::run_graph(graph,
                                            {{{7, 70}}, {{7, 70}}, {{7, 70}}, {{7, literal::REMOVE}},
                                             literal::SKIP, literal::SKIP, {{7, 71}}},
                                            {7, 7, 7, 7, 7, 7, 7},
                                            {true, false, false, false, false, false, false},
                                            {true, true, false, false, false, true, true});

    CHECK(actual.equals<bool>({true, true, false, false, false, false, true}));
}

TEST_CASE("a failed dynamic membership subscription preserves the previous binding",
          "[evaluation][series:tsd][series:reference][feature:collection-change]"
          "[shape:single-node][definition:aggregate][transition:update]") {
    const auto observe = node<failed_membership_rebind_definition>();

    const auto graph = [&](wp<TSD<i64, TS<i64>>> collection) { return observe(collection); };

    CHECK(bg::run_graph(graph, {{{7, 70}}, {{7, literal::REMOVE}}}).equals<bool>({true, false}));
}

TEST_CASE("absent TSD membership key churn releases unused descriptors while the graph is alive",
          "[evaluation][series:tsd][series:reference][feature:collection-change][allocation]"
          "[shape:multiple][transition:update]") {
    absent_membership_churn_source::cycle = 0;
    const auto source = node<absent_membership_churn_source>();

    const auto inspect_unused = node<unused_membership_descriptor_definition>();

    const auto sink = node([](bool selected, bool unused) { return selected || unused; });

    auto wiring = GraphWiring{};
    {
        auto context = WiringContext{wiring};
        const auto inputs = source();
        const auto selected = operators::contains_("collection"_n = inputs->collection, "key"_n = inputs->key);
        const auto unused = inspect_unused(inputs->collection, inputs->unused_key);
        (void)sink(selected, unused);
    }
    wiring.finalize();
    const auto compiled = CompiledGraph{wiring};

    auto memory = counting_memory_resource{};
    auto global_state = GlobalState{allocator{memory}};
    const auto allocations_before_runtime = memory.allocations();
    {
        auto runtime = RuntimeGraph{compiled, allocator{memory}, global_state};
        auto time = MIN_DT + std::chrono::microseconds{1};
        runtime.start(time);
        for (auto cycle = std::size_t{}; cycle < 20; ++cycle) {
            time += std::chrono::microseconds{1};
            runtime.evaluate(time);
        }

        const auto steady_allocations = memory.allocations();
        const auto steady_allocated_bytes = memory.allocated_bytes();
        auto maximum_allocations = steady_allocations;
        auto maximum_allocated_bytes = steady_allocated_bytes;
        for (auto cycle = std::size_t{}; cycle < 2'000; ++cycle) {
            time += std::chrono::microseconds{1};
            runtime.evaluate(time);
            maximum_allocations = std::max(maximum_allocations, memory.allocations());
            maximum_allocated_bytes = std::max(maximum_allocated_bytes, memory.allocated_bytes());
        }
        CHECK(maximum_allocations <= steady_allocations + 1);
        CHECK(maximum_allocated_bytes <= steady_allocated_bytes + 256);
        runtime.stop();
    }
    CHECK(memory.allocations() == allocations_before_runtime);
}

TEST_CASE("valid-item TSD membership observes selected item validity only",
          "[evaluation][series:tsd][series:reference][feature:collection-change][input-guarantee:valid-items]"
          "[binding:reference-to-value][transition:invalidation][transition:revalidation]") {
    using Definition = tsd_membership_evaluation_count_definition<true>;
    const auto observe = node<Definition>();

    const auto graph = [&](wp<TSD<i64, TS<i64>>> input) { return observe(input); };
    Definition::evaluation_count = 0;

    const auto actual = bg::run_graph(
        graph, {{{7, 70}}, {{8, 80}}, {{7, literal::INVALID}}, {{8, 81}}, {{7, 71}}});

    CHECK(actual.equals<bool>({true, std::nullopt, false, std::nullopt, true}));
    CHECK(Definition::evaluation_count == 3);
}

TEST_CASE("contains_ on a TSD handles an invalid and revalidated time-series key",
          "[feature:operator][series:tsd][series:value][scalar:i64][transition:invalidation]"
          "[transition:revalidation]") {
    const auto graph = [](wp<TSD<i64, TS<i64>>> collection, wp<TS<i64>> key) {
        return operators::contains_("collection"_n = collection, "key"_n = key);
    };

    const auto actual = bg::run_graph(graph, {{{7, 70}}, {{7, 70}}, {{7, 70}}}, {7, literal::INVALID, 7});

    CHECK(actual.equals({true, literal::INVALID, true}));
}

TEST_CASE("contains_ accepts an unchanged ordinary time-series key",
          "[feature:operator][series:tsd][series:value][scalar:i64][transition:update]") {
    const auto graph = [](wp<TSD<i64, TS<i64>>> collection, wp<TS<i64>> key) {
        return operators::contains_("collection"_n = collection, "key"_n = key);
    };

    const auto actual = bg::run_graph(graph, {{{7, 70}}, {{7, 70}}}, {7, 7});

    CHECK(actual.equals<bool>({true, true}));
}

TEST_CASE("contains_ follows a time-series key through absent-key churn",
          "[feature:operator][series:tsd][series:value][scalar:i64][transition:update]") {
    const auto graph = [](wp<TSD<i64, TS<i64>>> collection, wp<TS<i64>> key) {
        return operators::contains_("collection"_n = collection, "key"_n = key);
    };

    const auto actual = bg::run_graph(graph,
                                            {{{7, 70}}, literal::SKIP, literal::SKIP, literal::SKIP,
                                             literal::SKIP, literal::SKIP},
                                            {8, 9, 8, 7, 9, 7});

    CHECK(actual.equals<bool>({false, false, false, true, false, true}));
}

TEST_CASE("contains_ follows a selected TSD reference when membership is unchanged",
          "[feature:operator][series:tsd][series:reference][feature:collection-change]"
          "[binding:reference-to-value][transition:update][transition:removal]") {
    const auto graph = [](wp<TS<bool>> select_first, wp<TSD<i64, TS<i64>>> first,
                          wp<TSD<i64, TS<i64>>> second) {
        const auto selected = operators::if_then_else(select_first, first, second);
        return operators::contains_("collection"_n = selected, "key"_n = i64{7});
    };

    const auto actual = bg::run_graph(
        graph, {true, false, true, false, true, false, true},
        {{{7, 70}}, literal::SKIP, literal::SKIP, literal::SKIP, literal::SKIP, literal::SKIP,
         {{7, literal::REMOVE}}},
        {{{7, 70}}, literal::SKIP, literal::SKIP, literal::SKIP, literal::SKIP, literal::SKIP, literal::SKIP});

    CHECK(actual.equals<bool>({true, true, true, true, true, true, false}));
}

TEST_CASE("contains_ follows a stable nested TSD reference when its backing changes",
          "[feature:operator][series:tsd][series:reference][feature:collection-change]"
          "[binding:reference-to-value][transition:update]") {
    const auto forward_dictionary = node([](In<REF<TSD<i64, TSD<i64, TS<i64>>>>> input,
                                            Out<REF<TSD<i64, TSD<i64, TS<i64>>>>> output) {
        const auto stable = details::ReferenceProvider::representation(
            input.value(), cpp_time_series_runtime_type<TSD<i64, TSD<i64, TS<i64>>>>());
        output.set_value(stable);
    });
    const auto select_dictionary = node([](In<TS<bool>> select_second, In<REF<TSD<i64, TS<i64>>>> first,
                                           In<REF<TSD<i64, TS<i64>>>> second,
                                           Out<TSD<i64, REF<TSD<i64, TS<i64>>>>> output) {
        output[1].copy(*select_second ? second : first);
    });
    auto observed_references = std::vector<std::uintptr_t>{};
    const auto observe_reference = node([&](In<TS<bool>>, In<REF<TSD<i64, TS<i64>>>> input,
                                            Out<REF<TSD<i64, TS<i64>>>> output) {
        observed_references.push_back(input.value().raw());
        output.copy(input);
    });

    const auto tested = [&](wp<TS<bool>> select_second, wp<TSD<i64, TS<i64>>> first,
                            wp<TSD<i64, TS<i64>>> second) {
        const auto selected = forward_dictionary(select_dictionary(select_second, first, second))[i64{1}];
        const auto observed = observe_reference(select_second, selected);
        return operators::contains_("collection"_n = observed, "key"_n = i64{7});
    };

    const auto actual = bg::run_graph(
        tested, {false, true, false, true}, {{{7, 70}}, literal::SKIP, literal::SKIP, literal::SKIP},
        {{}, literal::SKIP, literal::SKIP, literal::SKIP});

    CHECK(actual.equals<bool>({true, false, true, false}));
    REQUIRE(observed_references.size() == 4);
    CHECK(observed_references[1] == observed_references[0]);
    CHECK(observed_references[2] == observed_references[0]);
    CHECK(observed_references[3] == observed_references[0]);
}

TEST_CASE("TSD membership resubscribes when its switch branch is recreated",
          "[evaluation][series:tsd][series:reference][feature:collection-change][feature:switch]"
          "[feature:nested-graph][shape:nested][transition:stop][transition:restart][definition:function]") {
    using Definition = tsd_membership_evaluation_count_definition<false>;
    const auto observe = node<Definition>();
    const auto make_dictionary = node([](In<TS<i64>> value, Out<TSD<i64, TS<i64>>> output) {
        output[7].set(*value);
    });
    const auto selected_branch = graph("name"_n = "selected_membership_branch", [&](wp<TS<i64>> value) {
        return observe(make_dictionary(value));
    });

    const auto tested = [&](wp<TS<i64>> selector, wp<TS<i64>> value) {
        return switch_(selector,
                       cases{case_(i64{1}, selected_branch),
                             case_(i64{2}, [](wp<TS<i64>>) { return operators::const_(false).as<TS<bool>>(); })},
                       value);
    };

    Definition::evaluation_count = 0;
    const auto actual = bg::run_graph(tested, {1, 2, 1, 2, 1}, {10, 20, 30, 40, 50});

    CHECK(actual.equals<bool>({true, false, true, false, true}));
    CHECK(Definition::evaluation_count == 3);
}

TEST_CASE("is_empty observes required and optional TSS membership",
          "[feature:operator][series:tss][scalar:i64][validity:required][validity:optional]"
          "[transition:first-tick][transition:invalidation][transition:revalidation]") {
    const auto required = [](wp<TSS<i64>> values) { return operators::is_empty(values); };
    const auto optional = [](wp<TSS<i64>> values) { return operators::is_empty(opt(values)); };
    CHECK(bg::run_graph(required, bg::input<TSS<i64>>(
                                           {literal::SKIP, {}, {1}, {literal::REMOVED(1)}}))
              .equals<bool>({std::nullopt, true, false, true}));
    CHECK(bg::run_graph(optional, bg::input<TSS<i64>>(
                                           {literal::SKIP, {}, {1}, {literal::REMOVED(1)}}))
              .equals<bool>({std::nullopt, true, false, true}));
}

TEST_CASE("TSS equality accounts for simultaneous membership changes",
          "[feature:operator][series:tss][scalar:i64][guarantee:ordinary][transition:addition]"
          "[transition:removal]") {
    const auto equal = [](wp<TSS<i64>> lhs, wp<TSS<i64>> rhs) { return lhs == rhs; };
    const auto not_equal = [](wp<TSS<i64>> lhs, wp<TSS<i64>> rhs) { return operators::ne(lhs, rhs); };

    const auto equal_result = bg::run_graph(
        equal, bg::input<TSS<i64>>({{1}, {2}, {3}, {literal::REMOVED(2)}}),
        bg::input<TSS<i64>>({{1}, {3}, {2}, {literal::REMOVED(2)}}));
    const auto not_equal_result = bg::run_graph(
        not_equal, bg::input<TSS<i64>>({{1}, {2}, {3}, {literal::REMOVED(2)}}),
        bg::input<TSS<i64>>({{1}, {3}, {2}, {literal::REMOVED(2)}}));
    CHECK(equal_result.equals<bool>({true, false, true, true}));
    CHECK(not_equal_result.equals<bool>({false, true, false, false}));
}

TEST_CASE("TSS equality preserves mismatch counts across same-cycle key changes",
          "[feature:operator][series:tss][scalar:i64][transition:addition][transition:removal]") {
    const auto equal = [](wp<TSS<i64>> lhs, wp<TSS<i64>> rhs) { return lhs == rhs; };

    CHECK(bg::run_graph(
              equal,
              bg::input<TSS<i64>>({{}, {1}, {2}, {literal::REMOVED(1)}, {literal::REMOVED(2)}, {2},
                                        {literal::REMOVED(2)}, literal::SKIP, {3}}),
              bg::input<TSS<i64>>({{}, {1}, literal::SKIP, {literal::REMOVED(1)}, {2}, literal::SKIP,
                                        {literal::REMOVED(2)}, {3}, literal::SKIP}))
              .equals<bool>({true, true, false, false, false, true, true, false, true}));
}

TEST_CASE("TSS equality reconciles after required input invalidation",
          "[feature:operator][series:tss][scalar:i64][validity:required][transition:invalidation]"
          "[transition:revalidation]") {
    const auto equal = [](wp<TS<bool>> condition, wp<TSS<i64>> never_valid, wp<TSS<i64>> values) {
        return operators::if_then_else(condition, never_valid, values) == values;
    };

    const auto result = bg::run_graph(
        equal, bg::input<TS<bool>>({false, true, true, false}),
        bg::input<TSS<i64>>({literal::SKIP, literal::SKIP, literal::SKIP, literal::SKIP}),
        bg::input<TSS<i64>>({{1}, {1}, {2}, {2}}));
    CHECK(result.equals<bool>({true, std::nullopt, std::nullopt, true}));
}

TEST_CASE("optional TSS equality treats invalid inputs as empty with ordinary outputs",
          "[feature:operator][series:tss][scalar:i64][guarantee:ordinary][validity:optional]"
          "[transition:invalidation][transition:revalidation]") {
    const auto equal = [](wp<TSS<i64>> lhs, wp<TSS<i64>> rhs) {
        return operators::eq("lhs"_n = opt(lhs), "rhs"_n = opt(rhs));
    };

    CHECK(bg::run_graph(equal, bg::input<TSS<i64>>({literal::SKIP, {}, {1}}),
                              bg::input<TSS<i64>>({{}, {}, {2}}))
              .equals<bool>({true, true, false}));
}

TEST_CASE("TSS equality preserves required guarantees and weakens optional guarantees",
          "[feature:operator][series:tss][scalar:i64][guarantee:final][guarantee:deduplicated]"
          "[validity:required][validity:optional]") {
    const auto final_set = node([](Scalar<i64> key, Out<TSS<i64, TimeSeriesGuarantee::final>> output) {
        output.apply({*key});
    });
    const auto check_final = [&] {
        const auto lhs = final_set(1);
        const auto rhs = final_set(1);
        const auto required = operators::eq(lhs, rhs);
        const auto optional = operators::eq("lhs"_n = opt(lhs), "rhs"_n = opt(rhs));
        const auto empty = operators::is_empty("value"_n = opt(lhs));
        CHECK(&required.type() == &TS<bool, TimeSeriesGuarantee::final>::type());
        CHECK(&optional.type() == &TS<bool>::type());
        CHECK(&empty.type() == &TS<bool>::type());
        return required;
    };
    const auto add_key = node([](In<TS<i64>> key, Out<TSS<i64, TimeSeriesGuarantee::deduplicated>> output) {
        output.apply({*key});
    });
    const auto check_deduplicated = [&](wp<TS<i64>> lhs_keys, wp<TS<i64>> rhs_keys) {
        const auto lhs = add_key(lhs_keys);
        const auto rhs = add_key(rhs_keys);
        const auto required = operators::eq(lhs, rhs);
        const auto optional = operators::ne("lhs"_n = opt(lhs), "rhs"_n = opt(rhs));
        CHECK(&required.type() == &TS<bool, TimeSeriesGuarantee::deduplicated>::type());
        CHECK(&optional.type() == &TS<bool>::type());
        return required;
    };

    CHECK(bg::run_graph(check_final).equals<bool>({true}));
    CHECK(bg::run_graph(check_deduplicated, {1, 2}, {1, 3}).equals<bool>({true, false}));
}

TEST_CASE("TSS set operators publish only changes to their result",
          "[feature:operator][series:tss][scalar:i64][transition:addition][transition:removal]") {
    const auto set_union = [](wp<TSS<i64>> lhs, wp<TSS<i64>> rhs) { return lhs | rhs; };
    const auto intersection = [](wp<TSS<i64>> lhs, wp<TSS<i64>> rhs) { return lhs & rhs; };
    const auto difference = [](wp<TSS<i64>> lhs, wp<TSS<i64>> rhs) { return lhs - rhs; };
    const auto symmetric_difference = [](wp<TSS<i64>> lhs, wp<TSS<i64>> rhs) { return lhs ^ rhs; };
    CHECK(bg::run_graph(set_union,
                              bg::input<TSS<i64>>({{1, 2}, {3, literal::REMOVED(1)}, literal::SKIP,
                                                        {literal::REMOVED(2)}}),
                              bg::input<TSS<i64>>({{2, 3}, {1, literal::REMOVED(2)}, {2, literal::REMOVED(3)},
                                                        literal::SKIP}))
              .equals({{1, 2, 3}, literal::SKIP, literal::SKIP, literal::SKIP}));
    CHECK(bg::run_graph(intersection,
                              bg::input<TSS<i64>>({{1, 2}, {3, literal::REMOVED(1)}, literal::SKIP,
                                                        {literal::REMOVED(2)}}),
                              bg::input<TSS<i64>>({{2, 3}, {1, literal::REMOVED(2)}, {2, literal::REMOVED(3)},
                                                        literal::SKIP}))
              .equals({{2}, {3, literal::REMOVED(2)}, {2, literal::REMOVED(3)}, {literal::REMOVED(2)}}));
    CHECK(bg::run_graph(difference,
                              bg::input<TSS<i64>>({{1, 2}, {3, literal::REMOVED(1)}, literal::SKIP,
                                                        {literal::REMOVED(2)}}),
                              bg::input<TSS<i64>>({{2, 3}, {1, literal::REMOVED(2)}, {2, literal::REMOVED(3)},
                                                        literal::SKIP}))
              .equals({{1}, {2, literal::REMOVED(1)}, {3, literal::REMOVED(2)}, literal::SKIP}));
    CHECK(bg::run_graph(symmetric_difference,
                              bg::input<TSS<i64>>({{1, 2}, {3, literal::REMOVED(1)}, literal::SKIP,
                                                        {literal::REMOVED(2)}}),
                              bg::input<TSS<i64>>({{2, 3}, {1, literal::REMOVED(2)}, {2, literal::REMOVED(3)},
                                                        literal::SKIP}))
              .equals({{1, 3}, {2, literal::REMOVED(3)}, {3, literal::REMOVED(2)}, {2}}));
}

TEST_CASE("TSS set operations handle same-cycle additions, removals, and key moves",
          "[feature:operator][series:tss][scalar:i64][transition:addition][transition:removal]") {
    const auto set_union = [](wp<TSS<i64>> lhs, wp<TSS<i64>> rhs) { return lhs | rhs; };
    const auto intersection = [](wp<TSS<i64>> lhs, wp<TSS<i64>> rhs) { return lhs & rhs; };
    const auto difference = [](wp<TSS<i64>> lhs, wp<TSS<i64>> rhs) { return lhs - rhs; };
    const auto symmetric_difference = [](wp<TSS<i64>> lhs, wp<TSS<i64>> rhs) { return lhs ^ rhs; };
    CHECK(bg::run_graph(set_union, bg::input<TSS<i64>>({{1}, {2}, {literal::REMOVED(2)}, {3}}),
                              bg::input<TSS<i64>>({{}, {2}, {3, literal::REMOVED(2)},
                                                        {literal::REMOVED(3)}}))
              .equals({{1}, {2}, {3, literal::REMOVED(2)}, literal::SKIP}));
    CHECK(bg::run_graph(intersection, bg::input<TSS<i64>>({{1}, {2}, {literal::REMOVED(2)}, {3}}),
                              bg::input<TSS<i64>>({{}, {2}, {3, literal::REMOVED(2)},
                                                        {literal::REMOVED(3)}}))
              .equals({{}, {2}, {literal::REMOVED(2)}, literal::SKIP}));
    CHECK(bg::run_graph(difference, bg::input<TSS<i64>>({{1}, {2}, {literal::REMOVED(2)}, {3}}),
                              bg::input<TSS<i64>>({{}, {2}, {3, literal::REMOVED(2)},
                                                        {literal::REMOVED(3)}}))
              .equals({{1}, literal::SKIP, literal::SKIP, {3}}));
    CHECK(bg::run_graph(
              symmetric_difference, bg::input<TSS<i64>>({{1}, {2}, {literal::REMOVED(2)}, {3}}),
              bg::input<TSS<i64>>({{}, {2}, {3, literal::REMOVED(2)}, {literal::REMOVED(3)}}))
              .equals({{1}, literal::SKIP, {3}, literal::SKIP}));
}

TEST_CASE("TSS set operators propagate guarantees and weaken optional inputs",
          "[feature:operator][series:tss][scalar:i64][guarantee:final][guarantee:deduplicated]"
          "[guarantee:const][validity:optional]") {
    const auto final_set = node([](Scalar<i64> key, Out<TSS<i64, TimeSeriesGuarantee::final>> output) {
        output.apply({*key});
    });
    const auto check_final = [&] {
        const auto lhs = final_set(1);
        const auto rhs = final_set(2);
        const auto result = lhs | rhs;
        const auto optional = opt(lhs) | rhs;
        CHECK(&result.type() == &TSS<i64, TimeSeriesGuarantee::final>::type());
        CHECK(&optional.type() == &TSS<i64>::type());
        return result;
    };
    const auto make_deduplicated = node([](Scalar<i64> key,
                                          Out<TSS<i64, TimeSeriesGuarantee::deduplicated>> output) {
        output.apply({*key});
    });
    const auto check_deduplicated = [&] {
        const auto lhs = make_deduplicated(1);
        const auto rhs = make_deduplicated(2);
        const auto result = lhs ^ rhs;
        CHECK(&result.type() == &TSS<i64, TimeSeriesGuarantee::deduplicated>::type());
        return result;
    };
    const auto constant_set = node([](Scalar<i64> first, Out<TSS<i64, TimeSeriesGuarantee::const_>> output) {
        output.apply({*first, *first + 1});
    });
    const auto check_const = [&] {
        const auto lhs = constant_set(1);
        const auto rhs = constant_set(2);
        const auto result = lhs & rhs;
        CHECK(&result.type() == &TSS<i64, TimeSeriesGuarantee::const_>::type());
        return result;
    };

    CHECK(bg::run_graph(check_final).equals({{1, 2}}));
    CHECK(bg::run_graph(check_deduplicated).equals({{1, 2}}));
    CHECK(bg::run_graph(check_const).equals({{2}}));
}

TEST_CASE("TSS set operations accept erased wiring points and treat optional invalid sets as empty",
          "[feature:operator][series:tss][scalar:i64][validity:optional][transition:first-tick]"
          "[declaration:generic]") {
    const auto set_union = [](wp<> lhs, wp<> rhs) { return lhs | rhs; };
    const auto difference = [](wp<> lhs, wp<> rhs) {
        return operators::sub("lhs"_n = opt(lhs), "rhs"_n = opt(rhs));
    };
    CHECK(bg::run_graph(set_union, bg::input<TSS<i64>>({literal::SKIP, {1, 2}}),
                              bg::input<TSS<i64>>({{1}, {2}}))
              .equals({literal::SKIP, {1, 2}}));
    CHECK(bg::run_graph(difference, bg::input<TSS<i64>>({literal::SKIP, {1, 2}}),
                              bg::input<TSS<i64>>({{1}, {2}}))
              .equals({{}, literal::SKIP}));

    const auto one_optional = [](wp<TSS<i64>> lhs, wp<TSS<i64>> rhs) { return opt(lhs) | rhs; };
    CHECK(bg::run_graph(one_optional, bg::input<TSS<i64>>({literal::SKIP, {1, 2}}),
                              bg::input<TSS<i64>>({{1}, {2}}))
              .equals({{1}, {2}}));
}

TEST_CASE("TSS set operations retain members while a required reference is invalid",
          "[feature:operator][series:tss][series:reference][scalar:i64][validity:required]"
          "[transition:invalidation][transition:revalidation][transition:addition][transition:removal]") {
    const auto select = node([](In<TS<i64>> phase, In<REF<TSS<i64>>, passive> populated,
                                In<REF<TSS<i64>>, passive> invalid, Out<REF<TSS<i64>>> output) {
        output.copy(*phase == 1 || *phase == 2 ? invalid : populated);
    });

    const auto graph = [&](wp<TS<i64>> phase, wp<TSS<i64>> populated, wp<TSS<i64>> invalid,
                           wp<TSS<i64>> rhs) {
        return select(phase, populated, invalid) | rhs;
    };

    CHECK(bg::run_graph(graph, {0, 1, 2, 3}, {{1}, literal::SKIP, literal::SKIP, literal::SKIP},
                              {literal::SKIP},
                              {{2}, {3, literal::REMOVED(2)}, {4, literal::REMOVED(3)}, literal::SKIP})
              .equals({{1, 2}, literal::SKIP, literal::SKIP, {4, literal::REMOVED(2)}}));
}

TEST_CASE("optional TSS set operations remove an invalid reference contribution",
          "[feature:operator][series:tss][series:reference][scalar:i64][validity:optional]"
          "[transition:invalidation][transition:revalidation]") {
    const auto set_union = [](wp<TS<bool>> condition, wp<TSS<i64>> populated, wp<TSS<i64>> invalid,
                              wp<TSS<i64>> rhs) {
        const auto selected = operators::if_then_else(condition, populated, invalid);
        return opt(selected) | rhs;
    };
    const auto intersection = [](wp<TS<bool>> condition, wp<TSS<i64>> populated, wp<TSS<i64>> invalid,
                                 wp<TSS<i64>> rhs) {
        const auto selected = operators::if_then_else(condition, populated, invalid);
        return opt(selected) & rhs;
    };
    const auto difference = [](wp<TS<bool>> condition, wp<TSS<i64>> populated, wp<TSS<i64>> invalid,
                               wp<TSS<i64>> rhs) {
        const auto selected = operators::if_then_else(condition, populated, invalid);
        return opt(selected) - rhs;
    };
    const auto symmetric_difference = [](wp<TS<bool>> condition, wp<TSS<i64>> populated,
                                          wp<TSS<i64>> invalid, wp<TSS<i64>> rhs) {
        const auto selected = operators::if_then_else(condition, populated, invalid);
        return opt(selected) ^ rhs;
    };

    CHECK(bg::run_graph(set_union, {true, false, true}, {{1, 2}, literal::SKIP, literal::SKIP}, {},
                              {{2}, literal::SKIP, literal::SKIP})
              .equals({{1, 2}, {literal::REMOVED(1)}, {1}}));
    CHECK(bg::run_graph(intersection, {true, false, true}, {{1, 2}, literal::SKIP, literal::SKIP}, {},
                              {{2}, literal::SKIP, literal::SKIP})
              .equals({{2}, {literal::REMOVED(2)}, {2}}));
    CHECK(bg::run_graph(difference, {true, false, true}, {{1, 2}, literal::SKIP, literal::SKIP}, {},
                              {{2}, literal::SKIP, literal::SKIP})
              .equals({{1}, {literal::REMOVED(1)}, {1}}));
    CHECK(bg::run_graph(symmetric_difference, {true, false, true},
                              {{1, 2}, literal::SKIP, literal::SKIP}, {}, {{2}, literal::SKIP, literal::SKIP})
              .equals({{1}, {2, literal::REMOVED(1)}, {1, literal::REMOVED(2)}}));
}

TEST_CASE("TSS set operations do not publish when a valid reference selects equal membership",
          "[feature:operator][series:tss][series:reference][scalar:i64][transition:update]") {
    const auto graph = [](wp<TS<bool>> condition, wp<TSS<i64>> first, wp<TSS<i64>> second,
                          wp<TSS<i64>> rhs) {
        return operators::if_then_else(condition, first, second) | rhs;
    };

    CHECK(bg::run_graph(graph, {true, false}, {{1}, literal::SKIP}, {{1}, literal::SKIP},
                              {{2}, literal::SKIP})
              .equals({{1, 2}, literal::SKIP}));
}

TEST_CASE("TSS set operations include the old target's same-cycle change when a reference is retargeted",
          "[feature:operator][series:tss][series:reference][transition:update]") {
    const auto graph = [](wp<TS<bool>> condition, wp<TSS<i64>> lhs, wp<TSS<i64>> old_target,
                          wp<TSS<i64>> new_target) {
        return lhs | operators::if_then_else(condition, old_target, new_target);
    };

    CHECK(bg::run_graph(graph, {true, false}, {{1}, literal::SKIP}, {{2}, {3, literal::REMOVED(2)}},
                              {{3}, literal::SKIP})
              .equals({{1, 2}, {3, literal::REMOVED(2)}}));
}

TEST_CASE("TSS set operations do not replay a stale old-target delta when a reference is retargeted",
          "[feature:operator][series:tss][series:reference][transition:update]") {
    const auto graph = [](wp<TS<bool>> condition, wp<TSS<i64>> lhs, wp<TSS<i64>> old_target,
                          wp<TSS<i64>> new_target) {
        return lhs | operators::if_then_else(condition, old_target, new_target);
    };

    CHECK(bg::run_graph(graph, {true, false}, {{1}, literal::SKIP}, {{2}, literal::SKIP},
                              {{3}, literal::SKIP})
              .equals({{1, 2}, {3, literal::REMOVED(2)}}));
}

TEST_CASE("TSS equality sees same-cycle removals from a sampled previous target",
          "[feature:operator][series:tss][series:reference][transition:update]") {
    const auto graph = [](wp<TS<bool>> condition, wp<TSS<i64>> old_target, wp<TSS<i64>> new_target) {
        return operators::if_then_else(condition, old_target, new_target) == new_target;
    };

    CHECK(bg::run_graph(graph, ticks({true, false}),
                              bg::input<TSS<i64>>({{2}, {3, literal::REMOVED(2)}}),
                              bg::input<TSS<i64>>({{3}, literal::SKIP}))
              .equals<bool>({false, true}));
}

TEST_CASE("TSS set operators support string keys",
          "[feature:operator][series:tss][scalar:string][transition:addition][transition:removal]") {
    const auto graph = [](wp<TSS<string>> lhs, wp<TSS<string>> rhs) { return lhs | rhs; };

    CHECK(bg::run_graph(graph, {{"alpha", "beta"}, {literal::REMOVED("alpha"), "gamma"}}, {{"beta"}})
              .equals({{"alpha", "beta"}, {"gamma", literal::REMOVED("alpha")}}));
}

TEST_CASE("contains_ does not reevaluate when unrelated TSS keys change",
          "[feature:operator][series:tss][scalar:i64][transition:addition][transition:removal]") {
    const auto graph = [](wp<TSS<i64>> collection) {
        return operators::contains_("collection"_n = collection, "key"_n = i64{1});
    };

    CHECK(bg::run_graph(graph, {{2}, {3}, {4}, {1}, {5}, {literal::REMOVED(1)}, {6}})
              .equals<bool>({false, std::nullopt, std::nullopt, true, std::nullopt, false, std::nullopt}));
}

TEST_CASE("mapped contains_ branches share an outer TSS and release removed subscriptions",
          "[feature:map][feature:operator][feature:collection-change][series:tss]"
          "[shape:doubly-nested][transition:addition][transition:removal]") {
    auto evaluations = std::array<std::size_t, 2>{};
    const auto record_membership = node(
        [&](In<TS<i64, TimeSeriesGuarantee::const_>> key, In<TS<bool>> membership) {
            ++evaluations[static_cast<std::size_t>(*key - 7)];
            return *membership;
        });
    const auto branch = graph(
        "name"_n = "mapped_outer_tss_membership",
        [&](wp<TS<i64, TimeSeriesGuarantee::const_>> key, wp<TSS<i64>> collection) {
            return record_membership(key, operators::contains_("collection"_n = collection, "key"_n = key));
        });

    const auto tested = [&](wp<TSS<i64>> branch_keys, wp<TSS<i64>> collection) {
        const auto result = map(branch, branch_keys, pass_through(collection), "__key_arg__"_n = 0);
        return result[i64{7}];
    };

    (void)bg::run_graph(tested, {{7, 8}, {}, {}, {literal::REMOVED(7)}, {}, {7}},
                              {{7}, {9}, {literal::REMOVED(7)}, {}, {7}, {}});

    const auto expected_evaluations = std::array<std::size_t, 2>{3, 1};
    CHECK(evaluations == expected_evaluations);
}

TEST_CASE("contains_ follows a time-series key through invalidation and revalidation",
          "[feature:operator][series:tss][scalar:i64][shape:single-node][validity:required]"
          "[transition:invalidation][transition:revalidation]") {
    const auto graph = [](wp<TSS<i64>> collection, wp<TS<i64>> key) {
        return operators::contains_("collection"_n = collection, "key"_n = key);
    };

    CHECK(bg::run_graph(graph, {{1, 2}, {1, 2}, {1, 2}}, {1, literal::INVALID, 2})
              .equals({true, literal::INVALID, true}));
}

TEST_CASE("contains_ follows a selected TSS reference through retargeting and invalidation",
          "[feature:operator][feature:reference][series:tss][scalar:i64][binding:reference-to-value]"
          "[transition:update][transition:invalidation][transition:revalidation]") {
    const auto query = [](wp<TS<bool>> condition, wp<TSS<i64>> left, wp<TSS<i64>> right) {
        const auto selected = operators::if_then_else(condition, left, right);
        return operators::contains_("collection"_n = selected, "key"_n = i64{1});
    };

    CHECK(bg::run_graph(query, {true, false, literal::INVALID, true, true, true},
                              {{1}, {2}, {3}, {1}, {1}, {1}}, {{2}, {2}, {3}, {3}, {3}, {3}})
              .equals({true, false, literal::INVALID, true, literal::SKIP, literal::SKIP}));

    CHECK(bg::run_graph(
              query, {true, true, true}, {literal::SKIP, literal::SKIP, literal::SKIP}, {{1}, {1}, {1}})
              .equals({literal::SKIP, literal::SKIP, literal::SKIP}));

    const auto observe_validity = [](wp<TS<bool>> condition, wp<TSS<i64>> left, wp<TSS<i64>> right) {
        const auto selected = operators::if_then_else(condition, left, right);
        return operators::valid(operators::contains_("collection"_n = selected, "key"_n = i64{1}));
    };
    CHECK(bg::run_graph(observe_validity,
                              {true, false, literal::INVALID, true, true, true},
                              {{1}, {2}, {3}, {1}, {1}, {1}}, {{2}, {2}, {3}, {3}, {3}, {3}})
              .equals({true, literal::SKIP, false, true, literal::SKIP, literal::SKIP}));
}

TEST_CASE("contains_ propagates a final TSS guarantee",
          "[feature:operator][series:tss][scalar:i64][shape:single-node][guarantee:final]") {
    const auto graph = [] {
        const auto result = operators::contains_("collection"_n = final_keys(), "key"_n = i64{1});
        CHECK(&result.type() == &TS<bool, TimeSeriesGuarantee::final>::type());
        return result;
    };

    CHECK(bg::run_graph(graph).equals<bool>({true}));
}

TEST_CASE("contains_ combines a final TSS with an ordinary time-series key",
          "[feature:operator][series:tss][scalar:i64][shape:single-node][guarantee:ordinary][transition:update]") {
    const auto graph = [](wp<TS<i64>> key) {
        const auto result = operators::contains_("collection"_n = final_keys(), "key"_n = key);
        CHECK(&result.type() == &TS<bool>::type());
        return result;
    };

    CHECK(bg::run_graph(graph, {1, 2, 3}).equals<bool>({true, true, false}));
}

TEST_CASE("contains_ propagates TSD collection and key guarantees",
          "[feature:operator][series:tsd][scalar:i64][guarantee:final][guarantee:ordinary]") {
    const auto final_dictionary = node([](Out<TSD<i64, TS<i64>, TimeSeriesGuarantee::final>> output) {
        output[1].set(1);
    });
    const auto final_key = node([](Out<TS<i64, TimeSeriesGuarantee::final>> output) { output.set(1); });
    const auto ordinary_key = node([](Out<TS<i64>>) {});

    const auto check_final_collection = [&](auto collection) {
        const auto physical = operators::contains_("collection"_n = collection, "key"_n = i64{1});
        const auto valid_item_contains =
            operators::contains_("collection"_n = valid_items(collection), "key"_n = i64{1});
        CHECK(&physical.type() == &TS<bool, TimeSeriesGuarantee::final>::type());
        CHECK(&valid_item_contains.type() == &TS<bool, TimeSeriesGuarantee::final>::type());
    };
    testing::test_wiring([&] { check_final_collection(final_dictionary()); });

    const auto final_collection_ordinary_key = [&](wp<TS<i64>> key) {
        const auto result = operators::contains_("collection"_n = final_dictionary(), "key"_n = key);
        CHECK(&result.type() == &TS<bool>::type());
        return result;
    };
    testing::test_wiring([&] { (void)final_collection_ordinary_key(ordinary_key()); });

    const auto final_collection_and_key = [&] {
        const auto result = operators::contains_("collection"_n = final_dictionary(), "key"_n = final_key());
        CHECK(&result.type() == &TS<bool, TimeSeriesGuarantee::final>::type());
        return result;
    };
    testing::test_wiring([&] { (void)final_collection_and_key(); });
}

TEST_CASE("contains_ rejects optional collection and key inputs for TSS and TSD",
          "[feature:operator][series:tss][series:tsd][validity:optional]") {
    const auto tss_source = node([](Out<TSS<i64>>) {});
    const auto tsd_source = node([](Out<TSD<i64, TS<i64>>>) {});
    const auto key_source = node([](Out<TS<i64>>) {});

    CHECK_THROWS_AS(testing::test_wiring([&] {
                        (void)operators::contains_("collection"_n = opt(tss_source()), "key"_n = i64{1});
                    }),
                    WiringError);
    CHECK_THROWS_AS(testing::test_wiring([&] {
                        (void)operators::contains_("collection"_n = tss_source(), "key"_n = opt(key_source()));
                    }),
                    WiringError);
    CHECK_THROWS_AS(testing::test_wiring([&] {
                        (void)operators::contains_("collection"_n = opt(tsd_source()), "key"_n = i64{1});
                    }),
                    WiringError);
    CHECK_THROWS_AS(testing::test_wiring([&] {
                        (void)operators::contains_("collection"_n = tsd_source(), "key"_n = opt(key_source()));
                    }),
                    WiringError);
}

TEST_CASE("len_ forwards equal sizes for ordinary inputs and suppresses them for deduplicated inputs",
          "[feature:operator][series:tss][scalar:i64][guarantee:ordinary][guarantee:deduplicated]"
          "[transition:update]") {
    const auto ordinary = [](wp<TSS<i64>> values) { return operators::len_("value"_n = values); };
    const auto deduplicated_set = node([](In<TS<i64>> step,
                                          Out<TSS<i64, TimeSeriesGuarantee::deduplicated>> output) {
        if (*step == 0) {
            output.apply({1, 2});
        } else if (*step == 1) {
            output.apply({literal::REMOVED(1), 3});
        } else {
            output.apply({literal::REMOVED(3), 1});
        }
    });
    const auto deduplicated = [&](wp<TS<i64>> step) {
        return operators::len_("value"_n = deduplicated_set(step));
    };

    CHECK(bg::run_graph(ordinary,
                              {{1, 2}, {literal::REMOVED(1), 3}, {literal::REMOVED(3), 1}})
              .equals<i64>({2, 2, 2}));
    CHECK(bg::run_graph(deduplicated, {0, 1, 2}).equals<i64>({2, std::nullopt, std::nullopt}));
}

TEST_CASE("len_ and contains_ query scalar time-series lists",
          "[feature:operator][series:value][scalar:list][scalar:i64][shape:single-node]") {
    const auto length = [](wp<TS<List<i64>>> values) { return operators::len_("value"_n = values); };
    const auto contains = [](wp<TS<List<i64>>> values) {
        return operators::contains_("collection"_n = values, "key"_n = i64{2});
    };
    const auto contains_key_series = [](wp<TS<List<i64>>> values, wp<TS<i64>> key) {
        return operators::contains_("collection"_n = values, "key"_n = key);
    };

    CHECK(bg::run_graph(length, {{1, 2, 3}, {}}).equals<i64>({3, 0}));
    CHECK(bg::run_graph(contains, {{1, 2, 3}, {1, 3}, {}}).equals<bool>({true, false, false}));
    CHECK(bg::run_graph(contains_key_series, {{1, 2, 3}, {1, 3}, {}}, {2, 3, 1})
              .equals<bool>({true, true, false}));
}

TEST_CASE("len_ and contains_ query scalar time-series sets",
          "[feature:operator][series:value][scalar:set][scalar:i64][shape:single-node]") {
    const auto length = [](wp<TS<Set<i64>>> values) { return operators::len_("value"_n = values); };
    const auto contains = [](wp<TS<Set<i64>>> values) {
        return operators::contains_("collection"_n = values, "key"_n = i64{2});
    };
    const auto contains_key_series = [](wp<TS<Set<i64>>> values, wp<TS<i64>> key) {
        return operators::contains_("collection"_n = values, "key"_n = key);
    };

    CHECK(bg::run_graph(length, {{1, 2, 3}, {2, 3}}).equals<i64>({3, 2}));
    CHECK(bg::run_graph(contains, {{1, 2, 3}, {1, 3}, {}}).equals<bool>({true, false, false}));
    CHECK(bg::run_graph(contains_key_series, {{1, 2, 3}, {1, 3}, {}}, {2, 3, 1})
              .equals<bool>({true, true, false}));
}

TEST_CASE("len_ and contains_ query scalar time-series dictionaries",
          "[feature:operator][series:value][scalar:dict][scalar:i64][shape:single-node]") {
    const auto length = [](wp<TS<Dict<i64, i64>>> values) { return operators::len_("value"_n = values); };
    const auto contains = [](wp<TS<Dict<i64, i64>>> values) {
        return operators::contains_("collection"_n = values, "key"_n = i64{2});
    };
    const auto contains_key_series = [](wp<TS<Dict<i64, i64>>> values, wp<TS<i64>> key) {
        return operators::contains_("collection"_n = values, "key"_n = key);
    };

    CHECK(bg::run_graph(length, {{{1, 10}, {2, 20}}, {{2, 20}}}).equals<i64>({2, 1}));
    CHECK(bg::run_graph(contains, {{{1, 10}, {2, 20}}, {{1, 10}}, {}}).equals<bool>({true, false, false}));
    CHECK(bg::run_graph(contains_key_series, {{{1, 10}, {2, 20}}, {{1, 10}}, {}}, {2, 1, 3})
              .equals<bool>({true, true, false}));
}

TEST_CASE("len_ and contains_ query strings and symbols without copying text",
          "[feature:operator][series:value][scalar:string][scalar:symbol][shape:single-node]") {
    const auto string_length = [](wp<TS<string>> value) { return operators::len_("value"_n = value); };
    const auto symbol_length = [](wp<TS<symbol>> value) { return operators::len_("value"_n = value); };
    const auto string_contains = [](wp<TS<string>> value, wp<TS<string>> item) {
        return operators::contains_("collection"_n = value, "key"_n = item);
    };
    const auto symbol_contains = [](wp<TS<symbol>> value, wp<TS<symbol>> item) {
        return operators::contains_("collection"_n = value, "key"_n = item);
    };
    const auto strings = std::array{std::string_view{"a\0bc", 4}, std::string_view{"abc"}};
    const auto string_items = std::array{std::string_view{"\0b", 2}, std::string_view{"d"}};
    const auto symbols = std::array{symbol{"alphabet"}, symbol{"gamma"}};
    const auto symbol_items = std::array{symbol{"pha"}, symbol{"am"}};

    CHECK(bg::run_graph(string_length, strings).equals<i64>({4, 3}));
    CHECK(bg::run_graph(symbol_length, symbols).equals<i64>({8, 5}));
    CHECK(bg::run_graph(string_contains, strings, string_items).equals<bool>({true, false}));
    CHECK(bg::run_graph(symbol_contains, symbols, symbol_items).equals<bool>({true, true}));
}
