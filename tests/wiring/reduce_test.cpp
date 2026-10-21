#include <bg/graph.hpp>
#include <bg/graph/compiled_graph.hpp>
#include <bg/graph/details/nested.hpp>
#include <bg/node.hpp>
#include <bg/reduce.hpp>
#include <bg/reduce/details/default_reduce_wiring.hpp>
#include <bg/reduce/details/reduce_graph_argument.hpp>
#include <bg/service.hpp>
#include <bg/testing/test_wiring.hpp>
#include <bg/time_series/tsd/tsd_type.hpp>
#include <bg/type/time_series_dict_type.hpp>
#include <bg/type/time_series_reference_type.hpp>
#include <bg/type/time_series_value_type.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>

namespace {

using namespace bg;
using namespace bg::literals;

const auto dictionary_source = node([](Out<TSD<i64, TS<i64>>>) {});

const auto reference_dictionary_source = node([](Out<REF<TSD<i64, TS<i64>>>>) {});
const auto final_item_dictionary_source =
    node([](Out<TSD<i64, TS<i64, TimeSeriesGuarantee::final>>>) {});

const auto add_values = node([](In<TS<i64>>, In<TS<i64>>, Out<TS<i64>>) {});

const auto add_with_context = node([](In<TS<i64>>, In<TS<i64>>,
                                      In<TS<i64>, "factor"_n, context>, Out<TS<i64>>) {});

const auto float_source = node([](Out<TS<float>>) {});

const auto reference_source = node([](Out<REF<TS<i64>>>) {});

TEST_CASE("reduce wiring records a binary graph over TSD item values",
          "[feature:reduce][feature:wiring][feature:graph-argument][series:tsd]") {
    const auto reducer = graph("name"_n = "add_values", [](wp<TS<i64>> left,
                                                              wp<TS<i64>> right) {
        return add_values(left, right);
    });

    testing::test_wiring([&] {
        const auto dictionary = dictionary_source();
        const auto &dictionary_type = type_cast<TimeSeriesDictionaryType>(dictionary.type());
        const auto built = details::build_reduce_graph_argument_wiring(
            reducer, dictionary_type.value_type(), TypeMap{});

        CHECK(built.structure.boundary_count() == 2);
        CHECK(built.graph.nested.input_nodes[0] != built.graph.nested.input_nodes[1]);
        CHECK(built.graph.nested.output_node != built.graph.nested.input_nodes[0]);
        CHECK(built.graph.nested.output_node != built.graph.nested.input_nodes[1]);

        const auto *left_reference = type_cast<TimeSeriesReferenceType>(&built.structure.boundary_type(0));
        const auto *right_reference = type_cast<TimeSeriesReferenceType>(&built.structure.boundary_type(1));
        REQUIRE(left_reference != nullptr);
        REQUIRE(right_reference != nullptr);
        CHECK(&left_reference->target_type() == &dictionary_type.value_type());
        CHECK(&right_reference->target_type() == &dictionary_type.value_type());
        REQUIRE(built.structure.result_type() != nullptr);
        const auto *result_value = type_cast<TimeSeriesValueType>(built.structure.result_type());
        REQUIRE(result_value != nullptr);
        CHECK(&result_value->value_type() ==
              &type_cast<TimeSeriesValueType>(dictionary_type.value_type()).value_type());
    });
}

TEST_CASE("reduce selects nested wiring for a reference-wrapped TSD",
          "[feature:reduce][feature:wiring][series:tsd][series:reference][shape:nested]") {
    const auto reducer = graph("name"_n = "add_values", [](wp<TS<i64>> left,
                                                              wp<TS<i64>> right) {
        return add_values(left, right);
    });

    testing::test_wiring([&] {
        const auto dictionary = reference_dictionary_source();
        const auto argument = details::build_reduce_graph_argument(
            reducer, WiringArgument{dictionary}, std::nullopt, TypeMap{});
        const auto &built = details::DefaultReduceWiring::from(argument);

        CHECK(built.graph.nested.wiring != nullptr);
        CHECK_FALSE(built.graph.compact_graph.has_value());
    });
}

TEST_CASE("reduce wiring records a graph whose result forwards one of its inputs",
          "[feature:reduce][feature:wiring][feature:graph-argument][series:tsd]") {
    const auto first_value = graph("name"_n = "first_value", [](wp<TS<i64>> left,
                                                                 wp<TS<i64>>) { return left; });

    testing::test_wiring([&] {
        const auto dictionary = dictionary_source();
        const auto &dictionary_type = type_cast<TimeSeriesDictionaryType>(dictionary.type());
        const auto built = details::build_reduce_graph_argument_wiring(
            first_value, dictionary_type.value_type(), TypeMap{});

        REQUIRE(built.graph.compact_graph.has_value());
        CHECK_FALSE(built.graph.compact_graph->output_node.has_value());
        CHECK(built.graph.compact_graph->output_input.has_value());
        CHECK(built.graph.compact_graph->output_input == std::optional<std::size_t>{0});
        CHECK(built.graph.compact_graph->graph->borrowed_input_bindings().size() == 2);
        CHECK(built.graph.nested.output_node != built.graph.nested.input_nodes[0]);
    });
}

TEST_CASE("reduce wiring rejects reducer graphs with a non-item result",
          "[feature:reduce][feature:wiring][feature:graph-argument][series:tsd]") {
    const auto wrong_result = graph("name"_n = "wrong_result", [](wp<TS<i64>>,
                                                                    wp<TS<i64>>) {
        return float_source();
    });

    testing::test_wiring([&] {
        const auto dictionary = dictionary_source();
        const auto &dictionary_type = type_cast<TimeSeriesDictionaryType>(dictionary.type());
        CHECK_THROWS_AS(details::build_reduce_graph_argument_wiring(
                            wrong_result, dictionary_type.value_type(), TypeMap{}),
                        WiringError);
    });
}

TEST_CASE("reduce wiring rejects a reference as the reducer result",
          "[feature:reduce][feature:wiring][feature:graph-argument][series:tsd]") {
    const auto reference_result = graph("name"_n = "reference_result", [](wp<TS<i64>>,
                                                                              wp<TS<i64>>) {
        return reference_source();
    });

    testing::test_wiring([&] {
        const auto dictionary = dictionary_source();
        const auto &dictionary_type = type_cast<TimeSeriesDictionaryType>(dictionary.type());
        CHECK_THROWS_AS(details::build_reduce_graph_argument_wiring(
                            reference_result, dictionary_type.value_type(), TypeMap{}),
                        WiringError);
    });
}

TEST_CASE("reduce wiring rejects reducer graphs with the wrong input type",
          "[feature:reduce][feature:wiring][feature:graph-argument][series:tsd]") {
    const auto wrong_input = graph("name"_n = "wrong_input", [](wp<TS<float>> left, wp<TS<float>> right) {
        return left;
    });

    testing::test_wiring([&] {
        const auto dictionary = dictionary_source();
        const auto &dictionary_type = type_cast<TimeSeriesDictionaryType>(dictionary.type());
        CHECK_THROWS(details::build_reduce_graph_argument_wiring(
            wrong_input, dictionary_type.value_type(), TypeMap{}));
    });
}

TEST_CASE("reduce wiring rejects reducer graphs with other than two inputs",
          "[feature:reduce][feature:wiring][feature:graph-argument][series:tsd]") {
    const auto wrong_arity = graph("name"_n = "wrong_arity", [](wp<TS<i64>> value) { return value; });

    testing::test_wiring([&] {
        const auto dictionary = dictionary_source();
        const auto &dictionary_type = type_cast<TimeSeriesDictionaryType>(dictionary.type());
        CHECK_THROWS_AS(details::build_reduce_graph_argument_wiring(
                            wrong_arity, dictionary_type.value_type(), TypeMap{}),
                        WiringError);
    });
}

TEST_CASE("reduce wiring preserves inherited context requirements",
          "[feature:reduce][feature:wiring][feature:context-time-series][series:tsd]") {
    const auto reducer = graph("name"_n = "context_reducer", [](wp<TS<i64>> left,
                                                                   wp<TS<i64>> right) {
        return add_with_context(left, right);
    });

    testing::test_wiring([&] {
        const auto dictionary = dictionary_source();

        const auto context_source = node([](Out<TS<i64>>) {});
        auto scope = with_context("factor"_n = context_source());
        const auto &dictionary_type = type_cast<TimeSeriesDictionaryType>(dictionary.type());
        const auto built = details::build_reduce_graph_argument_wiring(
            reducer, dictionary_type.value_type(), TypeMap{});

        CHECK(built.graph.nested.wiring->inherited_requirements().size() == 1);
        CHECK(built.structure.cacheable());
    });
}

TEST_CASE("an isolated compact reducer rejects context requirements without changing outer wiring",
          "[feature:reduce][feature:wiring][feature:context-time-series][series:tsd]") {
    const auto representation_sensitive = graph(
        "name"_n = "representation_sensitive_context_reducer",
        [](wp<TS<i64>> left, wp<TS<i64>> right) {
            if (type_cast<TimeSeriesReferenceType>(&left.type()) == nullptr) {
                return add_with_context(left, right);
            }
            return add_values(left, right);
        });

    testing::test_wiring([&] {
        const auto dictionary = dictionary_source();

        const auto context_source = node([](Out<TS<i64>>) {});
        auto context = with_context("factor"_n = context_source());
        auto &outer = WiringContext::current().wiring();
        const auto &dictionary_type = type_cast<TimeSeriesDictionaryType>(dictionary.type());

        const auto built = details::build_reduce_graph_argument_wiring(
            representation_sensitive, dictionary_type.value_type(), TypeMap{});

        CHECK_FALSE(built.graph.compact_graph.has_value());
        CHECK(built.graph.nested.wiring->inherited_requirements().empty());
        CHECK(outer.inherited_requirements().empty());
    });
}

TEST_CASE("an isolated compact reducer rejects service requirements without changing outer wiring",
          "[feature:reduce][feature:wiring][feature:service][series:tsd]") {
    const auto lookup = reference_service<auto()->TS<i64>>{"reduce_lookup"};

    const auto representation_sensitive = graph(
        "name"_n = "representation_sensitive_service_reducer",
        [&](wp<TS<i64>> left, wp<TS<i64>> right) {
            if (type_cast<TimeSeriesReferenceType>(&left.type()) != nullptr) {
                return add_values(left, right);
            }
            return lookup().as<TS<i64>>();
        });

    testing::test_wiring([&] {
        const auto dictionary = dictionary_source();
        auto &outer = WiringContext::current().wiring();
        const auto &dictionary_type = type_cast<TimeSeriesDictionaryType>(dictionary.type());

        const auto built = details::build_reduce_graph_argument_wiring(
            representation_sensitive, dictionary_type.value_type(), TypeMap{});

        CHECK_FALSE(built.graph.compact_graph.has_value());
        CHECK(built.graph.nested.wiring->inherited_requirements().empty());
        CHECK(outer.service_instances() == nullptr);
    });
}

TEST_CASE("reduce wires its TSD input and output type through the default overload",
          "[feature:reduce][feature:operator][feature:graph-argument][feature:wiring][series:tsd]") {
    const auto add = node([](In<TS<i64>> left, In<TS<i64>> right,
                             Out<TS<i64>> output) {});

    const auto reducer = graph("name"_n = "add_for_reduce", [add](wp<TS<i64>> left,
                                                                     wp<TS<i64>> right) {
        return add(left, right);
    });

    testing::test_wiring([&] {
        auto &wiring = WiringContext::current().wiring();
        const auto dictionary = dictionary_source();
        const auto result = reduce(reducer, dictionary);
        wiring.finalize();
        CHECK(wiring.nodes().size() == 3);
        CHECK(&type_cast<TimeSeriesType>(result.type()) ==
              &type_cast<TimeSeriesDictionaryType>(dictionary.type()).value_type());
    });
}

TEST_CASE("reduce accepts final TSD items and keeps an ordinary output guarantee",
          "[feature:reduce][feature:operator][feature:wiring][series:tsd][guarantee:final]") {
    const auto reducer = graph("name"_n = "add_final_items",
                                [](wp<TS<i64>> left, wp<TS<i64>> right) {
                                    return add_values(left, right);
                                });

    testing::test_wiring([&] {
        const auto dictionary = final_item_dictionary_source();
        const auto &dictionary_type = type_cast<TimeSeriesDictionaryType>(dictionary.type());
        const auto built = details::build_reduce_graph_argument_wiring(
            reducer, dictionary_type.value_type().target_type(), TypeMap{});
        CHECK_FALSE(built.graph.compact_graph.has_value());

        const auto result = reduce(reducer, dictionary);
        const auto *result_type = type_cast<TimeSeriesValueType>(&result.type());

        REQUIRE(result_type != nullptr);
        CHECK(result_type->guarantee().value() == TimeSeriesGuarantee::ordinary);
        const auto *item_type = type_cast<TimeSeriesValueType>(
            &type_cast<TimeSeriesDictionaryType>(dictionary.type()).value_type());
        REQUIRE(item_type != nullptr);
        CHECK(&result_type->value_type() == &item_type->value_type());
    });
}

} // namespace
