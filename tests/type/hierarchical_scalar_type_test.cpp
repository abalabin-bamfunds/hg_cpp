#include "test_hierarchical_scalar_provider.hpp"

#include <bg/node/input_properties.hpp>
#include <bg/scalar/bundle/scalar_type.hpp>
#include <bg/scalar/dict/scalar_type.hpp>
#include <bg/scalar/list/scalar_type.hpp>
#include <bg/scalar/set/scalar_type.hpp>
#include <bg/schema/reflection.hpp>
#include <bg/type/type_text.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>

#include <concepts>
#include <stdexcept>
#include <string>

namespace {

using test_hierarchical_scalar_provider::Instrument;

static_assert(std::same_as<bg::InputValidity, bg::Optionality>);
static_assert(bg::required == bg::InputValidity::required);
static_assert(bg::optional == bg::InputValidity::optional);

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

} // namespace

TEST_CASE("a hierarchical scalar has canonical ordered schema metadata",
          "[feature:type-system][scalar:hierarchical][schema:named][declaration:concrete]") {
    const auto &type = bg::cpp_scalar_type<bg::Bundle<Equity>>();
    const auto &same = bg::cpp_scalar_type<bg::Bundle<Equity>>();
    const auto &hierarchical = bg::type_cast<bg::HierarchicalScalarType>(type);
    auto text = std::string{};
    bg::append_type_text(text, type);

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
    STATIC_CHECK_FALSE(bg::scalar_declaration_traits<bg::List<Base>>::supports_time_series);
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
    const auto &portfolio = bg::type_cast<bg::HierarchicalScalarType>(
        bg::cpp_scalar_type<bg::Bundle<EquityPortfolio>>());
    const auto &required_price = bg::type_cast<bg::HierarchicalScalarType>(
        bg::cpp_scalar_type<bg::Bundle<RequiredPrice>>());

    REQUIRE(portfolio.field_count() == 1);
    CHECK(portfolio.field(0).type == &bg::cpp_scalar_type<bg::List<bg::Bundle<Equity>>>());
    REQUIRE(required_price.field_count() == 2);
    CHECK(required_price.field(1).name.text() == "price");
    CHECK_FALSE(required_price.field(1).optional);
}

TEST_CASE("hierarchical scalar field redefinitions preserve base compatibility",
          "[feature:type-system][scalar:hierarchical][schema:named][declaration:concrete]"
          "[binding:covariant]") {
    CHECK_THROWS_WITH(bg::cpp_scalar_type<bg::Bundle<InvalidPortfolio>>(),
                      "a redefined hierarchical scalar field must have a compatible narrower type");
    CHECK_THROWS_WITH(bg::cpp_scalar_type<bg::Bundle<OptionalSymbol>>(),
                      "a required hierarchical scalar field cannot become optional");
}
