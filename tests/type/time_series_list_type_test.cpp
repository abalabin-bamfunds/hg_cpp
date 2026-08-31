#include <bg/time_series/time_series_type.hpp>
#include <bg/time_series/tsl/tsl_type.hpp>
#include <bg/type/time_series_list_type.hpp>
#include <bg/type/time_series_value_type.hpp>

#include <catch2/catch_test_macros.hpp>

#include <type_traits>

TEST_CASE("fixed time series list annotations expose their item type and size", "[type_system][tsl]") {
    using FloatTimeSeriesList = bg::TSL<bg::TS<float>, 3>;

    STATIC_CHECK(std::same_as<FloatTimeSeriesList::item_type, bg::TS<float>>);
    STATIC_CHECK(FloatTimeSeriesList::size == 3);
    STATIC_CHECK(bg::details::is_time_series_list<FloatTimeSeriesList>);
}

TEST_CASE("resolved time series list types are canonical", "[type_system][tsl]") {
    const auto &item = bg::time_series_value_type(bg::float_type(), bg::TimeSeriesGuarantee::ordinary);
    const auto &list = bg::time_series_list_type(item, 3, bg::TimeSeriesGuarantee::ordinary);

    CHECK(&list == &bg::time_series_list_type(item, 3, bg::TimeSeriesGuarantee::ordinary));
    CHECK(&list.item_type() == &item);
    CHECK(list.item_count() == 3);
    CHECK(&list.item_type(2) == &item);
    CHECK_THROWS_AS(list.item_type(3), std::out_of_range);
}

TEST_CASE("generic time series list types resolve their item and guarantee", "[type_system][tsl]") {
    const auto &resolved_item = bg::time_series_value_type(bg::float_type(), bg::TimeSeriesGuarantee::final);
    const auto &resolved_list = bg::time_series_list_type(resolved_item, 2, bg::TimeSeriesGuarantee::final);
    const bg::GenericTimeSeriesValueType generic_item{bg::float_type(), bg::TimeSeriesGuarantee::final};
    const bg::GenericTimeSeriesListType generic_list{generic_item, 2, bg::TimeSeriesGuarantee::final};
    auto bindings = bg::TypeMap{};

    REQUIRE(generic_list.match(resolved_list, bindings));
    CHECK(&generic_list.resolve(bindings) == &resolved_list);
}
