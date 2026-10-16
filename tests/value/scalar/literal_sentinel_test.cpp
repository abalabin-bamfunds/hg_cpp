#include <bg/literal/scalar_literal.hpp>
#include <bg/scalar/literal.hpp>
#include <bg/scalar/scalar.hpp>
#include <bg/scalar/value.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <type_traits>

#include "../../type/test_shared_literal_sentinel.hpp"

static_assert(!std::is_copy_constructible_v<literal::Sentinel>);
static_assert(!std::is_copy_assignable_v<literal::Sentinel>);
static_assert(!std::is_move_constructible_v<literal::Sentinel>);
static_assert(!std::is_move_assignable_v<literal::Sentinel>);
static_assert(std::is_copy_constructible_v<literal::TaggedLiteral>);
static_assert(!std::is_copy_assignable_v<literal::TaggedLiteral>);

TEST_CASE("literal sentinels have shared scalar identity", "[feature:scalar-api][scalar:opaque][declaration:runtime]") {
    CHECK(&literal::SKIP == test_shared_literal_sentinel::skip());
    CHECK(&literal::INVALID == test_shared_literal_sentinel::invalid());
    CHECK(&literal::REMOVE == test_shared_literal_sentinel::remove());
    CHECK(&literal::REMOVED == test_shared_literal_sentinel::removed());
    CHECK(test_shared_literal_sentinel::extension()->name == bg::symbol{"test.literal.extension"});
    const auto tagged = literal::TaggedLiteral::from(
        literal::ScalarLiteral(*test_shared_literal_sentinel::extension_call()));
    REQUIRE(tagged);
    CHECK(tagged->sentinel == *test_shared_literal_sentinel::extension());
    CHECK(&tagged->argument == test_shared_literal_sentinel::extension_argument());
    const auto argument_value = bg::ScalarValue<bg::i64>::from_literal(tagged->argument);
    CHECK(argument_value.view().value() == 71);
    CHECK(&bg::cpp_scalar_type<literal::Sentinel>() == test_shared_literal_sentinel::semantic_type());
    CHECK(&bg::cpp_scalar_runtime_type<literal::Sentinel>() == test_shared_literal_sentinel::runtime_type());
    CHECK(&bg::cpp_scalar_type<literal::TaggedLiteral>() == test_shared_literal_sentinel::tagged_semantic_type());
    CHECK(&bg::cpp_scalar_runtime_type<literal::TaggedLiteral>() == test_shared_literal_sentinel::tagged_runtime_type());
}

TEST_CASE("literal sentinel storage keeps the singleton address through copies and erasure",
          "[feature:scalar-api][scalar:opaque]") {
    const auto owned = bg::ScalarValue<literal::Sentinel>{literal::INVALID};
    const auto scalar = owned.view();
    const auto erased_scalar = bg::Scalar<>::from(scalar);
    const auto erased_data = bg::details::scalar_data(erased_scalar);
    const auto copied = bg::ScalarValue<literal::Sentinel>{bg::Scalar<literal::Sentinel>::from(erased_scalar)};
    const auto copied_scalar = copied.view();
    const auto wrapped = bg::ScalarValue<literal::Sentinel>{literal::REMOVED};

    CHECK(&scalar.value() == &literal::INVALID);
    CHECK(&copied_scalar.value() == &literal::INVALID);
    CHECK(scalar.equals(copied_scalar));
    CHECK(scalar.equals(erased_scalar));
    CHECK(scalar.hash() == copied_scalar.hash());
    CHECK(scalar.hash() == erased_scalar.hash());
    CHECK(literal::Sentinel::from(erased_data) == &literal::INVALID);
    CHECK(literal::Sentinel::from(literal::ScalarLiteral(scalar)) == &literal::INVALID);
    CHECK(literal::Sentinel::from(literal::ScalarLiteral(erased_scalar)) == &literal::INVALID);
    CHECK(literal::Sentinel::from(literal::ScalarLiteral(wrapped)) == &literal::REMOVED);

    static const literal::Sentinel same_name_a{bg::symbol{"test.same-name"}};
    static const literal::Sentinel same_name_b{bg::symbol{"test.same-name"}};
    CHECK(same_name_a.name == same_name_b.name);
    CHECK(same_name_a != same_name_b);
    CHECK(literal::Sentinel::from(literal::ScalarLiteral(same_name_a)) == &same_name_a);
    const auto same_name_value_a = bg::ScalarValue<literal::Sentinel>{same_name_a};
    const auto same_name_value_b = bg::ScalarValue<literal::Sentinel>{same_name_b};
    CHECK_FALSE(same_name_value_a.view().equals(same_name_value_b.view()));

    const auto ordinary = bg::i64{3};
    CHECK(literal::Sentinel::from(literal::ScalarLiteral(ordinary)) == nullptr);
    const auto children = std::array{literal::ScalarLiteral(ordinary)};
    CHECK(literal::Sentinel::from(literal::ScalarLiteral{std::span{children}}) == nullptr);
}

TEST_CASE("tagged literals retain borrowed sentinel and argument references",
          "[feature:scalar-api][scalar:opaque]") {
    const auto &call = *test_shared_literal_sentinel::extension_call();
    const auto scalar_value = bg::ScalarValue<literal::TaggedLiteral>{call};
    const auto copied_value = bg::ScalarValue<literal::TaggedLiteral>{scalar_value.view()};
    const auto copied_call = copied_value.view().value();

    CHECK(&copied_call.sentinel == test_shared_literal_sentinel::extension());
    CHECK(&copied_call.argument == test_shared_literal_sentinel::extension_argument());

    const auto tagged_leaf = literal::ScalarLiteral(call);
    const auto nested_items = std::array{tagged_leaf};
    const auto nested_payload = literal::ScalarLiteral{std::span{nested_items}};
    const auto outer_items = std::array{nested_payload};
    const auto outer = literal::ScalarLiteral{std::span{outer_items}};
    const auto nested = outer.visit(
        [](const literal::ScalarLiteralItem &) { return std::span<const literal::ScalarLiteral>{}; },
        [](std::span<const literal::ScalarLiteral> values) { return values; }).value();
    const auto payload = nested.front().visit(
        [](const literal::ScalarLiteralItem &) { return std::span<const literal::ScalarLiteral>{}; },
        [](std::span<const literal::ScalarLiteral> values) { return values; }).value();
    const auto tagged = literal::TaggedLiteral::from(payload.front());

    REQUIRE(tagged);
    CHECK(tagged->sentinel == *test_shared_literal_sentinel::extension());
    CHECK(&tagged->argument == test_shared_literal_sentinel::extension_argument());
    CHECK_FALSE(literal::TaggedLiteral::from(outer));
}
