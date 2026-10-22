#include <bg/time_series/tss/tss_type.hpp>
#include <bg/type/scalar_type.hpp>
#include <bg/type/scalar_variable.hpp>
#include <bg/type/time_series_set_type.hpp>
#include <bg/type/time_series_reference_type.hpp>
#include <bg/type/type_map.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <concepts>

using namespace bg;

TEST_CASE("time series set annotations expose their item type", "[type_system][tss][series:tss][feature:type-system]") {
    using IntTimeSeriesSet = TSS<i64>;

    STATIC_CHECK(std::same_as<IntTimeSeriesSet::item_type, i64>);
    STATIC_CHECK(details::is_time_series_set<IntTimeSeriesSet>);
}

TEST_CASE("time series set declarations require canonical integer types",
          "[feature:type-system][series:tss][scalar:integer][declaration:concrete]") {
    STATIC_CHECK_FALSE(details::supported_time_series<TSS<int>>);
    STATIC_CHECK(details::supported_time_series<TSS<i64>>);
}

TEST_CASE("resolved time series set types are canonical", "[type_system][tss][series:tss][feature:type-system]") {
    const auto &set = TSS<i64>::type();

    CHECK(&set == &TSS<i64>::type());
    CHECK(&set.item_type() == &int64_type());
}

TEST_CASE("generic time series set types resolve their item and guarantee",
          "[type_system][tss][series:tss][feature:type-system][declaration:generic][guarantee:final]") {
    const auto &resolved = TSS<i64, TimeSeriesGuarantee::final>::type();
    using KEY = ScalarVariable<"K", Hashable>;
    const auto &generic = TSS<KEY, TimeSeriesGuarantee::final>::type();
    auto type_map = TypeMap{};

    REQUIRE(generic.match(resolved, type_map));
    CHECK(&generic.resolve(type_map) == &resolved);
}

TEST_CASE("time series set inputs accept sources with equal or stronger guarantees",
          "[feature:type-system][series:tss][guarantee:ordinary][guarantee:deduplicated]"
          "[guarantee:final][guarantee:const]") {
    constexpr auto guarantees = std::array{TimeSeriesGuarantee::ordinary, TimeSeriesGuarantee::deduplicated,
                                           TimeSeriesGuarantee::final, TimeSeriesGuarantee::const_};

    for (const auto required : guarantees) {
        for (const auto supplied : guarantees) {
            const auto &required_type = time_series_set_type(int64_type(), required);
            const auto &supplied_type = time_series_set_type(int64_type(), supplied);

            CHECK(required_type.accepts(supplied_type) == satisfies(supplied, required));
        }
    }

    CHECK(&TSS<i64>::type() != &TSS<i64, TimeSeriesGuarantee::final>::type());
}

TEST_CASE("time series set matching keeps scalar item types invariant",
          "[feature:type-system][series:tss][declaration:concrete]") {
    CHECK_FALSE(TSS<i64>::type().accepts(TSS<double>::type()));
}

TEST_CASE("time series set matching rolls back item variables when guarantee matching fails",
          "[feature:type-system][series:tss][declaration:generic][guarantee:final]") {
    using KEY = ScalarVariable<"TSS_KEY", Hashable>;
    const auto &required = TSS<KEY, TimeSeriesGuarantee::final>::type();
    const auto &supplied = TSS<i64>::type();
    auto type_map = TypeMap{};

    CHECK_FALSE(required.match(supplied, type_map));
    CHECK(type_map.find(KEY::type()) == nullptr);
}

TEST_CASE("resolved time series set matching applies a reference guarantee limit",
          "[feature:type-system][series:tss][binding:reference-to-value][guarantee:ordinary][guarantee:final]") {
    const auto &ordinary = TSS<i64>::type();
    const auto &final = TSS<i64, TimeSeriesGuarantee::final>::type();
    const auto &reference = time_series_reference_type(final, TimeSeriesGuarantee::ordinary);

    CHECK(ordinary.accepts(reference));
    CHECK_FALSE(final.accepts(reference));
}
