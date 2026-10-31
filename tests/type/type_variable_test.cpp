#include <bg/time_series/time_series_type.hpp>
#include <bg/type/scalar_variable.hpp>
#include <bg/type/signature.hpp>
#include <bg/type/type_variable.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>

using namespace bg;

namespace {

using KEY = ScalarVariable<"KEY", Hashable>;
using KEY_ARGUMENT = Union<"key", KEY, TS<KEY>>;
using LEFT_KEY_ARGUMENT = Union<"left_key", KEY, TS<KEY>>;
using RIGHT_KEY_ARGUMENT = Union<"right_key", KEY, TS<KEY>>;

TEST_CASE("a union type variable matches each alternative",
          "[feature:type-system][declaration:generic][series:value]") {
    auto scalar_type_map = TypeMap{};
    auto time_series_type_map = TypeMap{};

    REQUIRE(KEY_ARGUMENT::type().match(int64_type(), scalar_type_map));
    CHECK(&KEY::type().resolve(scalar_type_map) == &int64_type());
    CHECK(&KEY_ARGUMENT::type().resolve(scalar_type_map) == &int64_type());

    REQUIRE(KEY_ARGUMENT::type().match(TS<float>::type(), time_series_type_map));
    CHECK(&KEY::type().resolve(time_series_type_map) == &float_type());
    CHECK(&KEY_ARGUMENT::type().resolve(time_series_type_map) == &TS<float>::type());
}

TEST_CASE("reference transparency applies before union matching",
          "[feature:type-system][declaration:generic][feature:reference][binding:reference-to-value]") {
    auto type_map = TypeMap{};

    REQUIRE(KEY_ARGUMENT::type().match(REF<TS<i64>>::type(), type_map));
    CHECK(&KEY::type().resolve(type_map) == &int64_type());
    CHECK(&KEY_ARGUMENT::type().resolve(type_map) == &TS<i64>::type());
}

TEST_CASE("a seeded union mapping resolves variables in its selected alternative",
          "[feature:type-system][declaration:generic][series:value]") {
    auto type_map = TypeMap{};
    type_map.set(KEY_ARGUMENT::type(), &TS<double>::type());

    REQUIRE(KEY_ARGUMENT::type().match(TS<double>::type(), type_map));
    CHECK(&KEY::type().resolve(type_map) == &double_type());
}

TEST_CASE("repeated union declarations select one concrete type",
          "[feature:type-system][declaration:generic][series:value]") {
    const auto signature =
        Signature{util::symbol{"same"},
                  {{util::symbol{"left"}, &KEY_ARGUMENT::type()}, {util::symbol{"right"}, &KEY_ARGUMENT::type()}}};
    const auto arguments = std::array<const bg::Type *, 2>{&int64_type(), &TS<i64>::type()};

    CHECK_THROWS_AS(signature.resolve(arguments), TypeResolutionError);
}

TEST_CASE("named unions bind outer types independently and share inner variables",
          "[feature:type-system][declaration:generic][series:value]") {
    const auto signature = Signature{
        util::symbol{"independent"},
        {{util::symbol{"left"}, &LEFT_KEY_ARGUMENT::type()}, {util::symbol{"right"}, &RIGHT_KEY_ARGUMENT::type()}}};
    const auto arguments = std::array<const bg::Type *, 2>{&int64_type(), &TS<i64>::type()};
    const auto resolution = signature.resolve_with_type_map(arguments);

    CHECK(&KEY::type().resolve(resolution.type_map) == &int64_type());
    CHECK(&LEFT_KEY_ARGUMENT::type().resolve(resolution.type_map) == &int64_type());
    CHECK(&RIGHT_KEY_ARGUMENT::type().resolve(resolution.type_map) == &TS<i64>::type());
}

TEST_CASE("named unions still reject conflicting shared inner mappings",
          "[feature:type-system][declaration:generic][series:value]") {
    const auto signature = Signature{
        util::symbol{"conflicting"},
        {{util::symbol{"left"}, &LEFT_KEY_ARGUMENT::type()}, {util::symbol{"right"}, &RIGHT_KEY_ARGUMENT::type()}}};
    const auto arguments = std::array<const bg::Type *, 2>{&int64_type(), &TS<float>::type()};

    CHECK_THROWS_AS(signature.resolve(arguments), TypeResolutionError);
}

TEST_CASE("compatible overlapping union alternatives combine their mappings",
          "[feature:type-system][declaration:generic]") {
    using NUMBER = ScalarVariable<"NUMBER">;
    using INTEGER = TypeVariable<"INTEGER", NUMBER, i64>;

    auto type_map = TypeMap{};

    REQUIRE(INTEGER::type().match(int64_type(), type_map));
    CHECK(&NUMBER::type().resolve(type_map) == &int64_type());
}

TEST_CASE("type map comparison includes effective overlay mappings",
          "[feature:type-system][declaration:generic]") {
    using VALUE = ScalarVariable<"VALUE">;

    auto base = TypeMap{};
    base.set(KEY::type(), &int64_type());

    auto overlay = base.overlay();
    overlay.set(VALUE::type(), &float_type());
    auto equivalent = overlay.copy();

    CHECK(overlay.has_same_mappings(equivalent));

    equivalent.set(KEY::type(), &double_type());
    CHECK_FALSE(overlay.has_same_mappings(equivalent));
}

} // namespace
