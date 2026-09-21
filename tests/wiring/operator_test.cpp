#include <bg/graph.hpp>
#include <bg/graph/wiring_error.hpp>
#include <bg/node.hpp>
#include <bg/operator.hpp>
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
        CHECK(&output.type() == &bg::REF<bg::TS<float>>::type());
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
