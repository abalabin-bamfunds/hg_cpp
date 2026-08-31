#include <bg/time_series/tss/tss_type.hpp>
#include <bg/type/scalar_type.hpp>
#include <bg/type/scalar_variable.hpp>
#include <bg/type/time_series_set_type.hpp>
#include <bg/type/type_map.hpp>

#include <catch2/catch_test_macros.hpp>

#include <concepts>

TEST_CASE("time series set annotations expose their item type", "[type_system][tss]") {
    using IntTimeSeriesSet = bg::TSS<int>;

    STATIC_CHECK(std::same_as<IntTimeSeriesSet::item_type, int>);
    STATIC_CHECK(bg::details::is_time_series_set<IntTimeSeriesSet>);
}

TEST_CASE("resolved time series set types are canonical", "[type_system][tss]") {
    const auto &set = bg::time_series_set_type(bg::int_type(), bg::TimeSeriesGuarantee::ordinary);

    CHECK(&set == &bg::time_series_set_type(bg::int_type(), bg::TimeSeriesGuarantee::ordinary));
    CHECK(&set.item_type() == &bg::int_type());
}

TEST_CASE("generic time series set types resolve their item and guarantee", "[type_system][tss]") {
    const auto &resolved = bg::time_series_set_type(bg::int_type(), bg::TimeSeriesGuarantee::final);
    bg::TypeVariable<bg::ScalarType> item{bg::util::symbol{"K"}, {}, {&bg::supports_scalar_hash}};
    const bg::GenericTimeSeriesSetType generic{item, bg::TimeSeriesGuarantee::final};
    auto bindings = bg::TypeMap{};

    REQUIRE(generic.match(resolved, bindings));
    CHECK(&generic.resolve(bindings) == &resolved);
}
