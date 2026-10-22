#include <bg/time_series/time_series_type.hpp>
#include <bg/time_series/tsl/tsl_type.hpp>
#include <bg/type/details/time_series_type.hpp>
#include <bg/type/time_series_list_type.hpp>
#include <bg/type/time_series_reference_type.hpp>
#include <bg/type/time_series_value_type.hpp>
#include <bg/type/type_registry.hpp>
#include <bg/util/static_storage.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>

#include <array>
#include <type_traits>

using namespace bg;

TEST_CASE("fixed time series list annotations expose their item type and size",
          "[type_system][tsl][series:fixed-tsl][feature:type-system]") {
    using FloatTimeSeriesList = TSL<TS<float>, 3>;

    STATIC_CHECK(std::same_as<FloatTimeSeriesList::item_type, TS<float>>);
    STATIC_CHECK(FloatTimeSeriesList::size == 3);
    STATIC_CHECK(details::is_time_series_list<FloatTimeSeriesList>);
}

TEST_CASE("resolved time series list types are canonical",
          "[type_system][tsl][series:fixed-tsl][feature:type-system]") {
    const auto &item = TS<float>::type();
    const auto &list = TSL<TS<float>, 3>::type();

    CHECK(&list == &TSL<TS<float>, 3>::type());
    CHECK(&list.item_type() == &item);
    CHECK(list.item_count() == 3);
    CHECK(&list.item_type(2) == &item);
    CHECK_THROWS_AS(list.item_type(3), std::out_of_range);
}

TEST_CASE("common list types combine root and item guarantees recursively",
          "[feature:type-system][series:fixed-tsl][guarantee:ordinary][guarantee:final][guarantee:const]") {
    const auto &final_list = TSL<TS<i64, TimeSeriesGuarantee::final>, 2, TimeSeriesGuarantee::final>::type();
    const auto &constant_items = TSL<TS<i64, TimeSeriesGuarantee::const_>, 2>::type();
    const auto *common = final_list.common_type(constant_items);
    const auto &expected = TSL<TS<i64, TimeSeriesGuarantee::final>, 2>::type();

    REQUIRE(common != nullptr);
    CHECK(common == &expected);
    CHECK(constant_items.common_type(final_list) == common);
}

TEST_CASE("common sparse lists preserve their size form and combine item guarantees",
          "[feature:type-system][storage:variable][guarantee:ordinary][guarantee:final]") {
    const auto &final_items = time_series_list_type(TS<i64, TimeSeriesGuarantee::final>::type(), std::dynamic_extent,
                                                    TimeSeriesGuarantee::ordinary);
    const auto &ordinary_items =
        time_series_list_type(TS<i64>::type(), std::dynamic_extent, TimeSeriesGuarantee::ordinary);
    const auto *common = final_items.common_type(ordinary_items);

    REQUIRE(common != nullptr);
    const auto &list = type_cast<TimeSeriesListType>(*common);
    CHECK(list.item_count() == std::dynamic_extent);
    CHECK(list.guarantee().value() == TimeSeriesGuarantee::ordinary);
    CHECK(list.item_type().guarantee().value() == TimeSeriesGuarantee::ordinary);
    const auto &fixed = TSL<TS<i64>, 2>::type();
    CHECK(fixed.common_type(ordinary_items) == nullptr);
    CHECK(ordinary_items.common_type(fixed) == nullptr);
    const auto &i64_items = time_series_list_type(TS<i64>::type(), std::dynamic_extent, TimeSeriesGuarantee::ordinary);
    const auto &float_items =
        time_series_list_type(TS<float>::type(), std::dynamic_extent, TimeSeriesGuarantee::ordinary);
    CHECK(i64_items.common_type(float_items) == nullptr);
    CHECK(float_items.common_type(i64_items) == nullptr);

    const auto &unresolved = TSL<TS<SCALAR>, 2>::type();
    const auto &resolved = TSL<TS<i64>, 2>::type();
    CHECK(unresolved.common_type(resolved) == nullptr);
    CHECK(resolved.common_type(unresolved) == nullptr);
}

TEST_CASE("common nested list types combine each guarantee independently",
          "[feature:type-system][series:fixed-tsl][guarantee:ordinary][guarantee:final][guarantee:const]") {
    const auto &left = TSL<TSL<TS<i64, TimeSeriesGuarantee::const_>, 2, TimeSeriesGuarantee::final>, 3,
                           TimeSeriesGuarantee::final>::type();
    const auto &right = TSL<TSL<TS<i64, TimeSeriesGuarantee::final>, 2>, 3>::type();
    const auto *common = left.common_type(right);

    REQUIRE(common != nullptr);
    const auto &outer = type_cast<TimeSeriesListType>(*common);
    const auto &inner = type_cast<TimeSeriesListType>(outer.item_type());
    CHECK(outer.guarantee().value() == TimeSeriesGuarantee::ordinary);
    CHECK(inner.guarantee().value() == TimeSeriesGuarantee::ordinary);
    CHECK(inner.item_type().guarantee().value() == TimeSeriesGuarantee::final);
}

TEST_CASE("a time series list type derives its shape from resolved items",
          "[type_system][tsl][series:fixed-tsl][feature:type-system]"
          "[guarantee:ordinary][guarantee:final]") {
    const auto &ordinary = TS<float>::type();
    const auto &final = TS<float, TimeSeriesGuarantee::final>::type();
    const auto &reference = REF<TS<float, TimeSeriesGuarantee::final>>::type();
    const auto items = std::array<const TimeSeriesType *, 3>{&ordinary, &final, &reference};

    const auto &list = TimeSeriesListType::from_items(items, TimeSeriesGuarantee::final);

    CHECK(list.item_count() == 3);
    CHECK(list.guarantee().value() == TimeSeriesGuarantee::final);
    CHECK(&list.item_type() == &final);
}

TEST_CASE("dereferencing a direct time series preserves its type and guarantee",
          "[type_system][feature:type-system][series:value][guarantee:final]") {
    const auto &direct = TS<float, TimeSeriesGuarantee::final>::type();

    CHECK(&direct.dereferenced_type() == &direct);
    CHECK(direct.dereferenced_type().guarantee().value() == TimeSeriesGuarantee::final);
}

TEST_CASE("a time series list type applies reference guarantees to dereferenced items",
          "[type_system][tsl][reference][series:fixed-tsl][feature:type-system]"
          "[guarantee:ordinary][guarantee:deduplicated][guarantee:final][guarantee:const]") {
    constexpr auto guarantees = std::array{TimeSeriesGuarantee::ordinary, TimeSeriesGuarantee::deduplicated,
                                           TimeSeriesGuarantee::final, TimeSeriesGuarantee::const_};
    constexpr auto expected = std::array{
        std::array{TimeSeriesGuarantee::ordinary, TimeSeriesGuarantee::ordinary, TimeSeriesGuarantee::ordinary,
                   TimeSeriesGuarantee::ordinary},
        std::array{TimeSeriesGuarantee::ordinary, TimeSeriesGuarantee::ordinary, TimeSeriesGuarantee::ordinary,
                   TimeSeriesGuarantee::ordinary},
        std::array{TimeSeriesGuarantee::ordinary, TimeSeriesGuarantee::deduplicated, TimeSeriesGuarantee::final,
                   TimeSeriesGuarantee::final},
        std::array{TimeSeriesGuarantee::ordinary, TimeSeriesGuarantee::deduplicated, TimeSeriesGuarantee::final,
                   TimeSeriesGuarantee::const_},
    };

    for (auto reference_index = std::size_t{}; reference_index < guarantees.size(); ++reference_index) {
        for (auto target_index = std::size_t{}; target_index < guarantees.size(); ++target_index) {
            const auto &target = time_series_value_type(int64_type(), guarantees[target_index]);
            const auto &reference = time_series_reference_type(target, guarantees[reference_index]);
            const auto items = std::array<const TimeSeriesType *, 1>{&reference};
            const auto &list = TimeSeriesListType::from_items(items);
            CAPTURE(guarantees[reference_index], guarantees[target_index]);

            CHECK(list.item_type().guarantee().value() == expected[reference_index][target_index]);
        }
    }
}

TEST_CASE("a time series list type requires concrete homogeneous items",
          "[type_system][tsl][series:fixed-tsl][feature:type-system][declaration:generic]") {
    const auto empty = std::array<const TimeSeriesType *, 0>{};
    const auto null_item = std::array<const TimeSeriesType *, 1>{nullptr};
    const auto unresolved = std::array<const TimeSeriesType *, 1>{&TS<SCALAR>::type()};
    const auto mixed = std::array<const TimeSeriesType *, 2>{&TS<i64>::type(), &TS<float>::type()};

    CHECK_THROWS_WITH(TimeSeriesListType::from_items(empty), "a time series list type requires at least one item");
    CHECK_THROWS_WITH(TimeSeriesListType::from_items(null_item),
                      "a time series list type requires resolved item types");
    CHECK_THROWS_WITH(TimeSeriesListType::from_items(unresolved),
                      "a time series list type requires resolved item types");
    CHECK_THROWS_WITH(TimeSeriesListType::from_items(mixed), "a time series list requires compatible item types");
}

TEST_CASE("a list guarantee strengthens its item guarantee",
          "[type_system][tsl][series:fixed-tsl][feature:type-system][guarantee:ordinary][guarantee:final]") {
    const auto &list = TSL<TS<float, TimeSeriesGuarantee::ordinary>, 3, TimeSeriesGuarantee::final>::type();

    CHECK(list.item_type().guarantee().value() == TimeSeriesGuarantee::final);
}

TEST_CASE("generic time series list types resolve their item and guarantee",
          "[type_system][tsl][series:fixed-tsl][feature:type-system][declaration:generic]"
          "[guarantee:final]") {
    const auto &resolved_list = TSL<TS<float, TimeSeriesGuarantee::final>, 2, TimeSeriesGuarantee::final>::type();
    static const auto generic_item =
        util::static_storage<GenericTimeSeriesValueType>{float_type(), TimeSeriesGuarantee::final};
    [[maybe_unused]] static const auto &generic_item_registration = TypeRegistry::register_static(generic_item.get());
    static const auto generic_list =
        util::static_storage<GenericTimeSeriesListType>{generic_item.get(), 2, TimeSeriesGuarantee::final};
    [[maybe_unused]] static const auto &generic_list_registration = TypeRegistry::register_static(generic_list.get());
    auto type_map = TypeMap{};

    REQUIRE(generic_list.get().match(resolved_list, type_map));
    CHECK(&generic_list.get().resolve(type_map) == &resolved_list);
}

namespace {

struct ListVariableScope {};

} // namespace

TEST_CASE("a time series list size variable participates in type resolution",
          "[type_system][tsl][series:fixed-tsl][generic][declaration:generic][feature:type-system]") {
    const auto &parameter =
        details::time_series_type_for<ListVariableScope, 0, TimeSeriesTypeRole::input, TSL<TS<i64>, SIZE>>(
            TimeSeriesGuarantee::ordinary);
    const auto &argument = TSL<TS<i64>, 3>::type();
    auto type_map = TypeMap{};

    REQUIRE(parameter.match(argument, type_map));
    CHECK(&parameter.resolve(type_map) == &argument);
}

TEST_CASE("an output time series list can capture its size",
          "[type_system][tsl][series:fixed-tsl][generic][declaration:generic][feature:type-system]") {
    const auto &parameter =
        details::time_series_type_for<ListVariableScope, 0, TimeSeriesTypeRole::output, TSL<TS<i64>, SIZE>>(
            TimeSeriesGuarantee::ordinary);
    const auto &argument = TSL<TS<i64>, 3>::type();
    auto type_map = TypeMap{};

    REQUIRE(parameter.match(argument, type_map));
    CHECK(&parameter.resolve(type_map) == &argument);
}

TEST_CASE("repeated time series list size variables require equal sizes",
          "[type_system][tsl][series:fixed-tsl][generic][declaration:generic][feature:type-system]") {
    const auto &first =
        details::time_series_type_for<ListVariableScope, 0, TimeSeriesTypeRole::input, TSL<TS<i64>, SIZE>>(
            TimeSeriesGuarantee::ordinary);
    const auto &second =
        details::time_series_type_for<ListVariableScope, 1, TimeSeriesTypeRole::input, TSL<TS<i64>, SIZE>>(
            TimeSeriesGuarantee::ordinary);
    const auto &size_three = TSL<TS<i64>, 3>::type();
    const auto &size_four = TSL<TS<i64>, 4>::type();
    auto type_map = TypeMap{};

    REQUIRE(first.match(size_three, type_map));
    CHECK(second.match(size_three, type_map));
    CHECK_FALSE(second.match(size_four, type_map));
}

TEST_CASE("fixed time series list matching applies reference transparency to its items",
          "[type_system][tsl][series:fixed-tsl][feature:type-system][binding:reference-to-value]") {
    const auto &values = TSL<TS<float>, 2>::type();
    const auto &references = TSL<REF<TS<float>, TimeSeriesGuarantee::const_>, 2>::type();
    auto type_map = TypeMap{};

    CHECK(references.match(values, type_map));
    CHECK(values.match(references, type_map));
}
