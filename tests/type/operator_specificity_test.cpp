#include <bg/time_series/size_variable.hpp>
#include <bg/time_series/time_series_type.hpp>
#include <bg/time_series/tsl/tsl_type.hpp>
#include <bg/type/scalar_type.hpp>
#include <bg/type/scalar_variable.hpp>
#include <bg/type/signature.hpp>
#include <bg/type/time_series_reference_type.hpp>
#include <bg/type/time_series_value_type.hpp>
#include <bg/type/type_variable.hpp>

#include <catch2/catch_test_macros.hpp>

#include <compare>

TEST_CASE("an exact parameter is less generic than a type variable",
          "[feature:operator][feature:type-system][declaration:generic]") {
    const auto exact = bg::Signature{bg::util::symbol{"exact"}, {{bg::util::symbol{"value"}, &bg::int_type()}}};
    const auto variable = bg::TypeVariableType<bg::ScalarType>{bg::util::symbol{"T"}};
    const auto generic = bg::Signature{bg::util::symbol{"generic"}, {{bg::util::symbol{"value"}, &variable}}};

    CHECK(bg::compare_specificity(exact, generic) == std::partial_ordering::less);
    CHECK(bg::compare_specificity(generic, exact) == std::partial_ordering::greater);
}

TEST_CASE("a constrained variable is less generic than an unconstrained variable",
          "[feature:operator][feature:type-system][declaration:generic]") {
    const auto number =
        bg::TypeVariableType<bg::ScalarType>{bg::util::symbol{"Number"}, {&bg::int_type(), &bg::float_type()}};
    const auto scalar = bg::TypeVariableType<bg::ScalarType>{bg::util::symbol{"Scalar"}};
    const auto constrained = bg::Signature{bg::util::symbol{"number"}, {{bg::util::symbol{"value"}, &number}}};
    const auto unconstrained = bg::Signature{bg::util::symbol{"scalar"}, {{bg::util::symbol{"value"}, &scalar}}};

    CHECK(bg::compare_specificity(constrained, unconstrained) == std::partial_ordering::less);
}

TEST_CASE("a repeated variable is less generic than independent variables",
          "[feature:operator][feature:type-system][declaration:generic]") {
    const auto repeated = bg::TypeVariableType<bg::ScalarType>{bg::util::symbol{"T"}};
    const auto left = bg::TypeVariableType<bg::ScalarType>{bg::util::symbol{"L"}};
    const auto right = bg::TypeVariableType<bg::ScalarType>{bg::util::symbol{"R"}};
    const auto same = bg::Signature{bg::util::symbol{"same"},
                                    {{bg::util::symbol{"left"}, &repeated}, {bg::util::symbol{"right"}, &repeated}}};
    const auto independent = bg::Signature{bg::util::symbol{"independent"},
                                           {{bg::util::symbol{"left"}, &left}, {bg::util::symbol{"right"}, &right}}};

    CHECK(bg::compare_specificity(same, independent) == std::partial_ordering::less);
    CHECK(bg::compare_specificity(independent, same) == std::partial_ordering::greater);
}

TEST_CASE("constraints on different parameters are unordered",
          "[feature:operator][feature:type-system][declaration:generic]") {
    const auto any_left = bg::TypeVariableType<bg::ScalarType>{bg::util::symbol{"L"}};
    const auto any_right = bg::TypeVariableType<bg::ScalarType>{bg::util::symbol{"R"}};
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
    const auto &final = bg::TS<int, bg::TimeSeriesGuarantee::final>::type();
    const auto &ordinary = bg::TS<int>::type();
    const auto final_signature = bg::Signature{bg::util::symbol{"final"}, {{bg::util::symbol{"value"}, &final}}};
    const auto ordinary_signature =
        bg::Signature{bg::util::symbol{"ordinary"}, {{bg::util::symbol{"value"}, &ordinary}}};

    CHECK(bg::compare_specificity(final_signature, ordinary_signature) == std::partial_ordering::less);
}

TEST_CASE("reference shape does not affect specificity",
          "[feature:operator][feature:type-system][feature:reference][binding:reference-to-value]") {
    const auto &value = bg::TS<int>::type();
    const auto &reference = bg::REF<bg::TS<int>>::type();
    const auto value_signature = bg::Signature{bg::util::symbol{"value"}, {{bg::util::symbol{"value"}, &value}}};
    const auto reference_signature =
        bg::Signature{bg::util::symbol{"reference"}, {{bg::util::symbol{"value"}, &reference}}};

    CHECK(bg::compare_specificity(value_signature, reference_signature) == std::partial_ordering::equivalent);
}

TEST_CASE("a concrete time series is less generic than a time-series variable",
          "[feature:operator][feature:type-system][declaration:generic][series:value]") {
    const auto concrete =
        bg::Signature{bg::util::symbol{"concrete"}, {{bg::util::symbol{"value"}, &bg::TS<int>::type()}}};
    const auto generic = bg::Signature{bg::util::symbol{"generic"}, {{bg::util::symbol{"value"}, &bg::TS_T::type()}}};

    CHECK(bg::compare_specificity(concrete, generic) == std::partial_ordering::less);
    CHECK(bg::compare_specificity(generic, concrete) == std::partial_ordering::greater);
}

TEST_CASE("a literal list size is less generic than a size variable",
          "[feature:operator][feature:type-system][declaration:generic][series:fixed-tsl]") {
    const auto fixed =
        bg::Signature{bg::util::symbol{"fixed"}, {{bg::util::symbol{"value"}, &bg::TSL<bg::TS<int>, 3>::type()}}};
    const auto generic = bg::Signature{bg::util::symbol{"generic"},
                                       {{bg::util::symbol{"value"}, &bg::TSL<bg::TS<int>, bg::SIZE>::type()}}};

    CHECK(bg::compare_specificity(fixed, generic) == std::partial_ordering::less);
    CHECK(bg::compare_specificity(generic, fixed) == std::partial_ordering::greater);
}

TEST_CASE("unrelated scalar capability constraints are unordered",
          "[feature:operator][feature:type-system][declaration:generic]") {
    using EqualityScalar = bg::ScalarVariable<"EqualityScalar", bg::EqualityComparable>;
    using HashScalar = bg::ScalarVariable<"HashScalar", bg::Hashable>;

    const auto equality =
        bg::Signature{bg::util::symbol{"equality"}, {{bg::util::symbol{"value"}, &EqualityScalar::type()}}};
    const auto hash = bg::Signature{bg::util::symbol{"hash"}, {{bg::util::symbol{"value"}, &HashScalar::type()}}};

    CHECK(bg::compare_specificity(equality, hash) == std::partial_ordering::unordered);
    CHECK(bg::compare_specificity(hash, equality) == std::partial_ordering::unordered);
}

TEST_CASE("identical signatures have equivalent specificity", "[feature:operator][feature:type-system]") {
    const auto left = bg::Signature{bg::util::symbol{"left"}, {{bg::util::symbol{"value"}, &bg::TS<int>::type()}}};
    const auto right = bg::Signature{bg::util::symbol{"right"}, {{bg::util::symbol{"other"}, &bg::TS<int>::type()}}};

    CHECK(bg::compare_specificity(left, right) == std::partial_ordering::equivalent);
}

TEST_CASE("a union accepting a subset of types is less generic",
          "[feature:operator][feature:type-system][declaration:generic]") {
    using Number = bg::ScalarVariable<"Number">;

    const auto numeric =
        bg::TypeVariableType<bg::Type>{bg::util::symbol{"Numeric"}, {&bg::int64_type(), &bg::float_type()}};
    const auto scalar = bg::TypeVariableType<bg::Type>{bg::util::symbol{"Scalar"}, {&Number::type()}};
    const auto numeric_signature = bg::Signature{bg::util::symbol{"numeric"}, {{bg::util::symbol{"value"}, &numeric}}};
    const auto scalar_signature = bg::Signature{bg::util::symbol{"scalar"}, {{bg::util::symbol{"value"}, &scalar}}};

    CHECK(bg::compare_specificity(numeric_signature, scalar_signature) == std::partial_ordering::less);
    CHECK(bg::compare_specificity(scalar_signature, numeric_signature) == std::partial_ordering::greater);
}

TEST_CASE("partially overlapping unions are unordered",
          "[feature:operator][feature:type-system][declaration:generic]") {
    const auto left = bg::TypeVariableType<bg::Type>{bg::util::symbol{"Left"}, {&bg::int64_type(), &bg::float_type()}};
    const auto right =
        bg::TypeVariableType<bg::Type>{bg::util::symbol{"Right"}, {&bg::float_type(), &bg::double_type()}};
    const auto left_signature = bg::Signature{bg::util::symbol{"left"}, {{bg::util::symbol{"value"}, &left}}};
    const auto right_signature = bg::Signature{bg::util::symbol{"right"}, {{bg::util::symbol{"value"}, &right}}};

    CHECK(bg::compare_specificity(left_signature, right_signature) == std::partial_ordering::unordered);
    CHECK(bg::compare_specificity(right_signature, left_signature) == std::partial_ordering::unordered);
}
