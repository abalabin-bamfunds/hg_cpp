#include <bg/scalar/cpp_storage_representation.hpp>
#include <bg/time_series/ts/ts_output.hpp>
#include <bg/time_series/tsb/tsb_type.hpp>
#include <bg/type/details/time_series_type.hpp>
#include <bg/type/time_series_bundle_type.hpp>
#include <bg/type/time_series_value_type.hpp>
#include <bg/type/type_map.hpp>

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

TEST_CASE("bundle schemas include inherited fields", "[type_system][tsb]") {
    const auto &type = static_cast<const bg::TimeSeriesBundleType &>(
        bg::details::time_series_type_for<BundleTypeScope, 0, false, bg::TSB<DerivedBundle>>(
            bg::TimeSeriesGuarantee::ordinary));

    REQUIRE(type.item_count() == 2);
    CHECK(type.item_name(0).text() == "count");
    CHECK(type.item_name(1).text() == "value");
    CHECK(&type.item_type(0) ==
          &bg::time_series_value_type(bg::cpp_scalar_type<int>(), bg::TimeSeriesGuarantee::ordinary));
    CHECK(&type.item_type(1) == &bg::time_series_value_type(bg::float_type(), bg::TimeSeriesGuarantee::ordinary));
    CHECK(type.schema().base() != nullptr);
}

TEST_CASE("an unnamed bundle matches a compatible named schema", "[type_system][tsb]") {
    const auto &parameter = static_cast<const bg::TimeSeriesBundleType &>(
        bg::details::time_series_type_for<BundleTypeScope, 0, true, bg::TSB<BaseBundle>>(
            bg::TimeSeriesGuarantee::ordinary));
    const auto &count = bg::time_series_value_type(bg::cpp_scalar_type<int>(), bg::TimeSeriesGuarantee::ordinary);
    const auto fields = std::array{bg::BundleField{bg::util::symbol{"count"}, &count}};
    const auto &argument =
        bg::time_series_bundle_type(bg::unnamed_bundle_schema(fields), bg::TimeSeriesGuarantee::ordinary);
    auto bindings = bg::TypeMap{};

    CHECK(parameter.match(argument, bindings));
}

TEST_CASE("bundle field policies preserve inherited member access", "[type_system][tsb]") {
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

TEST_CASE("a redefined bundle field keeps its inherited position", "[type_system][tsb][inheritance]") {
    const auto &type = static_cast<const bg::TimeSeriesBundleType &>(
        bg::details::time_series_type_for<BundleTypeScope, 0, false, bg::TSB<RedefinedBundle>>(
            bg::TimeSeriesGuarantee::ordinary));

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

TEST_CASE("bundle matching resolves generic field types", "[type_system][tsb][generic]") {
    const auto &parameter = static_cast<const bg::TimeSeriesBundleType &>(
        bg::details::time_series_type_for<BundleTypeScope, 0, true, bg::TSB<GenericBundle>>(
            bg::TimeSeriesGuarantee::ordinary));
    const auto &value = bg::time_series_value_type(bg::cpp_scalar_type<int>(), bg::TimeSeriesGuarantee::ordinary);
    const auto fields = std::array{bg::BundleField{bg::util::symbol{"value"}, &value}};
    const auto &argument =
        bg::time_series_bundle_type(bg::unnamed_bundle_schema(fields), bg::TimeSeriesGuarantee::ordinary);
    auto bindings = bg::TypeMap{};

    REQUIRE(parameter.match(argument, bindings));
    const auto &resolved = static_cast<const bg::TimeSeriesBundleType &>(parameter.resolve(bindings));
    CHECK(&resolved.item_type(0) == &value);
}

namespace {

template <template <typename> typename Field> struct UnrelatedBundle {
    Field<bg::TS<int>> count;
};

} // namespace

TEST_CASE("named bundles with the same fields remain nominally distinct", "[type_system][tsb]") {
    const auto &parameter = static_cast<const bg::TimeSeriesBundleType &>(
        bg::details::time_series_type_for<BundleTypeScope, 0, true, bg::TSB<BaseBundle>>(
            bg::TimeSeriesGuarantee::ordinary));
    const auto &argument = static_cast<const bg::TimeSeriesBundleType &>(
        bg::details::time_series_type_for<BundleTypeScope, 1, false, bg::TSB<UnrelatedBundle>>(
            bg::TimeSeriesGuarantee::ordinary));
    auto bindings = bg::TypeMap{};

    CHECK_FALSE(parameter.match(argument, bindings));
}

namespace {

template <template <typename> typename Field> struct IncompatibleRedefinedBundle
    : bg::bundle_extends<BaseBundle, Field> {
    Field<bg::TS<float>> count;
};

} // namespace

TEST_CASE("a redefined bundle field must be narrower", "[type_system][tsb][inheritance]") {
    CHECK_THROWS_AS(
        (bg::details::time_series_type_for<BundleTypeScope, 0, false, bg::TSB<IncompatibleRedefinedBundle>>(
            bg::TimeSeriesGuarantee::ordinary)),
        std::invalid_argument);
}
