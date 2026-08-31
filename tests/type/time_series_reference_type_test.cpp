#include <bg/type/scalar_type.hpp>
#include <bg/type/time_series_reference_type.hpp>
#include <bg/type/time_series_value_type.hpp>
#include <bg/type/type_map.hpp>

#include <catch2/catch_test_macros.hpp>

#include <stdexcept>

TEST_CASE("time series reference types retain their target and guarantee", "[type][reference]") {
    const auto &target = bg::time_series_value_type(bg::int64_type(), bg::TimeSeriesGuarantee::ordinary);
    const auto &reference = bg::time_series_reference_type(target, bg::TimeSeriesGuarantee::const_);

    CHECK(&reference.target_type() == &target);
    CHECK(reference.guarantee().value() == bg::TimeSeriesGuarantee::const_);
    CHECK(&bg::time_series_reference_type(target, bg::TimeSeriesGuarantee::const_) == &reference);
}

TEST_CASE("a generic reference accepts a reference or its target", "[type][reference][resolution]") {
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

TEST_CASE("a reference cannot target another reference", "[type][reference]") {
    const auto &target = bg::time_series_value_type(bg::int64_type(), bg::TimeSeriesGuarantee::ordinary);
    const auto &reference = bg::time_series_reference_type(target, bg::TimeSeriesGuarantee::ordinary);

    CHECK_THROWS_AS(bg::time_series_reference_type(reference, bg::TimeSeriesGuarantee::ordinary),
                    std::invalid_argument);
}
