#include <bg/time_series/time_series_type.hpp>
#include <bg/type/scalar_type.hpp>
#include <bg/type/time_series_reference_type.hpp>
#include <bg/type/time_series_value_type.hpp>
#include <bg/type/type_map.hpp>

#include <catch2/catch_test_macros.hpp>

#include <stdexcept>

TEST_CASE("time series reference types retain their target and guarantee",
          "[type][reference][feature:type-system][guarantee:ordinary][guarantee:const]") {
    const auto &target = bg::TS<std::int64_t>::type();
    const auto &reference = bg::REF<bg::TS<std::int64_t>, bg::TimeSeriesGuarantee::const_>::type();

    CHECK(&reference.target_type() == &target);
    CHECK(reference.guarantee().value() == bg::TimeSeriesGuarantee::const_);
    CHECK(&bg::REF<bg::TS<std::int64_t>, bg::TimeSeriesGuarantee::const_>::type() == &reference);
}

TEST_CASE("a generic reference accepts a reference or its target",
          "[type][reference][resolution][feature:type-system][declaration:generic][guarantee:ordinary]"
          "[guarantee:const][binding:value-to-reference][binding:reference-to-reference]") {
    const auto &target = bg::TS<std::int64_t>::type();
    const auto &reference = bg::REF<bg::TS<std::int64_t>, bg::TimeSeriesGuarantee::const_>::type();
    const auto generic_target =
        bg::GenericTimeSeriesValueType{bg::int64_type(), bg::GuaranteeParameter{bg::TimeSeriesGuarantee::ordinary}};
    const auto generic_reference =
        bg::GenericTimeSeriesReferenceType{generic_target, bg::GuaranteeParameter{bg::TimeSeriesGuarantee::ordinary}};

    auto type_map = bg::TypeMap{};
    CHECK(generic_reference.match(reference, type_map));

    type_map = bg::TypeMap{};
    CHECK(generic_reference.match(target, type_map));
}

TEST_CASE("reference matching maps target and reference guarantees separately",
          "[type][reference][resolution][feature:type-system][guarantee:deduplicated][guarantee:final]"
          "[binding:reference-to-reference]") {
    const auto &reference =
        bg::REF<bg::TS<std::int64_t, bg::TimeSeriesGuarantee::final>, bg::TimeSeriesGuarantee::deduplicated>::type();
    auto target_guarantee = bg::GuaranteeVariable{bg::util::symbol{"target guarantee"}};
    auto reference_guarantee = bg::GuaranteeVariable{bg::util::symbol{"reference guarantee"}};
    const auto generic_target = bg::GenericTimeSeriesValueType{bg::int64_type(), target_guarantee};
    const auto generic_reference = bg::GenericTimeSeriesReferenceType{generic_target, reference_guarantee};
    auto type_map = bg::TypeMap{};

    REQUIRE(generic_reference.match(reference, type_map));
    const auto &resolved = static_cast<const bg::TimeSeriesReferenceType &>(generic_reference.resolve(type_map));

    CHECK(resolved.guarantee().value() == bg::TimeSeriesGuarantee::deduplicated);
    CHECK(resolved.target_type().guarantee().value() == bg::TimeSeriesGuarantee::final);
}

TEST_CASE("reference matching enforces the reference guarantee",
          "[type][reference][resolution][feature:type-system][guarantee:ordinary][guarantee:final]"
          "[binding:value-to-reference][binding:reference-to-reference]") {
    const auto &target = bg::TS<std::int64_t>::type();
    const auto &required = bg::REF<bg::TS<std::int64_t>, bg::TimeSeriesGuarantee::final>::type();
    const auto &ordinary = bg::REF<bg::TS<std::int64_t>>::type();
    auto type_map = bg::TypeMap{};

    CHECK_FALSE(required.match(ordinary, type_map));

    type_map = bg::TypeMap{};
    CHECK(required.match(target, type_map));
}

TEST_CASE("a reference cannot target another reference", "[type][reference][feature:type-system]") {
    CHECK_THROWS_AS((bg::REF<bg::REF<bg::TS<std::int64_t>>>::type()), std::invalid_argument);
}
