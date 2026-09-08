#include <bg/graph.hpp>
#include <bg/graph/wiring_arguments.hpp>
#include <bg/graph/wiring_error.hpp>
#include <bg/node.hpp>
#include <bg/operator.hpp>
#include <bg/scalar/opaque.hpp>
#include <bg/scalar/value.hpp>
#include <bg/testing/test_graph.hpp>
#include <bg/testing/test_wiring.hpp>

#include <catch2/catch_test_macros.hpp>

#include <stdexcept>
#include <string>

using namespace bg::literals;

namespace {

struct unhashable_wiring_value {
    int value;
};

struct rejecting_copy_definition {
    bg::In<bg::TS<float>> input;
    bg::Out<bg::TS<float>> output;

    static bg::RequirementResult require(const bg::WiringArguments &arguments) {
        CHECK(arguments.parameter_index("input") == 0);
        CHECK(arguments.parameter_name(0).text() == "input");
        CHECK(arguments.present("input"));
        CHECK(&arguments.original_type("input") == &bg::TS<float>::type());
        CHECK(&arguments.resolved_type("input") == &bg::TS<float>::type());
        CHECK_FALSE(arguments.scalar("input"));

        return bg::reject("the test overload is disabled");
    }

    void evaluate() const { output.set(*input); }
};

TEST_CASE("an aggregate requirement can explain why it rejected an overload",
          "[feature:operator][feature:wiring][definition:aggregate]") {
    auto operation = bg::operator_("identity", [](bg::In<bg::TS<float>, "value"_n>, bg::Out<bg::TS<float>>) {});
    operation.overload(bg::node<rejecting_copy_definition>());

    const auto graph = [&](bg::wp<bg::TS<float>> value) -> bg::wp<bg::TS<float>> {
        return bg::wp<bg::TS<float>>{*operation(value)};
    };

    try {
        (void)bg::testing::test_graph(graph, {1.0F});
        FAIL("expected rejected operator wiring");
    } catch (const bg::WiringError &error) {
        REQUIRE(error.diagnostic());
        REQUIRE(error.diagnostic()->candidates.size() == 1);
        CHECK(error.diagnostic()->candidates.front().stage == bg::WiringFailureStage::requirement);
        CHECK(std::string{error.what()}.contains("the test overload is disabled"));
    }
}

struct zero_mode_copy_definition {
    bg::In<bg::TS<float>> input;
    int mode;
    bg::Out<bg::TS<float>> output;

    inline static auto requirement_calls = 0;

    static bg::RequirementResult require(const bg::WiringArguments &arguments) {
        ++requirement_calls;
        return bg::scalar_cast<int>(*arguments.scalar("mode")).value() == 0 ? bg::accept : bg::reject();
    }

    void evaluate() const { output.set(*input); }
};

struct nonzero_mode_copy_definition {
    bg::In<bg::TS<float>> input;
    int mode;
    bg::Out<bg::TS<float>> output;

    inline static auto requirement_calls = 0;

    static bg::RequirementResult require(const bg::WiringArguments &arguments) {
        ++requirement_calls;
        return bg::scalar_cast<int>(*arguments.scalar("mode")).value() != 0 ? bg::accept : bg::reject();
    }

    void evaluate() const { output.set(-*input); }
};

TEST_CASE("an operator cache keys scalar values read by aggregate requirements",
          "[feature:operator][feature:wiring][definition:aggregate]") {
    zero_mode_copy_definition::requirement_calls = 0;
    nonzero_mode_copy_definition::requirement_calls = 0;

    auto operation = bg::operator_("mode", [](bg::In<bg::TS<float>, "value"_n>, int, bg::Out<bg::TS<float>>) {});
    operation.overload(bg::node<zero_mode_copy_definition>());
    operation.overload(bg::node<nonzero_mode_copy_definition>());

    const auto run = [&](int mode) {
        const auto graph = [&](bg::wp<bg::TS<float>> input) -> bg::wp<bg::TS<float>> {
            return bg::wp<bg::TS<float>>{*operation(input, mode)};
        };

        return bg::testing::test_graph(graph, {1.0F});
    };

    CHECK(run(0).equals({1.0F}));
    CHECK(run(0).equals({1.0F}));
    CHECK(zero_mode_copy_definition::requirement_calls == 1);
    CHECK(nonzero_mode_copy_definition::requirement_calls == 1);

    CHECK(run(1).equals({-1.0F}));
    CHECK(zero_mode_copy_definition::requirement_calls == 2);
    CHECK(nonzero_mode_copy_definition::requirement_calls == 2);
}

TEST_CASE("callable requirements control overload applicability",
          "[feature:operator][feature:wiring][definition:function]") {
    auto zero_requirement_calls = 0;
    auto nonzero_requirement_calls = 0;
    auto operation = bg::operator_("mode", [](bg::In<bg::TS<float>, "value"_n>, int, bg::Out<bg::TS<float>>) {});

    operation.overload(bg::node(
        bg::eval = [](bg::In<bg::TS<float>> input, int, bg::Out<bg::TS<float>> output) { output.set(*input); },
        bg::require =
            [&](const bg::WiringArguments &arguments) {
                ++zero_requirement_calls;
                return bg::scalar_cast<int>(*arguments.scalar(1)).value() == 0;
            }));
    operation.overload(bg::node(
        bg::eval = [](bg::In<bg::TS<float>> input, int, bg::Out<bg::TS<float>> output) { output.set(-*input); },
        bg::require =
            [&](const bg::WiringArguments &arguments) {
                ++nonzero_requirement_calls;
                return bg::scalar_cast<int>(*arguments.scalar(1)).value() != 0 ? bg::accept
                                                                               : bg::reject("mode must be nonzero");
            }));

    const auto run = [&](int mode) {
        const auto graph = [&](bg::wp<bg::TS<float>> input) -> bg::wp<bg::TS<float>> {
            return bg::wp<bg::TS<float>>{*operation(input, mode)};
        };

        return bg::testing::test_graph(graph, {2.0F});
    };

    CHECK(run(0).equals({2.0F}));
    CHECK(run(0).equals({2.0F}));
    CHECK(zero_requirement_calls == 1);
    CHECK(nonzero_requirement_calls == 1);

    CHECK(run(1).equals({-2.0F}));
    CHECK(zero_requirement_calls == 2);
    CHECK(nonzero_requirement_calls == 2);
}

TEST_CASE("an unhashable scalar read disables operator caching",
          "[feature:operator][feature:wiring][definition:function][scalar:opaque]") {
    using Argument = bg::Opaque<unhashable_wiring_value>;

    auto requirement_calls = 0;
    auto operation = bg::operator_("consume", [](bg::Scalar<Argument>) {});
    operation.overload(bg::node(
        bg::eval = [](bg::Scalar<Argument>) {}, bg::require =
                                                    [&](const bg::WiringArguments &arguments) {
                                                        ++requirement_calls;
                                                        return arguments.scalar(0).has_value();
                                                    }));
    const auto value = bg::ScalarValue<Argument>{unhashable_wiring_value{1}};

    bg::testing::test_wiring([&] {
        operation(value);
        operation(value);
    });

    CHECK(requirement_calls == 2);
}

TEST_CASE("an unread scalar does not participate in the operator cache key",
          "[feature:operator][feature:wiring][definition:function]") {
    auto requirement_calls = 0;
    auto operation = bg::operator_("mode", [](bg::In<bg::TS<float>, "value"_n>, int, int, bg::Out<bg::TS<float>>) {});
    operation.overload(bg::node(
        bg::eval = [](bg::In<bg::TS<float>>, int, int, bg::Out<bg::TS<float>>) {},
        bg::require =
            [&](const bg::WiringArguments &arguments) {
                ++requirement_calls;
                return bg::scalar_cast<int>(*arguments.scalar(2)).value() == 0;
            }));
    const auto source = bg::node([](bg::Out<bg::TS<float>>) {});

    bg::testing::test_wiring([&] {
        operation(source(), 1, 0);
        operation(source(), 2, 0);
    });

    CHECK(requirement_calls == 1);
}

TEST_CASE("a requirement exception reports its callback failure",
          "[feature:operator][feature:wiring][definition:function]") {
    auto operation = bg::operator_("identity", [](bg::In<bg::TS<float>>, bg::Out<bg::TS<float>>) {});
    operation.overload(bg::node(
        bg::eval = [](bg::In<bg::TS<float>>, bg::Out<bg::TS<float>>) {},
        bg::require = [](const bg::WiringArguments &) -> bool { throw std::runtime_error{"requirement failed"}; }));
    const auto source = bg::node([](bg::Out<bg::TS<float>>) {});

    try {
        bg::testing::test_wiring([&] { operation(source()); });
        FAIL("expected a requirement callback failure");
    } catch (const bg::WiringError &error) {
        REQUIRE(error.diagnostic());
        REQUIRE(error.diagnostic()->candidates.size() == 1);
        CHECK(error.diagnostic()->candidates.front().stage == bg::WiringFailureStage::callback);
        CHECK(std::string{error.what()}.contains("requirement failed"));
    }
}

} // namespace
