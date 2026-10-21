#include <bg/scalar/dict/type.hpp>
#include <bg/scalar/list/type.hpp>
#include <bg/scalar/mutable_collection.hpp>
#include <bg/scalar/opaque.hpp>
#include <bg/scalar/set/type.hpp>
#include <bg/scalar/value.hpp>

#include <catch2/catch_test_macros.hpp>

#include <concepts>
#include <cstdint>

using namespace bg;

namespace {

template <typename T>
concept HashableScalarHandle = requires(const T &value) { value.hash(); };

template <typename T>
concept EqualityComparableScalarHandle = requires(const T &left, const T &right) { left == right; };

template <typename T>
concept HasValueMember = requires(const T &value) { value.value(); };

template <typename T>
concept HasArrowOperator = requires(const T &value) { value.operator->(); };

using KEY = ScalarVariable<"KEY", EqualityComparable, Hashable>;

} // namespace

TEST_CASE("scalar variable constraints select handle operations",
          "[value][scalar][capability][feature:scalar-api][scalar:generic]") {
    STATIC_CHECK_FALSE(HashableScalarHandle<Scalar<SCALAR>>);
    STATIC_CHECK_FALSE(EqualityComparableScalarHandle<Scalar<SCALAR>>);
    STATIC_CHECK(HashableScalarHandle<Scalar<KEY>>);
    STATIC_CHECK(EqualityComparableScalarHandle<Scalar<KEY>>);
}

TEST_CASE("scalar collection handles propagate item capabilities",
          "[value][scalar][capability][collection][feature:scalar-api]") {
    STATIC_CHECK(HashableScalarHandle<Scalar<List<KEY>>>);
    STATIC_CHECK(EqualityComparableScalarHandle<Scalar<List<KEY>>>);
    STATIC_CHECK(HashableScalarHandle<Scalar<Set<KEY>>>);
    STATIC_CHECK(EqualityComparableScalarHandle<Scalar<Set<KEY>>>);
    STATIC_CHECK((HashableScalarHandle<Scalar<Dict<KEY, KEY>>>));
    STATIC_CHECK((EqualityComparableScalarHandle<Scalar<Dict<KEY, KEY>>>));
}

TEST_CASE("mutable collection views require a resolved scalar type",
          "[value][scalar][capability][collection][feature:scalar-api]") {
    STATIC_CHECK(details::mutable_scalar<List<i64>>);
    STATIC_CHECK(details::mutable_scalar<Set<i64>>);
    STATIC_CHECK((details::mutable_scalar<Dict<i64, i64>>));

    STATIC_CHECK_FALSE(details::mutable_scalar<List<SCALAR>>);
    STATIC_CHECK_FALSE(details::mutable_scalar<Set<SCALAR>>);
    STATIC_CHECK_FALSE((details::mutable_scalar<Dict<SCALAR, SCALAR>>));
}

TEST_CASE("scalar type interfaces expose only supported operations",
          "[value][scalar][capability][feature:scalar-api]") {
    STATIC_CHECK_FALSE(HashableScalarHandle<Scalar<Opaque<int>>>);
    STATIC_CHECK_FALSE(EqualityComparableScalarHandle<Scalar<Opaque<int>>>);
    STATIC_CHECK_FALSE(HasValueMember<Scalar<List<i64>>>);
    STATIC_CHECK_FALSE(HasValueMember<Scalar<Set<i64>>>);
    STATIC_CHECK_FALSE((HasValueMember<Scalar<Dict<i64, i64>>>));
    STATIC_CHECK_FALSE(HasArrowOperator<Scalar<SCALAR>>);
    STATIC_CHECK_FALSE(HasArrowOperator<Scalar<List<i64>>>);
    STATIC_CHECK_FALSE(HasArrowOperator<Scalar<Set<i64>>>);
    STATIC_CHECK_FALSE((HasArrowOperator<Scalar<Dict<i64, i64>>>));
    STATIC_CHECK_FALSE(std::default_initializable<List<SCALAR>>);
}
