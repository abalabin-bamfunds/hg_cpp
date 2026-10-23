#include <bg/graph.hpp>
#include <bg/map.hpp>
#include <bg/node.hpp>
#include <bg/testing/test_wiring.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace {

using namespace bg;
using namespace bg::literals;

TEST_CASE("map validates collection key types for every graph form",
          "[feature:map][feature:wiring][feature:type-system][series:tsd]"
          "[definition:function][definition:runtime]") {
    const auto integers = node([](Out<TSD<i64, TS<float>>>) {});

    const auto booleans = node([](Out<TSD<bool, TS<float>>>) {});

    const auto function = [](wp<TS<float>> left, wp<TS<float>>) { return left; };

    const auto graph_template = graph("name"_n = "map_key_types", function);

    testing::test_wiring([&] {
        CHECK_THROWS_WITH(map(function, integers(), booleans()),
                          "all map TSD and TSS arguments must have the same key type");
        CHECK_THROWS_WITH(map(graph_template, integers(), booleans()),
                          "all map TSD and TSS arguments must have the same key type");

        const auto arguments = std::array{
            MapArgument{WiringArgument{integers()}},
            MapArgument{WiringArgument{booleans()}},
        };
        const WiringTemplate &runtime_graph = graph_template;
        CHECK_THROWS_WITH(map(runtime_graph, std::span<const MapArgument>{arguments}),
                          "all map TSD and TSS arguments must have the same key type");
    });
}

TEST_CASE("map rejects a graph template with an unmatched argument count",
          "[feature:map][feature:wiring][series:tsd][definition:runtime]") {
    const auto values = node([](Out<TSD<i64, TS<float>>>) {});

    const auto two_parameters = graph(
        "name"_n = "two_map_parameters",
        [](wp<TS<float>> value, wp<TS<float>>) { return value; });

    testing::test_wiring([&] { CHECK_THROWS(map(two_parameters, values())); });
}

TEST_CASE("map requires one selected TSS for every graph form",
          "[feature:map][feature:wiring][series:tss][definition:function][definition:runtime]") {
    const auto first = node([](Out<TSS<i64>>) {});

    const auto second = node([](Out<TSS<i64>>) {});

    const auto function = [](wp<TSS<i64>>) {};

    const auto graph_template = graph("name"_n = "map_key_sets", function);

    testing::test_wiring([&] {
        CHECK_THROWS_WITH(map(function, first(), second()),
                          "a map call with multiple TSS arguments requires __key_set_arg__");
        CHECK_THROWS_WITH(map(graph_template, first(), second()),
                          "a map call with multiple TSS arguments requires __key_set_arg__");

        const auto arguments = std::array{
            MapArgument{WiringArgument{first()}},
            MapArgument{WiringArgument{second()}},
        };
        const WiringTemplate &runtime_graph = graph_template;
        CHECK_THROWS_WITH(map(runtime_graph, std::span<const MapArgument>{arguments}),
                          "a map call with multiple TSS arguments requires __key_set_arg__");

        CHECK_NOTHROW(map(function, first(), second(), "__key_set_arg__"_n = 1));
        CHECK_NOTHROW(map(graph_template, first(), second(), "__key_set_arg__"_n = 1));
    });
}

TEST_CASE("map passes reference items and pass-through collections consistently",
          "[feature:map][feature:wiring][feature:reference][series:tsd]"
          "[definition:function][definition:runtime][guarantee:final]") {
    const auto values =
        node([](Out<TSD<i64, REF<TS<float>, TimeSeriesGuarantee::final>>>) {});

    const auto factors = node([](Out<TSD<bool, TS<i64>>>) {});

    const auto function = [](wp<TS<float>> value, wp<TSD<bool, TS<i64>>>) { return value; };

    const auto graph_template = graph("name"_n = "map_reference_items", function);
    const auto &expected = TSD<
        i64,
        REF<TS<float>, TimeSeriesGuarantee::final>>::type();

    testing::test_wiring([&] {
        const auto callable_result = map(function, values(), pass_through(factors()));
        const auto graph_result = map(graph_template, values(), pass_through(factors()));

        const auto arguments = std::array{
            MapArgument{WiringArgument{values()}},
            MapArgument{WiringArgument{factors()}, true},
        };
        const WiringTemplate &runtime_graph = graph_template;
        const auto runtime_result = map(runtime_graph, std::span<const MapArgument>{arguments});

        CHECK(&callable_result.type() == &expected);
        CHECK(&graph_result.type() == &expected);
        CHECK(&runtime_result.type() == &expected);
    });
}

TEST_CASE("map reference guarantees follow branch lifetime",
          "[feature:map][feature:wiring][feature:reference][series:tsd][series:tss]") {
    const auto values = node([](Out<TSD<i64, TS<float>>>) {});
    const auto other_values = node([](Out<TSD<i64, TS<float>>>) {});
    const auto keys = node([](Out<TSS<i64>>) {});
    const auto reference_values = node([](Out<TSD<i64, REF<TS<float>, TimeSeriesGuarantee::final>>>) {});

    const auto const_item = graph(
        "name"_n = "const_map_item",
        [](wp<REF<TS<float>, TimeSeriesGuarantee::const_>> item) { return item; });
    const auto mutable_items = graph(
        "name"_n = "mutable_map_items",
        [](wp<REF<TS<float>, TimeSeriesGuarantee::ordinary>> item,
           wp<REF<TS<float>, TimeSeriesGuarantee::ordinary>>)
            -> wp<REF<TS<float>, TimeSeriesGuarantee::ordinary>> { return item; });
    const auto selected_items = graph(
        "name"_n = "selected_map_items",
        [](wp<REF<TS<float>, TimeSeriesGuarantee::const_>> selected, wp<REF<TS<float>>>) { return selected; });
    const auto final_item = graph(
        "name"_n = "final_map_item",
        [](wp<REF<TS<float>, TimeSeriesGuarantee::final>> item) { return item; });

    testing::test_wiring([&] {
        CHECK_NOTHROW(map(const_item, values()));
        CHECK_NOTHROW(map(mutable_items, values(), other_values()));
        CHECK_NOTHROW(map(selected_items, values(), other_values(), "__key_set_arg__"_n = 0));
        CHECK_NOTHROW(map([](wp<REF<TS<float>, TimeSeriesGuarantee::ordinary>> item)
                              -> wp<REF<TS<float>, TimeSeriesGuarantee::ordinary>> { return item; },
                          values(), keys(),
                          "__key_set_arg__"_n = 1));
        CHECK_THROWS(map(selected_items, values(), other_values()));
        CHECK_NOTHROW(map(final_item, reference_values()));
        CHECK_THROWS(map(const_item, reference_values()));

        const auto first_map = map([](auto item) { return item; }, values());
        CHECK_NOTHROW(map(const_item, first_map));
    });
}

TEST_CASE("a mutable root dictionary reference limits mapped item guarantees",
          "[feature:map][feature:wiring][feature:reference][series:tsd]") {
    const auto values = node([](Out<REF<TSD<i64, TS<float>>>>) {});
    const auto const_item = graph(
        "name"_n = "const_map_root_item",
        [](wp<REF<TS<float>, TimeSeriesGuarantee::const_>> item) { return item; });

    testing::test_wiring([&] { CHECK_THROWS(map(const_item, values())); });
}

TEST_CASE("map resolves explicit and inferred key arguments for every graph form",
          "[feature:map][feature:wiring][series:tsd][definition:function]"
          "[definition:runtime][guarantee:const]") {
    const auto values = node([](Out<TSD<i64, TS<float>>>) {});

    const auto function = [](wp<TS<i64, TimeSeriesGuarantee::const_>> key,
                             wp<TS<float>>) { return key; };

    const auto graph_template = graph("name"_n = "map_keys", function);
    const auto &expected = TSD<
        i64,
        REF<TS<i64, TimeSeriesGuarantee::const_>,
            TimeSeriesGuarantee::const_>>::type();

    testing::test_wiring([&] {
        CHECK(&map(function, values()).type() == &expected);
        CHECK(&map(graph_template, values(), "__key_arg__"_n = 0).type() == &expected);

        const auto arguments = std::array{
            MapArgument{WiringArgument{values()}},
            MapArgument{WiringArgument{util::symbol{"__key_arg__"}, i64{0}}},
        };
        const WiringTemplate &runtime_graph = graph_template;
        CHECK(&map(runtime_graph, std::span<const MapArgument>{arguments}).type() == &expected);
    });
}

TEST_CASE("map reports option errors through its public call forms",
          "[feature:map][feature:wiring][series:tsd][definition:function][definition:runtime]") {
    const auto values = node([](Out<TSD<i64, TS<float>>>) {});

    const auto function = [](wp<TS<float>> value) { return value; };

    const auto graph_template = graph("name"_n = "map_options", function);
    const auto sink = graph("name"_n = "map_option_sink", [](wp<TS<float>>) {});

    testing::test_wiring([&] {
        CHECK_THROWS_WITH(
            map(graph_template, values(), "__key_arg__"_n = 0, "__key_arg__"_n = 1),
            "a map call accepts one __key_arg__ option");
        CHECK_THROWS_WITH(
            map(graph_template, values(), "__key_set_arg__"_n = 0,
                "__key_set_arg__"_n = 0),
            "a map call accepts one __key_set_arg__ option");
        CHECK_THROWS_WITH(map(graph_template, values(), "__key_set_arg__"_n = -1),
                          "a map argument selector index must be non-negative");
        CHECK_THROWS_WITH(map(function, "value"_n = values(),
                              "__key_set_arg__"_n = "missing"),
                          "a map argument selector names an unknown argument");
        CHECK_THROWS_WITH(map(graph_template, values(), "__valid_items__"_n = i64{1}),
                          "__valid_items__ must be a present boolean scalar");
        CHECK_THROWS_WITH(map(graph_template, values(), "__valid_items__"_n = true,
                              "__valid_items__"_n = false),
                          "a map call accepts one __valid_items__ option");
        CHECK_THROWS_WITH(map(sink, values(), "__valid_items__"_n = true),
                          "__valid_items__ requires a mapped graph that produces an output");

        CHECK_NOTHROW(map(function, "value"_n = values(),
                          "__key_set_arg__"_n = "value"));

        const auto invalid_selector = std::array{
            MapArgument{WiringArgument{values()}},
            MapArgument{WiringArgument{util::symbol{"__key_arg__"}, true}},
        };
        const auto duplicate_key_arguments = std::array{
            MapArgument{WiringArgument{values()}},
            MapArgument{WiringArgument{util::symbol{"__key_arg__"}, i64{0}}},
            MapArgument{WiringArgument{util::symbol{"__key_arg__"}, i64{0}}},
        };
        const auto pass_through_option = std::array{
            MapArgument{WiringArgument{values()}},
            MapArgument{WiringArgument{util::symbol{"__key_set_arg__"}, i64{0}}, true},
        };
        const auto valid_items_option = std::array{
            MapArgument{WiringArgument{values()}},
            MapArgument{WiringArgument{util::symbol{"__valid_items__"}, true}},
        };
        const WiringTemplate &runtime_graph = graph_template;
        CHECK_THROWS_WITH(map(runtime_graph, std::span<const MapArgument>{invalid_selector}),
                          "a map argument selector must be an integer or a name");
        CHECK_THROWS_WITH(map(runtime_graph,
                              std::span<const MapArgument>{duplicate_key_arguments}),
                          "a map call accepts one __key_arg__ option");
        CHECK_THROWS_WITH(map(runtime_graph, std::span<const MapArgument>{pass_through_option}),
                          "a map option cannot be pass-through");
        CHECK_NOTHROW(map(runtime_graph, std::span<const MapArgument>{valid_items_option}));
    });
}

} // namespace
