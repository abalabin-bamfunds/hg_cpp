#include "test_scalar_provider.hpp"

#include <bg/bg.hpp>
#include <bg/testing/test_graph.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstring>
#include <memory>
#include <optional>
#include <type_traits>
#include <vector>

namespace {

struct Price {
    bg::i64 quantity;
    double value;
};

struct Count {
    bg::i64 value;
};

struct VenuePrice {
    Price price;
    bg::symbol venue;
};

struct OtherPrice {
    bg::i64 quantity;
    double value;
};

struct PaddedValue {
    bool enabled;
    bg::i64 number;
};

struct Empty {};

class NonAggregate {
  public:
    explicit NonAggregate(bg::i64 value) : value_{value} {}

  private:
    bg::i64 value_;
};

struct NonTrivial {
    bg::i64 value;

    ~NonTrivial() {}
};

struct NonStandardBase {
    bg::i64 base;
};

struct NonStandard : NonStandardBase {
    bg::i64 value;
};

struct StringField {
    bg::string value;
};

struct UnsupportedField {
    int value;
};

struct TooManyFields {
    bg::i64 f00;
    bg::i64 f01;
    bg::i64 f02;
    bg::i64 f03;
    bg::i64 f04;
    bg::i64 f05;
    bg::i64 f06;
    bg::i64 f07;
    bg::i64 f08;
    bg::i64 f09;
    bg::i64 f10;
    bg::i64 f11;
    bg::i64 f12;
    bg::i64 f13;
    bg::i64 f14;
    bg::i64 f15;
    bg::i64 f16;
    bg::i64 f17;
    bg::i64 f18;
    bg::i64 f19;
    bg::i64 f20;
    bg::i64 f21;
    bg::i64 f22;
    bg::i64 f23;
    bg::i64 f24;
    bg::i64 f25;
    bg::i64 f26;
    bg::i64 f27;
    bg::i64 f28;
    bg::i64 f29;
    bg::i64 f30;
    bg::i64 f31;
    bg::i64 f32;
};

static_assert(bg::native_struct<Price>);
static_assert(bg::native_struct<VenuePrice>);
static_assert(!bg::native_struct<Empty>);
static_assert(!bg::native_struct<NonAggregate>);
static_assert(!bg::native_struct<NonTrivial>);
static_assert(!bg::native_struct<NonStandard>);
static_assert(!bg::native_struct<StringField>);
static_assert(!bg::native_struct<UnsupportedField>);
static_assert(!bg::native_struct<TooManyFields>);
static_assert(!bg::native_struct<bg::string>);
static_assert(!bg::native_struct<bg::Opaque<int>>);
static_assert(!bg::native_struct<bg::SCALAR>);

TEST_CASE("a native compound scalar has canonical semantic and runtime metadata",
          "[feature:type-system][feature:scalar-storage][scalar:compound][declaration:concrete][storage:fixed]") {
    const auto &type = bg::cpp_scalar_type<Price>();
    const auto &same_type = bg::cpp_scalar_type<Price>();
    const auto &other_type = bg::cpp_scalar_type<OtherPrice>();
    const auto &runtime_type = bg::cpp_scalar_runtime_type<Price>();
    const auto &structure = bg::type_cast<bg::StructScalarType>(type);

    STATIC_CHECK(sizeof(bg::Scalar<Price>) == 2 * sizeof(void *));
    STATIC_CHECK(sizeof(bg::ScalarValue<Price>) == 2 * sizeof(void *));
    CHECK(&type == &same_type);
    CHECK(&type != &other_type);
    CHECK(structure.name().text() == "Price");
    CHECK(structure.full_name().text().ends_with("Price"));
    CHECK(structure.size() == sizeof(Price));
    CHECK(structure.alignment() == alignof(Price));
    REQUIRE(structure.field_count() == 2);
    CHECK(structure.field(0).name.text() == "quantity");
    CHECK(structure.field(0).type == &bg::cpp_scalar_type<bg::i64>());
    CHECK(structure.field(1).name.text() == "value");
    CHECK(structure.field(1).type == &bg::cpp_scalar_type<double>());
    CHECK(runtime_type.size() == sizeof(Price));
    CHECK(runtime_type.alignment() == alignof(Price));
    CHECK_FALSE(runtime_type.uses_variable_memory());
}

TEST_CASE("a native compound scalar has one semantic identity across translation units",
          "[feature:type-system][scalar:compound][declaration:concrete]") {
    CHECK(&bg::cpp_scalar_type<test_scalar_provider::compound_value>() ==
          &test_scalar_provider::compound_semantic_type_from_provider());
}

TEST_CASE("a concrete compound scalar exposes its native value without a large copy",
          "[feature:scalar-api][scalar:compound][declaration:concrete][storage:fixed]") {
    const auto value = bg::ScalarValue{Price{.quantity = 7, .value = 12.5}};
    const auto view = value.view();

    STATIC_CHECK(std::same_as<decltype(view.value()), const Price &>);
    CHECK(std::addressof(view.value()) == view.data());
    CHECK(view->quantity == 7);
    CHECK(view->value == 12.5);
}

TEST_CASE("a word-sized compound scalar uses inline value and generic field access",
          "[feature:scalar-api][feature:visitor][scalar:compound][storage:inline][declaration:runtime]") {
    const auto value = bg::ScalarValue{Count{.value = 42}};
    const auto view = value.view();
    const auto generic = bg::Scalar<bg::SCALAR>::from(view);
    auto field_visited = false;

    STATIC_CHECK(std::same_as<decltype(view.value()), Count>);
    CHECK(view.value().value == 42);
    generic.visit(
        [&](const bg::Scalar<bg::STRUCT> *structure) {
            REQUIRE(structure->field_count() == 1);
            (*structure)[0].visit(
                [&](const bg::Scalar<bg::i64> *field) {
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

    const auto left_value = bg::ScalarValue{left};
    const auto right_value = bg::ScalarValue{right};

    CHECK(std::memcmp(std::addressof(left), std::addressof(right), sizeof(left)) != 0);
    CHECK(left_value.equals(right_value));
    CHECK(left_value.hash() == right_value.hash());
}

TEST_CASE("a compound scalar visitor exposes ordered fields and nested compound values",
          "[feature:visitor][feature:scalar-api][scalar:compound][scalar:symbol][declaration:runtime]") {
    const auto value = bg::ScalarValue{VenuePrice{
        .price = Price{.quantity = 3, .value = 4.5},
        .venue = bg::symbol{"LSE"},
    }};
    auto structure_visited = false;
    auto nested_visited = false;
    auto quantity_visited = false;
    auto venue_visited = false;

    value.view().visit(
        [&](const bg::Scalar<bg::STRUCT> *structure) {
            structure_visited = true;
            REQUIRE(structure->field_count() == 2);
            CHECK(structure->field_name(0) == "price");
            CHECK(structure->field_name(1) == "venue");

            (*structure)[0].visit(
                [&](const bg::Scalar<bg::STRUCT> *price) {
                    nested_visited = true;
                    REQUIRE(price->field_count() == 2);
                    (*price)[0].visit(
                        [&](const bg::Scalar<bg::i64> *quantity) {
                            quantity_visited = true;
                            CHECK(quantity->value() == 3);
                        },
                        ddv::noop);
                },
                ddv::noop);
            (*structure)[1].visit(
                [&](const bg::Scalar<bg::symbol> *venue) {
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
    const auto left = bg::ScalarValue{VenuePrice{
        .price = Price{.quantity = 3, .value = 4.5},
        .venue = bg::symbol{"LSE"},
    }};
    const auto equal = bg::ScalarValue{VenuePrice{
        .price = Price{.quantity = 3, .value = 4.5},
        .venue = bg::symbol{"LSE"},
    }};
    const auto different = bg::ScalarValue{VenuePrice{
        .price = Price{.quantity = 3, .value = 4.5},
        .venue = bg::symbol{"NYSE"},
    }};

    CHECK(left.equals(equal));
    CHECK(left.hash() == equal.hash());
    CHECK_FALSE(left.equals(different));
}

TEST_CASE("compound scalar matching is nominal and STRUCT resolves inside a time series",
          "[feature:type-system][scalar:compound][series:value][declaration:generic]") {
    auto type_map = bg::TypeMap{};

    CHECK_FALSE(bg::TS<Price>::type().accepts(bg::TS<OtherPrice>::type()));
    REQUIRE(bg::TS<bg::STRUCT>::type().match(bg::TS<Price>::type(), type_map));
    CHECK(&bg::TS<bg::STRUCT>::type().resolve(type_map) == &bg::TS<Price>::type());
}

TEST_CASE("a STRUCT node binds to and copies a concrete compound time series",
          "[feature:test-graph][feature:type-system][feature:scalar-storage][scalar:compound]"
          "[series:value][declaration:generic]") {
    const auto copy = bg::node([](bg::In<bg::TS<bg::STRUCT>> input, bg::Out<bg::TS<bg::STRUCT>> output) {
        output.set_value(input.value_view());
    });
    const auto graph = [&](bg::wp<bg::TS<Price>> input) { return copy(input); };
    const auto first = Price{.quantity = 1, .value = 2.5};
    const auto second = Price{.quantity = 2, .value = 3.5};

    const auto result = bg::testing::test_graph(graph, std::vector{first, second});

    CHECK(result.equals<Price>({first, second}));
}

TEST_CASE("test_graph carries concrete compound scalar values and suppresses equal updates",
          "[feature:test-graph][feature:scalar-storage][scalar:compound][series:value]"
          "[guarantee:deduplicated][transition:update]") {
    const auto copy = bg::node([](bg::In<bg::TS<Price>> input,
                                  bg::Out<bg::TS<Price, bg::TimeSeriesGuarantee::deduplicated>> output) {
        output.set(input.value());
    });
    const auto graph = [&](bg::wp<bg::TS<Price>> input) { return copy(input); };
    const auto first = Price{.quantity = 1, .value = 2.5};
    const auto second = Price{.quantity = 2, .value = 3.5};

    const auto result = bg::testing::test_graph(graph, std::vector{first, first, second});

    CHECK(result.equals<Price>({first, std::nullopt, second}));
}

} // namespace
