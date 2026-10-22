#include <bg/dispatch.hpp>
#include <bg/graph.hpp>
#include <bg/graph/wiring_error.hpp>
#include <bg/node.hpp>
#include <bg/operators.hpp>
#include <bg/scalar/bundle/value.hpp>
#include <bg/testing/test_graph.hpp>
#include <bg/time_series/tsb/tsb.hpp>
#include <bg/time_series/time_series_variable.hpp>
#include <bg/type/time_series_reference_type.hpp>
#include <bg/type/time_series_value_type.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <memory>
#include <stdexcept>
#include <vector>

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

template <template <typename, auto...> typename Field> struct DispatchOtherBase {
    Field<symbol> name;
};

template <template <typename, auto...> typename Field>
struct DispatchOtherDerived : schema_extends<DispatchOtherBase, Field> {
    Field<i64> value;
};

struct DispatchGraphBoundary {
    static constexpr bool deduplicate_wiring = false;
    Out<TS<i64>> output;
    void evaluate() const {}
};

TEST_CASE("dispatch selects the most specific implementation for a runtime scalar type",
          "[feature:dispatch][feature:nested-graph][series:value]") {
    const auto base_implementation = graph("name"_n = "dispatch_generic", [](wp<TS<SCALAR>> input) {
        (void)input;
        return operators::const_(i64{10}).as<TS<i64, TimeSeriesGuarantee::const_>>();
    });
    const auto derived_implementation = node([](In<TS<Bundle<DispatchDerived>>>) { return i64{20}; });

    using DispatchSignature = auto(TS<Bundle<DispatchBase>>) -> i64;
    const auto dispatch = dispatch_<DispatchSignature>{base_implementation, derived_implementation};
    const auto publish_derived = node([](In<TS<i64>>, Scalar<Bundle<DispatchDerived>> value,
                                         Out<TS<Bundle<DispatchBase>>> output) { output.set(value); });

    auto memory = allocator{};
    const auto derived_value = bundle<DispatchDerived>(memory, symbol{"item"}, i64{42});
    auto output_is_reference_to_i64 = false;
    const auto derived_graph =
        [&](wp<TS<i64>> trigger) {
            const auto source = publish_derived(trigger, derived_value).as<TS<Bundle<DispatchBase>>>();
            const auto output = dispatch(source);
            const auto *reference = type_cast<TimeSeriesReferenceType>(&output.type());
            const auto *target = reference == nullptr
                                     ? nullptr
                                     : type_cast<TimeSeriesValueType>(&reference->target_type());
            output_is_reference_to_i64 = target != nullptr && &target->value_type() == &int64_type();
            return output.as<TS<i64>>();
        };
    const auto derived_result = testing::test_graph(derived_graph, {1, 1});

    CHECK(output_is_reference_to_i64);
    REQUIRE(derived_result.size() == 2);
    REQUIRE(derived_result[0].has_value());
    CHECK(scalar_cast<i64>(derived_result[0]->view()).value() == 20);
    REQUIRE(derived_result[1].has_value());
    CHECK(scalar_cast<i64>(derived_result[1]->view()).value() == 20);

    const auto base_value = bundle<DispatchBase>(memory, symbol{"item"});
    const auto publish_base = node([](In<TS<i64>>, Scalar<Bundle<DispatchBase>> value,
                                      Out<TS<Bundle<DispatchBase>>> output) { output.set(value); });
    const auto base_graph =
        [&](wp<TS<i64>> trigger) {
            const auto source = publish_base(trigger, base_value).as<TS<Bundle<DispatchBase>>>();
            return dispatch(source).as<TS<i64>>();
        };
    const auto base_result = testing::test_graph(base_graph, {1, 1});

    REQUIRE(base_result.size() == 2);
    REQUIRE(base_result[0].has_value());
    CHECK(scalar_cast<i64>(base_result[0]->view()).value() == 10);
    CHECK_FALSE(base_result[1].has_value());
}

TEST_CASE("dispatch passes graph arguments to the selected implementation",
          "[feature:dispatch][feature:switch][feature:graph-argument][feature:wiring]") {
    auto graph_wiring = GraphWiring{};
    const auto graph_context = WiringContext{graph_wiring};
    const auto graph_boundary = node<DispatchGraphBoundary>();
    const auto graph_input = graph_boundary();
    const auto graph_boundaries = std::array<WiringPoint, 1>{graph_input};
    (void)graph_wiring.complete_pending_wiring();
    const auto graph_argument = GraphArgument{
        details::graph_wiring_structure(graph_wiring, graph_boundaries, static_cast<WiringPoint>(graph_input)),
        std::make_shared<const int>(1)};
    auto received_graph_argument = false;
    const auto implementation = graph(
        "name"_n = "dispatch_graph_argument_implementation",
        [&](wp<TS<Bundle<DispatchBase>>> input, GraphArgument function) {
            const auto payload = function.payload<int>();
            received_graph_argument = function.structure().boundary_count() == 1 && payload && *payload == 1;
            (void)input;
            return operators::const_(i64{17}).as<TS<i64, TimeSeriesGuarantee::const_>>();
        });
    const auto dispatch = dispatch_<auto(TS<Bundle<DispatchBase>>, GRAPH_T) -> TS<i64>>{
        implementation};
    auto memory = allocator{};
    const auto value = bundle<DispatchBase>(memory, symbol{"item"});
    const auto publish = node([](In<TS<i64>>, Scalar<Bundle<DispatchBase>> value,
                                 Out<TS<Bundle<DispatchBase>>> output) { output.set(value); });
    const auto tested = [&](wp<TS<i64>> trigger) {
        return dispatch(publish(trigger, value), graph_argument).as<TS<i64>>();
    };

    CHECK(testing::test_graph(tested, {1}).equals<i64>({17}));
    CHECK(received_graph_argument);
}

TEST_CASE("dispatch selects a registered input-type tuple",
          "[feature:dispatch][feature:nested-graph][series:value][binding:covariant]") {
    const auto base_base = graph("name"_n = "dispatch_base_base",
                                 [](wp<TS<Bundle<DispatchBase>>> first,
                                    wp<TS<Bundle<DispatchOtherBase>>>) {
                                     (void)first;
                                     return operators::const_(i64{10})
                                         .as<TS<i64, TimeSeriesGuarantee::const_>>();
                                 });
    const auto derived_base = graph("name"_n = "dispatch_derived_base",
                                    [](wp<TS<Bundle<DispatchDerived>>> first,
                                       wp<TS<Bundle<DispatchOtherBase>>>) {
                                        (void)first;
                                        return operators::const_(i64{20})
                                            .as<TS<i64, TimeSeriesGuarantee::const_>>();
                                    });
    const auto base_derived = graph("name"_n = "dispatch_base_derived",
                                    [](wp<TS<Bundle<DispatchBase>>> first,
                                       wp<TS<Bundle<DispatchOtherDerived>>>) {
                                        (void)first;
                                        return operators::const_(i64{30})
                                            .as<TS<i64, TimeSeriesGuarantee::const_>>();
                                    });
    const auto derived_derived = graph("name"_n = "dispatch_derived_derived",
                                       [](wp<TS<Bundle<DispatchDerived>>> first,
                                          wp<TS<Bundle<DispatchOtherDerived>>>) {
                                           (void)first;
                                           return operators::const_(i64{40})
                                               .as<TS<i64, TimeSeriesGuarantee::const_>>();
                                       });

    using DispatchSignature = auto(TS<Bundle<DispatchBase>>, TS<Bundle<DispatchOtherBase>>) -> TS<i64>;
    const auto dispatch = dispatch_<DispatchSignature>{base_base, derived_base, base_derived, derived_derived};
    const auto publish_first = node([](In<TS<i64>>, Scalar<Bundle<DispatchDerived>> value,
                                       Out<TS<Bundle<DispatchBase>>> output) { output.set(value); });
    const auto publish_second = node([](In<TS<i64>>, Scalar<Bundle<DispatchOtherDerived>> value,
                                        Out<TS<Bundle<DispatchOtherBase>>> output) { output.set(value); });

    auto memory = allocator{};
    const auto first_value = bundle<DispatchDerived>(memory, symbol{"first"}, i64{1});
    const auto second_value = bundle<DispatchOtherDerived>(memory, symbol{"second"}, i64{2});
    const auto graph = [&](wp<TS<i64>> trigger) {
        const auto first = publish_first(trigger, first_value).as<TS<Bundle<DispatchBase>>>();
        const auto second = publish_second(trigger, second_value).as<TS<Bundle<DispatchOtherBase>>>();
        return dispatch(first, second).as<TS<i64>>();
    };
    const auto result = testing::test_graph(graph, {1, 1});

    REQUIRE(result.size() == 2);
    REQUIRE(result[0].has_value());
    CHECK(scalar_cast<i64>(result[0]->view()).value() == 40);
    CHECK_FALSE(result[1].has_value());
}

TEST_CASE("dispatch expands union input forms across selected inputs",
          "[feature:dispatch][feature:type-system][binding:covariant]") {
    using FirstForm = Union<"FIRST_FORM", TS<Bundle<DispatchBase>>, TS<Bundle<DispatchDerived>>>;
    using SecondForm =
        Union<"SECOND_FORM", TS<Bundle<DispatchOtherBase>>, TS<Bundle<DispatchOtherDerived>>>;

    auto saw_derived_pair = false;
    const auto implementation = graph(
        "name"_n = "dispatch_union_forms",
        [&](wp<FirstForm> first, wp<SecondForm> second) {
            saw_derived_pair |= &first.type().dereferenced_type() == &TS<Bundle<DispatchDerived>>::type() &&
                                &second.type().dereferenced_type() == &TS<Bundle<DispatchOtherDerived>>::type();
            return operators::const_(i64{30}).as<TS<i64, TimeSeriesGuarantee::const_>>();
        });
    using DispatchSignature = auto(TS<Bundle<DispatchBase>>, TS<Bundle<DispatchOtherBase>>) -> TS<i64>;
    const auto dispatch = dispatch_<DispatchSignature>{implementation};
    const auto publish_first = node([](In<TS<i64>>, Scalar<Bundle<DispatchDerived>> value,
                                       Out<TS<Bundle<DispatchBase>>> output) { output.set(value); });
    const auto publish_second = node([](In<TS<i64>>, Scalar<Bundle<DispatchOtherDerived>> value,
                                        Out<TS<Bundle<DispatchOtherBase>>> output) { output.set(value); });

    auto memory = allocator{};
    const auto first_value = bundle<DispatchDerived>(memory, symbol{"first"}, i64{1});
    const auto second_value = bundle<DispatchOtherDerived>(memory, symbol{"second"}, i64{2});
    const auto graph = [&](wp<TS<i64>> trigger) {
        const auto first = publish_first(trigger, first_value).as<TS<Bundle<DispatchBase>>>();
        const auto second = publish_second(trigger, second_value).as<TS<Bundle<DispatchOtherBase>>>();
        return dispatch(first, second).as<TS<i64>>();
    };
    const auto actual = testing::test_graph(graph, {1});

    CHECK(actual.equals<i64>({30}));
    CHECK(saw_derived_pair);
}

TEST_CASE("dispatch expands scalar-variable alternatives",
          "[feature:dispatch][feature:type-system][binding:covariant]") {
    using VALUE = ScalarVariable<"DISPATCH_VALUE", Bundle<DispatchBase>, Bundle<DispatchDerived>>;

    auto saw_derived = false;
    const auto implementation = graph(
        "name"_n = "dispatch_scalar_variable_alternatives",
        [&](wp<TS<VALUE>> input) {
            saw_derived |= &input.type().dereferenced_type() == &TS<Bundle<DispatchDerived>>::type();
            return operators::const_(i64{30}).as<TS<i64, TimeSeriesGuarantee::const_>>();
        });
    using DispatchSignature = auto(TS<Bundle<DispatchBase>>) -> TS<i64>;
    const auto dispatch = dispatch_<DispatchSignature>{implementation};
    const auto publish = node([](In<TS<i64>>, Scalar<Bundle<DispatchDerived>> value,
                                 Out<TS<Bundle<DispatchBase>>> output) { output.set(value); });

    auto memory = allocator{};
    const auto value = bundle<DispatchDerived>(memory, symbol{"item"}, i64{42});
    const auto tested = [&](wp<TS<i64>> trigger) {
        const auto source = publish(trigger, value).as<TS<Bundle<DispatchBase>>>();
        return dispatch(source).as<TS<i64>>();
    };
    const auto actual = testing::test_graph(tested, {1});

    CHECK(actual.equals<i64>({30}));
    CHECK(saw_derived);
}

TEST_CASE("dispatch input specificity precedes reference-to-value binding cost",
          "[feature:dispatch][feature:reference][binding:reference-to-value]") {
    const auto generic = graph("name"_n = "dispatch_generic_reference_input",
                               [](wp<REF<TS<SCALAR>>>) {
        return operators::const_(i64{10}).as<TS<i64, TimeSeriesGuarantee::const_>>();
    });
    const auto specific = graph("name"_n = "dispatch_specific_value_input",
                                [](wp<TS<Bundle<DispatchDerived>>>) {
        return operators::const_(i64{20}).as<TS<i64, TimeSeriesGuarantee::const_>>();
    });
    using DispatchSignature = auto(REF<TS<Bundle<DispatchBase>>>) -> TS<i64>;
    const auto dispatch = dispatch_<DispatchSignature>{generic, specific};
    const auto publish = node([](i64 selection, Scalar<Bundle<DispatchDerived>> derived,
                                 Scalar<Bundle<DispatchBase>> base, Out<TS<Bundle<DispatchBase>>> output) {
        if (selection == 1) {
            output.set(derived);
        } else {
            output.set(base);
        }
    });

    auto memory = allocator{};
    const auto derived = bundle<DispatchDerived>(memory, symbol{"derived"}, i64{42});
    const auto base = bundle<DispatchBase>(memory, symbol{"base"});
    const auto graph = [&](wp<TS<i64>> selection) {
        const auto source = publish(selection, derived, base).as<REF<TS<Bundle<DispatchBase>>>>();
        return dispatch(source).as<TS<i64>>();
    };
    const auto actual = testing::test_graph(graph, {1, 2});

    CHECK(actual.equals<i64>({20, 10}));
}

TEST_CASE("dispatch resolves selected-input candidates from other implementation inputs",
          "[feature:dispatch][feature:type-system][binding:covariant]") {
    auto saw_derived_input = false;
    const auto implementation = graph(
        "name"_n = "dispatch_type_fixed_by_other_input",
        [&](wp<TS<SCALAR>> selected, Scalar<SCALAR>) {
            saw_derived_input |= &selected.type().dereferenced_type() == &TS<Bundle<DispatchDerived>>::type();
            return operators::const_(i64{20}).as<TS<i64, TimeSeriesGuarantee::const_>>();
        });
    using DispatchSignature = auto(TS<Bundle<DispatchBase>>, Bundle<DispatchDerived>) -> TS<i64>;
    const auto dispatch = dispatch_<DispatchSignature>{implementation};
    const auto publish = node([](In<TS<i64>>, Scalar<Bundle<DispatchDerived>> value,
                                 Out<TS<Bundle<DispatchBase>>> output) { output.set(value); });

    auto memory = allocator{};
    const auto value = bundle<DispatchDerived>(memory, symbol{"item"}, i64{42});
    const auto graph = [&](wp<TS<i64>> trigger) {
        const auto source = publish(trigger, value).as<TS<Bundle<DispatchBase>>>();
        return dispatch(source, value).as<TS<i64>>();
    };
    const auto actual = testing::test_graph(graph, {1});

    CHECK(actual.equals<i64>({20}));
    CHECK(saw_derived_input);
}

TEST_CASE("dispatch preserves runtime ambiguity across partial specializations",
          "[feature:dispatch][feature:operator][binding:covariant]") {
    const auto derived_base = graph("name"_n = "dispatch_incomparable_derived_base",
                                    [](wp<TS<Bundle<DispatchDerived>>>,
                                       wp<TS<Bundle<DispatchOtherBase>>>) {
        return operators::const_(i64{10}).as<TS<i64, TimeSeriesGuarantee::const_>>();
    });
    const auto base_derived = graph("name"_n = "dispatch_incomparable_base_derived",
                                    [](wp<TS<Bundle<DispatchBase>>>,
                                       wp<TS<Bundle<DispatchOtherDerived>>>) {
        return operators::const_(i64{20}).as<TS<i64, TimeSeriesGuarantee::const_>>();
    });
    using DispatchSignature = auto(TS<Bundle<DispatchBase>>, TS<Bundle<DispatchOtherBase>>) -> TS<i64>;
    const auto dispatch = dispatch_<DispatchSignature>{derived_base, base_derived};
    const auto publish_first = node([](In<TS<i64>>, Scalar<Bundle<DispatchDerived>> value,
                                       Out<TS<Bundle<DispatchBase>>> output) { output.set(value); });
    const auto publish_second = node([](In<TS<i64>>, Scalar<Bundle<DispatchOtherDerived>> value,
                                        Out<TS<Bundle<DispatchOtherBase>>> output) { output.set(value); });

    auto memory = allocator{};
    const auto first_value = bundle<DispatchDerived>(memory, symbol{"first"}, i64{1});
    const auto second_value = bundle<DispatchOtherDerived>(memory, symbol{"second"}, i64{2});
    const auto graph = [&](wp<TS<i64>> trigger) {
        const auto first = publish_first(trigger, first_value).as<TS<Bundle<DispatchBase>>>();
        const auto second = publish_second(trigger, second_value).as<TS<Bundle<DispatchOtherBase>>>();
        return dispatch(first, second).as<TS<i64>>();
    };

    CHECK_THROWS_AS(testing::test_graph(graph, {1}), std::logic_error);
}

TEST_CASE("dispatch preserves the source time-series guarantee while narrowing its value type",
          "[feature:dispatch][feature:type-system][binding:covariant]") {
    const auto implementation =
        graph("name"_n = "dispatch_final_source", [](wp<TS<Bundle<DispatchDerived>>> input) {
            (void)input;
            return operators::const_(i64{20}).as<TS<i64, TimeSeriesGuarantee::const_>>();
        });
    using DispatchSignature = auto(TS<Bundle<DispatchBase>>) -> TS<i64>;
    const auto dispatch = dispatch_<DispatchSignature>{implementation};
    const auto publish_final = node([](In<TS<i64>>, Scalar<Bundle<DispatchDerived>> value,
                                       Out<TS<Bundle<DispatchBase>, TimeSeriesGuarantee::final>> output) {
        output.set(value);
    });

    auto memory = allocator{};
    const auto value = bundle<DispatchDerived>(memory, symbol{"item"}, i64{42});
    const auto graph = [&](wp<TS<i64>> trigger) {
        const auto source = publish_final(trigger, value).as<TS<Bundle<DispatchBase>, TimeSeriesGuarantee::final>>();
        return dispatch(source).as<TS<i64>>();
    };
    const auto result = testing::test_graph(graph, {1});

    REQUIRE(result.size() == 1);
    REQUIRE(result[0].has_value());
    CHECK(scalar_cast<i64>(result[0]->view()).value() == 20);
}

TEST_CASE("dispatch adapts a covariant scalar-value output to its declared type",
          "[feature:dispatch][feature:type-system][binding:covariant][series:value]") {
    auto memory = allocator{};
    const auto value = bundle<DispatchDerived>(memory, symbol{"item"}, i64{42});
    const auto implementation = graph("name"_n = "dispatch_covariant_output",
                                      [&](wp<TS<Bundle<DispatchBase>>> input)
                                          -> wp<TS<Bundle<DispatchBase>>> {
        (void)input;
        return operators::const_(value).as<TS<Bundle<DispatchBase>>>();
    });
    using DispatchSignature = auto(TS<Bundle<DispatchBase>>) -> TS<Bundle<DispatchBase>>;
    const auto dispatch = dispatch_<DispatchSignature>{implementation};
    const auto publish = node([](In<TS<i64>>, Scalar<Bundle<DispatchBase>> value,
                                 Out<TS<Bundle<DispatchBase>>> output) { output.set(value); });
    const auto read_name = node([](In<TS<Bundle<DispatchBase>>> input) {
        const auto base = scalar_cast<Bundle<DispatchBase>>(input.value());
        return scalar_cast<symbol>(base[0]).value() == symbol{"item"};
    });
    const auto tested = [&](wp<TS<i64>> trigger) {
        const auto source = publish(trigger, bundle<DispatchBase>(memory, symbol{"source"}));
        return read_name(dispatch(source));
    };

    const auto actual = testing::test_graph(tested, {1});

    CHECK(actual.equals({true}));
}

TEST_CASE("dispatch passes its explicit type mapping to the selected implementation",
          "[feature:dispatch][feature:type-system]") {
    const auto implementation = graph("name"_n = "dispatch_mapped_output",
                                      [](wp<TS<Bundle<DispatchBase>>>) {
        return operators::const_(i64{20}).as<TS<SCALAR>>();
    });
    using DispatchSignature = auto(TS<Bundle<DispatchBase>>) -> TS<SCALAR>;
    const auto dispatch = dispatch_<DispatchSignature>{implementation};
    const auto output_type = details::make_type_map<SCALAR>(type<SCALAR> = type<i64>);
    const auto publish = node([](In<TS<i64>>, Scalar<Bundle<DispatchBase>> value,
                                 Out<TS<Bundle<DispatchBase>>> output) { output.set(value); });

    auto memory = allocator{};
    const auto value = bundle<DispatchBase>(memory, symbol{"item"});
    const auto graph = [&](wp<TS<i64>> trigger) {
        const auto input = publish(trigger, value);
        return dispatch(output_type, input).as<TS<i64>>();
    };

    const auto actual = testing::test_graph(graph, {1, 2, 3});

    CHECK(actual.equals<i64>({20, std::nullopt, std::nullopt}));
}

TEST_CASE("dispatch follows runtime type changes, invalidation, and revalidation",
          "[feature:dispatch][feature:nested-graph][feature:validity][transition:update]"
          "[transition:invalidation][transition:revalidation][series:value]") {
    const auto base_implementation = graph("name"_n = "dispatch_transition_base",
                                           [](wp<TS<Bundle<DispatchBase>>>) {
        return operators::const_(i64{10}).as<TS<i64, TimeSeriesGuarantee::const_>>();
    });
    const auto derived_implementation = graph("name"_n = "dispatch_transition_derived",
                                              [](wp<TS<Bundle<DispatchDerived>>>) {
        return operators::const_(i64{20}).as<TS<i64, TimeSeriesGuarantee::const_>>();
    });
    using DispatchSignature = auto(TS<Bundle<DispatchBase>>) -> TS<i64>;
    const auto dispatch = dispatch_<DispatchSignature>{base_implementation, derived_implementation};
    const auto publish = node([](std::optional<i64> selection, Scalar<Bundle<DispatchDerived>> derived,
                                 Scalar<Bundle<DispatchBase>> base, Out<TS<Bundle<DispatchBase>>> output) {
        if (!selection) {
            output.invalidate();
            return;
        }
        if (*selection == 1) {
            output.set(derived);
        } else {
            output.set(base);
        }
    });

    auto memory = allocator{};
    const auto derived_value = bundle<DispatchDerived>(memory, symbol{"derived"}, i64{42});
    const auto base_value = bundle<DispatchBase>(memory, symbol{"base"});
    const auto graph = [&](wp<TS<i64>> selection) {
        const auto source = publish(selection, derived_value, base_value).as<TS<Bundle<DispatchBase>>>();
        return dispatch(source).as<TS<i64>>();
    };

    const auto actual = testing::test_graph(graph, {1, 2, literal::INVALID, 1, 2});

    CHECK(actual.equals<i64>({20, 10, std::nullopt, 20, 10}));
    auto invalidated = false;
    CHECK(actual.visit_tick(2, [&](const TimeSeriesType &, bool valid, util::buffer_reader &) {
        invalidated = !valid;
    }));
    CHECK(invalidated);
}

TEST_CASE("dispatch selects through a reference to a changing concrete value type",
          "[feature:dispatch][feature:reference][binding:reference-to-value][transition:update][series:value]") {
    const auto base_implementation = graph("name"_n = "dispatch_reference_base",
                                           [](wp<TS<Bundle<DispatchBase>>>) {
        return operators::const_(i64{10}).as<TS<i64, TimeSeriesGuarantee::const_>>();
    });
    const auto derived_implementation = graph("name"_n = "dispatch_reference_derived",
                                              [](wp<TS<Bundle<DispatchDerived>>>) {
        return operators::const_(i64{20}).as<TS<i64, TimeSeriesGuarantee::const_>>();
    });
    using DispatchSignature = auto(REF<TS<Bundle<DispatchBase>>>) -> TS<i64>;
    const auto dispatch = dispatch_<DispatchSignature>{base_implementation, derived_implementation};
    const auto publish = node([](i64 selection, Scalar<Bundle<DispatchDerived>> derived,
                                 Scalar<Bundle<DispatchBase>> base, Out<TS<Bundle<DispatchBase>>> output) {
        if (selection == 1) {
            output.set(derived);
        } else {
            output.set(base);
        }
    });

    auto memory = allocator{};
    const auto derived_value = bundle<DispatchDerived>(memory, symbol{"derived"}, i64{42});
    const auto base_value = bundle<DispatchBase>(memory, symbol{"base"});
    const auto graph = [&](wp<TS<i64>> selection) {
        const auto reference = publish(selection, derived_value, base_value)
                                   .as<REF<TS<Bundle<DispatchBase>>>>();
        return dispatch(reference).as<TS<i64>>();
    };
    const auto actual = testing::test_graph(graph, {1, 2});

    CHECK(actual.equals<i64>({20, 10}));
}

TEST_CASE("dispatch only adds runtime tuples whose declared implementation is selected",
          "[feature:dispatch][feature:operator][feature:nested-graph]") {
    const auto fallback = graph("name"_n = "dispatch_requirement_fallback",
                                [](wp<TS<Bundle<DispatchBase>>>, wp<TS<Bundle<DispatchOtherBase>>>) {
        return operators::const_(i64{10}).as<TS<i64, TimeSeriesGuarantee::const_>>();
    });
    const auto rejected_first = graph(
        "name"_n = "dispatch_requirement_rejected_first",
        require = [](const WiringArguments &) { return reject("derived specialization is disabled"); },
        [](wp<TS<Bundle<DispatchDerived>>>, wp<TS<Bundle<DispatchOtherBase>>>) {
            return operators::const_(i64{20}).as<TS<i64, TimeSeriesGuarantee::const_>>();
        });
    const auto rejected_second = graph(
        "name"_n = "dispatch_requirement_rejected_second",
        require = [](const WiringArguments &) { return reject("derived specialization is disabled"); },
        [](wp<TS<Bundle<DispatchBase>>>, wp<TS<Bundle<DispatchOtherDerived>>>) {
            return operators::const_(i64{20}).as<TS<i64, TimeSeriesGuarantee::const_>>();
        });
    using DispatchSignature = auto(TS<Bundle<DispatchBase>>, TS<Bundle<DispatchOtherBase>>) -> TS<i64>;
    const auto dispatch = dispatch_<DispatchSignature>{fallback, rejected_first, rejected_second};
    const auto publish_first = node([](In<TS<i64>>, Scalar<Bundle<DispatchDerived>> value,
                                       Out<TS<Bundle<DispatchBase>>> output) { output.set(value); });
    const auto publish_second = node([](In<TS<i64>>, Scalar<Bundle<DispatchOtherDerived>> value,
                                        Out<TS<Bundle<DispatchOtherBase>>> output) { output.set(value); });

    auto memory = allocator{};
    const auto first_value = bundle<DispatchDerived>(memory, symbol{"first"}, i64{42});
    const auto second_value = bundle<DispatchOtherDerived>(memory, symbol{"second"}, i64{42});
    const auto graph = [&](wp<TS<i64>> trigger) {
        const auto first = publish_first(trigger, first_value).as<TS<Bundle<DispatchBase>>>();
        const auto second = publish_second(trigger, second_value).as<TS<Bundle<DispatchOtherBase>>>();
        return dispatch(first, second).as<TS<i64>>();
    };
    const auto actual = testing::test_graph(graph, {1});

    CHECK(actual.equals<i64>({10}));
}

TEST_CASE("dispatch skips an implementation with a mismatched independent scalar constraint",
          "[feature:dispatch][feature:operator][binding:covariant]") {
    const auto constrained = graph(
        "name"_n = "dispatch_constrained_scalar",
        [](wp<TS<Bundle<DispatchBase>>>, float) {
            return operators::const_(i64{20}).as<TS<i64, TimeSeriesGuarantee::const_>>();
        });
    const auto fallback = graph(
        "name"_n = "dispatch_scalar_fallback",
        [](wp<TS<Bundle<DispatchBase>>>, i64) {
            return operators::const_(i64{10}).as<TS<i64, TimeSeriesGuarantee::const_>>();
        });
    using DispatchSignature = auto(TS<Bundle<DispatchBase>>, i64) -> TS<i64>;
    const auto dispatch = dispatch_<DispatchSignature>{constrained, fallback};
    const auto publish = node([](In<TS<i64>>, Scalar<Bundle<DispatchBase>> value,
                                 Out<TS<Bundle<DispatchBase>>> output) { output.set(value); });

    auto memory = allocator{};
    const auto value = bundle<DispatchBase>(memory, symbol{"item"});
    const auto graph = [&](wp<TS<i64>> trigger) {
        const auto input = publish(trigger, value).as<TS<Bundle<DispatchBase>>>();
        return dispatch(input, i64{1}).as<TS<i64>>();
    };

    CHECK(testing::test_graph(graph, {1}).equals<i64>({10}));
}

TEST_CASE("dispatch supports outputless implementation graphs",
          "[feature:dispatch][feature:nested-graph][shape:sink]") {
    auto selected = std::vector<i64>{};
    const auto sink_base = node([&](In<TS<Bundle<DispatchBase>>>) { selected.push_back(10); });
    const auto sink_derived = node([&](In<TS<Bundle<DispatchDerived>>>) { selected.push_back(20); });
    using DispatchSignature = auto(TS<Bundle<DispatchBase>>) -> void;
    const auto dispatch = dispatch_<DispatchSignature>{sink_base, sink_derived};
    const auto publish_derived = node([](In<TS<i64>>, Scalar<Bundle<DispatchDerived>> value,
                                         Out<TS<Bundle<DispatchBase>>> output) { output.set(value); });

    auto memory = allocator{};
    const auto value = bundle<DispatchDerived>(memory, symbol{"item"}, i64{42});
    const auto graph = [&](wp<TS<i64>> trigger) {
        const auto source = publish_derived(trigger, value).as<TS<Bundle<DispatchBase>>>();
        (void)dispatch(source);
        return trigger;
    };
    (void)testing::test_graph(graph, {1, 1});

    CHECK(selected == std::vector<i64>{20, 20});
}

TEST_CASE("dispatch rejects implementations with a different output presence",
          "[feature:dispatch][feature:operator][feature:nested-graph]") {
    const auto outputless_implementation = graph("name"_n = "dispatch_outputless_implementation",
                                                  [](wp<TS<Bundle<DispatchBase>>>) {});
    const auto output_implementation = graph("name"_n = "dispatch_output_implementation",
                                              [](wp<TS<Bundle<DispatchBase>>> input) {
        (void)input;
        return operators::const_(i64{10}).as<TS<i64, TimeSeriesGuarantee::const_>>();
    });
    const auto publish = node([](In<TS<i64>>, Scalar<Bundle<DispatchDerived>> value,
                                 Out<TS<Bundle<DispatchBase>>> output) { output.set(value); });

    auto memory = allocator{};
    const auto value = bundle<DispatchDerived>(memory, symbol{"item"}, i64{42});
    using OutputDispatchSignature = auto(TS<Bundle<DispatchBase>>) -> TS<i64>;
    const auto output_dispatch = dispatch_<OutputDispatchSignature>{outputless_implementation};
    const auto output_graph = [&](wp<TS<i64>> trigger) {
        const auto source = publish(trigger, value).as<TS<Bundle<DispatchBase>>>();
        return output_dispatch(source).as<TS<i64>>();
    };
    CHECK_THROWS_AS(testing::test_graph(output_graph, {1}), WiringError);

    using SinkDispatchSignature = auto(TS<Bundle<DispatchBase>>) -> void;
    const auto sink_dispatch = dispatch_<SinkDispatchSignature>{output_implementation};
    const auto sink_graph = [&](wp<TS<i64>> trigger) {
        const auto source = publish(trigger, value).as<TS<Bundle<DispatchBase>>>();
        (void)sink_dispatch(source);
        return trigger;
    };
    CHECK_THROWS_AS(testing::test_graph(sink_graph, {1}), WiringError);
}

TEST_CASE("dispatch enumerates implementation input types resolved by an explicit mapping",
          "[feature:dispatch][feature:type-system][binding:covariant]") {
    const auto implementation = graph("name"_n = "dispatch_mapped_input",
                                      [](wp<TS<SCALAR>>) {
        return operators::const_(i64{20}).as<TS<i64, TimeSeriesGuarantee::const_>>();
    });
    using DispatchSignature = auto(TS<Bundle<DispatchBase>>) -> TS<i64>;
    const auto dispatch = dispatch_<DispatchSignature>{implementation};
    const auto publish = node([](In<TS<i64>>, Scalar<Bundle<DispatchDerived>> value,
                                 Out<TS<Bundle<DispatchBase>>> output) { output.set(value); });

    auto memory = allocator{};
    const auto value = bundle<DispatchDerived>(memory, symbol{"item"}, i64{42});
    const auto input_type = details::make_type_map<SCALAR>(type<SCALAR> = type<Bundle<DispatchDerived>>);
    const auto graph = [&](wp<TS<i64>> trigger) {
        const auto source = publish(trigger, value).as<TS<Bundle<DispatchBase>>>();
        return dispatch(input_type, source).as<TS<i64>>();
    };
    const auto actual = testing::test_graph(graph, {1});

    CHECK(actual.equals<i64>({20}));
}

TEST_CASE("dispatch commits a selected implementation with a default scalar argument",
          "[feature:dispatch][feature:wiring-defaults][series:value]") {
    const auto implementation = graph({"input"_n, "offset"_n = i64{9}},
                                      [](wp<TS<Bundle<DispatchBase>>> input, i64 offset) {
                                          (void)input;
                                          return operators::const_(offset)
                                              .as<TS<i64, TimeSeriesGuarantee::const_>>();
                                      });
    const auto dispatch = dispatch_<auto(TS<Bundle<DispatchBase>>) -> TS<i64>>{implementation};
    const auto publish = node([](In<TS<i64>>, Out<TS<Bundle<DispatchBase>>> output) {
        auto memory = allocator{};
        output.set(bundle<DispatchBase>(memory, symbol{"item"}));
    });
    const auto tested = [&](wp<TS<i64>> trigger) {
        return dispatch(publish(trigger)).as<TS<i64>>();
    };

    CHECK(testing::test_graph(tested, {1}).equals<i64>({9}));
}

TEST_CASE("dispatch preserves named argument binding when invoking the selected implementation",
          "[feature:dispatch][feature:wiring][series:value]") {
    const auto implementation = graph([](wp<TS<Bundle<DispatchBase>>> input, i64 result) {
        (void)input;
        return operators::const_(result).as<TS<i64, TimeSeriesGuarantee::const_>>();
    });
    const auto dispatch = dispatch_<auto(TS<Bundle<DispatchBase>>, i64) -> TS<i64>>{implementation};
    const auto publish = node([](In<TS<i64>>, Out<TS<Bundle<DispatchBase>>> output) {
        auto memory = allocator{};
        output.set(bundle<DispatchBase>(memory, symbol{"item"}));
    });
    const auto tested = [&](wp<TS<i64>> trigger) {
        return dispatch("1"_n = i64{7}, "0"_n = publish(trigger)).as<TS<i64>>();
    };

    CHECK(testing::test_graph(tested, {1}).equals<i64>({7}));
}

TEST_CASE("dispatch commits a selected implementation with variadic time-series arguments",
          "[feature:dispatch][feature:variadic-wiring][series:tsb][schema:generic]") {
    const auto implementation = graph([](wp<TS<Bundle<DispatchBase>>> input,
                                         wp<TSB<SCHEMA>, var_args> values) {
        (void)input;
        return wp<TS<i64>>{values[0].time_series()};
    });
    const auto dispatch = dispatch_<auto(TS<Bundle<DispatchBase>>, TS<i64>) -> TS<i64>>{implementation};
    const auto publish = node([](In<TS<i64>>, Out<TS<Bundle<DispatchBase>>> output) {
        auto memory = allocator{};
        output.set(bundle<DispatchBase>(memory, symbol{"item"}));
    });
    const auto tested = [&](wp<TS<i64>> trigger, wp<TS<i64>> value) {
        return dispatch(publish(trigger), value).as<TS<i64>>();
    };

    CHECK(testing::test_graph(tested, {1, 1}, {3, 5}).equals<i64>({3, 5}));
}

TEST_CASE("dispatch reports implementation argument normalization failures",
          "[feature:dispatch][feature:operator][feature:wiring]") {
    const auto implementation = graph("name"_n = "dispatch_missing_argument",
                                      [](wp<TS<Bundle<DispatchBase>>> input, i64 required_value) {
                                          (void)input;
                                          return operators::const_(required_value)
                                              .as<TS<i64, TimeSeriesGuarantee::const_>>();
                                      });
    const auto dispatch = dispatch_<auto(TS<Bundle<DispatchBase>>) -> TS<i64>>{implementation};
    const auto tested = [&](wp<TS<Bundle<DispatchBase>>> input) {
        return dispatch(input).as<TS<i64>>();
    };

    try {
        (void)testing::test_graph(tested, testing::input<TS<Bundle<DispatchBase>>>({}));
        FAIL("dispatch should reject the missing implementation argument");
    } catch (const WiringError &error) {
        REQUIRE(error.diagnostic());
        REQUIRE(error.diagnostic()->candidates.size() == 1);
        const auto &candidate = error.diagnostic()->candidates.front();
        CHECK(candidate.signature.name.text() == "dispatch_missing_argument");
        CHECK(candidate.stage == WiringFailureStage::argument_count);
        CHECK(candidate.message.find("missing a fixed argument") != std::string::npos);
    }
}

} // namespace
