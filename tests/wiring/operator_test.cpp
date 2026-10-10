#include <bg/graph.hpp>
#include <bg/graph/wiring_error.hpp>
#include <bg/node.hpp>
#include <bg/operator.hpp>
#include <bg/operators.hpp>
#include <bg/scalar/type.hpp>
#include <bg/testing/test_graph.hpp>
#include <bg/testing/test_wiring.hpp>
#include <bg/time_series/time_series_type.hpp>
#include <bg/time_series/tsd/tsd_type.hpp>
#include <bg/time_series/tsl/tsl_type.hpp>
#include <bg/type/time_series_reference_type.hpp>
#include <bg/type/time_series_value_type.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <stdexcept>
#include <string>

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
    operation.overload(bg::graph("name"_n = "value", [&](bg::wp<bg::TSD<std::int64_t, bg::TS<float>>> value) {
        selected = "value";
        return value;
    }));
    operation.overload(
        bg::graph("name"_n = "reference", [&](bg::wp<bg::TSD<std::int64_t, bg::REF<bg::TS<float>>>> value) {
            selected = "reference";
            return value;
        }));
    const auto value_source = bg::node([](bg::Out<bg::TSD<std::int64_t, bg::TS<float>>>) {});
    const auto reference_source = bg::node([](bg::Out<bg::TSD<std::int64_t, bg::REF<bg::TS<float>>>>) {});

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
