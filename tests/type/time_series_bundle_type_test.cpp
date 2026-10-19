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

template <template <typename, auto...> typename Field> struct UnresolvedFieldBundle {
    Field<bg::TS<bg::i64, bg::guarantee_variable<"G1">>> value;
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

TEST_CASE("common bundles preserve schema identity and combine nested field guarantees",
          "[feature:type-system][series:tsb][schema:unnamed][guarantee:ordinary][guarantee:final]"
          "[guarantee:const]") {
    const auto left_fields = std::array{
        bg::SchemaField{bg::util::symbol{"value"}, &bg::TS<bg::i64, bg::TimeSeriesGuarantee::final>::type()},
        bg::SchemaField{bg::util::symbol{"items"},
                        &bg::TSL<bg::TS<float, bg::TimeSeriesGuarantee::final>, 2>::type()},
    };
    const auto right_fields = std::array{
        bg::SchemaField{bg::util::symbol{"value"}, &bg::TS<bg::i64, bg::TimeSeriesGuarantee::const_>::type()},
        bg::SchemaField{bg::util::symbol{"items"},
                        &bg::TSL<bg::TS<float, bg::TimeSeriesGuarantee::const_>, 2>::type()},
    };
    const auto &left_schema = bg::anonymous_schema(left_fields);
    const auto &right_schema = bg::anonymous_schema(right_fields);
    const auto &left = bg::time_series_bundle_type(left_schema, bg::TimeSeriesGuarantee::final);
    const auto &right = bg::time_series_bundle_type(right_schema, bg::TimeSeriesGuarantee::ordinary);
    const auto *common = left.common_type(right);

    REQUIRE(common != nullptr);
    const auto &bundle = bg::type_cast<bg::TimeSeriesBundleType>(*common);
    CHECK(bundle.guarantee().value() == bg::TimeSeriesGuarantee::ordinary);
    CHECK(bundle.schema().identity() == left.schema().identity());
    CHECK(bundle.item_name(0) == left.item_name(0));
    CHECK(bundle.item_name(1) == left.item_name(1));
    CHECK(bundle.item_type(0).guarantee().value() == bg::TimeSeriesGuarantee::final);
    const auto &items = bg::type_cast<bg::TimeSeriesListType>(bundle.item_type(1));
    CHECK(items.item_type().guarantee().value() == bg::TimeSeriesGuarantee::final);
    CHECK(right.common_type(left) == common);
}

TEST_CASE("TSB schema transformations preserve metadata and common types require matching metadata",
          "[feature:type-system][series:tsb][schema:named][guarantee:ordinary][guarantee:final]") {
    const auto metadata = bg::make_schema_metadata(
        {bg::schema_metadata_entry("extension.owner", bg::ScalarValue<bg::i64>{7})});
    const auto reference_fields = std::array{
        bg::SchemaField{bg::util::symbol{"value"}, &bg::REF<bg::TS<bg::i64>>::type()}};
    const auto &reference_schema = bg::schema(bg::util::symbol{"MetadataBundle"},
                                              std::type_index{typeid(BundleTypeScope)}, nullptr, reference_fields,
                                              metadata);
    const auto &source = bg::time_series_bundle_type(reference_schema, bg::TimeSeriesGuarantee::ordinary);

    CHECK(source.schema().direct_metadata() == metadata);

    const auto &limited = static_cast<const bg::TimeSeriesBundleType &>(
        source.with_guarantee_limit(bg::TimeSeriesGuarantee::final));
    CHECK(limited.schema().direct_metadata() == metadata);

    const auto &reference_free = static_cast<const bg::TimeSeriesBundleType &>(source.reference_free_type());
    CHECK(reference_free.schema().direct_metadata() == metadata);

    const auto fields = std::array{
        bg::SchemaField{bg::util::symbol{"value"}, &bg::TS<bg::i64>::type()}};
    const auto &left_schema = bg::anonymous_schema(fields, metadata);
    const auto &right_schema = bg::anonymous_schema(fields, metadata);
    const auto &left = bg::time_series_bundle_type(left_schema, bg::TimeSeriesGuarantee::final);
    const auto &right = bg::time_series_bundle_type(right_schema, bg::TimeSeriesGuarantee::ordinary);
    const auto *common = left.common_type(right);

    REQUIRE(common != nullptr);
    CHECK(bg::type_cast<bg::TimeSeriesBundleType>(*common).schema().direct_metadata() == metadata);

    const auto different_metadata = bg::make_schema_metadata(
        {bg::schema_metadata_entry("extension.owner", bg::ScalarValue<bg::i64>{8})});
    const auto identity = std::type_index{typeid(TestField<double>)};
    const auto &different_left_schema = bg::schema(bg::util::symbol{"MetadataBundle"}, identity, nullptr, fields,
                                                   metadata);
    const auto &different_right_schema = bg::schema(bg::util::symbol{"MetadataBundle"}, identity, nullptr, fields,
                                                    different_metadata);
    const auto &different_left =
        bg::time_series_bundle_type(different_left_schema, bg::TimeSeriesGuarantee::ordinary);
    const auto &different_right =
        bg::time_series_bundle_type(different_right_schema, bg::TimeSeriesGuarantee::ordinary);

    CHECK(different_left.common_type(different_right) == nullptr);
}

TEST_CASE("common named bundles retain their base and combine a narrowed inherited field",
          "[feature:type-system][series:tsb][schema:named][guarantee:ordinary][guarantee:final]"
          "[guarantee:const]") {
    const auto base_identity = std::type_index{typeid(BundleTypeScope)};
    const auto derived_identity = std::type_index{typeid(TestField<int>)};
    const auto base_left_fields = std::array{
        bg::SchemaField{bg::util::symbol{"value"}, &bg::TS<bg::i64, bg::TimeSeriesGuarantee::final>::type()}};
    const auto base_right_fields = std::array{
        bg::SchemaField{bg::util::symbol{"value"}, &bg::TS<bg::i64, bg::TimeSeriesGuarantee::ordinary>::type()}};
    const auto &base_left = bg::schema(bg::util::symbol{"CommonBase"}, base_identity, nullptr, base_left_fields);
    const auto &base_right = bg::schema(bg::util::symbol{"CommonBase"}, base_identity, nullptr, base_right_fields);
    const auto derived_left_fields = std::array{
        bg::SchemaField{bg::util::symbol{"value"}, &bg::TS<bg::i64, bg::TimeSeriesGuarantee::const_>::type()},
        bg::SchemaField{bg::util::symbol{"extra"}, &bg::TS<float, bg::TimeSeriesGuarantee::final>::type()},
    };
    const auto derived_right_fields = std::array{
        bg::SchemaField{bg::util::symbol{"value"}, &bg::TS<bg::i64, bg::TimeSeriesGuarantee::final>::type()},
        bg::SchemaField{bg::util::symbol{"extra"}, &bg::TS<float, bg::TimeSeriesGuarantee::ordinary>::type()},
    };
    const auto &derived_left =
        bg::schema(bg::util::symbol{"CommonDerived"}, derived_identity, &base_left, derived_left_fields);
    const auto &derived_right =
        bg::schema(bg::util::symbol{"CommonDerived"}, derived_identity, &base_right, derived_right_fields);
    const auto &left = bg::time_series_bundle_type(derived_left, bg::TimeSeriesGuarantee::final);
    const auto &right = bg::time_series_bundle_type(derived_right, bg::TimeSeriesGuarantee::ordinary);
    const auto *common = left.common_type(right);

    REQUIRE(common != nullptr);
    const auto &bundle = bg::type_cast<bg::TimeSeriesBundleType>(*common);
    REQUIRE(bundle.schema().base() != nullptr);
    CHECK(bundle.schema().identity() == bg::SchemaIdentity{derived_identity});
    CHECK(bundle.schema().base()->identity() == bg::SchemaIdentity{base_identity});
    CHECK(bundle.item_name(0).text() == "value");
    CHECK(bundle.item_type(0).guarantee().value() == bg::TimeSeriesGuarantee::final);
    CHECK(bundle.item_type(1).guarantee().value() == bg::TimeSeriesGuarantee::ordinary);
}

TEST_CASE("common bundles require matching schema and field structures",
          "[feature:type-system][series:tsb][schema:named]") {
    const auto identity = std::type_index{typeid(BundleTypeScope)};
    const auto value_field = std::array{
        bg::SchemaField{bg::util::symbol{"value"}, &bg::TS<bg::i64>::type()}};
    const auto other_field = std::array{
        bg::SchemaField{bg::util::symbol{"value"}, &bg::TS<float>::type()}};
    const auto &left_schema = bg::schema(bg::util::symbol{"Common"}, identity, nullptr, value_field);
    const auto &other_identity = bg::schema(bg::util::symbol{"Common"},
                                             std::type_index{typeid(TestField<double>)}, nullptr, value_field);
    const auto &wrong_field = bg::schema(bg::util::symbol{"Common"}, identity, nullptr, other_field);
    const auto &left = bg::time_series_bundle_type(left_schema, bg::TimeSeriesGuarantee::ordinary);

    CHECK(left.common_type(bg::time_series_bundle_type(other_identity, bg::TimeSeriesGuarantee::ordinary)) == nullptr);
    CHECK(left.common_type(bg::time_series_bundle_type(wrong_field, bg::TimeSeriesGuarantee::ordinary)) == nullptr);
    CHECK(bg::time_series_bundle_type(wrong_field, bg::TimeSeriesGuarantee::ordinary).common_type(left) == nullptr);

    const auto &unresolved = bg::TSB<UnresolvedFieldBundle>::type();
    const auto &resolved = bg::TSB<BaseBundle>::type();
    CHECK(unresolved.common_type(resolved) == nullptr);
    CHECK(resolved.common_type(unresolved) == nullptr);
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

TEST_CASE("bundle declarations remain generic when a field guarantee is unresolved",
          "[type_system][tsb][reference][series:tsb][feature:type-system][declaration:generic]"
          "[guarantee:ordinary]") {
    const auto &bundle = bg::TSB<UnresolvedFieldBundle>::type();
    const auto &reference = static_cast<const bg::TimeSeriesReferenceType &>(
        bg::REF<bg::TSB<UnresolvedFieldBundle>>::type());

    CHECK_FALSE(bundle.is_resolved());
    CHECK_FALSE(reference.is_resolved());
    CHECK_FALSE(reference.target_type().is_resolved());

    const auto fields = std::array{
        bg::SchemaField{bg::util::symbol{"value"}, &bg::TS<bg::i64, bg::TimeSeriesGuarantee::final>::type()}};
    const auto &schema = bg::schema(bg::util::symbol{"UnresolvedFieldBundle"},
                                    std::type_index{typeid(bg::details::schema_template<UnresolvedFieldBundle>)},
                                    nullptr, fields);
    const auto &concrete = bg::time_series_bundle_type(schema, bg::TimeSeriesGuarantee::ordinary);

    auto bundle_type_map = bg::TypeMap{};
    REQUIRE(bundle.match(concrete, bundle_type_map));
    CHECK(&bundle.resolve(bundle_type_map) == &concrete);

    auto reference_type_map = bg::TypeMap{};
    REQUIRE(reference.match(concrete, reference_type_map));
    const auto &resolved_reference =
        static_cast<const bg::TimeSeriesReferenceType &>(reference.resolve(reference_type_map));
    CHECK(&resolved_reference.target_type() == &concrete);
}

TEST_CASE("an unnamed bundle matches a compatible named schema",
          "[type_system][tsb][series:tsb][feature:type-system][schema:unnamed]") {
    const auto &parameter = static_cast<const bg::TimeSeriesBundleType &>(
        bg::details::time_series_type_for<BundleTypeScope, 0, bg::TimeSeriesTypeRole::input, bg::TSB<BaseBundle>>(
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
        bg::details::time_series_type_for<BundleTypeScope, 0, bg::TimeSeriesTypeRole::input, bg::TSB<GenericBundle>>(
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
        bg::details::time_series_type_for<BundleTypeScope, 0, bg::TimeSeriesTypeRole::input, bg::TSB<bg::SCHEMA>>(
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
    const auto &first = bg::details::time_series_type_for<BundleTypeScope, 0, bg::TimeSeriesTypeRole::input,
                                                         bg::TSB<bg::SCHEMA>>(
        bg::TimeSeriesGuarantee::ordinary);
    const auto &second = bg::details::time_series_type_for<BundleTypeScope, 1, bg::TimeSeriesTypeRole::input,
                                                          bg::TSB<bg::SCHEMA>>(
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
        bg::details::time_series_type_for<BundleTypeScope, 0, bg::TimeSeriesTypeRole::input, bg::TSB<BaseBundle>>(
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
