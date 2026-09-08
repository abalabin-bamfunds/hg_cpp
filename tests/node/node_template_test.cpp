#include <bg/node.hpp>
#include <bg/testing/test_graph.hpp>
#include <bg/type/scalar_type.hpp>
#include <bg/type/time_series_list_type.hpp>
#include <bg/type/time_series_value_type.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <concepts>
#include <optional>
#include <type_traits>
#include <utility>

namespace {

using namespace bg::literals;

struct weighted_add {
    bg::In<bg::TS<float>> lhs;
    bg::In<bg::TS<float>, bg::optional, bg::passive> rhs;
    bg::Out<bg::TS<float>> output;

    void evaluate() const { output.set(*lhs + *rhs); }
};

static_assert(bg::AggregateNodeClass<weighted_add>);

template <typename Template>
concept TemporaryWeightedAddCall = requires(Template &&node, bg::WiringPoint input) { std::move(node)(input, input); };

static_assert(!TemporaryWeightedAddCall<decltype(bg::node<weighted_add>())>);

} // namespace

TEST_CASE("an aggregate node class generates a signature", "[node_template][feature:node-api][definition:aggregate]") {
    const auto weighted_add_node = bg::node<weighted_add>();
    static_assert(std::derived_from<std::remove_cv_t<decltype(weighted_add_node)>, bg::NodeTemplate>);

    const auto &ts_final = bg::TS<float, bg::TimeSeriesGuarantee::final>::type();
    const auto &ts_deduplicated = bg::TS<float, bg::TimeSeriesGuarantee::deduplicated>::type();
    const std::array<const bg::Type *, 2> arguments{&ts_final, &ts_deduplicated};

    const bg::ResolvedSignature signature = weighted_add_node.signature().resolve(arguments);

    REQUIRE(signature.parameters.size() == 2);
    CHECK(signature.name.text() == "weighted_add");
    CHECK(signature.parameters[0].name.text() == "lhs");
    CHECK(signature.parameters[0].type == &ts_final);
    CHECK(signature.parameters[0].activity == bg::active);
    CHECK(signature.parameters[0].validity == bg::required);
    CHECK(signature.parameters[1].name.text() == "rhs");
    CHECK(signature.parameters[1].type == &ts_deduplicated);
    CHECK(signature.parameters[1].activity == bg::passive);
    CHECK(signature.parameters[1].validity == bg::optional);
    CHECK(signature.output == &bg::TS<float>::type());
}

TEST_CASE("a callable node receives typed inputs, scalars, and output",
          "[node_template][feature:node-api][definition:function]") {
    const auto factor = 2;
    const auto add = bg::node(
        [factor](bg::In<bg::TS<int>, "input"_n> input, int offset, bg::Out<bg::TS<int>> output) {
            output.set(*input * factor + offset);
        });
    const auto graph = [&](bg::wp<bg::TS<int>> input) { return add(input, 1); };

    CHECK(bg::testing::test_graph(graph, {1, 2, 3}).equals({3, 5, 7}));
}

TEST_CASE("a callable node can omit its output", "[node_template][feature:node-api][definition:function]") {
    auto sum = 0;
    const auto sink = bg::node([&](bg::In<bg::TS<int>> input) { sum += *input; });
    const auto graph = [&](bg::wp<bg::TS<int>> input) {
        sink(input);
        return input;
    };

    STATIC_CHECK(std::same_as<decltype(sink(std::declval<bg::WiringPoint>())), void>);
    CHECK(bg::testing::test_graph(graph, {1, 2, 3}).equals({1, 2, 3}));
    CHECK(sum == 6);
}

namespace {

using KeyScalar = bg::ScalarVariable<"Key", bg::EqualityComparable, bg::Hashable>;

struct key_sink {
    bg::Scalar<KeyScalar> key;

    void evaluate() const {
        static_cast<void>(key == key);
        static_cast<void>(key->hash());
    }
};

static_assert(bg::AggregateNodeClass<key_sink>);

} // namespace

TEST_CASE("a scalar variable enforces required operations", "[node_template][feature:node-api]") {
    const auto key_sink_node = bg::node<key_sink>();
    const std::array<const bg::Type *, 1> hashable_argument{&bg::int64_type()};
    const auto signature = key_sink_node.signature().resolve(hashable_argument);

    CHECK(signature.parameters[0].type == &bg::int64_type());

    const auto &opaque_type = bg::cpp_scalar_type<bg::Opaque<int>>();
    const std::array<const bg::Type *, 1> opaque_argument{&opaque_type};
    CHECK_THROWS_AS(key_sink_node.signature().resolve(opaque_argument), bg::TypeResolutionError);
}

TEST_CASE("an aggregate node template exposes its polymorphic interface",
          "[node_template][feature:node-api][definition:aggregate]") {
    const auto weighted_add_node = bg::node<weighted_add>();
    const bg::NodeTemplate &node_template = weighted_add_node;

    CHECK(&node_template.signature() == &weighted_add_node.signature());
}

namespace {

struct sink {
    bg::In<bg::TS<float>> input;

    void evaluate() const {}
};

static_assert(bg::AggregateNodeClass<sink>);

} // namespace

TEST_CASE("an aggregate node can omit its output", "[node_template][feature:node-api][definition:aggregate]") {
    const auto sink_node = bg::node<sink>();
    static_assert(std::same_as<decltype(sink_node(std::declval<bg::WiringPoint>())), void>);

    const std::array<const bg::Type *, 1> arguments{
        &bg::TS<float>::type()};
    const bg::ResolvedSignature signature = sink_node.signature().resolve(arguments);

    CHECK(signature.output == nullptr);
    CHECK(sink_node.output_runtime_type(signature, {}, {}) == nullptr);
}

namespace {

struct final_identity {
    bg::In<bg::TS<float, bg::TimeSeriesGuarantee::final>> input;
    bg::Out<bg::TS<float, bg::TimeSeriesGuarantee::final>> output;

    void evaluate() const { output.set(*input); }
};

} // namespace

TEST_CASE("an aggregate signature retains specified guarantees",
          "[node_template][feature:node-api][definition:aggregate][guarantee:const][guarantee:final]") {
    const auto final_identity_node = bg::node<final_identity>();
    const auto &ts_const = bg::TS<float, bg::TimeSeriesGuarantee::const_>::type();
    const auto &ts_final = bg::TS<float, bg::TimeSeriesGuarantee::final>::type();
    const std::array<const bg::Type *, 1> arguments{&ts_const};

    const bg::ResolvedSignature signature = final_identity_node.signature().resolve(arguments);

    REQUIRE(signature.parameters.size() == 1);
    CHECK(signature.parameters[0].type == &ts_final);
    CHECK(signature.output == &ts_final);
}

namespace {

struct invocation_state {
    int count{};
};

struct alignas(16) aligned_state {
    int count{};
};

struct scalar_state_node {
    float factor;
    bg::util::symbol name;
    std::optional<int> retries;
    bg::State<invocation_state> state;
    bg::State<aligned_state> aligned;
    bg::Out<bg::TS<float>> output;

    void evaluate() const {
        ++state->count;
        ++aligned->count;
        output.set(factor);
    }
};

static_assert(bg::AggregateNodeClass<scalar_state_node>);

} // namespace

TEST_CASE("aggregate scalar and state fields have distinct roles",
          "[node_template][feature:node-api][definition:aggregate][scalar:symbol]") {
    const auto scalar_state = bg::node<scalar_state_node>();
    const std::array<const bg::Type *, 3> arguments{&bg::float_type(), &bg::symbol_type(), &bg::int_type()};
    const bg::ResolvedSignature signature = scalar_state.signature().resolve(arguments);

    REQUIRE(signature.parameters.size() == 3);
    CHECK(signature.parameters[0].name.text() == "factor");
    CHECK(signature.parameters[0].type == &bg::float_type());
    CHECK(signature.parameters[0].validity == bg::required);
    CHECK(signature.parameters[1].name.text() == "name");
    CHECK(signature.parameters[1].type == &bg::symbol_type());
    CHECK(signature.parameters[2].name.text() == "retries");
    CHECK(signature.parameters[2].type == &bg::int_type());
    CHECK(signature.parameters[2].accepts_absent_scalar);
    const auto layout = scalar_state.state_layout(signature, {}, {});
    CHECK(layout.size == alignof(aligned_state) + sizeof(aligned_state));
    CHECK(layout.alignment == alignof(aligned_state));
}

namespace {

using LeftScalar = bg::ScalarVariable<"Left">;
using RightScalar = bg::ScalarVariable<"Right">;

struct generic_pair {
    bg::In<bg::TS<LeftScalar>> first;
    bg::In<bg::TS<LeftScalar>> second;
    bg::In<bg::TS<RightScalar>> other;
    bg::Out<bg::TS<LeftScalar>> output;

    void evaluate() const { output.set_value(first.value_view()); }
};

static_assert(bg::AggregateNodeClass<generic_pair>);

} // namespace

TEST_CASE("scalar variable declarations retain their identity", "[node_template][feature:node-api]") {
    const auto generic_pair_node = bg::node<generic_pair>();
    const auto &ts_float = bg::TS<float>::type();
    const auto &ts_double = bg::TS<double>::type();
    const std::array<const bg::Type *, 3> arguments{&ts_float, &ts_float, &ts_double};

    const auto signature = generic_pair_node.signature().resolve(arguments);

    CHECK(signature.parameters[0].type == &ts_float);
    CHECK(signature.parameters[1].type == &ts_float);
    CHECK(signature.parameters[2].type == &ts_double);
    CHECK(signature.output == &ts_float);
}

namespace {

inline constexpr auto captured_guarantee = bg::guarantee_variable<"G">;
using CapturedTimeSeries = bg::TimeSeriesVariable<"T">;

struct time_series_variable_guarantee {
    bg::In<CapturedTimeSeries::with<captured_guarantee>> input;
    bg::Out<bg::TS<int, captured_guarantee>> output;

    void evaluate() const {}
};

static_assert(bg::AggregateNodeClass<time_series_variable_guarantee>);

struct generic_guarantee_collection {
    bg::In<bg::TS<int, captured_guarantee>> input;
    bg::Out<bg::TSL<bg::TS<int>, 2, captured_guarantee>> output;

    void evaluate() const {}
};

static_assert(bg::AggregateNodeClass<generic_guarantee_collection>);

} // namespace

TEST_CASE("a time series variable can expose its guarantee as another variable",
          "[node_template][type_system][feature:node-api]") {
    const auto node = bg::node<time_series_variable_guarantee>();
    const auto &list =
        bg::TSL<bg::TS<float, bg::TimeSeriesGuarantee::ordinary>, 2, bg::TimeSeriesGuarantee::final>::type();
    const auto arguments = std::array<const bg::Type *, 1>{&list};

    const auto signature = node.signature().resolve(arguments);

    CHECK(signature.parameters[0].type == &list);
    CHECK(signature.output == &bg::TS<int, bg::TimeSeriesGuarantee::final>::type());
}

TEST_CASE("a collection output uses its resolved guarantee", "[node_template][type_system][feature:node-api]") {
    const auto node = bg::node<generic_guarantee_collection>();
    const auto &input = bg::TS<int, bg::TimeSeriesGuarantee::final>::type();
    const auto arguments = std::array<const bg::Type *, 1>{&input};
    const auto signature = node.signature().resolve(arguments);

    const auto *runtime_type = node.output_runtime_type(signature, {}, {});

    REQUIRE(runtime_type != nullptr);
    CHECK(&runtime_type->type() == signature.output);
}

namespace {

struct generic_scalar_source {
    bg::Scalar<bg::SCALAR> value;
    bg::Out<bg::TS<bg::SCALAR>> output;

    void evaluate() const { output.set_value(value); }
};

static_assert(bg::AggregateNodeClass<generic_scalar_source>);

} // namespace

TEST_CASE("a scalar variable can declare a scalar view field", "[node_template][feature:node-api]") {
    const auto generic_scalar_source_node = bg::node<generic_scalar_source>();
    const std::array<const bg::Type *, 1> arguments{&bg::float_type()};

    const auto signature = generic_scalar_source_node.signature().resolve(arguments);

    CHECK(signature.parameters[0].type == &bg::float_type());
    CHECK(signature.output == &bg::TS<float>::type());
}

namespace {

struct runtime_layout_collection_sink {
    bg::In<bg::TSS<int>> set;
    bg::In<bg::TSD<int, bg::TS<int>>> dictionary;

    void evaluate() const {}
};

} // namespace

TEST_CASE("collection inputs retain their resolved runtime layout", "[node_template][time_series][feature:node-api]") {
    const auto sink = bg::node<runtime_layout_collection_sink>();

    CHECK(sink.uses_time_series_input_runtime_type(0));
    CHECK(sink.uses_time_series_input_runtime_type(1));
}

namespace {

using NumericScalar = bg::ScalarVariable<"Numeric", float, double>;

struct numeric_sink {
    bg::In<bg::TS<NumericScalar>> input;

    void evaluate() const {}
};

static_assert(bg::AggregateNodeClass<numeric_sink>);

} // namespace

TEST_CASE("a scalar variable declaration constrains its resolved type", "[node_template][feature:node-api]") {
    const auto numeric_sink_node = bg::node<numeric_sink>();
    const auto &ts_double = bg::TS<double>::type();
    const auto &ts_int = bg::TS<int>::type();
    const std::array<const bg::Type *, 1> accepted{&ts_double};
    const std::array<const bg::Type *, 1> rejected{&ts_int};

    CHECK(numeric_sink_node.signature().resolve(accepted).parameters[0].type == &ts_double);
    CHECK_THROWS_AS(numeric_sink_node.signature().resolve(rejected), bg::TypeResolutionError);
}

namespace {

struct repeated_output {
    bg::Out<bg::TS<float>> first;
    bg::Out<bg::TS<float>> second;

    void evaluate() const {}
};

struct returned_output {
    bg::Out<bg::TS<float>> output;

    float evaluate() { return 0.0F; }
};

struct repeated_input_name {
    bg::In<bg::TS<float>, "input"_n> input;
    bg::Out<bg::TS<float>> output;

    void evaluate() const {}
};

struct int_identity {
    bg::In<bg::TS<int>> input;
    bg::Out<bg::TS<int>> output;

    void evaluate() const { output.set(*input); }
};

} // namespace

TEST_CASE("aggregate node validation rejects ambiguous output definitions",
          "[node_template][feature:node-api][definition:aggregate]") {
    STATIC_CHECK_FALSE(bg::AggregateNodeClass<repeated_output>);
    STATIC_CHECK_FALSE(bg::AggregateNodeClass<returned_output>);
    STATIC_CHECK_FALSE(bg::AggregateNodeClass<repeated_input_name>);
    STATIC_CHECK(bg::AggregateNodeClass<int_identity>);
}
