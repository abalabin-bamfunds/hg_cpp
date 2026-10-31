#include "test_scalar_provider.hpp"

#include <bg/bg.hpp>
#include <bg/run_graph.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstring>
#include <memory>
#include <optional>
#include <type_traits>
#include <vector>

using namespace bg;

namespace {

struct Price {
    i64 quantity;
    double value;
};

struct OtherPrice {
    i64 quantity;
    double value;
};

TEST_CASE("a native compound scalar has canonical semantic and runtime metadata",
          "[feature:type-system][feature:scalar-storage][scalar:compound][declaration:concrete][storage:fixed]") {
    const auto &type = cpp_scalar_type<Price>();
    const auto &same_type = cpp_scalar_type<Price>();
    const auto &other_type = cpp_scalar_type<OtherPrice>();
    const auto &runtime_type = cpp_scalar_runtime_type<Price>();
    const auto &structure = type_cast<StructScalarType>(type);

    STATIC_CHECK(sizeof(Scalar<Price>) == 2 * sizeof(void *));
    STATIC_CHECK(sizeof(ScalarValue<Price>) == 2 * sizeof(void *));
    CHECK(&type == &same_type);
    CHECK(&type != &other_type);
    CHECK(structure.name().text() == "Price");
    CHECK(structure.full_name().text().ends_with("Price"));
    CHECK(structure.size() == sizeof(Price));
    CHECK(structure.alignment() == alignof(Price));
    REQUIRE(structure.field_count() == 2);
    CHECK(structure.field(0).name.text() == "quantity");
    CHECK(structure.field(0).type == &cpp_scalar_type<i64>());
    CHECK(structure.field(1).name.text() == "value");
    CHECK(structure.field(1).type == &cpp_scalar_type<double>());
    CHECK(runtime_type.size() == sizeof(Price));
    CHECK(runtime_type.alignment() == alignof(Price));
    CHECK_FALSE(runtime_type.uses_variable_memory());
}

struct Count {
    i64 value;
};

struct VenuePrice {
    Price price;
    symbol venue;
};

struct PaddedValue {
    bool enabled;
    i64 number;
};

struct Empty {};

class NonAggregate {
  public:
    explicit NonAggregate(i64 value) : value_{value} {}

  private:
    i64 value_;
};

struct NonTrivial {
    i64 value;

    ~NonTrivial() {}
};

struct NonStandardBase {
    i64 base;
};

struct NonStandard : NonStandardBase {
    i64 value;
};

struct StringField {
    string value;
};

struct UnsupportedField {
    int value;
};

struct TooManyFields {
    i64 f00;
    i64 f01;
    i64 f02;
    i64 f03;
    i64 f04;
    i64 f05;
    i64 f06;
    i64 f07;
    i64 f08;
    i64 f09;
    i64 f10;
    i64 f11;
    i64 f12;
    i64 f13;
    i64 f14;
    i64 f15;
    i64 f16;
    i64 f17;
    i64 f18;
    i64 f19;
    i64 f20;
    i64 f21;
    i64 f22;
    i64 f23;
    i64 f24;
    i64 f25;
    i64 f26;
    i64 f27;
    i64 f28;
    i64 f29;
    i64 f30;
    i64 f31;
    i64 f32;
};

static_assert(native_struct<Price>);
static_assert(native_struct<VenuePrice>);
static_assert(!native_struct<Empty>);
static_assert(!native_struct<NonAggregate>);
static_assert(!native_struct<NonTrivial>);
static_assert(!native_struct<NonStandard>);
static_assert(!native_struct<StringField>);
static_assert(!native_struct<UnsupportedField>);
static_assert(!native_struct<TooManyFields>);
static_assert(!native_struct<string>);
static_assert(!native_struct<Opaque<int>>);
static_assert(!native_struct<SCALAR>);

TEST_CASE("a native compound scalar has one semantic identity across translation units",
          "[feature:type-system][scalar:compound][declaration:concrete]") {
    CHECK(&cpp_scalar_type<test_scalar_provider::compound_value>() ==
          &test_scalar_provider::compound_semantic_type_from_provider());
}

TEST_CASE("a concrete compound scalar exposes its native value without a large copy",
          "[feature:scalar-api][scalar:compound][declaration:concrete][storage:fixed]") {
    const auto value = ScalarValue{Price{.quantity = 7, .value = 12.5}};
    const auto view = value.view();

    STATIC_CHECK(std::same_as<decltype(view.value()), const Price &>);
    CHECK(std::addressof(view.value()) == view.data());
    CHECK(view->quantity == 7);
    CHECK(view->value == 12.5);
}

TEST_CASE("a word-sized compound scalar uses inline value and generic field access",
          "[feature:scalar-api][feature:visitor][scalar:compound][storage:inline][declaration:runtime]") {
    const auto value = ScalarValue{Count{.value = 42}};
    const auto view = value.view();
    const auto generic = Scalar<SCALAR>::from(view);
    auto field_visited = false;

    STATIC_CHECK(std::same_as<decltype(view.value()), Count>);
    CHECK(view.value().value == 42);
    generic.visit(
        [&](const Scalar<STRUCT> *structure) {
            REQUIRE(structure->field_count() == 1);
            (*structure)[0].visit(
                [&](const Scalar<i64> *field) {
                    field_visited = true;
                    CHECK(field->value() == 42);
                },
                ddv::noop);
        },
        ddv::noop);

    CHECK(field_visited);
}

TEST_CASE("compound scalar equality and hashing ignore padding",
          "[feature:scalar-api][feature:scalar-storage][scalar:compound][storage:fixed]") {
    auto left = PaddedValue{};
    auto right = PaddedValue{};
    std::memset(std::addressof(left), 0, sizeof(left));
    std::memset(std::addressof(right), 0xff, sizeof(right));
    left.enabled = true;
    left.number = 42;
    right.enabled = true;
    right.number = 42;

    const auto left_value = ScalarValue{left};
    const auto right_value = ScalarValue{right};

    CHECK(std::memcmp(std::addressof(left), std::addressof(right), sizeof(left)) != 0);
    CHECK(left_value.equals(right_value));
    CHECK(left_value.hash() == right_value.hash());
}

TEST_CASE("a compound scalar visitor exposes ordered fields and nested compound values",
          "[feature:visitor][feature:scalar-api][scalar:compound][scalar:symbol][declaration:runtime]") {
    const auto value = ScalarValue{VenuePrice{
        .price = Price{.quantity = 3, .value = 4.5},
        .venue = symbol{"LSE"},
    }};
    auto structure_visited = false;
    auto nested_visited = false;
    auto quantity_visited = false;
    auto venue_visited = false;

    value.view().visit(
        [&](const Scalar<STRUCT> *structure) {
            structure_visited = true;
            REQUIRE(structure->field_count() == 2);
            CHECK(structure->field_name(0) == "price");
            CHECK(structure->field_name(1) == "venue");

            (*structure)[0].visit(
                [&](const Scalar<STRUCT> *price) {
                    nested_visited = true;
                    REQUIRE(price->field_count() == 2);
                    (*price)[0].visit(
                        [&](const Scalar<i64> *quantity) {
                            quantity_visited = true;
                            CHECK(quantity->value() == 3);
                        },
                        ddv::noop);
                },
                ddv::noop);
            (*structure)[1].visit(
                [&](const Scalar<symbol> *venue) {
                    venue_visited = true;
                    CHECK(venue->value().text() == "LSE");
                },
                ddv::noop);
        },
        ddv::noop);

    CHECK(structure_visited);
    CHECK(nested_visited);
    CHECK(quantity_visited);
    CHECK(venue_visited);
}

TEST_CASE("nested compound scalar equality and hashing include symbol fields",
          "[feature:scalar-api][feature:scalar-storage][scalar:compound][scalar:symbol][storage:fixed]") {
    const auto left = ScalarValue{VenuePrice{
        .price = Price{.quantity = 3, .value = 4.5},
        .venue = symbol{"LSE"},
    }};
    const auto equal = ScalarValue{VenuePrice{
        .price = Price{.quantity = 3, .value = 4.5},
        .venue = symbol{"LSE"},
    }};
    const auto different = ScalarValue{VenuePrice{
        .price = Price{.quantity = 3, .value = 4.5},
        .venue = symbol{"NYSE"},
    }};

    CHECK(left.equals(equal));
    CHECK(left.hash() == equal.hash());
    CHECK_FALSE(left.equals(different));
}

TEST_CASE("compound scalar matching is nominal and STRUCT resolves inside a time series",
          "[feature:type-system][scalar:compound][series:value][declaration:generic]") {
    auto type_map = TypeMap{};

    CHECK_FALSE(TS<Price>::type().accepts(TS<OtherPrice>::type()));
    REQUIRE(TS<STRUCT>::type().match(TS<Price>::type(), type_map));
    CHECK(&TS<STRUCT>::type().resolve(type_map) == &TS<Price>::type());
}

TEST_CASE("a STRUCT node binds to and copies a concrete compound time series",
          "[feature:test-graph][feature:type-system][feature:scalar-storage][scalar:compound]"
          "[series:value][declaration:generic]") {
    const auto copy = node([](In<TS<STRUCT>> input, Out<TS<STRUCT>> output) { output.set_value(input.value_view()); });

    const auto graph = [&](wp<TS<Price>> input) { return copy(input); };
    const auto first = Price{.quantity = 1, .value = 2.5};
    const auto second = Price{.quantity = 2, .value = 3.5};

    const auto result = bg::run_graph(graph, std::vector{first, second});

    CHECK(result.equals<Price>({first, second}));
}

TEST_CASE("run_graph carries concrete compound scalar values and suppresses equal updates",
          "[feature:test-graph][feature:scalar-storage][scalar:compound][series:value]"
          "[guarantee:deduplicated][transition:update]") {
    const auto copy = node([](In<TS<Price>> input, Out<TS<Price, TimeSeriesGuarantee::deduplicated>> output) {
        output.set(input.value());
    });

    const auto graph = [&](wp<TS<Price>> input) { return copy(input); };
    const auto first = Price{.quantity = 1, .value = 2.5};
    const auto second = Price{.quantity = 2, .value = 3.5};

    const auto result = bg::run_graph(graph, std::vector{first, first, second});

    CHECK(result.equals<Price>({first, std::nullopt, second}));
}

} // namespace
