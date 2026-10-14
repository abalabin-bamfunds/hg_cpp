#include <bg/time_series/size_variable.hpp>
#include <bg/time_series/tsd/tsd_type.hpp>
#include <bg/time_series/time_series_type.hpp>
#include <bg/time_series/tsl/tsl_type.hpp>
#include <bg/scalar/dict/type.hpp>
#include <bg/type/scalar_type.hpp>
#include <bg/type/scalar_variable.hpp>
#include <bg/type/signature.hpp>
#include <bg/type/type_of.hpp>
#include <bg/type/time_series_reference_type.hpp>
#include <bg/type/time_series_value_type.hpp>
#include <bg/type/type_variable.hpp>
#include <bg/graph/graph_argument.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <compare>
#include <vector>

TEST_CASE("an exact parameter is less generic than a type variable",
          "[feature:operator][feature:type-system][declaration:generic]") {
    const auto exact = bg::Signature{bg::util::symbol{"exact"}, {{bg::util::symbol{"value"}, &bg::int_type()}}};
    using T = bg::ScalarVariable<"T">;
    const auto &variable = T::type();
    const auto generic = bg::Signature{bg::util::symbol{"generic"}, {{bg::util::symbol{"value"}, &variable}}};

    CHECK(bg::compare_specificity(exact, generic) == std::partial_ordering::less);
    CHECK(bg::compare_specificity(generic, exact) == std::partial_ordering::greater);
}

TEST_CASE("a constrained variable is less generic than an unconstrained variable",
          "[feature:operator][feature:type-system][declaration:generic]") {
    using NUMBER = bg::ScalarVariable<"NUMBER", int, float>;
    using SCALAR = bg::ScalarVariable<"Scalar">;
    const auto &number = NUMBER::type();
    const auto &scalar = SCALAR::type();
    const auto constrained = bg::Signature{bg::util::symbol{"number"}, {{bg::util::symbol{"value"}, &number}}};
    const auto unconstrained = bg::Signature{bg::util::symbol{"scalar"}, {{bg::util::symbol{"value"}, &scalar}}};

    CHECK(bg::compare_specificity(constrained, unconstrained) == std::partial_ordering::less);
}

TEST_CASE("a repeated variable is less generic than independent variables",
          "[feature:operator][feature:type-system][declaration:generic]") {
    using T = bg::ScalarVariable<"T">;
    using L = bg::ScalarVariable<"L">;
    using R = bg::ScalarVariable<"R">;
    const auto &repeated = T::type();
    const auto &left = L::type();
    const auto &right = R::type();
    const auto same = bg::Signature{bg::util::symbol{"same"},
                                    {{bg::util::symbol{"left"}, &repeated}, {bg::util::symbol{"right"}, &repeated}}};
    const auto independent = bg::Signature{bg::util::symbol{"independent"},
                                           {{bg::util::symbol{"left"}, &left}, {bg::util::symbol{"right"}, &right}}};

    CHECK(bg::compare_specificity(same, independent) == std::partial_ordering::less);
    CHECK(bg::compare_specificity(independent, same) == std::partial_ordering::greater);
}

TEST_CASE("same time-series declarations retain repeated-variable specificity",
          "[feature:operator][feature:type-system][declaration:generic][series:value]") {
    using T = bg::ScalarVariable<"T">;
    using R = bg::ScalarVariable<"R">;

    const auto repeated = bg::Signature{
        bg::util::symbol{"repeated"},
        {{bg::util::symbol{"first"}, &bg::TS<T>::type()}, {bg::util::symbol{"second"}, &bg::TS<T>::type()}}};
    const auto independent = bg::Signature{
        bg::util::symbol{"independent"},
        {{bg::util::symbol{"first"}, &bg::TS<T>::type()}, {bg::util::symbol{"second"}, &bg::TS<R>::type()}}};

    CHECK(bg::compare_specificity(repeated, independent) == std::partial_ordering::less);
    CHECK(bg::compare_specificity(independent, repeated) == std::partial_ordering::greater);
}

TEST_CASE("repeated variables contribute specificity as one declaration relationship",
          "[feature:operator][feature:type-system][declaration:generic]") {
    using T = bg::ScalarVariable<"T">;
    using R = bg::ScalarVariable<"R">;

    const auto repeated = bg::Signature{
        bg::util::symbol{"repeated"},
        {{bg::util::symbol{"first"}, &T::type()}, {bg::util::symbol{"second"}, &T::type()}}};
    const auto partly_concrete = bg::Signature{
        bg::util::symbol{"partly_concrete"},
        {{bg::util::symbol{"first"}, &bg::int64_type()}, {bg::util::symbol{"second"}, &R::type()}}};
    const auto concrete = bg::Signature{
        bg::util::symbol{"concrete"},
        {{bg::util::symbol{"first"}, &bg::int64_type()}, {bg::util::symbol{"second"}, &bg::int64_type()}}};

    CHECK(bg::compare_specificity(repeated, partly_concrete) == std::partial_ordering::unordered);
    CHECK(bg::compare_specificity(partly_concrete, repeated) == std::partial_ordering::unordered);
    CHECK(bg::compare_specificity(concrete, repeated) == std::partial_ordering::less);
}

TEST_CASE("constraints on different parameters are unordered",
          "[feature:operator][feature:type-system][declaration:generic]") {
    using L = bg::ScalarVariable<"L">;
    using R = bg::ScalarVariable<"R">;
    const auto &any_left = L::type();
    const auto &any_right = R::type();
    const auto left_exact =
        bg::Signature{bg::util::symbol{"left_exact"},
                      {{bg::util::symbol{"left"}, &bg::int_type()}, {bg::util::symbol{"right"}, &any_right}}};
    const auto right_exact =
        bg::Signature{bg::util::symbol{"right_exact"},
                      {{bg::util::symbol{"left"}, &any_left}, {bg::util::symbol{"right"}, &bg::int_type()}}};

    CHECK(bg::compare_specificity(left_exact, right_exact) == std::partial_ordering::unordered);
}

TEST_CASE("a stronger literal guarantee is less generic",
          "[feature:operator][feature:type-system][series:value][guarantee:final]") {
    const auto &final = bg::TS<bg::i64, bg::TimeSeriesGuarantee::final>::type();
    const auto &ordinary = bg::TS<bg::i64>::type();
    const auto final_signature = bg::Signature{bg::util::symbol{"final"}, {{bg::util::symbol{"value"}, &final}}};
    const auto ordinary_signature =
        bg::Signature{bg::util::symbol{"ordinary"}, {{bg::util::symbol{"value"}, &ordinary}}};

    CHECK(bg::compare_specificity(final_signature, ordinary_signature) == std::partial_ordering::less);
}

TEST_CASE("a literal guarantee is more specific than a variable with the same minimum",
          "[feature:operator][feature:type-system][declaration:generic][series:value]"
          "[guarantee:deduplicated]") {
    constexpr auto G = bg::guarantee_variable<"SPECIFICITY_G", bg::TimeSeriesGuarantee::deduplicated>;
    constexpr auto G_NO_MINIMUM = bg::guarantee_variable<"SPECIFICITY_G">;
    const auto literal = bg::Signature{
        bg::util::symbol{"literal"},
        {{bg::util::symbol{"value"}, &bg::TS<bg::i64, bg::TimeSeriesGuarantee::deduplicated>::type()}}};
    const auto constrained = bg::Signature{
        bg::util::symbol{"constrained"}, {{bg::util::symbol{"value"}, &bg::TS<bg::i64, G>::type()}}};
    const auto unconstrained = bg::Signature{
        bg::util::symbol{"unconstrained"}, {{bg::util::symbol{"value"}, &bg::TS<bg::i64, G_NO_MINIMUM>::type()}}};

    CHECK(bg::compare_specificity(literal, constrained) == std::partial_ordering::less);
    CHECK(bg::compare_specificity(constrained, literal) == std::partial_ordering::greater);
    CHECK(bg::compare_specificity(constrained, unconstrained) == std::partial_ordering::less);
}

TEST_CASE("reference shape does not affect specificity",
          "[feature:operator][feature:type-system][feature:reference][binding:reference-to-value]") {
    const auto &value = bg::TS<bg::i64>::type();
    const auto &reference = bg::REF<bg::TS<bg::i64>>::type();
    const auto value_signature = bg::Signature{bg::util::symbol{"value"}, {{bg::util::symbol{"value"}, &value}}};
    const auto reference_signature =
        bg::Signature{bg::util::symbol{"reference"}, {{bg::util::symbol{"value"}, &reference}}};

    CHECK(bg::compare_specificity(value_signature, reference_signature) == std::partial_ordering::equivalent);
}

TEST_CASE("a concrete time series is less generic than a time-series variable",
          "[feature:operator][feature:type-system][declaration:generic][series:value]") {
    const auto concrete =
        bg::Signature{bg::util::symbol{"concrete"}, {{bg::util::symbol{"value"}, &bg::TS<bg::i64>::type()}}};
    const auto generic = bg::Signature{bg::util::symbol{"generic"}, {{bg::util::symbol{"value"}, &bg::TS_T::type()}}};

    CHECK(bg::compare_specificity(concrete, generic) == std::partial_ordering::less);
    CHECK(bg::compare_specificity(generic, concrete) == std::partial_ordering::greater);
}

TEST_CASE("a literal list size is less generic than a size variable",
          "[feature:operator][feature:type-system][declaration:generic][series:fixed-tsl]") {
    const auto fixed =
        bg::Signature{bg::util::symbol{"fixed"}, {{bg::util::symbol{"value"}, &bg::TSL<bg::TS<bg::i64>, 3>::type()}}};
    const auto generic = bg::Signature{bg::util::symbol{"generic"},
                                       {{bg::util::symbol{"value"}, &bg::TSL<bg::TS<bg::i64>, bg::SIZE>::type()}}};

    CHECK(bg::compare_specificity(fixed, generic) == std::partial_ordering::less);
    CHECK(bg::compare_specificity(generic, fixed) == std::partial_ordering::greater);
}

TEST_CASE("size specificity compares declaration relationships, not numeric order",
          "[feature:operator][feature:type-system][declaration:generic][series:fixed-tsl]") {
    using M = bg::SizeVariableDeclaration<"M">;
    using SAME_SIZE = bg::TSL<bg::TS<bg::i64>, bg::SIZE>;
    using SIZE_THREE = bg::TSL<bg::TS<bg::i64>, 3>;
    using SIZE_M = bg::TSL<bg::TS<bg::i64>, M{}>;

    const auto repeated = bg::Signature{
        bg::util::symbol{"repeated"},
        {{bg::util::symbol{"first"}, &SAME_SIZE::type()}, {bg::util::symbol{"second"}, &SAME_SIZE::type()}}};
    const auto three_and_independent = bg::Signature{
        bg::util::symbol{"three_and_independent"},
        {{bg::util::symbol{"first"}, &SIZE_THREE::type()}, {bg::util::symbol{"second"}, &SIZE_M::type()}}};
    const auto fixed = bg::Signature{
        bg::util::symbol{"fixed"},
        {{bg::util::symbol{"first"}, &SIZE_THREE::type()}, {bg::util::symbol{"second"}, &SIZE_THREE::type()}}};

    CHECK(bg::compare_specificity(repeated, three_and_independent) == std::partial_ordering::unordered);
    CHECK(bg::compare_specificity(three_and_independent, repeated) == std::partial_ordering::unordered);
    CHECK(bg::compare_specificity(fixed, repeated) == std::partial_ordering::less);
}

TEST_CASE("unrelated scalar capability constraints are unordered",
          "[feature:operator][feature:type-system][declaration:generic]") {
    using EQUALITY_SCALAR = bg::ScalarVariable<"EQUALITY_SCALAR", bg::EqualityComparable>;
    using HASH_SCALAR = bg::ScalarVariable<"HASH_SCALAR", bg::Hashable>;

    const auto equality =
        bg::Signature{bg::util::symbol{"equality"}, {{bg::util::symbol{"value"}, &EQUALITY_SCALAR::type()}}};
    const auto hash = bg::Signature{bg::util::symbol{"hash"}, {{bg::util::symbol{"value"}, &HASH_SCALAR::type()}}};

    CHECK(bg::compare_specificity(equality, hash) == std::partial_ordering::unordered);
    CHECK(bg::compare_specificity(hash, equality) == std::partial_ordering::unordered);
}

TEST_CASE("identical signatures have equivalent specificity", "[feature:operator][feature:type-system]") {
    const auto left = bg::Signature{bg::util::symbol{"left"}, {{bg::util::symbol{"value"}, &bg::TS<bg::i64>::type()}}};
    const auto right = bg::Signature{bg::util::symbol{"right"}, {{bg::util::symbol{"other"}, &bg::TS<bg::i64>::type()}}};

    CHECK(bg::compare_specificity(left, right) == std::partial_ordering::equivalent);
}

TEST_CASE("a union accepting a subset of types is less generic",
          "[feature:operator][feature:type-system][declaration:generic]") {
    using NUMBER = bg::ScalarVariable<"NUMBER">;
    using NUMERIC = bg::Union<"Numeric", bg::i64, float>;
    using SCALAR = bg::Union<"Scalar", NUMBER>;

    const auto &numeric = NUMERIC::type();
    const auto &scalar = SCALAR::type();
    const auto numeric_signature = bg::Signature{bg::util::symbol{"numeric"}, {{bg::util::symbol{"value"}, &numeric}}};
    const auto scalar_signature = bg::Signature{bg::util::symbol{"scalar"}, {{bg::util::symbol{"value"}, &scalar}}};

    CHECK(bg::compare_specificity(numeric_signature, scalar_signature) == std::partial_ordering::less);
    CHECK(bg::compare_specificity(scalar_signature, numeric_signature) == std::partial_ordering::greater);
}

TEST_CASE("a concrete time series is more specific than a scalar-or-time-series union",
          "[feature:operator][feature:type-system][declaration:generic][series:value]") {
    using PROMOTED = bg::Union<"PROMOTED", bg::i64,
                               bg::TS<bg::i64, bg::guarantee_variable<"G">>>;
    const auto &promoted = PROMOTED::type();
    const auto promoted_signature =
        bg::Signature{bg::util::symbol{"promoted"}, {{bg::util::symbol{"value"}, &promoted}}};
    const auto time_series_signature =
        bg::Signature{bg::util::symbol{"time_series"},
                      {{bg::util::symbol{"value"}, &bg::TS<bg::i64, bg::guarantee_variable<"G">>::type()}}};

    CHECK(bg::compare_specificity(time_series_signature, promoted_signature) == std::partial_ordering::less);
    CHECK(bg::compare_specificity(promoted_signature, time_series_signature) == std::partial_ordering::greater);

    const auto other_time_series_signature = bg::Signature{
        bg::util::symbol{"other_time_series"},
        {{bg::util::symbol{"value"}, &bg::TS<float, bg::guarantee_variable<"G">>::type()}}};
    CHECK(bg::compare_specificity(other_time_series_signature, promoted_signature) ==
          std::partial_ordering::unordered);
    CHECK(bg::compare_specificity(promoted_signature, other_time_series_signature) ==
          std::partial_ordering::unordered);
}

TEST_CASE("partially overlapping unions are unordered",
          "[feature:operator][feature:type-system][declaration:generic]") {
    using LEFT = bg::Union<"Left", bg::i64, float>;
    using RIGHT = bg::Union<"Right", float, double>;
    const auto &left = LEFT::type();
    const auto &right = RIGHT::type();
    const auto left_signature = bg::Signature{bg::util::symbol{"left"}, {{bg::util::symbol{"value"}, &left}}};
    const auto right_signature = bg::Signature{bg::util::symbol{"right"}, {{bg::util::symbol{"value"}, &right}}};

    CHECK(bg::compare_specificity(left_signature, right_signature) == std::partial_ordering::unordered);
    CHECK(bg::compare_specificity(right_signature, left_signature) == std::partial_ordering::unordered);
}

TEST_CASE("specificity preserves guarantee-variable relationships through promoted alternatives",
          "[feature:operator][feature:type-system][declaration:generic]") {
    using PROMOTED_G1 = bg::Union<"PROMOTED_G1", bg::i64,
                                  bg::TS<bg::i64, bg::guarantee_variable<"G1">>>;
    using PROMOTED_G2 = bg::Union<"PROMOTED_G2", bg::i64,
                                  bg::TS<bg::i64, bg::guarantee_variable<"G2">>>;
    constexpr auto G = bg::guarantee_variable<"G">;

    const auto shared_guarantee = bg::Signature{
        bg::util::symbol{"shared_guarantee"},
        {{bg::util::symbol{"first"}, &bg::TS<bg::i64, G>::type()},
         {bg::util::symbol{"second"}, &bg::TS<bg::i64, G>::type()}}};
    const auto independent_guarantees = bg::Signature{
        bg::util::symbol{"independent_guarantees"},
        {{bg::util::symbol{"first"}, &PROMOTED_G1::type()},
         {bg::util::symbol{"second"}, &PROMOTED_G2::type()}}};

    CHECK(bg::compare_specificity(shared_guarantee, independent_guarantees) == std::partial_ordering::less);
    CHECK(bg::compare_specificity(independent_guarantees, shared_guarantee) == std::partial_ordering::greater);
}

TEST_CASE("independent guarantee variables do not impose a repeated guarantee requirement",
          "[feature:operator][feature:type-system][declaration:generic][series:value]") {
    constexpr auto G1 = bg::guarantee_variable<"G1">;
    constexpr auto G2 = bg::guarantee_variable<"G2">;
    constexpr auto G = bg::guarantee_variable<"G">;

    const auto repeated = bg::Signature{
        bg::util::symbol{"repeated"},
        {{bg::util::symbol{"first"}, &bg::TS<bg::i64, G>::type()},
         {bg::util::symbol{"second"}, &bg::TS<bg::i64, G>::type()}}};
    const auto independent = bg::Signature{
        bg::util::symbol{"independent"},
        {{bg::util::symbol{"first"}, &bg::TS<bg::i64, G1>::type()},
         {bg::util::symbol{"second"}, &bg::TS<bg::i64, G2>::type()}}};

    CHECK(bg::compare_specificity(repeated, independent) == std::partial_ordering::less);
}

TEST_CASE("invariant dictionary keys do not become projected key variables",
          "[feature:operator][feature:type-system][declaration:generic][series:tsd]") {
    using DICTIONARY_ONE = bg::Union<"DICTIONARY_ONE", bg::TSD<bg::i64, bg::TS<bg::i64>>,
                                     bg::TSD<bg::i64, bg::TS<double>>>;
    using DICTIONARY_TWO = bg::Union<"DICTIONARY_TWO", bg::TSD<bg::i64, bg::TS<bg::i64>>,
                                     bg::TSD<bg::i64, bg::TS<double>>>;
    using KEY = bg::ScalarVariable<"KEY">;
    using VALUE = bg::ScalarVariable<"VALUE">;
    using OTHER_VALUE = bg::ScalarVariable<"OTHER_VALUE">;

    const auto unions = bg::Signature{
        bg::util::symbol{"unions"},
        {{bg::util::symbol{"first"}, &DICTIONARY_ONE::type()},
         {bg::util::symbol{"second"}, &DICTIONARY_TWO::type()}}};
    const auto repeated_key = bg::Signature{
        bg::util::symbol{"repeated_key"},
        {{bg::util::symbol{"first"}, &bg::TSD<KEY, bg::TS<VALUE>>::type()},
         {bg::util::symbol{"second"}, &bg::TSD<KEY, bg::TS<OTHER_VALUE>>::type()}}};
    const auto supplied = std::array<const bg::Type *, 2>{
        &bg::TSD<bg::i64, bg::TS<bg::i64>>::type(), &bg::TSD<bg::i64, bg::TS<bg::i64>>::type()};

    CHECK(bg::details::compare_parameter_specificity(unions, repeated_key, supplied, supplied, 2) ==
          std::partial_ordering::less);
}

TEST_CASE("ambiguous union narrowing falls back to declaration comparison",
          "[feature:operator][feature:type-system][declaration:generic]") {
    using T = bg::ScalarVariable<"T">;
    using R = bg::ScalarVariable<"R">;
    using DICTIONARY_UNION = bg::Union<"DICTIONARY_UNION", bg::Dict<T, R>, bg::Dict<R, T>>;
    using KEY = bg::ScalarVariable<"KEY">;
    using VALUE = bg::ScalarVariable<"VALUE">;

    const auto union_signature = bg::Signature{
        bg::util::symbol{"union"}, {{bg::util::symbol{"value"}, &DICTIONARY_UNION::type()}}};
    const auto generic_signature = bg::Signature{
        bg::util::symbol{"generic"}, {{bg::util::symbol{"value"}, &bg::cpp_scalar_type<bg::Dict<KEY, VALUE>>()}}};
    const auto supplied = std::array<const bg::Type *, 1>{&bg::cpp_scalar_type<bg::Dict<bg::i64, double>>()};
    auto type_map = bg::TypeMap{};
    REQUIRE(T::type().match(bg::int64_type(), type_map));
    REQUIRE(R::type().match(bg::double_type(), type_map));
    REQUIRE_NOTHROW(union_signature.resolve_with_type_map(supplied, type_map));

    CHECK_NOTHROW(bg::details::compare_parameter_specificity(union_signature, generic_signature, supplied, supplied,
                                                             1));
}

TEST_CASE("contextual specificity orders a concrete type, its union, and constrained types",
          "[feature:operator][feature:type-system][declaration:generic]") {
    using PROMOTED = bg::Union<"PROMOTED", bg::i64, bg::TS<bg::i64>>;
    using EQUALITY_SCALAR = bg::ScalarVariable<"EQUALITY_SCALAR", bg::EqualityComparable>;
    using SCALAR = bg::ScalarVariable<"SCALAR">;

    const auto concrete = bg::Signature{bg::util::symbol{"concrete"},
                                        {{bg::util::symbol{"value"}, &bg::TS<bg::i64>::type()}}};
    const auto promoted = bg::Signature{bg::util::symbol{"promoted"},
                                        {{bg::util::symbol{"value"}, &PROMOTED::type()}}};
    const auto equality = bg::Signature{
        bg::util::symbol{"equality"},
        {{bg::util::symbol{"value"}, &bg::TS<EQUALITY_SCALAR>::type()}}};
    const auto scalar = bg::Signature{bg::util::symbol{"scalar"},
                                      {{bg::util::symbol{"value"}, &bg::TS<SCALAR>::type()}}};
    const auto supplied = std::array<const bg::Type *, 1>{&bg::TS<bg::i64>::type()};
    const auto matches_supplied = [&](const bg::Type &pattern) {
        auto mapping = bg::TypeMap{};
        return pattern.match(*supplied[0], mapping);
    };
    REQUIRE(matches_supplied(bg::TS<bg::i64>::type()));
    REQUIRE(matches_supplied(PROMOTED::type()));
    REQUIRE(matches_supplied(bg::TS<EQUALITY_SCALAR>::type()));
    REQUIRE(matches_supplied(bg::TS<SCALAR>::type()));
    REQUIRE_NOTHROW(concrete.resolve(supplied));
    REQUIRE_NOTHROW(promoted.resolve(supplied));
    REQUIRE_NOTHROW(equality.resolve(supplied));
    REQUIRE_NOTHROW(scalar.resolve(supplied));
    const auto compare = [&](const auto &left, const auto &right) {
        return bg::details::compare_parameter_specificity(left, right, supplied, supplied, 1);
    };

    CHECK(compare(concrete, promoted) == std::partial_ordering::less);
    CHECK(compare(promoted, equality) == std::partial_ordering::less);
    CHECK(compare(equality, scalar) == std::partial_ordering::less);
    CHECK(compare(concrete, scalar) == std::partial_ordering::less);
}

TEST_CASE("contextual specificity preserves partial overlap and conflicting alternatives",
          "[feature:operator][feature:type-system][declaration:generic]") {
    using LEFT = bg::Union<"LEFT", bg::i64, float>;
    using RIGHT = bg::Union<"RIGHT", float, double>;
    using MIXED_LEFT = bg::Union<"MIXED_LEFT", bg::SCALAR, bg::i64>;
    using EQUALITY_SCALAR = bg::ScalarVariable<"EQUALITY_SCALAR", bg::EqualityComparable>;
    using HASH_SCALAR = bg::ScalarVariable<"HASH_SCALAR", bg::Hashable>;
    using MIXED_RIGHT = bg::Union<"MIXED_RIGHT", EQUALITY_SCALAR, HASH_SCALAR>;

    const auto left = bg::Signature{bg::util::symbol{"left"}, {{bg::util::symbol{"value"}, &LEFT::type()}}};
    const auto right = bg::Signature{bg::util::symbol{"right"}, {{bg::util::symbol{"value"}, &RIGHT::type()}}};
    const auto mixed_left =
        bg::Signature{bg::util::symbol{"mixed_left"}, {{bg::util::symbol{"value"}, &MIXED_LEFT::type()}}};
    const auto mixed_right =
        bg::Signature{bg::util::symbol{"mixed_right"}, {{bg::util::symbol{"value"}, &MIXED_RIGHT::type()}}};
    const auto supplied_float = std::array<const bg::Type *, 1>{&bg::float_type()};
    const auto supplied_integer = std::array<const bg::Type *, 1>{&bg::int64_type()};
    auto mixed_left_mapping = bg::TypeMap{};
    auto mixed_right_mapping = bg::TypeMap{};
    REQUIRE(MIXED_LEFT::type().match(*supplied_integer[0], mixed_left_mapping));
    REQUIRE(MIXED_RIGHT::type().match(*supplied_integer[0], mixed_right_mapping));

    CHECK(bg::details::compare_parameter_specificity(left, right, supplied_float, supplied_float, 1) ==
          std::partial_ordering::unordered);
    CHECK(bg::details::compare_parameter_specificity(right, left, supplied_float, supplied_float, 1) ==
          std::partial_ordering::unordered);
    CHECK(bg::details::compare_parameter_specificity(mixed_left, mixed_right, supplied_integer, supplied_integer, 1) ==
          std::partial_ordering::unordered);
    CHECK(bg::details::compare_parameter_specificity(mixed_right, mixed_left, supplied_integer, supplied_integer, 1) ==
          std::partial_ordering::unordered);
}

TEST_CASE("contextual narrowing retains repeated-variable relationships",
          "[feature:operator][feature:type-system][declaration:generic]") {
    using LEFT = bg::Union<"LEFT", bg::i64, double>;
    using RIGHT = bg::Union<"RIGHT", bg::i64, double>;
    using T = bg::ScalarVariable<"T">;

    const auto independent = bg::Signature{
        bg::util::symbol{"independent"},
        {{bg::util::symbol{"first"}, &LEFT::type()}, {bg::util::symbol{"second"}, &RIGHT::type()}}};
    const auto repeated = bg::Signature{
        bg::util::symbol{"repeated"},
        {{bg::util::symbol{"first"}, &T::type()}, {bg::util::symbol{"second"}, &T::type()}}};
    const auto supplied = std::array<const bg::Type *, 2>{&bg::int64_type(), &bg::int64_type()};
    const auto matches_supplied = [&](const bg::Type &pattern, const bg::Type &argument) {
        auto mapping = bg::TypeMap{};
        return pattern.match(argument, mapping);
    };
    REQUIRE(matches_supplied(LEFT::type(), *supplied[0]));
    REQUIRE(matches_supplied(RIGHT::type(), *supplied[1]));
    REQUIRE(matches_supplied(T::type(), *supplied[0]));
    REQUIRE(matches_supplied(T::type(), *supplied[1]));
    REQUIRE_NOTHROW(independent.resolve(supplied));
    REQUIRE_NOTHROW(repeated.resolve(supplied));

    CHECK(bg::details::compare_parameter_specificity(independent, repeated, supplied, supplied, 2) ==
          std::partial_ordering::unordered);
    CHECK(bg::details::compare_parameter_specificity(repeated, independent, supplied, supplied, 2) ==
          std::partial_ordering::unordered);
}

TEST_CASE("contextual narrowing preserves nested repeated-variable relationships",
          "[feature:operator][feature:type-system][declaration:generic][series:value]") {
    using LEFT = bg::Union<"LEFT", bg::TS<bg::i64>, bg::TS<double>>;
    using RIGHT = bg::Union<"RIGHT", bg::TS<bg::i64>, bg::TS<double>>;
    using T = bg::ScalarVariable<"T">;

    const auto independent = bg::Signature{
        bg::util::symbol{"independent"},
        {{bg::util::symbol{"first"}, &LEFT::type()}, {bg::util::symbol{"second"}, &RIGHT::type()}}};
    const auto repeated = bg::Signature{
        bg::util::symbol{"repeated"},
        {{bg::util::symbol{"first"}, &bg::TS<T>::type()}, {bg::util::symbol{"second"}, &bg::TS<T>::type()}}};
    const auto supplied = std::array<const bg::Type *, 2>{&bg::TS<bg::i64>::type(), &bg::TS<bg::i64>::type()};
    const auto matches_supplied = [&](const bg::Type &pattern, const bg::Type &argument) {
        auto mapping = bg::TypeMap{};
        return pattern.match(argument, mapping);
    };
    REQUIRE(matches_supplied(LEFT::type(), *supplied[0]));
    REQUIRE(matches_supplied(RIGHT::type(), *supplied[1]));
    REQUIRE(matches_supplied(*repeated.parameter(0).type, *supplied[0]));
    REQUIRE(matches_supplied(*repeated.parameter(1).type, *supplied[1]));
    REQUIRE_NOTHROW(independent.resolve(supplied));
    REQUIRE_NOTHROW(repeated.resolve(supplied));

    CHECK(bg::details::compare_parameter_specificity(independent, repeated, supplied, supplied, 2) ==
          std::partial_ordering::unordered);
    CHECK(bg::details::compare_parameter_specificity(repeated, independent, supplied, supplied, 2) ==
          std::partial_ordering::unordered);
}

TEST_CASE("nested type specificity is consistent across a declaration matrix",
          "[feature:operator][feature:type-system][declaration:generic][series:value]") {
    using T = bg::ScalarVariable<"T">;
    using R = bg::ScalarVariable<"R">;
    using SCALAR = bg::ScalarVariable<"SCALAR">;
    using TIME_SERIES_CHOICE = bg::Union<"TIME_SERIES_CHOICE", bg::TS<bg::i64>, bg::TS<double>>;

    const auto declarations = std::array<const bg::Type *, 6>{
        &bg::TS<bg::i64>::type(), &bg::TS<T>::type(), &bg::TS<R>::type(), &TIME_SERIES_CHOICE::type(),
        &bg::TS<SCALAR>::type(), &bg::TS_T::type()};
    const auto supplied = std::array<const bg::Type *, 2>{&bg::TS<bg::i64>::type(), &bg::TS<bg::i64>::type()};
    auto signatures = std::vector<bg::Signature>{};
    for (const auto *first : declarations) {
        for (const auto *second : declarations) {
            signatures.emplace_back(bg::util::symbol{"candidate"},
                                    std::vector<bg::Parameter>{{bg::util::symbol{"first"}, first},
                                                               {bg::util::symbol{"second"}, second}});
            REQUIRE_NOTHROW(signatures.back().resolve(supplied));
        }
    }

    auto specificity = std::vector<std::vector<std::partial_ordering>>(
        signatures.size(), std::vector<std::partial_ordering>(signatures.size(), std::partial_ordering::equivalent));
    for (auto left = std::size_t{}; left < signatures.size(); ++left) {
        for (auto right = std::size_t{}; right < signatures.size(); ++right) {
            specificity[left][right] = bg::details::compare_parameter_specificity(
                signatures[left], signatures[right], supplied, supplied, 2);
        }
    }

    for (auto left = std::size_t{}; left < signatures.size(); ++left) {
        for (auto right = std::size_t{}; right < signatures.size(); ++right) {
            const auto reverse = specificity[left][right] == std::partial_ordering::less
                                     ? std::partial_ordering::greater
                                 : specificity[left][right] == std::partial_ordering::greater
                                     ? std::partial_ordering::less
                                     : specificity[left][right];
            CHECK(specificity[right][left] == reverse);
        }
    }

    for (auto first = std::size_t{}; first < signatures.size(); ++first) {
        for (auto second = std::size_t{}; second < signatures.size(); ++second) {
            if (specificity[first][second] != std::partial_ordering::less) {
                continue;
            }
            for (auto third = std::size_t{}; third < signatures.size(); ++third) {
                if (specificity[second][third] == std::partial_ordering::less) {
                    CHECK(specificity[first][third] == std::partial_ordering::less);
                }
            }
        }
    }
}

TEST_CASE("projected graph parameters ignore alternatives with different boundary shapes",
          "[feature:operator][feature:type-system][declaration:partially-generic][shape:nested]") {
    const auto one_parameter = std::array<const bg::Type *, 1>{&bg::TS<bg::i64>::type()};
    const auto two_parameters = std::array<const bg::Type *, 2>{&bg::TS<bg::i64>::type(), &bg::TS<double>::type()};
    const auto &one_parameter_graph = bg::graph_argument_type(one_parameter, &bg::TS<bg::i64>::type());
    const auto &two_parameter_graph = bg::graph_argument_type(two_parameters, &bg::TS<bg::i64>::type());
    const auto graph_union = bg::TypeVariableType<bg::Type>{
        bg::util::symbol{"GRAPH"}, {&one_parameter_graph, &two_parameter_graph}};
    const auto generic = bg::Signature{bg::util::symbol{"generic"}, {{bg::util::symbol{"function"}, &graph_union}}};
    const auto concrete =
        bg::Signature{bg::util::symbol{"concrete"}, {{bg::util::symbol{"function"}, &one_parameter_graph}}};
    const auto supplied = std::array<const bg::Type *, 1>{&one_parameter_graph};

    REQUIRE_NOTHROW(generic.resolve(supplied));
    CHECK(bg::details::compare_parameter_specificity(concrete, generic, supplied, supplied, 1) ==
          std::partial_ordering::less);
}

TEST_CASE("extension predicates retain conservative function-identity ordering",
          "[feature:operator][feature:type-system][declaration:generic][scalar:generic]") {
    const auto accepts_any = +[](const bg::ScalarType &) noexcept { return true; };
    const auto also_accepts_any = +[](const bg::ScalarType &) noexcept { return true; };
    const auto first = bg::TypeVariableType<bg::ScalarType>{bg::util::symbol{"FIRST"}, {}, {accepts_any}};
    const auto same_predicate =
        bg::TypeVariableType<bg::ScalarType>{bg::util::symbol{"SAME_PREDICATE"}, {}, {accepts_any}};
    const auto other_predicate =
        bg::TypeVariableType<bg::ScalarType>{bg::util::symbol{"OTHER_PREDICATE"}, {}, {also_accepts_any}};
    const auto first_signature =
        bg::Signature{bg::util::symbol{"first"}, {{bg::util::symbol{"value"}, &first}}};
    const auto same_signature =
        bg::Signature{bg::util::symbol{"same"}, {{bg::util::symbol{"value"}, &same_predicate}}};
    const auto other_signature =
        bg::Signature{bg::util::symbol{"other"}, {{bg::util::symbol{"value"}, &other_predicate}}};

    CHECK(bg::compare_specificity(first_signature, same_signature) == std::partial_ordering::equivalent);
    CHECK(bg::compare_specificity(first_signature, other_signature) == std::partial_ordering::unordered);
}
