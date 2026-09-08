#include <bg/graph.hpp>
#include <bg/graph/wiring_error.hpp>
#include <bg/node.hpp>
#include <bg/operator.hpp>
#include <bg/scalar/type.hpp>
#include <bg/testing/test_graph.hpp>
#include <bg/testing/test_wiring.hpp>
#include <bg/time_series/time_series_type.hpp>
#include <bg/type/time_series_reference_type.hpp>
#include <bg/type/time_series_value_type.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <stdexcept>
#include <string>

using namespace bg::literals;

auto declared_add(bg::In<bg::TS_T, "lhs"_n>, bg::In<bg::TS_T, "rhs"_n>) -> bg::TS_T;
void declared_consume(int);

namespace {

TEST_CASE("an operator selects the least generic applicable overload",
          "[feature:operator][feature:wiring][declaration:generic]") {
    auto operation =
        bg::operator_("select", [](bg::In<bg::TS_T, "value"_n>, bg::In<bg::TS_T, "fallback"_n>, bg::Out<bg::TS_T>) {});
    operation.overload(bg::graph("name"_m = "generic", [](bg::wp<bg::TS<bg::SCALAR>>,
                                                          bg::wp<bg::TS<bg::SCALAR>> fallback) { return fallback; }));
    operation.overload(
        bg::graph("name"_m = "float_exact", [](bg::wp<bg::TS<float>> value, bg::wp<bg::TS<float>>) { return value; }));

    const auto float_graph = [&](bg::wp<bg::TS<float>> value, bg::wp<bg::TS<float>> fallback) -> bg::wp<bg::TS<float>> {
        return bg::wp<bg::TS<float>>{*operation(value, fallback)};
    };

    const auto double_graph = [&](bg::wp<bg::TS<double>> value,
                                  bg::wp<bg::TS<double>> fallback) -> bg::wp<bg::TS<double>> {
        return bg::wp<bg::TS<double>>{*operation(value, fallback)};
    };

    CHECK(bg::testing::test_graph(float_graph, std::array{1.0F, 2.0F}, std::array{10.0F, 20.0F}).equals({1.0F, 2.0F}));
    CHECK(bg::testing::test_graph(double_graph, std::array{1.0, 2.0}, std::array{10.0, 20.0}).equals({10.0, 20.0}));
}

TEST_CASE("registering an overload clears cached selections", "[feature:operator][feature:wiring]") {
    auto selected = std::string{};
    auto operation = bg::operator_("identity", [](bg::In<bg::TS_T, "value"_n>, bg::Out<bg::TS_T>) {});
    operation.overload(bg::graph("name"_m = "generic", [&](bg::wp<bg::TS<bg::SCALAR>> value) {
        selected = "generic";
        return value;
    }));
    const auto source = bg::node([](bg::Out<bg::TS<float>>) {});

    bg::testing::test_wiring([&] { operation(source()); });
    CHECK(selected == "generic");

    operation.overload(bg::graph("name"_m = "float", [&](bg::wp<bg::TS<float>> value) {
        selected = "float";
        return value;
    }));

    bg::testing::test_wiring([&] { operation(source()); });
    CHECK(selected == "float");
}

TEST_CASE("an overload can have a different parameter count from its prototype",
          "[feature:operator][feature:wiring][definition:function]") {
    auto selected = false;
    auto operation = bg::operator_("choose", [](bg::In<bg::TS_T, "value"_n>, bg::Out<bg::TS_T>) {});
    operation.overload(bg::graph("name"_m = "choose_pair", [&](bg::wp<bg::TS<float>> value, bg::wp<bg::TS<float>>) {
        selected = true;
        return value;
    }));
    const auto source = bg::node([](bg::Out<bg::TS<float>>) {});

    bg::testing::test_wiring([&] { (void)operation(source(), source()); });

    CHECK(selected);
}

TEST_CASE("an operator reports equally specific overloads as ambiguous", "[feature:operator][feature:wiring]") {
    auto operation = bg::operator_("identity", [](bg::In<bg::TS<float>, "value"_n>, bg::Out<bg::TS<float>>) {});
    operation.overload(bg::graph("name"_m = "first", [](bg::wp<bg::TS<float>> value) { return value; }));
    operation.overload(bg::graph("name"_m = "second", [](bg::wp<bg::TS<float>> value) { return value; }));

    const auto graph = [&](bg::wp<bg::TS<float>> value) -> bg::wp<bg::TS<float>> {
        return bg::wp<bg::TS<float>>{*operation(value)};
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
    auto operation = bg::operator_("identity", [](bg::In<bg::TS<float>, "value"_n>, bg::Out<bg::TS<float>>) {});
    operation.overload(bg::graph("name"_m = "float_identity", [](bg::wp<bg::TS<float>> value) { return value; }));

    const auto graph = [&](bg::wp<bg::TS<double>> value) -> bg::wp<bg::TS<double>> {
        return bg::wp<bg::TS<double>>{*operation(value)};
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
        auto operation = bg::operator_("identity", [](bg::In<bg::TS<float>, "value"_n>, bg::Out<bg::TS<float>>) {});
        operation.overload(bg::graph("name"_m = "float_identity", [](bg::wp<bg::TS<float>> value) { return value; }));

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
    auto operation = bg::operator_("identity", [](bg::In<bg::TS<float>, "value"_n>, bg::Out<bg::TS<float>>) {});
    operation.overload(bg::graph("name"_m = "direct", [](bg::wp<bg::TS<float>> value) { return value; }));

    bg::testing::test_wiring([&] {
        const auto output = *operation(source());
        CHECK(&output.type() == &bg::REF<bg::TS<float>>::type());
    });
}

TEST_CASE("an operator prefers the overload with the argument's reference shape",
          "[feature:operator][feature:wiring][feature:reference][binding:reference-to-reference]") {
    auto selected = std::string{};
    auto operation = bg::operator_("identity", [](bg::In<bg::TS<float>, "value"_n>, bg::Out<bg::TS<float>>) {});
    operation.overload(bg::graph("name"_m = "direct", [&](bg::wp<bg::TS<float>> value) {
        selected = "direct";
        return value;
    }));
    operation.overload(bg::graph("name"_m = "reference", [&](bg::wp<bg::REF<bg::TS<float>>> value) {
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

TEST_CASE("an operator rejects an overload with a different output shape", "[feature:operator][feature:wiring]") {
    auto operation = bg::operator_("identity", [](bg::In<bg::TS<float>, "value"_n>, bg::Out<bg::TS<float>>) {});
    const auto sink = bg::graph("name"_m = "sink", [](bg::wp<bg::TS<float>>) {});

    CHECK_THROWS_AS(operation.overload(sink), std::invalid_argument);
}

TEST_CASE("an outputless operator invokes an outputless overload",
          "[feature:operator][feature:wiring][definition:function]") {
    auto called = false;
    auto operation = bg::operator_("consume", [](int) {});
    operation.overload(bg::graph("name"_m = "sink", [&](int) { called = true; }));

    bg::testing::test_wiring([&] { operation(1); });

    CHECK(called);
}

struct generic_identity_operator {
    bg::In<bg::TS_T> value;
    bg::Out<bg::TS_T> output;
};

TEST_CASE("an aggregate definition supplies an operator prototype",
          "[feature:operator][feature:type-system][definition:aggregate]") {
    const auto operation = bg::operator_<generic_identity_operator>();
    const auto &value = bg::TS<int>::type();
    const auto resolved = operation.signature().resolve(std::array<const bg::Type *, 1>{&value});

    CHECK(operation.signature().name().text() == "generic_identity_operator");
    REQUIRE(resolved.parameters.size() == 1);
    CHECK(resolved.parameters.front().name.text() == "value");
    CHECK(resolved.parameters.front().type == &value);
    CHECK(resolved.output == &value);
}

TEST_CASE("a function declaration supplies an output operator prototype",
          "[feature:operator][feature:type-system][definition:function]") {
    const auto operation = bg::operator_<declared_add>();
    const auto &value = bg::TS<int>::type();
    const auto resolved = operation.signature().resolve(std::array<const bg::Type *, 2>{&value, &value});

    CHECK(operation.signature().name().text() == "declared_add");
    REQUIRE(resolved.parameters.size() == 2);
    CHECK(resolved.parameters[0].name.text() == "lhs");
    CHECK(resolved.parameters[0].type == &value);
    CHECK(resolved.parameters[1].name.text() == "rhs");
    CHECK(resolved.parameters[1].type == &value);
    CHECK(resolved.output == &value);
}

TEST_CASE("a void function declaration supplies an outputless operator prototype",
          "[feature:operator][feature:type-system][definition:function]") {
    const auto operation = bg::operator_<declared_consume>();

    CHECK(operation.signature().name().text() == "declared_consume");
    REQUIRE(operation.signature().parameter_count() == 1);
    CHECK(operation.signature().parameter(0).type == &bg::int_type());
    CHECK_FALSE(operation.signature().has_output());
}

} // namespace
