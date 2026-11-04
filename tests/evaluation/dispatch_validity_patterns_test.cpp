#include <hg/dispatch.hpp>
#include <hg/graph.hpp>
#include <hg/node.hpp>
#include <hg/run_graph.hpp>
#include <hg/scalar/bundle/value.hpp>
#include <hg/time_series/tsb/tsb.hpp>
#include <hg/time_series/tsd/tsd_type.hpp>
#include <hg/time_series/tsl/tsl.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace hg;
using namespace hg::literals;
using namespace hg::util;

template <template <typename, auto...> typename Field> struct DispatchValidityBase {
    Field<symbol> name;
};

template <template <typename, auto...> typename Field>
struct DispatchValidityDerived : schema_extends<DispatchValidityBase, Field> {
    Field<i64> value;
};

TEST_CASE("dispatch preserves a shared value across reference path changes",
          "[feature:dispatch][feature:validity][series:value][series:reference][binding:reference-to-reference]"
          "[binding:covariant][transition:update]") {
    const auto base_case = graph([](wp<TS<Bundle<DispatchValidityBase>>>, wp<TS<i64>> value) { return value; });

    const auto derived_case = graph([](wp<TS<Bundle<DispatchValidityDerived>>>, wp<TS<i64>> value) { return value; });

    const auto choose = dispatch_<auto(TS<Bundle<DispatchValidityBase>>, TS<i64>)->TS<i64>>{base_case, derived_case};

    auto memory = allocator{};
    const auto base = bundle<DispatchValidityBase>(memory, symbol{"base"});
    const auto derived = bundle<DispatchValidityDerived>(memory, symbol{"derived"}, i64{1});
    const auto tested = [&](wp<TS<Bundle<DispatchValidityBase>>> input, wp<TS<i64>> value) {
        return choose(input, value);
    };

    CHECK(run_graph(tested, {base, derived, base, derived, base}, {10, literal::SKIP, literal::SKIP, 20, literal::SKIP})
              .equals<i64>({10, std::nullopt, std::nullopt, 20, std::nullopt}));
}

template <template <typename, auto...> typename Field> struct DispatchValidityBundle {
    Field<TS<i64>> left;
    Field<TS<i64>> right;
};

TEST_CASE("dispatch preserves partial bundle fields through invalidation and type changes",
          "[feature:dispatch][feature:validity][feature:invalidation][series:tsb][schema:named]"
          "[representation:free][representation:native][binding:covariant][validity:optional]"
          "[transition:update][transition:invalidation][transition:revalidation]") {
    const auto pack =
        node([](In<TS<i64>, optional> left, In<TS<i64>, optional> right, Out<TSB<DispatchValidityBundle>> output) {
            output->left.copy_tick(left);
            output->right.copy_tick(right);
        });

    const auto free_case = graph([](wp<TS<Bundle<DispatchValidityBase>>>, wp<TS<i64>> left, wp<TS<i64>> right) {
        return bundle<TSB<DispatchValidityBundle>>("left"_n = left, "right"_n = right);
    });

    const auto native_case = graph([&](wp<TS<Bundle<DispatchValidityDerived>>>, wp<TS<i64>> left, wp<TS<i64>> right) {
        return pack(opt(left), opt(right));
    });

    const auto choose =
        dispatch_<auto(TS<Bundle<DispatchValidityBase>>, TS<i64>, TS<i64>)->TSB<DispatchValidityBundle>>{free_case,
                                                                                                         native_case};

    const auto copy = node([](In<TSB<DispatchValidityBundle>> input, Out<TSB<DispatchValidityBundle>> output) {
        output.copy_tick(input);
    });

    auto memory = allocator{};
    const auto base = bundle<DispatchValidityBase>(memory, symbol{"base"});
    const auto derived = bundle<DispatchValidityDerived>(memory, symbol{"derived"}, i64{1});
    const auto tested = [&](wp<TS<Bundle<DispatchValidityBase>>> input, wp<TS<i64>> left, wp<TS<i64>> right) {
        return copy(choose(input, left, right));
    };

    const auto actual =
        run_graph(tested, {base, base, derived, derived, derived, base, base, derived},
                  {10, 11, literal::INVALID, literal::SKIP, 13, literal::SKIP, literal::INVALID, literal::SKIP},
                  {20, literal::SKIP, literal::SKIP, literal::INVALID, literal::SKIP, 24, literal::SKIP, 27});

    CHECK(actual.equals({{{"left", 10}, {"right", 20}},
                         {{"left", 11}},
                         {{"left", literal::INVALID}, {"right", 20}},
                         literal::INVALID,
                         {{"left", 13}},
                         {{"left", 13}, {"right", 24}},
                         {{"left", literal::INVALID}},
                         {{"right", 27}}}));
}

TEST_CASE("dispatch preserves partial fixed-list items through invalidation and type changes",
          "[feature:dispatch][feature:validity][feature:invalidation][series:fixed-tsl]"
          "[representation:free][representation:native][binding:covariant][validity:optional]"
          "[transition:update][transition:invalidation][transition:revalidation]") {
    const auto pack = node([](In<TS<i64>, optional> first, In<TS<i64>, optional> second, Out<TSL<TS<i64>, 2>> output) {
        output[0].copy_tick(first);
        output[1].copy_tick(second);
    });

    const auto free_case = graph(
        [](wp<TS<Bundle<DispatchValidityBase>>>, wp<TS<i64>> first, wp<TS<i64>> second) { return tsl(first, second); });

    const auto native_case = graph([&](wp<TS<Bundle<DispatchValidityDerived>>>, wp<TS<i64>> first, wp<TS<i64>> second) {
        return pack(opt(first), opt(second));
    });

    const auto choose =
        dispatch_<auto(TS<Bundle<DispatchValidityBase>>, TS<i64>, TS<i64>)->TSL<TS<i64>, 2>>{free_case, native_case};

    const auto copy = node([](In<TSL<TS<i64>, 2>> input, Out<TSL<TS<i64>, 2>> output) { output.copy_tick(input); });

    auto memory = allocator{};
    const auto base = bundle<DispatchValidityBase>(memory, symbol{"base"});
    const auto derived = bundle<DispatchValidityDerived>(memory, symbol{"derived"}, i64{1});
    const auto tested = [&](wp<TS<Bundle<DispatchValidityBase>>> input, wp<TS<i64>> first, wp<TS<i64>> second) {
        return copy(choose(input, first, second));
    };

    const auto actual =
        run_graph(tested, {base, base, derived, derived, derived, base, base, derived},
                  {10, 11, literal::INVALID, literal::SKIP, 13, literal::SKIP, literal::INVALID, literal::SKIP},
                  {20, literal::SKIP, literal::SKIP, literal::INVALID, literal::SKIP, 24, literal::SKIP, 27});

    CHECK(actual.equals({{{0, 10}, {1, 20}},
                         {{0, 11}},
                         {{0, literal::INVALID}, {1, 20}},
                         literal::INVALID,
                         {{0, 13}},
                         {{0, 13}, {1, 24}},
                         {{0, literal::INVALID}},
                         {{1, 27}}}));
}

TEST_CASE("dispatch preserves valid dictionary items through invalidation, removal, and type changes",
          "[feature:dispatch][feature:validity][feature:invalidation][series:tsd]"
          "[representation:native][binding:reference-to-reference][binding:covariant]"
          "[transition:update][transition:invalidation][transition:revalidation][transition:removal]") {
    const auto copy = node([](In<TSD<i64, TS<i64>>> input, Out<TSD<i64, TS<i64>>> output) { output.copy_tick(input); });

    const auto direct_case =
        graph([](wp<TS<Bundle<DispatchValidityBase>>>, wp<TSD<i64, TS<i64>>> values) { return values; });

    const auto copied_case =
        graph([&](wp<TS<Bundle<DispatchValidityDerived>>>, wp<TSD<i64, TS<i64>>> values) { return copy(values); });

    const auto choose = dispatch_<auto(TS<Bundle<DispatchValidityBase>>, TSD<i64, TS<i64>>)->TSD<i64, TS<i64>>>{
        direct_case, copied_case};

    auto memory = allocator{};
    const auto base = bundle<DispatchValidityBase>(memory, symbol{"base"});
    const auto derived = bundle<DispatchValidityDerived>(memory, symbol{"derived"}, i64{1});
    const auto tested = [&](wp<TS<Bundle<DispatchValidityBase>>> input, wp<TSD<i64, TS<i64>>> values) {
        return choose(input, values);
    };

    const auto actual = run_graph(tested, {base, base, derived, derived, derived, base, base, base, derived},
                                  {{{1, 10}, {2, 20}},
                                   {{1, 11}},
                                   {{1, literal::INVALID}},
                                   {{2, literal::REMOVE}},
                                   {{1, 12}},
                                   {{2, 24}},
                                   literal::INVALID,
                                   {{1, 13}},
                                   {{2, 25}}});

    CHECK(actual.equals({{{1, 10}, {2, 20}},
                         {{1, 11}},
                         {{1, literal::INVALID}, {2, 20}},
                         {{2, literal::REMOVE}},
                         {{1, 12}},
                         {{2, 24}, {1, 12}},
                         literal::INVALID,
                         {{1, 13}},
                         {{2, 25}, {1, 13}}}));
}
