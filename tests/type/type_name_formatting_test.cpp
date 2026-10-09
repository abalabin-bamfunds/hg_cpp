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

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <string>

namespace {

class ExtensionScalarType final : public bg::ScalarType {
  public:
    VISITOR_SUPPORT()

    bool is_resolved() const noexcept override { return true; }
    const bg::Type &resolve(const bg::TypeMap &) const override { return *this; }
    void format_name(std::string &result) const override { result += "extension_scalar"; }
    bool supports_equality() const noexcept override { return true; }
    bool supports_hash() const noexcept override { return true; }
};

class FallbackScalarType final : public bg::ScalarType {
  public:
    VISITOR_SUPPORT()

    bool is_resolved() const noexcept override { return true; }
    const bg::Type &resolve(const bg::TypeMap &) const override { return *this; }
};

class FallbackType final : public bg::Type {
  public:
    VISITOR_SUPPORT()

    bool is_resolved() const noexcept override { return true; }
    const bg::Type &resolve(const bg::TypeMap &) const override { return *this; }
};

const ExtensionScalarType &extension_scalar_type() {
    static const auto type = ExtensionScalarType{};
    [[maybe_unused]] static const auto &registration = bg::TypeRegistry::register_static(type);
    return type;
}

} // namespace

TEST_CASE("semantic types append their formal names", "[feature:type-system]") {
    auto text = std::string{"prefix:"};
    const bg::Type &scalar = bg::float_type();
    scalar.format_name(text);
    CHECK(text == "prefix:float");

    auto generic_scalar_text = std::string{};
    const auto generic_scalar = FallbackScalarType{};
    generic_scalar.format_name(generic_scalar_text);
    CHECK(generic_scalar_text == "scalar");

    auto generic_type_text = std::string{};
    const auto generic_type = FallbackType{};
    generic_type.format_name(generic_type_text);
    CHECK(generic_type_text == "type");

    auto variable_text = std::string{};
    const auto variable = bg::TypeVariableType<bg::Type>{bg::util::symbol{"VALUE"}};
    const bg::Type &variable_type = variable;
    variable_type.format_name(variable_text);
    CHECK(variable_text == "VALUE");
}

TEST_CASE("built-in composite types format their arguments and guarantees", "[feature:type-system]") {
    const auto &value = bg::TS<float>::type();
    const auto &reference = bg::generic_time_series_reference_type(value, bg::TimeSeriesGuarantee::deduplicated);
    const auto &list = bg::time_series_list_type(value, 3, bg::TimeSeriesGuarantee::final);
    const auto &set = bg::time_series_set_type(bg::float_type(), bg::TimeSeriesGuarantee::ordinary);
    const auto &dictionary =
        bg::time_series_dictionary_type(bg::string_type(), value, bg::TimeSeriesGuarantee::const_);

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

    const auto fields = std::array{bg::SchemaField{bg::util::symbol{"value"}, &value, false}};
    const auto &schema = bg::schema(bg::util::symbol{"Quote"}, std::monostate{}, nullptr, fields);
    const auto &bundle = bg::time_series_bundle_type(schema, bg::TimeSeriesGuarantee::ordinary);
    auto bundle_text = std::string{};
    bundle.format_name(bundle_text);
    CHECK(bundle_text == "TSB<Quote>");

    auto type_value_text = std::string{};
    bg::type_of(value).format_name(type_value_text);
    CHECK(type_value_text == "Type<TS<float>>");

    const auto &series_variable = bg::TypeVariableType<bg::TimeSeriesType>{bg::util::symbol{"SERIES"}};
    const auto generic_time_series = bg::GenericTimeSeriesVariableType{series_variable,
                                                                        bg::TimeSeriesGuarantee::final};
    auto generic_time_series_text = std::string{};
    generic_time_series.format_name(generic_time_series_text);
    CHECK(generic_time_series_text == "SERIES, final");
}

TEST_CASE("extension types format through Type inside built-in composites",
          "[feature:type-system][declaration:runtime][scalar:generic][series:value]") {
    const auto &extension = extension_scalar_type();
    const bg::Type &extension_type = extension;
    auto extension_text = std::string{};
    extension_type.format_name(extension_text);
    CHECK(extension_text == "extension_scalar");

    const auto &list = bg::list_scalar_type(extension);
    const bg::Type &list_type = list;
    auto list_text = std::string{};
    list_type.format_name(list_text);
    CHECK(list_text == "List<extension_scalar>");

    const auto &time_series = bg::time_series_value_type(list, bg::TimeSeriesGuarantee::ordinary);
    const bg::Type &time_series_type = time_series;
    auto time_series_text = std::string{};
    time_series_type.format_name(time_series_text);
    CHECK(time_series_text == "TS<List<extension_scalar>>");
}
