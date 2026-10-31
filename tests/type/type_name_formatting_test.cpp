#include <bg/graph/graph_argument.hpp>
#include <bg/scalar/dict/scalar_type.hpp>
#include <bg/scalar/list/scalar_type.hpp>
#include <bg/scalar/set/scalar_type.hpp>
#include <bg/time_series/time_series_type.hpp>
#include <bg/type/schema.hpp>
#include <bg/type/time_series_bundle_type.hpp>
#include <bg/type/time_series_dict_type.hpp>
#include <bg/type/time_series_list_type.hpp>
#include <bg/type/time_series_reference_type.hpp>
#include <bg/type/time_series_set_type.hpp>
#include <bg/type/time_series_value_type.hpp>
#include <bg/type/time_series_variable.hpp>
#include <bg/type/type_of.hpp>
#include <bg/type/type_registry.hpp>
#include <bg/type/type_variable.hpp>
#include <bg/util/static_storage.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <string>

using namespace bg;

namespace {

class FallbackScalarType final : public ScalarType {
  public:
    VISITOR_SUPPORT()

    bool is_resolved() const noexcept override { return true; }
    const bg::Type &resolve(const TypeMap &) const override { return *this; }
};

class FallbackType final : public bg::Type {
  public:
    VISITOR_SUPPORT()

    bool is_resolved() const noexcept override { return true; }
    const bg::Type &resolve(const TypeMap &) const override { return *this; }
};

} // namespace

TEST_CASE("semantic types append their formal names", "[feature:type-system]") {
    auto text = std::string{"prefix:"};
    const bg::Type &scalar = float_type();
    scalar.format_name(text);
    CHECK(text == "prefix:float");

    auto generic_scalar_text = std::string{};
    static const auto generic_scalar = util::static_storage<FallbackScalarType>{};
    [[maybe_unused]] static const auto &generic_scalar_registration =
        TypeRegistry::register_static(generic_scalar.get());
    generic_scalar.get().format_name(generic_scalar_text);
    CHECK(generic_scalar_text == "scalar");

    auto generic_type_text = std::string{};
    static const auto generic_type = util::static_storage<FallbackType>{};
    [[maybe_unused]] static const auto &generic_type_registration = TypeRegistry::register_static(generic_type.get());
    generic_type.get().format_name(generic_type_text);
    CHECK(generic_type_text == "type");

    auto variable_text = std::string{};
    static const auto variable = util::static_storage<TypeVariableType<bg::Type>>{util::symbol{"VALUE"}};
    [[maybe_unused]] static const auto &variable_registration = TypeRegistry::register_static(variable.get());
    const bg::Type &variable_type = variable.get();
    variable_type.format_name(variable_text);
    CHECK(variable_text == "VALUE");
}

TEST_CASE("built-in composite types format their arguments and guarantees", "[feature:type-system]") {
    const auto &value = TS<float>::type();
    const auto &reference = generic_time_series_reference_type(value, TimeSeriesGuarantee::deduplicated);
    const auto &list = time_series_list_type(value, 3, TimeSeriesGuarantee::final);
    const auto &set = time_series_set_type(float_type(), TimeSeriesGuarantee::ordinary);
    const auto &dictionary = time_series_dictionary_type(string_type(), value, TimeSeriesGuarantee::const_);

    auto reference_text = std::string{};
    reference.format_name(reference_text);
    CHECK(reference_text == "REF<TS<float>, deduplicated>");

    auto list_text = std::string{};
    list.format_name(list_text);
    CHECK(list_text == "TSL<TS<float, final>, 3, final>");

    auto set_text = std::string{};
    set.format_name(set_text);
    CHECK(set_text == "TSS<float>");

    auto dictionary_text = std::string{};
    dictionary.format_name(dictionary_text);
    CHECK(dictionary_text == "TSD<string, TS<float, const>, const>");

    const auto fields = std::array{SchemaField{util::symbol{"value"}, &value, false}};
    const auto &schema = bg::schema(util::symbol{"Quote"}, std::monostate{}, nullptr, fields);
    const auto &bundle = time_series_bundle_type(schema, TimeSeriesGuarantee::ordinary);
    auto bundle_text = std::string{};
    bundle.format_name(bundle_text);
    CHECK(bundle_text == "TSB<Quote>");

    auto type_value_text = std::string{};
    type_of(value).format_name(type_value_text);
    CHECK(type_value_text == "Type<TS<float>>");

    static const auto series_variable = util::static_storage<TypeVariableType<TimeSeriesType>>{util::symbol{"SERIES"}};
    [[maybe_unused]] static const auto &series_variable_registration =
        TypeRegistry::register_static(series_variable.get());
    static const auto generic_time_series =
        util::static_storage<GenericTimeSeriesVariableType>{series_variable.get(), TimeSeriesGuarantee::final};
    [[maybe_unused]] static const auto &generic_time_series_registration =
        TypeRegistry::register_static(generic_time_series.get());
    auto generic_time_series_text = std::string{};
    generic_time_series.get().format_name(generic_time_series_text);
    CHECK(generic_time_series_text == "SERIES, final");
}

namespace {

class ExtensionScalarType final : public ScalarType {
  public:
    VISITOR_SUPPORT()

    bool is_resolved() const noexcept override { return true; }
    const bg::Type &resolve(const TypeMap &) const override { return *this; }
    void format_name(std::string &result) const override { result += "extension_scalar"; }
    bool supports_equality() const noexcept override { return true; }
    bool supports_hash() const noexcept override { return true; }
};

const ExtensionScalarType &extension_scalar_type() {
    static const auto type = util::static_storage<ExtensionScalarType>{};
    [[maybe_unused]] static const auto &registration = TypeRegistry::register_static(type.get());
    return type.get();
}

} // namespace

TEST_CASE("extension types format through Type inside built-in composites",
          "[feature:type-system][declaration:runtime][scalar:generic][series:value]") {
    const auto &extension = extension_scalar_type();
    const bg::Type &extension_type = extension;
    auto extension_text = std::string{};
    extension_type.format_name(extension_text);
    CHECK(extension_text == "extension_scalar");

    const auto &list = list_scalar_type(extension);
    const bg::Type &list_type = list;
    auto list_text = std::string{};
    list_type.format_name(list_text);
    CHECK(list_text == "List<extension_scalar>");

    const auto &time_series = time_series_value_type(list, TimeSeriesGuarantee::ordinary);
    const bg::Type &time_series_type = time_series;
    auto time_series_text = std::string{};
    time_series_type.format_name(time_series_text);
    CHECK(time_series_text == "TS<List<extension_scalar>>");
}
