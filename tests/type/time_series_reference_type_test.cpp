#include <bg/type/scalar_type.hpp>
#include <bg/type/time_series_reference_type.hpp>
#include <bg/type/time_series_value_type.hpp>
#include <bg/type/type_map.hpp>

#include <catch2/catch_test_macros.hpp>

#include <stdexcept>

TEST_CASE("time series reference types retain their target and guarantee",
          "[type][reference][feature:type-system][guarantee:ordinary][guarantee:const]") {
    const auto &target = bg::time_series_value_type(bg::int64_type(), bg::TimeSeriesGuarantee::ordinary);
    const auto &reference = bg::time_series_reference_type(target, bg::TimeSeriesGuarantee::const_);

    CHECK(&reference.target_type() == &target);
    CHECK(reference.guarantee().value() == bg::TimeSeriesGuarantee::const_);
    CHECK(&bg::time_series_reference_type(target, bg::TimeSeriesGuarantee::const_) == &reference);
}

TEST_CASE("a generic reference accepts a reference or its target",
          "[type][reference][resolution][feature:type-system][declaration:generic][guarantee:ordinary]"
          "[guarantee:const][binding:value-to-reference][binding:reference-to-reference]") {
    const auto &target = bg::time_series_value_type(bg::int64_type(), bg::TimeSeriesGuarantee::ordinary);
    const auto &reference = bg::time_series_reference_type(target, bg::TimeSeriesGuarantee::const_);
    const auto generic_target =
        bg::GenericTimeSeriesValueType{bg::int64_type(), bg::GuaranteeParameter{bg::TimeSeriesGuarantee::ordinary}};
    const auto generic_reference =
        bg::GenericTimeSeriesReferenceType{generic_target, bg::GuaranteeParameter{bg::TimeSeriesGuarantee::ordinary}};

    auto bindings = bg::TypeMap{};
    CHECK(generic_reference.match(reference, bindings));

    bindings = bg::TypeMap{};
    CHECK(generic_reference.match(target, bindings));
}

TEST_CASE("reference matching binds target and reference guarantees separately",
          "[type][reference][resolution][feature:type-system][guarantee:deduplicated][guarantee:final]"
          "[binding:reference-to-reference]") {
    const auto &target = bg::time_series_value_type(bg::int64_type(), bg::TimeSeriesGuarantee::final);
    const auto &reference = bg::time_series_reference_type(target, bg::TimeSeriesGuarantee::deduplicated);
    auto target_guarantee = bg::GuaranteeVariable{bg::util::symbol{"target guarantee"}};
    auto reference_guarantee = bg::GuaranteeVariable{bg::util::symbol{"reference guarantee"}};
    const auto generic_target = bg::GenericTimeSeriesValueType{bg::int64_type(), target_guarantee};
    const auto generic_reference = bg::GenericTimeSeriesReferenceType{generic_target, reference_guarantee};
    auto bindings = bg::TypeMap{};

    REQUIRE(generic_reference.match(reference, bindings));
    const auto &resolved = static_cast<const bg::TimeSeriesReferenceType &>(generic_reference.resolve(bindings));

    CHECK(resolved.guarantee().value() == bg::TimeSeriesGuarantee::deduplicated);
    CHECK(resolved.target_type().guarantee().value() == bg::TimeSeriesGuarantee::final);
}

TEST_CASE("reference matching enforces the reference guarantee",
          "[type][reference][resolution][feature:type-system][guarantee:ordinary][guarantee:final]"
          "[binding:value-to-reference][binding:reference-to-reference]") {
    const auto &target = bg::time_series_value_type(bg::int64_type(), bg::TimeSeriesGuarantee::ordinary);
    const auto &required = bg::time_series_reference_type(target, bg::TimeSeriesGuarantee::final);
    const auto &ordinary = bg::time_series_reference_type(target, bg::TimeSeriesGuarantee::ordinary);
    auto bindings = bg::TypeMap{};

    CHECK_FALSE(required.match(ordinary, bindings));

    bindings = bg::TypeMap{};
    CHECK(required.match(target, bindings));
}

TEST_CASE("a reference cannot target another reference", "[type][reference][feature:type-system]") {
    const auto &target = bg::time_series_value_type(bg::int64_type(), bg::TimeSeriesGuarantee::ordinary);
    const auto &reference = bg::time_series_reference_type(target, bg::TimeSeriesGuarantee::ordinary);

    CHECK_THROWS_AS(bg::time_series_reference_type(reference, bg::TimeSeriesGuarantee::ordinary),
                    std::invalid_argument);
}
