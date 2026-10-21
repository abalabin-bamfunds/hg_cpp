#include <bg/graph.hpp>
#include <bg/graph/details/dispatch_cast.hpp>
#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/graph/wiring_error.hpp>
#include <bg/node.hpp>
#include <bg/operators.hpp>
#include <bg/scalar/bundle/value.hpp>
#include <bg/switch.hpp>
#include <bg/testing/test_graph.hpp>
#include <bg/time_series/time_series_variable.hpp>
#include <bg/type/time_series_reference_type.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>

namespace {

using namespace bg;
using namespace bg::literals;

template <template <typename, auto...> typename Field> struct DispatchBase {
    Field<symbol> name;
};

template <template <typename, auto...> typename Field>
struct DispatchDerived : schema_extends<DispatchBase, Field> {
    Field<i64> value;
};

TEST_CASE("dispatch casts preserve direct input bindings", "[feature:wiring][feature:dispatch]") {
    auto wiring = GraphWiring{};
    auto input = WiringPoint{};
    auto cast = WiringPoint{};
    const auto &source_type = TS<Bundle<DispatchBase>>::type();
    const auto &target_type = TS<Bundle<DispatchDerived>>::type();

    {
        auto context = WiringContext{wiring};
        input = wiring.borrowed_input(source_type);
        cast = details::dispatch_cast(input, target_type);
        CHECK(&cast.type() == &target_type);
        CHECK_THROWS_AS(input.as<TS<Bundle<DispatchDerived>>>(), WiringError);

        const auto binding = wiring.wire_input(cast, target_type);
        CHECK(binding.is_borrowed());
        CHECK(binding.index() == 0);
        CHECK_FALSE(binding.dereferences_reference());
    }

    CHECK(wiring.nodes().empty());
}

TEST_CASE("dispatch casts preserve reference bindings for reference and value inputs",
          "[feature:wiring][feature:dispatch][feature:reference]") {
    const auto value_source = node([](Out<TS<Bundle<DispatchBase>>>) {});
    const auto reference_pass = node([](In<REF<TS<Bundle<DispatchBase>>>> input,
                                        Out<REF<TS<Bundle<DispatchBase>>>> output) { output.copy(input); });
    const auto reference_sink = node([](In<REF<TS<Bundle<DispatchDerived>>>>) {});
    const auto value_sink = node([](In<TS<Bundle<DispatchDerived>>>) {});
    auto wiring = GraphWiring{};
    auto value_output = WiringPoint{};
    auto reference_output = WiringPoint{};

    {
        auto context = WiringContext{wiring};
        value_output = value_source();
        reference_output = reference_pass(value_output);
        const auto *source_reference = type_cast<TimeSeriesReferenceType>(&reference_output.type());
        const auto &target_type = time_series_reference_type(
            TS<Bundle<DispatchDerived>>::type(), source_reference->guarantee().value().value());
        const auto cast = details::dispatch_cast(reference_output, target_type);
        (void)reference_sink(cast);
        (void)value_sink(cast);
    }

    (void)wiring.complete_pending_wiring();

    REQUIRE(wiring.nodes().size() == 5);
    const auto reference_sink_node = std::ranges::find_if(wiring.nodes(), [&](const auto &candidate) {
        return &candidate.node_template() == &reference_sink;
    });
    const auto value_sink_node = std::ranges::find_if(wiring.nodes(), [&](const auto &candidate) {
        return &candidate.node_template() == &value_sink;
    });
    REQUIRE(value_sink_node != wiring.nodes().end());
    REQUIRE(reference_sink_node != wiring.nodes().end());
    CHECK(reference_sink_node->inputs()[0].is_output());
    CHECK(reference_sink_node->inputs()[0].index() == wiring.output_index(reference_output));
    CHECK_FALSE(reference_sink_node->inputs()[0].dereferences_reference());
    CHECK(value_sink_node->inputs()[0].is_output());
    CHECK(value_sink_node->inputs()[0].index() == wiring.output_index(reference_output));
    CHECK(value_sink_node->inputs()[0].dereferences_reference());
}

TEST_CASE("dispatch casts work inside nested switch cases",
          "[feature:dispatch][feature:switch][feature:nested-graph][series:value]") {
    auto memory = allocator{};
    const auto value = bundle<DispatchDerived>(memory, symbol{"item"}, i64{42});
    const auto publish = node([](i64, Scalar<Bundle<DispatchDerived>> value,
                                 Out<TS<Bundle<DispatchBase>>> output) { output.set(value); });
    const auto read_derived = node([](In<TS<Bundle<DispatchDerived>>> input) {
        const auto derived = scalar_cast<Bundle<DispatchDerived>>(input.value());
        return scalar_cast<i64>(derived[1]).value();
    });

    const auto leaf = graph("name"_n = "dispatch_cast_leaf", [&](wp<TS<Bundle<DispatchBase>>> input) {
        const auto *source_reference = type_cast<TimeSeriesReferenceType>(&input.type());
        const auto &target_type = time_series_reference_type(
            TS<Bundle<DispatchDerived>>::type(), source_reference->guarantee().value().value());
        return read_derived(details::dispatch_cast(input, target_type));
    });
    const auto middle = graph("name"_n = "dispatch_cast_middle", [&](wp<TS<Bundle<DispatchBase>>> input) {
        return switch_(operators::const_(i64{1}), cases{case_(1, leaf)}, input).as<TS<i64>>();
    });
    const auto tested = [&](wp<TS<i64>> trigger, wp<TS<i64>> selector) {
        const auto source = publish(trigger, value);
        return switch_(selector, cases{case_(1, middle)}, source).as<TS<i64>>();
    };

    const auto actual = testing::test_graph(tested, {1}, {1});

    CHECK(actual.equals({42}));
}

TEST_CASE("a narrowed reference cast can feed a value input",
          "[feature:dispatch][feature:reference][binding:reference-to-value][series:value]") {
    auto memory = allocator{};
    const auto value = bundle<DispatchDerived>(memory, symbol{"item"}, i64{42});
    const auto publish = node([](In<TS<i64>>, Scalar<Bundle<DispatchDerived>> value,
                                 Out<TS<Bundle<DispatchBase>>> output) { output.set(value); });
    const auto reference_pass = node([](In<REF<TS<Bundle<DispatchBase>>>> input,
                                        Out<REF<TS<Bundle<DispatchBase>>>> output) { output.copy(input); });
    const auto read_derived = node([](In<TS<Bundle<DispatchDerived>>> input) {
        const auto derived = scalar_cast<Bundle<DispatchDerived>>(input.value());
        return scalar_cast<i64>(derived[1]).value();
    });
    const auto tested = [&](wp<TS<i64>> trigger) {
        const auto source = publish(trigger, value);
        const auto reference = reference_pass(source);
        const auto *source_reference = type_cast<TimeSeriesReferenceType>(&reference.type());
        const auto &target_type = time_series_reference_type(
            TS<Bundle<DispatchDerived>>::type(), source_reference->guarantee().value().value());
        const auto cast = details::dispatch_cast(reference, target_type);
        return read_derived(cast);
    };

    const auto actual = testing::test_graph(tested, {1});

    CHECK(actual.equals({42}));
}

TEST_CASE("a widened dispatch branch can advertise the switch output type",
          "[feature:dispatch][feature:switch][feature:nested-graph][series:value]") {
    auto memory = allocator{};
    const auto value = bundle<DispatchDerived>(memory, symbol{"item"}, i64{42});
    const auto publish_derived = node([](In<TS<i64>>, Scalar<Bundle<DispatchDerived>> value,
                                         Out<TS<Bundle<DispatchDerived>>> output) { output.set(value); });
    const auto base_case = graph("name"_n = "dispatch_cast_base_case",
                                 [](wp<TS<Bundle<DispatchBase>>> input) {
        const auto *source_reference = type_cast<TimeSeriesReferenceType>(&input.type());
        const auto &target_type = time_series_reference_type(
            TS<Bundle<DispatchBase>>::type(), source_reference->guarantee().value().value());
        return details::dispatch_cast(input, target_type).as<REF<TS<Bundle<DispatchBase>>>>();
    });
    const auto derived_case = graph("name"_n = "dispatch_cast_derived_case",
                                    [](wp<TS<Bundle<DispatchDerived>>> input) {
        const auto *source_reference = type_cast<TimeSeriesReferenceType>(&input.type());
        const auto &target_type = time_series_reference_type(
            TS<Bundle<DispatchBase>>::type(), source_reference->guarantee().value().value());
        return details::dispatch_cast(input, target_type).as<REF<TS<Bundle<DispatchBase>>>>();
    });
    const auto read_base = node([](In<TS<Bundle<DispatchBase>>> input) {
        const auto base = scalar_cast<Bundle<DispatchBase>>(input.value());
        return scalar_cast<symbol>(base[0]).value().text() == "item";
    });
    const auto tested = [&](wp<TS<i64>> trigger, wp<TS<i64>> selector) {
        const auto source = publish_derived(trigger, value);
        const auto selected = switch_(selector,
                                     cases{case_(i64{1}, base_case), case_(i64{2}, derived_case)}, source);
        return read_base(selected);
    };

    const auto actual = testing::test_graph(tested, {1, 1}, {1, 2});

    CHECK(actual.equals({true, true}));
}

} // namespace
