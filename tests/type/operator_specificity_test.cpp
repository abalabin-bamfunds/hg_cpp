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

using namespace bg;

TEST_CASE("an exact parameter is less generic than a type variable",
          "[feature:operator][feature:type-system][declaration:generic]") {
    const auto exact = Signature{util::symbol{"exact"}, {{util::symbol{"value"}, &int_type()}}};
    using T = ScalarVariable<"T">;
    const auto &variable = T::type();
    const auto generic = Signature{util::symbol{"generic"}, {{util::symbol{"value"}, &variable}}};

    CHECK(compare_specificity(exact, generic) == std::partial_ordering::less);
    CHECK(compare_specificity(generic, exact) == std::partial_ordering::greater);
}

TEST_CASE("a constrained variable is less generic than an unconstrained variable",
          "[feature:operator][feature:type-system][declaration:generic]") {
    using NUMBER = ScalarVariable<"NUMBER", int, float>;
    using SCALAR = ScalarVariable<"Scalar">;
    const auto &number = NUMBER::type();
    const auto &scalar = SCALAR::type();
    const auto constrained = Signature{util::symbol{"number"}, {{util::symbol{"value"}, &number}}};
    const auto unconstrained = Signature{util::symbol{"scalar"}, {{util::symbol{"value"}, &scalar}}};

    CHECK(compare_specificity(constrained, unconstrained) == std::partial_ordering::less);
}

TEST_CASE("a repeated variable is less generic than independent variables",
          "[feature:operator][feature:type-system][declaration:generic]") {
    using T = ScalarVariable<"T">;
    using L = ScalarVariable<"L">;
    using R = ScalarVariable<"R">;
    const auto &repeated = T::type();
    const auto &left = L::type();
    const auto &right = R::type();
    const auto same =
        Signature{util::symbol{"same"}, {{util::symbol{"left"}, &repeated}, {util::symbol{"right"}, &repeated}}};
    const auto independent =
        Signature{util::symbol{"independent"}, {{util::symbol{"left"}, &left}, {util::symbol{"right"}, &right}}};

    CHECK(compare_specificity(same, independent) == std::partial_ordering::less);
    CHECK(compare_specificity(independent, same) == std::partial_ordering::greater);
}

TEST_CASE("same time-series declarations retain repeated-variable specificity",
          "[feature:operator][feature:type-system][declaration:generic][series:value]") {
    using T = ScalarVariable<"T">;
    using R = ScalarVariable<"R">;

    const auto repeated = Signature{
        util::symbol{"repeated"}, {{util::symbol{"first"}, &TS<T>::type()}, {util::symbol{"second"}, &TS<T>::type()}}};
    const auto independent =
        Signature{util::symbol{"independent"},
                  {{util::symbol{"first"}, &TS<T>::type()}, {util::symbol{"second"}, &TS<R>::type()}}};

    CHECK(compare_specificity(repeated, independent) == std::partial_ordering::less);
    CHECK(compare_specificity(independent, repeated) == std::partial_ordering::greater);
}

TEST_CASE("repeated variables contribute specificity as one declaration relationship",
          "[feature:operator][feature:type-system][declaration:generic]") {
    using T = ScalarVariable<"T">;
    using R = ScalarVariable<"R">;

    const auto repeated = Signature{util::symbol{"repeated"},
                                    {{util::symbol{"first"}, &T::type()}, {util::symbol{"second"}, &T::type()}}};
    const auto partly_concrete =
        Signature{util::symbol{"partly_concrete"},
                  {{util::symbol{"first"}, &int64_type()}, {util::symbol{"second"}, &R::type()}}};
    const auto concrete = Signature{util::symbol{"concrete"},
                                    {{util::symbol{"first"}, &int64_type()}, {util::symbol{"second"}, &int64_type()}}};

    CHECK(compare_specificity(repeated, partly_concrete) == std::partial_ordering::unordered);
    CHECK(compare_specificity(partly_concrete, repeated) == std::partial_ordering::unordered);
    CHECK(compare_specificity(concrete, repeated) == std::partial_ordering::less);
}

TEST_CASE("constraints on different parameters are unordered",
          "[feature:operator][feature:type-system][declaration:generic]") {
    using L = ScalarVariable<"L">;
    using R = ScalarVariable<"R">;
    const auto &any_left = L::type();
    const auto &any_right = R::type();
    const auto left_exact = Signature{util::symbol{"left_exact"},
                                      {{util::symbol{"left"}, &int_type()}, {util::symbol{"right"}, &any_right}}};
    const auto right_exact = Signature{util::symbol{"right_exact"},
                                       {{util::symbol{"left"}, &any_left}, {util::symbol{"right"}, &int_type()}}};

    CHECK(compare_specificity(left_exact, right_exact) == std::partial_ordering::unordered);
}

TEST_CASE("a stronger literal guarantee is less generic",
          "[feature:operator][feature:type-system][series:value][guarantee:final]") {
    const auto &final = TS<i64, TimeSeriesGuarantee::final>::type();
    const auto &ordinary = TS<i64>::type();
    const auto final_signature = Signature{util::symbol{"final"}, {{util::symbol{"value"}, &final}}};
    const auto ordinary_signature = Signature{util::symbol{"ordinary"}, {{util::symbol{"value"}, &ordinary}}};

    CHECK(compare_specificity(final_signature, ordinary_signature) == std::partial_ordering::less);
}

TEST_CASE("a literal guarantee is more specific than a variable with the same minimum",
          "[feature:operator][feature:type-system][declaration:generic][series:value]"
          "[guarantee:deduplicated]") {
    constexpr auto G = guarantee_variable<"SPECIFICITY_G", TimeSeriesGuarantee::deduplicated>;
    constexpr auto G_NO_MINIMUM = guarantee_variable<"SPECIFICITY_G">;
    const auto literal = Signature{util::symbol{"literal"},
                                   {{util::symbol{"value"}, &TS<i64, TimeSeriesGuarantee::deduplicated>::type()}}};
    const auto constrained = Signature{util::symbol{"constrained"}, {{util::symbol{"value"}, &TS<i64, G>::type()}}};
    const auto unconstrained =
        Signature{util::symbol{"unconstrained"}, {{util::symbol{"value"}, &TS<i64, G_NO_MINIMUM>::type()}}};

    CHECK(compare_specificity(literal, constrained) == std::partial_ordering::less);
    CHECK(compare_specificity(constrained, literal) == std::partial_ordering::greater);
    CHECK(compare_specificity(constrained, unconstrained) == std::partial_ordering::less);
}

TEST_CASE("reference shape does not affect specificity",
          "[feature:operator][feature:type-system][feature:reference][binding:reference-to-value]") {
    const auto &value = TS<i64>::type();
    const auto &reference = REF<TS<i64>>::type();
    const auto value_signature = Signature{util::symbol{"value"}, {{util::symbol{"value"}, &value}}};
    const auto reference_signature = Signature{util::symbol{"reference"}, {{util::symbol{"value"}, &reference}}};

    CHECK(compare_specificity(value_signature, reference_signature) == std::partial_ordering::equivalent);
}

TEST_CASE("a concrete time series is less generic than a time-series variable",
          "[feature:operator][feature:type-system][declaration:generic][series:value]") {
    const auto concrete = Signature{util::symbol{"concrete"}, {{util::symbol{"value"}, &TS<i64>::type()}}};
    const auto generic = Signature{util::symbol{"generic"}, {{util::symbol{"value"}, &TS_T::type()}}};

    CHECK(compare_specificity(concrete, generic) == std::partial_ordering::less);
    CHECK(compare_specificity(generic, concrete) == std::partial_ordering::greater);
}

TEST_CASE("a literal list size is less generic than a size variable",
          "[feature:operator][feature:type-system][declaration:generic][series:fixed-tsl]") {
    const auto fixed = Signature{util::symbol{"fixed"}, {{util::symbol{"value"}, &TSL<TS<i64>, 3>::type()}}};
    const auto generic = Signature{util::symbol{"generic"}, {{util::symbol{"value"}, &TSL<TS<i64>, SIZE>::type()}}};

    CHECK(compare_specificity(fixed, generic) == std::partial_ordering::less);
    CHECK(compare_specificity(generic, fixed) == std::partial_ordering::greater);
}

TEST_CASE("size specificity compares declaration relationships, not numeric order",
          "[feature:operator][feature:type-system][declaration:generic][series:fixed-tsl]") {
    using M = SizeVariableDeclaration<"M">;
    using SAME_SIZE = TSL<TS<i64>, SIZE>;
    using SIZE_THREE = TSL<TS<i64>, 3>;
    using SIZE_M = TSL<TS<i64>, M{}>;

    const auto repeated =
        Signature{util::symbol{"repeated"},
                  {{util::symbol{"first"}, &SAME_SIZE::type()}, {util::symbol{"second"}, &SAME_SIZE::type()}}};
    const auto three_and_independent =
        Signature{util::symbol{"three_and_independent"},
                  {{util::symbol{"first"}, &SIZE_THREE::type()}, {util::symbol{"second"}, &SIZE_M::type()}}};
    const auto fixed =
        Signature{util::symbol{"fixed"},
                  {{util::symbol{"first"}, &SIZE_THREE::type()}, {util::symbol{"second"}, &SIZE_THREE::type()}}};

    CHECK(compare_specificity(repeated, three_and_independent) == std::partial_ordering::unordered);
    CHECK(compare_specificity(three_and_independent, repeated) == std::partial_ordering::unordered);
    CHECK(compare_specificity(fixed, repeated) == std::partial_ordering::less);
}

TEST_CASE("unrelated scalar capability constraints are unordered",
          "[feature:operator][feature:type-system][declaration:generic]") {
    using EQUALITY_SCALAR = ScalarVariable<"EQUALITY_SCALAR", EqualityComparable>;
    using HASH_SCALAR = ScalarVariable<"HASH_SCALAR", Hashable>;

    const auto equality = Signature{util::symbol{"equality"}, {{util::symbol{"value"}, &EQUALITY_SCALAR::type()}}};
    const auto hash = Signature{util::symbol{"hash"}, {{util::symbol{"value"}, &HASH_SCALAR::type()}}};

    CHECK(compare_specificity(equality, hash) == std::partial_ordering::unordered);
    CHECK(compare_specificity(hash, equality) == std::partial_ordering::unordered);
}

TEST_CASE("identical signatures have equivalent specificity", "[feature:operator][feature:type-system]") {
    const auto left = Signature{util::symbol{"left"}, {{util::symbol{"value"}, &TS<i64>::type()}}};
    const auto right = Signature{util::symbol{"right"}, {{util::symbol{"other"}, &TS<i64>::type()}}};

    CHECK(compare_specificity(left, right) == std::partial_ordering::equivalent);
}

TEST_CASE("a union accepting a subset of types is less generic",
          "[feature:operator][feature:type-system][declaration:generic]") {
    using NUMBER = ScalarVariable<"NUMBER">;
    using NUMERIC = Union<"Numeric", i64, float>;
    using SCALAR = Union<"Scalar", NUMBER>;

    const auto &numeric = NUMERIC::type();
    const auto &scalar = SCALAR::type();
    const auto numeric_signature = Signature{util::symbol{"numeric"}, {{util::symbol{"value"}, &numeric}}};
    const auto scalar_signature = Signature{util::symbol{"scalar"}, {{util::symbol{"value"}, &scalar}}};

    CHECK(compare_specificity(numeric_signature, scalar_signature) == std::partial_ordering::less);
    CHECK(compare_specificity(scalar_signature, numeric_signature) == std::partial_ordering::greater);
}

TEST_CASE("a concrete time series is more specific than a scalar-or-time-series union",
          "[feature:operator][feature:type-system][declaration:generic][series:value]") {
    using PROMOTED = Union<"PROMOTED", i64, TS<i64, guarantee_variable<"G">>>;
    const auto &promoted = PROMOTED::type();
    const auto promoted_signature = Signature{util::symbol{"promoted"}, {{util::symbol{"value"}, &promoted}}};
    const auto time_series_signature =
        Signature{util::symbol{"time_series"}, {{util::symbol{"value"}, &TS<i64, guarantee_variable<"G">>::type()}}};

    CHECK(compare_specificity(time_series_signature, promoted_signature) == std::partial_ordering::less);
    CHECK(compare_specificity(promoted_signature, time_series_signature) == std::partial_ordering::greater);

    const auto other_time_series_signature = Signature{
        util::symbol{"other_time_series"}, {{util::symbol{"value"}, &TS<float, guarantee_variable<"G">>::type()}}};
    CHECK(compare_specificity(other_time_series_signature, promoted_signature) == std::partial_ordering::unordered);
    CHECK(compare_specificity(promoted_signature, other_time_series_signature) == std::partial_ordering::unordered);
}

TEST_CASE("partially overlapping unions are unordered",
          "[feature:operator][feature:type-system][declaration:generic]") {
    using LEFT = Union<"Left", i64, float>;
    using RIGHT = Union<"Right", float, double>;
    const auto &left = LEFT::type();
    const auto &right = RIGHT::type();
    const auto left_signature = Signature{util::symbol{"left"}, {{util::symbol{"value"}, &left}}};
    const auto right_signature = Signature{util::symbol{"right"}, {{util::symbol{"value"}, &right}}};

    CHECK(compare_specificity(left_signature, right_signature) == std::partial_ordering::unordered);
    CHECK(compare_specificity(right_signature, left_signature) == std::partial_ordering::unordered);
}

TEST_CASE("specificity preserves guarantee-variable relationships through promoted alternatives",
          "[feature:operator][feature:type-system][declaration:generic]") {
    using PROMOTED_G1 = Union<"PROMOTED_G1", i64, TS<i64, guarantee_variable<"G1">>>;
    using PROMOTED_G2 = Union<"PROMOTED_G2", i64, TS<i64, guarantee_variable<"G2">>>;
    constexpr auto G = guarantee_variable<"G">;

    const auto shared_guarantee =
        Signature{util::symbol{"shared_guarantee"},
                  {{util::symbol{"first"}, &TS<i64, G>::type()}, {util::symbol{"second"}, &TS<i64, G>::type()}}};
    const auto independent_guarantees =
        Signature{util::symbol{"independent_guarantees"},
                  {{util::symbol{"first"}, &PROMOTED_G1::type()}, {util::symbol{"second"}, &PROMOTED_G2::type()}}};

    CHECK(compare_specificity(shared_guarantee, independent_guarantees) == std::partial_ordering::less);
    CHECK(compare_specificity(independent_guarantees, shared_guarantee) == std::partial_ordering::greater);
}

TEST_CASE("independent guarantee variables do not impose a repeated guarantee requirement",
          "[feature:operator][feature:type-system][declaration:generic][series:value]") {
    constexpr auto G1 = guarantee_variable<"G1">;
    constexpr auto G2 = guarantee_variable<"G2">;
    constexpr auto G = guarantee_variable<"G">;

    const auto repeated =
        Signature{util::symbol{"repeated"},
                  {{util::symbol{"first"}, &TS<i64, G>::type()}, {util::symbol{"second"}, &TS<i64, G>::type()}}};
    const auto independent =
        Signature{util::symbol{"independent"},
                  {{util::symbol{"first"}, &TS<i64, G1>::type()}, {util::symbol{"second"}, &TS<i64, G2>::type()}}};

    CHECK(compare_specificity(repeated, independent) == std::partial_ordering::less);
}

TEST_CASE("invariant dictionary keys do not become projected key variables",
          "[feature:operator][feature:type-system][declaration:generic][series:tsd]") {
    using DICTIONARY_ONE = Union<"DICTIONARY_ONE", TSD<i64, TS<i64>>, TSD<i64, TS<double>>>;
    using DICTIONARY_TWO = Union<"DICTIONARY_TWO", TSD<i64, TS<i64>>, TSD<i64, TS<double>>>;
    using KEY = ScalarVariable<"KEY">;
    using VALUE = ScalarVariable<"VALUE">;
    using OTHER_VALUE = ScalarVariable<"OTHER_VALUE">;

    const auto unions = Signature{
        util::symbol{"unions"},
        {{util::symbol{"first"}, &DICTIONARY_ONE::type()}, {util::symbol{"second"}, &DICTIONARY_TWO::type()}}};
    const auto repeated_key = Signature{util::symbol{"repeated_key"},
                                        {{util::symbol{"first"}, &TSD<KEY, TS<VALUE>>::type()},
                                         {util::symbol{"second"}, &TSD<KEY, TS<OTHER_VALUE>>::type()}}};
    const auto supplied = std::array<const bg::Type *, 2>{&TSD<i64, TS<i64>>::type(), &TSD<i64, TS<i64>>::type()};

    CHECK(details::compare_parameter_specificity(unions, repeated_key, supplied, supplied, 2) ==
          std::partial_ordering::less);
}

TEST_CASE("ambiguous union narrowing falls back to declaration comparison",
          "[feature:operator][feature:type-system][declaration:generic]") {
    using T = ScalarVariable<"T">;
    using R = ScalarVariable<"R">;
    using DICTIONARY_UNION = Union<"DICTIONARY_UNION", Dict<T, R>, Dict<R, T>>;
    using KEY = ScalarVariable<"KEY">;
    using VALUE = ScalarVariable<"VALUE">;

    const auto union_signature = Signature{util::symbol{"union"}, {{util::symbol{"value"}, &DICTIONARY_UNION::type()}}};
    const auto generic_signature =
        Signature{util::symbol{"generic"}, {{util::symbol{"value"}, &cpp_scalar_type<Dict<KEY, VALUE>>()}}};
    const auto supplied = std::array<const bg::Type *, 1>{&cpp_scalar_type<Dict<i64, double>>()};
    auto type_map = TypeMap{};
    REQUIRE(T::type().match(int64_type(), type_map));
    REQUIRE(R::type().match(double_type(), type_map));
    REQUIRE_NOTHROW(union_signature.resolve_with_type_map(supplied, type_map));

    CHECK_NOTHROW(details::compare_parameter_specificity(union_signature, generic_signature, supplied, supplied, 1));
}

TEST_CASE("contextual specificity orders a concrete type, its union, and constrained types",
          "[feature:operator][feature:type-system][declaration:generic]") {
    using PROMOTED = Union<"PROMOTED", i64, TS<i64>>;
    using EQUALITY_SCALAR = ScalarVariable<"EQUALITY_SCALAR", EqualityComparable>;
    using SCALAR = ScalarVariable<"SCALAR">;

    const auto concrete = Signature{util::symbol{"concrete"}, {{util::symbol{"value"}, &TS<i64>::type()}}};
    const auto promoted = Signature{util::symbol{"promoted"}, {{util::symbol{"value"}, &PROMOTED::type()}}};
    const auto equality = Signature{util::symbol{"equality"}, {{util::symbol{"value"}, &TS<EQUALITY_SCALAR>::type()}}};
    const auto scalar = Signature{util::symbol{"scalar"}, {{util::symbol{"value"}, &TS<SCALAR>::type()}}};
    const auto supplied = std::array<const bg::Type *, 1>{&TS<i64>::type()};
    const auto matches_supplied = [&](const bg::Type &pattern) {
        auto mapping = TypeMap{};
        return pattern.match(*supplied[0], mapping);
    };
    REQUIRE(matches_supplied(TS<i64>::type()));
    REQUIRE(matches_supplied(PROMOTED::type()));
    REQUIRE(matches_supplied(TS<EQUALITY_SCALAR>::type()));
    REQUIRE(matches_supplied(TS<SCALAR>::type()));
    REQUIRE_NOTHROW(concrete.resolve(supplied));
    REQUIRE_NOTHROW(promoted.resolve(supplied));
    REQUIRE_NOTHROW(equality.resolve(supplied));
    REQUIRE_NOTHROW(scalar.resolve(supplied));
    const auto compare = [&](const auto &left, const auto &right) {
        return details::compare_parameter_specificity(left, right, supplied, supplied, 1);
    };

    CHECK(compare(concrete, promoted) == std::partial_ordering::less);
    CHECK(compare(promoted, equality) == std::partial_ordering::less);
    CHECK(compare(equality, scalar) == std::partial_ordering::less);
    CHECK(compare(concrete, scalar) == std::partial_ordering::less);
}

TEST_CASE("contextual specificity preserves partial overlap and conflicting alternatives",
          "[feature:operator][feature:type-system][declaration:generic]") {
    using LEFT = Union<"LEFT", i64, float>;
    using RIGHT = Union<"RIGHT", float, double>;
    using MIXED_LEFT = Union<"MIXED_LEFT", bg::SCALAR, i64>;
    using EQUALITY_SCALAR = ScalarVariable<"EQUALITY_SCALAR", EqualityComparable>;
    using HASH_SCALAR = ScalarVariable<"HASH_SCALAR", Hashable>;
    using MIXED_RIGHT = Union<"MIXED_RIGHT", EQUALITY_SCALAR, HASH_SCALAR>;

    const auto left = Signature{util::symbol{"left"}, {{util::symbol{"value"}, &LEFT::type()}}};
    const auto right = Signature{util::symbol{"right"}, {{util::symbol{"value"}, &RIGHT::type()}}};
    const auto mixed_left = Signature{util::symbol{"mixed_left"}, {{util::symbol{"value"}, &MIXED_LEFT::type()}}};
    const auto mixed_right = Signature{util::symbol{"mixed_right"}, {{util::symbol{"value"}, &MIXED_RIGHT::type()}}};
    const auto supplied_float = std::array<const bg::Type *, 1>{&float_type()};
    const auto supplied_integer = std::array<const bg::Type *, 1>{&int64_type()};
    auto mixed_left_mapping = TypeMap{};
    auto mixed_right_mapping = TypeMap{};
    REQUIRE(MIXED_LEFT::type().match(*supplied_integer[0], mixed_left_mapping));
    REQUIRE(MIXED_RIGHT::type().match(*supplied_integer[0], mixed_right_mapping));

    CHECK(details::compare_parameter_specificity(left, right, supplied_float, supplied_float, 1) ==
          std::partial_ordering::unordered);
    CHECK(details::compare_parameter_specificity(right, left, supplied_float, supplied_float, 1) ==
          std::partial_ordering::unordered);
    CHECK(details::compare_parameter_specificity(mixed_left, mixed_right, supplied_integer, supplied_integer, 1) ==
          std::partial_ordering::unordered);
    CHECK(details::compare_parameter_specificity(mixed_right, mixed_left, supplied_integer, supplied_integer, 1) ==
          std::partial_ordering::unordered);
}

TEST_CASE("contextual narrowing retains repeated-variable relationships",
          "[feature:operator][feature:type-system][declaration:generic]") {
    using LEFT = Union<"LEFT", i64, double>;
    using RIGHT = Union<"RIGHT", i64, double>;
    using T = ScalarVariable<"T">;

    const auto independent =
        Signature{util::symbol{"independent"},
                  {{util::symbol{"first"}, &LEFT::type()}, {util::symbol{"second"}, &RIGHT::type()}}};
    const auto repeated = Signature{util::symbol{"repeated"},
                                    {{util::symbol{"first"}, &T::type()}, {util::symbol{"second"}, &T::type()}}};
    const auto supplied = std::array<const bg::Type *, 2>{&int64_type(), &int64_type()};
    const auto matches_supplied = [&](const bg::Type &pattern, const bg::Type &argument) {
        auto mapping = TypeMap{};
        return pattern.match(argument, mapping);
    };
    REQUIRE(matches_supplied(LEFT::type(), *supplied[0]));
    REQUIRE(matches_supplied(RIGHT::type(), *supplied[1]));
    REQUIRE(matches_supplied(T::type(), *supplied[0]));
    REQUIRE(matches_supplied(T::type(), *supplied[1]));
    REQUIRE_NOTHROW(independent.resolve(supplied));
    REQUIRE_NOTHROW(repeated.resolve(supplied));

    CHECK(details::compare_parameter_specificity(independent, repeated, supplied, supplied, 2) ==
          std::partial_ordering::unordered);
    CHECK(details::compare_parameter_specificity(repeated, independent, supplied, supplied, 2) ==
          std::partial_ordering::unordered);
}

TEST_CASE("contextual narrowing preserves nested repeated-variable relationships",
          "[feature:operator][feature:type-system][declaration:generic][series:value]") {
    using LEFT = Union<"LEFT", TS<i64>, TS<double>>;
    using RIGHT = Union<"RIGHT", TS<i64>, TS<double>>;
    using T = ScalarVariable<"T">;

    const auto independent =
        Signature{util::symbol{"independent"},
                  {{util::symbol{"first"}, &LEFT::type()}, {util::symbol{"second"}, &RIGHT::type()}}};
    const auto repeated = Signature{
        util::symbol{"repeated"}, {{util::symbol{"first"}, &TS<T>::type()}, {util::symbol{"second"}, &TS<T>::type()}}};
    const auto supplied = std::array<const bg::Type *, 2>{&TS<i64>::type(), &TS<i64>::type()};
    const auto matches_supplied = [&](const bg::Type &pattern, const bg::Type &argument) {
        auto mapping = TypeMap{};
        return pattern.match(argument, mapping);
    };
    REQUIRE(matches_supplied(LEFT::type(), *supplied[0]));
    REQUIRE(matches_supplied(RIGHT::type(), *supplied[1]));
    REQUIRE(matches_supplied(*repeated.parameter(0).type, *supplied[0]));
    REQUIRE(matches_supplied(*repeated.parameter(1).type, *supplied[1]));
    REQUIRE_NOTHROW(independent.resolve(supplied));
    REQUIRE_NOTHROW(repeated.resolve(supplied));

    CHECK(details::compare_parameter_specificity(independent, repeated, supplied, supplied, 2) ==
          std::partial_ordering::unordered);
    CHECK(details::compare_parameter_specificity(repeated, independent, supplied, supplied, 2) ==
          std::partial_ordering::unordered);
}

TEST_CASE("nested type specificity is consistent across a declaration matrix",
          "[feature:operator][feature:type-system][declaration:generic][series:value]") {
    using T = ScalarVariable<"T">;
    using R = ScalarVariable<"R">;
    using SCALAR = ScalarVariable<"SCALAR">;
    using TIME_SERIES_CHOICE = Union<"TIME_SERIES_CHOICE", TS<i64>, TS<double>>;

    const auto declarations = std::array<const bg::Type *, 6>{&TS<i64>::type(),    &TS<T>::type(),
                                                              &TS<R>::type(),      &TIME_SERIES_CHOICE::type(),
                                                              &TS<SCALAR>::type(), &TS_T::type()};
    const auto supplied = std::array<const bg::Type *, 2>{&TS<i64>::type(), &TS<i64>::type()};
    auto signatures = std::vector<Signature>{};
    for (const auto *first : declarations) {
        for (const auto *second : declarations) {
            signatures.emplace_back(
                util::symbol{"candidate"},
                std::vector<Parameter>{{util::symbol{"first"}, first}, {util::symbol{"second"}, second}});
            REQUIRE_NOTHROW(signatures.back().resolve(supplied));
        }
    }

    auto specificity = std::vector<std::vector<std::partial_ordering>>(
        signatures.size(), std::vector<std::partial_ordering>(signatures.size(), std::partial_ordering::equivalent));
    for (auto left = std::size_t{}; left < signatures.size(); ++left) {
        for (auto right = std::size_t{}; right < signatures.size(); ++right) {
            specificity[left][right] =
                details::compare_parameter_specificity(signatures[left], signatures[right], supplied, supplied, 2);
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
    const auto one_parameter = std::array<const bg::Type *, 1>{&TS<i64>::type()};
    const auto two_parameters = std::array<const bg::Type *, 2>{&TS<i64>::type(), &TS<double>::type()};
    const auto &one_parameter_graph = graph_argument_type(one_parameter, &TS<i64>::type());
    const auto &two_parameter_graph = graph_argument_type(two_parameters, &TS<i64>::type());
    const auto graph_union =
        TypeVariableType<bg::Type>{util::symbol{"GRAPH"}, {&one_parameter_graph, &two_parameter_graph}};
    const auto generic = Signature{util::symbol{"generic"}, {{util::symbol{"function"}, &graph_union}}};
    const auto concrete = Signature{util::symbol{"concrete"}, {{util::symbol{"function"}, &one_parameter_graph}}};
    const auto supplied = std::array<const bg::Type *, 1>{&one_parameter_graph};

    REQUIRE_NOTHROW(generic.resolve(supplied));
    CHECK(details::compare_parameter_specificity(concrete, generic, supplied, supplied, 1) ==
          std::partial_ordering::less);
}

TEST_CASE("extension predicates retain conservative function-identity ordering",
          "[feature:operator][feature:type-system][declaration:generic][scalar:generic]") {
    const auto accepts_any = +[](const ScalarType &) noexcept { return true; };
    const auto also_accepts_any = +[](const ScalarType &) noexcept { return true; };
    const auto first = TypeVariableType<ScalarType>{util::symbol{"FIRST"}, {}, {accepts_any}};
    const auto same_predicate = TypeVariableType<ScalarType>{util::symbol{"SAME_PREDICATE"}, {}, {accepts_any}};
    const auto other_predicate = TypeVariableType<ScalarType>{util::symbol{"OTHER_PREDICATE"}, {}, {also_accepts_any}};
    const auto first_signature = Signature{util::symbol{"first"}, {{util::symbol{"value"}, &first}}};
    const auto same_signature = Signature{util::symbol{"same"}, {{util::symbol{"value"}, &same_predicate}}};
    const auto other_signature = Signature{util::symbol{"other"}, {{util::symbol{"value"}, &other_predicate}}};

    CHECK(compare_specificity(first_signature, same_signature) == std::partial_ordering::equivalent);
    CHECK(compare_specificity(first_signature, other_signature) == std::partial_ordering::unordered);
}
