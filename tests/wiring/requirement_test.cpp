#include <bg/graph.hpp>
#include <bg/graph/wiring_arguments.hpp>
#include <bg/graph/wiring_error.hpp>
#include <bg/node.hpp>
#include <bg/operator.hpp>
#include <bg/scalar/opaque.hpp>
#include <bg/scalar/value.hpp>
#include <bg/testing/test_graph.hpp>
#include <bg/testing/test_wiring.hpp>
#include <bg/time_series/tsb/tsb.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <optional>
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
        CHECK(arguments.argument_index("input") == 0);
        CHECK(arguments.argument_name(0).text() == "input");
        CHECK(arguments.present("input"));
        CHECK(&arguments.original_type("input") == &bg::TS<float>::type());
        CHECK(&arguments.resolved_type("input") == &bg::TS<float>::type());
        CHECK_FALSE(arguments.scalar("input"));

        return bg::reject("the test overload is disabled");
    }

    void evaluate() const { output.set(*input); }
};

struct flat_positional_requirement_definition {
    std::optional<int> fixed;
    bg::In<bg::TSB<bg::SCHEMA>, bg::var_args> values;

    inline static auto calls = 0;

    static bg::RequirementResult require(const bg::WiringArguments &arguments) {
        ++calls;
        REQUIRE(arguments.size() == 2);
        CHECK(arguments.argument_name(0).text() == "fixed");
        CHECK(arguments.argument_name(1).text() == "1");
        CHECK_FALSE(arguments.present(0));
        CHECK(arguments.present(1));
        CHECK(bg::scalar_cast<std::int64_t>(*arguments.scalar(1)).value() == 7);

        return bg::accept;
    }

    void evaluate() const {}
};

struct named_capture_collision_definition {
    int mode;
    bg::In<bg::TSB<bg::SCHEMA>, bg::var_kwargs> values;

    void evaluate() const {}
};

TEST_CASE("an aggregate requirement can explain why it rejected an overload",
          "[feature:operator][feature:wiring][definition:aggregate]") {
    auto operation = bg::operator_<auto(bg::TS<float>) -> bg::TS<float>>{"identity", {"value"}};
    operation.overload(bg::node<rejecting_copy_definition>());

    const auto graph = [&](bg::wp<bg::TS<float>> value) -> bg::wp<bg::TS<float>> {
        return operation(value).as<bg::TS<float>>();
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

TEST_CASE("requirements see one flat positional argument sequence",
          "[feature:operator][feature:wiring][feature:variadic-wiring][definition:aggregate]") {
    flat_positional_requirement_definition::calls = 0;

    auto operation = bg::operator_<void(int, bg::TS_T)>{"consume", {"fixed", "values"}};
    operation.overload(bg::node<flat_positional_requirement_definition>());
    const auto sender = bg::graph(
        "name"_n = "sender", [&](std::optional<int> fixed, bg::wp<bg::TSB<bg::SCHEMA>, bg::var_args> values) {
            operation(fixed, values);
        });

    bg::testing::test_wiring([&] { operation(std::optional<int>{}, std::optional<int>{}, 7); });
    bg::testing::test_wiring([&] { sender(std::optional<int>{}, std::optional<int>{}, 7); });

    CHECK(flat_positional_requirement_definition::calls == 2);
}

TEST_CASE("requirements find direct and forwarded named captures",
          "[feature:operator][feature:wiring][feature:variadic-wiring][definition:function]") {
    auto calls = 0;
    auto operation = bg::operator_<void(bg::TS_T)>{"consume", {"values"}};
    operation.overload(bg::node(
        bg::eval = [](bg::In<bg::TSB<bg::SCHEMA>, bg::var_kwargs>) {},
        bg::require = [&](const bg::WiringArguments &arguments) {
            ++calls;
            REQUIRE(arguments.size() == 2);
            CHECK(arguments.argument_index("first") == 0);
            CHECK(arguments.argument_index("second") == 1);
            CHECK(arguments.argument_name(0).text() == "first");
            CHECK(&arguments.original_type("first") == &bg::cpp_scalar_type<std::int64_t>());
            CHECK(&arguments.resolved_type("first") == &bg::cpp_scalar_type<std::int64_t>());
            CHECK(bg::scalar_cast<std::int64_t>(*arguments.scalar("second")).value() == 2);

            return bg::accept;
        }));
    const auto sender = bg::graph(
        "name"_n = "sender",
        [&](bg::wp<bg::TSB<bg::SCHEMA>, bg::var_kwargs> values) { operation(values); });

    bg::testing::test_wiring([&] { operation("first"_n = 1, "second"_n = 2); });
    bg::testing::test_wiring([&] { sender("first"_n = 1, "second"_n = 2); });

    CHECK(calls == 2);
}

TEST_CASE("a named capture cannot reuse a fixed parameter name",
          "[feature:operator][feature:wiring][feature:variadic-wiring][definition:aggregate]") {
    auto operation = bg::operator_<void(int, bg::TS_T)>{"consume", {"mode", "values"}};
    operation.overload(bg::node<named_capture_collision_definition>());

    bg::testing::test_wiring([&] {
        CHECK_THROWS_AS(operation(1, "mode"_n = std::optional<int>{}), bg::WiringError);
    });
}

struct zero_mode_copy_definition {
    bg::In<bg::TS<float>> input;
    int mode;
    bg::Out<bg::TS<float>> output;

    inline static auto requirement_calls = 0;

    static bg::RequirementResult require(const bg::WiringArguments &arguments) {
        ++requirement_calls;
        return bg::scalar_cast<std::int64_t>(*arguments.scalar("mode")).value() == 0 ? bg::accept : bg::reject();
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
        return bg::scalar_cast<std::int64_t>(*arguments.scalar("mode")).value() != 0 ? bg::accept : bg::reject();
    }

    void evaluate() const { output.set(-*input); }
};

TEST_CASE("an operator cache keys scalar values read by aggregate requirements",
          "[feature:operator][feature:wiring][definition:aggregate]") {
    zero_mode_copy_definition::requirement_calls = 0;
    nonzero_mode_copy_definition::requirement_calls = 0;

    auto operation = bg::operator_<auto(bg::TS<float>, int) -> bg::TS<float>>{"mode", {"value", "mode"}};
    operation.overload(bg::node<zero_mode_copy_definition>());
    operation.overload(bg::node<nonzero_mode_copy_definition>());

    const auto run = [&](int mode) {
        const auto graph = [&](bg::wp<bg::TS<float>> input) -> bg::wp<bg::TS<float>> {
            return operation(input, mode).as<bg::TS<float>>();
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
    auto operation = bg::operator_<auto(bg::TS<float>, int) -> bg::TS<float>>{"mode", {"value", "mode"}};

    operation.overload(bg::node(
        bg::eval = [](bg::In<bg::TS<float>> input, int, bg::Out<bg::TS<float>> output) { output.set(*input); },
        bg::require =
            [&](const bg::WiringArguments &arguments) {
                ++zero_requirement_calls;
                return bg::scalar_cast<std::int64_t>(*arguments.scalar(1)).value() == 0;
            }));
    operation.overload(bg::node(
        bg::eval = [](bg::In<bg::TS<float>> input, int, bg::Out<bg::TS<float>> output) { output.set(-*input); },
        bg::require =
            [&](const bg::WiringArguments &arguments) {
                ++nonzero_requirement_calls;
                return bg::scalar_cast<std::int64_t>(*arguments.scalar(1)).value() != 0
                           ? bg::accept
                           : bg::reject("mode must be nonzero");
            }));

    const auto run = [&](int mode) {
        const auto graph = [&](bg::wp<bg::TS<float>> input) -> bg::wp<bg::TS<float>> {
            return operation(input, mode).as<bg::TS<float>>();
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
    auto operation = bg::operator_<void(Argument)>{"consume", {"value"}};
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
    auto operation = bg::operator_<auto(bg::TS<float>, int, int) -> bg::TS<float>>{
        "mode", {"value", "ignored", "mode"}};
    operation.overload(bg::node(
        bg::eval = [](bg::In<bg::TS<float>>, int, int, bg::Out<bg::TS<float>>) {},
        bg::require =
            [&](const bg::WiringArguments &arguments) {
                ++requirement_calls;
                return bg::scalar_cast<std::int64_t>(*arguments.scalar(2)).value() == 0;
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
    auto operation = bg::operator_<auto(bg::TS<float>) -> bg::TS<float>>{"identity", {"value"}};
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
