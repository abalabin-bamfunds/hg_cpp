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
    const auto &item = bg::TS<float>::type();
    const auto &list = bg::TSL<bg::TS<float>, 3>::type();

    CHECK(&list == &bg::TSL<bg::TS<float>, 3>::type());
    CHECK(&list.item_type() == &item);
    CHECK(list.item_count() == 3);
    CHECK(&list.item_type(2) == &item);
    CHECK_THROWS_AS(list.item_type(3), std::out_of_range);
}

TEST_CASE("generic time series list types resolve their item and guarantee",
          "[type_system][tsl][series:fixed-tsl][feature:type-system][declaration:generic]"
          "[guarantee:final]") {
    const auto &resolved_list =
        bg::TSL<bg::TS<float, bg::TimeSeriesGuarantee::final>, 2, bg::TimeSeriesGuarantee::final>::type();
    const bg::GenericTimeSeriesValueType generic_item{bg::float_type(), bg::TimeSeriesGuarantee::final};
    const bg::GenericTimeSeriesListType generic_list{generic_item, 2, bg::TimeSeriesGuarantee::final};
    auto type_map = bg::TypeMap{};

    REQUIRE(generic_list.match(resolved_list, type_map));
    CHECK(&generic_list.resolve(type_map) == &resolved_list);
}

TEST_CASE("a time series list size variable participates in type resolution",
          "[type_system][tsl][series:fixed-tsl][generic][declaration:generic][feature:type-system]") {
    const auto &parameter =
        bg::details::time_series_type_for<ListVariableScope, 0, true, bg::TSL<bg::TS<std::int64_t>, bg::SIZE>>(
            bg::TimeSeriesGuarantee::ordinary);
    const auto &argument = bg::TSL<bg::TS<std::int64_t>, 3>::type();
    auto type_map = bg::TypeMap{};

    REQUIRE(parameter.match(argument, type_map));
    CHECK(&parameter.resolve(type_map) == &argument);
}

TEST_CASE("an output time series list can capture its size",
          "[type_system][tsl][series:fixed-tsl][generic][declaration:generic][feature:type-system]") {
    const auto &parameter =
        bg::details::time_series_type_for<ListVariableScope, 0, false, bg::TSL<bg::TS<std::int64_t>, bg::SIZE>>(
            bg::TimeSeriesGuarantee::ordinary);
    const auto &argument = bg::TSL<bg::TS<std::int64_t>, 3>::type();
    auto type_map = bg::TypeMap{};

    REQUIRE(parameter.match(argument, type_map));
    CHECK(&parameter.resolve(type_map) == &argument);
}

TEST_CASE("repeated time series list size variables require equal sizes",
          "[type_system][tsl][series:fixed-tsl][generic][declaration:generic][feature:type-system]") {
    const auto &first =
        bg::details::time_series_type_for<ListVariableScope, 0, true, bg::TSL<bg::TS<std::int64_t>, bg::SIZE>>(
            bg::TimeSeriesGuarantee::ordinary);
    const auto &second =
        bg::details::time_series_type_for<ListVariableScope, 1, true, bg::TSL<bg::TS<std::int64_t>, bg::SIZE>>(
            bg::TimeSeriesGuarantee::ordinary);
    const auto &size_three = bg::TSL<bg::TS<std::int64_t>, 3>::type();
    const auto &size_four = bg::TSL<bg::TS<std::int64_t>, 4>::type();
    auto type_map = bg::TypeMap{};

    REQUIRE(first.match(size_three, type_map));
    CHECK(second.match(size_three, type_map));
    CHECK_FALSE(second.match(size_four, type_map));
}

TEST_CASE("fixed time series list matching applies reference transparency to its items",
          "[type_system][tsl][series:fixed-tsl][feature:type-system][binding:reference-to-value]") {
    const auto &values = bg::TSL<bg::TS<float>, 2>::type();
    const auto &references = bg::TSL<bg::REF<bg::TS<float>, bg::TimeSeriesGuarantee::const_>, 2>::type();
    auto type_map = bg::TypeMap{};

    CHECK(references.match(values, type_map));
    CHECK(values.match(references, type_map));
}
