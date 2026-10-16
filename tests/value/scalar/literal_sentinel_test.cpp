#include <bg/literal/scalar_literal.hpp>
#include <bg/scalar/literal.hpp>
#include <bg/scalar/scalar.hpp>
#include <bg/scalar/value.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>

#include "../../type/test_shared_literal_sentinel.hpp"

TEST_CASE("literal sentinels have shared constants and canonical scalar identity",
          "[feature:scalar-api][scalar:opaque][declaration:runtime]") {
    CHECK(&literal::SKIP == test_shared_literal_sentinel::skip());
    CHECK(&literal::INVALID == test_shared_literal_sentinel::invalid());
    CHECK(&literal::REMOVE == test_shared_literal_sentinel::remove());
    CHECK(&literal::REMOVED == test_shared_literal_sentinel::removed());
    CHECK(test_shared_literal_sentinel::extension()->name == bg::symbol{"test.literal.extension"});
    CHECK(test_shared_literal_sentinel::extension()->argument == test_shared_literal_sentinel::extension_argument());
    const auto argument_value =
        bg::ScalarValue<bg::i64>::from_literal(*test_shared_literal_sentinel::extension_argument());
    CHECK(argument_value.view().value() == 71);
    CHECK(&bg::cpp_scalar_type<literal::Sentinel>() == test_shared_literal_sentinel::semantic_type());
    CHECK(&bg::cpp_scalar_runtime_type<literal::Sentinel>() == test_shared_literal_sentinel::runtime_type());
}

TEST_CASE("literal sentinel recognition accepts scalar and literal wrappers",
          "[feature:scalar-api][scalar:opaque]") {
    const auto scalar = bg::Scalar<literal::Sentinel>::from(literal::INVALID);
    const auto erased_scalar = bg::Scalar<>::from(scalar);
    const auto wrapped = bg::ScalarValue<literal::Sentinel>{literal::REMOVED};
    const auto scalar_data = bg::details::scalar_data(erased_scalar);

    REQUIRE(literal::Sentinel::from(scalar_data));
    CHECK(literal::Sentinel::from(scalar_data)->name == literal::INVALID.name);
    REQUIRE(literal::Sentinel::from(literal::ScalarLiteral(scalar)));
    CHECK(literal::Sentinel::from(literal::ScalarLiteral(scalar))->name == literal::INVALID.name);
    REQUIRE(literal::Sentinel::from(literal::ScalarLiteral(erased_scalar)));
    CHECK(literal::Sentinel::from(literal::ScalarLiteral(erased_scalar))->name == literal::INVALID.name);
    REQUIRE(literal::Sentinel::from(literal::ScalarLiteral(wrapped)));
    CHECK(literal::Sentinel::from(literal::ScalarLiteral(wrapped))->name == literal::REMOVED.name);
    REQUIRE(literal::Sentinel::from(literal::ScalarLiteral(scalar_data)));
    CHECK(literal::Sentinel::from(literal::ScalarLiteral(scalar_data))->name == literal::INVALID.name);

    const auto ordinary = bg::i64{3};
    CHECK_FALSE(literal::Sentinel::from(literal::ScalarLiteral(ordinary)));
    const auto children = std::array{literal::ScalarLiteral(ordinary)};
    CHECK_FALSE(literal::Sentinel::from(literal::ScalarLiteral{std::span{children}}));
}
