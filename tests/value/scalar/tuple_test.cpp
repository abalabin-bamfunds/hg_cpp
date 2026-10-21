#include <bg/bg.hpp>
#include <bg/scalar/bundle/value.hpp>
#include <bg/scalar/literal.hpp>
#include <bg/testing/test_graph.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>

using namespace bg;

namespace {

using Market = Tuple<string, i64, symbol>;
using Nested = Tuple<i64, Tuple<string, symbol>>;
using Empty = Tuple<>;
using Single = Tuple<i64>;
using NoEquality = Tuple<Opaque<std::array<int, 2>>>;
using ITEM = ScalarVariable<"ITEM">;
using MismatchAfterBinding = Tuple<ITEM, i64>;

template <typename T> concept HasScalarEquality = requires(const T &value) { value.equals(value); };
template <typename T> concept HasScalarHash = requires(const T &value) { value.hash(); };

struct TrackedValue {
    static inline int instances{};

    TrackedValue() { ++instances; }
    TrackedValue(const TrackedValue &) { ++instances; }
    ~TrackedValue() { --instances; }
};

using GenericTuple = Tuple<i64, ITEM>;
using ShutdownConcreteTuple = Tuple<i64, symbol>;

struct TupleShutdownProbe {
    bool active{};

    ~TupleShutdownProbe() noexcept {
        if (!active) {
            return;
        }

        const auto &generic = static_cast<const TupleScalarType &>(cpp_scalar_type<GenericTuple>());
        const auto &concrete = static_cast<const TupleScalarType &>(cpp_scalar_type<ShutdownConcreteTuple>());
        auto type_map = TypeMap{};
        if (generic.size() != 2 || !generic.match(concrete, type_map) || &generic.resolve(type_map) != &concrete) {
            std::abort();
        }

        const auto &runtime = static_cast<const ScalarTupleRuntimeType &>(
            cpp_scalar_runtime_type<ShutdownConcreteTuple>());
        if (&runtime.type() != &concrete ||
            &runtime.item_runtime_type(0) != &cpp_scalar_runtime_type<i64>() ||
            &runtime.item_runtime_type(1) != &cpp_scalar_runtime_type<symbol>()) {
            std::abort();
        }
    }
};

TupleShutdownProbe tuple_shutdown_probe;

template <template <typename, auto...> typename Field> struct BaseRecord {
    Field<symbol> name;
};

template <template <typename, auto...> typename Field> struct DerivedRecord
    : schema_extends<BaseRecord, Field> {
    Field<i64> quantity;
};

using BaseTuple = Tuple<Bundle<BaseRecord>>;
using DerivedTuple = Tuple<Bundle<DerivedRecord>>;

} // namespace

TEST_CASE("a positional scalar tuple has canonical ordered type identity",
          "[feature:type-system][feature:scalar-api][scalar:tuple][declaration:concrete]") {
    const auto &type = cpp_scalar_type<Market>();
    const auto &same_type = cpp_scalar_type<Market>();
    const auto &reordered_type = cpp_scalar_type<Tuple<i64, string, symbol>>();

    CHECK(&type == &same_type);
    CHECK(&type != &reordered_type);
    CHECK(type.is_scalar());
    CHECK(type.is_resolved());

    auto text = std::string{};
    type.format_name(text);
    CHECK(text == "Tuple<string, i64, symbol>");
}

TEST_CASE("tuple scalar descriptors remain usable during process shutdown",
          "[feature:type-system][scalar:tuple][declaration:generic][storage:variable]") {
    const auto &generic = static_cast<const TupleScalarType &>(cpp_scalar_type<GenericTuple>());
    const auto &concrete = static_cast<const TupleScalarType &>(cpp_scalar_type<ShutdownConcreteTuple>());
    auto type_map = TypeMap{};

    REQUIRE(generic.match(concrete, type_map));
    CHECK(&generic.resolve(type_map) == &concrete);
    CHECK(&cpp_scalar_runtime_type<ShutdownConcreteTuple>().type() == &concrete);

    tuple_shutdown_probe.active = true;
}

TEST_CASE("a positional scalar tuple matches and resolves each item in order",
          "[feature:type-system][scalar:tuple][declaration:generic]") {
    const auto &generic = cpp_scalar_type<GenericTuple>();
    const auto &concrete = cpp_scalar_type<Tuple<i64, symbol>>();
    const auto &wrong_order = cpp_scalar_type<Tuple<symbol, i64>>();
    auto type_map = TypeMap{};

    REQUIRE(generic.match(concrete, type_map));
    CHECK(&generic.resolve(type_map) == &concrete);
    CHECK_FALSE(generic.accepts(wrong_order));

    auto partial_type_map = TypeMap{};
    const auto &mismatch_after_binding = cpp_scalar_type<MismatchAfterBinding>();
    const auto &mismatch = cpp_scalar_type<Tuple<string, symbol>>();
    CHECK_FALSE(mismatch_after_binding.match(mismatch, partial_type_map));
    CHECK(partial_type_map.find(ITEM::type()) == nullptr);
}

TEST_CASE("positional scalar literals construct nested tuple values",
          "[feature:scalar-api][scalar:tuple][scalar:string][scalar:symbol][storage:variable]") {
    auto value = ScalarValue<Nested>::from_literal(
        literal::ScalarLiteral{7, {"NASDAQ", symbol{"NQ"}}});
    const auto view = value.view();
    const auto nested = view.get<1>();

    CHECK(view.get<0>().value() == 7);
    CHECK(nested.get<0>().value() == "NASDAQ");
    CHECK(nested.get<1>().value() == symbol{"NQ"});

    const auto generic = Scalar<TUPLE>::from(view);
    REQUIRE(generic.size() == 2);
    CHECK(&generic.item_type(0) == &cpp_scalar_type<i64>());
    auto first_item = i64{};
    generic[0].visit([&](const Scalar<i64> *item) { first_item = item->value(); }, ddv::noop);
    CHECK(first_item == 7);
}

TEST_CASE("positional scalar tuple equality and hashing use ordered items",
          "[feature:scalar-api][scalar:tuple][storage:variable]") {
    const auto left = ScalarValue<Market>::from_literal(
        literal::ScalarLiteral{"LSE", 3, symbol{"VOD"}});
    const auto same = ScalarValue<Market>::from_literal(
        literal::ScalarLiteral{"LSE", 3, symbol{"VOD"}});
    const auto different = ScalarValue<Market>::from_literal(
        literal::ScalarLiteral{"LSE", 3, symbol{"BARC"}});

    CHECK(left.equals(same.view()));
    CHECK(left.hash() == same.hash());
    CHECK_FALSE(left.equals(different.view()));
}

TEST_CASE("positional scalar tuples support empty and single-item values and reject wrong arity",
          "[feature:scalar-api][scalar:tuple][storage:variable]") {
    const auto empty = ScalarValue<Empty>::from_literal(literal::ScalarLiteral{});
    CHECK(empty.view().runtime_type().type().size() == 0);

    const auto single = ScalarValue<Single>::from_literal(literal::ScalarLiteral{42});
    CHECK(single.view().get<0>().value() == 42);

    CHECK_THROWS_AS(ScalarValue<Single>::from_literal(literal::ScalarLiteral{}), std::invalid_argument);
    CHECK_THROWS_AS(ScalarValue<Single>::from_literal(literal::ScalarLiteral{1, 2}), std::invalid_argument);
}

TEST_CASE("positional scalar tuple copies retain nested items after the source is destroyed",
          "[feature:scalar-api][scalar:tuple][storage:variable]") {
    const auto copy = [&] {
        auto source = ScalarValue<Nested>::from_literal(
            literal::ScalarLiteral{7, {"NASDAQ", symbol{"NQ"}}});
        return ScalarValue<Nested>{source.view()};
    }();

    CHECK(copy.view().get<1>().get<0>().value() == "NASDAQ");
    CHECK(copy.view().get<1>().get<1>().value() == symbol{"NQ"});
}

TEST_CASE("compatible tuple runtime types reconstruct items through the destination runtime",
          "[feature:scalar-api][scalar:tuple][scalar:hierarchical][binding:covariant]"
          "[storage:variable]") {
    auto memory = allocator{};
    const auto derived = bundle<DerivedRecord>(memory, symbol{"ABC"}, 100);
    auto source = ScalarValue<DerivedTuple>::from_literal({derived.view()}, memory);
    const auto &destination_runtime = cpp_scalar_runtime_type<BaseTuple>();
    REQUIRE(destination_runtime.accepts_input(source.runtime_type()));

    struct Context {
        const details::ScalarData *source;
    } context{std::addressof(details::scalar_data(source))};
    auto destination = ScalarValue<>::from_construction(
        destination_runtime,
        {&context, [](void *opaque_context, const ScalarRuntimeType &runtime_type, void *storage,
                      allocator &resource) {
            runtime_type.construct_from_input(storage, *static_cast<Context *>(opaque_context)->source, resource);
        }},
        memory);

    const auto &tuple_runtime = static_cast<const ScalarTupleRuntimeType &>(destination.runtime_type());
    CHECK(&tuple_runtime.item_runtime_type(0) == &cpp_scalar_runtime_type<Bundle<BaseRecord>>());
    const auto tuple = Scalar<TUPLE>::from(destination);
    const auto base = scalar_cast<Bundle<BaseRecord>>(tuple[0]);
    CHECK(scalar_cast<symbol>(base[0]).value() == symbol{"ABC"});
}

TEST_CASE("positional scalar tuple equality and hashing require support from every item",
          "[feature:scalar-api][scalar:tuple][storage:variable]") {
    const auto &type = cpp_scalar_type<NoEquality>();
    CHECK_FALSE(type.supports_equality());
    CHECK_FALSE(type.supports_hash());
    STATIC_CHECK_FALSE(HasScalarEquality<Scalar<NoEquality>>);
    STATIC_CHECK_FALSE(HasScalarHash<Scalar<NoEquality>>);
}

TEST_CASE("positional scalar tuple construction destroys completed items after a later failure",
          "[feature:scalar-api][scalar:tuple][scalar:opaque][storage:variable]") {
    using Item = Opaque<TrackedValue>;
    using Value = Tuple<Item, Item>;

    auto source = ScalarValue<Item>{TrackedValue{}};
    const auto baseline = TrackedValue::instances;
    struct Context {
        const details::ScalarData *source;
    } context{std::addressof(details::scalar_data(source))};

    const auto construction = ScalarConstruction{
        &context, [](void *opaque_context, const ScalarRuntimeType &runtime_type, void *destination,
                     allocator &memory) {
            auto &context = *static_cast<Context *>(opaque_context);
            const auto &tuple_runtime = static_cast<const ScalarTupleRuntimeType &>(runtime_type);
            tuple_runtime.construct_fields_with(
                destination, memory,
                {&context, [](void *field_context, std::size_t index, const ScalarRuntimeType &item_runtime,
                              void *item_destination, allocator &item_memory) {
                    if (index != 0) {
                        throw std::runtime_error{"later tuple item construction failed"};
                    }
                    item_runtime.construct_from_input(item_destination,
                                                      *static_cast<Context *>(field_context)->source, item_memory);
                    return true;
                }});
        }};

    CHECK_THROWS_AS((ScalarValue<Value>::from_construction(construction)), std::runtime_error);
    CHECK(TrackedValue::instances == baseline);
}

TEST_CASE("test_graph accepts a tuple-valued time series input and output",
          "[feature:test-graph][evaluation][series:value][scalar:tuple][shape:single-node]") {
    auto retained = std::optional<ScalarValue<Nested>>{};
    const auto identity = node([&](In<TS<Nested>> input, Out<TS<Nested>> output) {
        if (!retained) {
            retained.emplace(input.value());
        }
        output.set_value(input.value());
    });

    const auto graph = [&](wp<TS<Nested>> input) { return identity(input); };

    const auto actual = testing::test_graph(
        graph, {{1, {"NASDAQ", symbol{"NQ"}}}, {2, {"NYSE", symbol{"IBM"}}}});

    REQUIRE(actual.size() == 2);
    const auto first = actual[0]->view();
    CHECK(first.get<0>().value() == 1);
    CHECK(first.get<1>().get<0>().value() == "NASDAQ");
    CHECK(first.get<1>().get<1>().value() == symbol{"NQ"});
    const auto second = actual[1]->view();
    CHECK(second.get<0>().value() == 2);
    CHECK(second.get<1>().get<0>().value() == "NYSE");
    CHECK(second.get<1>().get<1>().value() == symbol{"IBM"});

    using bg::literals::operator""_n;
    const auto erased_graph = bg::graph("name"_n = "erased_tuple_identity", [&](wp<> input) {
        return wp<>{identity(input.as<TS<Nested>>())};
    });
    const auto erased = testing::test_graph(
        erased_graph, testing::input<TS<Nested>>({{1, {"NASDAQ", symbol{"NQ"}}},
                                                          {2, {"NYSE", symbol{"IBM"}}}}));
    CHECK(erased.equals({{1, {"NASDAQ", symbol{"NQ"}}}, {2, {"NYSE", symbol{"IBM"}}}}));
    REQUIRE(retained.has_value());
    CHECK(retained->view().get<0>().value() == 1);
    CHECK(retained->view().get<1>().get<0>().value() == "NASDAQ");
    CHECK(retained->view().get<1>().get<1>().value() == symbol{"NQ"});
}
