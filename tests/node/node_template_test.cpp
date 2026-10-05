#include <bg/node.hpp>
#include <bg/testing/test_graph.hpp>
#include <bg/type/scalar_type.hpp>
#include <bg/type/time_series_reference_type.hpp>
#include <bg/type/time_series_list_type.hpp>
#include <bg/type/time_series_value_type.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <concepts>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

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

namespace {
namespace dynamic_input_test {

using ITEM = bg::TimeSeriesVariable<"ITEM">;
inline constexpr auto G = bg::guarantee_variable<"G">;

struct dynamic_reference_input_node {
    bg::In<bg::REF<bg::TSD<std::int64_t, ITEM, G>, G>> dictionary;
    bg::In<bg::REF<ITEM, G>, bg::dynamic_binding, bg::optional, "selected"_n> item;
    bg::Out<bg::REF<ITEM, G>> output;

    void evaluate() const {}
};

} // namespace dynamic_input_test
} // namespace

TEST_CASE("a dynamic reference input is internal to an aggregate node",
          "[node_template][feature:node-api][feature:reference][definition:aggregate]") {
    const auto node = bg::node<dynamic_input_test::dynamic_reference_input_node>();
    const std::array<const bg::Type *, 1> arguments{&bg::TSD<std::int64_t, bg::TS<float>>::type()};

    const auto resolution = node.signature().resolve_with_type_map(arguments);
    const auto dynamic_inputs = node.dynamic_reference_input_types(resolution.type_map);

    REQUIRE(resolution.signature.parameters.size() == 1);
    CHECK(resolution.signature.parameters[0].name.text() == "dictionary");
    REQUIRE(dynamic_inputs.size() == 1);
    CHECK(dynamic_inputs[0] == resolution.signature.output);
    const auto &reference = static_cast<const bg::TimeSeriesReferenceType &>(*dynamic_inputs[0]);
    CHECK(&reference.target_type() == &bg::TS<float>::type());
}

TEST_CASE("a callable node receives typed inputs, scalars, and output",
          "[node_template][feature:node-api][definition:function]") {
    const auto factor = 2;
    const auto add = bg::node([factor](bg::In<bg::TS<bg::i64>, "input"_n> input, int offset, bg::Out<bg::TS<bg::i64>> output) {
        output.set(*input * factor + offset);
    });
    const auto graph = [&](bg::wp<bg::TS<bg::i64>> input) { return add(input, 1); };

    CHECK(bg::testing::test_graph(graph, {1, 2, 3}).equals({3, 5, 7}));
}

TEST_CASE("a callable node can omit its output", "[node_template][feature:node-api][definition:function]") {
    auto sum = 0;
    const auto sink = bg::node([&](bg::In<bg::TS<bg::i64>> input) { sum += *input; });
    const auto graph = [&](bg::wp<bg::TS<bg::i64>> input) {
        sink(input);
        return input;
    };

    STATIC_CHECK(std::same_as<decltype(sink(std::declval<bg::WiringPoint>())), void>);
    CHECK(bg::testing::test_graph(graph, {1, 2, 3}).equals({1, 2, 3}));
    CHECK(sum == 6);
}

namespace {

struct reference_source {
    bg::Out<bg::REF<bg::TS<bg::i64>>> output;

    void evaluate() const {}
};

} // namespace

TEST_CASE("aggregate and callable reference outputs default to deduplicated",
          "[node_template][reference][feature:node-api][definition:aggregate][definition:function]"
          "[guarantee:deduplicated]") {
    const auto aggregate = bg::node<reference_source>();
    const auto callable = bg::node([](bg::Out<bg::REF<bg::TS<bg::i64>>>) {});

    CHECK(aggregate.signature().output()->guarantee().value() == bg::TimeSeriesGuarantee::deduplicated);
    CHECK(callable.signature().output()->guarantee().value() == bg::TimeSeriesGuarantee::deduplicated);
}

namespace {

using KEY_SCALAR = bg::ScalarVariable<"KEY", bg::EqualityComparable, bg::Hashable>;

struct key_sink {
    bg::Scalar<KEY_SCALAR> key;

    void evaluate() const {
        static_cast<void>(key == key);
        static_cast<void>(key.hash());
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

    const std::array<const bg::Type *, 1> arguments{&bg::TS<float>::type()};
    const bg::ResolvedSignature signature = sink_node.signature().resolve(arguments);

    CHECK(signature.output == nullptr);
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
    const std::array<const bg::Type *, 3> arguments{&bg::float_type(), &bg::symbol_type(), &bg::int64_type()};
    const bg::ResolvedSignature signature = scalar_state.signature().resolve(arguments);

    REQUIRE(signature.parameters.size() == 3);
    CHECK(signature.parameters[0].name.text() == "factor");
    CHECK(signature.parameters[0].type == &bg::float_type());
    CHECK(signature.parameters[0].validity == bg::required);
    CHECK(signature.parameters[1].name.text() == "name");
    CHECK(signature.parameters[1].type == &bg::symbol_type());
    CHECK(signature.parameters[2].name.text() == "retries");
    CHECK(signature.parameters[2].type == &bg::int64_type());
    CHECK(signature.parameters[2].accepts_absent_scalar);
    const auto layout = scalar_state.state_layout(signature, {}, {});
    CHECK(layout.size == alignof(aligned_state) + sizeof(aligned_state));
    CHECK(layout.alignment == alignof(aligned_state));
}

namespace {

using LEFT_SCALAR = bg::ScalarVariable<"LEFT">;
using RIGHT_SCALAR = bg::ScalarVariable<"RIGHT">;

struct generic_pair {
    bg::In<bg::TS<LEFT_SCALAR>> first;
    bg::In<bg::TS<LEFT_SCALAR>> second;
    bg::In<bg::TS<RIGHT_SCALAR>> other;
    bg::Out<bg::TS<LEFT_SCALAR>> output;

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

inline constexpr auto G = bg::guarantee_variable<"G">;
using CAPTURED_TIME_SERIES = bg::TimeSeriesVariable<"T">;

struct concrete_output_definition {
    bg::Out<bg::TS<bg::i64>> output;

    void evaluate() const {}
};

struct guarantee_variable_output_definition {
    bg::In<bg::TS<bg::i64, G>> input;
    bg::Out<bg::TS<bg::i64, G>> output;

    void evaluate() const { output.set(*input); }
};

struct time_series_variable_guarantee {
    bg::In<CAPTURED_TIME_SERIES::with<G>> input;
    bg::Out<bg::TS<bg::i64, G>> output;

    void evaluate() const {}
};

static_assert(bg::AggregateNodeClass<time_series_variable_guarantee>);

} // namespace

TEST_CASE("a time series variable can expose its guarantee as another variable",
          "[node_template][type_system][feature:node-api]") {
    const auto node = bg::node<time_series_variable_guarantee>();
    const auto &list =
        bg::TSL<bg::TS<float, bg::TimeSeriesGuarantee::ordinary>, 2, bg::TimeSeriesGuarantee::final>::type();
    const auto arguments = std::array<const bg::Type *, 1>{&list};

    const auto signature = node.signature().resolve(arguments);

    CHECK(signature.parameters[0].type == &list);
    CHECK(signature.output == &bg::TS<bg::i64, bg::TimeSeriesGuarantee::final>::type());
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

template <template <typename, auto...> typename Field> struct runtime_layout_bundle {
    Field<bg::TS<bg::i64>> value;
};

template <template <typename, auto...> typename Field> struct guarantee_variable_bundle {
    Field<bg::TS<bg::i64, bg::guarantee_variable<"G">>> value;
};

struct runtime_layout_collection_sink {
    bg::In<bg::TSS<bg::i64>> set;
    bg::In<bg::TSD<bg::i64, bg::TS<bg::i64>>> dictionary;
    bg::In<bg::TSL<bg::TS<bg::i64>, 2>> list;
    bg::In<bg::TSB<runtime_layout_bundle>> bundle;

    void evaluate() const {}
};

struct guarantee_variable_input_sink {
    bg::In<bg::TS<bg::i64, bg::guarantee_variable<"G">>> input;
    bg::Out<bg::TS<bg::i64>> output;

    void evaluate() const {
        if (!input.runtime_type().type().is_resolved()) {
            throw std::logic_error{"a guarantee-variable input must keep its resolved runtime type"};
        }
        output.set(*input);
    }
};

struct generic_set_input_sink {
    bg::In<bg::TSS<bg::SCALAR>> input;

    void evaluate() const { (void)input.size(); }
};

struct scalar_or_time_series_input_definition {
    static const bg::TypeVariableType<bg::Type> &input_type() {
        static const auto guarantee = bg::GuaranteeVariable{bg::util::symbol{"G"}};
        static const auto time_series = bg::GenericTimeSeriesValueType{bg::int64_type(), guarantee};
        static const auto type = bg::TypeVariableType<bg::Type>{
            bg::util::symbol{"VALUE"}, std::vector<const bg::Type *>{&bg::int64_type(), &time_series}};
        return type;
    }

    const bg::Signature &signature() const {
        static const auto value = bg::Signature{bg::util::symbol{"scalar_or_time_series"},
                                                std::vector<bg::Parameter>{{bg::util::symbol{"value"}, &input_type()}}};
        return value;
    }

    static void evaluate(bg::NodeInvocation<>) {}
};

} // namespace

TEST_CASE("collection inputs retain their resolved runtime layout", "[node_template][time_series][feature:node-api]") {
    static_assert(!bg::details::direct_time_series_input<bg::In<bg::TS<bg::SCALAR>>>);
    static_assert(bg::details::direct_time_series_input<bg::In<bg::TS<bg::i64, bg::guarantee_variable<"G">>>>);
    static_assert(bg::details::input_requires_runtime_type<
                  bg::In<bg::TS<bg::i64, bg::guarantee_variable<"G">>>>);
    static_assert(bg::details::direct_time_series_input<bg::In<bg::TSS<bg::i64>>>);
    static_assert(!bg::details::input_requires_runtime_type<bg::In<bg::TSS<bg::i64>>>);
    static_assert(bg::details::input_requires_runtime_type<bg::In<bg::TSS<bg::SCALAR>>>);
    static_assert(bg::details::direct_time_series_input<bg::In<bg::TSS<bg::i64, bg::guarantee_variable<"G">>>>);
    static_assert(bg::details::input_requires_runtime_type<
                  bg::In<bg::TSS<bg::i64, bg::guarantee_variable<"G">>>>);
    static_assert(!bg::details::direct_time_series_input<bg::In<bg::TSD<bg::i64, bg::TS<bg::i64>>>>);
    static_assert(!bg::details::direct_time_series_input<bg::In<bg::TSL<bg::TS<bg::i64>, 2>>>);
    static_assert(!bg::details::direct_time_series_input<bg::In<bg::TSB<bg::SCHEMA>>>);
    static_assert(bg::details::direct_time_series_input<bg::In<bg::REF<bg::TS<bg::i64>>>>);
    static_assert(bg::details::direct_time_series_input<bg::In<bg::REF<bg::TS<bg::SCALAR>>>>);
    static_assert(bg::details::input_requires_runtime_type<bg::In<bg::REF<bg::TS<bg::SCALAR>>>>);
    static_assert(bg::details::direct_time_series_input<
                  bg::In<bg::REF<bg::TS<bg::i64, bg::guarantee_variable<"G">>>>>);
    static_assert(bg::details::direct_time_series_input<bg::In<bg::TS<bg::i64>>>);
    static_assert(bg::details::direct_time_series_input<
                  bg::In<bg::TS<bg::i64, bg::TimeSeriesGuarantee::final>>>);

    const auto sink = bg::node<runtime_layout_collection_sink>();
    const auto arguments = std::array<const bg::Type *, 4>{
        &bg::TSS<bg::i64>::type(), &bg::TSD<bg::i64, bg::TS<bg::i64>>::type(),
        &bg::TSL<bg::TS<bg::i64>, 2>::type(), &bg::TSB<runtime_layout_bundle>::type()};
    const auto signature = sink.signature().resolve(arguments);

    CHECK_FALSE(sink.uses_time_series_input_runtime_type(signature, 0));
    CHECK(sink.uses_time_series_input_runtime_type(signature, 1));
    CHECK(sink.uses_time_series_input_runtime_type(signature, 2));
    CHECK(sink.uses_time_series_input_runtime_type(signature, 3));

    const auto guarantee_input = bg::node<guarantee_variable_input_sink>();
    const std::array<const bg::Type *, 1> guarantee_arguments{&bg::TS<bg::i64>::type()};
    const auto guarantee_signature = guarantee_input.signature().resolve(guarantee_arguments);
    CHECK(guarantee_input.uses_time_series_input_runtime_type(guarantee_signature, 0));

    const auto generic_set_input = bg::node<generic_set_input_sink>();
    const std::array<const bg::Type *, 1> generic_set_arguments{&bg::TSS<bg::i64>::type()};
    const auto generic_set_signature = generic_set_input.signature().resolve(generic_set_arguments);
    CHECK(generic_set_input.uses_time_series_input_runtime_type(generic_set_signature, 0));

    const auto check_guarantee = bg::node([](bg::In<bg::TS<bg::i64, bg::guarantee_variable<"G">>> input,
                                             bg::Out<bg::TS<bg::i64>> output) {
        if (!input.runtime_type().type().is_resolved()) {
            throw std::logic_error{"a guarantee-variable input must keep its resolved runtime type"};
        }
        output.set(*input);
    });
    const auto graph = [&](bg::wp<bg::TS<bg::i64>> input) { return check_guarantee(input); };
    CHECK(bg::testing::test_graph(graph, std::array<bg::i64, 1>{42}).equals<bg::i64>({42}));
}

TEST_CASE("direct TSS inputs keep concrete and resolved key runtime types",
          "[node_template][time_series][feature:node-api]") {
    const auto make_set = bg::node([](bg::In<bg::TS<bg::i64>> input, bg::Out<bg::TSS<bg::i64>> output) {
        output.insert(*input);
    });
    const auto read_concrete = bg::node([](bg::In<bg::TSS<bg::i64>> input, bg::Out<bg::TS<bg::i64>> output) {
        output.set(input.item_at(0).value());
    });
    const auto read_generic = bg::node([](bg::In<bg::TSS<bg::SCALAR>> input, bg::Out<bg::TS<bg::i64>> output) {
        if (!input.runtime_type().type().is_resolved()) {
            throw std::logic_error{"a generic TSS input must keep its resolved runtime type"};
        }
        output.set(bg::scalar_cast<bg::i64>(input.item_at(0)).value());
    });
    const auto read_guarantee = bg::node([](bg::In<bg::TSS<bg::i64, bg::guarantee_variable<"G">>> input,
                                            bg::Out<bg::TS<bg::i64>> output) {
        if (!input.runtime_type().type().is_resolved()) {
            throw std::logic_error{"a guarantee-variable TSS input must keep its resolved runtime type"};
        }
        output.set(input.item_at(0).value());
    });

    const auto concrete_graph = [&](bg::wp<bg::TS<bg::i64>> input) { return read_concrete(make_set(input)); };
    const auto generic_graph = [&](bg::wp<bg::TS<bg::i64>> input) { return read_generic(make_set(input)); };
    const auto guarantee_graph = [&](bg::wp<bg::TS<bg::i64>> input) { return read_guarantee(make_set(input)); };

    CHECK(bg::testing::test_graph(concrete_graph, std::array<bg::i64, 1>{42}).equals<bg::i64>({42}));
    CHECK(bg::testing::test_graph(generic_graph, std::array<bg::i64, 1>{42}).equals<bg::i64>({42}));
    CHECK(bg::testing::test_graph(guarantee_graph, std::array<bg::i64, 1>{42}).equals<bg::i64>({42}));
}

TEST_CASE("native collection outputs bind directly only for concrete default declarations",
          "[node_template][time_series][feature:node-api]") {
    static_assert(!bg::details::direct_time_series_output<bg::Out<bg::TS<bg::SCALAR>>>);
    static_assert(!bg::details::direct_time_series_output<bg::Out<bg::TS<bg::i64>, bg::view>>);
    static_assert(bg::details::direct_time_series_output<bg::Out<bg::TSS<bg::i64>>>);
    static_assert(bg::details::direct_time_series_output<bg::Out<bg::TSD<bg::i64, bg::TS<bg::i64>>>>);
    static_assert(bg::details::direct_time_series_output<bg::Out<bg::TSL<bg::TS<bg::i64>, 2>>>);
    static_assert(bg::details::direct_time_series_output<bg::Out<bg::TS<bg::i64, bg::guarantee_variable<"G">>>>);
    static_assert(bg::details::output_requires_runtime_type<
                  bg::Out<bg::TS<bg::i64, bg::guarantee_variable<"G">>>>);
    static_assert(bg::details::output_requires_runtime_type<
                  bg::Out<bg::TSD<bg::i64, bg::TS<bg::i64, bg::guarantee_variable<"G">>>>>);
    using GuaranteedReference = bg::REF<bg::TS<bg::i64, bg::guarantee_variable<"G">>>;
    static_assert(bg::details::output_requires_runtime_type<bg::Out<bg::TSD<bg::i64, GuaranteedReference>>>);
    static_assert(bg::details::output_requires_runtime_type<
                  bg::Out<bg::TSD<bg::i64, bg::TSB<guarantee_variable_bundle>>>>);
    static_assert(!bg::details::output_requires_runtime_type<bg::Out<bg::TS<bg::i64>>>);
    static_assert(!bg::details::output_requires_runtime_type<bg::Out<bg::TSS<bg::i64>>>);
    static_assert(!bg::details::output_requires_runtime_type<bg::Out<bg::TSD<bg::i64, bg::TS<bg::i64>>>>);
    static_assert(!bg::details::output_requires_runtime_type<bg::Out<bg::TSL<bg::TS<bg::i64>, 2>>>);
    static_assert(!bg::details::direct_time_series_output<bg::Out<bg::TSS<bg::SCALAR>>>);
    static_assert(!bg::details::direct_time_series_output<bg::Out<bg::TSD<bg::SCALAR, bg::TS_T>>>);
    static_assert(!bg::details::direct_time_series_output<bg::Out<bg::TSL<bg::TS_T, 2>>>);
    static_assert(!bg::details::direct_time_series_output<bg::Out<bg::TSL<bg::TS<bg::i64>>>>);
    static_assert(!bg::details::direct_time_series_output<bg::Out<bg::TSS<bg::i64>, bg::view>>);
    static_assert(!bg::details::direct_time_series_output<bg::Out<bg::TSB<runtime_layout_bundle>>>);
}

TEST_CASE("only outputs with unresolved declarations store an output runtime type",
          "[node_template][time_series][feature:type-resolution]") {
    CHECK_FALSE(bg::node<concrete_output_definition>().uses_output_runtime_type());
    CHECK(bg::node<guarantee_variable_output_definition>().uses_output_runtime_type());
}

TEST_CASE("runtime type use follows resolved input types",
          "[node_template][feature:type-resolution][feature:node-api]") {
    const auto node = bg::node(scalar_or_time_series_input_definition{});
    const auto scalar_arguments = std::array<const bg::Type *, 1>{&bg::int64_type()};
    const auto time_series_arguments = std::array<const bg::Type *, 1>{&bg::TS<bg::i64>::type()};
    const auto scalar_signature = node.signature().resolve(scalar_arguments);
    const auto time_series_signature = node.signature().resolve(time_series_arguments);

    CHECK(node.uses_scalar_input_runtime_type(scalar_signature, 0));
    CHECK_FALSE(node.uses_time_series_input_runtime_type(scalar_signature, 0));
    CHECK_FALSE(node.uses_scalar_input_runtime_type(time_series_signature, 0));
    CHECK(node.uses_time_series_input_runtime_type(time_series_signature, 0));
}

namespace {

using NUMERIC_SCALAR = bg::ScalarVariable<"NUMERIC", float, double>;

struct numeric_sink {
    bg::In<bg::TS<NUMERIC_SCALAR>> input;

    void evaluate() const {}
};

static_assert(bg::AggregateNodeClass<numeric_sink>);

} // namespace

TEST_CASE("a scalar variable declaration constrains its resolved type", "[node_template][feature:node-api]") {
    const auto numeric_sink_node = bg::node<numeric_sink>();
    const auto &ts_double = bg::TS<double>::type();
    const auto &ts_int = bg::TS<bg::i64>::type();
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

struct pending_output {
    bg::Out<bg::TS<float>> output;

    bool evaluate() const { return false; }
};

struct repeated_input_name {
    bg::In<bg::TS<float>, "input"_n> input;
    bg::Out<bg::TS<float>> output;

    void evaluate() const {}
};

struct int_identity {
    bg::In<bg::TS<bg::i64>> input;
    bg::Out<bg::TS<bg::i64>> output;

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

TEST_CASE("an aggregate node evaluation can report pending",
          "[node_template][feature:node-api][definition:aggregate]") {
    const auto pending = bg::node<pending_output>();
    const auto signature = pending.signature().resolve(std::array<const bg::Type *, 0>{});

    CHECK(pending.evaluation_function(signature) != nullptr);
}
