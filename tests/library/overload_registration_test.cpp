#include <bg/graph.hpp>
#include <bg/library.hpp>
#include <bg/testing/test_graph.hpp>
#include <bg/testing/test_wiring.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <memory>
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
inline operator_<auto(TS_T) -> TS_T> sink_graph_operator{"sink_graph", {"value"}};
inline operator_<auto(SCALAR) -> SCALAR> family_operator{"family", {"value"}};
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
static_assert(std::is_empty_v<overload<callable_operator>>);
static_assert(std::is_empty_v<instantiate_overloads<family_operator, pass_value, i64, double>>);

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
    CHECK(testing::test_graph(callable_operator, std::array<i64, 1>{7}).equals<i64>({42}));
}

TEST_CASE("an aggregate overload registers when its declaration object is constructed",
          "[feature:operator][feature:wiring][definition:aggregate]") {
    struct registration_group {
        overload<aggregate_operator, pass_i64> pass;
    };

    CHECK_THROWS_AS(testing::test_graph(aggregate_operator, std::array<i64, 1>{3}), WiringError);

    const registration_group registrations{};
    CHECK(testing::test_graph(aggregate_operator, std::array<i64, 1>{3}).equals<i64>({3}));
}

TEST_CASE("an overload registers an already constructed node template",
          "[feature:operator][feature:wiring][definition:aggregate]") {
    {
        auto implementation = node<pass_i64>();
        overload<node_template_operator> registration{std::move(implementation)};
    }

    CHECK(testing::test_graph(node_template_operator, std::array<i64, 1>{5}).equals<i64>({5}));
}

TEST_CASE("an overload registers an already constructed graph template",
          "[feature:operator][feature:wiring][definition:function]") {
    {
        const auto implementation = graph("name"_n = "identity", [](wp<TS<i64>> value) { return value; });
        overload<graph_template_operator> registration{implementation};
    }

    CHECK(testing::test_graph(graph_template_operator, std::array<i64, 1>{8}).equals<i64>({8}));
}

TEST_CASE("instantiate_overloads registers each selected class-template specialization",
          "[feature:operator][feature:wiring][definition:aggregate]") {
    {
        const instantiate_overloads<family_operator, pass_value, i64, double> registrations;
    }
    CHECK(testing::test_graph(family_operator, std::array<i64, 2>{2, 4}).equals<i64>({2, 4}));
    CHECK(testing::test_graph(family_operator, std::array{1.5, 3.5}).equals<double>({1.5, 3.5}));
}

TEST_CASE("an overload forwards node options to node",
          "[feature:operator][feature:wiring][definition:function][transition:start]") {
    const overload<scheduled_operator> registration{
        eval = [](In<TS_T, "value"_n, optional> value, Out<TS<bool>> output) {
            output.set(value.valid());
        },
        schedule_on_start = true};

    CHECK(testing::test_graph(scheduled_operator, std::array<std::optional<i64>, 1>{std::nullopt})
              .equals<bool>({false}));
}

TEST_CASE("an overload forwards callable requirements and resolvers to node",
          "[feature:operator][feature:wiring][definition:function][declaration:generic]") {
    const overload<resolved_callable_operator> registration{
        eval = [](In<TS<RESULT>> value, Out<TS<RESULT>> output) { output.copy_tick(value); },
        resolve<RESULT> = []() -> const ScalarType & { return int64_type(); },
        require = [](const WiringArguments &arguments) { return &arguments.resolved_type(0) == &TS<i64>::type(); }};

    CHECK(testing::test_graph(resolved_callable_operator, std::array<i64, 2>{2, 4}).equals<i64>({2, 4}));
    CHECK_THROWS_AS(testing::test_graph(resolved_callable_operator, std::array{1.5}), WiringError);
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
    CHECK(testing::test_graph(registration_order_operator, std::array<i64, 1>{0}).equals<i64>({2}));
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
