#include <hg/graph.hpp>
#include <hg/mesh.hpp>
#include <hg/node.hpp>
#include <hg/operators.hpp>
#include <hg/run_graph.hpp>
#include <hg/switch.hpp>
#include <hg/time_series/tsb/tsb.hpp>
#include <hg/time_series/tsd/tsd_type.hpp>
#include <hg/time_series/tsl/tsl.hpp>
#include <hg/time_series/tss/tss_type.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace hg;
using namespace hg::literals;
using namespace hg::util::literals;

namespace {

template <template <typename, auto...> typename Field> struct MeshStructuredBundle {
    Field<TS<i64>> value;
    Field<TS<i64>> other;
};

const auto capture_mesh_bundles = node([](In<TSD<i64, TSB<MeshStructuredBundle>>> input,
                                           Out<TSD<i64, TSB<MeshStructuredBundle>>> output) {
    output.copy_tick(input);
});

TEST_CASE("mesh forwards partial TSB child updates",
          "[feature:mesh][feature:nested-graph][series:tsd][series:tsb][schema:named]"
          "[definition:function][transition:update]") {
    const auto child = graph("name"_n = "mesh_structured_child",
                             [](wp<TS<i64>> value) {
                                 return bundle<MeshStructuredBundle>("value"_n = value,
                                                                     "other"_n = i64{20});
                             });

    const auto tested = [&](wp<TSD<i64, TS<i64>>> values) {
        return capture_mesh_bundles(mesh(child, values));
    };

    const auto actual = run_graph(tested, {{{1, 3}}, {{1, 4}}});

    CHECK(actual.equals({{{1, {{"value", 3}, {"other", 20}}}},
                         {{1, {{"value", 4}}}}}));
}

TEST_CASE("mesh structured field references follow their source branch",
          "[feature:mesh][feature:nested-graph][feature:reference][series:tsd][series:tsb]"
          "[schema:named][binding:reference-to-reference][transition:update]") {
    const auto add_hundred = node([](i64 value) { return value + 100; });

    const auto tested = [add_hundred](wp<TS<i64>> source, wp<TSS<i64>> keys) {
        const auto result = mesh(
            [add_hundred](auto key, auto value) -> wp<TSB<MeshStructuredBundle>> {
                return switch_(
                           key,
                           cases{
                               case_(i64{0}, [add_hundred](auto item) {
                                   return bundle<MeshStructuredBundle>("value"_n = item,
                                                                       "other"_n = add_hundred(item));
                               }),
                               case_(DEFAULT, [](auto) {
                                   const auto peer = mesh()[i64{0}];
                                   return bundle<MeshStructuredBundle>(
                                       "value"_n = operators::getattr(peer, "value"_sym),
                                       "other"_n = operators::getattr(peer, "other"_sym));
                               }),
                           },
                           value)
                    .template as<TSB<MeshStructuredBundle>>();
            },
            source,
            "__keys__"_n = keys,
            "__key_arg__"_n = 0);

        return capture_mesh_bundles(result);
    };

    const auto actual = run_graph(tested, {10, 11}, {{1}, literal::SKIP});

    CHECK(actual.equals({{{0, {{"value", 10}, {"other", 110}}},
                          {1, {{"value", 10}, {"other", 110}}}},
                         {{0, {{"value", 11}, {"other", 111}}},
                          {1, {{"value", 11}, {"other", 111}}}}}));
}

TEST_CASE("mesh structured output removes and recreates a branch",
          "[feature:mesh][feature:nested-graph][series:tsd][series:tsb][schema:named]"
          "[transition:removal][transition:addition][transition:lifecycle]") {
    const auto tested = [](wp<TSD<i64, TS<i64>>> values) {
        const auto result = mesh(
            [](auto value) { return bundle<MeshStructuredBundle>("value"_n = value, "other"_n = value); },
            values);
        return capture_mesh_bundles(result);
    };

    const auto actual = run_graph(tested, {{{1, 7}, {2, 8}},
                                           {{1, literal::REMOVE}},
                                           {{1, 9}}});

    CHECK(actual.equals({{{1, {{"value", 7}, {"other", 7}}}, {2, {{"value", 8}, {"other", 8}}}},
                         {{1, literal::REMOVE}},
                         {{1, {{"value", 9}, {"other", 9}}}}}));
}

const auto capture_mesh_lists = node([](In<TSD<i64, TSL<TS<i64>, 2>>> input,
                                        Out<TSD<i64, TSL<TS<i64>, 2>>> output) {
    output.copy_tick(input);
});

TEST_CASE("mesh structured item references follow a source TSL",
          "[feature:mesh][feature:nested-graph][feature:reference][series:tsd][series:fixed-tsl]"
          "[binding:reference-to-reference][transition:update]") {
    const auto add_one = node([](i64 value) { return value + 1; });

    const auto tested = [add_one](wp<TSD<i64, TS<i64>>> values) {
        const auto result = mesh(
            [add_one](auto key, auto value) -> wp<TSL<TS<i64>, 2>> {
                return switch_(
                           key,
                           cases{
                               case_(i64{0}, [add_one](auto item) { return tsl(item, add_one(item)); }),
                               case_(DEFAULT, [](auto) {
                                   const auto peer = mesh()[i64{0}];
                                   return tsl(peer[i64{0}], peer[i64{1}]);
                               }),
                           },
                           value)
                    .template as<TSL<TS<i64>, 2>>();
            },
            values);

        return capture_mesh_lists(result);
    };

    const auto actual = run_graph(tested, {{{0, 4}, {1, 0}}, {{0, 7}}});

    CHECK(actual.equals({{{0, {{0, 4}, {1, 5}}}, {1, {{0, 4}, {1, 5}}}},
                         {{0, {{0, 7}, {1, 8}}}, {1, {{0, 7}, {1, 8}}}}}));
}

TEST_CASE("mesh forwards partial TSL child updates",
          "[feature:mesh][feature:nested-graph][series:tsd][series:fixed-tsl]"
          "[definition:function][transition:update]") {
    const auto child = graph("name"_n = "mesh_list_child",
                             [](wp<TS<i64>> value) {
                                 return tsl(value, operators::const_(i64{20}).as<TS<i64>>());
                             });

    const auto tested = [&](wp<TSD<i64, TS<i64>>> values) {
        return capture_mesh_lists(mesh(child, values));
    };

    const auto actual = run_graph(tested, {{{1, 3}}, {{1, 4}}});

    CHECK(actual.equals({{{1, {{0, 3}, {1, 20}}}},
                         {{1, {{0, 4}}}}}));
}

TEST_CASE("mesh TSL output removes and recreates a branch",
          "[feature:mesh][feature:nested-graph][series:tsd][series:fixed-tsl]"
          "[definition:function][transition:removal][transition:addition][transition:lifecycle]") {
    const auto add_one = node([](i64 value) { return value + 1; });

    const auto child = graph("name"_n = "mesh_list_lifecycle_child",
                             [add_one](wp<TS<i64>> value) { return tsl(value, add_one(value)); });

    const auto tested = [&](wp<TSD<i64, TS<i64>>> values) {
        return capture_mesh_lists(mesh(child, values));
    };

    const auto actual = run_graph(tested, {{{1, 3}, {2, 8}},
                                           {{1, literal::REMOVE}},
                                           {{1, 4}}});

    CHECK(actual.equals({{{1, {{0, 3}, {1, 4}}}, {2, {{0, 8}, {1, 9}}}},
                         {{1, literal::REMOVE}},
                         {{1, {{0, 4}, {1, 5}}}}}));
}

} // namespace
