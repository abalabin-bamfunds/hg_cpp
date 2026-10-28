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
#include <bg/type/type_resolution_error.hpp>
#include <bg/type/type_registry.hpp>
#include <bg/util/static_storage.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <array>
#include <compare>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace run_graph_argument {

struct captured_pointer {
    std::shared_ptr<int> value;

    explicit captured_pointer(std::shared_ptr<int> value) : value{std::move(value)} {}
    captured_pointer(const captured_pointer &) = default;
    captured_pointer(captured_pointer &&) noexcept = default;
    ~captured_pointer() { value.reset(); }

    friend bool operator==(const captured_pointer &, const captured_pointer &) = default;
};

} // namespace run_graph_argument

namespace std {

template <> struct hash<run_graph_argument::captured_pointer> {
    std::size_t operator()(const run_graph_argument::captured_pointer &value) const noexcept {
        return std::hash<std::shared_ptr<int>>{}(value.value);
    }
};

} // namespace std

namespace bg {

template <> struct scalar_type_traits<run_graph_argument::captured_pointer> {
    static const ScalarType &type() noexcept {
        class Type final : public ScalarType {
          public:
            bool is_resolved() const noexcept override { return true; }
            const bg::Type &resolve(const TypeMap &) const override { return *this; }
            bool supports_equality() const noexcept override { return true; }
            bool supports_hash() const noexcept override { return true; }
        };

        static const auto type = util::static_storage<Type>{};
        [[maybe_unused]] static const auto &registration = TypeRegistry::register_static(type.get());
        return type.get();
    }
};

} // namespace bg

namespace {

using namespace bg;
using namespace bg::literals;

struct boundary_node {
    static constexpr bool deduplicate_wiring = false;
    Out<TS<i64>> output;
    void evaluate() const {}
};

struct other_boundary_node {
    static constexpr bool deduplicate_wiring = false;
    Out<TS<i64>> output;
    void evaluate() const {}
};

struct float_boundary_node {
    static constexpr bool deduplicate_wiring = false;
    Out<TS<float>> output;
    void evaluate() const {}
};

struct add_node {
    In<TS<i64>> left;
    In<TS<i64>> right;
    Out<TS<i64>> output;
    void evaluate() const {}
};

struct multiply_node {
    In<TS<i64>> left;
    In<TS<i64>> right;
    Out<TS<i64>> output;
    void evaluate() const {}
};

const auto add_operation = node<add_node>();

inline constexpr auto addition_pattern = graph_pattern(
    [](wp<TS<i64>> left, wp<TS<i64>> right) {
        return add_operation(left, right);
    });

inline constexpr auto binary_first_pattern = graph_pattern(
    [](auto left, auto) {
        return left;
    });

inline constexpr auto unary_identity_pattern = graph_pattern(
    [](wp<TS<i64, TimeSeriesGuarantee::ordinary>> value) {
        return value;
    });

int unary_pattern_calls{};
inline constexpr auto counted_unary_pattern = graph_pattern(
    [](wp<TS<i64>> value) {
        ++unary_pattern_calls;
        return value;
    });

using GRAPH_VALUE = ScalarVariable<"GRAPH_VALUE">;

struct opaque_value {
    int value;
};

struct opaque_source_node {
    Scalar<Opaque<opaque_value>> value;
    Out<TS<i64>> output;
    void evaluate() const {}
};

struct captured_integer_node {
    Scalar<int> value;
    Out<TS<i64>> output;
    void evaluate() const {}
};

struct captured_shared_pointer_node {
    Scalar<run_graph_argument::captured_pointer> value;
    Out<TS<i64>> output;
    void evaluate() const {}
};

struct graph_result_node {
    Out<TS<i64>> output;
    void evaluate() const {}
};

struct graph_sink_node {
    In<TS<i64>> left;
    In<TS<i64>> right;
    void evaluate() const {}
};

template <template <typename, auto...> typename Field> struct graph_value_bundle {
    Field<TS<i64>> value;
};

template <template <typename, auto...> typename Field> struct graph_shared_pointer_bundle {
    Field<TS<run_graph_argument::captured_pointer>> value;
};

using GRAPH_INPUT_FORM = Union<"GRAPH_INPUT_FORM", TS<i64>, TSB<graph_value_bundle>>;
using GRAPH_SHARED_VALUE = ScalarVariable<"GRAPH_SHARED_VALUE">;
using GRAPH_LEFT_FORM = Union<"GRAPH_LEFT_FORM", TS<GRAPH_SHARED_VALUE>, TSB<graph_value_bundle>>;
using GRAPH_RIGHT_FORM = Union<"GRAPH_RIGHT_FORM", TS<GRAPH_SHARED_VALUE>, TSB<graph_value_bundle>>;

template <typename Boundary = boundary_node>
details::GraphWiringStructure identity_structure() {
    auto wiring = GraphWiring{};
    const auto context = WiringContext{wiring};

    const auto boundary = node<Boundary>();
    const auto value = boundary();
    const auto boundaries = std::array<WiringPoint, 1>{value};

    (void)wiring.complete_pending_wiring();
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

    (void)wiring.complete_pending_wiring();
    return details::graph_wiring_structure(wiring, boundaries, static_cast<WiringPoint>(result));
}

details::GraphWiringStructure uncacheable_structure() {
    auto wiring = GraphWiring{};
    const auto context = WiringContext{wiring};
    const auto value = ScalarValue<Opaque<opaque_value>>{opaque_value{1}};

    const auto source = node<opaque_source_node>();
    const auto result = source(value);

    (void)wiring.complete_pending_wiring();
    return details::graph_wiring_structure(wiring, std::span<const WiringPoint>{},
                                           static_cast<WiringPoint>(result));
}

details::GraphWiringStructure captured_integer_structure(int captured_value) {
    auto wiring = GraphWiring{};
    const auto context = WiringContext{wiring};

    const auto source = node<captured_integer_node>();
    const auto result = source(ScalarValue<int>{captured_value});

    (void)wiring.complete_pending_wiring();
    return details::graph_wiring_structure(wiring, std::span<const WiringPoint>{},
                                           static_cast<WiringPoint>(result));
}

details::GraphWiringStructure captured_shared_pointer_structure(const std::shared_ptr<int> &captured_value) {
    auto wiring = GraphWiring{};
    const auto context = WiringContext{wiring};

    const auto source = node<captured_shared_pointer_node>();
    const auto result = source(ScalarValue<run_graph_argument::captured_pointer>{
        run_graph_argument::captured_pointer{captured_value}});

    (void)wiring.complete_pending_wiring();
    return details::graph_wiring_structure(wiring, std::span<const WiringPoint>{},
                                           static_cast<WiringPoint>(result));
}

details::GraphWiringStructure source_structure() {
    auto wiring = GraphWiring{};
    const auto context = WiringContext{wiring};

    const auto source = node<graph_result_node>();
    const auto result = source();

    (void)wiring.complete_pending_wiring();
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

    (void)wiring.complete_pending_wiring();
    return details::graph_wiring_structure(wiring, boundaries);
}

details::GraphWiringStructure unary_add_structure() {
    auto wiring = GraphWiring{};
    const auto context = WiringContext{wiring};

    const auto boundary = node<boundary_node>();
    const auto input = boundary();

    const auto add = node<add_node>();
    const auto result = add(input, input);
    const auto boundaries = std::array<WiringPoint, 1>{input};

    (void)wiring.complete_pending_wiring();
    return details::graph_wiring_structure(wiring, boundaries, static_cast<WiringPoint>(result));
}

GraphArgument argument(details::GraphWiringStructure structure) {
    return GraphArgument{std::move(structure), std::make_shared<const int>(1)};
}

GraphArgument argument(std::vector<details::GraphWiringStructure> structures) {
    return GraphArgument{std::move(structures), std::make_shared<const int>(1)};
}

Signature graph_overload_signature(util::symbol name,
                                   std::shared_ptr<const GraphArgumentConstraint> constraint, bool output,
                                   const Type &parameter_type = GraphArgument::type()) {
    auto parameter = Parameter{util::symbol{"function"}, &parameter_type};
    parameter.graph_constraint = std::move(constraint);
    auto parameters = std::vector<Parameter>{std::move(parameter)};
    if (output) {
        return Signature{name, std::move(parameters), TS<i64>::type()};
    }
    return Signature{name, std::move(parameters)};
}

class GraphOverload final : public WiringTemplate {
  public:
    GraphOverload(util::symbol name, details::GraphWiringStructure structure, bool output,
                  std::string &selected, int *requirements = nullptr,
                  const Type &parameter_type = GraphArgument::type())
        : constraint_{std::make_shared<GraphArgumentConstraint>(std::move(structure))},
          signature_{graph_overload_signature(name, constraint_, output, parameter_type)},
          output_{output}, selected_{&selected}, requirements_{requirements} {}

    GraphOverload(util::symbol name, bool output, std::string &selected, int *requirements = nullptr)
        : signature_{graph_overload_signature(name, {}, output)},
          output_{output}, selected_{&selected}, requirements_{requirements} {}

    const Signature &default_signature() const final { return signature_; }

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
    std::optional<WiringPoint> wire(GraphWiring &, ResolvedSignature, const WiringArguments &,
                                    std::span<const WiringArgument>) const final {
        *selected_ = signature_.name().text();
        if (!output_) {
            return std::nullopt;
        }

        return static_cast<WiringPoint>(result_source_());
    }

    std::shared_ptr<const GraphArgumentConstraint> constraint_;
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

    const Signature &default_signature() const final { return signature_; }
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
    const auto exact = GraphArgumentConstraint{add_structure};
    const auto other = GraphArgumentConstraint{multiply};

    const auto add_operation = node<add_node>();

    const auto add_graph = graph("name"_n = "add_pattern",
                                 [add_operation](wp<TS<SCALAR>> left, wp<TS<SCALAR>> right) {
        return add_operation(left, right);
    });
    const auto pattern = GraphArgumentConstraint{std::make_shared<const decltype(add_graph)>(add_graph)};

    CHECK(GraphArgument::type().accepts(supplied.exact_type()));
    CHECK(exact.matches(supplied));
    CHECK_FALSE(other.matches(supplied));
    CHECK(pattern.matches(supplied));
}

TEST_CASE("operator cache constraints retain every ordered graph structure",
          "[feature:operator][feature:wiring][feature:graph-argument]") {
    const auto identity = identity_structure();
    const auto addition = binary_structure<add_node>();
    const auto multiplication = binary_structure<multiply_node>();
    const auto uncacheable = uncacheable_structure();

    const auto first = GraphArgumentConstraint{
        std::vector<details::GraphWiringStructure>{identity, addition}};
    const auto one_graph_set = GraphArgumentConstraint{
        std::vector<details::GraphWiringStructure>{identity}};
    const auto one_graph = GraphArgumentConstraint{identity};
    const auto equivalent = GraphArgumentConstraint{
        std::vector<details::GraphWiringStructure>{identity, addition}};
    const auto different_second = GraphArgumentConstraint{
        std::vector<details::GraphWiringStructure>{identity, multiplication}};
    const auto reversed = GraphArgumentConstraint{
        std::vector<details::GraphWiringStructure>{addition, identity}};
    const auto uncacheable_constraint = GraphArgumentConstraint{
        std::vector<details::GraphWiringStructure>{identity, uncacheable}};

    CHECK(first.same_constraint(equivalent));
    CHECK_FALSE(first.same_constraint(different_second));
    CHECK_FALSE(first.same_constraint(reversed));
    CHECK_FALSE(one_graph_set.same_constraint(one_graph));
    CHECK(first.compare_specificity(equivalent) == std::partial_ordering::equivalent);
    CHECK(first.compare_specificity(different_second) == std::partial_ordering::unordered);
    CHECK(uncacheable_constraint.matches(argument({identity, uncacheable})));
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
              std::array<const Type *, 2>{&TS<i64>::type(), &TS<i64>::type()}, &TS<i64>::type()));
    CHECK(std::get<const Type *>(*type_map.find(VALUE::type())) == &cpp_scalar_type<i64>());
}

TEST_CASE("graph result declarations use declaration guarantee defaults",
          "[feature:type-system][feature:graph-argument][guarantee:ordinary]") {
    using DefaultReference = GRAPH<auto() -> REF<TS<i64>>>;
    using ExplicitReference = GRAPH<auto() -> REF<TS<i64, TimeSeriesGuarantee::ordinary>,
                                                   TimeSeriesGuarantee::deduplicated>>;

    CHECK(DefaultReference::type().output()->guarantee().value() == TimeSeriesGuarantee::ordinary);
    CHECK(ExplicitReference::type().output()->guarantee().value() == TimeSeriesGuarantee::deduplicated);
}

TEST_CASE("graph signature interning is independent of structural patterns",
          "[feature:operator][feature:type-system][feature:graph-argument]") {
    using Prototype = auto(TS<i64, TimeSeriesGuarantee::ordinary>) -> TS<i64, TimeSeriesGuarantee::ordinary>;
    using Unpatterned = GRAPH<Prototype>;
    using Patterned = GRAPH<Prototype, unary_identity_pattern>;

    CHECK(&Unpatterned::type() == &graph_argument_type(Unpatterned::type().parameters(),
                                                       Unpatterned::type().output()));
    CHECK(&Patterned::type() == &Unpatterned::type());

    const auto first_structure = identity_structure<boundary_node>();
    const auto second_structure = identity_structure<other_boundary_node>();
    const auto first_argument = argument(first_structure);
    const auto second_argument = argument(second_structure);
    CHECK(&first_argument.exact_type() == &second_argument.exact_type());
}

TEST_CASE("graph types resolve scalar parameters with the same variables as time series parameters",
          "[feature:operator][feature:type-system][feature:graph-argument][scalar:integer]") {
    using VALUE = ScalarVariable<"VALUE">;
    using Function = GRAPH<auto(VALUE, TS<VALUE>) -> TS<VALUE>>;

    const auto &supplied = graph_argument_type(
        std::array<const Type *, 2>{&cpp_scalar_type<i64>(), &TS<i64>::type()}, &TS<i64>::type());
    auto type_map = TypeMap{};

    REQUIRE(Function::type().match(supplied, type_map));
    CHECK(std::get<const Type *>(*type_map.find(VALUE::type())) == &cpp_scalar_type<i64>());
}

TEST_CASE("repeated variables in graph types reject inconsistent graph signatures",
          "[feature:operator][feature:type-system][feature:graph-argument]") {
    using VALUE = ScalarVariable<"VALUE">;
    using Function = GRAPH<auto(TS<VALUE>, TS<VALUE>) -> TS<VALUE>>;

    const auto &mixed = graph_argument_type(
        std::array<const Type *, 2>{&TS<i64>::type(), &TS<float>::type()}, &TS<i64>::type());
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

TEST_CASE("callable graph time series unions accept each form and preserve the supplied point",
          "[feature:wiring][feature:type-system][feature:graph-argument][feature:wiring-materialisation]") {
    STATIC_CHECK(details::graph_time_series_union<GRAPH_INPUT_FORM>::value);
    STATIC_CHECK_FALSE(details::graph_time_series_union<Union<"INVALID_GRAPH_INPUT", TS<i64>, i64>>::value);

    using namespace bg::literals;
    auto builder_calls = 0;
    auto records = std::vector<const details::WiringRecord *>{};
    auto types = std::vector<const TimeSeriesType *>{};
    auto option_targets = std::vector<const TimeSeriesType *>{};

    const auto inspect = graph("name"_n = "inspect_union_input", [&](wp<GRAPH_INPUT_FORM> input) {
        records.push_back(static_cast<WiringPoint>(input).record());
        types.push_back(&input.type());
        const auto options = input.materialisation_options();
        option_targets.push_back(options.empty() ? nullptr : options[0].target_type);
    });

    auto wiring = GraphWiring{};
    const auto context = WiringContext{wiring};

    const auto source = node<boundary_node>();
    const auto value = source();
    auto deferred = wiring.create_wp<TS<i64>>(value);
    deferred.materialisation_option<TSB<graph_value_bundle>>(
        [&builder_calls](wp<TS<i64>> input) {
            ++builder_calls;
            return static_cast<WiringPoint>(bundle<TSB<graph_value_bundle>>("value"_n = input));
        },
        0.5);
    const auto bundle_value = bundle<TSB<graph_value_bundle>>("value"_n = value);
    const auto deferred_record = static_cast<WiringPoint>(deferred).record();
    const auto bundle_record = static_cast<WiringPoint>(bundle_value).record();

    inspect(deferred);
    inspect(bundle_value);

    REQUIRE(records.size() == 2);
    CHECK(records[0] == deferred_record);
    CHECK(records[1] == bundle_record);
    CHECK(types[0] == &TS<i64>::type());
    CHECK(type_cast<TimeSeriesBundleType>(types[1]) != nullptr);
    CHECK(option_targets[0] == &TSB<graph_value_bundle>::type());
    CHECK(option_targets[1] == nullptr);
    CHECK(builder_calls == 0);
}

TEST_CASE("callable graph union names share variables inside their alternatives",
          "[feature:wiring][feature:type-system][feature:graph-argument]") {
    const auto inspect = graph("name"_n = "inspect_union_variables",
                               [](wp<GRAPH_LEFT_FORM>, wp<GRAPH_RIGHT_FORM>) {});

    const auto integer_source = node<boundary_node>();

    const auto floating_source = node<float_boundary_node>();

    testing::test_wiring([&] {
        const auto left = integer_source();
        const auto right = integer_source();
        inspect(left, right);
    });

    testing::test_wiring([&] {
        const auto left = integer_source();
        const auto right = floating_source();
        CHECK_THROWS_AS(inspect(left, right), TypeResolutionError);
    });
}

TEST_CASE("callable graph labels validate time series union alternatives",
          "[feature:wiring][feature:type-system][feature:graph-argument]") {
    using LABEL_FORM = Union<"LABEL_FORM", TS<i64>, TS<double>>;
    auto label_calls = 0;

    const auto inspect = graph(
        "name"_n = "inspect_union_label", "label"_n = [&label_calls](wp<LABEL_FORM>) {
            ++label_calls;
            return "selected";
        },
        [](wp<TS<bool>>) {});

    const auto source = node([](Out<TS<bool>>) {});

    testing::test_wiring([&] { CHECK_THROWS_AS(inspect(source()), WiringError); });
    CHECK(label_calls == 0);
}

TEST_CASE("aggregate graph time series unions accept each declared form",
          "[feature:wiring][feature:type-system][feature:graph-argument][definition:aggregate]") {
    struct union_graph {
        wp<GRAPH_INPUT_FORM> input;

        void wire() const {
            if (!input.type().is_time_series()) {
                throw std::logic_error{"the graph input is not a time series"};
            }
        }
    };

    using namespace bg::literals;

    const auto inspect = graph<union_graph>();

    const auto source = node<boundary_node>();

    testing::test_wiring([&] { inspect(source()); });
    testing::test_wiring([&] {
        const auto value = source();
        inspect(bundle<TSB<graph_value_bundle>>("value"_n = value));
    });
}

TEST_CASE("graph patterns constrain graph parameters during matching",
          "[feature:operator][feature:type-system][feature:graph-argument]") {
    using Addition = GRAPH<auto(TS<i64>, TS<i64>) -> TS<i64>, addition_pattern>;

    const auto add = argument(binary_structure<add_node>());
    const auto multiply = argument(binary_structure<multiply_node>());

    CHECK(Addition::type().accepts(add.exact_type()));
    CHECK(Addition::constraint()->matches(add));
    CHECK_FALSE(Addition::constraint()->matches(multiply));
}

TEST_CASE("graph structure diagnostics retain the reason for identical graph types",
          "[feature:operator][feature:wiring][feature:graph-argument]") {
    const auto expected_structure = binary_structure<add_node>();
    const auto supplied = argument(binary_structure<multiply_node>());
    const auto &graph_type = graph_argument_type(expected_structure);
    auto selected = std::string{};
    auto operation = Operator{Signature{
        util::symbol{"identity"}, {{util::symbol{"function"}, &graph_type}}}};
    operation.overload(GraphOverload{util::symbol{"identity"}, expected_structure, false, selected, nullptr,
                                     graph_type});

    try {
        testing::test_wiring([&] { (void)operation(supplied); });
        FAIL("expected the graph structure constraint to reject the supplied graph");
    } catch (const WiringError &error) {
        REQUIRE(error.diagnostic());
        REQUIRE(error.diagnostic()->candidates.size() == 1);
        const auto &failure = error.diagnostic()->candidates.front();
        CHECK(failure.expected == &graph_type);
        CHECK(failure.supplied == &graph_type);
        CHECK(std::string{error.what()}.contains("graph structure does not match the declared graph constraint"));
    }
}

TEST_CASE("graph pattern constraints run only after graph signature matching",
          "[feature:operator][feature:type-system][feature:graph-argument]") {
    using Constrained = GRAPH<auto(TS<i64>) -> TS<i64>, counted_unary_pattern>;
    auto parameter = Parameter{util::symbol{"function"}, &Constrained::type()};
    parameter.graph_constraint = Constrained::constraint();
    const auto signature = Signature{util::symbol{"constrained"}, {std::move(parameter)}};

    const auto wrong_signature = argument(identity_structure<float_boundary_node>());
    const auto mismatched_structure = argument(unary_add_structure());
    const auto wrong_arguments = std::array<WiringArgument, 1>{WiringArgument{wrong_signature}};
    const auto mismatched_arguments = std::array<WiringArgument, 1>{WiringArgument{mismatched_structure}};

    unary_pattern_calls = 0;
    CHECK(signature.match_wiring_arguments(wrong_arguments).empty());
    CHECK(unary_pattern_calls == 0);
    CHECK_THROWS_AS(signature.match_wiring_arguments(mismatched_arguments), TypeResolutionError);
    CHECK(unary_pattern_calls == 1);
}

TEST_CASE("graph types with the same pattern retain signature specificity",
          "[feature:operator][feature:type-system][feature:graph-argument]") {
    using VALUE = ScalarVariable<"VALUE">;
    using Generic = GRAPH<auto(TS<VALUE>, TS<VALUE>) -> TS<VALUE>, binary_first_pattern>;
    using Exact = GRAPH<auto(TS<i64>, TS<i64>) -> TS<i64>, binary_first_pattern>;

    auto generic_parameter = Parameter{util::symbol{"function"}, &Generic::type()};
    generic_parameter.graph_constraint = Generic::constraint();
    const auto generic = Signature{util::symbol{"generic"}, {std::move(generic_parameter)}};
    auto exact_parameter = Parameter{util::symbol{"function"}, &Exact::type()};
    exact_parameter.graph_constraint = Exact::constraint();
    const auto exact = Signature{util::symbol{"exact"}, {std::move(exact_parameter)}};

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
        CHECK(&result.type() == &TS<i64>::type());
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

        CHECK(&operation(function, integer).type() == &TS<i64>::type());
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

    (void)wiring.complete_pending_wiring();
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
    auto operation = operator_<auto(GraphArgument) -> TS<i64>>{"apply_graph", {"function"}};
    operation.overload(implementation);

    testing::test_wiring([&] {
        const auto result = operation(argument(identity_structure()));
        CHECK(&result.type() == &TS<i64>::type());
    });
}

TEST_CASE("graph patterns are less generic than any graph and distinct patterns are unordered",
          "[feature:operator][feature:type-system][feature:graph-argument]") {
    const auto add_node_template = node<add_node>();

    const auto multiply_node_template = node<multiply_node>();

    const auto add_graph = graph("name"_n = "add_pattern", [add_node_template](wp<TS<i64>> left, wp<TS<i64>> right) {
        return add_node_template(left, right);
    });

    const auto multiply_graph = graph("name"_n = "multiply_pattern",
                                      [multiply_node_template](wp<TS<i64>> left, wp<TS<i64>> right) {
        return multiply_node_template(left, right);
    });
    auto add_parameter = Parameter{util::symbol{"function"}, &GraphArgument::type()};
    add_parameter.graph_constraint = std::make_shared<GraphArgumentConstraint>(
        std::make_shared<const decltype(add_graph)>(add_graph));
    auto multiply_parameter = Parameter{util::symbol{"function"}, &GraphArgument::type()};
    multiply_parameter.graph_constraint = std::make_shared<GraphArgumentConstraint>(
        std::make_shared<const decltype(multiply_graph)>(multiply_graph));
    const auto add_signature = Signature{util::symbol{"add"}, {std::move(add_parameter)}};
    const auto multiply_signature = Signature{util::symbol{"multiply"}, {std::move(multiply_parameter)}};
    const auto any_signature =
        Signature{util::symbol{"any"}, {{util::symbol{"function"}, &GraphArgument::type()}}};
    auto empty_constraint_parameter = Parameter{util::symbol{"function"}, &GraphArgument::type()};
    empty_constraint_parameter.graph_constraint = std::make_shared<GraphArgumentConstraint>();
    const auto empty_constraint_signature =
        Signature{util::symbol{"empty_constraint"}, {std::move(empty_constraint_parameter)}};

    CHECK(compare_specificity(add_signature, any_signature) == std::partial_ordering::less);
    CHECK(compare_specificity(any_signature, add_signature) == std::partial_ordering::greater);
    CHECK(compare_specificity(add_signature, multiply_signature) == std::partial_ordering::unordered);
    CHECK(compare_specificity(any_signature, empty_constraint_signature) == std::partial_ordering::equivalent);
}

TEST_CASE("an output operator selects source, value, and sink graph overloads",
          "[feature:operator][feature:wiring][feature:graph-argument]") {
    auto selected = std::string{};
    auto operation = Operator{
        Signature{util::symbol{"map"}, {{util::symbol{"function"}, &GraphArgument::type()}}, TS_T::type()}};
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
    auto operation = Operator{
        Signature{util::symbol{"map"}, {{util::symbol{"function"}, &GraphArgument::type()}}, TS_T::type()}};
    operation.overload(GraphOverload{util::symbol{"generic"}, true, selected, &requirements});

    testing::test_wiring([&] {
        (void)operation(argument(identity_structure<boundary_node>()));
        (void)operation(argument(identity_structure<other_boundary_node>()));
    });
    CHECK(requirements == 1);

    const auto captured_one = captured_integer_structure(1);
    const auto captured_two = captured_integer_structure(2);
    CHECK_FALSE(captured_one == captured_two);
    const auto captured_one_argument = argument(captured_one);
    const auto captured_two_argument = argument(captured_two);
    CHECK(&captured_one_argument.exact_type() == &captured_two_argument.exact_type());
    testing::test_wiring([&] {
        (void)operation(argument(captured_one));
        (void)operation(argument(captured_one));
        (void)operation(argument(captured_two));
        (void)operation(argument(captured_two));
    });
    CHECK(requirements == 3);

    testing::test_wiring([&] {
        (void)operation(argument(uncacheable_structure()));
        (void)operation(argument(uncacheable_structure()));
        const auto uncacheable = uncacheable_structure();
        (void)operation(argument(std::vector<details::GraphWiringStructure>{identity_structure(), uncacheable}));
        (void)operation(argument(std::vector<details::GraphWiringStructure>{identity_structure(), uncacheable}));
        (void)operation(argument(std::vector<details::GraphWiringStructure>{}));
        (void)operation(argument(std::vector<details::GraphWiringStructure>{}));
    });
    CHECK(requirements == 9);
}

TEST_CASE("root GraphWiring owns cached graph scalar copies through nested wiring",
          "[feature:operator][feature:wiring][feature:graph-argument][scalar:opaque][shape:nested]") {
    auto lifetime = std::weak_ptr<int>{};
    auto selected = std::string{};
    auto requirements = 0;
    auto operation = Operator{
        Signature{util::symbol{"map"}, {{util::symbol{"function"}, &GraphArgument::type()}}, TS_T::type()}};
    operation.overload(GraphOverload{util::symbol{"generic"}, true, selected, &requirements});

    {
        auto root = GraphWiring{};
        {
            auto nested = GraphWiring{root};
            const auto context = WiringContext{nested};
            auto captured = std::make_shared<int>(7);
            lifetime = captured;
            auto structure = captured_shared_pointer_structure(captured);
            REQUIRE(structure.cacheable());

            (void)operation(argument(std::move(structure)));
            CHECK(requirements == 1);
            captured.reset();
        }

        CHECK_FALSE(lifetime.expired());
    }

    CHECK(lifetime.expired());
}

TEST_CASE("graph signature constraints own captured scalars without registry retention",
          "[feature:wiring][feature:type-system][feature:graph-argument]") {
    auto lifetime = std::weak_ptr<int>{};
    auto signature = std::unique_ptr<Signature>{};

    {
        auto captured = std::make_shared<int>(7);
        lifetime = captured;
        const auto structure = captured_shared_pointer_structure(captured);
        auto graph_value = argument(structure);
        auto parameter = Parameter{util::symbol{"function"}, &GraphArgument::type()};
        parameter.graph_constraint =
            std::make_shared<GraphArgumentConstraint>(structure);
        signature = std::make_unique<Signature>(util::symbol{"captured"},
                                                std::vector<Parameter>{std::move(parameter)});

        captured.reset();
        CHECK_FALSE(lifetime.expired());
    }

    CHECK_FALSE(lifetime.expired());
    signature.reset();
    CHECK(lifetime.expired());
}

TEST_CASE("named scalar arguments remain alive through graph wiring",
          "[feature:wiring][feature:graph-argument][scalar:opaque][lifetime:retained]") {
    const auto source = node<captured_shared_pointer_node>();
    auto wiring = GraphWiring{};
    auto rvalue_lifetime = std::weak_ptr<int>{};
    auto lvalue_lifetime = std::weak_ptr<int>{};
    auto const_lvalue_lifetime = std::weak_ptr<int>{};

    {
        auto context = WiringContext{wiring};

        {
            auto owner = std::make_shared<int>(1);
            rvalue_lifetime = owner;
            (void)source("value"_n = run_graph_argument::captured_pointer{owner});
        }

        {
            auto owner = std::make_shared<int>(2);
            lvalue_lifetime = owner;
            auto named = "value"_n = run_graph_argument::captured_pointer{owner};
            (void)source(named);
        }

        {
            auto owner = std::make_shared<int>(3);
            const_lvalue_lifetime = owner;
            const auto named = "value"_n = run_graph_argument::captured_pointer{owner};
            (void)source(named);
        }
    }

    (void)wiring.complete_pending_wiring();
    CHECK_FALSE(rvalue_lifetime.expired());
    CHECK_FALSE(lvalue_lifetime.expired());
    CHECK_FALSE(const_lvalue_lifetime.expired());
}

TEST_CASE("named built-in scalar arguments preserve their name and value",
          "[feature:wiring][feature:graph-argument][scalar:native]") {
    auto mutable_named = "default"_n = i64{20};
    const auto const_named = "default"_n = i64{21};

    const auto mutable_argument = WiringArgument{mutable_named};
    const auto const_argument = WiringArgument{const_named};
    const auto rvalue_argument = WiringArgument{"default"_n = i64{22}};

    CHECK(mutable_argument.name() == std::optional{util::symbol{"default"}});
    CHECK(const_argument.name() == std::optional{util::symbol{"default"}});
    CHECK(rvalue_argument.name() == std::optional{util::symbol{"default"}});
    CHECK(mutable_argument.type().accepts(cpp_scalar_type<i64>()));
    CHECK(const_argument.type().accepts(cpp_scalar_type<i64>()));
    CHECK(rvalue_argument.type().accepts(cpp_scalar_type<i64>()));
    CHECK(scalar_cast<i64>(mutable_argument.scalar()).value() == 20);
    CHECK(scalar_cast<i64>(const_argument.scalar()).value() == 21);
    CHECK(scalar_cast<i64>(rvalue_argument.scalar()).value() == 22);
}

TEST_CASE("typed bundles retain named scalar sources through construction",
          "[feature:wiring][feature:graph-argument][series:tsb][schema:named][lifetime:retained]") {
    auto wiring = GraphWiring{};
    auto lifetime = std::weak_ptr<int>{};

    {
        auto context = WiringContext{wiring};
        auto owner = std::make_shared<int>(7);
        lifetime = owner;
        (void)bundle<graph_shared_pointer_bundle>(
            "value"_n = run_graph_argument::captured_pointer{owner});
    }

    (void)wiring.complete_pending_wiring();
    CHECK_FALSE(lifetime.expired());
}

TEST_CASE("free TSB bundles retain named scalar sources through construction",
          "[feature:wiring][feature:graph-argument][series:tsb][schema:named][lifetime:retained]") {
    auto wiring = GraphWiring{};
    auto lifetime = std::weak_ptr<int>{};
    auto retained = std::optional<WiringPoint>{};

    {
        auto context = WiringContext{wiring};
        auto owner = std::make_shared<int>(11);
        lifetime = owner;
        retained.emplace(bundle("value"_n = run_graph_argument::captured_pointer{owner}));
    }

    (void)wiring.complete_pending_wiring();
    REQUIRE(retained.has_value());
    const auto fields = retained->record()->collection_original_arguments();
    REQUIRE(fields.has_value());
    REQUIRE(fields->size() == 1);
    const auto value = fields->front().scalar();
    CHECK(*scalar_cast<run_graph_argument::captured_pointer>(value).value().value == 11);
    REQUIRE_FALSE(lifetime.expired());
    CHECK(*lifetime.lock() == 11);
}

TEST_CASE("graph var_kwargs retain named scalar arguments after the caller returns",
          "[feature:wiring][feature:variadic-wiring][series:tsb][schema:named][lifetime:retained]") {
    auto wiring = GraphWiring{};
    auto lifetime = std::weak_ptr<int>{};
    auto retained = std::optional<WiringPoint>{};
    const auto capture = graph("name"_n = "retain_named_scalar_kwargs",
                               [&](wp<TSB<SCHEMA>, "values"_n, var_kwargs> values) {
                                   retained.emplace(static_cast<WiringPoint>(values));
                               });

    {
        auto context = WiringContext{wiring};
        auto owner = std::make_shared<int>(13);
        lifetime = owner;
        const auto named = "payload"_n = run_graph_argument::captured_pointer{owner};
        (void)capture(named);
    }

    (void)wiring.complete_pending_wiring();
    REQUIRE(retained.has_value());
    const auto fields = retained->record()->collection_original_arguments();
    REQUIRE(fields.has_value());
    REQUIRE(fields->size() == 1);
    REQUIRE(fields->front().name());
    CHECK(fields->front().name()->text() == "payload");
    const auto value = fields->front().scalar();
    CHECK(*scalar_cast<run_graph_argument::captured_pointer>(value).value().value == 13);
    REQUIRE_FALSE(lifetime.expired());
    CHECK(*lifetime.lock() == 13);
}

TEST_CASE("operator diagnostics retain graph argument types",
          "[feature:operator][feature:wiring][feature:graph-argument]") {
    auto operation = Operator{
        Signature{util::symbol{"map"}, {{util::symbol{"function"}, &GraphArgument::type()}}, TS_T::type()}};
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
        "function: GRAPH<auto(TS<i64>) -> TS<i64>>"));
}

TEST_CASE("variadic parameters reject graph arguments",
          "[feature:wiring][feature:graph-argument][feature:variadic-wiring][series:tsb][schema:generic]") {
    const auto variadic = graph("name"_n = "variadic", [](wp<TSB<SCHEMA>, var_args>) {});

    CHECK_THROWS_WITH(testing::test_wiring([&] { variadic(argument(identity_structure())); }),
                      "a variadic parameter cannot capture a graph argument");
}

TEST_CASE("a graph argument cannot become node runtime storage",
          "[feature:operator][feature:wiring][feature:graph-argument]") {
    auto wiring = GraphWiring{};
    {
        const auto context = WiringContext{wiring};
        const auto invalid = GraphNode{};
        CHECK_THROWS_WITH(invalid(argument(identity_structure())),
                          Catch::Matchers::ContainsSubstring("a graph argument cannot be stored by a node"));
    }

    CHECK_NOTHROW(wiring.finalize());
    CHECK(wiring.nodes().empty());
}
