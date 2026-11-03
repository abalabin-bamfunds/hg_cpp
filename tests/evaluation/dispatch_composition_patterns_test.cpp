#include <hg/dispatch.hpp>
#include <hg/graph.hpp>
#include <hg/map.hpp>
#include <hg/node.hpp>
#include <hg/operators.hpp>
#include <hg/run_graph.hpp>
#include <hg/scalar/bundle/value.hpp>
#include <hg/switch.hpp>
#include <hg/time_series/tsb/tsb.hpp>
#include <hg/time_series/tsd/tsd_type.hpp>

#include <catch2/catch_test_macros.hpp>

namespace {

using namespace hg;
using namespace hg::literals;
using namespace hg::util;

template <template <typename, auto...> typename Field> struct DispatchPatternBase {
    Field<symbol> name;
};

template <template <typename, auto...> typename Field>
struct DispatchPatternDerived : schema_extends<DispatchPatternBase, Field> {
    Field<i64> value;
};

TEST_CASE("map dispatches each item by its concrete bundle type",
          "[feature:dispatch][feature:map][feature:nested-graph][series:tsd][series:value][scalar:hierarchical]"
          "[binding:covariant][shape:nested][transition:update][transition:removal][transition:addition]") {
    const auto base_case = graph([](wp<TS<Bundle<DispatchPatternBase>>>) {
        return operators::const_(i64{10});
    });

    const auto derived_case = graph([](wp<TS<Bundle<DispatchPatternDerived>>>) {
        return operators::const_(i64{20});
    });

    const auto choose = dispatch_<auto(TS<Bundle<DispatchPatternBase>>) -> TS<i64>>{base_case, derived_case};

    const auto select = graph([&](wp<TS<Bundle<DispatchPatternBase>>> value) { return choose(value); });

    const auto publish = node([](i64 phase,
                                 Scalar<Bundle<DispatchPatternBase>> base,
                                 Scalar<Bundle<DispatchPatternDerived>> derived,
                                 Out<TSD<i64, TS<Bundle<DispatchPatternBase>>>> output) {
        if (phase == 1) {
            output[1].set(derived);
            output[2].set(base);
        } else if (phase == 2) {
            output[1].set(base);
        } else if (phase == 3) {
            output.erase(1);
            output[2].set(derived);
        } else {
            output[1].set(base);
        }
    });

    const auto copy = node([](In<TSD<i64, TS<i64>>> values, Out<TSD<i64, TS<i64>>> output) {
        output.copy_tick(values);
    });

    auto memory = allocator{};
    const auto base = bundle<DispatchPatternBase>(memory, symbol{"base"});
    const auto derived = bundle<DispatchPatternDerived>(memory, symbol{"derived"}, i64{7});
    const auto tested = [&](wp<TS<i64>> phase) {
        return copy(map(select, publish(phase, base, derived)));
    };

    CHECK(run_graph(tested, {1, 2, 3, 4})
              .equals({{{1, 20}, {2, 10}}, {{1, 10}}, {{1, literal::REMOVE}, {2, 20}}, {{1, 10}}}));
}

TEST_CASE("mapped dispatch keeps state per key and resets it after removal",
          "[feature:dispatch][feature:map][feature:nested-graph][feature:lifecycle][series:tsd]"
          "[binding:covariant][shape:nested][transition:update][transition:removal][transition:resurrection]") {
    struct CountBase {
        State<i64> evaluations;
        In<TS<Bundle<DispatchPatternBase>>> input;
        Out<TS<i64>> output;

        void evaluate() const { output.set(100 + ++*evaluations); }
    };

    struct CountDerived {
        State<i64> evaluations;
        In<TS<Bundle<DispatchPatternDerived>>> input;
        Out<TS<i64>> output;

        void evaluate() const { output.set(200 + ++*evaluations); }
    };

    const auto count_base = node<CountBase>();

    const auto count_derived = node<CountDerived>();

    const auto base_case = graph([&](wp<TS<Bundle<DispatchPatternBase>>> value) { return count_base(value); });

    const auto derived_case =
        graph([&](wp<TS<Bundle<DispatchPatternDerived>>> value) { return count_derived(value); });

    const auto choose = dispatch_<auto(TS<Bundle<DispatchPatternBase>>) -> TS<i64>>{base_case, derived_case};

    const auto publish = node([](i64 phase,
                                 Scalar<Bundle<DispatchPatternBase>> base,
                                 Scalar<Bundle<DispatchPatternDerived>> derived,
                                 Out<TSD<i64, TS<Bundle<DispatchPatternBase>>>> output) {
        if (phase == 1) {
            output[1].set(base);
            output[2].set(derived);
        } else if (phase == 2) {
            output[1].set(base);
        } else if (phase == 4) {
            output[1].set(derived);
            output[2].set(derived);
        } else if (phase == 5) {
            output[1].set(derived);
            output[2].set(base);
        } else if (phase == 6) {
            output.erase(1);
            output[2].set(base);
        } else if (phase == 7) {
            output[1].set(base);
            output[2].set(base);
        }
    });

    const auto copy = node([](In<TSD<i64, TS<i64>>> values, Out<TSD<i64, TS<i64>>> output) {
        output.copy_tick(values);
    });

    auto memory = allocator{};
    const auto base = bundle<DispatchPatternBase>(memory, symbol{"base"});
    const auto derived = bundle<DispatchPatternDerived>(memory, symbol{"derived"}, i64{7});
    const auto tested = [&](wp<TS<i64>> phase) {
        return copy(map([&](wp<TS<Bundle<DispatchPatternBase>>> value) { return choose(value); },
                        publish(phase, base, derived)));
    };

    CHECK(run_graph(tested, {1, 2, literal::SKIP, 4, 5, 6, 7, literal::SKIP})
              .equals({{{1, 101}, {2, 201}}, {{1, 102}}, literal::SKIP, {{1, 201}, {2, 202}},
                       {{1, 202}, {2, 101}}, {{1, literal::REMOVE}, {2, 102}}, {{1, 101}, {2, 103}},
                       literal::SKIP}));
}

template <template <typename, auto...> typename Field> struct DispatchPatternResult {
    Field<TS<i64>> score;
    Field<TS<symbol>> kind;
};

TEST_CASE("dispatch keeps its structural output schema while selected fields change",
          "[feature:dispatch][feature:type-system][series:tsb][schema:named]"
          "[binding:covariant][transition:update]") {
    const auto pack = node([](In<TS<i64>> score, In<TS<symbol>> kind, Out<TSB<DispatchPatternResult>> output) {
        output->score.copy_tick(score);
        output->kind.copy_tick(kind);
    });

    const auto base_case = graph([&](wp<TS<Bundle<DispatchPatternBase>>>,
                                     wp<TS<i64>> score,
                                     wp<TS<symbol>> kind) { return pack(score, kind); });

    const auto derived_case = graph([&](wp<TS<Bundle<DispatchPatternDerived>>>,
                                        wp<TS<i64>> score,
                                        wp<TS<symbol>> kind) { return pack(score, kind); });

    const auto choose = dispatch_<auto(TS<Bundle<DispatchPatternBase>>, TS<i64>, TS<symbol>)
                                          -> TSB<DispatchPatternResult>>{base_case, derived_case};
    const auto publish = node([](i64 phase,
                                 Scalar<Bundle<DispatchPatternBase>> base,
                                 Scalar<Bundle<DispatchPatternDerived>> ten,
                                 Scalar<Bundle<DispatchPatternDerived>> eleven,
                                 Out<TS<Bundle<DispatchPatternBase>>> output) {
        if (phase == 1) {
            output.set(ten);
        } else if (phase == 2) {
            output.set(eleven);
        } else {
            output.set(base);
        }
    });
    auto memory = allocator{};
    const auto base = bundle<DispatchPatternBase>(memory, symbol{"base"});
    const auto ten = bundle<DispatchPatternDerived>(memory, symbol{"ten"}, i64{10});
    const auto eleven = bundle<DispatchPatternDerived>(memory, symbol{"eleven"}, i64{11});
    const auto tested = [&](wp<TS<i64>> phase, wp<TS<i64>> score, wp<TS<symbol>> kind) {
        return choose(publish(phase, base, ten, eleven), score, kind);
    };

    CHECK(run_graph(tested,
                    {1, 2, 3},
                    {10, 11, 12},
                    {symbol{"derived"}, literal::SKIP, symbol{"base"}})
              .equals({{{"score", 10}, {"kind", symbol{"derived"}}},
                       {{"score", 11}},
                       {{"score", 12}, {"kind", symbol{"base"}}}}));
}

TEST_CASE("dispatch inside a switch follows its changing inherited context",
          "[feature:dispatch][feature:switch][feature:context-time-series][feature:nested-graph]"
          "[series:value][binding:covariant][shape:doubly-nested][transition:update]") {
    const auto read_offset = node([](In<TS<i64>, "offset"_n, context> offset) { return *offset; });

    const auto base_case = graph([&](wp<TS<Bundle<DispatchPatternBase>>>, wp<TS<i64>>) {
        return read_offset();
    });

    const auto add_values = node([](i64 left, i64 right) { return left + right; });

    const auto derived_case = graph([&](wp<TS<Bundle<DispatchPatternDerived>>>, wp<TS<i64>> amount) {
        return add_values(amount, read_offset());
    });

    const auto choose = dispatch_<auto(TS<Bundle<DispatchPatternBase>>, TS<i64>) -> TS<i64>>{
        base_case, derived_case};

    const auto selected = graph([&](wp<TS<Bundle<DispatchPatternBase>>> value, wp<TS<i64>> amount) {
        return choose(value, amount);
    });

    const auto publish = node([](i64 phase,
                                 Scalar<Bundle<DispatchPatternBase>> base,
                                 Scalar<Bundle<DispatchPatternDerived>> derived,
                                 Out<TS<Bundle<DispatchPatternBase>>> output) {
        if (phase == 1) {
            output.set(derived);
        } else if (phase == 3) {
            output.set(base);
        } else if (phase == 5) {
            output.set(derived);
        }
    });

    auto memory = allocator{};
    const auto base = bundle<DispatchPatternBase>(memory, symbol{"base"});
    const auto derived = bundle<DispatchPatternDerived>(memory, symbol{"derived"}, i64{10});
    const auto tested = [&](wp<TS<bool>> selector,
                            wp<TS<i64>> phase,
                            wp<TS<i64>> amount,
                            wp<TS<i64>> offset) {
        const auto value = publish(phase, base, derived);
        return with_context("offset"_n = offset, [&] {
            return switch_(selector,
                           cases{case_(true, selected), case_(false, selected)},
                           value, amount);
        });
    };

    CHECK(run_graph(tested,
                    {true, true, false, false, true},
                    {1, 2, 3, 4, 5},
                    {10, 11, 12, 13, 11},
                    {100, 101, 102, 103, 104})
              .equals<i64>({110, 112, 102, 103, 115}));
}

} // namespace
