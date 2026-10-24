#include <bg/node.hpp>
#include <bg/run_graph.hpp>
#include <bg/type/scalar_type.hpp>
#include <bg/type/time_series_reference_type.hpp>
#include <bg/type/time_series_list_type.hpp>
#include <bg/type/time_series_value_type.hpp>
#include <bg/type/type_variable.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <concepts>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

using namespace bg;

namespace {

using namespace bg::literals;

struct weighted_add {
    In<TS<float>> lhs;
    In<TS<float>, optional, passive> rhs;
    Out<TS<float>> output;

    void evaluate() const { output.set(*lhs + *rhs); }
};

static_assert(AggregateNodeClass<weighted_add>);

template <typename Template>
concept TemporaryWeightedAddCall = requires(Template &&node, WiringPoint input) { std::move(node)(input, input); };

static_assert(!TemporaryWeightedAddCall<decltype(node<weighted_add>())>);

} // namespace

TEST_CASE("an aggregate node class generates a signature", "[node_template][feature:node-api][definition:aggregate]") {
    const auto weighted_add_node = node<weighted_add>();
    static_assert(std::derived_from<std::remove_cv_t<decltype(weighted_add_node)>, NodeTemplate>);

    const auto &ts_final = TS<float, TimeSeriesGuarantee::final>::type();
    const auto &ts_deduplicated = TS<float, TimeSeriesGuarantee::deduplicated>::type();
    const std::array<const Type *, 2> arguments{&ts_final, &ts_deduplicated};

    const ResolvedSignature signature = weighted_add_node.signature().resolve(arguments);

    REQUIRE(signature.parameters.size() == 2);
    CHECK(signature.name.text() == "weighted_add");
    CHECK(signature.parameters[0].name.text() == "lhs");
    CHECK(signature.parameters[0].type == &ts_final);
    CHECK(signature.parameters[0].activity == active);
    CHECK(signature.parameters[0].validity == required);
    CHECK(signature.parameters[1].name.text() == "rhs");
    CHECK(signature.parameters[1].type == &ts_deduplicated);
    CHECK(signature.parameters[1].activity == passive);
    CHECK(signature.parameters[1].validity == optional);
    CHECK(signature.output == &TS<float>::type());
}

namespace {
namespace dynamic_input_test {

using ITEM = TimeSeriesVariable<"ITEM">;
inline constexpr auto G = guarantee_variable<"G">;

struct dynamic_reference_input_node {
    In<REF<TSD<i64, ITEM, G>, G>> dictionary;
    In<REF<ITEM, G>, dynamic_binding, optional, "selected"_n> item;
    Out<REF<ITEM, G>> output;

    void evaluate() const {}
};

} // namespace dynamic_input_test
} // namespace

TEST_CASE("a dynamic reference input is internal to an aggregate node",
          "[node_template][feature:node-api][feature:reference][definition:aggregate]"
          "[series:reference][guarantee:ordinary][guarantee:final]") {
    const auto node = bg::node<dynamic_input_test::dynamic_reference_input_node>();

    const auto check_dynamic_reference = [&](const Type &argument, TimeSeriesGuarantee guarantee,
                                             const Type &target) {
        const std::array<const Type *, 1> arguments{&argument};

        const auto resolution = node.signature().resolve_with_type_map(arguments);

        const auto dynamic_inputs = node.dynamic_input_types(resolution.type_map);

        REQUIRE(resolution.signature.parameters.size() == 1);
        CHECK(resolution.signature.parameters[0].name.text() == "dictionary");
        REQUIRE(dynamic_inputs.size() == 1);
        CHECK(dynamic_inputs[0] == resolution.signature.output);
        const auto &reference = static_cast<const TimeSeriesReferenceType &>(*dynamic_inputs[0]);
        CHECK(reference.guarantee().value() == guarantee);
        CHECK(&reference.target_type() == &target);
    };

    check_dynamic_reference(TSD<i64, TS<float>>::type(), TimeSeriesGuarantee::ordinary,
                            TS<float>::type());
    check_dynamic_reference(TSD<i64, TS<float>, TimeSeriesGuarantee::final>::type(),
                            TimeSeriesGuarantee::final,
                            TS<float, TimeSeriesGuarantee::final>::type());
}

namespace {
namespace dynamic_input_test {

struct dynamic_reference_input_default_guarantee_node {
    In<REF<TS<float>>, dynamic_binding, optional> item;

    void evaluate() const {}
};

} // namespace dynamic_input_test
} // namespace

TEST_CASE("an internal dynamic reference input defaults its root guarantee to ordinary",
          "[node_template][feature:node-api][feature:reference][definition:aggregate]"
          "[series:reference][guarantee:ordinary]") {
    const auto node = bg::node<dynamic_input_test::dynamic_reference_input_default_guarantee_node>();

    const auto resolution = node.signature().resolve_with_type_map(std::array<const Type *, 0>{});

    const auto dynamic_inputs = node.dynamic_input_types(resolution.type_map);

    REQUIRE(dynamic_inputs.size() == 1);
    const auto &reference = static_cast<const TimeSeriesReferenceType &>(*dynamic_inputs[0]);
    CHECK(reference.guarantee().value() == TimeSeriesGuarantee::ordinary);
    CHECK(&reference.target_type() == &TS<float>::type());
}

TEST_CASE("a callable node receives typed inputs, scalars, and output",
          "[node_template][feature:node-api][definition:function]") {
    const auto factor = 2;

    const auto add = node([factor](In<TS<i64>, "input"_n> input, int offset, Out<TS<i64>> output) {
        output.set(*input * factor + offset);
    });

    const auto graph = [&](wp<TS<i64>> input) { return add(input, 1); };

    CHECK(bg::run_graph(graph, {1, 2, 3}).equals({3, 5, 7}));
}

TEST_CASE("a callable node can omit its output", "[node_template][feature:node-api][definition:function]") {
    auto sum = 0;

    const auto sink = node([&](In<TS<i64>> input) { sum += *input; });

    const auto graph = [&](wp<TS<i64>> input) {
        sink(input);
        return input;
    };

    STATIC_CHECK(std::same_as<decltype(sink(std::declval<WiringPoint>())), void>);
    CHECK(bg::run_graph(graph, {1, 2, 3}).equals({1, 2, 3}));
    CHECK(sum == 6);
}

namespace {

struct reference_source {
    Out<REF<TS<i64>>> output;

    void evaluate() const {}
};

template <template <typename, auto...> typename Field> struct ReferenceBundle {
    Field<REF<TS<i64>>> reference;
};

struct reference_bundle_source {
    Out<TSB<ReferenceBundle>> output;

    void evaluate() const {}
};

} // namespace

TEST_CASE("aggregate and callable reference outputs default to deduplicated",
          "[node_template][reference][feature:node-api][definition:aggregate][definition:function]"
          "[guarantee:deduplicated]") {
    const auto aggregate = node<reference_source>();

    const auto callable = node([](Out<REF<TS<i64>>>) {});

    CHECK(aggregate.signature().output()->guarantee().value() == TimeSeriesGuarantee::deduplicated);
    CHECK(callable.signature().output()->guarantee().value() == TimeSeriesGuarantee::deduplicated);
}

TEST_CASE("callable and aggregate collection outputs apply reference defaults to nested fields",
          "[node_template][reference][feature:node-api][definition:aggregate][definition:function]"
          "[series:tsb][guarantee:ordinary][guarantee:deduplicated]") {
    const auto aggregate = node<reference_bundle_source>();

    const auto callable = node([](Out<TSB<ReferenceBundle>>) {});
    const auto &aggregate_output =
        static_cast<const TimeSeriesBundleType &>(*aggregate.signature().output());
    const auto &callable_output = static_cast<const TimeSeriesBundleType &>(*callable.signature().output());

    CHECK(aggregate_output.item_type(0).guarantee().value() == TimeSeriesGuarantee::deduplicated);
    CHECK(callable_output.item_type(0).guarantee().value() == TimeSeriesGuarantee::deduplicated);

    const auto enclosing_reference = node([](Out<REF<TSB<ReferenceBundle>>>) {});
    const auto &reference = static_cast<const TimeSeriesReferenceType &>(*enclosing_reference.signature().output());
    const auto &target = static_cast<const TimeSeriesBundleType &>(reference.target_type());

    CHECK(reference.guarantee().value() == TimeSeriesGuarantee::deduplicated);
    CHECK(target.item_type(0).guarantee().value() == TimeSeriesGuarantee::ordinary);

    const auto enclosing_dictionary_reference = node(
        [](Out<REF<TSD<i64, REF<TS<i64>>>>>) {});
    const auto &dictionary_reference = static_cast<const TimeSeriesReferenceType &>(
        *enclosing_dictionary_reference.signature().output());
    const auto &dictionary_target =
        static_cast<const TimeSeriesDictionaryType &>(dictionary_reference.target_type());
    CHECK(dictionary_reference.guarantee().value() == TimeSeriesGuarantee::deduplicated);
    CHECK(dictionary_target.value_type().guarantee().value() == TimeSeriesGuarantee::ordinary);

    const auto enclosing_list_reference = node(
        [](Out<REF<TSL<REF<TS<i64>>, 2>>>) {});
    const auto &list_reference = static_cast<const TimeSeriesReferenceType &>(
        *enclosing_list_reference.signature().output());
    const auto &list_target = static_cast<const TimeSeriesListType &>(list_reference.target_type());
    CHECK(list_reference.guarantee().value() == TimeSeriesGuarantee::deduplicated);
    CHECK(list_target.item_type().guarantee().value() == TimeSeriesGuarantee::ordinary);
}

namespace {

using KEY_SCALAR = ScalarVariable<"KEY", EqualityComparable, Hashable>;

struct key_sink {
    Scalar<KEY_SCALAR> key;

    void evaluate() const {
        static_cast<void>(key == key);
        static_cast<void>(key.hash());
    }
};

static_assert(AggregateNodeClass<key_sink>);

} // namespace

TEST_CASE("a scalar variable enforces required operations", "[node_template][feature:node-api]") {
    const auto key_sink_node = node<key_sink>();
    const std::array<const Type *, 1> hashable_argument{&int64_type()};
    const auto signature = key_sink_node.signature().resolve(hashable_argument);

    CHECK(signature.parameters[0].type == &int64_type());

    const auto &opaque_type = cpp_scalar_type<Opaque<int>>();
    const std::array<const Type *, 1> opaque_argument{&opaque_type};
    CHECK_THROWS_AS(key_sink_node.signature().resolve(opaque_argument), TypeResolutionError);
}

TEST_CASE("an aggregate node template exposes its polymorphic interface",
          "[node_template][feature:node-api][definition:aggregate]") {
    const auto weighted_add_node = node<weighted_add>();
    const NodeTemplate &node_template = weighted_add_node;

    CHECK(&node_template.signature() == &weighted_add_node.signature());
}

namespace {

struct sink {
    In<TS<float>> input;

    void evaluate() const {}
};

static_assert(AggregateNodeClass<sink>);

} // namespace

TEST_CASE("an aggregate node can omit its output", "[node_template][feature:node-api][definition:aggregate]") {
    const auto sink_node = node<sink>();
    static_assert(std::same_as<decltype(sink_node(std::declval<WiringPoint>())), void>);

    const std::array<const Type *, 1> arguments{&TS<float>::type()};
    const ResolvedSignature signature = sink_node.signature().resolve(arguments);

    CHECK(signature.output == nullptr);
}

namespace {

struct final_identity {
    In<TS<float, TimeSeriesGuarantee::final>> input;
    Out<TS<float, TimeSeriesGuarantee::final>> output;

    void evaluate() const { output.set(*input); }
};

} // namespace

TEST_CASE("an aggregate signature retains specified guarantees",
          "[node_template][feature:node-api][definition:aggregate][guarantee:const][guarantee:final]") {
    const auto final_identity_node = node<final_identity>();
    const auto &ts_const = TS<float, TimeSeriesGuarantee::const_>::type();
    const auto &ts_final = TS<float, TimeSeriesGuarantee::final>::type();
    const std::array<const Type *, 1> arguments{&ts_const};

    const ResolvedSignature signature = final_identity_node.signature().resolve(arguments);

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
    util::symbol name;
    std::optional<int> retries;
    State<invocation_state> state;
    State<aligned_state> aligned;
    Out<TS<float>> output;

    void evaluate() const {
        ++state->count;
        ++aligned->count;
        output.set(factor);
    }
};

static_assert(AggregateNodeClass<scalar_state_node>);

} // namespace

TEST_CASE("aggregate scalar and state fields have distinct roles",
          "[node_template][feature:node-api][definition:aggregate][scalar:symbol]") {
    const auto scalar_state = node<scalar_state_node>();
    const std::array<const Type *, 3> arguments{&float_type(), &symbol_type(), &int64_type()};
    const ResolvedSignature signature = scalar_state.signature().resolve(arguments);

    REQUIRE(signature.parameters.size() == 3);
    CHECK(signature.parameters[0].name.text() == "factor");
    CHECK(signature.parameters[0].type == &float_type());
    CHECK(signature.parameters[0].validity == required);
    CHECK(signature.parameters[1].name.text() == "name");
    CHECK(signature.parameters[1].type == &symbol_type());
    CHECK(signature.parameters[2].name.text() == "retries");
    CHECK(signature.parameters[2].type == &int64_type());
    CHECK(signature.parameters[2].accepts_absent_scalar);
    const auto layout = scalar_state.state_layout(signature, {}, {});
    CHECK(layout.size == alignof(aligned_state) + sizeof(aligned_state));
    CHECK(layout.alignment == alignof(aligned_state));
}

namespace {

using LEFT_SCALAR = ScalarVariable<"LEFT">;
using RIGHT_SCALAR = ScalarVariable<"RIGHT">;

struct generic_pair {
    In<TS<LEFT_SCALAR>> first;
    In<TS<LEFT_SCALAR>> second;
    In<TS<RIGHT_SCALAR>> other;
    Out<TS<LEFT_SCALAR>> output;

    void evaluate() const { output.set_value(first.value_view()); }
};

static_assert(AggregateNodeClass<generic_pair>);

} // namespace

TEST_CASE("scalar variable declarations retain their identity", "[node_template][feature:node-api]") {
    const auto generic_pair_node = node<generic_pair>();
    const auto &ts_float = TS<float>::type();
    const auto &ts_double = TS<double>::type();
    const std::array<const Type *, 3> arguments{&ts_float, &ts_float, &ts_double};

    const auto signature = generic_pair_node.signature().resolve(arguments);

    CHECK(signature.parameters[0].type == &ts_float);
    CHECK(signature.parameters[1].type == &ts_float);
    CHECK(signature.parameters[2].type == &ts_double);
    CHECK(signature.output == &ts_float);
}

namespace {

inline constexpr auto G = guarantee_variable<"G">;
using CAPTURED_TIME_SERIES = TimeSeriesVariable<"T">;

struct concrete_output_definition {
    Out<TS<i64>> output;

    void evaluate() const {}
};

struct guarantee_variable_output_definition {
    In<TS<i64, G>> input;
    Out<TS<i64, G>> output;

    void evaluate() const { output.set(*input); }
};

struct time_series_variable_guarantee {
    In<CAPTURED_TIME_SERIES::with<G>> input;
    Out<TS<i64, G>> output;

    void evaluate() const {}
};

static_assert(AggregateNodeClass<time_series_variable_guarantee>);

} // namespace

TEST_CASE("a time series variable can expose its guarantee as another variable",
          "[node_template][type_system][feature:node-api]") {
    const auto node = bg::node<time_series_variable_guarantee>();
    const auto &list =
        TSL<TS<float, TimeSeriesGuarantee::ordinary>, 2, TimeSeriesGuarantee::final>::type();
    const auto arguments = std::array<const Type *, 1>{&list};

    const auto signature = node.signature().resolve(arguments);

    CHECK(signature.parameters[0].type == &list);
    CHECK(signature.output == &TS<i64, TimeSeriesGuarantee::final>::type());
}

namespace {

struct generic_scalar_source {
    Scalar<SCALAR> value;
    Out<TS<SCALAR>> output;

    void evaluate() const { output.set_value(value); }
};

static_assert(AggregateNodeClass<generic_scalar_source>);

} // namespace

TEST_CASE("a scalar variable can declare a scalar view field", "[node_template][feature:node-api]") {
    const auto generic_scalar_source_node = node<generic_scalar_source>();
    const std::array<const Type *, 1> arguments{&float_type()};

    const auto signature = generic_scalar_source_node.signature().resolve(arguments);

    CHECK(signature.parameters[0].type == &float_type());
    CHECK(signature.output == &TS<float>::type());
}

namespace {

template <template <typename, auto...> typename Field> struct runtime_layout_bundle {
    Field<TS<i64>> value;
};

template <template <typename, auto...> typename Field> struct guarantee_variable_bundle {
    Field<TS<i64, guarantee_variable<"G">>> value;
};

struct runtime_layout_collection_sink {
    In<TSS<i64>> set;
    In<TSD<i64, TS<i64>>> dictionary;
    In<TSL<TS<i64>, 2>> list;
    In<TSB<runtime_layout_bundle>> bundle;

    void evaluate() const {}
};

struct guarantee_variable_input_sink {
    In<TS<i64, guarantee_variable<"G">>> input;
    Out<TS<i64>> output;

    void evaluate() const {
        if (!input.runtime_type().type().is_resolved()) {
            throw std::logic_error{"a guarantee-variable input must keep its resolved runtime type"};
        }
        output.set(*input);
    }
};

struct generic_set_input_sink {
    In<TSS<SCALAR>> input;

    void evaluate() const { (void)input.size(); }
};

struct scalar_or_time_series_input_definition {
    using VALUE = Union<"VALUE", i64, TS<i64, guarantee_variable<"G">>>;

    const Signature &signature() const {
        static const auto value = Signature{
            util::symbol{"scalar_or_time_series"},
            std::vector<Parameter>{{util::symbol{"value"}, &VALUE::type()}}};
        return value;
    }

    static void evaluate(NodeInvocation<>) {}
};

} // namespace

TEST_CASE("collection inputs retain their resolved runtime layout", "[node_template][time_series][feature:node-api]") {
    static_assert(!details::direct_time_series_input<In<TS<SCALAR>>>);
    static_assert(details::direct_time_series_input<In<TS<i64, guarantee_variable<"G">>>>);
    static_assert(details::input_requires_runtime_type<
                  In<TS<i64, guarantee_variable<"G">>>>);
    static_assert(details::direct_time_series_input<In<TSS<i64>>>);
    static_assert(details::input_requires_runtime_type<In<TSS<i64>>>);
    static_assert(details::input_requires_runtime_type<In<TSS<SCALAR>>>);
    static_assert(details::direct_time_series_input<In<TSS<i64, guarantee_variable<"G">>>>);
    static_assert(details::input_requires_runtime_type<
                  In<TSS<i64, guarantee_variable<"G">>>>);
    static_assert(!details::direct_time_series_input<In<TSD<i64, TS<i64>>>>);
    static_assert(!details::direct_time_series_input<In<TSL<TS<i64>, 2>>>);
    static_assert(!details::direct_time_series_input<In<TSB<SCHEMA>>>);
    static_assert(details::direct_time_series_input<In<REF<TS<i64>>>>);
    static_assert(details::direct_time_series_input<In<REF<TS<SCALAR>>>>);
    static_assert(details::input_requires_runtime_type<In<REF<TS<SCALAR>>>>);
    static_assert(details::direct_time_series_input<
                  In<REF<TS<i64, guarantee_variable<"G">>>>>);
    static_assert(details::direct_time_series_input<In<TS<i64>>>);
    static_assert(details::direct_time_series_input<
                  In<TS<i64, TimeSeriesGuarantee::final>>>);

    const auto sink = node<runtime_layout_collection_sink>();
    const auto arguments = std::array<const Type *, 4>{
        &TSS<i64>::type(), &TSD<i64, TS<i64>>::type(),
        &TSL<TS<i64>, 2>::type(), &TSB<runtime_layout_bundle>::type()};
    const auto signature = sink.signature().resolve(arguments);

    CHECK(sink.uses_time_series_input_runtime_type(signature, 0));
    CHECK(sink.uses_time_series_input_runtime_type(signature, 1));
    CHECK(sink.uses_time_series_input_runtime_type(signature, 2));
    CHECK(sink.uses_time_series_input_runtime_type(signature, 3));

    const auto guarantee_input = node<guarantee_variable_input_sink>();
    const std::array<const Type *, 1> guarantee_arguments{&TS<i64>::type()};
    const auto guarantee_signature = guarantee_input.signature().resolve(guarantee_arguments);
    CHECK(guarantee_input.uses_time_series_input_runtime_type(guarantee_signature, 0));

    const auto generic_set_input = node<generic_set_input_sink>();
    const std::array<const Type *, 1> generic_set_arguments{&TSS<i64>::type()};
    const auto generic_set_signature = generic_set_input.signature().resolve(generic_set_arguments);
    CHECK(generic_set_input.uses_time_series_input_runtime_type(generic_set_signature, 0));

    const auto check_guarantee = node([](In<TS<i64, guarantee_variable<"G">>> input,
                                             Out<TS<i64>> output) {
        if (!input.runtime_type().type().is_resolved()) {
            throw std::logic_error{"a guarantee-variable input must keep its resolved runtime type"};
        }
        output.set(*input);
    });

    const auto graph = [&](wp<TS<i64>> input) { return check_guarantee(input); };
    CHECK(bg::run_graph(graph, {42}).equals<i64>({42}));
}

TEST_CASE("direct TSS inputs keep concrete and resolved key runtime types",
          "[node_template][time_series][feature:node-api]") {
    const auto read_concrete = node([](In<TSS<i64>> input, Out<TS<i64>> output) {
        output.set(input.item_at(0).value());
    });

    const auto read_generic = node([](In<TSS<SCALAR>> input, Out<TS<i64>> output) {
        if (!input.runtime_type().type().is_resolved()) {
            throw std::logic_error{"a generic TSS input must keep its resolved runtime type"};
        }
        output.set(scalar_cast<i64>(input.item_at(0)).value());
    });

    const auto read_guarantee = node([](In<TSS<i64, guarantee_variable<"G">>> input,
                                            Out<TS<i64>> output) {
        if (!input.runtime_type().type().is_resolved()) {
            throw std::logic_error{"a guarantee-variable TSS input must keep its resolved runtime type"};
        }
        output.set(input.item_at(0).value());
    });

    const auto concrete_graph = [&](wp<TSS<i64>> input) { return read_concrete(input); };

    const auto generic_graph = [&](wp<TSS<i64>> input) { return read_generic(input); };

    const auto guarantee_graph = [&](wp<TSS<i64>> input) { return read_guarantee(input); };

    CHECK(bg::run_graph(concrete_graph, {{42}}).equals<i64>({42}));
    CHECK(bg::run_graph(generic_graph, {{42}}).equals<i64>({42}));
    CHECK(bg::run_graph(guarantee_graph, {{42}}).equals<i64>({42}));
}

TEST_CASE("native collection outputs bind directly only for concrete declarations",
          "[node_template][time_series][feature:node-api]") {
    static_assert(!details::direct_time_series_output<Out<TS<SCALAR>>>);
    static_assert(details::direct_time_series_output<Out<TSS<i64>>>);
    static_assert(details::direct_time_series_output<Out<TSD<i64, TS<i64>>>>);
    static_assert(details::direct_time_series_output<Out<TSL<TS<i64>, 2>>>);
    static_assert(details::direct_time_series_output<Out<TS<i64, guarantee_variable<"G">>>>);
    static_assert(details::output_requires_runtime_type<
                  Out<TS<i64, guarantee_variable<"G">>>>);
    static_assert(details::output_requires_runtime_type<
                  Out<TSD<i64, TS<i64, guarantee_variable<"G">>>>>);
    using GuaranteedReference = REF<TS<i64, guarantee_variable<"G">>>;
    static_assert(details::output_requires_runtime_type<Out<TSD<i64, GuaranteedReference>>>);
    static_assert(details::output_requires_runtime_type<
                  Out<TSD<i64, TSB<guarantee_variable_bundle>>>>);
    static_assert(!details::output_requires_runtime_type<Out<TS<i64>>>);
    static_assert(!details::output_requires_runtime_type<Out<TSS<i64>>>);
    static_assert(!details::output_requires_runtime_type<Out<TSD<i64, TS<i64>>>>);
    static_assert(!details::output_requires_runtime_type<Out<TSL<TS<i64>, 2>>>);
    static_assert(!details::direct_time_series_output<Out<TSS<SCALAR>>>);
    static_assert(!details::direct_time_series_output<Out<TSD<SCALAR, TS_T>>>);
    static_assert(!details::direct_time_series_output<Out<TSL<TS_T, 2>>>);
    static_assert(!details::direct_time_series_output<Out<TSL<TS<i64>>>>);
    static_assert(!details::direct_time_series_output<Out<TSB<runtime_layout_bundle>>>);
}

TEST_CASE("only outputs with unresolved declarations store an output runtime type",
          "[node_template][time_series][feature:type-system]") {
    CHECK_FALSE(node<concrete_output_definition>().uses_output_runtime_type());
    CHECK(node<guarantee_variable_output_definition>().uses_output_runtime_type());
}

TEST_CASE("runtime type use follows resolved input types",
          "[node_template][feature:type-system][feature:node-api]") {
    const auto node = bg::node(scalar_or_time_series_input_definition{});
    const auto scalar_arguments = std::array<const Type *, 1>{&int64_type()};
    const auto time_series_arguments = std::array<const Type *, 1>{&TS<i64>::type()};

    const auto scalar_signature = node.signature().resolve(scalar_arguments);

    const auto time_series_signature = node.signature().resolve(time_series_arguments);

    CHECK(node.uses_scalar_input_runtime_type(scalar_signature, 0));
    CHECK_FALSE(node.uses_time_series_input_runtime_type(scalar_signature, 0));
    CHECK_FALSE(node.uses_scalar_input_runtime_type(time_series_signature, 0));
    CHECK(node.uses_time_series_input_runtime_type(time_series_signature, 0));
}

namespace {

using NUMERIC_SCALAR = ScalarVariable<"NUMERIC", float, double>;

struct numeric_sink {
    In<TS<NUMERIC_SCALAR>> input;

    void evaluate() const {}
};

static_assert(AggregateNodeClass<numeric_sink>);

} // namespace

TEST_CASE("a scalar variable declaration constrains its resolved type", "[node_template][feature:node-api]") {
    const auto numeric_sink_node = node<numeric_sink>();
    const auto &ts_double = TS<double>::type();
    const auto &ts_int = TS<i64>::type();
    const std::array<const Type *, 1> accepted{&ts_double};
    const std::array<const Type *, 1> rejected{&ts_int};

    CHECK(numeric_sink_node.signature().resolve(accepted).parameters[0].type == &ts_double);
    CHECK_THROWS_AS(numeric_sink_node.signature().resolve(rejected), TypeResolutionError);
}

namespace {

struct repeated_output {
    Out<TS<float>> first;
    Out<TS<float>> second;

    void evaluate() const {}
};

struct returned_output {
    Out<TS<float>> output;

    float evaluate() { return 0.0F; }
};

struct pending_output {
    Out<TS<float>> output;

    bool evaluate() const { return false; }
};

struct repeated_input_name {
    In<TS<float>, "input"_n> input;
    Out<TS<float>> output;

    void evaluate() const {}
};

struct int_identity {
    In<TS<i64>> input;
    Out<TS<i64>> output;

    void evaluate() const { output.set(*input); }
};

} // namespace

TEST_CASE("aggregate node validation rejects ambiguous output definitions",
          "[node_template][feature:node-api][definition:aggregate]") {
    STATIC_CHECK_FALSE(AggregateNodeClass<repeated_output>);
    STATIC_CHECK_FALSE(AggregateNodeClass<returned_output>);
    STATIC_CHECK_FALSE(AggregateNodeClass<repeated_input_name>);
    STATIC_CHECK(AggregateNodeClass<int_identity>);
}

TEST_CASE("an aggregate node evaluation can report pending",
          "[node_template][feature:node-api][definition:aggregate]") {
    const auto pending = node<pending_output>();
    const auto signature = pending.signature().resolve(std::array<const Type *, 0>{});

    CHECK(pending.evaluation_function(signature) != nullptr);
}
