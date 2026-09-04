#include <bg/time_series/time_series_type.hpp>
#include <bg/time_series/tsl/tsl_type.hpp>
#include <bg/type/details/time_series_type.hpp>
#include <bg/type/time_series_list_type.hpp>
#include <bg/type/time_series_reference_type.hpp>
#include <bg/type/time_series_value_type.hpp>

#include <catch2/catch_test_macros.hpp>

#include <type_traits>

namespace {

struct ListVariableScope {};

} // namespace

TEST_CASE("fixed time series list annotations expose their item type and size",
          "[type_system][tsl][series:fixed-tsl][feature:type-system]") {
    using FloatTimeSeriesList = bg::TSL<bg::TS<float>, 3>;

    STATIC_CHECK(std::same_as<FloatTimeSeriesList::item_type, bg::TS<float>>);
    STATIC_CHECK(FloatTimeSeriesList::size == 3);
    STATIC_CHECK(bg::details::is_time_series_list<FloatTimeSeriesList>);
}

TEST_CASE("resolved time series list types are canonical",
          "[type_system][tsl][series:fixed-tsl][feature:type-system]") {
    const auto &item = bg::time_series_value_type(bg::float_type(), bg::TimeSeriesGuarantee::ordinary);
    const auto &list = bg::time_series_list_type(item, 3, bg::TimeSeriesGuarantee::ordinary);

    CHECK(&list == &bg::time_series_list_type(item, 3, bg::TimeSeriesGuarantee::ordinary));
    CHECK(&list.item_type() == &item);
    CHECK(list.item_count() == 3);
    CHECK(&list.item_type(2) == &item);
    CHECK_THROWS_AS(list.item_type(3), std::out_of_range);
}

TEST_CASE("generic time series list types resolve their item and guarantee",
          "[type_system][tsl][series:fixed-tsl][feature:type-system][declaration:generic]"
          "[guarantee:final]") {
    const auto &resolved_item = bg::time_series_value_type(bg::float_type(), bg::TimeSeriesGuarantee::final);
    const auto &resolved_list = bg::time_series_list_type(resolved_item, 2, bg::TimeSeriesGuarantee::final);
    const bg::GenericTimeSeriesValueType generic_item{bg::float_type(), bg::TimeSeriesGuarantee::final};
    const bg::GenericTimeSeriesListType generic_list{generic_item, 2, bg::TimeSeriesGuarantee::final};
    auto bindings = bg::TypeMap{};

    REQUIRE(generic_list.match(resolved_list, bindings));
    CHECK(&generic_list.resolve(bindings) == &resolved_list);
}

TEST_CASE("a time series list size variable participates in type resolution",
          "[type_system][tsl][series:fixed-tsl][generic][declaration:generic][feature:type-system]") {
    const auto &parameter =
        bg::details::time_series_type_for<ListVariableScope, 0, true, bg::TSL<bg::TS<std::int64_t>, bg::SIZE>>(
            bg::TimeSeriesGuarantee::ordinary);
    const auto &item =
        bg::time_series_value_type(bg::cpp_scalar_type<std::int64_t>(), bg::TimeSeriesGuarantee::ordinary);
    const auto &argument = bg::time_series_list_type(item, 3, bg::TimeSeriesGuarantee::ordinary);
    auto bindings = bg::TypeMap{};

    REQUIRE(parameter.match(argument, bindings));
    CHECK(&parameter.resolve(bindings) == &argument);
}

TEST_CASE("an output time series list can capture its size",
          "[type_system][tsl][series:fixed-tsl][generic][declaration:generic][feature:type-system]") {
    const auto &parameter =
        bg::details::time_series_type_for<ListVariableScope, 0, false, bg::TSL<bg::TS<std::int64_t>, bg::SIZE>>(
            bg::TimeSeriesGuarantee::ordinary);
    const auto &item =
        bg::time_series_value_type(bg::cpp_scalar_type<std::int64_t>(), bg::TimeSeriesGuarantee::ordinary);
    const auto &argument = bg::time_series_list_type(item, 3, bg::TimeSeriesGuarantee::ordinary);
    auto bindings = bg::TypeMap{};

    REQUIRE(parameter.match(argument, bindings));
    CHECK(&parameter.resolve(bindings) == &argument);
}

TEST_CASE("repeated time series list size variables require equal sizes",
          "[type_system][tsl][series:fixed-tsl][generic][declaration:generic][feature:type-system]") {
    const auto &first =
        bg::details::time_series_type_for<ListVariableScope, 0, true, bg::TSL<bg::TS<std::int64_t>, bg::SIZE>>(
            bg::TimeSeriesGuarantee::ordinary);
    const auto &second =
        bg::details::time_series_type_for<ListVariableScope, 1, true, bg::TSL<bg::TS<std::int64_t>, bg::SIZE>>(
            bg::TimeSeriesGuarantee::ordinary);
    const auto &item =
        bg::time_series_value_type(bg::cpp_scalar_type<std::int64_t>(), bg::TimeSeriesGuarantee::ordinary);
    const auto &size_three = bg::time_series_list_type(item, 3, bg::TimeSeriesGuarantee::ordinary);
    const auto &size_four = bg::time_series_list_type(item, 4, bg::TimeSeriesGuarantee::ordinary);
    auto bindings = bg::TypeMap{};

    REQUIRE(first.match(size_three, bindings));
    CHECK(second.match(size_three, bindings));
    CHECK_FALSE(second.match(size_four, bindings));
}

TEST_CASE("fixed time series list matching applies reference transparency to its items",
          "[type_system][tsl][series:fixed-tsl][feature:type-system][binding:reference-to-value]") {
    const auto &item = bg::time_series_value_type(bg::float_type(), bg::TimeSeriesGuarantee::ordinary);
    const auto &reference = bg::time_series_reference_type(item, bg::TimeSeriesGuarantee::const_);
    const auto &values = bg::time_series_list_type(item, 2, bg::TimeSeriesGuarantee::ordinary);
    const auto &references = bg::time_series_list_type(reference, 2, bg::TimeSeriesGuarantee::ordinary);
    auto bindings = bg::TypeMap{};

    CHECK(references.match(values, bindings));
    CHECK(values.match(references, bindings));
}
