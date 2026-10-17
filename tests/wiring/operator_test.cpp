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

using namespace bg::literals;

namespace {

TEST_CASE("operator scalar type-variable results retain their identity",
          "[feature:operator][feature:type-system][declaration:generic][series:value][scalar:generic]") {
    using VALUE = bg::ScalarVariable<"VALUE">;

    const auto generic = bg::operator_<auto(bg::SCALAR) -> bg::SCALAR>{"generic_scalar_result", {"value"}};
    auto named = bg::operator_<VALUE, auto(VALUE) -> VALUE>{"named_scalar_result", {"value"}};

    CHECK(generic.signature().parameter(0).type == &bg::SCALAR::type());
    CHECK(generic.signature().output() == &bg::TS<bg::SCALAR>::type());
    CHECK(named.signature().parameter(0).type == &VALUE::type());
    CHECK(named.signature().output() == &bg::TS<VALUE>::type());

    named.overload(bg::graph("name"_n = "constant", [](double value) {
        return bg::operators::const_(value).as<bg::TS<double, bg::TimeSeriesGuarantee::const_>>();
    }));
    bg::testing::test_wiring([&] {
        const auto result = named[bg::type<VALUE> = bg::type<double>](2.5);
        CHECK(&result.type() == &bg::TS<double, bg::TimeSeriesGuarantee::const_>::type());
    });
}

TEST_CASE("operator concrete scalar results use canonical time-series declarations",
          "[feature:operator][feature:type-system][declaration:concrete][series:value]"
          "[scalar:floating-point][scalar:integer]") {
    const auto double_result = bg::operator_<auto() -> double>{"double_scalar_result"};
    const auto integer_result = bg::operator_<auto() -> int>{"integer_scalar_result"};

    CHECK(double_result.signature().output() == &bg::TS<double>::type());
    CHECK(integer_result.signature().output() == &bg::TS<bg::i64>::type());
}

TEST_CASE("operator scalar-result prototypes do not constrain overload arguments or outputs",
          "[feature:operator][feature:wiring][series:value][guarantee:final][declaration:concrete]") {
    auto operation = bg::operator_<auto(bg::SCALAR) -> bg::SCALAR>{"final_identity", {"value"}};
    const auto implementation = bg::graph(
        "name"_n = "final_identity",
        [](bg::wp<bg::TS<double, bg::TimeSeriesGuarantee::final>> value) { return value; });
    operation.overload(implementation);
    const auto source = bg::node([](bg::Out<bg::TS<double, bg::TimeSeriesGuarantee::final>>) {});

    bg::testing::test_wiring([&] {
        const auto result = operation(source());
        CHECK(&result.type() == &bg::TS<double, bg::TimeSeriesGuarantee::final>::type());
    });
}

TEST_CASE("explicit time-series and sink operator prototypes keep their result form",
          "[feature:operator][feature:wiring][series:value]") {
    const auto time_series = bg::operator_<auto() -> bg::TS<double>>{"time_series_result"};
    const auto sink = bg::operator_<void(double)>{"sink", {"value"}};

    CHECK(time_series.signature().output() == &bg::TS<double>::type());
    CHECK_FALSE(sink.signature().has_output());
}

TEST_CASE("an operator selects the least generic applicable overload",
          "[feature:operator][feature:wiring][declaration:generic]") {
    auto operation = bg::operator_<auto(bg::TS_T, bg::TS_T)->bg::TS_T>{"select", {"value", "fallback"}};
    operation.overload(bg::graph("name"_n = "generic", [](bg::wp<bg::TS<bg::SCALAR>>,
                                                          bg::wp<bg::TS<bg::SCALAR>> fallback) { return fallback; }));
    operation.overload(
        bg::graph("name"_n = "float_exact", [](bg::wp<bg::TS<float>> value, bg::wp<bg::TS<float>>) { return value; }));

    const auto float_graph = [&](bg::wp<bg::TS<float>> value, bg::wp<bg::TS<float>> fallback) -> bg::wp<bg::TS<float>> {
        return operation(value, fallback).as<bg::TS<float>>();
    };

    const auto double_graph = [&](bg::wp<bg::TS<double>> value,
                                  bg::wp<bg::TS<double>> fallback) -> bg::wp<bg::TS<double>> {
        return operation(value, fallback).as<bg::TS<double>>();
    };

    CHECK(bg::testing::test_graph(float_graph, std::array{1.0F, 2.0F}, std::array{10.0F, 20.0F}).equals({1.0F, 2.0F}));
    CHECK(bg::testing::test_graph(double_graph, std::array{1.0, 2.0}, std::array{10.0, 20.0}).equals({10.0, 20.0}));
}

TEST_CASE("operator specificity is independent of overload registration order",
          "[feature:operator][feature:wiring][declaration:generic][series:value]") {
    using EQUALITY_SCALAR = bg::ScalarVariable<"EQUALITY_SCALAR", bg::EqualityComparable>;
    constexpr auto G = bg::guarantee_variable<"G">;

    const auto check_registration_orders = [&](auto order, bg::i64 expected_result, std::size_t selected_overload) {
        do {
            auto selected = std::array<int, 4>{};
            auto operation = bg::operator_<auto(bg::TS_T) -> bg::TS<bg::i64>>{"specificity_order", {"value"}};
            const auto exact_result = bg::node([](bg::Out<bg::TS<bg::i64>> output) { output.set(1); });
            const auto equality_result = bg::node([](bg::Out<bg::TS<bg::i64>> output) { output.set(3); });
            const auto scalar_result = bg::node([](bg::Out<bg::TS<bg::i64>> output) { output.set(4); });
            const auto promoted = bg::node([&](bg::i64 value) {
                ++selected[1];
                return value + 10;
            });

            const auto add_overload = [&](int index) {
                switch (index) {
                case 0:
                    operation.overload(bg::graph("name"_n = "exact", [&, exact_result](bg::wp<bg::TS<bg::i64, G>>) {
                        ++selected[0];
                        return exact_result();
                    }));
                    break;
                case 1:
                    operation.overload(promoted);
                    break;
                case 2:
                    operation.overload(
                        bg::graph("name"_n = "equality", [&, equality_result](bg::wp<bg::TS<EQUALITY_SCALAR, G>>) {
                            ++selected[2];
                            return equality_result();
                        }));
                    break;
                case 3:
                    operation.overload(
                        bg::graph("name"_n = "scalar", [&, scalar_result](bg::wp<bg::TS<bg::SCALAR, G>>) {
                            ++selected[3];
                            return scalar_result();
                        }));
                    break;
                }
            };

            for (const auto index : order) {
                add_overload(index);
            }

            const auto graph = [&](bg::wp<bg::TS<bg::i64>> value) -> bg::wp<bg::TS<bg::i64>> {
                (void)operation("value"_n = value);
                return operation("value"_n = value).as<bg::TS<bg::i64>>();
            };
            const auto result = bg::testing::test_graph(graph, std::array<bg::i64, 1>{1});

            CHECK(result.equals({expected_result}));
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
    auto operation = bg::operator_<auto(bg::TS_T)->bg::TS_T>{"guarantee", {"value"}};
    operation.overload(bg::graph("name"_n = "ordinary", [&](bg::wp<bg::TS<bg::i64>> value) {
        selected = "ordinary";
        return value;
    }));
    operation.overload(bg::graph("name"_n = "final", [&](bg::wp<bg::TS<bg::i64, bg::TimeSeriesGuarantee::final>> value) {
        selected = "final";
        return value;
    }));

    const auto ordinary_source = bg::node([](bg::Out<bg::TS<bg::i64>>) {});
    const auto final_source = bg::node([](bg::Out<bg::TS<bg::i64, bg::TimeSeriesGuarantee::final>>) {});

    bg::testing::test_wiring([&] {
        operation(final_source());
        CHECK(selected == "final");
    });
    bg::testing::test_wiring([&] {
        operation(ordinary_source());
        CHECK(selected == "ordinary");
    });
}

TEST_CASE("literal guarantee overloads outrank variables with the same minimum in either registration order",
          "[feature:operator][feature:wiring][declaration:generic][series:value]"
          "[guarantee:deduplicated]") {
    constexpr auto G = bg::guarantee_variable<"OVERLOAD_GUARANTEE", bg::TimeSeriesGuarantee::deduplicated>;
    const auto source = bg::node([](bg::Out<bg::TS<bg::i64, bg::TimeSeriesGuarantee::deduplicated>>) {});

    const auto check_registration_order = [&](bool variable_first) {
        auto selected = std::string{};
        auto operation = bg::operator_<auto(bg::TS_T)->bg::TS_T>{"minimum_guarantee_specificity", {"value"}};
        const auto literal_result = bg::node([](bg::Out<bg::TS<bg::i64>>) {});
        const auto variable_result = bg::node([](bg::Out<bg::TS<bg::i64>>) {});
        const auto literal = bg::graph("name"_n = "literal", [&](bg::wp<bg::TS<bg::i64,
                                                                            bg::TimeSeriesGuarantee::deduplicated>>) {
            selected = "literal";
            return literal_result();
        });
        const auto variable = bg::graph("name"_n = "variable", [&](bg::wp<bg::TS<bg::i64, G>>) {
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

        bg::testing::test_wiring([&] {
            CHECK(&operation(source()).type() == &bg::TS<bg::i64>::type());
            CHECK(selected == "literal");
        });
    };

    check_registration_order(true);
    check_registration_order(false);
}

TEST_CASE("registering an overload clears cached selections", "[feature:operator][feature:wiring]") {
    auto selected = std::string{};
    auto operation = bg::operator_<auto(bg::TS_T)->bg::TS_T>{"identity", {"value"}};
    operation.overload(bg::graph("name"_n = "generic", [&](bg::wp<bg::TS<bg::SCALAR>> value) {
        selected = "generic";
        return value;
    }));
    const auto source = bg::node([](bg::Out<bg::TS<float>>) {});

    bg::testing::test_wiring([&] { operation(source()); });
    CHECK(selected == "generic");

    operation.overload(bg::graph("name"_n = "float", [&](bg::wp<bg::TS<float>> value) {
        selected = "float";
        return value;
    }));

    bg::testing::test_wiring([&] { operation(source()); });
    CHECK(selected == "float");
}

TEST_CASE("operator resolution caches are shared by nested wiring and isolated between roots",
          "[feature:operator][feature:wiring][shape:nested]") {
    auto requirement_calls = 0;
    auto operation = bg::operator_<auto(bg::TS_T) -> bg::TS_T>{"identity", {"value"}};
    operation.overload(bg::graph(
        "name"_n = "identity_impl",
        bg::require = [&](const bg::WiringArguments &) {
            ++requirement_calls;
            return true;
        },
        [](bg::wp<bg::TS<bg::SCALAR>> value) { return value; }));
    const auto source = bg::node([](bg::Out<bg::TS<float>>) {});

    auto root = bg::GraphWiring{};
    {
        const auto context = bg::WiringContext{root};
        (void)operation(source());
    }
    {
        auto nested = bg::GraphWiring{root};
        const auto context = bg::WiringContext{nested};
        (void)operation(source());
    }
    CHECK(requirement_calls == 1);

    auto separate_root = bg::GraphWiring{};
    {
        const auto context = bg::WiringContext{separate_root};
        (void)operation(source());
    }
    CHECK(requirement_calls == 2);
}

TEST_CASE("overload registration invalidates resolution caches in every live root",
          "[feature:operator][feature:wiring]") {
    auto generic_requirement_calls = 0;
    auto float_requirement_calls = 0;
    auto selected_overload = std::string{};
    auto operation = bg::operator_<auto(bg::TS_T) -> bg::TS_T>{"identity", {"value"}};
    operation.overload(bg::graph(
        "name"_n = "generic",
        bg::require = [&](const bg::WiringArguments &) {
            ++generic_requirement_calls;
            return true;
        },
        [&](bg::wp<bg::TS<bg::SCALAR>> value) {
            selected_overload = "generic";
            return value;
        }));
    const auto source = bg::node([](bg::Out<bg::TS<float>>) {});
    auto first_root = bg::GraphWiring{};
    auto second_root = bg::GraphWiring{};
    const auto wire = [&](bg::GraphWiring &wiring) {
        const auto context = bg::WiringContext{wiring};
        const auto result = operation(source());
        CHECK(&result.type() == &bg::TS<float>::type());
    };

    wire(first_root);
    CHECK(selected_overload == "generic");
    wire(second_root);
    CHECK(selected_overload == "generic");
    CHECK(generic_requirement_calls == 2);

    operation.overload(bg::graph(
        "name"_n = "float",
        bg::require = [&](const bg::WiringArguments &) {
            ++float_requirement_calls;
            return true;
        },
        [&](bg::wp<bg::TS<float>> value) {
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
    using Operator = bg::Operator;

    alignas(Operator) auto storage = std::array<std::byte, sizeof(Operator)>{};
    auto generic_requirement_calls = 0;
    auto float_requirement_calls = 0;
    const auto source = bg::node([](bg::Out<bg::TS<float>>) {});
    auto wiring = bg::GraphWiring{};
    const auto context = bg::WiringContext{wiring};

    {
        auto *operation = std::construct_at(
            static_cast<Operator *>(static_cast<void *>(storage.data())),
            bg::Signature{bg::util::symbol{"identity"}, {{bg::util::symbol{"value"}, &bg::TS_T::type()}},
                          bg::TS_T::type()});
        const auto cleanup = bg::util::scope_exit{[&]() noexcept { std::destroy_at(operation); }};
        operation->overload(bg::graph(
            "name"_n = "generic",
            bg::require = [&](const bg::WiringArguments &) {
                ++generic_requirement_calls;
                return true;
            },
            [](bg::wp<bg::TS<bg::SCALAR>> value) { return value; }));
        (void)(*operation)(source());
        CHECK(generic_requirement_calls == 1);
    }

    {
        auto *operation = std::construct_at(
            static_cast<Operator *>(static_cast<void *>(storage.data())),
            bg::Signature{bg::util::symbol{"identity"}, {{bg::util::symbol{"value"}, &bg::TS_T::type()}},
                          bg::TS_T::type()});
        const auto cleanup = bg::util::scope_exit{[&]() noexcept { std::destroy_at(operation); }};
        operation->overload(bg::graph(
            "name"_n = "float",
            bg::require = [&](const bg::WiringArguments &) {
                ++float_requirement_calls;
                return true;
            },
            [](bg::wp<bg::TS<float>> value) { return value; }));
        (void)(*operation)(source());
        CHECK(float_requirement_calls == 1);
    }
}

TEST_CASE("operator cache references survive nested resolution adding cache entries",
          "[feature:operator][feature:wiring][shape:nested]") {
    auto nested_operations = std::vector<std::unique_ptr<bg::Operator>>{};
    for (auto index = 0; index < 64; ++index) {
        auto nested = std::make_unique<bg::Operator>(bg::Signature{
            bg::util::symbol{"nested"}, {{bg::util::symbol{"value"}, &bg::TS_T::type()}}, bg::TS_T::type()});
        nested->overload(bg::graph("name"_n = "identity_impl",
                                   [](bg::wp<bg::TS<bg::SCALAR>> value) { return value; }));
        nested_operations.push_back(std::move(nested));
    }

    auto requirement_calls = 0;
    auto operation = bg::operator_<auto(bg::TS_T) -> bg::TS_T>{"outer", {"value"}};
    auto input = bg::WiringPoint{};
    operation.overload(bg::graph(
        "name"_n = "outer_impl",
        bg::require = [&](const bg::WiringArguments &) {
            ++requirement_calls;
            for (const auto &nested : nested_operations) {
                (void)(*nested)(input);
            }
            return true;
        },
        [](bg::wp<bg::TS<bg::SCALAR>> value) { return value; }));
    const auto source = bg::node([](bg::Out<bg::TS<float>>) {});
    auto wiring = bg::GraphWiring{};
    const auto context = bg::WiringContext{wiring};
    input = source();

    (void)operation(input);
    (void)operation(input);
    CHECK(requirement_calls == 1);
}

TEST_CASE("an overload can have a different parameter count from its prototype",
          "[feature:operator][feature:wiring][definition:function]") {
    auto selected = false;
    auto operation = bg::operator_<auto(bg::TS_T)->bg::TS_T>{"choose", {"value"}};
    operation.overload(bg::graph("name"_n = "choose_pair", [&](bg::wp<bg::TS<float>> value, bg::wp<bg::TS<float>>) {
        selected = true;
        return value;
    }));
    const auto source = bg::node([](bg::Out<bg::TS<float>>) {});

    bg::testing::test_wiring([&] { (void)operation(source(), source()); });

    CHECK(selected);
}

TEST_CASE("operator prototype names provide keyword aliases for overload parameters",
          "[feature:operator][feature:wiring]") {
    auto selected = std::string{};
    auto operation = bg::operator_<auto(bg::TS_T, bg::TS_T)->bg::TS_T>{"choose", {"value", "fallback"}};
    operation.overload(
        bg::graph("name"_n = "implementation", [&](bg::wp<bg::TS<float>> first, bg::wp<bg::TS<float>>) {
            selected = "first";
            return first;
        }));
    const auto source = bg::node([](bg::Out<bg::TS<float>>) {});

    bg::testing::test_wiring([&] { (void)operation("fallback"_n = source(), "value"_n = source()); });

    CHECK(selected == "first");
}

TEST_CASE("an operator reports equally specific overloads as ambiguous", "[feature:operator][feature:wiring]") {
    auto operation = bg::operator_<auto(bg::TS<float>)->bg::TS<float>>{"identity", {"value"}};
    operation.overload(bg::graph("name"_n = "first", [](bg::wp<bg::TS<float>> value) { return value; }));
    operation.overload(bg::graph("name"_n = "second", [](bg::wp<bg::TS<float>> value) { return value; }));

    const auto graph = [&](bg::wp<bg::TS<float>> value) -> bg::wp<bg::TS<float>> {
        return operation(value).as<bg::TS<float>>();
    };

    try {
        (void)bg::testing::test_graph(graph, {1.0F});
        FAIL("expected ambiguous operator wiring");
    } catch (const bg::WiringError &error) {
        REQUIRE(error.diagnostic());
        CHECK(error.diagnostic()->ambiguous);
        CHECK(error.diagnostic()->candidates.size() == 2);
        CHECK(std::string{error.what()}.contains("Ambiguous operator identity"));
    }
}

TEST_CASE("opt selects optional inputs and keeps that choice distinct in the cache",
          "[feature:operator][feature:wiring][feature:validity][validity:optional][series:value]") {
    auto selected = std::string{};
    auto operation = bg::operator_<auto(bg::TS_T)->bg::TS_T>{"optional", {"value"}};
    operation.overload(bg::graph("name"_n = "required", [&](bg::wp<bg::TS<bg::i64>> value) {
        selected = "required";
        return value;
    }));
    operation.overload(bg::graph("name"_n = "optional", [&](bg::wp<bg::TS<bg::i64>, bg::optional> value) {
        selected = "optional";
        return value;
    }));

    const auto source = bg::node([](bg::Out<bg::TS<bg::i64>>) {});
    bg::testing::test_wiring([&] {
        const auto value = source();

        (void)operation(value);
        CHECK(selected == "required");

        (void)operation(bg::opt(value));
        CHECK(selected == "optional");

        (void)operation(value);
        CHECK(selected == "required");
    });
}

TEST_CASE("scalar-only overloads prefer presence and keep absent calls distinct in the cache",
          "[feature:operator][feature:wiring][scalar:integer][definition:function]") {
    const auto required = bg::graph("name"_n = "requires_presence", [](bg::i64 value) {
        return bg::operators::const_(value + 10).as<bg::TS<bg::i64, bg::TimeSeriesGuarantee::const_>>();
    });
    const auto optional = bg::graph("name"_n = "accepts_absence", [](std::optional<bg::i64> value) {
        return bg::operators::const_(value.value_or(0) + 20).as<bg::TS<bg::i64, bg::TimeSeriesGuarantee::const_>>();
    });

    auto required_first = bg::operator_<auto(bg::i64) -> bg::TS<bg::i64>>{"presence", {"value"}};
    required_first.overload(required);
    required_first.overload(optional);

    auto optional_first = bg::operator_<auto(bg::i64) -> bg::TS<bg::i64>>{"presence", {"value"}};
    optional_first.overload(optional);
    optional_first.overload(required);

    const auto required_first_graph = [&] { return required_first(bg::i64{1}).as<bg::TS<bg::i64>>(); };
    const auto required_first_absent = [&] {
        return required_first(std::optional<bg::i64>{}).as<bg::TS<bg::i64>>();
    };
    const auto required_first_omitted = [&] { return required_first().as<bg::TS<bg::i64>>(); };
    const auto optional_first_graph = [&] { return optional_first(bg::i64{1}).as<bg::TS<bg::i64>>(); };
    const auto optional_first_absent = [&] {
        return optional_first(std::optional<bg::i64>{}).as<bg::TS<bg::i64>>();
    };
    const auto optional_first_omitted = [&] { return optional_first().as<bg::TS<bg::i64>>(); };

    CHECK(bg::testing::test_graph(required_first_graph).equals({11}));
    CHECK(bg::testing::test_graph(required_first_absent).equals({20}));
    CHECK(bg::testing::test_graph(required_first_omitted).equals({20}));
    CHECK(bg::testing::test_graph(required_first_omitted).equals({20}));
    CHECK(bg::testing::test_graph(required_first_graph).equals({11}));
    CHECK(bg::testing::test_graph(optional_first_graph).equals({11}));
    CHECK(bg::testing::test_graph(optional_first_absent).equals({20}));
    CHECK(bg::testing::test_graph(optional_first_omitted).equals({20}));
}

TEST_CASE("a graph can omit an optional scalar parameter",
          "[feature:wiring][scalar:integer][definition:function]") {
    const auto optional_graph = bg::graph("name"_n = "optional_scalar",
                                          [](std::optional<bg::i64> value) {
                                              return bg::operators::const_(value.value_or(7))
                                                  .as<bg::TS<bg::i64, bg::TimeSeriesGuarantee::const_>>();
                                          });
    const auto omitted_graph = [&] { return optional_graph().as<bg::TS<bg::i64>>(); };

    CHECK(bg::testing::test_graph(omitted_graph).equals({7}));
}

TEST_CASE("promoted node overloads prefer presence for scalar arguments",
          "[feature:operator][feature:wiring][scalar:integer][definition:function]") {
    const auto required = bg::node([](int value) { return value + 10; });
    const auto optional = bg::node([](std::optional<int> value) { return value.value_or(0) + 20; });
    auto operation = bg::operator_<auto(bg::i64) -> bg::TS<bg::i64>>{"promoted_presence", {"value"}};
    operation.overload(required);
    operation.overload(optional);

    const auto required_graph = [&] { return operation(bg::i64{1}).as<bg::TS<bg::i64>>(); };
    const auto absent_graph = [&] { return operation(std::optional<bg::i64>{}).as<bg::TS<bg::i64>>(); };
    const auto omitted_graph = [&] { return operation().as<bg::TS<bg::i64>>(); };

    CHECK(bg::testing::test_graph(required_graph).equals({11}));
    CHECK(bg::testing::test_graph(absent_graph).equals({20}));
    CHECK(bg::testing::test_graph(omitted_graph).equals({20}));
    CHECK(bg::testing::test_graph(omitted_graph).equals({20}));
    CHECK(bg::testing::test_graph(required_graph).equals({11}));
}

TEST_CASE("named omission and scalar presence remain distinct in one root wiring",
          "[feature:operator][feature:wiring][scalar:integer][definition:function]") {
    auto operation = bg::operator_<auto(bg::i64, bg::i64) -> bg::TS<bg::i64>>{
        "named_omission", {"first", "second"}};
    operation.overload(bg::graph("name"_n = "required_first", [](bg::i64 first, bg::i64 second) {
        return bg::operators::const_(100 + first + second)
            .as<bg::TS<bg::i64, bg::TimeSeriesGuarantee::const_>>();
    }));
    operation.overload(bg::graph("name"_n = "optional_first", [](std::optional<bg::i64> first,
                                                                    bg::i64 second) {
        return bg::operators::const_(first.value_or(0) + second)
            .as<bg::TS<bg::i64, bg::TimeSeriesGuarantee::const_>>();
    }));

    const auto add = bg::node([](bg::i64 lhs, bg::i64 rhs) { return lhs + rhs; });
    const auto graph = [&] {
        const auto omitted = operation("second"_n = bg::i64{5});
        const auto omitted_again = operation("second"_n = bg::i64{5});
        const auto present = operation(bg::i64{2}, "second"_n = bg::i64{5});
        const auto present_again = operation(bg::i64{2}, "second"_n = bg::i64{5});
        return add(add(omitted, present), add(omitted_again, present_again));
    };

    CHECK(bg::testing::test_graph(graph).equals({224}));
}

TEST_CASE("type specificity precedes scalar presence preference",
          "[feature:operator][feature:wiring][scalar:integer][definition:function]") {
    const auto promoted = bg::node([](int value) { return value + 100; });
    const auto explicit_scalar = bg::graph("name"_n = "optional_scalar", [](std::optional<bg::i64> value) {
        return bg::operators::const_(value.value_or(0) + 200).as<bg::TS<bg::i64, bg::TimeSeriesGuarantee::const_>>();
    });
    auto operation = bg::operator_<auto(bg::i64) -> bg::TS<bg::i64>>{"specificity", {"value"}};
    operation.overload(promoted);
    operation.overload(explicit_scalar);

    const auto graph = [&] { return operation(bg::i64{1}).as<bg::TS<bg::i64>>(); };
    CHECK(bg::testing::test_graph(graph).equals({201}));
}

TEST_CASE("conflicting scalar presence preferences remain ambiguous",
          "[feature:operator][feature:wiring][scalar:integer][definition:function]") {
    auto operation = bg::operator_<auto(bg::i64, bg::i64) -> bg::TS<bg::i64>>{"presence", {"lhs", "rhs"}};
    operation.overload(bg::graph("name"_n = "required_lhs", [](bg::i64 lhs, std::optional<bg::i64>) {
        return bg::operators::const_(lhs).as<bg::TS<bg::i64, bg::TimeSeriesGuarantee::const_>>();
    }));
    operation.overload(bg::graph("name"_n = "required_rhs", [](std::optional<bg::i64>, bg::i64 rhs) {
        return bg::operators::const_(rhs).as<bg::TS<bg::i64, bg::TimeSeriesGuarantee::const_>>();
    }));

    try {
        bg::testing::test_wiring([&] { (void)operation(bg::i64{1}, bg::i64{2}); });
        FAIL("expected conflicting scalar presence preferences to remain ambiguous");
    } catch (const bg::WiringError &error) {
        REQUIRE(error.diagnostic());
        CHECK(error.diagnostic()->ambiguous);
        CHECK(error.diagnostic()->candidates.size() == 2);
    }
}

TEST_CASE("scalar presence and time-series validity preferences share one partial order",
          "[feature:operator][feature:wiring][scalar:integer][validity:optional][series:value]"
          "[definition:function]") {
    auto selected = std::string{};
    auto operation = bg::operator_<auto(bg::i64, bg::TS<bg::i64>) -> bg::TS<bg::i64>>{
        "presence_and_validity", {"value", "trigger"}};
    operation.overload(bg::graph("name"_n = "required_scalar", [&](bg::i64 value,
                                                                      bg::wp<bg::TS<bg::i64>, bg::optional>) {
        selected = "required_scalar";
        return bg::operators::const_(value).as<bg::TS<bg::i64, bg::TimeSeriesGuarantee::const_>>();
    }));
    operation.overload(bg::graph("name"_n = "optional_scalar", [&](std::optional<bg::i64> value,
                                                                       bg::wp<bg::TS<bg::i64>>) {
        selected = "optional_scalar";
        return bg::operators::const_(value.value_or(0)).as<bg::TS<bg::i64, bg::TimeSeriesGuarantee::const_>>();
    }));

    const auto trigger = bg::node([](bg::Out<bg::TS<bg::i64>>) {});
    try {
        bg::testing::test_wiring([&] { (void)operation(bg::i64{1}, trigger()); });
        FAIL("expected scalar and time-series preferences to conflict");
    } catch (const bg::WiringError &error) {
        REQUIRE(error.diagnostic());
        CHECK(error.diagnostic()->ambiguous);
        CHECK(error.diagnostic()->candidates.size() == 2);
    }

    bg::testing::test_wiring([&] {
        const auto optional_trigger = bg::opt(trigger());
        const auto result = operation(bg::i64{1}, optional_trigger);
        CHECK(result.has_output());
        CHECK(result.type().accepts(bg::TS<bg::i64, bg::TimeSeriesGuarantee::const_>::type()));
        CHECK(selected == "required_scalar");
    });
}

TEST_CASE("a required scalar graph rejects absence before running its body",
          "[feature:operator][feature:wiring][scalar:integer][definition:function]") {
    auto body_calls = 0;
    const auto graph = bg::graph("name"_n = "required_scalar", [&](bg::i64 value) {
        ++body_calls;
        return bg::operators::const_(value).as<bg::TS<bg::i64, bg::TimeSeriesGuarantee::const_>>();
    });

    try {
        bg::testing::test_wiring([&] { (void)graph(std::optional<bg::i64>{}); });
        FAIL("expected an absent scalar to be rejected");
    } catch (const bg::TypeResolutionError &error) {
        CHECK(error.code() == bg::TypeResolutionErrorCode::mismatch);
        CHECK(std::string{error.what()}.contains("required scalar parameter has no value"));
    }
    CHECK_THROWS_AS(bg::testing::test_wiring([&] { (void)graph(); }), bg::WiringError);
    CHECK(body_calls == 0);
}

TEST_CASE("an operator rejects absent required scalars before overload wiring",
          "[feature:operator][feature:wiring][scalar:integer]") {
    auto overload_calls = 0;
    auto operation = bg::operator_<auto(bg::i64) -> bg::TS<bg::i64>>{"required", {"value"}};
    operation.overload(bg::graph("name"_n = "required", [&](bg::i64 value) {
        ++overload_calls;
        return bg::operators::const_(value).as<bg::TS<bg::i64, bg::TimeSeriesGuarantee::const_>>();
    }));

    try {
        bg::testing::test_wiring([&] { (void)operation(std::optional<bg::i64>{}); });
        FAIL("expected an absent scalar to be rejected");
    } catch (const bg::WiringError &error) {
        REQUIRE(error.diagnostic());
        REQUIRE(error.diagnostic()->candidates.size() == 1);
        CHECK(error.diagnostic()->candidates.front().stage == bg::WiringFailureStage::type_match);
        CHECK(error.diagnostic()->candidates.front().message.find("required scalar parameter has no value") !=
              std::string::npos);
    }
    CHECK_THROWS_AS(bg::testing::test_wiring([&] { (void)operation(); }), bg::WiringError);
    CHECK(overload_calls == 0);
}

TEST_CASE("all_valid is preferred over optional for an unmarked input",
          "[feature:operator][feature:wiring][feature:validity][validity:all-valid]") {
    auto operation = bg::operator_<auto(bg::TS_T)->bg::TS_T>{"validity", {"value"}};
    operation.overload(bg::node([](bg::In<bg::TS<bg::i64>, bg::all_valid> value, bg::Out<bg::TS<bg::i64>> output) {
        output.set(value.value());
    }));
    operation.overload(bg::node([](bg::In<bg::TS<bg::i64>, bg::optional>, bg::Out<bg::TS<bg::i64>> output) {
        output.set(0);
    }));

    const auto graph = [&](bg::wp<bg::TS<bg::i64>> value) -> bg::wp<bg::TS<bg::i64>> {
        return operation(value).as<bg::TS<bg::i64>>();
    };

    CHECK(bg::testing::test_graph(graph, std::array<bg::i64, 2>{1, 2}).equals({1, 2}));
}

TEST_CASE("required and all_valid overloads are equally specific",
          "[feature:operator][feature:wiring][feature:validity][validity:all-valid]") {
    auto operation = bg::operator_<auto(bg::TS_T)->bg::TS_T>{"validity", {"value"}};
    operation.overload(bg::node([](bg::In<bg::TS<bg::i64>>, bg::Out<bg::TS<bg::i64>> output) { output.set(1); }));
    operation.overload(
        bg::node([](bg::In<bg::TS<bg::i64>, bg::all_valid>, bg::Out<bg::TS<bg::i64>> output) { output.set(2); }));

    const auto graph = [&](bg::wp<bg::TS<bg::i64>> value) -> bg::wp<bg::TS<bg::i64>> {
        return operation(value).as<bg::TS<bg::i64>>();
    };

    try {
        (void)bg::testing::test_graph(graph, std::array<bg::i64, 1>{1});
        FAIL("expected required and all_valid overloads to be ambiguous");
    } catch (const bg::WiringError &error) {
        REQUIRE(error.diagnostic());
        CHECK(error.diagnostic()->ambiguous);
        CHECK(error.diagnostic()->candidates.size() == 2);
        CHECK(error.diagnostic()->candidates[1].signature.parameters.front().validity == bg::InputValidity::all_valid);
        CHECK(std::string{error.what()}.contains("all_valid "));
    }
}

TEST_CASE("opt-qualified arguments do not match all_valid inputs",
          "[feature:operator][feature:wiring][feature:validity][validity:all-valid][validity:optional]") {
    auto operation = bg::operator_<auto(bg::TS_T)->bg::TS_T>{"validity", {"value"}};
    operation.overload(
        bg::node([](bg::In<bg::TS<bg::i64>, bg::all_valid>, bg::Out<bg::TS<bg::i64>> output) { output.set(0); }));
    const auto graph = [&](bg::wp<bg::TS<bg::i64>> value) -> bg::wp<bg::TS<bg::i64>> {
        return operation(bg::opt(value)).as<bg::TS<bg::i64>>();
    };

    CHECK_THROWS_AS(bg::testing::test_graph(graph, std::array<bg::i64, 1>{1}), bg::WiringError);
}

TEST_CASE("type specificity precedes required-input preference",
          "[feature:operator][feature:wiring][feature:validity][validity:optional][series:value]") {
    auto selected = std::string{};
    auto operation = bg::operator_<auto(bg::TS_T)->bg::TS_T>{"specificity", {"value"}};
    operation.overload(bg::graph("name"_n = "required_generic", [&](bg::wp<bg::TS<bg::SCALAR>> value) {
        selected = "required_generic";
        return value;
    }));
    operation.overload(bg::graph("name"_n = "optional_exact", [&](bg::wp<bg::TS<bg::i64>, bg::optional> value) {
        selected = "optional_exact";
        return value;
    }));

    const auto source = bg::node([](bg::Out<bg::TS<bg::i64>>) {});
    bg::testing::test_wiring([&] { (void)operation(source()); });

    CHECK(selected == "optional_exact");
}

TEST_CASE("conflicting required-input preferences remain ambiguous",
          "[feature:operator][feature:wiring][feature:validity][validity:optional][series:value]") {
    auto operation = bg::operator_<auto(bg::TS_T, bg::TS_T)->bg::TS_T>{"validity", {"lhs", "rhs"}};
    operation.overload(bg::graph("name"_n = "required_lhs", [](bg::wp<bg::TS<bg::i64>> lhs,
                                                                  bg::wp<bg::TS<bg::i64>, bg::optional> rhs) {
        return lhs;
    }));
    operation.overload(bg::graph("name"_n = "required_rhs", [](bg::wp<bg::TS<bg::i64>, bg::optional> lhs,
                                                                  bg::wp<bg::TS<bg::i64>> rhs) {
        return rhs;
    }));

    const auto source = bg::node([](bg::Out<bg::TS<bg::i64>>) {});
    try {
        bg::testing::test_wiring([&] { (void)operation(source(), source()); });
        FAIL("expected ambiguous operator wiring");
    } catch (const bg::WiringError &error) {
        REQUIRE(error.diagnostic());
        CHECK(error.diagnostic()->ambiguous);
        CHECK(error.diagnostic()->candidates.size() == 2);
    }
}

TEST_CASE("an opt-qualified argument rejects a required-only overload",
          "[feature:operator][feature:wiring][feature:validity][validity:optional][series:value]") {
    auto operation = bg::operator_<auto(bg::TS_T)->bg::TS_T>{"required", {"value"}};
    operation.overload(bg::graph("name"_n = "required", [](bg::wp<bg::TS<bg::i64>> value) { return value; }));
    const auto source = bg::node([](bg::Out<bg::TS<bg::i64>>) {});

    try {
        bg::testing::test_wiring([&] { (void)operation(bg::opt(source())); });
        FAIL("expected an optional-input mismatch");
    } catch (const bg::WiringError &error) {
        REQUIRE(error.diagnostic());
        REQUIRE(error.diagnostic()->arguments.size() == 1);
        CHECK(error.diagnostic()->arguments.front().validity == bg::InputValidity::optional);
        REQUIRE(error.diagnostic()->candidates.size() == 1);
        CHECK(error.diagnostic()->candidates.front().signature.parameters.front().validity ==
              bg::InputValidity::required);
        CHECK(std::string{error.what()}.contains("opt TS<i64>"));
    }
}

TEST_CASE("an operator reports expected and supplied types for a mismatch", "[feature:operator][feature:wiring]") {
    auto operation = bg::operator_<auto(bg::TS<float>)->bg::TS<float>>{"identity", {"value"}};
    operation.overload(bg::graph("name"_n = "float_identity", [](bg::wp<bg::TS<float>> value) { return value; }));

    const auto graph = [&](bg::wp<bg::TS<double>> value) -> bg::wp<bg::TS<double>> {
        return operation(value).as<bg::TS<double>>();
    };

    try {
        (void)bg::testing::test_graph(graph, {1.0});
        FAIL("expected unmatched operator wiring");
    } catch (const bg::WiringError &error) {
        REQUIRE(error.diagnostic());
        REQUIRE(error.diagnostic()->candidates.size() == 1);
        const auto &failure = error.diagnostic()->candidates.front();
        CHECK(failure.stage == bg::WiringFailureStage::type_match);
        CHECK(failure.expected != nullptr);
        CHECK(failure.supplied != nullptr);
        CHECK(std::string{error.what()}.contains("received TS<double>"));
    }
}

TEST_CASE("operator diagnostics own signatures and retain prototype argument names",
          "[feature:operator][feature:wiring]") {
    const auto error = [] {
        auto operation = bg::operator_<auto(bg::TS<float>)->bg::TS<float>>{"identity", {"value"}};
        operation.overload(bg::graph("name"_n = "float_identity", [](bg::wp<bg::TS<float>> value) { return value; }));

        try {
            bg::testing::test_wiring([&] {
                const auto source = bg::node([](bg::Out<bg::TS<double>>) {});
                (void)operation(source());
            });
        } catch (const bg::WiringError &caught) {
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
    const auto source = bg::node([](bg::Out<bg::REF<bg::TS<float>>>) {});
    auto operation = bg::operator_<auto(bg::TS<float>)->bg::TS<float>>{"identity", {"value"}};
    operation.overload(bg::graph("name"_n = "direct", [](bg::wp<bg::TS<float>> value) { return value; }));

    bg::testing::test_wiring([&] {
        const auto output = operation(source());
        CHECK(&output.type() == &bg::REF<bg::TS<float>, bg::TimeSeriesGuarantee::deduplicated>::type());
    });
}

TEST_CASE("an operator prefers the overload with the argument's reference shape",
          "[feature:operator][feature:wiring][feature:reference][binding:reference-to-reference]") {
    auto selected = std::string{};
    auto operation = bg::operator_<auto(bg::TS<float>)->bg::TS<float>>{"identity", {"value"}};
    operation.overload(bg::graph("name"_n = "direct", [&](bg::wp<bg::TS<float>> value) {
        selected = "direct";
        return value;
    }));
    operation.overload(bg::graph("name"_n = "reference", [&](bg::wp<bg::REF<bg::TS<float>>> value) {
        selected = "reference";
        return value;
    }));
    const auto direct_source = bg::node([](bg::Out<bg::TS<float>>) {});
    const auto reference_source = bg::node([](bg::Out<bg::REF<bg::TS<float>>>) {});

    bg::testing::test_wiring([&] {
        operation(direct_source());
        CHECK(selected == "direct");
    });

    bg::testing::test_wiring([&] {
        operation(reference_source());
        CHECK(selected == "reference");
    });
}

TEST_CASE("an operator prefers the overload with a contained reference shape",
          "[feature:operator][feature:wiring][feature:reference][series:tsd]") {
    auto selected = std::string{};
    auto operation = bg::operator_<auto(bg::TS_T)->bg::TS_T>{"identity", {"value"}};
    operation.overload(bg::graph("name"_n = "value", [&](bg::wp<bg::TSD<bg::i64, bg::TS<float>>> value) {
        selected = "value";
        return value;
    }));
    operation.overload(
        bg::graph("name"_n = "reference", [&](bg::wp<bg::TSD<bg::i64, bg::REF<bg::TS<float>>>> value) {
            selected = "reference";
            return value;
        }));
    const auto value_source = bg::node([](bg::Out<bg::TSD<bg::i64, bg::TS<float>>>) {});
    const auto reference_source = bg::node([](bg::Out<bg::TSD<bg::i64, bg::REF<bg::TS<float>>>>) {});

    bg::testing::test_wiring([&] {
        operation(value_source());
        CHECK(selected == "value");
    });

    bg::testing::test_wiring([&] {
        operation(reference_source());
        CHECK(selected == "reference");
    });
}

TEST_CASE("an operator prefers a generic overload with the argument's contained reference shape",
          "[feature:operator][feature:wiring][feature:reference][series:fixed-tsl]"
          "[declaration:generic]") {
    auto selected = std::string{};
    auto operation = bg::operator_<auto(bg::TS_T)->bg::TS_T>{"identity", {"value"}};
    operation.overload(bg::graph("name"_n = "value", [&](bg::wp<bg::TSL<bg::TS_T, bg::SIZE>> value) {
        selected = "value";
        return value;
    }));
    operation.overload(bg::graph("name"_n = "reference", [&](bg::wp<bg::TSL<bg::REF<bg::TS_T>, bg::SIZE>> value) {
        selected = "reference";
        return value;
    }));
    const auto value_source = bg::node([](bg::Out<bg::TSL<bg::TS<float>, 2>>) {});
    const auto reference_source = bg::node([](bg::Out<bg::TSL<bg::REF<bg::TS<float>>, 2>>) {});

    bg::testing::test_wiring([&] {
        operation(value_source());
        CHECK(selected == "value");
    });

    bg::testing::test_wiring([&] {
        operation(reference_source());
        CHECK(selected == "reference");
    });
}

TEST_CASE("conflicting contained reference preferences are ambiguous",
          "[feature:operator][feature:wiring][feature:reference][series:fixed-tsl]"
          "[declaration:generic]") {
    auto operation = bg::operator_<auto(bg::TS_T, bg::TS_T)->bg::TS_T>{"choose", {"left", "right"}};
    operation.overload(bg::graph("name"_n = "values", [](bg::wp<bg::TSL<bg::TS_T, bg::SIZE>> left,
                                                         bg::wp<bg::TSL<bg::TS_T, bg::SIZE>>) { return left; }));
    operation.overload(
        bg::graph("name"_n = "references", [](bg::wp<bg::TSL<bg::REF<bg::TS_T>, bg::SIZE>> left,
                                              bg::wp<bg::TSL<bg::REF<bg::TS_T>, bg::SIZE>>) { return left; }));
    const auto value_source = bg::node([](bg::Out<bg::TSL<bg::TS<float>, 2>>) {});
    const auto reference_source = bg::node([](bg::Out<bg::TSL<bg::REF<bg::TS<float>>, 2>>) {});

    CHECK_THROWS_AS(bg::testing::test_wiring([&] { operation(reference_source(), value_source()); }), bg::WiringError);
}

TEST_CASE("an output operator returns an empty wiring point for a sink overload",
          "[feature:operator][feature:wiring]") {
    auto operation = bg::operator_<auto(bg::TS<float>)->bg::TS<float>>{"identity", {"value"}};
    const auto sink = bg::graph("name"_n = "sink", [](bg::wp<bg::TS<float>>) {});
    const auto source = bg::node([](bg::Out<bg::TS<float>>) {});
    operation.overload(sink);

    bg::testing::test_wiring([&] { CHECK_FALSE(operation(source()).has_output()); });
}

TEST_CASE("an outputless operator rejects a value overload", "[feature:operator][feature:wiring]") {
    auto operation = bg::operator_<void(bg::TS<float>)>{"consume", {"value"}};
    const auto identity = bg::graph("name"_n = "identity", [](bg::wp<bg::TS<float>> value) { return value; });

    CHECK_THROWS_AS(operation.overload(identity), std::invalid_argument);
}

TEST_CASE("an outputless operator invokes an outputless overload",
          "[feature:operator][feature:wiring][definition:function]") {
    auto called = false;
    auto operation = bg::operator_<void(int)>{"consume", {"value"}};
    operation.overload(bg::graph("name"_n = "sink", [&](int) { called = true; }));

    STATIC_CHECK(std::same_as<decltype(operation(1)), void>);

    bg::testing::test_wiring([&] { operation(1); });

    CHECK(called);
}

TEST_CASE("a function type declares a public output operator",
          "[feature:operator][feature:type-system][definition:function]") {
    const auto operation = bg::operator_<auto(bg::TS_T, bg::TS_T)->bg::TS_T>{"add", {"lhs", "rhs"}};

    CHECK(operation.signature().name().text() == "add");
    REQUIRE(operation.signature().parameter_count() == 2);
    CHECK(operation.signature().parameter(0).name.text() == "lhs");
    CHECK(operation.signature().parameter(1).name.text() == "rhs");
    CHECK(operation.signature().has_output());
}

TEST_CASE("a function type declares a public outputless operator",
          "[feature:operator][feature:type-system][definition:function]") {
    const auto operation = bg::operator_<void(int)>{"consume", {"value"}};

    CHECK(operation.signature().name().text() == "consume");
    REQUIRE(operation.signature().parameter_count() == 1);
    CHECK(operation.signature().parameter(0).name.text() == "value");
    CHECK(operation.signature().parameter(0).type == &bg::int64_type());
    CHECK_FALSE(operation.signature().has_output());
}

TEST_CASE("an operator prototype names every parameter", "[feature:operator][feature:type-system]") {
    CHECK_THROWS_AS(bg::operator_<void(int)>{"consume"}, std::invalid_argument);
}

} // namespace
