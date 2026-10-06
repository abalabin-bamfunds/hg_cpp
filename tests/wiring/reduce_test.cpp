#include <bg/graph.hpp>
#include <bg/graph/details/nested.hpp>
#include <bg/node.hpp>
#include <bg/reduce.hpp>
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

const auto dictionary_source = node([](Out<TSD<std::int64_t, TS<std::int64_t>>>) {});
const auto final_item_dictionary_source =
    node([](Out<TSD<std::int64_t, TS<std::int64_t, TimeSeriesGuarantee::final>>>) {});
const auto add_values = node([](In<TS<std::int64_t>>, In<TS<std::int64_t>>, Out<TS<std::int64_t>>) {});
const auto add_with_context = node([](In<TS<std::int64_t>>, In<TS<std::int64_t>>,
                                      In<TS<std::int64_t>, "factor"_n, context>, Out<TS<std::int64_t>>) {});
const auto float_source = node([](Out<TS<float>>) {});
const auto reference_source = node([](Out<REF<TS<std::int64_t>>>) {});

TEST_CASE("reduce wiring records a binary graph over TSD item values",
          "[feature:reduce][feature:wiring][feature:graph-argument][series:tsd]") {
    const auto reducer = graph("name"_n = "add_values", [](wp<TS<std::int64_t>> left,
                                                              wp<TS<std::int64_t>> right) {
        return add_values(left, right);
    });

    testing::test_wiring([&] {
        const auto dictionary = dictionary_source();
        const auto &dictionary_type = type_cast<TimeSeriesDictionaryType>(dictionary.type());
        const auto built = details::build_reduce_graph_argument_wiring(
            reducer, dictionary_type.value_type(), TypeMap{});

        CHECK(built.structure.boundary_count() == 2);
        CHECK(built.graph.input_nodes[0] != built.graph.input_nodes[1]);
        CHECK(built.graph.output_node != built.graph.input_nodes[0]);
        CHECK(built.graph.output_node != built.graph.input_nodes[1]);

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

TEST_CASE("reduce wiring records a graph whose result forwards one of its inputs",
          "[feature:reduce][feature:wiring][feature:graph-argument][series:tsd]") {
    const auto first_value = graph("name"_n = "first_value", [](wp<TS<std::int64_t>> left,
                                                                 wp<TS<std::int64_t>>) { return left; });

    testing::test_wiring([&] {
        const auto dictionary = dictionary_source();
        const auto &dictionary_type = type_cast<TimeSeriesDictionaryType>(dictionary.type());
        const auto built = details::build_reduce_graph_argument_wiring(
            first_value, dictionary_type.value_type(), TypeMap{});

        CHECK(built.graph.output_node != built.graph.input_nodes[0]);
    });
}

TEST_CASE("reduce wiring rejects reducer graphs with a non-item result",
          "[feature:reduce][feature:wiring][feature:graph-argument][series:tsd]") {
    const auto wrong_result = graph("name"_n = "wrong_result", [](wp<TS<std::int64_t>>,
                                                                    wp<TS<std::int64_t>>) {
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
    const auto reference_result = graph("name"_n = "reference_result", [](wp<TS<std::int64_t>>,
                                                                              wp<TS<std::int64_t>>) {
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
    const auto wrong_arity = graph("name"_n = "wrong_arity", [](wp<TS<std::int64_t>> value) { return value; });

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
    const auto reducer = graph("name"_n = "context_reducer", [](wp<TS<std::int64_t>> left,
                                                                   wp<TS<std::int64_t>> right) {
        return add_with_context(left, right);
    });

    testing::test_wiring([&] {
        const auto dictionary = dictionary_source();
        const auto context_source = node([](Out<TS<std::int64_t>>) {});
        auto scope = with_context("factor"_n = context_source());
        const auto &dictionary_type = type_cast<TimeSeriesDictionaryType>(dictionary.type());
        const auto built = details::build_reduce_graph_argument_wiring(
            reducer, dictionary_type.value_type(), TypeMap{});

        CHECK(built.graph.wiring->inherited_requirements().size() == 1);
        CHECK(built.structure.cacheable());
    });
}

TEST_CASE("an isolated compact reducer rejects context requirements without changing outer wiring",
          "[feature:reduce][feature:wiring][feature:context-time-series][series:tsd]") {
    const auto representation_sensitive = graph(
        "name"_n = "representation_sensitive_context_reducer",
        [](wp<TS<std::int64_t>> left, wp<TS<std::int64_t>> right) {
            if (type_cast<TimeSeriesReferenceType>(&left.type()) == nullptr) {
                return add_with_context(left, right);
            }
            return add_values(left, right);
        });

    testing::test_wiring([&] {
        const auto dictionary = dictionary_source();
        const auto context_source = node([](Out<TS<std::int64_t>>) {});
        auto context = with_context("factor"_n = context_source());
        auto &outer = WiringContext::current().wiring();
        const auto &dictionary_type = type_cast<TimeSeriesDictionaryType>(dictionary.type());

        const auto built = details::build_reduce_graph_argument_wiring(
            representation_sensitive, dictionary_type.value_type(), TypeMap{});

        CHECK_FALSE(built.graph.compact_graph.has_value());
        CHECK(built.graph.wiring->inherited_requirements().empty());
        CHECK(outer.inherited_requirements().empty());
    });
}

TEST_CASE("an isolated compact reducer rejects service requirements without changing outer wiring",
          "[feature:reduce][feature:wiring][feature:service][series:tsd]") {
    const auto lookup = reference_service<auto()->TS<std::int64_t>>{"reduce_lookup"};
    const auto representation_sensitive = graph(
        "name"_n = "representation_sensitive_service_reducer",
        [&](wp<TS<std::int64_t>> left, wp<TS<std::int64_t>> right) {
            if (type_cast<TimeSeriesReferenceType>(&left.type()) != nullptr) {
                return add_values(left, right);
            }
            return lookup().as<TS<std::int64_t>>();
        });

    testing::test_wiring([&] {
        const auto dictionary = dictionary_source();
        auto &outer = WiringContext::current().wiring();
        const auto &dictionary_type = type_cast<TimeSeriesDictionaryType>(dictionary.type());

        const auto built = details::build_reduce_graph_argument_wiring(
            representation_sensitive, dictionary_type.value_type(), TypeMap{});

        CHECK_FALSE(built.graph.compact_graph.has_value());
        CHECK(built.graph.wiring->inherited_requirements().empty());
        CHECK(outer.service_requirements() == nullptr);
    });
}

TEST_CASE("reduce wires its TSD input and output type through the default overload",
          "[feature:reduce][feature:operator][feature:graph-argument][feature:wiring][series:tsd]") {
    const auto add = node([](In<TS<std::int64_t>> left, In<TS<std::int64_t>> right,
                             Out<TS<std::int64_t>> output) {});
    const auto reducer = graph("name"_n = "add_for_reduce", [add](wp<TS<std::int64_t>> left,
                                                                     wp<TS<std::int64_t>> right) {
        return add(left, right);
    });

    testing::test_wiring([&] {
        const auto dictionary = dictionary_source();
        const auto result = reduce(reducer, dictionary);
        CHECK(&type_cast<TimeSeriesType>(result.type()) ==
              &type_cast<TimeSeriesDictionaryType>(dictionary.type()).value_type());
    });
}

TEST_CASE("reduce accepts final TSD items and keeps an ordinary output guarantee",
          "[feature:reduce][feature:operator][feature:wiring][series:tsd][guarantee:final]") {
    const auto reducer = graph("name"_n = "add_final_items",
                                [](wp<TS<std::int64_t>> left, wp<TS<std::int64_t>> right) {
                                    return add_values(left, right);
                                });

    testing::test_wiring([&] {
        const auto dictionary = final_item_dictionary_source();
        const auto &dictionary_type = type_cast<TimeSeriesDictionaryType>(dictionary.type());
        const auto built = details::build_reduce_graph_argument_wiring(
            reducer, dictionary_type.value_type().target_type(), TypeMap{});
        CHECK(built.graph.compact_graph.has_value());

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
