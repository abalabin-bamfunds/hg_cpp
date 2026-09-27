#include <bg/schema/reflection.hpp>

#include <catch2/catch_test_macros.hpp>

#include <concepts>
#include <cstddef>
#include <utility>

namespace {

template <typename T> struct reflected_field {
    using value_type = T;
};

template <template <typename> typename Field> inline constexpr bool is_reflected_field = false;
template <> inline constexpr bool is_reflected_field<reflected_field> = true;

template <template <template <typename> typename> typename Base, template <typename> typename Field,
          bool Reflection = is_reflected_field<Field>>
struct extends;

template <template <template <typename> typename> typename Base, template <typename> typename Field>
struct extends<Base, Field, false> : Base<Field> {};

template <template <template <typename> typename> typename Base, template <typename> typename Field>
struct extends<Base, Field, true> : bg::details::schema_base_marker<Base> {};

template <template <typename> typename Field> struct base_schema {
    Field<int> first;
    Field<double> replaced;
};

template <template <typename> typename Field> struct derived_schema : extends<base_schema, Field> {
    Field<float> replaced;
    Field<bool> last;
};

using reflection = bg::details::schema_reflection<derived_schema, reflected_field>;

template <typename T> struct constructed_field {
    std::size_t index;
};

struct construction {
    template <template <template <typename> typename> typename Base, template <typename> typename Field>
    static extends<Base, Field> make_base(Base<Field> base) {
        return extends<Base, Field>{std::move(base)};
    }
};

struct factory {
    template <typename ReflectedField>
    constructed_field<typename ReflectedField::value_type> make(std::size_t index) const {
        return {index};
    }
};

} // namespace

TEST_CASE("schema reflection discovers base, replacement, and new fields", "[reflection][coverage-exempt]") {
    STATIC_CHECK(reflection::has_base);
    STATIC_CHECK(reflection::direct_field_count == 2);
    STATIC_CHECK(reflection::field_count == 3);
    STATIC_CHECK(reflection::direct_field_name<0>() == "replaced");
    STATIC_CHECK(reflection::direct_field_name<1>() == "last");
    STATIC_CHECK(reflection::field_index("first") == 0);
    STATIC_CHECK(reflection::field_index("replaced") == 1);
    STATIC_CHECK(reflection::field_index("last") == 2);
    STATIC_CHECK(reflection::field_index("missing") == bg::details::missing_schema_field);
    STATIC_CHECK(reflection::direct_field_logical_index(0) == 1);
    STATIC_CHECK(reflection::direct_field_logical_index(1) == 2);
}

TEST_CASE("schema reflection resolves inherited and replaced field types", "[reflection][coverage-exempt]") {
    STATIC_CHECK(std::same_as<typename reflection::field_type<"first">::value_type, int>);
    STATIC_CHECK(std::same_as<typename reflection::field_type<"replaced">::value_type, float>);
    STATIC_CHECK(std::same_as<typename reflection::field_type<"last">::value_type, bool>);
}

TEST_CASE("schema construction uses logical field indices throughout the hierarchy", "[reflection][coverage-exempt]") {
    auto values = factory{};

    const auto result = reflection::make<constructed_field, construction>(values);
    const auto &base = static_cast<const base_schema<constructed_field> &>(result);

    CHECK(base.first.index == 0);
    CHECK(base.replaced.index == 1);
    CHECK(result.replaced.index == 1);
    CHECK(result.last.index == 2);
}
