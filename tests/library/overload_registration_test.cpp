#include <bg/graph.hpp>
#include <bg/library.hpp>
#include <bg/run_graph.hpp>
#include <bg/testing/test_wiring.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <algorithm>
#include <memory>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

using namespace bg;
using namespace bg::literals;

namespace overload_registration_tests {

inline operator_<auto(i64) -> i64> callable_operator{"callable", {"value"}};
inline operator_<auto(i64) -> i64> aggregate_operator{"aggregate", {"value"}};
inline operator_<auto(i64) -> i64> node_template_operator{"node_template", {"value"}};
inline operator_<auto(i64) -> i64> graph_template_operator{"graph_template", {"value"}};
inline operator_<auto(i64) -> i64> reflected_graph_operator{"reflected_graph", {"value"}};
inline operator_<auto(i64) -> i64> reflected_callable_operator{"reflected_callable", {"value"}};
inline operator_<auto(TS_T) -> TS_T> sink_graph_operator{"sink_graph", {"value"}};
inline operator_<auto(SCALAR) -> SCALAR> family_operator{"family", {"value"}};
inline operator_<auto(SCALAR) -> SCALAR> reflected_family_operator{"reflected_family", {"value"}};
inline operator_<auto(TS_T) -> bool> scheduled_operator{"scheduled", {"value"}};
inline operator_<auto(TS_T) -> TS_T> resolved_callable_operator{"resolved_callable", {"value"}};
inline operator_<auto(i64) -> i64> registration_order_operator{"registration_order", {"value"}};

using RESULT = ScalarVariable<"REGISTRATION_RESULT">;
inline std::vector<int> registration_order;

struct pass_i64 {
    In<TS<i64>> value;
    Out<TS<i64>> output;

    void evaluate() const { output.copy_tick(value); }
};

template <typename T> struct pass_value {
    In<TS<T>> value;
    Out<TS<T>> output;

    void evaluate() const { output.copy_tick(value); }
};

template <typename Registration, typename... Arguments>
concept CanConstruct = requires(Arguments &&...arguments) {
    Registration{std::forward<Arguments>(arguments)...};
};

static_assert(CanConstruct<overload<aggregate_operator, pass_i64>>);
static_assert(!std::default_initializable<overload<aggregate_operator, i64>>);
static_assert(CanConstruct<overload<node_template_operator>, AggregateNodeTemplate<pass_i64> &>);
static_assert(!CanConstruct<overload<callable_operator>, int>);
static_assert(!std::is_empty_v<overload<callable_operator>>);
static_assert(!std::is_empty_v<instantiate_overloads<family_operator, pass_value, i64, double>>);

} // namespace overload_registration_tests

using namespace overload_registration_tests;

TEST_CASE("an overload forwards callable arguments to node and retains the callable",
          "[feature:operator][feature:wiring][definition:function]") {
    std::weak_ptr<const i64> captured_lifetime;
    {
        const auto captured = std::make_shared<const i64>(6);
        captured_lifetime = captured;
        overload<callable_operator> registration{[captured](i64 value) { return value * *captured; }};
    }

    CHECK_FALSE(captured_lifetime.expired());
    const auto callable = [](wp<TS<i64>> value) { return callable_operator(value); };
    CHECK(bg::run_graph(callable, std::array<i64, 1>{7}).equals<i64>({42}));
}

TEST_CASE("an aggregate overload registers when its declaration object is constructed",
          "[feature:operator][feature:wiring][definition:aggregate]") {
    struct registration_group {
        overload<aggregate_operator, pass_i64> pass_impl;
    };

    const auto aggregate = [](wp<TS<i64>> value) { return aggregate_operator(value); };
    CHECK_THROWS_AS(bg::run_graph(aggregate, std::array<i64, 1>{3}), WiringError);

    register_overloads<registration_group>();
    CHECK(bg::run_graph(aggregate, std::array<i64, 1>{3}).equals<i64>({3}));

    testing::test_wiring([&] {
        const auto source = node([](Out<TS<i64>>) {});
        aggregate_operator(source());
        auto &wiring = WiringContext::current().wiring();
        wiring.finalize();
        const auto nodes = wiring.nodes();
        CHECK(std::ranges::any_of(nodes, [](const auto &wired_node) {
            return wired_node.signature().name.text() == "pass_impl";
        }));
    });

    try {
        testing::test_wiring([&] { aggregate_operator(std::string_view{"invalid"}); });
        FAIL("expected the aggregate overload to reject the argument type");
    } catch (const WiringError &error) {
        REQUIRE(error.diagnostic().has_value());
        REQUIRE(error.diagnostic()->candidates.size() == 1);
        CHECK(error.diagnostic()->candidates.front().signature.name.text() == "pass_impl");
    }
}

TEST_CASE("callable overload group members name their wired nodes and diagnostics",
          "[feature:operator][feature:wiring][definition:function]") {
    struct registration_group {
        overload<reflected_callable_operator> callable_impl{[](i64 value) { return value; }};
    };

    register_overloads<registration_group>();

    testing::test_wiring([&] {
        reflected_callable_operator(i64{3});
        auto &wiring = WiringContext::current().wiring();
        wiring.finalize();
        const auto nodes = wiring.nodes();
        CHECK(std::ranges::any_of(nodes, [](const auto &wired_node) {
            return wired_node.signature().name.text() == "callable_impl";
        }));
    });

    try {
        testing::test_wiring([&] { reflected_callable_operator(std::string_view{"invalid"}); });
        FAIL("expected the callable overload to reject the argument type");
    } catch (const WiringError &error) {
        REQUIRE(error.diagnostic().has_value());
        REQUIRE(error.diagnostic()->candidates.size() == 1);
        CHECK(error.diagnostic()->candidates.front().signature.name.text() == "callable_impl");
    }
}

TEST_CASE("an overload registers an already constructed node template",
          "[feature:operator][feature:wiring][definition:aggregate]") {
    {
        auto implementation = node<pass_i64>();
        overload<node_template_operator> registration{std::move(implementation)};
    }

    const auto graph = [](wp<TS<i64>> value) { return node_template_operator(value); };
    CHECK(bg::run_graph(graph, std::array<i64, 1>{5}).equals<i64>({5}));
}

TEST_CASE("an overload registers an already constructed graph template",
          "[feature:operator][feature:wiring][definition:function]") {
    {
        const auto implementation = graph([](wp<TS<i64>> value) { return value; });
        overload<graph_template_operator> registration{implementation};
    }
    const auto graph = [](wp<TS<i64>> value) { return graph_template_operator(value); };
    CHECK(bg::run_graph(graph, std::array<i64, 1>{8}).equals<i64>({8}));
}

TEST_CASE("graph overload groups reflect member names and rename copies independently",
          "[feature:operator][feature:wiring][definition:function]") {
    struct registration_group {
        overload<reflected_graph_operator> identity_graph{graph([](wp<TS<i64>> value) { return value; })};
    };

    register_overloads<registration_group>();

    try {
        testing::test_wiring([&] { reflected_graph_operator(std::string_view{"invalid"}); });
        FAIL("expected the graph overload to reject the argument type");
    } catch (const WiringError &error) {
        REQUIRE(error.diagnostic().has_value());
        REQUIRE(error.diagnostic()->candidates.size() == 1);
        CHECK(error.diagnostic()->candidates.front().signature.name.text() == "identity_graph");
    }

    auto implementation = graph([](wp<TS<i64>> value) { return value; });
    auto named = implementation;
    named.set_name(util::symbol{"identity_graph"});
    auto renamed = named;
    renamed.set_name(util::symbol{"renamed_identity"});
    CHECK(named.signature().name().text() == "identity_graph");
    CHECK(renamed.signature().name().text() == "renamed_identity");
    CHECK(implementation.signature().name().text() != "identity_graph");
    const auto graph = [](wp<TS<i64>> value) { return reflected_graph_operator(value); };
    CHECK(bg::run_graph(graph, std::array<i64, 1>{8}).equals<i64>({8}));
}

TEST_CASE("instantiate_overloads registers each selected class-template specialization",
          "[feature:operator][feature:wiring][definition:aggregate]") {
    {
        const instantiate_overloads<family_operator, pass_value, i64, double> registrations;
    }
    const auto integer_graph = [](wp<TS<i64>> value) { return family_operator(value); };
    const auto floating_graph = [](wp<TS<double>> value) { return family_operator(value); };
    CHECK(bg::run_graph(integer_graph, std::array<i64, 2>{2, 4}).equals<i64>({2, 4}));
    CHECK(bg::run_graph(floating_graph, std::array{1.5, 3.5}).equals<double>({1.5, 3.5}));
}

TEST_CASE("family overload groups apply one reflected name to each specialization",
          "[feature:operator][feature:wiring][definition:aggregate]") {
    struct registration_group {
        instantiate_overloads<reflected_family_operator, pass_value, i64, double> family_impl;
    };

    register_overloads<registration_group>();

    try {
        testing::test_wiring([&] { reflected_family_operator(std::string_view{"invalid"}); });
        FAIL("expected the family overloads to reject the argument type");
    } catch (const WiringError &error) {
        REQUIRE(error.diagnostic().has_value());
        REQUIRE(error.diagnostic()->candidates.size() == 2);
        CHECK(error.diagnostic()->candidates[0].signature.name.text() == "family_impl");
        CHECK(error.diagnostic()->candidates[1].signature.name.text() == "family_impl");
    }
}

TEST_CASE("aggregate template renaming keeps shared signatures independent",
          "[feature:operator][feature:wiring][definition:aggregate]") {
    auto first = node<pass_i64>();
    auto second = node<pass_i64>();
    first.set_name(util::symbol{"first"});
    second.set_name(util::symbol{"second"});

    CHECK(first.signature().name().text() == "first");
    CHECK(second.signature().name().text() == "second");
}

TEST_CASE("library callable implementations use their registered names",
          "[feature:operator][feature:wiring][library]") {
    testing::test_wiring([&] {
        operators::add(1.0, 2.0);
        operators::len_(std::string_view{"value"});
        operators::contains_(std::string_view{"value"}, std::string_view{"a"});
        auto &wiring = WiringContext::current().wiring();
        wiring.finalize();
        const auto nodes = wiring.nodes();
        CHECK(std::ranges::any_of(nodes, [](const auto &wired_node) {
            return wired_node.signature().name.text() == "add_double";
        }));
        CHECK(std::ranges::any_of(nodes, [](const auto &wired_node) {
            return wired_node.signature().name.text() == "len_string";
        }));
        CHECK(std::ranges::any_of(nodes, [](const auto &wired_node) {
            return wired_node.signature().name.text() == "contains_string";
        }));
    });
}

TEST_CASE("operator diagnostics use the renamed operator signature",
    "[feature:operator][feature:wiring][diagnostic]") {
    auto operation = operator_<auto(i64) -> i64>{"before", {"value"}};
    const auto implementation = node([](i64 value) { return value; });
    operation.overload(implementation);
    operation.set_name(util::symbol{"after"});

    CHECK(operation.signature().name().text() == "after");
    try {
        testing::test_wiring([&] { operation(std::string_view{"invalid"}); });
        FAIL("expected the renamed operator to reject the argument type");
    } catch (const WiringError &error) {
        REQUIRE(error.diagnostic().has_value());
        CHECK(error.diagnostic()->operator_name.text() == "after");
    }
}

TEST_CASE("an overload forwards node options to node",
          "[feature:operator][feature:wiring][definition:function][transition:start]") {
    const overload<scheduled_operator> registration{
        eval = [](In<TS_T, "value"_n, optional> value, Out<TS<bool>> output) {
            output.set(value.valid());
        },
        schedule_on_start = true};

    CHECK(bg::run_graph(scheduled_operator, input<TS<i64>>({literal::INVALID}))
              .equals<bool>({false}));
}

TEST_CASE("an overload forwards callable requirements and resolvers to node",
          "[feature:operator][feature:wiring][definition:function][declaration:generic]") {
    const overload<resolved_callable_operator> registration{
        eval = [](In<TS<RESULT>> value, Out<TS<RESULT>> output) { output.copy_tick(value); },
        resolve<RESULT> = []() -> const ScalarType & { return int64_type(); },
        require = [](const WiringArguments &arguments) { return &arguments.resolved_type(0) == &TS<i64>::type(); }};

    CHECK(bg::run_graph(resolved_callable_operator, input<TS<i64>>({2, 4})).equals<i64>({2, 4}));
    CHECK_THROWS_AS(bg::run_graph(resolved_callable_operator, input<TS<double>>({1.5})), WiringError);
}

TEST_CASE("registration members run in declaration order",
          "[feature:operator][feature:wiring][definition:function]") {
    struct registration_group {
        overload<registration_order_operator> first{
            eval = [](i64) { return i64{1}; },
            require = [](const WiringArguments &) {
                registration_order.push_back(1);
                return reject("select the next candidate");
            }};
        overload<registration_order_operator> second{
            eval = [](i64) { return i64{2}; },
            require = [](const WiringArguments &) {
                registration_order.push_back(2);
                return accept;
            }};
    };

    registration_order.clear();
    const registration_group registrations{};
    const auto graph = [](wp<TS<i64>> value) { return registration_order_operator(value); };
    CHECK(bg::run_graph(graph, std::array<i64, 1>{0}).equals<i64>({2}));
    CHECK(registration_order == std::vector<int>{1, 2});
}

TEST_CASE("an output operator can register a sink graph overload",
          "[feature:operator][feature:wiring][definition:function][shape:sink]") {
    const auto sink_node = node([](In<TS<i64>>) {});
    const auto sink_graph = graph("name"_n = "sink", [sink_node](wp<TS<i64>> value) { sink_node(value); });
    overload<sink_graph_operator> registration{sink_graph};

    testing::test_wiring([&] {
        const auto source = node([](Out<TS<i64>>) {});
        CHECK_FALSE(sink_graph_operator(source()).has_output());
    });
}
