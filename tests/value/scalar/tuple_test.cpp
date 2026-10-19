#include <bg/bg.hpp>
#include <bg/scalar/bundle/value.hpp>
#include <bg/scalar/literal.hpp>
#include <bg/testing/test_graph.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>

namespace {

using Market = bg::Tuple<bg::string, bg::i64, bg::symbol>;
using Nested = bg::Tuple<bg::i64, bg::Tuple<bg::string, bg::symbol>>;
using Empty = bg::Tuple<>;
using Single = bg::Tuple<bg::i64>;
using NoEquality = bg::Tuple<bg::Opaque<std::array<int, 2>>>;
using ITEM = bg::ScalarVariable<"ITEM">;
using MismatchAfterBinding = bg::Tuple<ITEM, bg::i64>;

template <typename T> concept HasScalarEquality = requires(const T &value) { value.equals(value); };
template <typename T> concept HasScalarHash = requires(const T &value) { value.hash(); };

struct TrackedValue {
    static inline int instances{};

    TrackedValue() { ++instances; }
    TrackedValue(const TrackedValue &) { ++instances; }
    ~TrackedValue() { --instances; }
};

using GenericTuple = bg::Tuple<bg::i64, ITEM>;

template <template <typename, auto...> typename Field> struct BaseRecord {
    Field<bg::symbol> name;
};

template <template <typename, auto...> typename Field> struct DerivedRecord
    : bg::schema_extends<BaseRecord, Field> {
    Field<bg::i64> quantity;
};

using BaseTuple = bg::Tuple<bg::Bundle<BaseRecord>>;
using DerivedTuple = bg::Tuple<bg::Bundle<DerivedRecord>>;

} // namespace

TEST_CASE("a positional scalar tuple has canonical ordered type identity",
          "[feature:type-system][feature:scalar-api][scalar:tuple][declaration:concrete]") {
    const auto &type = bg::cpp_scalar_type<Market>();
    const auto &same_type = bg::cpp_scalar_type<Market>();
    const auto &reordered_type = bg::cpp_scalar_type<bg::Tuple<bg::i64, bg::string, bg::symbol>>();

    CHECK(&type == &same_type);
    CHECK(&type != &reordered_type);
    CHECK(type.is_scalar());
    CHECK(type.is_resolved());

    auto text = std::string{};
    type.format_name(text);
    CHECK(text == "Tuple<string, i64, symbol>");
}

TEST_CASE("a positional scalar tuple matches and resolves each item in order",
          "[feature:type-system][scalar:tuple][declaration:generic]") {
    const auto &generic = bg::cpp_scalar_type<GenericTuple>();
    const auto &concrete = bg::cpp_scalar_type<bg::Tuple<bg::i64, bg::symbol>>();
    const auto &wrong_order = bg::cpp_scalar_type<bg::Tuple<bg::symbol, bg::i64>>();
    auto type_map = bg::TypeMap{};

    REQUIRE(generic.match(concrete, type_map));
    CHECK(&generic.resolve(type_map) == &concrete);
    CHECK_FALSE(generic.accepts(wrong_order));

    auto partial_type_map = bg::TypeMap{};
    const auto &mismatch_after_binding = bg::cpp_scalar_type<MismatchAfterBinding>();
    const auto &mismatch = bg::cpp_scalar_type<bg::Tuple<bg::string, bg::symbol>>();
    CHECK_FALSE(mismatch_after_binding.match(mismatch, partial_type_map));
    CHECK(partial_type_map.find(ITEM::type()) == nullptr);
}

TEST_CASE("positional scalar literals construct nested tuple values",
          "[feature:scalar-api][scalar:tuple][scalar:string][scalar:symbol][storage:variable]") {
    auto value = bg::ScalarValue<Nested>::from_literal(
        literal::ScalarLiteral{7, {"NASDAQ", bg::symbol{"NQ"}}});
    const auto view = value.view();
    const auto nested = view.get<1>();

    CHECK(view.get<0>().value() == 7);
    CHECK(nested.get<0>().value() == "NASDAQ");
    CHECK(nested.get<1>().value() == bg::symbol{"NQ"});

    const auto generic = bg::Scalar<bg::TUPLE>::from(view);
    REQUIRE(generic.size() == 2);
    CHECK(&generic.item_type(0) == &bg::cpp_scalar_type<bg::i64>());
    auto first_item = bg::i64{};
    generic[0].visit([&](const bg::Scalar<bg::i64> *item) { first_item = item->value(); }, ddv::noop);
    CHECK(first_item == 7);
}

TEST_CASE("positional scalar tuple equality and hashing use ordered items",
          "[feature:scalar-api][scalar:tuple][storage:variable]") {
    const auto left = bg::ScalarValue<Market>::from_literal(
        literal::ScalarLiteral{"LSE", 3, bg::symbol{"VOD"}});
    const auto same = bg::ScalarValue<Market>::from_literal(
        literal::ScalarLiteral{"LSE", 3, bg::symbol{"VOD"}});
    const auto different = bg::ScalarValue<Market>::from_literal(
        literal::ScalarLiteral{"LSE", 3, bg::symbol{"BARC"}});

    CHECK(left.equals(same.view()));
    CHECK(left.hash() == same.hash());
    CHECK_FALSE(left.equals(different.view()));
}

TEST_CASE("positional scalar tuples support empty and single-item values and reject wrong arity",
          "[feature:scalar-api][scalar:tuple][storage:variable]") {
    const auto empty = bg::ScalarValue<Empty>::from_literal(literal::ScalarLiteral{});
    CHECK(empty.view().runtime_type().type().size() == 0);

    const auto single = bg::ScalarValue<Single>::from_literal(literal::ScalarLiteral{42});
    CHECK(single.view().get<0>().value() == 42);

    CHECK_THROWS_AS(bg::ScalarValue<Single>::from_literal(literal::ScalarLiteral{}), std::invalid_argument);
    CHECK_THROWS_AS(bg::ScalarValue<Single>::from_literal(literal::ScalarLiteral{1, 2}), std::invalid_argument);
}

TEST_CASE("positional scalar tuple copies retain nested items after the source is destroyed",
          "[feature:scalar-api][scalar:tuple][storage:variable]") {
    const auto copy = [&] {
        auto source = bg::ScalarValue<Nested>::from_literal(
            literal::ScalarLiteral{7, {"NASDAQ", bg::symbol{"NQ"}}});
        return bg::ScalarValue<Nested>{source.view()};
    }();

    CHECK(copy.view().get<1>().get<0>().value() == "NASDAQ");
    CHECK(copy.view().get<1>().get<1>().value() == bg::symbol{"NQ"});
}

TEST_CASE("compatible tuple runtime types reconstruct items through the destination runtime",
          "[feature:scalar-api][scalar:tuple][scalar:hierarchical][binding:covariant]"
          "[storage:variable]") {
    auto memory = bg::allocator{};
    const auto derived = bg::bundle<DerivedRecord>(memory, bg::symbol{"ABC"}, 100);
    auto source = bg::ScalarValue<DerivedTuple>::from_literal({derived.view()}, memory);
    const auto &destination_runtime = bg::cpp_scalar_runtime_type<BaseTuple>();
    REQUIRE(destination_runtime.accepts_input(source.runtime_type()));

    struct Context {
        const bg::details::ScalarData *source;
    } context{std::addressof(bg::details::scalar_data(source))};
    auto destination = bg::ScalarValue<>::from_construction(
        destination_runtime,
        {&context, [](void *opaque_context, const bg::ScalarRuntimeType &runtime_type, void *storage,
                      bg::allocator &resource) {
            runtime_type.construct_from_input(storage, *static_cast<Context *>(opaque_context)->source, resource);
        }},
        memory);

    const auto &tuple_runtime = static_cast<const bg::ScalarTupleRuntimeType &>(destination.runtime_type());
    CHECK(&tuple_runtime.item_runtime_type(0) == &bg::cpp_scalar_runtime_type<bg::Bundle<BaseRecord>>());
    const auto tuple = bg::Scalar<bg::TUPLE>::from(destination);
    const auto base = bg::scalar_cast<bg::Bundle<BaseRecord>>(tuple[0]);
    CHECK(bg::scalar_cast<bg::symbol>(base[0]).value() == bg::symbol{"ABC"});
}

TEST_CASE("positional scalar tuple equality and hashing require support from every item",
          "[feature:scalar-api][scalar:tuple][storage:variable]") {
    const auto &type = bg::cpp_scalar_type<NoEquality>();
    CHECK_FALSE(type.supports_equality());
    CHECK_FALSE(type.supports_hash());
    STATIC_CHECK_FALSE(HasScalarEquality<bg::Scalar<NoEquality>>);
    STATIC_CHECK_FALSE(HasScalarHash<bg::Scalar<NoEquality>>);
}

TEST_CASE("positional scalar tuple construction destroys completed items after a later failure",
          "[feature:scalar-api][scalar:tuple][scalar:opaque][storage:variable]") {
    using Item = bg::Opaque<TrackedValue>;
    using Value = bg::Tuple<Item, Item>;

    auto source = bg::ScalarValue<Item>{TrackedValue{}};
    const auto baseline = TrackedValue::instances;
    struct Context {
        const bg::details::ScalarData *source;
    } context{std::addressof(bg::details::scalar_data(source))};

    const auto construction = bg::ScalarConstruction{
        &context, [](void *opaque_context, const bg::ScalarRuntimeType &runtime_type, void *destination,
                     bg::allocator &memory) {
            auto &context = *static_cast<Context *>(opaque_context);
            const auto &tuple_runtime = static_cast<const bg::ScalarTupleRuntimeType &>(runtime_type);
            tuple_runtime.construct_fields_with(
                destination, memory,
                {&context, [](void *field_context, std::size_t index, const bg::ScalarRuntimeType &item_runtime,
                              void *item_destination, bg::allocator &item_memory) {
                    if (index != 0) {
                        throw std::runtime_error{"later tuple item construction failed"};
                    }
                    item_runtime.construct_from_input(item_destination,
                                                      *static_cast<Context *>(field_context)->source, item_memory);
                    return true;
                }});
        }};

    CHECK_THROWS_AS((bg::ScalarValue<Value>::from_construction(construction)), std::runtime_error);
    CHECK(TrackedValue::instances == baseline);
}

TEST_CASE("test_graph accepts a tuple-valued time series input and output",
          "[feature:test-graph][evaluation][series:value][scalar:tuple][shape:single-node]") {
    auto retained = std::optional<bg::ScalarValue<Nested>>{};
    const auto identity = bg::node([&](bg::In<bg::TS<Nested>> input, bg::Out<bg::TS<Nested>> output) {
        if (!retained) {
            retained.emplace(input.value_view());
        }
        output.set_value(input.value_view());
    });
    const auto graph = [&](bg::wp<bg::TS<Nested>> input) { return identity(input); };

    const auto actual = bg::testing::test_graph(
        graph, {{1, {"NASDAQ", bg::symbol{"NQ"}}}, {2, {"NYSE", bg::symbol{"IBM"}}}});

    REQUIRE(actual.size() == 2);
    const auto first = actual[0]->view();
    CHECK(first.get<0>().value() == 1);
    CHECK(first.get<1>().get<0>().value() == "NASDAQ");
    CHECK(first.get<1>().get<1>().value() == bg::symbol{"NQ"});
    const auto second = actual[1]->view();
    CHECK(second.get<0>().value() == 2);
    CHECK(second.get<1>().get<0>().value() == "NYSE");
    CHECK(second.get<1>().get<1>().value() == bg::symbol{"IBM"});

    using bg::literals::operator""_n;
    const auto erased_graph = bg::graph("name"_n = "erased_tuple_identity", [&](bg::wp<> input) {
        return bg::wp<>{identity(input.as<bg::TS<Nested>>())};
    });
    const auto erased = bg::testing::test_graph(
        erased_graph, bg::testing::input<bg::TS<Nested>>({{1, {"NASDAQ", bg::symbol{"NQ"}}},
                                                          {2, {"NYSE", bg::symbol{"IBM"}}}}));
    CHECK(erased.equals({{1, {"NASDAQ", bg::symbol{"NQ"}}}, {2, {"NYSE", bg::symbol{"IBM"}}}}));
    REQUIRE(retained.has_value());
    CHECK(retained->view().get<0>().value() == 1);
    CHECK(retained->view().get<1>().get<0>().value() == "NASDAQ");
    CHECK(retained->view().get<1>().get<1>().value() == bg::symbol{"NQ"});
}
