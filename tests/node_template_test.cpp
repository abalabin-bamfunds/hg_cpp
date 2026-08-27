#include <bg/node.hpp>
#include <bg/type/scalar_type.hpp>
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

    void evaluate() const { output.set(lhs.value() + rhs.value()); }
};

struct sink {
    bg::In<bg::TS<float>> input;

    void evaluate() const {}
};

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

    void evaluate() const { output.set(input.value()); }
};

struct final_identity {
    bg::In<bg::TS<float, bg::TimeSeriesGuarantee::final>> input;
    bg::Out<bg::TS<float, bg::TimeSeriesGuarantee::final>> output;

    void evaluate() const { output.set(input.value()); }
};

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

using LeftScalar = bg::ScalarVariable<"Left">;
using RightScalar = bg::ScalarVariable<"Right">;
using NumericScalar = bg::ScalarVariable<"Numeric", float, double>;
using KeyScalar = bg::ScalarVariable<"Key", bg::EqualityComparable, bg::Hashable>;

struct generic_pair {
    bg::In<bg::TS<LeftScalar>> first;
    bg::In<bg::TS<LeftScalar>> second;
    bg::In<bg::TS<RightScalar>> other;
    bg::Out<bg::TS<LeftScalar>> output;

    void evaluate() const { output.set_value(first.value_view()); }
};

struct generic_scalar_source {
    bg::Scalar<bg::SCALAR> value;
    bg::Out<bg::TS<bg::SCALAR>> output;

    void evaluate() const { output.set_value(value); }
};

struct numeric_sink {
    bg::In<bg::TS<NumericScalar>> input;

    void evaluate() const {}
};

struct key_sink {
    bg::Scalar<KeyScalar> key;

    void evaluate() const {
        static_cast<void>(key == key);
        static_cast<void>(key->hash());
    }
};

static_assert(bg::AggregateNodeClass<weighted_add>);
static_assert(bg::AggregateNodeClass<sink>);
static_assert(!bg::AggregateNodeClass<repeated_output>);
static_assert(!bg::AggregateNodeClass<returned_output>);
static_assert(!bg::AggregateNodeClass<repeated_input_name>);
static_assert(bg::AggregateNodeClass<int_identity>);
static_assert(bg::AggregateNodeClass<scalar_state_node>);
static_assert(bg::AggregateNodeClass<generic_pair>);
static_assert(bg::AggregateNodeClass<generic_scalar_source>);
static_assert(bg::AggregateNodeClass<numeric_sink>);
static_assert(bg::AggregateNodeClass<key_sink>);

inline const auto weighted_add_node = bg::node<weighted_add>();
inline const auto final_identity_node = bg::node<final_identity>();
inline const auto sink_node = bg::node<sink>();
inline const auto scalar_state = bg::node<scalar_state_node>();
inline const auto generic_pair_node = bg::node<generic_pair>();
inline const auto generic_scalar_source_node = bg::node<generic_scalar_source>();
inline const auto numeric_sink_node = bg::node<numeric_sink>();
inline const auto key_sink_node = bg::node<key_sink>();

static_assert(std::same_as<decltype(sink_node(std::declval<bg::WiringPoint>())), void>);

static_assert(std::derived_from<std::remove_cv_t<decltype(weighted_add_node)>, bg::NodeTemplate>);

} // namespace

TEST_CASE("an aggregate node class generates a signature", "[node_template]") {
    const auto &ts_final = bg::time_series_value_type(bg::float_type(), bg::TimeSeriesGuarantee::final);
    const auto &ts_deduplicated = bg::time_series_value_type(bg::float_type(), bg::TimeSeriesGuarantee::deduplicated);
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
    CHECK(signature.output == &bg::time_series_value_type(bg::float_type(), bg::TimeSeriesGuarantee::ordinary));
}

TEST_CASE("a scalar variable enforces required operations", "[node_template]") {
    const std::array<const bg::Type *, 1> hashable_argument{&bg::int64_type()};
    const auto signature = key_sink_node.signature().resolve(hashable_argument);

    CHECK(signature.parameters[0].type == &bg::int64_type());

    const auto &opaque_type = bg::cpp_scalar_type<bg::Opaque<int>>();
    const std::array<const bg::Type *, 1> opaque_argument{&opaque_type};
    CHECK_THROWS_AS(key_sink_node.signature().resolve(opaque_argument), bg::TypeResolutionError);
}

TEST_CASE("an aggregate node template exposes its polymorphic interface", "[node_template]") {
    const bg::NodeTemplate &node_template = weighted_add_node;

    CHECK(&node_template.signature() == &weighted_add_node.signature());
}

TEST_CASE("an aggregate node can omit its output", "[node_template]") {
    const std::array<const bg::Type *, 1> arguments{
        &bg::time_series_value_type(bg::float_type(), bg::TimeSeriesGuarantee::ordinary)};
    const bg::ResolvedSignature signature = sink_node.signature().resolve(arguments);

    CHECK(signature.output == nullptr);
    CHECK(sink_node.output_storage_representation(signature, {}) == nullptr);
}

TEST_CASE("an aggregate signature retains specified guarantees", "[node_template]") {
    const auto &ts_const = bg::time_series_value_type(bg::float_type(), bg::TimeSeriesGuarantee::const_);
    const auto &ts_final = bg::time_series_value_type(bg::float_type(), bg::TimeSeriesGuarantee::final);
    const std::array<const bg::Type *, 1> arguments{&ts_const};

    const bg::ResolvedSignature signature = final_identity_node.signature().resolve(arguments);

    REQUIRE(signature.parameters.size() == 1);
    CHECK(signature.parameters[0].type == &ts_final);
    CHECK(signature.output == &ts_final);
}

TEST_CASE("aggregate scalar and state fields have distinct roles", "[node_template]") {
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
    CHECK(scalar_state.state_size(signature) == alignof(aligned_state) + sizeof(aligned_state));
    CHECK(scalar_state.state_alignment(signature) == alignof(aligned_state));
}

TEST_CASE("scalar variable declarations retain their identity", "[node_template]") {
    const auto &ts_float = bg::time_series_value_type(bg::float_type(), bg::TimeSeriesGuarantee::ordinary);
    const auto &ts_double = bg::time_series_value_type(bg::double_type(), bg::TimeSeriesGuarantee::ordinary);
    const std::array<const bg::Type *, 3> arguments{&ts_float, &ts_float, &ts_double};

    const auto signature = generic_pair_node.signature().resolve(arguments);

    CHECK(signature.parameters[0].type == &ts_float);
    CHECK(signature.parameters[1].type == &ts_float);
    CHECK(signature.parameters[2].type == &ts_double);
    CHECK(signature.output == &ts_float);
}

TEST_CASE("a scalar variable can declare a scalar view field", "[node_template]") {
    const std::array<const bg::Type *, 1> arguments{&bg::float_type()};

    const auto signature = generic_scalar_source_node.signature().resolve(arguments);

    CHECK(signature.parameters[0].type == &bg::float_type());
    CHECK(signature.output == &bg::time_series_value_type(bg::float_type(), bg::TimeSeriesGuarantee::ordinary));
}

TEST_CASE("a scalar variable declaration constrains its resolved type", "[node_template]") {
    const auto &ts_double = bg::time_series_value_type(bg::double_type(), bg::TimeSeriesGuarantee::ordinary);
    const auto &ts_int = bg::time_series_value_type(bg::int_type(), bg::TimeSeriesGuarantee::ordinary);
    const std::array<const bg::Type *, 1> accepted{&ts_double};
    const std::array<const bg::Type *, 1> rejected{&ts_int};

    CHECK(numeric_sink_node.signature().resolve(accepted).parameters[0].type == &ts_double);
    CHECK_THROWS_AS(numeric_sink_node.signature().resolve(rejected), bg::TypeResolutionError);
}
