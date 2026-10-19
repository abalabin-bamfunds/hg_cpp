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

TEST_CASE("common list types combine root and item guarantees recursively",
          "[feature:type-system][series:fixed-tsl][guarantee:ordinary][guarantee:final][guarantee:const]") {
    const auto &final_list =
        bg::TSL<bg::TS<bg::i64, bg::TimeSeriesGuarantee::final>, 2, bg::TimeSeriesGuarantee::final>::type();
    const auto &constant_items = bg::TSL<bg::TS<bg::i64, bg::TimeSeriesGuarantee::const_>, 2>::type();
    const auto *common = final_list.common_type(constant_items);
    const auto &expected = bg::TSL<bg::TS<bg::i64, bg::TimeSeriesGuarantee::final>, 2>::type();

    REQUIRE(common != nullptr);
    CHECK(common == &expected);
    CHECK(constant_items.common_type(final_list) == common);
}

TEST_CASE("common sparse lists preserve their size form and combine item guarantees",
          "[feature:type-system][storage:variable][guarantee:ordinary][guarantee:final]") {
    const auto &final_items = bg::time_series_list_type(bg::TS<bg::i64, bg::TimeSeriesGuarantee::final>::type(),
                                                        std::dynamic_extent, bg::TimeSeriesGuarantee::ordinary);
    const auto &ordinary_items = bg::time_series_list_type(bg::TS<bg::i64>::type(), std::dynamic_extent,
                                                           bg::TimeSeriesGuarantee::ordinary);
    const auto *common = final_items.common_type(ordinary_items);

    REQUIRE(common != nullptr);
    const auto &list = bg::type_cast<bg::TimeSeriesListType>(*common);
    CHECK(list.item_count() == std::dynamic_extent);
    CHECK(list.guarantee().value() == bg::TimeSeriesGuarantee::ordinary);
    CHECK(list.item_type().guarantee().value() == bg::TimeSeriesGuarantee::ordinary);
    const auto &fixed = bg::TSL<bg::TS<bg::i64>, 2>::type();
    CHECK(fixed.common_type(ordinary_items) == nullptr);
    CHECK(ordinary_items.common_type(fixed) == nullptr);
    const auto &i64_items = bg::time_series_list_type(bg::TS<bg::i64>::type(), std::dynamic_extent,
                                                      bg::TimeSeriesGuarantee::ordinary);
    const auto &float_items = bg::time_series_list_type(bg::TS<float>::type(), std::dynamic_extent,
                                                        bg::TimeSeriesGuarantee::ordinary);
    CHECK(i64_items.common_type(float_items) == nullptr);
    CHECK(float_items.common_type(i64_items) == nullptr);

    const auto &unresolved = bg::TSL<bg::TS<bg::SCALAR>, 2>::type();
    const auto &resolved = bg::TSL<bg::TS<bg::i64>, 2>::type();
    CHECK(unresolved.common_type(resolved) == nullptr);
    CHECK(resolved.common_type(unresolved) == nullptr);
}

TEST_CASE("common nested list types combine each guarantee independently",
          "[feature:type-system][series:fixed-tsl][guarantee:ordinary][guarantee:final][guarantee:const]") {
    const auto &left = bg::TSL<bg::TSL<bg::TS<bg::i64, bg::TimeSeriesGuarantee::const_>, 2,
                                        bg::TimeSeriesGuarantee::final>,
                              3, bg::TimeSeriesGuarantee::final>::type();
    const auto &right = bg::TSL<bg::TSL<bg::TS<bg::i64, bg::TimeSeriesGuarantee::final>, 2>, 3>::type();
    const auto *common = left.common_type(right);

    REQUIRE(common != nullptr);
    const auto &outer = bg::type_cast<bg::TimeSeriesListType>(*common);
    const auto &inner = bg::type_cast<bg::TimeSeriesListType>(outer.item_type());
    CHECK(outer.guarantee().value() == bg::TimeSeriesGuarantee::ordinary);
    CHECK(inner.guarantee().value() == bg::TimeSeriesGuarantee::ordinary);
    CHECK(inner.item_type().guarantee().value() == bg::TimeSeriesGuarantee::final);
}

TEST_CASE("a time series list type derives its shape from resolved items",
          "[type_system][tsl][series:fixed-tsl][feature:type-system]"
          "[guarantee:ordinary][guarantee:final]") {
    const auto &ordinary = bg::TS<float>::type();
    const auto &final = bg::TS<float, bg::TimeSeriesGuarantee::final>::type();
    const auto &reference = bg::REF<bg::TS<float, bg::TimeSeriesGuarantee::final>>::type();
    const auto items = std::array<const bg::TimeSeriesType *, 3>{&ordinary, &final, &reference};

    const auto &list = bg::TimeSeriesListType::from_items(items, bg::TimeSeriesGuarantee::final);

    CHECK(list.item_count() == 3);
    CHECK(list.guarantee().value() == bg::TimeSeriesGuarantee::final);
    CHECK(&list.item_type() == &final);
}

TEST_CASE("dereferencing a direct time series preserves its type and guarantee",
          "[type_system][feature:type-system][series:value][guarantee:final]") {
    const auto &direct = bg::TS<float, bg::TimeSeriesGuarantee::final>::type();

    CHECK(&direct.dereferenced_type() == &direct);
    CHECK(direct.dereferenced_type().guarantee().value() == bg::TimeSeriesGuarantee::final);
}

TEST_CASE("a time series list type applies reference guarantees to dereferenced items",
          "[type_system][tsl][reference][series:fixed-tsl][feature:type-system]"
          "[guarantee:ordinary][guarantee:deduplicated][guarantee:final][guarantee:const]") {
    constexpr auto guarantees = std::array{bg::TimeSeriesGuarantee::ordinary,
                                           bg::TimeSeriesGuarantee::deduplicated,
                                           bg::TimeSeriesGuarantee::final,
                                           bg::TimeSeriesGuarantee::const_};
    constexpr auto expected = std::array{
        std::array{bg::TimeSeriesGuarantee::ordinary, bg::TimeSeriesGuarantee::ordinary,
                   bg::TimeSeriesGuarantee::ordinary, bg::TimeSeriesGuarantee::ordinary},
        std::array{bg::TimeSeriesGuarantee::ordinary, bg::TimeSeriesGuarantee::ordinary,
                   bg::TimeSeriesGuarantee::ordinary, bg::TimeSeriesGuarantee::ordinary},
        std::array{bg::TimeSeriesGuarantee::ordinary, bg::TimeSeriesGuarantee::deduplicated,
                   bg::TimeSeriesGuarantee::final, bg::TimeSeriesGuarantee::final},
        std::array{bg::TimeSeriesGuarantee::ordinary, bg::TimeSeriesGuarantee::deduplicated,
                   bg::TimeSeriesGuarantee::final, bg::TimeSeriesGuarantee::const_},
    };

    for (auto reference_index = std::size_t{}; reference_index < guarantees.size(); ++reference_index) {
        for (auto target_index = std::size_t{}; target_index < guarantees.size(); ++target_index) {
            const auto &target = bg::time_series_value_type(bg::int64_type(), guarantees[target_index]);
            const auto &reference = bg::time_series_reference_type(target, guarantees[reference_index]);
            const auto items = std::array<const bg::TimeSeriesType *, 1>{&reference};
            const auto &list = bg::TimeSeriesListType::from_items(items);
            CAPTURE(guarantees[reference_index], guarantees[target_index]);

            CHECK(list.item_type().guarantee().value() == expected[reference_index][target_index]);
        }
    }
}

TEST_CASE("a time series list type requires concrete homogeneous items",
          "[type_system][tsl][series:fixed-tsl][feature:type-system][declaration:generic]") {
    const auto empty = std::array<const bg::TimeSeriesType *, 0>{};
    const auto null_item = std::array<const bg::TimeSeriesType *, 1>{nullptr};
    const auto unresolved = std::array<const bg::TimeSeriesType *, 1>{&bg::TS<bg::SCALAR>::type()};
    const auto mixed = std::array<const bg::TimeSeriesType *, 2>{&bg::TS<bg::i64>::type(), &bg::TS<float>::type()};

    CHECK_THROWS_WITH(bg::TimeSeriesListType::from_items(empty),
                      "a time series list type requires at least one item");
    CHECK_THROWS_WITH(bg::TimeSeriesListType::from_items(null_item),
                      "a time series list type requires resolved item types");
    CHECK_THROWS_WITH(bg::TimeSeriesListType::from_items(unresolved),
                      "a time series list type requires resolved item types");
    CHECK_THROWS_WITH(bg::TimeSeriesListType::from_items(mixed),
                      "a time series list requires compatible item types");
}

TEST_CASE("a list guarantee strengthens its item guarantee",
          "[type_system][tsl][series:fixed-tsl][feature:type-system][guarantee:ordinary][guarantee:final]") {
    const auto &list =
        bg::TSL<bg::TS<float, bg::TimeSeriesGuarantee::ordinary>, 3, bg::TimeSeriesGuarantee::final>::type();

    CHECK(list.item_type().guarantee().value() == bg::TimeSeriesGuarantee::final);
}

TEST_CASE("generic time series list types resolve their item and guarantee",
          "[type_system][tsl][series:fixed-tsl][feature:type-system][declaration:generic]"
          "[guarantee:final]") {
    const auto &resolved_list =
        bg::TSL<bg::TS<float, bg::TimeSeriesGuarantee::final>, 2, bg::TimeSeriesGuarantee::final>::type();
    static const auto generic_item = bg::util::static_storage<bg::GenericTimeSeriesValueType>{
        bg::float_type(), bg::TimeSeriesGuarantee::final};
    [[maybe_unused]] static const auto &generic_item_registration =
        bg::TypeRegistry::register_static(generic_item.get());
    static const auto generic_list = bg::util::static_storage<bg::GenericTimeSeriesListType>{
        generic_item.get(), 2, bg::TimeSeriesGuarantee::final};
    [[maybe_unused]] static const auto &generic_list_registration =
        bg::TypeRegistry::register_static(generic_list.get());
    auto type_map = bg::TypeMap{};

    REQUIRE(generic_list.get().match(resolved_list, type_map));
    CHECK(&generic_list.get().resolve(type_map) == &resolved_list);
}

TEST_CASE("a time series list size variable participates in type resolution",
          "[type_system][tsl][series:fixed-tsl][generic][declaration:generic][feature:type-system]") {
    const auto &parameter =
        bg::details::time_series_type_for<ListVariableScope, 0, bg::TimeSeriesTypeRole::input,
                                          bg::TSL<bg::TS<bg::i64>, bg::SIZE>>(
            bg::TimeSeriesGuarantee::ordinary);
    const auto &argument = bg::TSL<bg::TS<bg::i64>, 3>::type();
    auto type_map = bg::TypeMap{};

    REQUIRE(parameter.match(argument, type_map));
    CHECK(&parameter.resolve(type_map) == &argument);
}

TEST_CASE("an output time series list can capture its size",
          "[type_system][tsl][series:fixed-tsl][generic][declaration:generic][feature:type-system]") {
    const auto &parameter =
        bg::details::time_series_type_for<ListVariableScope, 0, bg::TimeSeriesTypeRole::output,
                                          bg::TSL<bg::TS<bg::i64>, bg::SIZE>>(
            bg::TimeSeriesGuarantee::ordinary);
    const auto &argument = bg::TSL<bg::TS<bg::i64>, 3>::type();
    auto type_map = bg::TypeMap{};

    REQUIRE(parameter.match(argument, type_map));
    CHECK(&parameter.resolve(type_map) == &argument);
}

TEST_CASE("repeated time series list size variables require equal sizes",
          "[type_system][tsl][series:fixed-tsl][generic][declaration:generic][feature:type-system]") {
    const auto &first =
        bg::details::time_series_type_for<ListVariableScope, 0, bg::TimeSeriesTypeRole::input,
                                          bg::TSL<bg::TS<bg::i64>, bg::SIZE>>(
            bg::TimeSeriesGuarantee::ordinary);
    const auto &second =
        bg::details::time_series_type_for<ListVariableScope, 1, bg::TimeSeriesTypeRole::input,
                                          bg::TSL<bg::TS<bg::i64>, bg::SIZE>>(
            bg::TimeSeriesGuarantee::ordinary);
    const auto &size_three = bg::TSL<bg::TS<bg::i64>, 3>::type();
    const auto &size_four = bg::TSL<bg::TS<bg::i64>, 4>::type();
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
