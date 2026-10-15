#include <bg/scalar/dict/type.hpp>
#include <bg/scalar/list/type.hpp>
#include <bg/scalar/mutable_collection.hpp>
#include <bg/scalar/opaque.hpp>
#include <bg/scalar/set/type.hpp>
#include <bg/scalar/value.hpp>

#include <catch2/catch_test_macros.hpp>

#include <concepts>
#include <cstdint>

namespace {

template <typename T>
concept HashableScalarHandle = requires(const T &value) { value.hash(); };

template <typename T>
concept EqualityComparableScalarHandle = requires(const T &left, const T &right) { left == right; };

template <typename T>
concept HasValueMember = requires(const T &value) { value.value(); };

template <typename T>
concept HasArrowOperator = requires(const T &value) { value.operator->(); };

using KEY = bg::ScalarVariable<"KEY", bg::EqualityComparable, bg::Hashable>;

} // namespace

TEST_CASE("scalar variable constraints select handle operations",
          "[value][scalar][capability][feature:scalar-api][scalar:generic]") {
    STATIC_CHECK_FALSE(HashableScalarHandle<bg::Scalar<bg::SCALAR>>);
    STATIC_CHECK_FALSE(EqualityComparableScalarHandle<bg::Scalar<bg::SCALAR>>);
    STATIC_CHECK(HashableScalarHandle<bg::Scalar<KEY>>);
    STATIC_CHECK(EqualityComparableScalarHandle<bg::Scalar<KEY>>);
}

TEST_CASE("scalar collection handles propagate item capabilities",
          "[value][scalar][capability][collection][feature:scalar-api]") {
    STATIC_CHECK(HashableScalarHandle<bg::Scalar<bg::List<KEY>>>);
    STATIC_CHECK(EqualityComparableScalarHandle<bg::Scalar<bg::List<KEY>>>);
    STATIC_CHECK(HashableScalarHandle<bg::Scalar<bg::Set<KEY>>>);
    STATIC_CHECK(EqualityComparableScalarHandle<bg::Scalar<bg::Set<KEY>>>);
    STATIC_CHECK((HashableScalarHandle<bg::Scalar<bg::Dict<KEY, KEY>>>));
    STATIC_CHECK((EqualityComparableScalarHandle<bg::Scalar<bg::Dict<KEY, KEY>>>));
}

TEST_CASE("mutable collection views require a resolved scalar type",
          "[value][scalar][capability][collection][feature:scalar-api]") {
    STATIC_CHECK(bg::details::mutable_scalar<bg::List<bg::i64>>);
    STATIC_CHECK(bg::details::mutable_scalar<bg::Set<bg::i64>>);
    STATIC_CHECK((bg::details::mutable_scalar<bg::Dict<bg::i64, bg::i64>>));

    STATIC_CHECK_FALSE(bg::details::mutable_scalar<bg::List<bg::SCALAR>>);
    STATIC_CHECK_FALSE(bg::details::mutable_scalar<bg::Set<bg::SCALAR>>);
    STATIC_CHECK_FALSE((bg::details::mutable_scalar<bg::Dict<bg::SCALAR, bg::SCALAR>>));
}

TEST_CASE("scalar type interfaces expose only supported operations",
          "[value][scalar][capability][feature:scalar-api]") {
    STATIC_CHECK_FALSE(HashableScalarHandle<bg::Scalar<bg::Opaque<int>>>);
    STATIC_CHECK_FALSE(EqualityComparableScalarHandle<bg::Scalar<bg::Opaque<int>>>);
    STATIC_CHECK_FALSE(HasValueMember<bg::Scalar<bg::List<bg::i64>>>);
    STATIC_CHECK_FALSE(HasValueMember<bg::Scalar<bg::Set<bg::i64>>>);
    STATIC_CHECK_FALSE((HasValueMember<bg::Scalar<bg::Dict<bg::i64, bg::i64>>>));
    STATIC_CHECK_FALSE(HasArrowOperator<bg::Scalar<bg::SCALAR>>);
    STATIC_CHECK_FALSE(HasArrowOperator<bg::Scalar<bg::List<bg::i64>>>);
    STATIC_CHECK_FALSE(HasArrowOperator<bg::Scalar<bg::Set<bg::i64>>>);
    STATIC_CHECK_FALSE((HasArrowOperator<bg::Scalar<bg::Dict<bg::i64, bg::i64>>>));
    STATIC_CHECK_FALSE(std::default_initializable<bg::List<bg::SCALAR>>);
}
