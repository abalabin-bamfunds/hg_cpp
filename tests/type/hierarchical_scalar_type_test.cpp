#include "test_hierarchical_scalar_provider.hpp"
#include "test_shared_schema_provider.hpp"

#include <bg/node/input_properties.hpp>
#include <bg/scalar/bundle/scalar_type.hpp>
#include <bg/scalar/bundle/value.hpp>
#include <bg/scalar/dict/scalar_type.hpp>
#include <bg/scalar/list/scalar.hpp>
#include <bg/scalar/list/scalar_type.hpp>
#include <bg/scalar/opaque.hpp>
#include <bg/scalar/set/scalar_type.hpp>
#include <bg/schema/reflection.hpp>
#include <bg/type/opaque_scalar_type.hpp>
#include <bg/type/schema_metadata_values.hpp>
#include <bg/util/named_argument.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>

#include <array>
#include <barrier>
#include <concepts>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

using test_hierarchical_scalar_provider::Instrument;

static_assert(!std::same_as<bg::InputValidity, bg::Optionality>);
static_assert(sizeof(bg::InputValidity) == sizeof(std::uint8_t));
static_assert(std::is_trivially_copyable_v<bg::InputValidity>);
static_assert(bg::required == bg::InputValidity::required);
static_assert(bg::optional == bg::InputValidity::optional);
static_assert(bg::InputValidity::required == bg::required);
static_assert(bg::InputValidity::optional == bg::optional);
static_assert(bg::all_valid == bg::InputValidity::all_valid);

template <template <typename, auto...> typename Field>
struct Equity : bg::schema_extends<Instrument, Field> {
    Field<bg::i64> lot_size;
};

template <template <typename, auto...> typename Field>
struct RequiredPrice : bg::schema_extends<Instrument, Field> {
    Field<double> price;
};

template <template <typename, auto...> typename Field> struct UnrelatedInstrument {
    Field<bg::symbol> symbol;
    Field<double, bg::optional> price;
};

template <template <typename, auto...> typename Field>
using ENTITY = bg::BundleSchemaVariableDeclaration<"ENTITY", Field>;

template <template <typename, auto...> typename Field> struct NotYetRegistered {
    Field<bg::i64> value;
};

template <template <typename, auto...> typename Field> struct MetadataBase {
    Field<bg::i64> value;

    static bg::SchemaMetadata metadata() {
        return bg::make_schema_metadata({
            bg::schema_metadata_entry("extension.base", bg::ScalarValue<bg::i64>{5}),
            bg::schema_metadata_entry("extension.version", bg::ScalarValue<bg::i64>{1}),
        });
    }
};

template <template <typename, auto...> typename Field>
struct MetadataDerived : bg::schema_extends<MetadataBase, Field> {
    Field<bg::i64> extra;

    static bg::SchemaMetadata metadata() {
        return bg::make_schema_metadata(
            {bg::schema_metadata_entry("extension.version", bg::ScalarValue<bg::i64>{2})});
    }
};

template <template <typename, auto...> typename Field>
struct MetadataInheritingDerived : bg::schema_extends<MetadataBase, Field> {
    Field<bg::i64> extra;
};

const bg::ResolvedHierarchicalScalarType &resolved_hierarchical_scalar_type(const bg::Type &type) {
    const auto &hierarchical = bg::type_cast<bg::HierarchicalScalarType>(type);
    if (!hierarchical.is_resolved()) {
        throw std::logic_error{"expected a resolved hierarchical scalar type"};
    }
    return static_cast<const bg::ResolvedHierarchicalScalarType &>(hierarchical);
}

} // namespace

TEST_CASE("a hierarchical scalar has canonical ordered schema metadata",
          "[feature:type-system][scalar:hierarchical][schema:named][declaration:concrete]") {
    const auto &type = bg::cpp_scalar_type<bg::Bundle<Equity>>();
    const auto &same = bg::cpp_scalar_type<bg::Bundle<Equity>>();
    const auto &hierarchical = resolved_hierarchical_scalar_type(type);
    auto text = std::string{};
    type.format_name(text);

    CHECK(&type == &same);
    CHECK(type.is_scalar());
    CHECK(type.is_resolved());
    CHECK(hierarchical.name().text() == "Equity");
    REQUIRE(hierarchical.base() != nullptr);
    CHECK(hierarchical.base()->name().text() == "Instrument");
    REQUIRE(hierarchical.direct_fields().size() == 1);
    CHECK(hierarchical.direct_fields()[0].name.text() == "lot_size");
    REQUIRE(hierarchical.field_count() == 3);
    CHECK(hierarchical.field(0).name.text() == "symbol");
    CHECK(hierarchical.field(1).name.text() == "price");
    CHECK(hierarchical.field(1).optional);
    CHECK(hierarchical.field(2).name.text() == "lot_size");
    CHECK_FALSE(hierarchical.field(2).optional);
    CHECK(hierarchical.find(bg::util::symbol{"missing"}) == nullptr);
    CHECK(hierarchical.supports_equality());
    CHECK(hierarchical.supports_hash());
    CHECK(text == "Equity");
}

TEST_CASE("a hierarchical scalar has one semantic identity across translation units",
          "[feature:type-system][scalar:hierarchical][schema:named][declaration:concrete]") {
    CHECK(&bg::cpp_scalar_type<bg::Bundle<Instrument>>() ==
          &test_hierarchical_scalar_provider::instrument_type());
}

TEST_CASE("C++ schema metadata is registered for scalar and TSB projections",
          "[feature:type-system][schema:named][declaration:concrete]") {
    const auto &scalar_schema = bg::details::cpp_hierarchical_scalar_schema<MetadataDerived>();
    const auto &series_schema = bg::TSB<MetadataDerived>::type().schema();

    REQUIRE(scalar_schema.base() != nullptr);
    REQUIRE(scalar_schema.metadata().size() == 2);
    REQUIRE(series_schema.metadata().size() == 2);
    CHECK(scalar_schema.metadata() == series_schema.metadata());
    CHECK(bg::Scalar<>::from(*series_schema.metadata().find("extension.base"))
              .equals(bg::ScalarValue<bg::i64>{5}));
    CHECK(bg::Scalar<>::from(*series_schema.metadata().find("extension.version"))
              .equals(bg::ScalarValue<bg::i64>{2}));
    CHECK(bg::Scalar<>::from(*scalar_schema.metadata().find("extension.base"))
              .equals(bg::ScalarValue<bg::i64>{5}));
    CHECK(bg::Scalar<>::from(*scalar_schema.metadata().find("extension.version"))
              .equals(bg::ScalarValue<bg::i64>{2}));
    CHECK(bg::Scalar<>::from(*scalar_schema.direct_metadata().find("extension.version"))
              .equals(bg::ScalarValue<bg::i64>{2}));
    CHECK(&bg::schema_type<MetadataDerived>::type() == &series_schema);
}

TEST_CASE("C++ schemas without a metadata hook inherit only their base metadata",
          "[feature:type-system][schema:named][declaration:concrete]") {
    const auto &schema = bg::details::cpp_hierarchical_scalar_schema<MetadataInheritingDerived>();

    REQUIRE(schema.base() != nullptr);
    CHECK(schema.direct_metadata().empty());
    REQUIRE(schema.metadata().size() == 2);
    CHECK(bg::Scalar<>::from(*schema.metadata().find("extension.base"))
              .equals(bg::ScalarValue<bg::i64>{5}));
    CHECK(bg::Scalar<>::from(*schema.metadata().find("extension.version"))
              .equals(bg::ScalarValue<bg::i64>{1}));
}

TEST_CASE("a C++ schema needs its scalar bundle registered before runtime lookup",
          "[feature:type-system][scalar:hierarchical][schema:named][declaration:concrete]") {
    const auto &schema = bg::details::cpp_hierarchical_scalar_schema<NotYetRegistered>();
    const auto lookup = [&] { return &bg::hierarchical_scalar_type(schema); };

    CHECK_THROWS_WITH(lookup(), "a C++-identified schema requires prior hierarchical scalar registration");
    CHECK_THROWS_WITH(lookup(), "a C++-identified schema requires prior hierarchical scalar registration");

    const auto &registered = bg::cpp_scalar_type<bg::Bundle<NotYetRegistered>>();
    const auto &resolved = bg::hierarchical_scalar_type(schema);
    CHECK(&resolved == &bg::type_cast<bg::HierarchicalScalarType>(registered));
}

TEST_CASE("runtime schemas have canonical hierarchical scalar types",
          "[feature:type-system][scalar:hierarchical][schema:named][declaration:runtime]") {
    const auto runtime_fields = std::array{bg::SchemaField{bg::util::symbol{"value"}, &bg::int64_type()}};
    const auto &runtime_schema = bg::schema(bg::util::symbol{"RuntimeRecord"}, bg::new_runtime_schema_id(), nullptr,
                                            runtime_fields);
    const auto &runtime_type = bg::hierarchical_scalar_type(runtime_schema);
    CHECK(&bg::hierarchical_scalar_type(runtime_schema) == &runtime_type);
    CHECK(runtime_type.accepts(runtime_type));

    const auto same_shape_fields = std::array{bg::SchemaField{bg::util::symbol{"value"}, &bg::int64_type()}};
    const auto &same_shape = bg::hierarchical_scalar_type(bg::schema(
        bg::util::symbol{"OtherRuntimeRecord"}, bg::new_runtime_schema_id(), nullptr, same_shape_fields));
    CHECK_FALSE(runtime_type.accepts(same_shape));
}

TEST_CASE("anonymous schemas have canonical hierarchical scalar types",
          "[feature:type-system][scalar:hierarchical][schema:unnamed][declaration:runtime]") {
    const auto fields = std::array{bg::SchemaField{bg::util::symbol{"label"}, &bg::string_type()}};
    const auto &anonymous = bg::anonymous_schema(fields);
    const auto &type = bg::hierarchical_scalar_type(anonymous);

    CHECK(&bg::hierarchical_scalar_type(anonymous) == &type);
    CHECK(type.accepts(type));
}

TEST_CASE("runtime hierarchical schema compatibility follows nominal inheritance",
          "[feature:type-system][scalar:hierarchical][schema:named][declaration:runtime][binding:covariant]") {
    const auto base_fields = std::array{bg::SchemaField{bg::util::symbol{"symbol"}, &bg::symbol_type()}};
    const auto &base_schema =
        bg::schema(bg::util::symbol{"RuntimeInstrument"}, bg::new_runtime_schema_id(), nullptr, base_fields);
    const auto derived_fields = std::array{bg::SchemaField{bg::util::symbol{"lot_size"}, &bg::int64_type()}};
    const auto &derived_schema = bg::schema(bg::util::symbol{"RuntimeEquity"}, bg::new_runtime_schema_id(),
                                            &base_schema, derived_fields);

    const auto &base_type = bg::hierarchical_scalar_type(base_schema);
    const auto &derived_type = bg::hierarchical_scalar_type(derived_schema);

    CHECK(base_type.accepts(derived_type));
    CHECK_FALSE(derived_type.accepts(base_type));
    CHECK(derived_type.field_count() == 2);
}

TEST_CASE("shared schemas have one scalar and TSB projection across shared libraries",
          "[feature:type-system][schema:named][declaration:concrete]") {
    CHECK(&test_shared_schema_provider::scalar_type() ==
          &bg::cpp_scalar_type<bg::Bundle<test_shared_schema_provider::SharedSchema>>());
    CHECK(&test_shared_schema_provider::time_series_type() ==
          &bg::TSB<test_shared_schema_provider::SharedSchema>::type());
}

TEST_CASE("a conflicting hierarchical scalar declaration in another translation unit is rejected",
          "[feature:type-system][scalar:hierarchical][schema:named][declaration:concrete]") {
    CHECK_THROWS_WITH(test_hierarchical_scalar_provider::conflicting_instrument_type(),
                      "conflicting hierarchical scalar declaration: Instrument");
    CHECK(&test_hierarchical_scalar_provider::instrument_type() ==
          &bg::cpp_scalar_type<bg::Bundle<Instrument>>());
}

TEST_CASE("hierarchical scalar compatibility follows declared inheritance",
          "[feature:type-system][scalar:hierarchical][schema:named][declaration:concrete]"
          "[binding:covariant]") {
    const auto &base = bg::cpp_scalar_type<bg::Bundle<Instrument>>();
    const auto &derived = bg::cpp_scalar_type<bg::Bundle<Equity>>();
    const auto &unrelated = bg::cpp_scalar_type<bg::Bundle<UnrelatedInstrument>>();

    CHECK(base.accepts(derived));
    CHECK(derived.accepts(derived));
    CHECK_FALSE(derived.accepts(base));
    CHECK_FALSE(base.accepts(unrelated));
}

TEST_CASE("HIERARCHICAL resolves to a concrete hierarchical scalar",
          "[feature:type-system][scalar:hierarchical][declaration:generic]") {
    const auto &concrete = bg::cpp_scalar_type<bg::Bundle<Equity>>();
    const auto &variable = bg::cpp_scalar_type<bg::HIERARCHICAL>();
    auto type_map = bg::TypeMap{};

    REQUIRE(variable.match(concrete, type_map));
    CHECK(&variable.resolve(type_map) == &concrete);

    auto rejected_type_map = bg::TypeMap{};
    CHECK_FALSE(variable.match(bg::int64_type(), rejected_type_map));
}

TEST_CASE("a generic bundle type keeps its wrapper in type text",
          "[feature:type-system][scalar:hierarchical][declaration:generic]") {
    const auto &type = bg::cpp_scalar_type<bg::Bundle<bg::SCHEMA>>();
    auto text = std::string{};

    type.format_name(text);

    CHECK(text == "Bundle<SCHEMA>");
}

TEST_CASE("a generic bundle declaration captures and resolves its schema variable",
          "[feature:type-system][scalar:hierarchical][declaration:generic]") {
    const auto &generic = bg::cpp_scalar_type<bg::Bundle<bg::SCHEMA>>();
    const auto &concrete = bg::cpp_scalar_type<bg::Bundle<Equity>>();
    const auto &schema_variable = bg::details::schema_variable_instance<bg::SCHEMA>();
    auto type_map = bg::TypeMap{};

    REQUIRE(generic.match(concrete, type_map));
    CHECK(std::get<const bg::Type *>(*type_map.find(schema_variable)) ==
          &bg::type_cast<bg::HierarchicalScalarType>(concrete).schema_type());
    CHECK(&generic.resolve(type_map) == &bg::type_cast<bg::HierarchicalScalarType>(concrete));

    const auto &unrelated = bg::cpp_scalar_type<bg::Bundle<UnrelatedInstrument>>();
    CHECK_FALSE(generic.match(unrelated, type_map));
    CHECK(std::get<const bg::Type *>(*type_map.find(schema_variable)) ==
          &bg::type_cast<bg::HierarchicalScalarType>(concrete).schema_type());
}

TEST_CASE("a generic bundle declaration captures a named schema variable",
          "[feature:type-system][scalar:hierarchical][schema:named][declaration:generic]") {
    const auto &generic = bg::cpp_scalar_type<bg::Bundle<ENTITY>>();
    const auto &concrete = bg::cpp_scalar_type<bg::Bundle<Equity>>();
    const auto &schema_variable = bg::details::schema_variable_instance<ENTITY>();
    auto type_map = bg::TypeMap{};

    REQUIRE(generic.match(concrete, type_map));
    CHECK(schema_variable.name().text() == "ENTITY");
    CHECK(std::get<const bg::Type *>(*type_map.find(schema_variable)) ==
          &bg::type_cast<bg::HierarchicalScalarType>(concrete).schema_type());
}

TEST_CASE("a generic bundle declaration enumerates its schema variable",
          "[feature:type-system][scalar:hierarchical][declaration:generic]") {
    auto visited = false;
    bg::for_each_declaration_variable<bg::Bundle<bg::SCHEMA>>(
        [&]<typename Declaration>(const bg::Variable &variable) {
            STATIC_CHECK(std::same_as<Declaration, bg::schema_type<bg::SCHEMA>>);
            CHECK(variable.name().text() == "SCHEMA");
            visited = true;
        });

    CHECK(visited);
}

TEST_CASE("hierarchical scalar covariance composes through scalar containers",
          "[feature:type-system][scalar:hierarchical][scalar:list][scalar:dict]"
          "[schema:named][declaration:concrete][binding:covariant]") {
    using Base = bg::Bundle<Instrument>;
    using Derived = bg::Bundle<Equity>;

    const auto &base_nested = bg::cpp_scalar_type<bg::List<bg::Dict<bg::symbol, bg::List<Base>>>>();
    const auto &derived_nested = bg::cpp_scalar_type<bg::List<bg::Dict<bg::symbol, bg::List<Derived>>>>();

    CHECK(base_nested.accepts(derived_nested));
    CHECK_FALSE(derived_nested.accepts(base_nested));

    STATIC_CHECK_FALSE(bg::scalar_declaration_traits<Base>::supports_implicit_argument);
    STATIC_CHECK_FALSE(bg::scalar_declaration_traits<bg::List<Base>>::supports_implicit_argument);
    STATIC_CHECK(bg::scalar_declaration_traits<bg::List<Base>>::supports_time_series);
}

TEST_CASE("hash container keys remain invariant",
          "[feature:type-system][scalar:hierarchical][scalar:set][scalar:dict]"
          "[schema:named][declaration:concrete]") {
    using Base = bg::Bundle<Instrument>;
    using Derived = bg::Bundle<Equity>;

    const auto &base_set = bg::cpp_scalar_type<bg::Set<Base>>();
    const auto &derived_set = bg::cpp_scalar_type<bg::Set<Derived>>();
    const auto &base_dict = bg::cpp_scalar_type<bg::Dict<Base, bg::i64>>();
    const auto &derived_dict = bg::cpp_scalar_type<bg::Dict<Derived, bg::i64>>();

    CHECK_FALSE(base_set.accepts(derived_set));
    CHECK_FALSE(derived_set.accepts(base_set));
    CHECK_FALSE(base_dict.accepts(derived_dict));
    CHECK_FALSE(derived_dict.accepts(base_dict));
}

namespace {

template <template <typename, auto...> typename Field> struct Portfolio {
    Field<bg::List<bg::Bundle<Instrument>>> instruments;
};

template <template <typename, auto...> typename Field>
struct EquityPortfolio : bg::schema_extends<Portfolio, Field> {
    Field<bg::List<bg::Bundle<Equity>>> instruments;
};

template <template <typename, auto...> typename Field>
struct InvalidPortfolio : bg::schema_extends<Portfolio, Field> {
    Field<bg::List<bg::i64>> instruments;
};

template <template <typename, auto...> typename Field>
struct OptionalSymbol : bg::schema_extends<Instrument, Field> {
    Field<bg::symbol, bg::optional> symbol;
};

} // namespace

TEST_CASE("a derived hierarchical scalar can narrow inherited fields",
          "[feature:type-system][scalar:hierarchical][scalar:list][schema:named]"
          "[declaration:concrete][binding:covariant]") {
    const auto &portfolio = resolved_hierarchical_scalar_type(
        bg::cpp_scalar_type<bg::Bundle<EquityPortfolio>>());
    const auto &required_price = resolved_hierarchical_scalar_type(
        bg::cpp_scalar_type<bg::Bundle<RequiredPrice>>());

    REQUIRE(portfolio.field_count() == 1);
    CHECK(portfolio.field(0).type == &bg::cpp_scalar_type<bg::List<bg::Bundle<Equity>>>());
    using namespace bg::literals;
    auto memory = bg::allocator{};
    const auto equity = bg::bundle<Equity>(memory, "symbol"_n = bg::symbol{"ABC"}, "lot_size"_n = bg::i64{100});
    const auto instruments =
        bg::ScalarValue<bg::List<bg::Bundle<Equity>>>{std::array{equity.view()}, memory};
    const auto derived_value = bg::bundle<EquityPortfolio>(memory, instruments.view());
    const auto derived_view = bg::Scalar<bg::Bundle<EquityPortfolio>>::from(derived_value);
    const auto base_view = bg::Scalar<bg::Bundle<Portfolio>>::from(derived_view);
    CHECK(&base_view.field_type(0) == &bg::cpp_scalar_type<bg::List<bg::Bundle<Equity>>>());
    REQUIRE(required_price.field_count() == 2);
    CHECK(required_price.field(1).name.text() == "price");
    CHECK_FALSE(required_price.field(1).optional);
}

TEST_CASE("hierarchical scalar field redefinitions preserve base compatibility",
          "[feature:type-system][scalar:hierarchical][schema:named][declaration:concrete]"
          "[binding:covariant]") {
    CHECK_THROWS_WITH(bg::cpp_scalar_type<bg::Bundle<InvalidPortfolio>>(),
                      "a redefined schema field must have a compatible narrower type");
    CHECK_THROWS_WITH(bg::cpp_scalar_type<bg::Bundle<OptionalSymbol>>(),
                      "a required schema field cannot become optional");
}

namespace {

template <template <typename, auto...> typename Field> struct DirectNode {
    Field<bg::i64> value;
    Field<bg::Bundle<DirectNode>, bg::optional> next;
};

template <template <typename, auto...> typename Field> struct MutualRight;

template <template <typename, auto...> typename Field> struct MutualLeft {
    Field<bg::Bundle<MutualRight>, bg::optional> right;
};

template <template <typename, auto...> typename Field> struct MutualRight {
    Field<bg::Bundle<MutualLeft>, bg::optional> left;
};

template <template <typename, auto...> typename Field> struct ListNode {
    Field<bg::List<bg::Bundle<ListNode>>> children;
};

template <template <typename, auto...> typename Field> struct SetNode {
    Field<bg::Set<bg::Bundle<SetNode>>> children;
};

template <template <typename, auto...> typename Field> struct DictNode {
    Field<bg::Dict<bg::symbol, bg::Bundle<DictNode>>> children;
};

} // namespace

TEST_CASE("hierarchical scalar schemas can refer to themselves directly",
          "[feature:type-system][scalar:hierarchical][schema:named][declaration:concrete]") {
    const auto &type = resolved_hierarchical_scalar_type(
        bg::cpp_scalar_type<bg::Bundle<DirectNode>>());
    const auto &schema = bg::details::cpp_hierarchical_scalar_schema<DirectNode>();

    REQUIRE(type.field_count() == 2);
    CHECK(type.field(1).type == &type);
    CHECK(type.field(1).optional);
    CHECK(type.supports_equality());
    CHECK(type.supports_hash());
    CHECK(&bg::hierarchical_scalar_type(schema) == &type);
}

TEST_CASE("hierarchical scalar schemas can refer to each other",
          "[feature:type-system][scalar:hierarchical][schema:named][declaration:concrete]") {
    const auto &left = resolved_hierarchical_scalar_type(
        bg::cpp_scalar_type<bg::Bundle<MutualLeft>>());
    const auto &right = resolved_hierarchical_scalar_type(
        bg::cpp_scalar_type<bg::Bundle<MutualRight>>());

    REQUIRE(left.field_count() == 1);
    REQUIRE(right.field_count() == 1);
    CHECK(left.field(0).type == &right);
    CHECK(right.field(0).type == &left);
    CHECK(left.supports_hash());
    CHECK(right.supports_hash());
}

TEST_CASE("hierarchical scalar recursion composes through scalar collections",
          "[feature:type-system][scalar:hierarchical][scalar:list][scalar:set][scalar:dict]"
          "[schema:named][declaration:concrete]") {
    const auto &list_node = resolved_hierarchical_scalar_type(
        bg::cpp_scalar_type<bg::Bundle<ListNode>>());
    const auto &list = bg::type_cast<bg::ListScalarType>(*list_node.field(0).type);
    const auto &set_node = resolved_hierarchical_scalar_type(
        bg::cpp_scalar_type<bg::Bundle<SetNode>>());
    const auto &set = bg::type_cast<bg::SetScalarType>(*set_node.field(0).type);
    const auto &dict_node = resolved_hierarchical_scalar_type(
        bg::cpp_scalar_type<bg::Bundle<DictNode>>());
    const auto &dict = bg::type_cast<bg::DictScalarType>(*dict_node.field(0).type);

    CHECK(&list.item_type() == &list_node);
    CHECK(&set.item_type() == &set_node);
    CHECK(&dict.key_type() == &bg::cpp_scalar_type<bg::symbol>());
    CHECK(&dict.mapped_type() == &dict_node);
    CHECK(list_node.supports_hash());
    CHECK(dict_node.supports_hash());
}

TEST_CASE("a recursive hierarchical scalar has one identity across translation units",
          "[feature:type-system][scalar:hierarchical][schema:named][declaration:concrete]") {
    const auto &type = resolved_hierarchical_scalar_type(
        bg::cpp_scalar_type<bg::Bundle<test_hierarchical_scalar_provider::RecursiveInstrument>>());

    CHECK(&type == &test_hierarchical_scalar_provider::recursive_instrument_type());
    REQUIRE(type.field_count() == 2);
    CHECK(type.field(1).type == &type);
}

namespace {

template <template <typename, auto...> typename Field> struct RecursiveBase {
    Field<bg::i64> value;
};

template <template <typename, auto...> typename Field>
struct InvalidRecursive : bg::schema_extends<RecursiveBase, Field> {
    Field<bg::List<bg::Bundle<InvalidRecursive>>> children;
    Field<double> value;
};

template <template <typename, auto...> typename Field> struct ConcurrentNode {
    Field<bg::List<bg::Bundle<ConcurrentNode>>> children;
};

template <template <typename, auto...> typename Field> struct RequiredSelfCycle {
    Field<bg::Bundle<RequiredSelfCycle>> next;
};

template <template <typename, auto...> typename Field> struct RecursiveSchemaParent {
    Field<bg::i64> value;
};

template <template <typename, auto...> typename Field>
struct RecursiveSchemaChild : bg::schema_extends<RecursiveSchemaParent, Field> {
    Field<bg::i64> extra;
};

template <template <typename, auto...> typename Field> struct RecursiveInheritanceBase;
template <template <typename, auto...> typename Field> struct RecursiveInheritanceDerived;

template <template <typename, auto...> typename Field> struct RecursiveInheritanceBase {
    Field<bg::Bundle<RecursiveInheritanceDerived>, bg::optional> nested;
    Field<bg::Bundle<RecursiveSchemaParent>> item;
};

template <template <typename, auto...> typename Field>
struct RecursiveInheritanceDerived : bg::schema_extends<RecursiveInheritanceBase, Field> {
    Field<bg::Bundle<RecursiveSchemaChild>> item;
    Field<bg::i64> extra;
};

template <template <typename, auto...> typename Field> struct RequiredMutualRight;

template <template <typename, auto...> typename Field> struct RequiredMutualLeft {
    Field<bg::Bundle<RequiredMutualRight>> right;
};

template <template <typename, auto...> typename Field> struct RequiredMutualRight {
    Field<bg::Bundle<RequiredMutualLeft>> left;
};

template <template <typename, auto...> typename Field> struct UnhashableRecursiveKey {
    Field<bg::Opaque<int>> payload;
    Field<bg::Dict<bg::Bundle<UnhashableRecursiveKey>, bg::i64>> index;
};

} // namespace

TEST_CASE("failed recursive registration can be retried",
          "[feature:type-system][scalar:hierarchical][scalar:list]"
          "[schema:named][declaration:concrete]") {
    const auto register_type = [] { return &bg::cpp_scalar_type<bg::Bundle<InvalidRecursive>>(); };

    CHECK_THROWS_WITH(register_type(),
                      "a redefined schema field must have a compatible narrower type");
    CHECK_THROWS_WITH(register_type(),
                      "a redefined schema field must have a compatible narrower type");
}

TEST_CASE("a required direct self-cycle is rejected",
          "[feature:type-system][scalar:hierarchical][schema:named][declaration:concrete]") {
    const auto register_type = [] {
        return &bg::cpp_scalar_type<bg::Bundle<RequiredSelfCycle>>();
    };

    CHECK_THROWS_WITH(register_type(),
                      "required hierarchical scalar fields cannot form a recursive cycle");
    CHECK_THROWS_WITH(register_type(),
                      "required hierarchical scalar fields cannot form a recursive cycle");
}

TEST_CASE("recursive scalar schemas preserve base inheritance and narrowed fields",
          "[feature:type-system][scalar:hierarchical][schema:named][declaration:concrete]") {
    const auto &base = resolved_hierarchical_scalar_type(
        bg::cpp_scalar_type<bg::Bundle<RecursiveInheritanceBase>>());
    const auto &derived = resolved_hierarchical_scalar_type(
        bg::cpp_scalar_type<bg::Bundle<RecursiveInheritanceDerived>>());
    const auto &parent = resolved_hierarchical_scalar_type(
        bg::cpp_scalar_type<bg::Bundle<RecursiveSchemaParent>>());
    const auto &child = resolved_hierarchical_scalar_type(
        bg::cpp_scalar_type<bg::Bundle<RecursiveSchemaChild>>());

    CHECK(derived.base() == &base);
    REQUIRE(derived.field_count() == 3);
    CHECK(derived.field(0).name.text() == "nested");
    CHECK(derived.field(0).type == &derived);
    CHECK(derived.field(1).name.text() == "item");
    CHECK(derived.field(1).type == &child);
    CHECK(child.base() == &parent);
}

TEST_CASE("a required direct mutual cycle is rejected",
          "[feature:type-system][scalar:hierarchical][schema:named][declaration:concrete]") {
    CHECK_THROWS_WITH(bg::cpp_scalar_type<bg::Bundle<RequiredMutualLeft>>(),
                      "required hierarchical scalar fields cannot form a recursive cycle");
}

TEST_CASE("recursive dictionary keys are validated after capability finalization",
          "[feature:type-system][scalar:hierarchical][scalar:dict]"
          "[schema:named][declaration:concrete]") {
    const auto register_type = [] {
        return &bg::cpp_scalar_type<bg::Bundle<UnhashableRecursiveKey>>();
    };

    CHECK_THROWS_WITH(register_type(), "dictionary key type does not support hashing");
    CHECK_THROWS_WITH(register_type(), "dictionary key type does not support hashing");
}

TEST_CASE("concurrent first access publishes one complete recursive type",
          "[feature:type-system][scalar:hierarchical][scalar:list]"
          "[schema:named][declaration:concrete]") {
    constexpr auto thread_count = std::size_t{8};
    auto start = std::barrier{thread_count};
    auto results = std::array<const bg::ResolvedHierarchicalScalarType *, thread_count>{};
    auto threads = std::array<std::jthread, thread_count>{};

    for (auto index = std::size_t{}; index < thread_count; ++index) {
        threads[index] = std::jthread{[&, index] {
            start.arrive_and_wait();
            results[index] = &resolved_hierarchical_scalar_type(
                bg::cpp_scalar_type<bg::Bundle<ConcurrentNode>>());
        }};
    }
    for (auto &thread : threads) {
        thread.join();
    }

    const auto *type = results.front();
    REQUIRE(type != nullptr);
    REQUIRE(type->field_count() == 1);
    const auto &list = bg::type_cast<bg::ListScalarType>(*type->field(0).type);
    CHECK(&list.item_type() == type);
    for (const auto *result : results) {
        CHECK(result == type);
        CHECK(result->field_count() == 1);
    }
}
