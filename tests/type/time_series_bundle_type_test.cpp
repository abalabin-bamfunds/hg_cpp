#include <bg/scalar/cpp_runtime_type.hpp>
#include <bg/scalar/bundle/scalar_type.hpp>
#include <bg/time_series/time_series_type.hpp>
#include <bg/time_series/ts/ts_output.hpp>
#include <bg/time_series/tsb/tsb_type.hpp>
#include <bg/type/details/time_series_type.hpp>
#include <bg/type/time_series_bundle_type.hpp>
#include <bg/type/time_series_value_type.hpp>
#include <bg/type/type_map.hpp>
#include <bg/type/variable.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>

#include <array>
#include <stdexcept>

namespace {

template <template <typename, auto...> typename Field> struct BaseBundle {
    Field<bg::TS<bg::i64>> count;
};

template <template <typename, auto...> typename Field> struct DerivedBundle : bg::bundle_extends<BaseBundle, Field> {
    Field<bg::TS<float>> value;
};

template <template <typename, auto...> typename Field> struct SharedBundle {
    Field<bg::i64> count;
    Field<double, bg::optional> value;
};

template <template <typename, auto...> typename Field> struct InvalidBundle {
    Field<int *> value;
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
    CHECK(&type.item_type(0) == &bg::TS<bg::i64>::type());
    CHECK(&type.item_type(1) == &bg::TS<float>::type());
    CHECK(type.schema().base() != nullptr);
}

TEST_CASE("one schema can describe a hierarchical scalar and a time series bundle",
          "[feature:type-system][scalar:hierarchical][series:tsb][schema:named][declaration:concrete]") {
    const auto &scalar = bg::cpp_scalar_type<bg::Bundle<SharedBundle>>();
    const auto &time_series = bg::TSB<SharedBundle>::type();

    STATIC_CHECK(bg::details::hierarchical_scalar_schema_supported<SharedBundle>);
    STATIC_CHECK(bg::details::supported_time_series<bg::TSB<SharedBundle>>);
    CHECK(scalar.is_scalar());
    REQUIRE(time_series.item_count() == 2);
    CHECK(&time_series.item_type(0) == &bg::TS<bg::i64>::type());
    CHECK(&time_series.item_type(1) == &bg::TS<double>::type());

    const auto &hierarchical = bg::type_cast<bg::HierarchicalScalarType>(scalar);
    const auto &scalar_schema = static_cast<const bg::ResolvedHierarchicalScalarType &>(hierarchical).schema();
    const auto &time_series_schema = time_series.schema();
    CHECK(scalar_schema.identity() == time_series_schema.identity());
    CHECK(&scalar_schema != &time_series_schema);
}

TEST_CASE("explicit time series fields remain time series fields",
          "[feature:type-system][series:tsb][schema:named][declaration:concrete]") {
    STATIC_CHECK_FALSE(bg::details::hierarchical_scalar_schema_supported<BaseBundle>);
    STATIC_CHECK(bg::details::supported_time_series<bg::TSB<BaseBundle>>);
    CHECK(&bg::TSB<BaseBundle>::type().item_type(0) == &bg::TS<bg::i64>::type());
}

TEST_CASE("invalid shared-schema fields are rejected without instantiating a projection",
          "[feature:type-system][series:tsb][schema:named][declaration:concrete]") {
    STATIC_CHECK_FALSE(bg::details::hierarchical_scalar_schema_supported<InvalidBundle>);
    STATIC_CHECK_FALSE(bg::details::supported_time_series<bg::TSB<InvalidBundle>>);
}

TEST_CASE("a time series bundle type derives an unnamed schema from resolved fields",
          "[type_system][tsb][series:tsb][feature:type-system][schema:unnamed][guarantee:final]") {
    const auto fields = std::array{
        bg::SchemaField{bg::util::symbol{"count"}, &bg::TS<bg::i64>::type()},
        bg::SchemaField{bg::util::symbol{"value"}, &bg::TS<float>::type()},
        bg::SchemaField{bg::util::symbol{"reference"}, &bg::REF<bg::TS<float>>::type()},
    };

    const auto &type = bg::TimeSeriesBundleType::from_fields(fields, bg::TimeSeriesGuarantee::final);

    CHECK(type.item_count() == 3);
    CHECK(type.guarantee().value() == bg::TimeSeriesGuarantee::final);
    CHECK(type.item_type(0).guarantee().value() == bg::TimeSeriesGuarantee::final);
    CHECK(&type.item_type(2) ==
          &bg::REF<bg::TS<float>, bg::TimeSeriesGuarantee::final>::type());
}

TEST_CASE("a time series bundle type requires concrete fields",
          "[type_system][tsb][series:tsb][feature:type-system][schema:unnamed][declaration:generic]") {
    const auto null_field = std::array{bg::SchemaField{bg::util::symbol{"value"}, nullptr}};
    const auto unresolved_field =
        std::array{bg::SchemaField{bg::util::symbol{"value"}, &bg::TS<bg::SCALAR>::type()}};
    const auto duplicate_fields = std::array{
        bg::SchemaField{bg::util::symbol{"value"}, &bg::TS<bg::i64>::type()},
        bg::SchemaField{bg::util::symbol{"value"}, &bg::TS<float>::type()},
    };

    CHECK_THROWS_WITH(bg::TimeSeriesBundleType::from_fields(null_field),
                      "a time series bundle type requires resolved time-series fields");
    CHECK_THROWS_WITH(bg::TimeSeriesBundleType::from_fields(unresolved_field),
                      "a time series bundle type requires resolved time-series fields");
    CHECK_THROWS_WITH(bg::TimeSeriesBundleType::from_fields(duplicate_fields),
                      "a schema has duplicate field names");
}

namespace {

template <template <typename, auto...> typename Field> struct OrdinaryBundle {
    Field<bg::TS<bg::i64, bg::TimeSeriesGuarantee::ordinary>> value;
};

} // namespace

TEST_CASE("a bundle guarantee strengthens its field guarantees",
          "[type_system][tsb][series:tsb][feature:type-system][schema:named]"
          "[guarantee:ordinary][guarantee:final]") {
    const auto &type = bg::TSB<OrdinaryBundle, bg::TimeSeriesGuarantee::final>::type();

    CHECK(type.item_type(0).guarantee().value() == bg::TimeSeriesGuarantee::final);
}

TEST_CASE("an unnamed bundle matches a compatible named schema",
          "[type_system][tsb][series:tsb][feature:type-system][schema:unnamed]") {
    const auto &parameter = static_cast<const bg::TimeSeriesBundleType &>(
        bg::details::time_series_type_for<BundleTypeScope, 0, true, bg::TSB<BaseBundle>>(
            bg::TimeSeriesGuarantee::ordinary));
    const auto &count = bg::TS<bg::i64>::type();
    const auto fields = std::array{bg::SchemaField{bg::util::symbol{"count"}, &count}};
    const auto &argument =
        bg::time_series_bundle_type(bg::anonymous_schema(fields), bg::TimeSeriesGuarantee::ordinary);
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

template <template <typename, auto...> typename Field> struct RedefinedBundle : bg::bundle_extends<BaseBundle, Field> {
    Field<bg::TS<bg::i64>> count;
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

template <template <typename, auto...> typename Field> struct GenericBundle {
    Field<bg::TS<bg::SCALAR>> value;
};

} // namespace

TEST_CASE("bundle matching resolves generic field types",
          "[type_system][tsb][series:tsb][generic][declaration:generic][feature:type-system]") {
    const auto &parameter = static_cast<const bg::TimeSeriesBundleType &>(
        bg::details::time_series_type_for<BundleTypeScope, 0, true, bg::TSB<GenericBundle>>(
            bg::TimeSeriesGuarantee::ordinary));
    const auto &value = bg::TS<bg::i64>::type();
    const auto fields = std::array{bg::SchemaField{bg::util::symbol{"value"}, &value}};
    const auto &argument =
        bg::time_series_bundle_type(bg::anonymous_schema(fields), bg::TimeSeriesGuarantee::ordinary);
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

    CHECK(parameter.item_count() == std::dynamic_extent);
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

template <template <typename, auto...> typename Field> struct UnrelatedBundle {
    Field<bg::TS<bg::i64>> count;
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

template <template <typename, auto...> typename Field>
struct IncompatibleRedefinedBundle : bg::bundle_extends<BaseBundle, Field> {
    Field<bg::TS<float>> count;
};

} // namespace

TEST_CASE("a redefined bundle field must be narrower",
          "[type_system][tsb][series:tsb][inheritance][feature:type-system][schema:named]") {
    CHECK_THROWS_AS(bg::TSB<IncompatibleRedefinedBundle>::type(), std::invalid_argument);
}
