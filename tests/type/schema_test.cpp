#include <bg/scalar/bundle/scalar_type.hpp>
#include <bg/scalar/data.hpp>
#include <bg/scalar/details/data.hpp>
#include <bg/scalar/list/scalar_type.hpp>
#include <bg/scalar/list/type.hpp>
#include <bg/scalar/opaque.hpp>
#include <bg/scalar/value.hpp>
#include <bg/type/scalar_variable.hpp>
#include <bg/time_series/tsb/tsb_type.hpp>
#include <bg/type/schema.hpp>
#include <bg/type/schema_metadata_values.hpp>
#include <bg/type/scalar_type.hpp>
#include <bg/type/time_series_bundle_type.hpp>
#include <bg/type/type_map.hpp>
#include <bg/type/type_registry.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>

#include <array>
#include <cstddef>
#include <cstdlib>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

bg::SchemaMetadata shutdown_schema_metadata() {
    const auto labels = bg::ScalarValue<bg::List<bg::string>>{
        std::vector<std::string_view>{"shutdown", "probe"}};
    return bg::make_schema_metadata({bg::schema_metadata_entry("extension.labels", labels)});
}

struct ShutdownProbe {
    std::optional<bg::RuntimeSchemaId> identity;
    const bg::Schema *canonical_schema{};

    ~ShutdownProbe() noexcept {
        if (!identity || canonical_schema == nullptr) {
            return;
        }

        const auto &list_type = bg::cpp_scalar_type<bg::List<bg::string>>();
        auto list_name = std::string{};
        list_type.format_name(list_name);
        if (list_name != "List<string>" || &list_type != &bg::list_scalar_type(bg::string_type())) {
            std::abort();
        }
        if (!bg::TypeRegistry::instance().contains(list_type)) {
            std::abort();
        }

        const auto metadata = shutdown_schema_metadata();
        const auto fields = std::array{bg::SchemaField{bg::util::symbol{"items"}, &list_type}};
        const auto &descriptor = bg::schema(bg::util::symbol{"ShutdownSchema"}, *identity, nullptr, fields, metadata);
        if (&descriptor != canonical_schema || descriptor.field_count() != 1 ||
            descriptor.field(0).type != &list_type || descriptor.metadata() != metadata ||
            !bg::TypeRegistry::instance().contains(descriptor)) {
            std::abort();
        }

        const auto *stored_labels = descriptor.metadata().find("extension.labels");
        if (stored_labels == nullptr || &stored_labels->runtime_type().type() != &list_type) {
            std::abort();
        }
        const auto labels =
            bg::Scalar<bg::List<bg::string>>::from(stored_labels->runtime_type(), stored_labels->data());
        if (&labels.runtime_type() != &bg::cpp_scalar_runtime_type<bg::List<bg::string>>() || labels.size() != 2 ||
            labels[0].value() != "shutdown" || labels[1].value() != "probe") {
            std::abort();
        }

        const auto first_item = labels[0];
        const auto &data = bg::details::scalar_data(first_item);
        alignas(std::string_view) auto storage = std::array<std::byte, sizeof(std::string_view)>{};
        data.runtime_type().read_value(data.mode(), data.data_word(), storage.data());
        auto *text = std::launder(reinterpret_cast<std::string_view *>(storage.data()));
        if (*text != "shutdown") {
            std::abort();
        }
        std::destroy_at(text);
    }
};

ShutdownProbe shutdown_probe;

} // namespace

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

TEST_CASE("schema metadata owns scalar values and ignores entry order in canonical identity",
          "[feature:type-system][schema:named][declaration:runtime]") {
    const auto identity = bg::new_runtime_schema_id();
    const auto fields = std::array{bg::SchemaField{bg::util::symbol{"value"}, &bg::int64_type()}};
    const auto metadata = [] {
        return bg::make_schema_metadata({
            bg::schema_metadata_entry("extension.version", bg::ScalarValue<bg::i64>{7}),
            bg::schema_metadata_entry("extension.enabled", bg::ScalarValue<bool>{true}),
        });
    }();
    const auto reordered = bg::make_schema_metadata({
        bg::schema_metadata_entry("extension.enabled", bg::ScalarValue<bool>{true}),
        bg::schema_metadata_entry("extension.version", bg::ScalarValue<bg::i64>{7}),
    });
    const auto &first = bg::schema(bg::util::symbol{"MetadataSchema"}, identity, nullptr, fields, metadata);
    const auto &same = bg::schema(bg::util::symbol{"MetadataSchema"}, identity, nullptr, fields, reordered);

    CHECK(metadata == reordered);
    CHECK(&first == &same);
    REQUIRE(first.metadata().find("extension.version") != nullptr);
    CHECK(bg::Scalar<>::from(*first.metadata().find("extension.version"))
              .equals(bg::ScalarValue<bg::i64>{7}));
    CHECK(first.metadata().find("missing") == nullptr);

    const auto different = bg::make_schema_metadata({
        bg::schema_metadata_entry("extension.version", bg::ScalarValue<bg::i64>{8}),
        bg::schema_metadata_entry("extension.enabled", bg::ScalarValue<bool>{true}),
    });
    CHECK(&bg::schema(bg::util::symbol{"MetadataSchema"}, identity, nullptr, fields, different) != &first);
}

TEST_CASE("registered schema and List<string> descriptors survive process shutdown",
          "[feature:type-system][schema:named][declaration:runtime]") {
    const auto identity = bg::new_runtime_schema_id();
    const auto fields = std::array{
        bg::SchemaField{bg::util::symbol{"items"}, &bg::cpp_scalar_type<bg::List<bg::string>>()},
    };
    const auto metadata = shutdown_schema_metadata();
    const auto &registered =
        bg::schema(bg::util::symbol{"ShutdownSchema"}, identity, nullptr, fields, metadata);

    REQUIRE(registered.field_count() == 1);
    CHECK(registered.metadata() == metadata);
    shutdown_probe.identity = identity;
    shutdown_probe.canonical_schema = &registered;
}

TEST_CASE("derived schema metadata inherits base entries and replaces matching names",
          "[feature:type-system][schema:named][declaration:runtime]") {
    const auto base_metadata = bg::make_schema_metadata({
        bg::schema_metadata_entry("extension.owner", bg::ScalarValue<bg::i64>{1}),
        bg::schema_metadata_entry("extension.version", bg::ScalarValue<bg::i64>{1}),
    });
    const auto derived_metadata = bg::make_schema_metadata({
        bg::schema_metadata_entry("extension.version", bg::ScalarValue<bg::i64>{2}),
        bg::schema_metadata_entry("extension.kind", bg::ScalarValue<bg::i64>{3}),
    });
    const auto base_fields = std::array{bg::SchemaField{bg::util::symbol{"value"}, &bg::int64_type()}};
    const auto &base = bg::schema(bg::util::symbol{"MetadataBase"}, bg::new_runtime_schema_id(), nullptr,
                                  base_fields, base_metadata);
    const auto derived_fields = std::array{bg::SchemaField{bg::util::symbol{"extra"}, &bg::double_type()}};
    const auto &derived = bg::schema(bg::util::symbol{"MetadataDerived"}, bg::new_runtime_schema_id(), &base,
                                     derived_fields, derived_metadata);

    REQUIRE(derived.direct_metadata().size() == 2);
    REQUIRE(derived.metadata().size() == 3);
    CHECK(bg::Scalar<>::from(*derived.metadata().find("extension.owner"))
              .equals(bg::ScalarValue<bg::i64>{1}));
    CHECK(bg::Scalar<>::from(*derived.metadata().find("extension.version"))
              .equals(bg::ScalarValue<bg::i64>{2}));
    CHECK(bg::Scalar<>::from(*derived.metadata().find("extension.kind"))
              .equals(bg::ScalarValue<bg::i64>{3}));
}

TEST_CASE("schema metadata rejects values without stable equality",
          "[feature:type-system][schema:named][declaration:runtime]") {
    CHECK_THROWS_WITH(bg::make_schema_metadata({
                          bg::schema_metadata_entry("extension.value", bg::ScalarValue<bg::Opaque<int>>{7})}),
                      "schema metadata values must support equality");
    CHECK_THROWS_WITH(bg::make_schema_metadata({bg::schema_metadata_entry(
                          "extension.value", bg::ScalarValue<double>{std::numeric_limits<double>::quiet_NaN()})}),
                      "schema metadata values must compare equal to themselves");
}

TEST_CASE("schema metadata rejects duplicate names and empty values",
          "[feature:type-system][schema:named][declaration:runtime]") {
    CHECK_THROWS_WITH(bg::make_schema_metadata({
                          bg::schema_metadata_entry("extension.value", bg::ScalarValue<bg::i64>{1}),
                          bg::schema_metadata_entry("extension.value", bg::ScalarValue<bg::i64>{2}),
                      }),
                      "schema metadata has duplicate names");

    CHECK_THROWS_WITH(bg::make_schema_metadata(
                          {bg::SchemaMetadataEntry{"extension.value", bg::ScalarData{}}}),
                      "schema metadata values must be present scalar values");
}

TEST_CASE("schema resolution preserves identity and resolves inherited field types",
          "[feature:type-system][schema:named][declaration:runtime]") {
    using VALUE = bg::ScalarVariable<"schema_test_value">;
    const auto &variable = VALUE::type();
    const auto base_identity = bg::new_runtime_schema_id();
    const auto derived_identity = bg::new_runtime_schema_id();
    const auto base_metadata = bg::make_schema_metadata(
        {bg::schema_metadata_entry("extension.base", bg::ScalarValue<bg::i64>{1})});
    const auto derived_metadata = bg::make_schema_metadata(
        {bg::schema_metadata_entry("extension.derived", bg::ScalarValue<bg::i64>{2})});
    const auto base_fields = std::array{bg::SchemaField{bg::util::symbol{"value"}, &variable}};
    const auto &base = bg::schema(bg::util::symbol{"Base"}, base_identity, nullptr, base_fields, base_metadata);
    const auto derived_fields = std::array{bg::SchemaField{bg::util::symbol{"extra"}, &bg::float_type()}};
    const auto &derived =
        bg::schema(bg::util::symbol{"Derived"}, derived_identity, &base, derived_fields, derived_metadata);
    auto type_map = bg::TypeMap{};
    type_map.set(variable, &bg::int64_type());

    const auto &resolved = derived.resolve(type_map);
    REQUIRE(resolved.is_resolved());
    REQUIRE(resolved.base() != nullptr);
    CHECK(resolved.identity() == derived.identity());
    CHECK(resolved.base()->identity() == base.identity());
    CHECK(resolved.base()->field(0).type == &bg::int64_type());
    CHECK(resolved.direct_metadata() == derived_metadata);
    CHECK(resolved.metadata() == derived.metadata());
    CHECK(bg::Scalar<>::from(*resolved.metadata().find("extension.base"))
              .equals(bg::ScalarValue<bg::i64>{1}));
    CHECK(&resolved == &derived.resolve(type_map));
}

TEST_CASE("schema registration rolls back and retries the same runtime identity",
          "[feature:type-system][schema:named][declaration:runtime]") {
    const auto identity = bg::new_runtime_schema_id();
    const auto name = bg::util::symbol{"RetrySchema"};
    const auto metadata = bg::make_schema_metadata(
        {bg::schema_metadata_entry("extension.version", bg::ScalarValue<bg::i64>{1})});
    const auto failed_fields = std::array{bg::SchemaField{bg::util::symbol{"value"}, &bg::float_type()}};
    const auto fields = std::array{bg::SchemaField{bg::util::symbol{"value"}, &bg::double_type()}};

    CHECK_THROWS([&] {
        auto registration = bg::SchemaRegistration{};
        auto &reserved = registration.reserve_schema(name, identity, nullptr, metadata);
        reserved.finalize(failed_fields);
        throw std::runtime_error{"failed schema registration"};
    }());

    auto registration = bg::SchemaRegistration{};
    auto &retried = registration.reserve_schema(name, identity, nullptr, metadata);
    retried.finalize(fields);
    registration.commit();

    CHECK(&bg::schema(name, identity, nullptr, fields, metadata) == &retried);
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
