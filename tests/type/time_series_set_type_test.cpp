#include <bg/time_series/tss/tss_type.hpp>
#include <bg/type/scalar_type.hpp>
#include <bg/type/scalar_variable.hpp>
#include <bg/type/time_series_set_type.hpp>
#include <bg/type/type_map.hpp>

#include <catch2/catch_test_macros.hpp>

#include <concepts>

TEST_CASE("time series set annotations expose their item type", "[type_system][tss][series:tss][feature:type-system]") {
    using IntTimeSeriesSet = bg::TSS<bg::i64>;

    STATIC_CHECK(std::same_as<IntTimeSeriesSet::item_type, bg::i64>);
    STATIC_CHECK(bg::details::is_time_series_set<IntTimeSeriesSet>);
}

TEST_CASE("time series set declarations require canonical integer types",
          "[feature:type-system][series:tss][scalar:integer][declaration:concrete]") {
    STATIC_CHECK_FALSE(bg::details::supported_time_series<bg::TSS<int>>);
    STATIC_CHECK(bg::details::supported_time_series<bg::TSS<bg::i64>>);
}

TEST_CASE("resolved time series set types are canonical", "[type_system][tss][series:tss][feature:type-system]") {
    const auto &set = bg::TSS<bg::i64>::type();

    CHECK(&set == &bg::TSS<bg::i64>::type());
    CHECK(&set.item_type() == &bg::int64_type());
}

TEST_CASE("generic time series set types resolve their item and guarantee",
          "[type_system][tss][series:tss][feature:type-system][declaration:generic][guarantee:final]") {
    const auto &resolved = bg::TSS<bg::i64, bg::TimeSeriesGuarantee::final>::type();
    using KEY = bg::ScalarVariable<"K", bg::Hashable>;
    const auto &generic = bg::TSS<KEY, bg::TimeSeriesGuarantee::final>::type();
    auto type_map = bg::TypeMap{};

    REQUIRE(generic.match(resolved, type_map));
    CHECK(&generic.resolve(type_map) == &resolved);
}
