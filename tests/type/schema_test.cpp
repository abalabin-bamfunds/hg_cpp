#include <bg/scalar/bundle/scalar_type.hpp>
#include <bg/type/scalar_variable.hpp>
#include <bg/time_series/tsb/tsb_type.hpp>
#include <bg/type/schema.hpp>
#include <bg/type/scalar_type.hpp>
#include <bg/type/time_series_bundle_type.hpp>
#include <bg/type/type_map.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>

#include <array>

TEST_CASE("runtime schema identities and field metadata key canonical descriptors",
          "[feature:type-system][schema:named][declaration:runtime]") {
    const auto identity = bg::new_runtime_schema_id();
    const auto other_identity = bg::new_runtime_schema_id();
    const auto fields = std::array{
        bg::SchemaField{bg::util::symbol{"bid"}, &bg::double_type()},
        bg::SchemaField{bg::util::symbol{"ask"}, &bg::double_type()},
    };
    const auto &canonical = bg::schema(bg::util::symbol{"Quote"}, identity, nullptr, fields);

    CHECK(identity != other_identity);
    CHECK(&bg::schema(bg::util::symbol{"Quote"}, identity, nullptr, fields) == &canonical);
    CHECK(&bg::schema(bg::util::symbol{"OtherQuote"}, identity, nullptr, fields) != &canonical);
    CHECK(&bg::schema(bg::util::symbol{"Quote"}, other_identity, nullptr, fields) != &canonical);

    const auto reversed_fields = std::array{fields[1], fields[0]};
    CHECK(&bg::schema(bg::util::symbol{"Quote"}, identity, nullptr, reversed_fields) != &canonical);

    const auto optional_fields = std::array{
        bg::SchemaField{bg::util::symbol{"bid"}, &bg::double_type()},
        bg::SchemaField{bg::util::symbol{"ask"}, &bg::double_type(), true},
    };
    CHECK(&bg::schema(bg::util::symbol{"Quote"}, identity, nullptr, optional_fields) != &canonical);

    const auto different_type_fields = std::array{
        bg::SchemaField{bg::util::symbol{"bid"}, &bg::float_type()},
        bg::SchemaField{bg::util::symbol{"ask"}, &bg::double_type()},
    };
    CHECK(&bg::schema(bg::util::symbol{"Quote"}, identity, nullptr, different_type_fields) != &canonical);
}

TEST_CASE("schema resolution preserves identity and resolves inherited field types",
          "[feature:type-system][schema:named][declaration:runtime]") {
    using VALUE = bg::ScalarVariable<"schema_test_value">;
    const auto &variable = VALUE::type();
    const auto base_identity = bg::new_runtime_schema_id();
    const auto derived_identity = bg::new_runtime_schema_id();
    const auto base_fields = std::array{bg::SchemaField{bg::util::symbol{"value"}, &variable}};
    const auto &base = bg::schema(bg::util::symbol{"Base"}, base_identity, nullptr, base_fields);
    const auto derived_fields = std::array{bg::SchemaField{bg::util::symbol{"extra"}, &bg::float_type()}};
    const auto &derived = bg::schema(bg::util::symbol{"Derived"}, derived_identity, &base, derived_fields);
    auto type_map = bg::TypeMap{};
    type_map.set(variable, &bg::int64_type());

    const auto &resolved = derived.resolve(type_map);
    REQUIRE(resolved.is_resolved());
    REQUIRE(resolved.base() != nullptr);
    CHECK(resolved.identity() == derived.identity());
    CHECK(resolved.base()->identity() == base.identity());
    CHECK(resolved.base()->field(0).type == &bg::int64_type());
    CHECK(&resolved == &derived.resolve(type_map));
}

TEST_CASE("time series bundle types reject common schemas with non-time-series fields",
          "[feature:type-system][series:tsb][schema:named][declaration:runtime]") {
    const auto scalar_fields = std::array{bg::SchemaField{bg::util::symbol{"value"}, &bg::int64_type()}};
    const auto &scalar_schema = bg::schema(bg::util::symbol{"ScalarField"}, bg::new_runtime_schema_id(), nullptr,
                                           scalar_fields);
    CHECK_THROWS_WITH(bg::time_series_bundle_type(scalar_schema, bg::TimeSeriesGuarantee::ordinary),
                      "a time series bundle requires resolved, present time-series fields");

    const auto optional_fields = std::array{
        bg::SchemaField{bg::util::symbol{"value"}, &bg::TS<bg::i64>::type(), true},
    };
    const auto &optional_schema = bg::schema(bg::util::symbol{"OptionalField"}, bg::new_runtime_schema_id(), nullptr,
                                             optional_fields);
    CHECK_THROWS_WITH(bg::time_series_bundle_type(optional_schema, bg::TimeSeriesGuarantee::ordinary),
                      "a time series bundle requires resolved, present time-series fields");
}
