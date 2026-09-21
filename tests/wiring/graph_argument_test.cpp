#include <bg/graph.hpp>
#include <bg/graph/graph_argument.hpp>
#include <bg/graph/operator.hpp>
#include <bg/graph/wiring_arguments.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/node.hpp>
#include <bg/operator.hpp>
#include <bg/scalar/opaque.hpp>
#include <bg/scalar/value.hpp>
#include <bg/testing/test_wiring.hpp>
#include <bg/time_series/tsb/tsb.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>

#include <array>
#include <compare>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace bg;
using namespace bg::literals;

struct boundary_node {
    static constexpr bool deduplicate_wiring = false;
    Out<TS<int>> output;
    void evaluate() const {}
};

struct other_boundary_node {
    static constexpr bool deduplicate_wiring = false;
    Out<TS<int>> output;
    void evaluate() const {}
};

struct float_boundary_node {
    static constexpr bool deduplicate_wiring = false;
    Out<TS<float>> output;
    void evaluate() const {}
};

struct add_node {
    In<TS<int>> left;
    In<TS<int>> right;
    Out<TS<int>> output;
    void evaluate() const {}
};

struct multiply_node {
    In<TS<int>> left;
    In<TS<int>> right;
    Out<TS<int>> output;
    void evaluate() const {}
};

const auto add_operation = node<add_node>();

inline constexpr auto addition_pattern = graph_pattern(
    [](wp<TS<int>> left, wp<TS<int>> right) {
        return add_operation(left, right);
    });

inline constexpr auto binary_first_pattern = graph_pattern(
    [](auto left, auto) {
        return left;
    });

using GRAPH_VALUE = ScalarVariable<"GRAPH_VALUE">;

struct opaque_value {
    int value;
};

struct opaque_source_node {
    Scalar<Opaque<opaque_value>> value;
    Out<TS<int>> output;
    void evaluate() const {}
};

struct graph_result_node {
    Out<TS<int>> output;
    void evaluate() const {}
};

struct graph_sink_node {
    In<TS<int>> left;
    In<TS<int>> right;
    void evaluate() const {}
};

template <typename Boundary = boundary_node>
details::GraphWiringStructure identity_structure() {
    auto wiring = GraphWiring{};
    const auto context = WiringContext{wiring};
    const auto boundary = node<Boundary>();
    const auto value = boundary();
    const auto boundaries = std::array<WiringPoint, 1>{value};

    return details::graph_wiring_structure(wiring, boundaries, static_cast<WiringPoint>(value));
}

template <typename Operation>
details::GraphWiringStructure binary_structure() {
    auto wiring = GraphWiring{};
    const auto context = WiringContext{wiring};
    const auto boundary = node<boundary_node>();
    const auto left = boundary();
    const auto right = boundary();
    const auto operation = node<Operation>();
    const auto result = operation(left, right);
    const auto boundaries = std::array<WiringPoint, 2>{left, right};

    return details::graph_wiring_structure(wiring, boundaries, static_cast<WiringPoint>(result));
}

details::GraphWiringStructure uncacheable_structure() {
    auto wiring = GraphWiring{};
    const auto context = WiringContext{wiring};
    const auto value = ScalarValue<Opaque<opaque_value>>{opaque_value{1}};
    const auto source = node<opaque_source_node>();
    const auto result = source(value);

    return details::graph_wiring_structure(wiring, std::span<const WiringPoint>{},
                                           static_cast<WiringPoint>(result));
}

details::GraphWiringStructure source_structure() {
    auto wiring = GraphWiring{};
    const auto context = WiringContext{wiring};
    const auto source = node<graph_result_node>();
    const auto result = source();

    return details::graph_wiring_structure(wiring, std::span<const WiringPoint>{},
                                           static_cast<WiringPoint>(result));
}

details::GraphWiringStructure sink_structure() {
    auto wiring = GraphWiring{};
    const auto context = WiringContext{wiring};
    const auto boundary = node<boundary_node>();
    const auto left = boundary();
    const auto right = boundary();
    const auto sink = node<graph_sink_node>();
    sink(left, right);
    const auto boundaries = std::array<WiringPoint, 2>{left, right};

    return details::graph_wiring_structure(wiring, boundaries);
}

GraphArgument argument(details::GraphWiringStructure structure) {
    return GraphArgument{std::move(structure), std::make_shared<const int>(1)};
}

Signature graph_overload_signature(util::symbol name, const GraphArgumentType &type, bool output) {
    auto parameters = std::vector<Parameter>{{util::symbol{"function"}, &type}};
    if (output) {
        return Signature{name, std::move(parameters), TS<int>::type()};
    }
    return Signature{name, std::move(parameters)};
}

class TestDependentOperator final : public Operator {
  public:
    explicit TestDependentOperator(Signature prototype) : Operator(std::move(prototype), dependent_result) {}
};

class GraphOverload final : public WiringTemplate {
  public:
    GraphOverload(util::symbol name, details::GraphWiringStructure structure, bool output,
                  std::string &selected, int *requirements = nullptr)
        : type_{std::make_shared<GraphArgumentType>(std::move(structure))},
          signature_{graph_overload_signature(name, *type_, output)},
          output_{output}, selected_{&selected}, requirements_{requirements} {}

    GraphOverload(util::symbol name, bool output, std::string &selected, int *requirements = nullptr)
        : signature_{graph_overload_signature(name, GraphArgument::type(), output)},
          output_{output}, selected_{&selected}, requirements_{requirements} {}

    const Signature &signature() const final { return signature_; }

    RequirementResult requirement(const WiringArguments &arguments) const final {
        if (arguments.graph("function") == nullptr) {
            return reject("the function argument is not a graph");
        }
        if (requirements_ != nullptr) {
            ++*requirements_;
        }
        return accept;
    }

  private:
    std::optional<WiringPoint> wire(GraphWiring &, ResolvedSignature, const TypeMap &,
                                    std::span<const WiringArgument>) const final {
        *selected_ = signature_.name().text();
        if (!output_) {
            return std::nullopt;
        }

        return static_cast<WiringPoint>(result_source_());
    }

    std::shared_ptr<const GraphArgumentType> type_;
    Signature signature_;
    bool output_;
    std::string *selected_;
    int *requirements_;
    decltype(node<graph_result_node>()) result_source_{node<graph_result_node>()};
};

class GraphNode final : public NodeTemplate {
  public:
    GraphNode() : signature_{util::symbol{"invalid_graph_node"},
                             {{util::symbol{"function"}, &GraphArgument::type()}}} {}

    const Signature &signature() const final { return signature_; }
    EvaluationFunction evaluation_function(const ResolvedSignature &) const noexcept final { return nullptr; }

  private:
    Signature signature_;
};

struct aggregate_graph_argument_sink {
    GraphArgument function;

    void wire() const {
        if (function.structure().boundary_count() != 1) {
            throw std::logic_error{"the graph argument has the wrong boundary count"};
        }
    }
};

struct typed_aggregate_graph_argument_sink {
    GRAPH<auto(TS<GRAPH_VALUE>, TS<GRAPH_VALUE>) -> TS<GRAPH_VALUE>> function;

    void wire() const {
        if (function.graph_argument().structure().boundary_count() != 2) {
            throw std::logic_error{"the graph argument has the wrong boundary count"};
        }
    }
};

} // namespace

TEST_CASE("graph argument matching supports any, exact, and generic patterns",
          "[feature:operator][feature:wiring][feature:graph-argument]") {
    const auto add_structure = binary_structure<add_node>();
    const auto multiply = binary_structure<multiply_node>();
    const auto supplied = argument(add_structure);
    const auto exact = GraphArgumentType{add_structure};
    const auto other = GraphArgumentType{multiply};

    const auto add_operation = node<add_node>();
    const auto add_graph = graph("name"_n = "add_pattern",
                                 [add_operation](wp<TS<SCALAR>> left, wp<TS<SCALAR>> right) {
        return add_operation(left.as<TS<int>>(), right.as<TS<int>>());
    });
    const auto pattern = GraphArgumentType{std::make_shared<const decltype(add_graph)>(add_graph)};

    CHECK(GraphArgument::type().accepts(supplied.exact_type()));
    CHECK(exact.accepts(supplied.exact_type()));
    CHECK_FALSE(other.accepts(supplied.exact_type()));
    CHECK(pattern.accepts(supplied.exact_type()));
}

TEST_CASE("graph types resolve variables from their parameters and output",
          "[feature:operator][feature:type-system][feature:graph-argument]") {
    using VALUE = ScalarVariable<"VALUE">;
    using Function = GRAPH<auto(TS<VALUE>, TS<VALUE>) -> TS<VALUE>>;

    const auto supplied = argument(binary_structure<add_node>());
    auto type_map = TypeMap{};

    REQUIRE(Function::type().match(supplied.exact_type(), type_map));
    CHECK(&Function::type().resolve(type_map) ==
          &graph_argument_type(
              std::array<const Type *, 2>{&TS<int>::type(), &TS<int>::type()}, &TS<int>::type()));
    CHECK(std::get<const Type *>(*type_map.find(VALUE::type())) == &cpp_scalar_type<int>());
}

TEST_CASE("graph types resolve scalar parameters with the same variables as time series parameters",
          "[feature:operator][feature:type-system][feature:graph-argument][argument:scalar]") {
    using VALUE = ScalarVariable<"VALUE">;
    using Function = GRAPH<auto(VALUE, TS<VALUE>) -> TS<VALUE>>;

    const auto supplied = GraphArgumentType{
        std::vector<const Type *>{&cpp_scalar_type<int>(), &TS<int>::type()}, &TS<int>::type()};
    auto type_map = TypeMap{};

    REQUIRE(Function::type().match(supplied, type_map));
    CHECK(std::get<const Type *>(*type_map.find(VALUE::type())) == &cpp_scalar_type<int>());
}

TEST_CASE("repeated variables in graph types reject inconsistent graph signatures",
          "[feature:operator][feature:type-system][feature:graph-argument]") {
    using VALUE = ScalarVariable<"VALUE">;
    using Function = GRAPH<auto(TS<VALUE>, TS<VALUE>) -> TS<VALUE>>;

    const auto mixed = GraphArgumentType{
        std::vector<const Type *>{&TS<int>::type(), &TS<float>::type()}, &TS<int>::type()};
    auto type_map = TypeMap{};

    CHECK_FALSE(Function::type().match(mixed, type_map));
    CHECK(type_map.find(VALUE::type()) == nullptr);
}

TEST_CASE("repeated variables make graph types more specific",
          "[feature:operator][feature:type-system][feature:graph-argument]") {
    using VALUE = ScalarVariable<"VALUE">;
    using LEFT = ScalarVariable<"LEFT">;
    using RIGHT = ScalarVariable<"RIGHT">;
    using RESULT = ScalarVariable<"RESULT">;
    using Same = GRAPH<auto(TS<VALUE>, TS<VALUE>) -> TS<VALUE>>;
    using Independent = GRAPH<auto(TS<LEFT>, TS<RIGHT>) -> TS<RESULT>>;

    const auto same = Signature{util::symbol{"same"}, {{util::symbol{"function"}, &Same::type()}}};
    const auto independent =
        Signature{util::symbol{"independent"}, {{util::symbol{"function"}, &Independent::type()}}};

    CHECK(compare_specificity(same, independent) == std::partial_ordering::less);
    CHECK(compare_specificity(independent, same) == std::partial_ordering::greater);
}

TEST_CASE("graph patterns constrain graph types during matching",
          "[feature:operator][feature:type-system][feature:graph-argument]") {
    using Addition = GRAPH<auto(TS<int>, TS<int>) -> TS<int>, addition_pattern>;

    const auto add = argument(binary_structure<add_node>());
    const auto multiply = argument(binary_structure<multiply_node>());

    CHECK(Addition::type().accepts(add.exact_type()));
    CHECK_FALSE(Addition::type().accepts(multiply.exact_type()));
}

TEST_CASE("graph types with the same pattern retain signature specificity",
          "[feature:operator][feature:type-system][feature:graph-argument]") {
    using VALUE = ScalarVariable<"VALUE">;
    using Generic = GRAPH<auto(TS<VALUE>, TS<VALUE>) -> TS<VALUE>, binary_first_pattern>;
    using Exact = GRAPH<auto(TS<int>, TS<int>) -> TS<int>, binary_first_pattern>;

    const auto generic =
        Signature{util::symbol{"generic"}, {{util::symbol{"function"}, &Generic::type()}}};
    const auto exact = Signature{util::symbol{"exact"}, {{util::symbol{"function"}, &Exact::type()}}};

    CHECK(compare_specificity(exact, generic) == std::partial_ordering::less);
    CHECK(compare_specificity(generic, exact) == std::partial_ordering::greater);
}

TEST_CASE("a callable operator overload resolves variables nested in a graph type",
          "[feature:operator][feature:type-system][feature:graph-argument][definition:function]") {
    const auto result_source = node<graph_result_node>();
    const auto implementation = graph(
        "name"_n = "typed_graph_overload",
        [result_source](GRAPH<auto(TS<GRAPH_VALUE>, TS<GRAPH_VALUE>) -> TS<GRAPH_VALUE>> function) {
            CHECK(function.graph_argument().structure().boundary_count() == 2);
            return result_source();
        });
    auto operation = operator_<auto(GRAPH_T) -> TS_T>{"apply", {"function"}};
    operation.overload(implementation);

    testing::test_wiring([&] {
        const auto result = operation(argument(binary_structure<add_node>()));
        CHECK(&result.type() == &TS<int>::type());
    });
}

TEST_CASE("graph type variables constrain other overload parameters",
          "[feature:operator][feature:type-system][feature:graph-argument][definition:function]") {
    using VALUE = ScalarVariable<"VALUE">;

    const auto result_source = node<graph_result_node>();
    const auto implementation = graph(
        "name"_n = "graph_and_value",
        [result_source](GRAPH<auto(TS<VALUE>) -> TS<VALUE>>, wp<TS<VALUE>>) {
            return result_source();
        });
    auto operation = operator_<auto(GRAPH_T, TS_T) -> TS_T>{"apply", {"function", "value"}};
    operation.overload(implementation);

    testing::test_wiring([&] {
        const auto integer_source = node<boundary_node>();
        const auto floating_point_source = node<float_boundary_node>();
        const auto integer = integer_source();
        const auto floating_point = floating_point_source();
        const auto function = argument(identity_structure());

        CHECK(&operation(function, integer).type() == &TS<int>::type());
        CHECK_THROWS_AS(operation(function, floating_point), WiringError);
    });
}

TEST_CASE("an aggregate graph resolves variables nested in a graph type",
          "[feature:wiring][feature:type-system][feature:graph-argument][definition:aggregate]") {
    const auto sink = graph<typed_aggregate_graph_argument_sink>();

    testing::test_wiring([&] {
        sink(argument(binary_structure<add_node>()));
    });
}

TEST_CASE("graph structure treats declared inputs as external boundaries",
          "[feature:wiring][feature:graph-argument]") {
    CHECK(identity_structure<boundary_node>() == identity_structure<other_boundary_node>());
}

TEST_CASE("graph structure rejects overlapping external boundaries",
          "[feature:wiring][feature:graph-argument]") {
    auto wiring = GraphWiring{};
    const auto context = WiringContext{wiring};
    const auto boundary = node<boundary_node>();
    const auto value = boundary();
    const auto boundaries = std::array<WiringPoint, 2>{value, value};

    CHECK_THROWS_WITH(details::graph_wiring_structure(wiring, boundaries),
                      "graph boundaries must identify distinct outputs");
}

TEST_CASE("a callable graph receives an owning graph argument",
          "[feature:wiring][feature:graph-argument][definition:function]") {
    const auto sink = graph("name"_n = "graph_argument_sink", [](const GraphArgument &function) {
        CHECK(function.structure().boundary_count() == 1);
    });

    testing::test_wiring([&] { sink(argument(identity_structure())); });
}

TEST_CASE("an aggregate graph receives an owning graph argument",
          "[feature:wiring][feature:graph-argument][definition:aggregate]") {
    const auto sink = graph<aggregate_graph_argument_sink>();

    testing::test_wiring([&] { sink(argument(identity_structure())); });
}

TEST_CASE("a typed operator prototype accepts a graph argument",
          "[feature:operator][feature:wiring][feature:graph-argument][definition:function]") {
    const auto result_source = node<graph_result_node>();
    const auto implementation = graph("name"_n = "apply_graph", [result_source](const GraphArgument &function) {
        CHECK(function.structure().boundary_count() == 1);
        return result_source();
    });
    auto operation = operator_<auto(GraphArgument) -> TS<int>>{"apply_graph", {"function"}};
    operation.overload(implementation);

    testing::test_wiring([&] {
        const auto result = operation(argument(identity_structure()));
        CHECK(&result.type() == &TS<int>::type());
    });
}

TEST_CASE("graph patterns are less generic than any graph and distinct patterns are unordered",
          "[feature:operator][feature:type-system][feature:graph-argument]") {
    const auto add_node_template = node<add_node>();
    const auto multiply_node_template = node<multiply_node>();
    const auto add_graph = graph("name"_n = "add_pattern", [add_node_template](wp<TS<int>> left, wp<TS<int>> right) {
        return add_node_template(left, right);
    });
    const auto multiply_graph = graph("name"_n = "multiply_pattern",
                                      [multiply_node_template](wp<TS<int>> left, wp<TS<int>> right) {
        return multiply_node_template(left, right);
    });
    const auto add = GraphArgumentType{std::make_shared<const decltype(add_graph)>(add_graph)};
    const auto multiply = GraphArgumentType{std::make_shared<const decltype(multiply_graph)>(multiply_graph)};
    const auto add_signature = Signature{util::symbol{"add"}, {{util::symbol{"function"}, &add}}};
    const auto multiply_signature = Signature{util::symbol{"multiply"}, {{util::symbol{"function"}, &multiply}}};
    const auto any_signature =
        Signature{util::symbol{"any"}, {{util::symbol{"function"}, &GraphArgument::type()}}};

    CHECK(compare_specificity(add_signature, any_signature) == std::partial_ordering::less);
    CHECK(compare_specificity(any_signature, add_signature) == std::partial_ordering::greater);
    CHECK(compare_specificity(add_signature, multiply_signature) == std::partial_ordering::unordered);
}

TEST_CASE("a dependent-result operator selects source, value, and sink graph overloads",
          "[feature:operator][feature:wiring][feature:graph-argument]") {
    auto selected = std::string{};
    auto operation = TestDependentOperator{
        Signature{util::symbol{"map"}, {{util::symbol{"function"}, &GraphArgument::type()}}}};
    operation.overload(GraphOverload{util::symbol{"value"}, identity_structure(), true, selected});
    operation.overload(GraphOverload{util::symbol{"source"}, source_structure(), true, selected});
    operation.overload(GraphOverload{util::symbol{"sink"}, sink_structure(), false, selected});

    testing::test_wiring([&] {
        const auto result = operation(argument(identity_structure()));
        CHECK(result.has_value());
        CHECK(selected == "value");
    });

    testing::test_wiring([&] {
        const auto result = operation(argument(source_structure()));
        CHECK(result.has_value());
        CHECK(selected == "source");
    });

    testing::test_wiring([&] {
        const auto result = operation(argument(sink_structure()));
        CHECK_FALSE(result.has_value());
        CHECK(selected == "sink");
    });
}

TEST_CASE("operator caching uses graph structure and bypasses uncacheable structures",
          "[feature:operator][feature:wiring][feature:graph-argument]") {
    auto selected = std::string{};
    auto requirements = 0;
    auto operation = TestDependentOperator{
        Signature{util::symbol{"map"}, {{util::symbol{"function"}, &GraphArgument::type()}}}};
    operation.overload(GraphOverload{util::symbol{"generic"}, true, selected, &requirements});

    testing::test_wiring([&] {
        (void)operation(argument(identity_structure<boundary_node>()));
        (void)operation(argument(identity_structure<other_boundary_node>()));
    });
    CHECK(requirements == 1);

    testing::test_wiring([&] {
        (void)operation(argument(uncacheable_structure()));
        (void)operation(argument(uncacheable_structure()));
    });
    CHECK(requirements == 3);
}

TEST_CASE("operator diagnostics retain graph argument types",
          "[feature:operator][feature:wiring][feature:graph-argument]") {
    auto operation = TestDependentOperator{
        Signature{util::symbol{"map"}, {{util::symbol{"function"}, &GraphArgument::type()}}}};
    auto failure = std::optional<WiringError>{};

    try {
        testing::test_wiring([&] { (void)operation(argument(identity_structure())); });
    } catch (const WiringError &error) {
        failure = error;
    }

    REQUIRE(failure);
    REQUIRE(failure->diagnostic());
    REQUIRE(failure->diagnostic()->arguments.size() == 1);
    CHECK(failure->diagnostic()->arguments[0].type->is_graph());
    CHECK(std::string_view{failure->what()}.contains(
        "function: GRAPH<auto(TS<int>) -> TS<int>>"));
}

TEST_CASE("variadic parameters reject graph arguments",
          "[feature:wiring][feature:graph-argument][feature:variadic-wiring][series:tsb][schema:generic]") {
    const auto variadic = graph("name"_n = "variadic", [](wp<TSB<SCHEMA>, var_args>) {});

    CHECK_THROWS_WITH(testing::test_wiring([&] { variadic(argument(identity_structure())); }),
                      "a variadic parameter cannot capture a graph argument");
}

TEST_CASE("a graph argument cannot become node runtime storage",
          "[feature:operator][feature:wiring][feature:graph-argument]") {
    CHECK_THROWS_WITH(testing::test_wiring([&] {
                          const auto invalid = GraphNode{};
                          (void)invalid(argument(identity_structure()));
                      }),
                      "a graph argument cannot be stored by a node");
}
