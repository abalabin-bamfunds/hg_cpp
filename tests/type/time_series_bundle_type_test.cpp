#include <bg/scalar/cpp_runtime_type.hpp>
#include <bg/time_series/ts/ts_output.hpp>
#include <bg/time_series/tsb/tsb_type.hpp>
#include <bg/type/details/time_series_type.hpp>
#include <bg/type/time_series_bundle_type.hpp>
#include <bg/type/time_series_value_type.hpp>
#include <bg/type/type_map.hpp>
#include <bg/type/variable.hpp>

#include <catch2/catch_test_macros.hpp>

#include <stdexcept>

namespace {

template <template <typename> typename Field> struct BaseBundle {
    Field<bg::TS<int>> count;
};

template <template <typename> typename Field> struct DerivedBundle : bg::bundle_extends<BaseBundle, Field> {
    Field<bg::TS<float>> value;
};

struct BundleTypeScope {};

template <typename> struct TestField {
    int value;
};

} // namespace

TEST_CASE("bundle schemas include inherited fields",
          "[type_system][tsb][series:tsb][feature:type-system][schema:named]") {
    const auto &type = bg::TSB<DerivedBundle>::type();

    REQUIRE(type.item_count() == 2);
    CHECK(type.item_name(0).text() == "count");
    CHECK(type.item_name(1).text() == "value");
    CHECK(&type.item_type(0) == &bg::TS<int>::type());
    CHECK(&type.item_type(1) == &bg::TS<float>::type());
    CHECK(type.schema().base() != nullptr);
}

TEST_CASE("an unnamed bundle matches a compatible named schema",
          "[type_system][tsb][series:tsb][feature:type-system][schema:unnamed]") {
    const auto &parameter = static_cast<const bg::TimeSeriesBundleType &>(
        bg::details::time_series_type_for<BundleTypeScope, 0, true, bg::TSB<BaseBundle>>(
            bg::TimeSeriesGuarantee::ordinary));
    const auto &count = bg::TS<int>::type();
    const auto fields = std::array{bg::BundleField{bg::util::symbol{"count"}, &count}};
    const auto &argument =
        bg::time_series_bundle_type(bg::unnamed_bundle_schema(fields), bg::TimeSeriesGuarantee::ordinary);
    auto type_map = bg::TypeMap{};

    CHECK(parameter.match(argument, type_map));
}

TEST_CASE("bundle field policies preserve inherited member access",
          "[type_system][tsb][series:tsb][feature:type-system][schema:named]") {
    auto bundle = DerivedBundle<TestField>{};
    bundle.count.value = 1;
    bundle.value.value = 2;

    CHECK(bundle.count.value == 1);
    CHECK(bundle.value.value == 2);
}

namespace {

template <template <typename> typename Field> struct RedefinedBundle : bg::bundle_extends<BaseBundle, Field> {
    Field<bg::TS<int>> count;
    Field<bg::TS<float>> value;
};

} // namespace

TEST_CASE("a redefined bundle field keeps its inherited position",
          "[type_system][tsb][series:tsb][inheritance][feature:type-system][schema:named]") {
    const auto &type = bg::TSB<RedefinedBundle>::type();

    REQUIRE(type.item_count() == 2);
    CHECK(type.item_name(0).text() == "count");
    CHECK(type.item_name(1).text() == "value");
    CHECK(bg::details::bundle_field_index<"count", RedefinedBundle> == 0);
    CHECK(bg::details::bundle_field_index<"value", RedefinedBundle> == 1);
}

namespace {

template <template <typename> typename Field> struct GenericBundle {
    Field<bg::TS<bg::SCALAR>> value;
};

} // namespace

TEST_CASE("bundle matching resolves generic field types",
          "[type_system][tsb][series:tsb][generic][declaration:generic][feature:type-system]") {
    const auto &parameter = static_cast<const bg::TimeSeriesBundleType &>(
        bg::details::time_series_type_for<BundleTypeScope, 0, true, bg::TSB<GenericBundle>>(
            bg::TimeSeriesGuarantee::ordinary));
    const auto &value = bg::TS<int>::type();
    const auto fields = std::array{bg::BundleField{bg::util::symbol{"value"}, &value}};
    const auto &argument =
        bg::time_series_bundle_type(bg::unnamed_bundle_schema(fields), bg::TimeSeriesGuarantee::ordinary);
    auto type_map = bg::TypeMap{};

    REQUIRE(parameter.match(argument, type_map));
    const auto &resolved = static_cast<const bg::TimeSeriesBundleType &>(parameter.resolve(type_map));
    CHECK(&resolved.item_type(0) == &value);
}

TEST_CASE("a bundle schema variable participates in type resolution",
          "[type_system][tsb][series:tsb][generic][declaration:generic][feature:type-system]"
          "[schema:generic]") {
    const auto &parameter = static_cast<const bg::TimeSeriesBundleType &>(
        bg::details::time_series_type_for<BundleTypeScope, 0, true, bg::TSB<bg::SCHEMA>>(
            bg::TimeSeriesGuarantee::ordinary));
    const auto &argument = bg::TSB<DerivedBundle>::type();
    auto type_map = bg::TypeMap{};

    REQUIRE(parameter.match(argument, type_map));
    const auto &variable = dynamic_cast<const bg::Variable &>(parameter.schema_type());
    const auto *mapping = type_map.find(variable);
    REQUIRE(mapping != nullptr);
    CHECK(std::get<const bg::Type *>(*mapping) == &argument.schema());
    CHECK(&parameter.resolve(type_map) == &argument);
}

TEST_CASE("repeated bundle schema variables require the same schema",
          "[type_system][tsb][series:tsb][generic][declaration:generic][feature:type-system]"
          "[schema:generic]") {
    const auto &first = bg::details::time_series_type_for<BundleTypeScope, 0, true, bg::TSB<bg::SCHEMA>>(
        bg::TimeSeriesGuarantee::ordinary);
    const auto &second = bg::details::time_series_type_for<BundleTypeScope, 1, true, bg::TSB<bg::SCHEMA>>(
        bg::TimeSeriesGuarantee::ordinary);
    const auto &derived = bg::TSB<DerivedBundle>::type();
    const auto &base = bg::TSB<BaseBundle>::type();
    auto type_map = bg::TypeMap{};

    REQUIRE(first.match(derived, type_map));
    CHECK(second.match(derived, type_map));
    CHECK_FALSE(second.match(base, type_map));
}

namespace {

template <template <typename> typename Field> struct UnrelatedBundle {
    Field<bg::TS<int>> count;
};

} // namespace

TEST_CASE("named bundles with the same fields remain nominally distinct",
          "[type_system][tsb][series:tsb][feature:type-system][schema:named]") {
    const auto &parameter = static_cast<const bg::TimeSeriesBundleType &>(
        bg::details::time_series_type_for<BundleTypeScope, 0, true, bg::TSB<BaseBundle>>(
            bg::TimeSeriesGuarantee::ordinary));
    const auto &argument = bg::TSB<UnrelatedBundle>::type();
    auto type_map = bg::TypeMap{};

    CHECK_FALSE(parameter.match(argument, type_map));
}

namespace {

template <template <typename> typename Field>
struct IncompatibleRedefinedBundle : bg::bundle_extends<BaseBundle, Field> {
    Field<bg::TS<float>> count;
};

} // namespace

TEST_CASE("a redefined bundle field must be narrower",
          "[type_system][tsb][series:tsb][inheritance][feature:type-system][schema:named]") {
    CHECK_THROWS_AS(bg::TSB<IncompatibleRedefinedBundle>::type(), std::invalid_argument);
}
