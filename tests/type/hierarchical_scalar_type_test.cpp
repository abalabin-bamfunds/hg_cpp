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

using namespace bg;

namespace {

using test_hierarchical_scalar_provider::Instrument;

static_assert(!std::same_as<InputValidity, Optionality>);
static_assert(sizeof(InputValidity) == sizeof(std::uint8_t));
static_assert(std::is_trivially_copyable_v<InputValidity>);
static_assert(required == InputValidity::required);
static_assert(bg::optional == InputValidity::optional);
static_assert(InputValidity::required == required);
static_assert(InputValidity::optional == bg::optional);
static_assert(all_valid == InputValidity::all_valid);

template <template <typename, auto...> typename Field> struct Equity : schema_extends<Instrument, Field> {
    Field<i64> lot_size;
};

template <template <typename, auto...> typename Field> struct RequiredPrice : schema_extends<Instrument, Field> {
    Field<double> price;
};

const ResolvedHierarchicalScalarType &resolved_hierarchical_scalar_type(const bg::Type &type) {
    const auto &hierarchical = type_cast<HierarchicalScalarType>(type);
    if (!hierarchical.is_resolved()) {
        throw std::logic_error{"expected a resolved hierarchical scalar type"};
    }
    return static_cast<const ResolvedHierarchicalScalarType &>(hierarchical);
}

} // namespace

TEST_CASE("a hierarchical scalar has canonical ordered schema metadata",
          "[feature:type-system][scalar:hierarchical][schema:named][declaration:concrete]") {
    const auto &type = cpp_scalar_type<Bundle<Equity>>();
    const auto &same = cpp_scalar_type<Bundle<Equity>>();
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
    CHECK(hierarchical.find(util::symbol{"missing"}) == nullptr);
    CHECK(hierarchical.supports_equality());
    CHECK(hierarchical.supports_hash());
    CHECK(hierarchical.supports_value_covariance());
    CHECK(text == "Equity");
}

TEST_CASE("a hierarchical scalar has one semantic identity across translation units",
          "[feature:type-system][scalar:hierarchical][schema:named][declaration:concrete]") {
    CHECK(&cpp_scalar_type<Bundle<Instrument>>() == &test_hierarchical_scalar_provider::instrument_type());
}

namespace {

template <template <typename, auto...> typename Field> struct MetadataBase {
    Field<i64> value;

    static SchemaMetadata metadata() {
        return make_schema_metadata({
            schema_metadata_entry("extension.base", ScalarValue<i64>{5}),
            schema_metadata_entry("extension.version", ScalarValue<i64>{1}),
        });
    }
};

template <template <typename, auto...> typename Field> struct MetadataDerived : schema_extends<MetadataBase, Field> {
    Field<i64> extra;

    static SchemaMetadata metadata() {
        return make_schema_metadata({schema_metadata_entry("extension.version", ScalarValue<i64>{2})});
    }
};

template <template <typename, auto...> typename Field>
struct MetadataInheritingDerived : schema_extends<MetadataBase, Field> {
    Field<i64> extra;
};

} // namespace

TEST_CASE("C++ schema metadata is registered for scalar and TSB projections",
          "[feature:type-system][schema:named][declaration:concrete]") {
    const auto &scalar_schema = details::cpp_hierarchical_scalar_schema<MetadataDerived>();
    const auto &series_schema = TSB<MetadataDerived>::type().schema();

    REQUIRE(scalar_schema.base() != nullptr);
    REQUIRE(scalar_schema.metadata().size() == 2);
    REQUIRE(series_schema.metadata().size() == 2);
    CHECK(scalar_schema.metadata() == series_schema.metadata());
    CHECK(Scalar<>::from(*series_schema.metadata().find("extension.base")).equals(ScalarValue<i64>{5}));
    CHECK(Scalar<>::from(*series_schema.metadata().find("extension.version")).equals(ScalarValue<i64>{2}));
    CHECK(Scalar<>::from(*scalar_schema.metadata().find("extension.base")).equals(ScalarValue<i64>{5}));
    CHECK(Scalar<>::from(*scalar_schema.metadata().find("extension.version")).equals(ScalarValue<i64>{2}));
    CHECK(Scalar<>::from(*scalar_schema.direct_metadata().find("extension.version")).equals(ScalarValue<i64>{2}));
    CHECK(&schema_type<MetadataDerived>::type() == &series_schema);
}

TEST_CASE("C++ schemas without a metadata hook inherit only their base metadata",
          "[feature:type-system][schema:named][declaration:concrete]") {
    const auto &schema = details::cpp_hierarchical_scalar_schema<MetadataInheritingDerived>();

    REQUIRE(schema.base() != nullptr);
    CHECK(schema.direct_metadata().empty());
    REQUIRE(schema.metadata().size() == 2);
    CHECK(Scalar<>::from(*schema.metadata().find("extension.base")).equals(ScalarValue<i64>{5}));
    CHECK(Scalar<>::from(*schema.metadata().find("extension.version")).equals(ScalarValue<i64>{1}));
}

namespace {

template <template <typename, auto...> typename Field> struct NotYetRegistered {
    Field<i64> value;
};

} // namespace

TEST_CASE("a C++ schema needs its scalar bundle registered before runtime lookup",
          "[feature:type-system][scalar:hierarchical][schema:named][declaration:concrete]") {
    const auto &schema = details::cpp_hierarchical_scalar_schema<NotYetRegistered>();
    const auto lookup = [&] { return &hierarchical_scalar_type(schema); };

    CHECK_THROWS_WITH(lookup(), "a C++-identified schema requires prior hierarchical scalar registration");
    CHECK_THROWS_WITH(lookup(), "a C++-identified schema requires prior hierarchical scalar registration");

    const auto &registered = cpp_scalar_type<Bundle<NotYetRegistered>>();
    const auto &resolved = hierarchical_scalar_type(schema);
    CHECK(&resolved == &type_cast<HierarchicalScalarType>(registered));
}

TEST_CASE("runtime schemas have canonical hierarchical scalar types",
          "[feature:type-system][scalar:hierarchical][schema:named][declaration:runtime]") {
    const auto runtime_fields = std::array{SchemaField{util::symbol{"value"}, &int64_type()}};
    const auto &runtime_schema =
        bg::schema(util::symbol{"RuntimeRecord"}, new_runtime_schema_id(), nullptr, runtime_fields);
    const auto &runtime_type = hierarchical_scalar_type(runtime_schema);
    CHECK(&hierarchical_scalar_type(runtime_schema) == &runtime_type);
    CHECK(runtime_type.accepts(runtime_type));

    const auto same_shape_fields = std::array{SchemaField{util::symbol{"value"}, &int64_type()}};
    const auto &same_shape = hierarchical_scalar_type(
        bg::schema(util::symbol{"OtherRuntimeRecord"}, new_runtime_schema_id(), nullptr, same_shape_fields));
    CHECK_FALSE(runtime_type.accepts(same_shape));
}

TEST_CASE("anonymous schemas have canonical hierarchical scalar types",
          "[feature:type-system][scalar:hierarchical][schema:unnamed][declaration:runtime]") {
    const auto fields = std::array{SchemaField{util::symbol{"label"}, &string_type()}};
    const auto &anonymous = anonymous_schema(fields);
    const auto &type = hierarchical_scalar_type(anonymous);

    CHECK(&hierarchical_scalar_type(anonymous) == &type);
    CHECK(type.accepts(type));
}

TEST_CASE("runtime hierarchical schema compatibility follows nominal inheritance",
          "[feature:type-system][scalar:hierarchical][schema:named][declaration:runtime][binding:covariant]") {
    const auto base_fields = std::array{SchemaField{util::symbol{"symbol"}, &symbol_type()}};
    const auto &base_schema =
        bg::schema(util::symbol{"RuntimeInstrument"}, new_runtime_schema_id(), nullptr, base_fields);
    const auto derived_fields = std::array{SchemaField{util::symbol{"lot_size"}, &int64_type()}};
    const auto &derived_schema =
        bg::schema(util::symbol{"RuntimeEquity"}, new_runtime_schema_id(), &base_schema, derived_fields);

    const auto &base_type = hierarchical_scalar_type(base_schema);
    const auto &derived_type = hierarchical_scalar_type(derived_schema);

    CHECK(base_type.accepts(derived_type));
    CHECK_FALSE(derived_type.accepts(base_type));
    CHECK(derived_type.field_count() == 2);
}

TEST_CASE("shared schemas have one scalar and TSB projection across shared libraries",
          "[feature:type-system][schema:named][declaration:concrete]") {
    CHECK(&test_shared_schema_provider::scalar_type() ==
          &cpp_scalar_type<Bundle<test_shared_schema_provider::SharedSchema>>());
    CHECK(&test_shared_schema_provider::time_series_type() == &TSB<test_shared_schema_provider::SharedSchema>::type());
}

TEST_CASE("a conflicting hierarchical scalar declaration in another translation unit is rejected",
          "[feature:type-system][scalar:hierarchical][schema:named][declaration:concrete]") {
    CHECK_THROWS_WITH(test_hierarchical_scalar_provider::conflicting_instrument_type(),
                      "conflicting hierarchical scalar declaration: Instrument");
}

namespace {

template <template <typename, auto...> typename Field> struct UnrelatedInstrument {
    Field<bg::symbol> symbol;
    Field<double, bg::optional> price;
};

} // namespace

TEST_CASE("hierarchical scalar compatibility follows declared inheritance",
          "[feature:type-system][scalar:hierarchical][schema:named][declaration:concrete]"
          "[binding:covariant]") {
    const auto &base = cpp_scalar_type<Bundle<Instrument>>();
    const auto &derived = cpp_scalar_type<Bundle<Equity>>();
    const auto &unrelated = cpp_scalar_type<Bundle<UnrelatedInstrument>>();

    CHECK(base.accepts(derived));
    CHECK(derived.accepts(derived));
    CHECK_FALSE(derived.accepts(base));
    CHECK_FALSE(base.accepts(unrelated));
}

TEST_CASE("HIERARCHICAL resolves to a concrete hierarchical scalar",
          "[feature:type-system][scalar:hierarchical][declaration:generic]") {
    const auto &concrete = cpp_scalar_type<Bundle<Equity>>();
    const auto &variable = cpp_scalar_type<HIERARCHICAL>();
    auto type_map = TypeMap{};

    REQUIRE(variable.match(concrete, type_map));
    CHECK(&variable.resolve(type_map) == &concrete);

    auto rejected_type_map = TypeMap{};
    CHECK_FALSE(variable.match(int64_type(), rejected_type_map));
}

TEST_CASE("a generic bundle type keeps its wrapper in type text",
          "[feature:type-system][scalar:hierarchical][declaration:generic]") {
    const auto &type = cpp_scalar_type<Bundle<SCHEMA>>();
    auto text = std::string{};

    type.format_name(text);

    CHECK(text == "Bundle<SCHEMA>");
}

namespace {

template <template <typename, auto...> typename Field> using ENTITY = BundleSchemaVariableDeclaration<"ENTITY", Field>;

} // namespace

TEST_CASE("a generic bundle declaration captures and resolves its schema variable",
          "[feature:type-system][scalar:hierarchical][declaration:generic]") {
    const auto &generic = cpp_scalar_type<Bundle<SCHEMA>>();
    const auto &concrete = cpp_scalar_type<Bundle<Equity>>();
    const auto &schema_variable = details::schema_variable_instance<SCHEMA>();
    auto type_map = TypeMap{};

    REQUIRE(generic.match(concrete, type_map));
    CHECK(std::get<const bg::Type *>(*type_map.find(schema_variable)) ==
          &type_cast<HierarchicalScalarType>(concrete).schema_type());
    CHECK(&generic.resolve(type_map) == &type_cast<HierarchicalScalarType>(concrete));

    const auto &unrelated = cpp_scalar_type<Bundle<UnrelatedInstrument>>();
    CHECK_FALSE(generic.match(unrelated, type_map));
    CHECK(std::get<const bg::Type *>(*type_map.find(schema_variable)) ==
          &type_cast<HierarchicalScalarType>(concrete).schema_type());
}

TEST_CASE("a generic bundle declaration captures a named schema variable",
          "[feature:type-system][scalar:hierarchical][schema:named][declaration:generic]") {
    const auto &generic = cpp_scalar_type<Bundle<ENTITY>>();
    const auto &concrete = cpp_scalar_type<Bundle<Equity>>();
    const auto &schema_variable = details::schema_variable_instance<ENTITY>();
    auto type_map = TypeMap{};

    CHECK_FALSE(generic.supports_value_covariance());
    REQUIRE(generic.match(concrete, type_map));
    CHECK(schema_variable.name().text() == "ENTITY");
    CHECK(std::get<const bg::Type *>(*type_map.find(schema_variable)) ==
          &type_cast<HierarchicalScalarType>(concrete).schema_type());
}

TEST_CASE("a generic bundle declaration enumerates its schema variable",
          "[feature:type-system][scalar:hierarchical][declaration:generic]") {
    auto visited = false;
    for_each_declaration_variable<Bundle<SCHEMA>>([&]<typename Declaration>(const Variable &variable) {
        STATIC_CHECK(std::same_as<Declaration, schema_type<SCHEMA>>);
        CHECK(variable.name().text() == "SCHEMA");
        visited = true;
    });

    CHECK(visited);
}

TEST_CASE("hierarchical scalar covariance composes through scalar containers",
          "[feature:type-system][scalar:hierarchical][scalar:list][scalar:dict]"
          "[schema:named][declaration:concrete][binding:covariant]") {
    using Base = Bundle<Instrument>;
    using Derived = Bundle<Equity>;

    const auto &base_nested = cpp_scalar_type<List<Dict<bg::symbol, List<Base>>>>();
    const auto &derived_nested = cpp_scalar_type<List<Dict<bg::symbol, List<Derived>>>>();

    CHECK(base_nested.accepts(derived_nested));
    CHECK_FALSE(derived_nested.accepts(base_nested));

    STATIC_CHECK_FALSE(scalar_declaration_traits<Base>::supports_implicit_argument);
    STATIC_CHECK_FALSE(scalar_declaration_traits<List<Base>>::supports_implicit_argument);
    STATIC_CHECK(scalar_declaration_traits<List<Base>>::supports_time_series);
}

TEST_CASE("hash container keys remain invariant",
          "[feature:type-system][scalar:hierarchical][scalar:set][scalar:dict]"
          "[schema:named][declaration:concrete]") {
    using Base = Bundle<Instrument>;
    using Derived = Bundle<Equity>;

    const auto &base_set = cpp_scalar_type<Set<Base>>();
    const auto &derived_set = cpp_scalar_type<Set<Derived>>();
    const auto &base_dict = cpp_scalar_type<Dict<Base, i64>>();
    const auto &derived_dict = cpp_scalar_type<Dict<Derived, i64>>();

    CHECK_FALSE(base_set.accepts(derived_set));
    CHECK_FALSE(derived_set.accepts(base_set));
    CHECK_FALSE(base_dict.accepts(derived_dict));
    CHECK_FALSE(derived_dict.accepts(base_dict));
}

namespace {

template <template <typename, auto...> typename Field> struct Portfolio {
    Field<List<Bundle<Instrument>>> instruments;
};

template <template <typename, auto...> typename Field> struct EquityPortfolio : schema_extends<Portfolio, Field> {
    Field<List<Bundle<Equity>>> instruments;
};

template <template <typename, auto...> typename Field> struct InvalidPortfolio : schema_extends<Portfolio, Field> {
    Field<List<i64>> instruments;
};

template <template <typename, auto...> typename Field> struct OptionalSymbol : schema_extends<Instrument, Field> {
    Field<bg::symbol, bg::optional> symbol;
};

} // namespace

TEST_CASE("a derived hierarchical scalar can narrow inherited fields",
          "[feature:type-system][scalar:hierarchical][scalar:list][schema:named]"
          "[declaration:concrete][binding:covariant]") {
    const auto &portfolio = resolved_hierarchical_scalar_type(cpp_scalar_type<Bundle<EquityPortfolio>>());
    const auto &required_price = resolved_hierarchical_scalar_type(cpp_scalar_type<Bundle<RequiredPrice>>());

    REQUIRE(portfolio.field_count() == 1);
    CHECK(portfolio.field(0).type == &cpp_scalar_type<List<Bundle<Equity>>>());
    using namespace literals;
    auto memory = allocator{};
    const auto equity = bundle<Equity>(memory, "symbol"_n = bg::symbol{"ABC"}, "lot_size"_n = i64{100});
    const auto instruments = ScalarValue<List<Bundle<Equity>>>{std::array{equity.view()}, memory};
    const auto derived_value = bundle<EquityPortfolio>(memory, instruments.view());
    const auto derived_view = Scalar<Bundle<EquityPortfolio>>::from(derived_value);
    const auto base_view = Scalar<Bundle<Portfolio>>::from(derived_view);
    CHECK(&base_view.type() == &derived_view.type());
    const auto base_value = ScalarValue<Bundle<Portfolio>>{derived_view, memory};
    CHECK(&base_value.type() == &derived_view.type());
    CHECK(&base_view.field_type(0) == &cpp_scalar_type<List<Bundle<Equity>>>());
    REQUIRE(required_price.field_count() == 2);
    CHECK(required_price.field(1).name.text() == "price");
    CHECK_FALSE(required_price.field(1).optional);
}

TEST_CASE("hierarchical scalar field redefinitions preserve base compatibility",
          "[feature:type-system][scalar:hierarchical][schema:named][declaration:concrete]"
          "[binding:covariant]") {
    CHECK_THROWS_WITH(cpp_scalar_type<Bundle<InvalidPortfolio>>(),
                      "a redefined schema field must have a compatible narrower type");
    CHECK_THROWS_WITH(cpp_scalar_type<Bundle<OptionalSymbol>>(), "a required schema field cannot become optional");
}

namespace {

template <template <typename, auto...> typename Field> struct DirectNode {
    Field<i64> value;
    Field<Bundle<DirectNode>, bg::optional> next;
};

template <template <typename, auto...> typename Field> struct MutualRight;

template <template <typename, auto...> typename Field> struct MutualLeft {
    Field<Bundle<MutualRight>, bg::optional> right;
};

template <template <typename, auto...> typename Field> struct MutualRight {
    Field<Bundle<MutualLeft>, bg::optional> left;
};

template <template <typename, auto...> typename Field> struct ListNode {
    Field<List<Bundle<ListNode>>> children;
};

template <template <typename, auto...> typename Field> struct SetNode {
    Field<Set<Bundle<SetNode>>> children;
};

template <template <typename, auto...> typename Field> struct DictNode {
    Field<Dict<bg::symbol, Bundle<DictNode>>> children;
};

} // namespace

TEST_CASE("hierarchical scalar schemas can refer to themselves directly",
          "[feature:type-system][scalar:hierarchical][schema:named][declaration:concrete]") {
    const auto &type = resolved_hierarchical_scalar_type(cpp_scalar_type<Bundle<DirectNode>>());
    const auto &schema = details::cpp_hierarchical_scalar_schema<DirectNode>();

    REQUIRE(type.field_count() == 2);
    CHECK(type.field(1).type == &type);
    CHECK(type.field(1).optional);
    CHECK(type.supports_equality());
    CHECK(type.supports_hash());
    CHECK(&hierarchical_scalar_type(schema) == &type);
}

TEST_CASE("hierarchical scalar schemas can refer to each other",
          "[feature:type-system][scalar:hierarchical][schema:named][declaration:concrete]") {
    const auto &left = resolved_hierarchical_scalar_type(cpp_scalar_type<Bundle<MutualLeft>>());
    const auto &right = resolved_hierarchical_scalar_type(cpp_scalar_type<Bundle<MutualRight>>());

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
    const auto &list_node = resolved_hierarchical_scalar_type(cpp_scalar_type<Bundle<ListNode>>());
    const auto &list = type_cast<ListScalarType>(*list_node.field(0).type);
    const auto &set_node = resolved_hierarchical_scalar_type(cpp_scalar_type<Bundle<SetNode>>());
    const auto &set = type_cast<SetScalarType>(*set_node.field(0).type);
    const auto &dict_node = resolved_hierarchical_scalar_type(cpp_scalar_type<Bundle<DictNode>>());
    const auto &dict = type_cast<DictScalarType>(*dict_node.field(0).type);

    CHECK(&list.item_type() == &list_node);
    CHECK(&set.item_type() == &set_node);
    CHECK(&dict.key_type() == &cpp_scalar_type<bg::symbol>());
    CHECK(&dict.mapped_type() == &dict_node);
    CHECK(list_node.supports_hash());
    CHECK(dict_node.supports_hash());
}

TEST_CASE("a recursive hierarchical scalar has one identity across translation units",
          "[feature:type-system][scalar:hierarchical][schema:named][declaration:concrete]") {
    const auto &type = resolved_hierarchical_scalar_type(
        cpp_scalar_type<Bundle<test_hierarchical_scalar_provider::RecursiveInstrument>>());

    CHECK(&type == &test_hierarchical_scalar_provider::recursive_instrument_type());
    REQUIRE(type.field_count() == 2);
    CHECK(type.field(1).type == &type);
}

namespace {

template <template <typename, auto...> typename Field> struct RecursiveBase {
    Field<i64> value;
};

template <template <typename, auto...> typename Field> struct InvalidRecursive : schema_extends<RecursiveBase, Field> {
    Field<List<Bundle<InvalidRecursive>>> children;
    Field<double> value;
};

template <template <typename, auto...> typename Field> struct ConcurrentNode {
    Field<List<Bundle<ConcurrentNode>>> children;
};

template <template <typename, auto...> typename Field> struct RequiredSelfCycle {
    Field<Bundle<RequiredSelfCycle>> next;
};

template <template <typename, auto...> typename Field> struct RequiredTupleCycle {
    Field<Tuple<i64, Bundle<RequiredTupleCycle>>> next;
};

template <template <typename, auto...> typename Field> struct RecursiveSchemaParent {
    Field<i64> value;
};

template <template <typename, auto...> typename Field>
struct RecursiveSchemaChild : schema_extends<RecursiveSchemaParent, Field> {
    Field<i64> extra;
};

template <template <typename, auto...> typename Field> struct RecursiveInheritanceBase;
template <template <typename, auto...> typename Field> struct RecursiveInheritanceDerived;

template <template <typename, auto...> typename Field> struct RecursiveInheritanceBase {
    Field<Bundle<RecursiveInheritanceDerived>, bg::optional> nested;
    Field<Bundle<RecursiveSchemaParent>> item;
};

template <template <typename, auto...> typename Field>
struct RecursiveInheritanceDerived : schema_extends<RecursiveInheritanceBase, Field> {
    Field<Bundle<RecursiveSchemaChild>> item;
    Field<i64> extra;
};

template <template <typename, auto...> typename Field> struct RequiredMutualRight;

template <template <typename, auto...> typename Field> struct RequiredMutualLeft {
    Field<Bundle<RequiredMutualRight>> right;
};

template <template <typename, auto...> typename Field> struct RequiredMutualRight {
    Field<Bundle<RequiredMutualLeft>> left;
};

template <template <typename, auto...> typename Field> struct UnhashableRecursiveKey {
    Field<Opaque<int>> payload;
    Field<Dict<Bundle<UnhashableRecursiveKey>, i64>> index;
};

} // namespace

TEST_CASE("an invalid recursive registration fails setup",
          "[feature:type-system][scalar:hierarchical][scalar:list]"
          "[schema:named][declaration:concrete]") {
    CHECK_THROWS_WITH(cpp_scalar_type<Bundle<InvalidRecursive>>(),
                      "a redefined schema field must have a compatible narrower type");
}

TEST_CASE("a required direct self-cycle is rejected",
          "[feature:type-system][scalar:hierarchical][schema:named][declaration:concrete]") {
    CHECK_THROWS_WITH(cpp_scalar_type<Bundle<RequiredSelfCycle>>(),
                      "required hierarchical scalar fields cannot form a recursive cycle");
}

TEST_CASE("a required cycle through tuple items is rejected",
          "[feature:type-system][scalar:hierarchical][scalar:tuple][schema:named][declaration:concrete]") {
    CHECK_THROWS_WITH(cpp_scalar_type<Bundle<RequiredTupleCycle>>(),
                      "required hierarchical scalar fields cannot form a recursive cycle");
}

TEST_CASE("recursive scalar schemas preserve base inheritance and narrowed fields",
          "[feature:type-system][scalar:hierarchical][schema:named][declaration:concrete]") {
    const auto &base = resolved_hierarchical_scalar_type(cpp_scalar_type<Bundle<RecursiveInheritanceBase>>());
    const auto &derived = resolved_hierarchical_scalar_type(cpp_scalar_type<Bundle<RecursiveInheritanceDerived>>());
    const auto &parent = resolved_hierarchical_scalar_type(cpp_scalar_type<Bundle<RecursiveSchemaParent>>());
    const auto &child = resolved_hierarchical_scalar_type(cpp_scalar_type<Bundle<RecursiveSchemaChild>>());

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
    CHECK_THROWS_WITH(cpp_scalar_type<Bundle<RequiredMutualLeft>>(),
                      "required hierarchical scalar fields cannot form a recursive cycle");
}

TEST_CASE("recursive dictionary keys are validated after capability finalization",
          "[feature:type-system][scalar:hierarchical][scalar:dict]"
          "[schema:named][declaration:concrete]") {
    CHECK_THROWS_WITH(cpp_scalar_type<Bundle<UnhashableRecursiveKey>>(),
                      "dictionary key type does not support hashing");
}

TEST_CASE("completed recursive types support concurrent reads",
          "[feature:type-system][scalar:hierarchical][scalar:list]"
          "[schema:named][declaration:concrete]") {
    constexpr auto thread_count = std::size_t{8};
    auto results = std::array<const ResolvedHierarchicalScalarType *, thread_count>{};
    auto threads = std::array<std::jthread, thread_count>{};
    const auto &registered = resolved_hierarchical_scalar_type(cpp_scalar_type<Bundle<ConcurrentNode>>());

    for (auto index = std::size_t{}; index < thread_count; ++index) {
        threads[index] = std::jthread{[&, index] {
            results[index] = &resolved_hierarchical_scalar_type(cpp_scalar_type<Bundle<ConcurrentNode>>());
        }};
    }
    for (auto &thread : threads) {
        thread.join();
    }

    const auto *type = &registered;
    REQUIRE(type != nullptr);
    REQUIRE(type->field_count() == 1);
    const auto &list = type_cast<ListScalarType>(*type->field(0).type);
    CHECK(&list.item_type() == type);
    for (const auto *result : results) {
        CHECK(result == type);
        CHECK(result->field_count() == 1);
    }
}
