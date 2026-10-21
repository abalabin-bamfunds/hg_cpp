#include <bg/scalar/cpp_runtime_type.hpp>
#include <bg/scalar/bundle/scalar_type.hpp>
#include <bg/schema/reflection.hpp>
#include <bg/time_series/time_series_type.hpp>
#include <bg/time_series/ts/ts_output.hpp>
#include <bg/time_series/tsb/tsb_type.hpp>
#include <bg/type/details/time_series_type.hpp>
#include <bg/type/time_series_bundle_type.hpp>
#include <bg/type/time_series_list_type.hpp>
#include <bg/type/time_series_reference_type.hpp>
#include <bg/type/time_series_value_type.hpp>
#include <bg/type/type_map.hpp>
#include <bg/type/variable.hpp>
#include <bg/type/schema_metadata_values.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>

#include <array>
#include <stdexcept>
#include <typeindex>

using namespace bg;

namespace {

template <template <typename, auto...> typename Field> struct BaseBundle {
    Field<TS<i64>> count;
};

template <template <typename, auto...> typename Field> struct DerivedBundle : bundle_extends<BaseBundle, Field> {
    Field<TS<float>> value;
};

template <template <typename, auto...> typename Field> struct SharedBundle {
    Field<i64> count;
    Field<double, bg::optional> value;
};

template <template <typename, auto...> typename Field> struct InvalidBundle {
    Field<int *> value;
};

template <template <typename, auto...> typename Field> struct UnresolvedFieldBundle {
    Field<TS<i64, guarantee_variable<"G1">>> value;
};

struct BundleTypeScope {};

template <typename> struct TestField {
    int value;
};

} // namespace

TEST_CASE("bundle schemas include inherited fields",
          "[type_system][tsb][series:tsb][feature:type-system][schema:named]") {
    const auto &type = TSB<DerivedBundle>::type();

    REQUIRE(type.item_count() == 2);
    CHECK(type.item_name(0).text() == "count");
    CHECK(type.item_name(1).text() == "value");
    CHECK(&type.item_type(0) == &TS<i64>::type());
    CHECK(&type.item_type(1) == &TS<float>::type());
    CHECK(type.schema().base() != nullptr);
}

TEST_CASE("common bundles preserve schema identity and combine nested field guarantees",
          "[feature:type-system][series:tsb][schema:unnamed][guarantee:ordinary][guarantee:final]"
          "[guarantee:const]") {
    const auto left_fields = std::array{
        SchemaField{util::symbol{"value"}, &TS<i64, TimeSeriesGuarantee::final>::type()},
        SchemaField{util::symbol{"items"}, &TSL<TS<float, TimeSeriesGuarantee::final>, 2>::type()},
    };
    const auto right_fields = std::array{
        SchemaField{util::symbol{"value"}, &TS<i64, TimeSeriesGuarantee::const_>::type()},
        SchemaField{util::symbol{"items"}, &TSL<TS<float, TimeSeriesGuarantee::const_>, 2>::type()},
    };
    const auto &left_schema = anonymous_schema(left_fields);
    const auto &right_schema = anonymous_schema(right_fields);
    const auto &left = time_series_bundle_type(left_schema, TimeSeriesGuarantee::final);
    const auto &right = time_series_bundle_type(right_schema, TimeSeriesGuarantee::ordinary);
    const auto *common = left.common_type(right);

    REQUIRE(common != nullptr);
    const auto &bundle = type_cast<TimeSeriesBundleType>(*common);
    CHECK(bundle.guarantee().value() == TimeSeriesGuarantee::ordinary);
    CHECK(bundle.schema().identity() == left.schema().identity());
    CHECK(bundle.item_name(0) == left.item_name(0));
    CHECK(bundle.item_name(1) == left.item_name(1));
    CHECK(bundle.item_type(0).guarantee().value() == TimeSeriesGuarantee::final);
    const auto &items = type_cast<TimeSeriesListType>(bundle.item_type(1));
    CHECK(items.item_type().guarantee().value() == TimeSeriesGuarantee::final);
    CHECK(right.common_type(left) == common);
}

TEST_CASE("TSB schema transformations preserve metadata and common types require matching metadata",
          "[feature:type-system][series:tsb][schema:named][guarantee:ordinary][guarantee:final]") {
    const auto metadata = make_schema_metadata({schema_metadata_entry("extension.owner", ScalarValue<i64>{7})});
    const auto reference_fields = std::array{SchemaField{util::symbol{"value"}, &REF<TS<i64>>::type()}};
    const auto &reference_schema = bg::schema(util::symbol{"MetadataBundle"}, std::type_index{typeid(BundleTypeScope)},
                                              nullptr, reference_fields, metadata);
    const auto &source = time_series_bundle_type(reference_schema, TimeSeriesGuarantee::ordinary);

    CHECK(source.schema().direct_metadata() == metadata);

    const auto &limited =
        static_cast<const TimeSeriesBundleType &>(source.with_guarantee_limit(TimeSeriesGuarantee::final));
    CHECK(limited.schema().direct_metadata() == metadata);

    const auto &reference_free = static_cast<const TimeSeriesBundleType &>(source.reference_free_type());
    CHECK(reference_free.schema().direct_metadata() == metadata);

    const auto fields = std::array{SchemaField{util::symbol{"value"}, &TS<i64>::type()}};
    const auto &left_schema = anonymous_schema(fields, metadata);
    const auto &right_schema = anonymous_schema(fields, metadata);
    const auto &left = time_series_bundle_type(left_schema, TimeSeriesGuarantee::final);
    const auto &right = time_series_bundle_type(right_schema, TimeSeriesGuarantee::ordinary);
    const auto *common = left.common_type(right);

    REQUIRE(common != nullptr);
    CHECK(type_cast<TimeSeriesBundleType>(*common).schema().direct_metadata() == metadata);

    const auto different_metadata =
        make_schema_metadata({schema_metadata_entry("extension.owner", ScalarValue<i64>{8})});
    const auto identity = std::type_index{typeid(TestField<double>)};
    const auto &different_left_schema = bg::schema(util::symbol{"MetadataBundle"}, identity, nullptr, fields, metadata);
    const auto &different_right_schema =
        bg::schema(util::symbol{"MetadataBundle"}, identity, nullptr, fields, different_metadata);
    const auto &different_left = time_series_bundle_type(different_left_schema, TimeSeriesGuarantee::ordinary);
    const auto &different_right = time_series_bundle_type(different_right_schema, TimeSeriesGuarantee::ordinary);

    CHECK(different_left.common_type(different_right) == nullptr);
}

TEST_CASE("common named bundles retain their base and combine a narrowed inherited field",
          "[feature:type-system][series:tsb][schema:named][guarantee:ordinary][guarantee:final]"
          "[guarantee:const]") {
    const auto base_identity = std::type_index{typeid(BundleTypeScope)};
    const auto derived_identity = std::type_index{typeid(TestField<int>)};
    const auto base_left_fields =
        std::array{SchemaField{util::symbol{"value"}, &TS<i64, TimeSeriesGuarantee::final>::type()}};
    const auto base_right_fields =
        std::array{SchemaField{util::symbol{"value"}, &TS<i64, TimeSeriesGuarantee::ordinary>::type()}};
    const auto &base_left = bg::schema(util::symbol{"CommonBase"}, base_identity, nullptr, base_left_fields);
    const auto &base_right = bg::schema(util::symbol{"CommonBase"}, base_identity, nullptr, base_right_fields);
    const auto derived_left_fields = std::array{
        SchemaField{util::symbol{"value"}, &TS<i64, TimeSeriesGuarantee::const_>::type()},
        SchemaField{util::symbol{"extra"}, &TS<float, TimeSeriesGuarantee::final>::type()},
    };
    const auto derived_right_fields = std::array{
        SchemaField{util::symbol{"value"}, &TS<i64, TimeSeriesGuarantee::final>::type()},
        SchemaField{util::symbol{"extra"}, &TS<float, TimeSeriesGuarantee::ordinary>::type()},
    };
    const auto &derived_left =
        bg::schema(util::symbol{"CommonDerived"}, derived_identity, &base_left, derived_left_fields);
    const auto &derived_right =
        bg::schema(util::symbol{"CommonDerived"}, derived_identity, &base_right, derived_right_fields);
    const auto &left = time_series_bundle_type(derived_left, TimeSeriesGuarantee::final);
    const auto &right = time_series_bundle_type(derived_right, TimeSeriesGuarantee::ordinary);
    const auto *common = left.common_type(right);

    REQUIRE(common != nullptr);
    const auto &bundle = type_cast<TimeSeriesBundleType>(*common);
    REQUIRE(bundle.schema().base() != nullptr);
    CHECK(bundle.schema().identity() == SchemaIdentity{derived_identity});
    CHECK(bundle.schema().base()->identity() == SchemaIdentity{base_identity});
    CHECK(bundle.item_name(0).text() == "value");
    CHECK(bundle.item_type(0).guarantee().value() == TimeSeriesGuarantee::final);
    CHECK(bundle.item_type(1).guarantee().value() == TimeSeriesGuarantee::ordinary);
}

TEST_CASE("common bundles require matching schema and field structures",
          "[feature:type-system][series:tsb][schema:named]") {
    const auto identity = std::type_index{typeid(BundleTypeScope)};
    const auto value_field = std::array{SchemaField{util::symbol{"value"}, &TS<i64>::type()}};
    const auto other_field = std::array{SchemaField{util::symbol{"value"}, &TS<float>::type()}};
    const auto &left_schema = bg::schema(util::symbol{"Common"}, identity, nullptr, value_field);
    const auto &other_identity =
        bg::schema(util::symbol{"Common"}, std::type_index{typeid(TestField<double>)}, nullptr, value_field);
    const auto &wrong_field = bg::schema(util::symbol{"Common"}, identity, nullptr, other_field);
    const auto &left = time_series_bundle_type(left_schema, TimeSeriesGuarantee::ordinary);

    CHECK(left.common_type(time_series_bundle_type(other_identity, TimeSeriesGuarantee::ordinary)) == nullptr);
    CHECK(left.common_type(time_series_bundle_type(wrong_field, TimeSeriesGuarantee::ordinary)) == nullptr);
    CHECK(time_series_bundle_type(wrong_field, TimeSeriesGuarantee::ordinary).common_type(left) == nullptr);

    const auto &unresolved = TSB<UnresolvedFieldBundle>::type();
    const auto &resolved = TSB<BaseBundle>::type();
    CHECK(unresolved.common_type(resolved) == nullptr);
    CHECK(resolved.common_type(unresolved) == nullptr);
}

TEST_CASE("one schema can describe a hierarchical scalar and a time series bundle",
          "[feature:type-system][scalar:hierarchical][series:tsb][schema:named][declaration:concrete]") {
    const auto &scalar = cpp_scalar_type<Bundle<SharedBundle>>();
    const auto &time_series = TSB<SharedBundle>::type();

    STATIC_CHECK(details::hierarchical_scalar_schema_supported<SharedBundle>);
    STATIC_CHECK(details::supported_time_series<TSB<SharedBundle>>);
    CHECK(scalar.is_scalar());
    REQUIRE(time_series.item_count() == 2);
    CHECK(&time_series.item_type(0) == &TS<i64>::type());
    CHECK(&time_series.item_type(1) == &TS<double>::type());

    const auto &hierarchical = type_cast<HierarchicalScalarType>(scalar);
    const auto &scalar_schema = static_cast<const ResolvedHierarchicalScalarType &>(hierarchical).schema();
    const auto &time_series_schema = time_series.schema();
    CHECK(scalar_schema.identity() == time_series_schema.identity());
    CHECK(&scalar_schema != &time_series_schema);
}

TEST_CASE("explicit time series fields remain time series fields",
          "[feature:type-system][series:tsb][schema:named][declaration:concrete]") {
    STATIC_CHECK_FALSE(details::hierarchical_scalar_schema_supported<BaseBundle>);
    STATIC_CHECK(details::supported_time_series<TSB<BaseBundle>>);
    CHECK(&TSB<BaseBundle>::type().item_type(0) == &TS<i64>::type());
}

TEST_CASE("invalid shared-schema fields are rejected without instantiating a projection",
          "[feature:type-system][series:tsb][schema:named][declaration:concrete]") {
    STATIC_CHECK_FALSE(details::hierarchical_scalar_schema_supported<InvalidBundle>);
    STATIC_CHECK_FALSE(details::supported_time_series<TSB<InvalidBundle>>);
}

TEST_CASE("a time series bundle type derives an unnamed schema from resolved fields",
          "[type_system][tsb][series:tsb][feature:type-system][schema:unnamed][guarantee:final]") {
    const auto fields = std::array{
        SchemaField{util::symbol{"count"}, &TS<i64>::type()},
        SchemaField{util::symbol{"value"}, &TS<float>::type()},
        SchemaField{util::symbol{"reference"}, &REF<TS<float>>::type()},
    };

    const auto &type = TimeSeriesBundleType::from_fields(fields, TimeSeriesGuarantee::final);

    CHECK(type.item_count() == 3);
    CHECK(type.guarantee().value() == TimeSeriesGuarantee::final);
    CHECK(type.item_type(0).guarantee().value() == TimeSeriesGuarantee::final);
    CHECK(&type.item_type(2) == &REF<TS<float>, TimeSeriesGuarantee::final>::type());
}

TEST_CASE("a time series bundle type requires concrete fields",
          "[type_system][tsb][series:tsb][feature:type-system][schema:unnamed][declaration:generic]") {
    const auto null_field = std::array{SchemaField{util::symbol{"value"}, nullptr}};
    const auto unresolved_field = std::array{SchemaField{util::symbol{"value"}, &TS<SCALAR>::type()}};
    const auto duplicate_fields = std::array{
        SchemaField{util::symbol{"value"}, &TS<i64>::type()},
        SchemaField{util::symbol{"value"}, &TS<float>::type()},
    };

    CHECK_THROWS_WITH(TimeSeriesBundleType::from_fields(null_field),
                      "a time series bundle type requires resolved time-series fields");
    CHECK_THROWS_WITH(TimeSeriesBundleType::from_fields(unresolved_field),
                      "a time series bundle type requires resolved time-series fields");
    CHECK_THROWS_WITH(TimeSeriesBundleType::from_fields(duplicate_fields), "a schema has duplicate field names");
}

namespace {

template <template <typename, auto...> typename Field> struct OrdinaryBundle {
    Field<TS<i64, TimeSeriesGuarantee::ordinary>> value;
};

} // namespace

TEST_CASE("a bundle guarantee strengthens its field guarantees",
          "[type_system][tsb][series:tsb][feature:type-system][schema:named]"
          "[guarantee:ordinary][guarantee:final]") {
    const auto &type = TSB<OrdinaryBundle, TimeSeriesGuarantee::final>::type();

    CHECK(type.item_type(0).guarantee().value() == TimeSeriesGuarantee::final);
}

TEST_CASE("bundle declarations remain generic when a field guarantee is unresolved",
          "[type_system][tsb][reference][series:tsb][feature:type-system][declaration:generic]"
          "[guarantee:ordinary]") {
    const auto &bundle = TSB<UnresolvedFieldBundle>::type();
    const auto &reference = static_cast<const TimeSeriesReferenceType &>(REF<TSB<UnresolvedFieldBundle>>::type());

    CHECK_FALSE(bundle.is_resolved());
    CHECK_FALSE(reference.is_resolved());
    CHECK_FALSE(reference.target_type().is_resolved());

    const auto fields = std::array{SchemaField{util::symbol{"value"}, &TS<i64, TimeSeriesGuarantee::final>::type()}};
    const auto &schema =
        bg::schema(util::symbol{"UnresolvedFieldBundle"},
                   std::type_index{typeid(details::schema_template<UnresolvedFieldBundle>)}, nullptr, fields);
    const auto &concrete = time_series_bundle_type(schema, TimeSeriesGuarantee::ordinary);

    auto bundle_type_map = TypeMap{};
    REQUIRE(bundle.match(concrete, bundle_type_map));
    CHECK(&bundle.resolve(bundle_type_map) == &concrete);

    auto reference_type_map = TypeMap{};
    REQUIRE(reference.match(concrete, reference_type_map));
    const auto &resolved_reference =
        static_cast<const TimeSeriesReferenceType &>(reference.resolve(reference_type_map));
    CHECK(&resolved_reference.target_type() == &concrete);
}

TEST_CASE("an unnamed bundle matches a compatible named schema",
          "[type_system][tsb][series:tsb][feature:type-system][schema:unnamed]") {
    const auto &parameter = static_cast<const TimeSeriesBundleType &>(
        details::time_series_type_for<BundleTypeScope, 0, TimeSeriesTypeRole::input, TSB<BaseBundle>>(
            TimeSeriesGuarantee::ordinary));
    const auto &count = TS<i64>::type();
    const auto fields = std::array{SchemaField{util::symbol{"count"}, &count}};
    const auto &argument = time_series_bundle_type(anonymous_schema(fields), TimeSeriesGuarantee::ordinary);
    auto type_map = TypeMap{};

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

template <template <typename, auto...> typename Field> struct RedefinedBundle : bundle_extends<BaseBundle, Field> {
    Field<TS<i64>> count;
    Field<TS<float>> value;
};

} // namespace

TEST_CASE("a redefined bundle field keeps its inherited position",
          "[type_system][tsb][series:tsb][inheritance][feature:type-system][schema:named]") {
    const auto &type = TSB<RedefinedBundle>::type();

    REQUIRE(type.item_count() == 2);
    CHECK(type.item_name(0).text() == "count");
    CHECK(type.item_name(1).text() == "value");
    CHECK(details::bundle_field_index<"count", RedefinedBundle> == 0);
    CHECK(details::bundle_field_index<"value", RedefinedBundle> == 1);
}

namespace {

template <template <typename, auto...> typename Field> struct GenericBundle {
    Field<TS<SCALAR>> value;
};

} // namespace

TEST_CASE("bundle matching resolves generic field types",
          "[type_system][tsb][series:tsb][generic][declaration:generic][feature:type-system]") {
    const auto &parameter = static_cast<const TimeSeriesBundleType &>(
        details::time_series_type_for<BundleTypeScope, 0, TimeSeriesTypeRole::input, TSB<GenericBundle>>(
            TimeSeriesGuarantee::ordinary));
    const auto &value = TS<i64>::type();
    const auto fields = std::array{SchemaField{util::symbol{"value"}, &value}};
    const auto &argument = time_series_bundle_type(anonymous_schema(fields), TimeSeriesGuarantee::ordinary);
    auto type_map = TypeMap{};

    REQUIRE(parameter.match(argument, type_map));
    const auto &resolved = static_cast<const TimeSeriesBundleType &>(parameter.resolve(type_map));
    CHECK(&resolved.item_type(0) == &value);
}

TEST_CASE("a bundle schema variable participates in type resolution",
          "[type_system][tsb][series:tsb][generic][declaration:generic][feature:type-system]"
          "[schema:generic]") {
    const auto &parameter = static_cast<const TimeSeriesBundleType &>(
        details::time_series_type_for<BundleTypeScope, 0, TimeSeriesTypeRole::input, TSB<SCHEMA>>(
            TimeSeriesGuarantee::ordinary));
    const auto &argument = TSB<DerivedBundle>::type();
    auto type_map = TypeMap{};

    CHECK(parameter.item_count() == std::dynamic_extent);
    REQUIRE(parameter.match(argument, type_map));
    const auto &variable = dynamic_cast<const Variable &>(parameter.schema_type());
    const auto *mapping = type_map.find(variable);
    REQUIRE(mapping != nullptr);
    CHECK(std::get<const bg::Type *>(*mapping) == &argument.schema());
    CHECK(&parameter.resolve(type_map) == &argument);
}

TEST_CASE("repeated bundle schema variables require the same schema",
          "[type_system][tsb][series:tsb][generic][declaration:generic][feature:type-system]"
          "[schema:generic]") {
    const auto &first = details::time_series_type_for<BundleTypeScope, 0, TimeSeriesTypeRole::input, TSB<SCHEMA>>(
        TimeSeriesGuarantee::ordinary);
    const auto &second = details::time_series_type_for<BundleTypeScope, 1, TimeSeriesTypeRole::input, TSB<SCHEMA>>(
        TimeSeriesGuarantee::ordinary);
    const auto &derived = TSB<DerivedBundle>::type();
    const auto &base = TSB<BaseBundle>::type();
    auto type_map = TypeMap{};

    REQUIRE(first.match(derived, type_map));
    CHECK(second.match(derived, type_map));
    CHECK_FALSE(second.match(base, type_map));
}

namespace {

template <template <typename, auto...> typename Field> struct UnrelatedBundle {
    Field<TS<i64>> count;
};

} // namespace

TEST_CASE("named bundles with the same fields remain nominally distinct",
          "[type_system][tsb][series:tsb][feature:type-system][schema:named]") {
    const auto &parameter = static_cast<const TimeSeriesBundleType &>(
        details::time_series_type_for<BundleTypeScope, 0, TimeSeriesTypeRole::input, TSB<BaseBundle>>(
            TimeSeriesGuarantee::ordinary));
    const auto &argument = TSB<UnrelatedBundle>::type();
    auto type_map = TypeMap{};

    CHECK_FALSE(parameter.match(argument, type_map));
}

namespace {

template <template <typename, auto...> typename Field>
struct IncompatibleRedefinedBundle : bundle_extends<BaseBundle, Field> {
    Field<TS<float>> count;
};

} // namespace

TEST_CASE("a redefined bundle field must be narrower",
          "[type_system][tsb][series:tsb][inheritance][feature:type-system][schema:named]") {
    CHECK_THROWS_AS(TSB<IncompatibleRedefinedBundle>::type(), std::invalid_argument);
}
