#include <bg/graph.hpp>
#include <bg/graph/wiring_error.hpp>
#include <bg/node.hpp>
#include <bg/operator.hpp>
#include <bg/operators.hpp>
#include <bg/scalar/type.hpp>
#include <bg/testing/test_graph.hpp>
#include <bg/testing/test_wiring.hpp>
#include <bg/type/guarantee_variable.hpp>
#include <bg/type/scalar_variable.hpp>
#include <bg/time_series/time_series_type.hpp>
#include <bg/time_series/tsd/tsd_type.hpp>
#include <bg/time_series/tsl/tsl_type.hpp>
#include <bg/type/type_variable.hpp>
#include <bg/type/time_series_reference_type.hpp>
#include <bg/type/time_series_value_type.hpp>
#include <bg/type/type_resolution_error.hpp>
#include <bg/util/scope_exit.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

using namespace bg;
using namespace bg::literals;

namespace {

TEST_CASE("operator scalar type-variable results retain their identity",
          "[feature:operator][feature:type-system][declaration:generic][series:value][scalar:generic]") {
    using VALUE = ScalarVariable<"VALUE">;

    const auto generic = operator_<auto(SCALAR)->SCALAR>{"generic_scalar_result", {"value"}};
    auto named = operator_<VALUE, auto(VALUE)->VALUE>{"named_scalar_result", {"value"}};

    CHECK(generic.signature().parameter(0).type == &SCALAR::type());
    CHECK(generic.signature().output() == &TS<SCALAR>::type());
    CHECK(named.signature().parameter(0).type == &VALUE::type());
    CHECK(named.signature().output() == &TS<VALUE>::type());

    named.overload(graph("name"_n = "constant", [](double value) {
        return operators::const_(value).as<TS<double, TimeSeriesGuarantee::const_>>();
    }));
    testing::test_wiring([&] {
        const auto result = named[type<VALUE> = type<double>](2.5);
        CHECK(&result.type() == &TS<double, TimeSeriesGuarantee::const_>::type());
        CHECK_THROWS_AS((named[type<VALUE> = type<i64>](2.5)), WiringError);
    });
}

TEST_CASE("an explicit operator type argument requires an overload that uses it",
          "[feature:operator][feature:type-system][declaration:generic]") {
    using VALUE = ScalarVariable<"VALUE">;
    auto operation = operator_<VALUE, auto(TS_T)->TS_T>{"explicit_type_selection", {"value"}};
    operation.overload(graph("name"_n = "concrete", [](wp<TS<i64>> value) { return value; }));

    testing::test_wiring([&] {
        const auto value = operators::const_(i64{3});
        CHECK(&operation(value).type() == &value.type());
        CHECK_THROWS_AS((operation[type<i64>](value)), WiringError);
    });

    operation.overload(graph("name"_n = "generic", [](wp<TS<VALUE>> value) { return value; }));
    testing::test_wiring([&] {
        const auto value = operators::const_(i64{3});
        CHECK(&operation[type<i64>](value).type() == &value.type());
        CHECK_THROWS_AS((operation[type<double>](value)), WiringError);
    });
}

TEST_CASE("operator concrete scalar results use canonical time-series declarations",
          "[feature:operator][feature:type-system][declaration:concrete][series:value]"
          "[scalar:floating-point][scalar:integer]") {
    const auto double_result = operator_<auto()->double>{"double_scalar_result"};
    const auto integer_result = operator_<auto()->int>{"integer_scalar_result"};

    CHECK(double_result.signature().output() == &TS<double>::type());
    CHECK(integer_result.signature().output() == &TS<i64>::type());
}

TEST_CASE("operator scalar-result prototypes do not constrain overload arguments or outputs",
          "[feature:operator][feature:wiring][series:value][guarantee:final][declaration:concrete]") {
    auto operation = operator_<auto(SCALAR)->SCALAR>{"final_identity", {"value"}};
    const auto implementation =
        graph("name"_n = "final_identity", [](wp<TS<double, TimeSeriesGuarantee::final>> value) { return value; });
    operation.overload(implementation);

    const auto source = node([](Out<TS<double, TimeSeriesGuarantee::final>>) {});

    testing::test_wiring([&] {
        const auto result = operation(source());
        CHECK(&result.type() == &TS<double, TimeSeriesGuarantee::final>::type());
    });
}

TEST_CASE("explicit time-series and sink operator prototypes keep their result form",
          "[feature:operator][feature:wiring][series:value]") {
    const auto time_series = operator_<auto()->TS<double>>{"time_series_result"};
    const auto sink = operator_<void(double)>{"sink", {"value"}};

    CHECK(time_series.signature().output() == &TS<double>::type());
    CHECK_FALSE(sink.signature().has_output());
}

TEST_CASE("an operator selects the least generic applicable overload",
          "[feature:operator][feature:wiring][declaration:generic]") {
    auto operation = operator_<auto(TS_T, TS_T)->TS_T>{"select", {"value", "fallback"}};
    operation.overload(graph("name"_n = "generic", [](wp<TS<SCALAR>>, wp<TS<SCALAR>> fallback) { return fallback; }));
    operation.overload(graph("name"_n = "float_exact", [](wp<TS<float>> value, wp<TS<float>>) { return value; }));

    const auto float_graph = [&](wp<TS<float>> value, wp<TS<float>> fallback) {
        return operation(value, fallback);
    };

    const auto double_graph = [&](wp<TS<double>> value, wp<TS<double>> fallback) {
        return operation(value, fallback);
    };

    CHECK(testing::test_graph(float_graph, {1.0F, 2.0F}, {10.0F, 20.0F}).equals<float>({1.0F, 2.0F}));
    CHECK(testing::test_graph(double_graph, {1.0, 2.0}, {10.0, 20.0}).equals<double>({10.0, 20.0}));
}

TEST_CASE("operator specificity is independent of overload registration order",
          "[feature:operator][feature:wiring][declaration:generic][series:value]") {
    using EQUALITY_SCALAR = ScalarVariable<"EQUALITY_SCALAR", EqualityComparable>;
    constexpr auto G = guarantee_variable<"G">;

    const auto check_registration_orders = [&](auto order, i64 expected_result, std::size_t selected_overload) {
        do {
            auto selected = std::array<int, 4>{};
            auto operation = operator_<auto(TS_T)->TS<i64>>{"specificity_order", {"value"}};

            const auto exact_result = node([](Out<TS<i64>> output) { output.set(1); });

            const auto equality_result = node([](Out<TS<i64>> output) { output.set(3); });

            const auto scalar_result = node([](Out<TS<i64>> output) { output.set(4); });

            const auto promoted = node([&](i64 value) {
                ++selected[1];
                return value + 10;
            });

            const auto add_overload = [&](int index) {
                switch (index) {
                case 0:
                    operation.overload(bg::graph("name"_n = "exact", [&, exact_result](wp<TS<i64, G>>) {
                        ++selected[0];
                        return exact_result();
                    }));
                    break;
                case 1:
                    operation.overload(promoted);
                    break;
                case 2:
                    operation.overload(
                        bg::graph("name"_n = "equality", [&, equality_result](wp<TS<EQUALITY_SCALAR, G>>) {
                            ++selected[2];
                            return equality_result();
                        }));
                    break;
                case 3:
                    operation.overload(bg::graph("name"_n = "scalar", [&, scalar_result](wp<TS<SCALAR, G>>) {
                        ++selected[3];
                        return scalar_result();
                    }));
                    break;
                }
            };

            for (const auto index : order) {
                add_overload(index);
            }

            const auto graph = [&](wp<TS<i64>> value) {
                (void)operation("value"_n = value);
                return operation("value"_n = value);
            };
            const auto result = testing::test_graph(graph, {1});

            CHECK((result.template equals<i64>({expected_result})));
            for (auto index = std::size_t{}; index < selected.size(); ++index) {
                if (index != selected_overload) {
                    CHECK(selected[index] == 0);
                } else if (index == 1) {
                    CHECK(selected[index] > 0);
                } else {
                    CHECK(selected[index] == 2);
                }
            }
        } while (std::ranges::next_permutation(order).found);
    };

    check_registration_orders(std::array{0, 1, 2, 3}, 1, 0);
    check_registration_orders(std::array{1, 2, 3}, 11, 1);
    check_registration_orders(std::array{2, 3}, 3, 2);
    check_registration_orders(std::array{3}, 4, 3);
}

TEST_CASE("an operator prefers the overload requiring the stronger guarantee",
          "[feature:operator][feature:wiring][series:value][guarantee:ordinary][guarantee:final]") {
    auto selected = std::string{};
    auto operation = operator_<auto(TS_T)->TS_T>{"guarantee", {"value"}};
    operation.overload(graph("name"_n = "ordinary", [&](wp<TS<i64>> value) {
        selected = "ordinary";
        return value;
    }));
    operation.overload(graph("name"_n = "final", [&](wp<TS<i64, TimeSeriesGuarantee::final>> value) {
        selected = "final";
        return value;
    }));

    const auto ordinary_source = node([](Out<TS<i64>>) {});

    const auto final_source = node([](Out<TS<i64, TimeSeriesGuarantee::final>>) {});

    testing::test_wiring([&] {
        operation(final_source());
        CHECK(selected == "final");
    });
    testing::test_wiring([&] {
        operation(ordinary_source());
        CHECK(selected == "ordinary");
    });
}

TEST_CASE("literal guarantee overloads outrank variables with the same minimum in either registration order",
          "[feature:operator][feature:wiring][declaration:generic][series:value]"
          "[guarantee:deduplicated]") {
    constexpr auto G = guarantee_variable<"OVERLOAD_GUARANTEE", TimeSeriesGuarantee::deduplicated>;

    const auto source = node([](Out<TS<i64, TimeSeriesGuarantee::deduplicated>>) {});

    const auto check_registration_order = [&](bool variable_first) {
        auto selected = std::string{};
        auto operation = operator_<auto(TS_T)->TS_T>{"minimum_guarantee_specificity", {"value"}};

        const auto literal_result = node([](Out<TS<i64>>) {});

        const auto variable_result = node([](Out<TS<i64>>) {});

        const auto literal = graph("name"_n = "literal", [&](wp<TS<i64, TimeSeriesGuarantee::deduplicated>>) {
            selected = "literal";
            return literal_result();
        });

        const auto variable = graph("name"_n = "variable", [&](wp<TS<i64, G>>) {
            selected = "variable";
            return variable_result();
        });

        if (variable_first) {
            operation.overload(variable);
            operation.overload(literal);
        } else {
            operation.overload(literal);
            operation.overload(variable);
        }

        testing::test_wiring([&] {
            CHECK(&operation(source()).type() == &TS<i64>::type());
            CHECK(selected == "literal");
        });
    };

    check_registration_order(true);
    check_registration_order(false);
}

TEST_CASE("registering an overload clears cached selections", "[feature:operator][feature:wiring]") {
    auto selected = std::string{};
    auto operation = operator_<auto(TS_T)->TS_T>{"identity", {"value"}};
    operation.overload(graph("name"_n = "generic", [&](wp<TS<SCALAR>> value) {
        selected = "generic";
        return value;
    }));

    const auto source = node([](Out<TS<float>>) {});

    testing::test_wiring([&] { operation(source()); });
    CHECK(selected == "generic");

    operation.overload(graph("name"_n = "float", [&](wp<TS<float>> value) {
        selected = "float";
        return value;
    }));

    testing::test_wiring([&] { operation(source()); });
    CHECK(selected == "float");
}

TEST_CASE("operator resolution caches are shared by nested wiring and isolated between roots",
          "[feature:operator][feature:wiring][shape:nested]") {
    auto requirement_calls = 0;
    auto operation = operator_<auto(TS_T)->TS_T>{"identity", {"value"}};
    operation.overload(graph(
        "name"_n = "identity_impl",
        require =
            [&](const WiringArguments &) {
                ++requirement_calls;
                return true;
            },
        [](wp<TS<SCALAR>> value) { return value; }));

    const auto source = node([](Out<TS<float>>) {});

    auto root = GraphWiring{};
    {
        const auto context = WiringContext{root};
        (void)operation(source());
    }
    {
        auto nested = GraphWiring{root};
        const auto context = WiringContext{nested};
        (void)operation(source());
    }
    CHECK(requirement_calls == 1);

    auto separate_root = GraphWiring{};
    {
        const auto context = WiringContext{separate_root};
        (void)operation(source());
    }
    CHECK(requirement_calls == 2);
}

TEST_CASE("operator graph resolvers cache scalar-dependent outputs separately",
          "[feature:operator][feature:wiring][feature:type-system]") {
    using RESULT = TimeSeriesVariable<"OPERATOR_GRAPH_RESOLVED_RESULT">;

    auto resolver_calls = std::size_t{};
    auto requirement_calls = std::size_t{};
    auto body_calls = std::size_t{};
    auto operation = operator_<auto(i64) -> TS_T>{"scalar_dependent_result", {"value"}};
    const auto integer_source = node([](Out<TS<i64>>) {});
    const auto floating_source = node([](Out<TS<double>>) {});
    operation.overload(graph<RESULT>(
        "name"_n = "scalar_dependent_impl",
        resolve<RESULT> = [&](const WiringArguments &arguments) -> const TimeSeriesType & {
            ++resolver_calls;
            return scalar_cast<i64>(*arguments.scalar(0)).value() % 2 == 0 ? TS<i64>::type() : TS<double>::type();
        },
        require = [&](const WiringArguments &arguments) {
            ++requirement_calls;
            const auto value = scalar_cast<i64>(*arguments.scalar(0)).value();
            const auto *output = arguments.resolved_output_type();
            return output == (value % 2 == 0 ? &TS<i64>::type() : &TS<double>::type());
        },
        [&](const WiringArguments &, i64 value) -> wp<RESULT> {
            ++body_calls;
            if (value % 2 == 0) {
                return wp<RESULT>{integer_source()};
            }
            return wp<RESULT>{floating_source()};
        }));

    auto wiring = GraphWiring{};
    {
        const auto context = WiringContext{wiring};
        const auto first = operation(1);
        const auto repeated = operation(1);
        const auto second = operation(2);
        const auto repeated_again = operation(1);
        CHECK(&first.type() == &TS<double>::type());
        CHECK(&repeated.type() == &TS<double>::type());
        CHECK(&second.type() == &TS<i64>::type());
        CHECK(&repeated_again.type() == &TS<double>::type());
    }

    CHECK(resolver_calls == 2);
    CHECK(requirement_calls == 2);
    CHECK(body_calls == 4);
}

TEST_CASE("overload registration invalidates resolution caches in every live root",
          "[feature:operator][feature:wiring]") {
    auto generic_requirement_calls = 0;
    auto float_requirement_calls = 0;
    auto selected_overload = std::string{};
    auto operation = operator_<auto(TS_T)->TS_T>{"identity", {"value"}};
    operation.overload(graph(
        "name"_n = "generic",
        require =
            [&](const WiringArguments &) {
                ++generic_requirement_calls;
                return true;
            },
        [&](wp<TS<SCALAR>> value) {
            selected_overload = "generic";
            return value;
        }));

    const auto source = node([](Out<TS<float>>) {});
    auto first_root = GraphWiring{};
    auto second_root = GraphWiring{};
    const auto wire = [&](GraphWiring &wiring) {
        const auto context = WiringContext{wiring};
        const auto result = operation(source());
        CHECK(&result.type() == &TS<float>::type());
    };

    wire(first_root);
    CHECK(selected_overload == "generic");
    wire(second_root);
    CHECK(selected_overload == "generic");
    CHECK(generic_requirement_calls == 2);

    operation.overload(graph(
        "name"_n = "float",
        require =
            [&](const WiringArguments &) {
                ++float_requirement_calls;
                return true;
            },
        [&](wp<TS<float>> value) {
            selected_overload = "float";
            return value;
        }));

    wire(first_root);
    CHECK(selected_overload == "float");
    wire(second_root);
    CHECK(selected_overload == "float");
    CHECK(generic_requirement_calls == 4);
    CHECK(float_requirement_calls == 2);
}

TEST_CASE("a cached operator identity is checked when an address is reused",
          "[feature:operator][feature:wiring]") {
    using Operator = Operator;

    alignas(Operator) auto storage = std::array<std::byte, sizeof(Operator)>{};
    auto generic_requirement_calls = 0;
    auto float_requirement_calls = 0;

    const auto source = node([](Out<TS<float>>) {});
    auto wiring = GraphWiring{};
    const auto context = WiringContext{wiring};

    {
        auto *operation = std::construct_at(
            static_cast<Operator *>(static_cast<void *>(storage.data())),
            Signature{util::symbol{"identity"}, {{util::symbol{"value"}, &TS_T::type()}}, TS_T::type()});
        const auto cleanup = util::scope_exit{[&]() noexcept { std::destroy_at(operation); }};
        operation->overload(graph(
            "name"_n = "generic",
            require =
                [&](const WiringArguments &) {
                    ++generic_requirement_calls;
                    return true;
                },
            [](wp<TS<SCALAR>> value) { return value; }));
        (void)(*operation)(source());
        CHECK(generic_requirement_calls == 1);
    }

    {
        auto *operation = std::construct_at(
            static_cast<Operator *>(static_cast<void *>(storage.data())),
            Signature{util::symbol{"identity"}, {{util::symbol{"value"}, &TS_T::type()}}, TS_T::type()});
        const auto cleanup = util::scope_exit{[&]() noexcept { std::destroy_at(operation); }};
        operation->overload(graph(
            "name"_n = "float",
            require =
                [&](const WiringArguments &) {
                    ++float_requirement_calls;
                    return true;
                },
            [](wp<TS<float>> value) { return value; }));
        (void)(*operation)(source());
        CHECK(float_requirement_calls == 1);
    }
}

TEST_CASE("operator cache references survive nested resolution adding cache entries",
          "[feature:operator][feature:wiring][shape:nested]") {
    auto nested_operations = std::vector<std::unique_ptr<Operator>>{};
    for (auto index = 0; index < 64; ++index) {
        auto nested = std::make_unique<Operator>(
            Signature{util::symbol{"nested"}, {{util::symbol{"value"}, &TS_T::type()}}, TS_T::type()});
        nested->overload(graph("name"_n = "identity_impl", [](wp<TS<SCALAR>> value) { return value; }));
        nested_operations.push_back(std::move(nested));
    }

    auto requirement_calls = 0;
    auto operation = operator_<auto(TS_T)->TS_T>{"outer", {"value"}};
    auto input = WiringPoint{};
    operation.overload(graph(
        "name"_n = "outer_impl",
        require =
            [&](const WiringArguments &) {
                ++requirement_calls;
                for (const auto &nested : nested_operations) {
                    (void)(*nested)(input);
                }
                return true;
            },
        [](wp<TS<SCALAR>> value) { return value; }));

    const auto source = node([](Out<TS<float>>) {});
    auto wiring = GraphWiring{};
    const auto context = WiringContext{wiring};
    input = source();

    (void)operation(input);
    (void)operation(input);
    CHECK(requirement_calls == 1);
}

TEST_CASE("an overload can have a different parameter count from its prototype",
          "[feature:operator][feature:wiring][definition:function]") {
    auto selected = false;
    auto operation = operator_<auto(TS_T)->TS_T>{"choose", {"value"}};
    operation.overload(graph("name"_n = "choose_pair", [&](wp<TS<float>> value, wp<TS<float>>) {
        selected = true;
        return value;
    }));

    const auto source = node([](Out<TS<float>>) {});

    testing::test_wiring([&] { (void)operation(source(), source()); });

    CHECK(selected);
}

TEST_CASE("operator prototype names provide keyword aliases for overload parameters",
          "[feature:operator][feature:wiring]") {
    auto selected = std::string{};
    auto operation = operator_<auto(TS_T, TS_T)->TS_T>{"choose", {"value", "fallback"}};
    operation.overload(graph("name"_n = "implementation", [&](wp<TS<float>> first, wp<TS<float>>) {
        selected = "first";
        return first;
    }));

    const auto source = node([](Out<TS<float>>) {});

    testing::test_wiring([&] { (void)operation("fallback"_n = source(), "value"_n = source()); });

    CHECK(selected == "first");
}

TEST_CASE("an operator reports equally specific overloads as ambiguous", "[feature:operator][feature:wiring]") {
    auto operation = operator_<auto(TS<float>)->TS<float>>{"identity", {"value"}};
    operation.overload(bg::graph("name"_n = "first", [](wp<TS<float>> value) { return value; }));
    operation.overload(bg::graph("name"_n = "second", [](wp<TS<float>> value) { return value; }));

    const auto graph = [&](wp<TS<float>> value) { return operation(value); };

    try {
        (void)testing::test_graph(graph, {1.0F});
        FAIL("expected ambiguous operator wiring");
    } catch (const WiringError &error) {
        REQUIRE(error.diagnostic());
        CHECK(error.diagnostic()->ambiguous);
        CHECK(error.diagnostic()->candidates.size() == 2);
        CHECK(std::string{error.what()}.contains("Ambiguous operator identity"));
    }
}

TEST_CASE("operator candidate resolution returns the selected implementation and concrete forms",
          "[feature:operator][feature:wiring]") {
    auto operation = bg::operator_<auto(bg::TS_T) -> bg::TS_T>{"resolve_candidates", {"value"}};
    const auto &generic = operation.overload(
        bg::graph("name"_n = "generic", [](bg::wp<bg::TS<bg::SCALAR>> value) { return value; }));
    const auto &exact = operation.overload(
        bg::graph("name"_n = "exact", [](bg::wp<bg::TS<float>> value) { return value; }));
    const auto source = bg::node([](bg::Out<bg::TS<float>>) {});
    const auto all = std::array<const bg::WiringTemplate *, 2>{&generic, &exact};
    const auto generic_only = std::array<const bg::WiringTemplate *, 1>{&generic};
    const auto exact_only = std::array<const bg::WiringTemplate *, 1>{&exact};

    bg::testing::test_wiring([&] {
        const auto value = source();
        const auto arguments = std::array{bg::WiringArgument{static_cast<bg::WiringPoint>(value)}};
        const auto resolution = operation.resolve_overloads(bg::WiringContext::current().wiring(), arguments,
                                                            nullptr, all);

        REQUIRE(resolution.selected);
        CHECK(resolution.selected->implementation == &exact);
        CHECK(resolution.selected->resolution.signature.output == &bg::TS<float>::type());
        REQUIRE(resolution.selected->argument_forms.size() == 1);
        CHECK(resolution.selected->argument_forms.front() == nullptr);
    });

    bg::testing::test_wiring([&] {
        const auto value = source();
        const auto arguments = std::array{bg::WiringArgument{static_cast<bg::WiringPoint>(value)}};
        const auto resolution = operation.resolve_overloads(bg::WiringContext::current().wiring(), arguments,
                                                            nullptr, generic_only);

        REQUIRE(resolution.selected);
        CHECK(resolution.selected->implementation == &generic);
    });

    const auto integer_source = bg::node([](bg::Out<bg::TS<bg::i64>>) {});
    bg::testing::test_wiring([&] {
        const auto value = integer_source();
        const auto arguments = std::array{bg::WiringArgument{static_cast<bg::WiringPoint>(value)}};
        const auto resolution = operation.resolve_overloads(bg::WiringContext::current().wiring(), arguments,
                                                            nullptr, exact_only);

        CHECK_FALSE(resolution.selected);
        CHECK_FALSE(resolution.ambiguous);
        CHECK_FALSE(resolution.failures.empty());
    });
}

TEST_CASE("operator candidate resolution reports ambiguity and callback failures",
          "[feature:operator][feature:wiring]") {
    auto ambiguous = bg::operator_<auto(bg::TS<float>) -> bg::TS<float>>{"ambiguous_candidates", {"value"}};
    const auto &first = ambiguous.overload(
        bg::graph("name"_n = "first", [](bg::wp<bg::TS<float>> value) { return value; }));
    const auto &second = ambiguous.overload(
        bg::graph("name"_n = "second", [](bg::wp<bg::TS<float>> value) { return value; }));
    const auto source = bg::node([](bg::Out<bg::TS<float>>) {});
    const auto candidates = std::array<const bg::WiringTemplate *, 2>{&first, &second};

    bg::testing::test_wiring([&] {
        const auto value = source();
        const auto arguments = std::array{bg::WiringArgument{static_cast<bg::WiringPoint>(value)}};
        const auto resolution = ambiguous.resolve_overloads(bg::WiringContext::current().wiring(), arguments,
                                                            nullptr, candidates);

        CHECK_FALSE(resolution.selected);
        CHECK(resolution.ambiguous);
        CHECK(resolution.failures.size() == 2);
    });

    auto callback_failure = bg::operator_<auto(bg::TS<float>) -> bg::TS<float>>{"callback_failure", {"value"}};
    const auto &throws = callback_failure.overload(bg::graph(
        "name"_n = "throws",
        bg::require = [](const bg::WiringArguments &) -> bool { throw std::runtime_error{"requirement failed"}; },
        [](bg::wp<bg::TS<float>> value) { return value; }));
    const auto throwing_candidate = std::array<const bg::WiringTemplate *, 1>{&throws};

    bg::testing::test_wiring([&] {
        const auto value = source();
        const auto arguments = std::array{bg::WiringArgument{static_cast<bg::WiringPoint>(value)}};
        try {
            (void)callback_failure.resolve_overloads(bg::WiringContext::current().wiring(), arguments, nullptr,
                                                     throwing_candidate);
            FAIL("expected a callback failure");
        } catch (const bg::WiringError &error) {
            REQUIRE(error.diagnostic());
            REQUIRE_FALSE(error.diagnostic()->candidates.empty());
            CHECK(error.diagnostic()->candidates.back().stage == bg::WiringFailureStage::callback);
        }
    });
}

TEST_CASE("opt selects optional inputs and keeps that choice distinct in the cache",
          "[feature:operator][feature:wiring][feature:validity][validity:optional][series:value]") {
    auto selected = std::string{};
    auto operation = operator_<auto(TS_T)->TS_T>{"optional", {"value"}};
    operation.overload(graph("name"_n = "required", [&](wp<TS<i64>> value) {
        selected = "required";
        return value;
    }));
    operation.overload(graph("name"_n = "optional", [&](wp<TS<i64>, bg::optional> value) {
        selected = "optional";
        return value;
    }));

    const auto source = node([](Out<TS<i64>>) {});
    testing::test_wiring([&] {
        const auto value = source();

        (void)operation(value);
        CHECK(selected == "required");

        (void)operation(opt(value));
        CHECK(selected == "optional");

        (void)operation(value);
        CHECK(selected == "required");
    });
}

TEST_CASE("valid_items selects capable overloads and prefers ordinary inputs otherwise",
          "[feature:operator][feature:wiring][feature:validity][series:tsd]"
          "[input-guarantee:valid-items]") {
    auto selected = std::string{};
    using TSD = TSD<i64, TS<i64>>;
    auto operation = operator_<auto(TSD)->TS<i64>>{"valid_items", {"value"}};
    operation.overload(graph("name"_n = "ordinary", [&](wp<TSD> value) {
        selected = "ordinary";
        return value;
    }));
    operation.overload(graph("name"_n = "valid_items", [&](wp<TSD, valid_items> value) {
        selected = "valid_items";
        return value;
    }));

    const auto source = node([](Out<TSD>) {});
    testing::test_wiring([&] {
        const auto value = source();

        (void)operation(value);
        CHECK(selected == "ordinary");

        (void)operation(valid_items(value));
        CHECK(selected == "valid_items");

        (void)operation(value);
        CHECK(selected == "ordinary");
    });
}

TEST_CASE("scalar-only overloads prefer presence and keep absent calls distinct in the cache",
          "[feature:operator][feature:wiring][scalar:integer][definition:function]") {
    const auto required = graph("name"_n = "requires_presence", [](i64 value) {
        return operators::const_(value + 10).as<TS<i64, TimeSeriesGuarantee::const_>>();
    });

    const auto optional = graph("name"_n = "accepts_absence", [](std::optional<i64> value) {
        return operators::const_(value.value_or(0) + 20).as<TS<i64, TimeSeriesGuarantee::const_>>();
    });

    auto required_first = operator_<auto(i64)->TS<i64>>{"presence", {"value"}};
    required_first.overload(required);
    required_first.overload(optional);

    auto optional_first = operator_<auto(i64)->TS<i64>>{"presence", {"value"}};
    optional_first.overload(optional);
    optional_first.overload(required);

    const auto required_first_graph = [&] { return required_first(i64{1}); };
    const auto required_first_absent = [&] { return required_first(std::optional<i64>{}); };
    const auto required_first_omitted = [&] { return required_first(); };
    const auto optional_first_graph = [&] { return optional_first(i64{1}); };
    const auto optional_first_absent = [&] { return optional_first(std::optional<i64>{}); };
    const auto optional_first_omitted = [&] { return optional_first(); };

    CHECK(testing::test_graph(required_first_graph).equals({11}));
    CHECK(testing::test_graph(required_first_absent).equals({20}));
    CHECK(testing::test_graph(required_first_omitted).equals({20}));
    CHECK(testing::test_graph(required_first_omitted).equals({20}));
    CHECK(testing::test_graph(required_first_graph).equals({11}));
    CHECK(testing::test_graph(optional_first_graph).equals({11}));
    CHECK(testing::test_graph(optional_first_absent).equals({20}));
    CHECK(testing::test_graph(optional_first_omitted).equals({20}));
}

TEST_CASE("a graph can omit an optional scalar parameter",
          "[feature:wiring][scalar:integer][definition:function]") {
    const auto optional_graph = graph("name"_n = "optional_scalar", [](std::optional<i64> value) {
        return operators::const_(value.value_or(7)).as<TS<i64, TimeSeriesGuarantee::const_>>();
    });
    const auto omitted_graph = [&] { return optional_graph(); };

    CHECK(testing::test_graph(omitted_graph).equals({7}));
}

TEST_CASE("promoted node overloads prefer presence for scalar arguments",
          "[feature:operator][feature:wiring][scalar:integer][definition:function]") {
    const auto required = node([](int value) { return value + 10; });

    const auto optional = node([](std::optional<int> value) { return value.value_or(0) + 20; });
    auto operation = operator_<auto(i64)->TS<i64>>{"promoted_presence", {"value"}};
    operation.overload(required);
    operation.overload(optional);

    const auto required_graph = [&] { return operation(i64{1}); };
    const auto absent_graph = [&] { return operation(std::optional<i64>{}); };
    const auto omitted_graph = [&] { return operation(); };

    CHECK(testing::test_graph(required_graph).equals({11}));
    CHECK(testing::test_graph(absent_graph).equals({20}));
    CHECK(testing::test_graph(omitted_graph).equals({20}));
    CHECK(testing::test_graph(omitted_graph).equals({20}));
    CHECK(testing::test_graph(required_graph).equals({11}));
}

TEST_CASE("named omission and scalar presence remain distinct in one root wiring",
          "[feature:operator][feature:wiring][scalar:integer][definition:function]") {
    auto operation = operator_<auto(i64, i64)->TS<i64>>{"named_omission", {"first", "second"}};
    operation.overload(bg::graph("name"_n = "required_first", [](i64 first, i64 second) {
        return operators::const_(100 + first + second).as<TS<i64, TimeSeriesGuarantee::const_>>();
    }));
    operation.overload(bg::graph("name"_n = "optional_first", [](std::optional<i64> first, i64 second) {
        return operators::const_(first.value_or(0) + second).as<TS<i64, TimeSeriesGuarantee::const_>>();
    }));

    const auto add = node([](i64 lhs, i64 rhs) { return lhs + rhs; });
    const auto graph = [&] {
        const auto omitted = operation("second"_n = i64{5});
        const auto omitted_again = operation("second"_n = i64{5});
        const auto present = operation(i64{2}, "second"_n = i64{5});
        const auto present_again = operation(i64{2}, "second"_n = i64{5});
        return add(add(omitted, present), add(omitted_again, present_again));
    };

    CHECK(testing::test_graph(graph).equals({224}));
}

TEST_CASE("type specificity precedes scalar presence preference",
          "[feature:operator][feature:wiring][scalar:integer][definition:function]") {
    const auto promoted = node([](int value) { return value + 100; });

    const auto explicit_scalar = bg::graph("name"_n = "optional_scalar", [](std::optional<i64> value) {
        return operators::const_(value.value_or(0) + 200).as<TS<i64, TimeSeriesGuarantee::const_>>();
    });
    auto operation = operator_<auto(i64)->TS<i64>>{"specificity", {"value"}};
    operation.overload(promoted);
    operation.overload(explicit_scalar);

    const auto graph = [&] { return operation(i64{1}); };
    CHECK(testing::test_graph(graph).equals({201}));
}

TEST_CASE("conflicting scalar presence preferences remain ambiguous",
          "[feature:operator][feature:wiring][scalar:integer][definition:function]") {
    auto operation = operator_<auto(i64, i64)->TS<i64>>{"presence", {"lhs", "rhs"}};
    operation.overload(graph("name"_n = "required_lhs", [](i64 lhs, std::optional<i64>) {
        return operators::const_(lhs).as<TS<i64, TimeSeriesGuarantee::const_>>();
    }));
    operation.overload(graph("name"_n = "required_rhs", [](std::optional<i64>, i64 rhs) {
        return operators::const_(rhs).as<TS<i64, TimeSeriesGuarantee::const_>>();
    }));

    try {
        testing::test_wiring([&] { (void)operation(i64{1}, i64{2}); });
        FAIL("expected conflicting scalar presence preferences to remain ambiguous");
    } catch (const WiringError &error) {
        REQUIRE(error.diagnostic());
        CHECK(error.diagnostic()->ambiguous);
        CHECK(error.diagnostic()->candidates.size() == 2);
    }
}

TEST_CASE("scalar presence and time-series validity preferences share one partial order",
          "[feature:operator][feature:wiring][scalar:integer][validity:optional][series:value]"
          "[definition:function]") {
    auto selected = std::string{};
    auto operation = operator_<auto(i64, TS<i64>)->TS<i64>>{"presence_and_validity", {"value", "trigger"}};
    operation.overload(graph("name"_n = "required_scalar", [&](i64 value, wp<TS<i64>, bg::optional>) {
        selected = "required_scalar";
        return operators::const_(value).as<TS<i64, TimeSeriesGuarantee::const_>>();
    }));
    operation.overload(graph("name"_n = "optional_scalar", [&](std::optional<i64> value, wp<TS<i64>>) {
        selected = "optional_scalar";
        return operators::const_(value.value_or(0)).as<TS<i64, TimeSeriesGuarantee::const_>>();
    }));

    const auto trigger = node([](Out<TS<i64>>) {});
    try {
        testing::test_wiring([&] { (void)operation(i64{1}, trigger()); });
        FAIL("expected scalar and time-series preferences to conflict");
    } catch (const WiringError &error) {
        REQUIRE(error.diagnostic());
        CHECK(error.diagnostic()->ambiguous);
        CHECK(error.diagnostic()->candidates.size() == 2);
    }

    testing::test_wiring([&] {
        const auto optional_trigger = opt(trigger());
        const auto result = operation(i64{1}, optional_trigger);
        CHECK(result.has_output());
        CHECK(result.type().accepts(TS<i64, TimeSeriesGuarantee::const_>::type()));
        CHECK(selected == "required_scalar");
    });
}

TEST_CASE("a required scalar graph rejects absence before running its body",
          "[feature:operator][feature:wiring][scalar:integer][definition:function]") {
    auto body_calls = 0;

    const auto graph = bg::graph("name"_n = "required_scalar", [&](i64 value) {
        ++body_calls;
        return operators::const_(value).as<TS<i64, TimeSeriesGuarantee::const_>>();
    });

    try {
        testing::test_wiring([&] { (void)graph(std::optional<i64>{}); });
        FAIL("expected an absent scalar to be rejected");
    } catch (const TypeResolutionError &error) {
        CHECK(error.code() == TypeResolutionErrorCode::mismatch);
        CHECK(std::string{error.what()}.contains("required scalar parameter has no value"));
    }
    CHECK_THROWS_AS(testing::test_wiring([&] { (void)graph(); }), WiringError);
    CHECK(body_calls == 0);
}

TEST_CASE("an operator rejects absent required scalars before overload wiring",
          "[feature:operator][feature:wiring][scalar:integer]") {
    auto overload_calls = 0;
    auto operation = operator_<auto(i64)->TS<i64>>{"required", {"value"}};
    operation.overload(graph("name"_n = "required", [&](i64 value) {
        ++overload_calls;
        return operators::const_(value).as<TS<i64, TimeSeriesGuarantee::const_>>();
    }));

    try {
        testing::test_wiring([&] { (void)operation(std::optional<i64>{}); });
        FAIL("expected an absent scalar to be rejected");
    } catch (const WiringError &error) {
        REQUIRE(error.diagnostic());
        REQUIRE(error.diagnostic()->candidates.size() == 1);
        CHECK(error.diagnostic()->candidates.front().stage == WiringFailureStage::type_match);
        CHECK(error.diagnostic()->candidates.front().message.find("required scalar parameter has no value") !=
              std::string::npos);
    }
    CHECK_THROWS_AS(testing::test_wiring([&] { (void)operation(); }), WiringError);
    CHECK(overload_calls == 0);
}

TEST_CASE("all_valid is preferred over optional for an unmarked input",
          "[feature:operator][feature:wiring][feature:validity][validity:all-valid]") {
    auto operation = operator_<auto(TS_T)->TS_T>{"validity", {"value"}};
    operation.overload(node([](In<TS<i64>, all_valid> value, Out<TS<i64>> output) { output.set(value.value()); }));
    operation.overload(node([](In<TS<i64>, bg::optional>, Out<TS<i64>> output) { output.set(0); }));

    const auto graph = [&](wp<TS<i64>> value) { return operation(value); };

    CHECK(testing::test_graph(graph, {1, 2}).equals<i64>({1, 2}));
}

TEST_CASE("required and all_valid overloads are equally specific",
          "[feature:operator][feature:wiring][feature:validity][validity:all-valid]") {
    auto operation = operator_<auto(TS_T)->TS_T>{"validity", {"value"}};
    operation.overload(node([](In<TS<i64>>, Out<TS<i64>> output) { output.set(1); }));
    operation.overload(node([](In<TS<i64>, all_valid>, Out<TS<i64>> output) { output.set(2); }));

    const auto graph = [&](wp<TS<i64>> value) { return operation(value); };

    try {
        (void)testing::test_graph(graph, {1});
        FAIL("expected required and all_valid overloads to be ambiguous");
    } catch (const WiringError &error) {
        REQUIRE(error.diagnostic());
        CHECK(error.diagnostic()->ambiguous);
        CHECK(error.diagnostic()->candidates.size() == 2);
        CHECK(error.diagnostic()->candidates[1].signature.parameters.front().validity == InputValidity::all_valid);
        CHECK(std::string{error.what()}.contains("all_valid "));
    }
}

TEST_CASE("opt-qualified arguments do not match all_valid inputs",
          "[feature:operator][feature:wiring][feature:validity][validity:all-valid][validity:optional]") {
    auto operation = operator_<auto(TS_T)->TS_T>{"validity", {"value"}};
    operation.overload(node([](In<TS<i64>, all_valid>, Out<TS<i64>> output) { output.set(0); }));

    const auto graph = [&](wp<TS<i64>> value) { return operation(opt(value)); };

    CHECK_THROWS_AS(testing::test_graph(graph, {1}), WiringError);
}

TEST_CASE("type specificity precedes required-input preference",
          "[feature:operator][feature:wiring][feature:validity][validity:optional][series:value]") {
    auto selected = std::string{};
    auto operation = operator_<auto(TS_T)->TS_T>{"specificity", {"value"}};
    operation.overload(graph("name"_n = "required_generic", [&](wp<TS<SCALAR>> value) {
        selected = "required_generic";
        return value;
    }));
    operation.overload(graph("name"_n = "optional_exact", [&](wp<TS<i64>, bg::optional> value) {
        selected = "optional_exact";
        return value;
    }));

    const auto source = node([](Out<TS<i64>>) {});
    testing::test_wiring([&] { (void)operation(source()); });

    CHECK(selected == "optional_exact");
}

TEST_CASE("conflicting required-input preferences remain ambiguous",
          "[feature:operator][feature:wiring][feature:validity][validity:optional][series:value]") {
    auto operation = operator_<auto(TS_T, TS_T)->TS_T>{"validity", {"lhs", "rhs"}};
    operation.overload(
        graph("name"_n = "required_lhs", [](wp<TS<i64>> lhs, wp<TS<i64>, bg::optional> rhs) { return lhs; }));
    operation.overload(
        graph("name"_n = "required_rhs", [](wp<TS<i64>, bg::optional> lhs, wp<TS<i64>> rhs) { return rhs; }));

    const auto source = node([](Out<TS<i64>>) {});
    try {
        testing::test_wiring([&] { (void)operation(source(), source()); });
        FAIL("expected ambiguous operator wiring");
    } catch (const WiringError &error) {
        REQUIRE(error.diagnostic());
        CHECK(error.diagnostic()->ambiguous);
        CHECK(error.diagnostic()->candidates.size() == 2);
    }
}

TEST_CASE("named validity preferences align with supplied arguments",
          "[feature:operator][feature:wiring][feature:validity][input-guarantee:valid-items][series:tsd]") {
    const auto source = node([](Out<TSD<i64, TS<i64>>>) {});

    const auto left_first = graph(
        "name"_n = "left_first", [](wp<TSD<i64, TS<i64>>, "left"_n, valid_items>, wp<TSD<i64, TS<i64>>, "right"_n>) {});

    const auto right_first = graph("name"_n = "right_first", [](wp<TSD<i64, TS<i64>>, "right"_n>,
                                                                wp<TSD<i64, TS<i64>>, "left"_n, valid_items>) {});

    for (const auto reverse : {false, true}) {
        auto operation = operator_<auto(TS_T, TS_T)->void>{"validity", {"left", "right"}};
        if (reverse) {
            operation.overload(right_first);
            operation.overload(left_first);
        } else {
            operation.overload(left_first);
            operation.overload(right_first);
        }

        try {
            testing::test_wiring([&] { operation("left"_n = valid_items(source()), "right"_n = source()); });
            FAIL("expected named validity preferences to be ambiguous");
        } catch (const WiringError &error) {
            REQUIRE(error.diagnostic());
            CHECK(error.diagnostic()->ambiguous);
            CHECK(error.diagnostic()->candidates.size() == 2);
        }
    }
}

TEST_CASE("positional validity preferences follow supplied argument order",
          "[feature:operator][feature:wiring][feature:validity][input-guarantee:valid-items][series:tsd]") {
    const auto source = node([](Out<TSD<i64, TS<i64>>>) {});

    for (const auto reverse : {false, true}) {
        auto selected = std::string{};
        auto operation = operator_<auto(TS_T, TS_T)->void>{"validity", {"left", "right"}};
        if (reverse) {
            operation.overload(graph("name"_n = "right_first",
                                     [&](wp<TSD<i64, TS<i64>>, "right"_n>,
                                         wp<TSD<i64, TS<i64>>, "left"_n, valid_items>) { selected = "right_first"; }));
            operation.overload(
                graph("name"_n = "left_first", [&](wp<TSD<i64, TS<i64>>, "left"_n, valid_items>,
                                                   wp<TSD<i64, TS<i64>>, "right"_n>) { selected = "left_first"; }));
        } else {
            operation.overload(
                graph("name"_n = "left_first", [&](wp<TSD<i64, TS<i64>>, "left"_n, valid_items>,
                                                   wp<TSD<i64, TS<i64>>, "right"_n>) { selected = "left_first"; }));
            operation.overload(graph("name"_n = "right_first",
                                     [&](wp<TSD<i64, TS<i64>>, "right"_n>,
                                         wp<TSD<i64, TS<i64>>, "left"_n, valid_items>) { selected = "right_first"; }));
        }

        testing::test_wiring([&] { operation(valid_items(source()), source()); });
        CHECK(selected == "left_first");
    }
}

TEST_CASE("an opt-qualified argument rejects a required-only overload",
          "[feature:operator][feature:wiring][feature:validity][validity:optional][series:value]") {
    auto operation = operator_<auto(TS_T)->TS_T>{"required", {"value"}};
    operation.overload(graph("name"_n = "required", [](wp<TS<i64>> value) { return value; }));

    const auto source = node([](Out<TS<i64>>) {});

    try {
        testing::test_wiring([&] { (void)operation(opt(source())); });
        FAIL("expected an optional-input mismatch");
    } catch (const WiringError &error) {
        REQUIRE(error.diagnostic());
        REQUIRE(error.diagnostic()->arguments.size() == 1);
        CHECK(error.diagnostic()->arguments.front().validity == InputValidity::optional);
        REQUIRE(error.diagnostic()->candidates.size() == 1);
        CHECK(error.diagnostic()->candidates.front().signature.parameters.front().validity == InputValidity::required);
        CHECK(std::string{error.what()}.contains("opt TS<i64>"));
    }
}

TEST_CASE("an operator reports expected and supplied types for a mismatch", "[feature:operator][feature:wiring]") {
    auto operation = operator_<auto(TS<float>)->TS<float>>{"identity", {"value"}};
    operation.overload(bg::graph("name"_n = "float_identity", [](wp<TS<float>> value) { return value; }));

    const auto graph = [&](wp<TS<double>> value) { return operation(value); };

    try {
        (void)testing::test_graph(graph, {1.0});
        FAIL("expected unmatched operator wiring");
    } catch (const WiringError &error) {
        REQUIRE(error.diagnostic());
        REQUIRE(error.diagnostic()->candidates.size() == 1);
        const auto &failure = error.diagnostic()->candidates.front();
        CHECK(failure.stage == WiringFailureStage::type_match);
        CHECK(failure.expected != nullptr);
        CHECK(failure.supplied != nullptr);
        CHECK(std::string{error.what()}.contains("received TS<double>"));
    }
}

TEST_CASE("operator diagnostics own signatures and retain prototype argument names",
          "[feature:operator][feature:wiring]") {
    const auto error = [] {
        auto operation = operator_<auto(TS<float>)->TS<float>>{"identity", {"value"}};
        operation.overload(graph("name"_n = "float_identity", [](wp<TS<float>> value) { return value; }));

        try {
            testing::test_wiring([&] {
                const auto source = node([](Out<TS<double>>) {});
                (void)operation(source());
            });
        } catch (const WiringError &caught) {
            return caught;
        }

        throw std::logic_error{"expected unmatched operator wiring"};
    }();

    REQUIRE(error.diagnostic());
    REQUIRE(error.diagnostic()->arguments.size() == 1);
    CHECK(error.diagnostic()->arguments.front().name.text() == "value");
    REQUIRE(error.diagnostic()->candidates.size() == 1);
    CHECK(error.diagnostic()->candidates.front().signature.name.text() == "float_identity");
    CHECK(std::string{error.what()}.contains("value: TS<double>"));
}

TEST_CASE("operator matching treats a reference argument transparently",
          "[feature:operator][feature:wiring][feature:reference][binding:reference-to-value]") {
    const auto source = node([](Out<REF<TS<float>>>) {});
    auto operation = operator_<auto(TS<float>)->TS<float>>{"identity", {"value"}};
    operation.overload(graph("name"_n = "direct", [](wp<TS<float>> value) { return value; }));

    testing::test_wiring([&] {
        const auto output = operation(source());
        CHECK(&output.type() == &REF<TS<float>, TimeSeriesGuarantee::deduplicated>::type());
    });
}

TEST_CASE("an operator prefers the overload with the argument's reference shape",
          "[feature:operator][feature:wiring][feature:reference][binding:reference-to-reference]") {
    auto selected = std::string{};
    auto operation = operator_<auto(TS<float>)->TS<float>>{"identity", {"value"}};
    operation.overload(graph("name"_n = "direct", [&](wp<TS<float>> value) {
        selected = "direct";
        return value;
    }));
    operation.overload(graph("name"_n = "reference", [&](wp<REF<TS<float>>> value) {
        selected = "reference";
        return value;
    }));

    const auto direct_source = node([](Out<TS<float>>) {});

    const auto reference_source = node([](Out<REF<TS<float>>>) {});

    testing::test_wiring([&] {
        operation(direct_source());
        CHECK(selected == "direct");
    });

    testing::test_wiring([&] {
        operation(reference_source());
        CHECK(selected == "reference");
    });
}

TEST_CASE("an operator prefers the overload with a contained reference shape",
          "[feature:operator][feature:wiring][feature:reference][series:tsd]") {
    auto selected = std::string{};
    auto operation = operator_<auto(TS_T)->TS_T>{"identity", {"value"}};
    operation.overload(graph("name"_n = "value", [&](wp<TSD<i64, TS<float>>> value) {
        selected = "value";
        return value;
    }));
    operation.overload(graph("name"_n = "reference", [&](wp<TSD<i64, REF<TS<float>>>> value) {
        selected = "reference";
        return value;
    }));

    const auto value_source = node([](Out<TSD<i64, TS<float>>>) {});

    const auto reference_source = node([](Out<TSD<i64, REF<TS<float>>>>) {});

    testing::test_wiring([&] {
        operation(value_source());
        CHECK(selected == "value");
    });

    testing::test_wiring([&] {
        operation(reference_source());
        CHECK(selected == "reference");
    });
}

TEST_CASE("an operator prefers a generic overload with the argument's contained reference shape",
          "[feature:operator][feature:wiring][feature:reference][series:fixed-tsl]"
          "[declaration:generic]") {
    auto selected = std::string{};
    auto operation = operator_<auto(TS_T)->TS_T>{"identity", {"value"}};
    operation.overload(graph("name"_n = "value", [&](wp<TSL<TS_T, SIZE>> value) {
        selected = "value";
        return value;
    }));
    operation.overload(graph("name"_n = "reference", [&](wp<TSL<REF<TS_T>, SIZE>> value) {
        selected = "reference";
        return value;
    }));

    const auto value_source = node([](Out<TSL<TS<float>, 2>>) {});

    const auto reference_source = node([](Out<TSL<REF<TS<float>>, 2>>) {});

    testing::test_wiring([&] {
        operation(value_source());
        CHECK(selected == "value");
    });

    testing::test_wiring([&] {
        operation(reference_source());
        CHECK(selected == "reference");
    });
}

TEST_CASE("conflicting contained reference preferences are ambiguous",
          "[feature:operator][feature:wiring][feature:reference][series:fixed-tsl]"
          "[declaration:generic]") {
    auto operation = operator_<auto(TS_T, TS_T)->TS_T>{"choose", {"left", "right"}};
    operation.overload(graph("name"_n = "values", [](wp<TSL<TS_T, SIZE>> left, wp<TSL<TS_T, SIZE>>) { return left; }));
    operation.overload(
        graph("name"_n = "references", [](wp<TSL<REF<TS_T>, SIZE>> left, wp<TSL<REF<TS_T>, SIZE>>) { return left; }));

    const auto value_source = node([](Out<TSL<TS<float>, 2>>) {});

    const auto reference_source = node([](Out<TSL<REF<TS<float>>, 2>>) {});

    CHECK_THROWS_AS(testing::test_wiring([&] { operation(reference_source(), value_source()); }), WiringError);
}

TEST_CASE("an output operator returns an empty wiring point for a sink overload",
          "[feature:operator][feature:wiring]") {
    auto operation = operator_<auto(TS<float>)->TS<float>>{"identity", {"value"}};

    const auto sink = graph("name"_n = "sink", [](wp<TS<float>>) {});

    const auto source = node([](Out<TS<float>>) {});
    operation.overload(sink);

    testing::test_wiring([&] { CHECK_FALSE(operation(source()).has_output()); });
}

TEST_CASE("an outputless operator rejects a value overload", "[feature:operator][feature:wiring]") {
    auto operation = operator_<void(TS<float>)>{"consume", {"value"}};

    const auto identity = graph("name"_n = "identity", [](wp<TS<float>> value) { return value; });

    CHECK_THROWS_AS(operation.overload(identity), std::invalid_argument);
}

TEST_CASE("an outputless operator invokes an outputless overload",
          "[feature:operator][feature:wiring][definition:function]") {
    auto called = false;
    auto operation = operator_<void(int)>{"consume", {"value"}};
    operation.overload(graph("name"_n = "sink", [&](int) { called = true; }));

    STATIC_CHECK(std::same_as<decltype(operation(1)), void>);

    testing::test_wiring([&] { operation(1); });

    CHECK(called);
}

TEST_CASE("a function type declares a public output operator",
          "[feature:operator][feature:type-system][definition:function]") {
    const auto operation = operator_<auto(TS_T, TS_T)->TS_T>{"add", {"lhs", "rhs"}};

    CHECK(operation.signature().name().text() == "add");
    REQUIRE(operation.signature().parameter_count() == 2);
    CHECK(operation.signature().parameter(0).name.text() == "lhs");
    CHECK(operation.signature().parameter(1).name.text() == "rhs");
    CHECK(operation.signature().has_output());
}

TEST_CASE("a function type declares a public outputless operator",
          "[feature:operator][feature:type-system][definition:function]") {
    const auto operation = operator_<void(int)>{"consume", {"value"}};

    CHECK(operation.signature().name().text() == "consume");
    REQUIRE(operation.signature().parameter_count() == 1);
    CHECK(operation.signature().parameter(0).name.text() == "value");
    CHECK(operation.signature().parameter(0).type == &int64_type());
    CHECK_FALSE(operation.signature().has_output());
}

TEST_CASE("an operator prototype names every parameter", "[feature:operator][feature:type-system]") {
    CHECK_THROWS_AS(operator_<void(int)>{"consume"}, std::invalid_argument);
}

} // namespace
