#include <bg/scalar/cpp_runtime_type.hpp>
#include <bg/scalar/value.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <memory_resource>
#include <string_view>
#include <typeinfo>

TEST_CASE("an erased scalar value copies a borrowed value",
          "[value][scalar][erased][feature:scalar-api][scalar:generic]") {
    auto source = std::pmr::string{"value"};
    const auto view = bg::Scalar<bg::SCALAR>::from<bg::string>(source);
    auto value = bg::ScalarValue<>{view};

    source.assign("changed");

    CHECK(&value.type() == &bg::string_type());
    CHECK(*bg::scalar_cast<bg::string>(value) == "value");

    const auto same_source = bg::ScalarValue{std::string_view{"value"}};
    const auto same = bg::ScalarValue<>{same_source};
    CHECK(value.equals(same));
    CHECK(value.hash() == same.hash());
}

TEST_CASE("an erased scalar value stores a pointer-sized borrowed value inline",
          "[value][scalar][erased][feature:scalar-api][scalar:generic][scalar:integer][storage:inline]") {
    const auto source_value = std::int64_t{42};
    const auto source = bg::Scalar<std::int64_t>::from(source_value);
    const auto value = bg::ScalarValue<>{source, *std::pmr::null_memory_resource()};

    CHECK(*bg::scalar_cast<std::int64_t>(value) == source_value);
}

TEST_CASE("an erased scalar value can replace its value",
          "[value][scalar][erased][feature:scalar-api][scalar:generic]") {
    const auto first = bg::ScalarValue{std::string_view{"first"}};
    const auto second = bg::ScalarValue{42};
    auto value = bg::ScalarValue<>{first};

    value = second;

    const auto view = value.view();
    CHECK(*bg::scalar_cast<std::int64_t>(view) == 42);
}

TEST_CASE("an erased scalar value can replace its value from a borrowed view",
          "[value][scalar][erased][feature:scalar-api][scalar:generic]") {
    const auto source_value = std::int64_t{42};
    const auto source = bg::Scalar<std::int64_t>::from(source_value);
    const auto initial = bg::ScalarValue{std::string_view{"first"}};
    auto value = bg::ScalarValue<>{initial};

    value = source;

    const auto view = value.view();
    CHECK(*bg::scalar_cast<std::int64_t>(view) == source_value);
}

TEST_CASE("scalar handles compare by type and value", "[value][scalar][erased][feature:scalar-api]") {
    const auto left_value = std::int64_t{42};
    const auto equal_value = std::int64_t{42};
    const auto unequal_value = std::int64_t{43};
    const auto double_value = 42.0;
    const auto left = bg::Scalar<std::int64_t>::from(left_value);
    const auto equal = bg::Scalar<std::int64_t>::from(equal_value);
    const auto different_value = bg::Scalar<std::int64_t>::from(unequal_value);
    const auto different_type = bg::Scalar<double>::from(double_value);

    CHECK(left == equal);
    CHECK_FALSE(left == different_value);
    CHECK_FALSE(left == different_type);
    CHECK(left.hash() == equal.hash());
}

TEST_CASE("a constrained scalar variable exposes its required operations",
          "[value][scalar][erased][feature:scalar-api][scalar:generic]") {
    using KEY = bg::ScalarVariable<"KEY", bg::EqualityComparable, bg::Hashable>;

    const auto left_value = std::int64_t{42};
    const auto equal_value = std::int64_t{42};
    const auto different_value = std::int64_t{43};
    const auto left = bg::Scalar<KEY>::from<std::int64_t>(left_value);
    const auto equal = bg::Scalar<KEY>::from<std::int64_t>(equal_value);
    const auto different = bg::Scalar<KEY>::from<std::int64_t>(different_value);

    CHECK(left == equal);
    CHECK_FALSE(left == different);
    CHECK(left.hash() == equal.hash());
    CHECK(&left.type() == &bg::int64_type());
}

TEST_CASE("scalar variable equality rejects a different semantic type",
          "[value][scalar][erased][feature:scalar-api][scalar:generic]") {
    using VALUE = bg::ScalarVariable<"VALUE", bg::EqualityComparable>;

    const auto integer_value = std::int64_t{42};
    const auto string_value = std::pmr::string{"42"};
    const auto integer = bg::Scalar<VALUE>::from<std::int64_t>(integer_value);
    const auto string = bg::Scalar<VALUE>::from<bg::string>(string_value);

    CHECK_FALSE(integer == string);
    CHECK_FALSE(string == integer);
}

TEST_CASE("checked scalar casts reject an incompatible value", "[value][scalar][erased][feature:scalar-api]") {
    const auto value = std::int64_t{42};
    const auto erased = bg::Scalar<>::from<std::int64_t>(value);

    CHECK(*bg::scalar_cast<std::int64_t>(erased) == value);
    CHECK(*bg::unchecked_scalar_cast<std::int64_t>(erased) == value);
    CHECK_THROWS_AS(bg::scalar_cast<double>(erased), std::bad_cast);
}
