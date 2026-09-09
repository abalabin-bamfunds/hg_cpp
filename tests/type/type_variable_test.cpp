#include <bg/time_series/time_series_type.hpp>
#include <bg/type/scalar_variable.hpp>
#include <bg/type/signature.hpp>
#include <bg/type/type_variable.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>

namespace {

using Key = bg::ScalarVariable<"Key", bg::Hashable>;
using KeyArgument = bg::Union<Key, bg::TS<Key>>;

TEST_CASE("a union type variable matches each alternative",
          "[feature:type-system][declaration:generic][series:value]") {
    auto scalar_type_map = bg::TypeMap{};
    auto time_series_type_map = bg::TypeMap{};

    REQUIRE(KeyArgument::type().match(bg::int64_type(), scalar_type_map));
    CHECK(&Key::type().resolve(scalar_type_map) == &bg::int64_type());
    CHECK(&KeyArgument::type().resolve(scalar_type_map) == &bg::int64_type());

    REQUIRE(KeyArgument::type().match(bg::TS<float>::type(), time_series_type_map));
    CHECK(&Key::type().resolve(time_series_type_map) == &bg::float_type());
    CHECK(&KeyArgument::type().resolve(time_series_type_map) == &bg::TS<float>::type());
}

TEST_CASE("reference transparency applies before union matching",
          "[feature:type-system][declaration:generic][feature:reference][binding:reference-to-value]") {
    auto type_map = bg::TypeMap{};

    REQUIRE(KeyArgument::type().match(bg::REF<bg::TS<std::int64_t>>::type(), type_map));
    CHECK(&Key::type().resolve(type_map) == &bg::int64_type());
    CHECK(&KeyArgument::type().resolve(type_map) == &bg::TS<std::int64_t>::type());
}

TEST_CASE("a seeded union mapping resolves variables in its selected alternative",
          "[feature:type-system][declaration:generic][series:value]") {
    auto type_map = bg::TypeMap{};
    type_map.set(KeyArgument::type(), &bg::TS<double>::type());

    REQUIRE(KeyArgument::type().match(bg::TS<double>::type(), type_map));
    CHECK(&Key::type().resolve(type_map) == &bg::double_type());
}

TEST_CASE("repeated union declarations select one concrete type",
          "[feature:type-system][declaration:generic][series:value]") {
    const auto signature = bg::Signature{
        bg::util::symbol{"same"},
        {{bg::util::symbol{"left"}, &KeyArgument::type()}, {bg::util::symbol{"right"}, &KeyArgument::type()}}};
    const auto arguments = std::array<const bg::Type *, 2>{&bg::int64_type(), &bg::TS<std::int64_t>::type()};

    CHECK_THROWS_AS(signature.resolve(arguments), bg::TypeResolutionError);
}

TEST_CASE("compatible overlapping union alternatives combine their mappings",
          "[feature:type-system][declaration:generic]") {
    using Number = bg::ScalarVariable<"Number">;
    using Integer = bg::TypeVariable<"Integer", Number, std::int64_t>;

    auto type_map = bg::TypeMap{};

    REQUIRE(Integer::type().match(bg::int64_type(), type_map));
    CHECK(&Number::type().resolve(type_map) == &bg::int64_type());
}

} // namespace
