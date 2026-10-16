#include <bg/time_series/tss/tss_type.hpp>
#include <bg/type/scalar_type.hpp>
#include <bg/type/scalar_variable.hpp>
#include <bg/type/time_series_set_type.hpp>
#include <bg/type/time_series_reference_type.hpp>
#include <bg/type/type_map.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
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

TEST_CASE("time series set inputs accept sources with equal or stronger guarantees",
          "[feature:type-system][series:tss][guarantee:ordinary][guarantee:deduplicated]"
          "[guarantee:final][guarantee:const]") {
    constexpr auto guarantees = std::array{bg::TimeSeriesGuarantee::ordinary,
                                           bg::TimeSeriesGuarantee::deduplicated,
                                           bg::TimeSeriesGuarantee::final,
                                           bg::TimeSeriesGuarantee::const_};

    for (const auto required : guarantees) {
        for (const auto supplied : guarantees) {
            const auto &required_type = bg::time_series_set_type(bg::int64_type(), required);
            const auto &supplied_type = bg::time_series_set_type(bg::int64_type(), supplied);

            CHECK(required_type.accepts(supplied_type) == bg::satisfies(supplied, required));
        }
    }

    CHECK(&bg::TSS<bg::i64>::type() != &bg::TSS<bg::i64, bg::TimeSeriesGuarantee::final>::type());
}

TEST_CASE("time series set matching keeps scalar item types invariant",
          "[feature:type-system][series:tss][declaration:concrete]") {
    CHECK_FALSE(bg::TSS<bg::i64>::type().accepts(bg::TSS<double>::type()));
}

TEST_CASE("time series set matching rolls back item variables when guarantee matching fails",
          "[feature:type-system][series:tss][declaration:generic][guarantee:final]") {
    using KEY = bg::ScalarVariable<"TSS_KEY", bg::Hashable>;
    const auto &required = bg::TSS<KEY, bg::TimeSeriesGuarantee::final>::type();
    const auto &supplied = bg::TSS<bg::i64>::type();
    auto type_map = bg::TypeMap{};

    CHECK_FALSE(required.match(supplied, type_map));
    CHECK(type_map.find(KEY::type()) == nullptr);
}

TEST_CASE("resolved time series set matching applies a reference guarantee limit",
          "[feature:type-system][series:tss][binding:reference-to-value][guarantee:ordinary][guarantee:final]") {
    const auto &ordinary = bg::TSS<bg::i64>::type();
    const auto &final = bg::TSS<bg::i64, bg::TimeSeriesGuarantee::final>::type();
    const auto &reference = bg::time_series_reference_type(final, bg::TimeSeriesGuarantee::ordinary);

    CHECK(ordinary.accepts(reference));
    CHECK_FALSE(final.accepts(reference));
}
