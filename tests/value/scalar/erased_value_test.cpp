#include <bg/scalar/cpp_runtime_type.hpp>
#include <bg/scalar/value.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <memory_resource>
#include <string_view>
#include <typeinfo>

TEST_CASE("an erased scalar value copies a borrowed value",
          "[value][scalar][erased][feature:scalar-api][scalar:generic]") {
    auto source = std::string{"value"};
    auto source_owner = bg::ScalarValue<bg::string>{source};
    const auto source_view = source_owner.view();
    const auto view = bg::Scalar<bg::SCALAR>::from(source_view);
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
    const auto source_value = bg::i64{42};
    const auto source = bg::Scalar<bg::i64>::from(source_value);
    auto allocator = bg::allocator{*std::pmr::null_memory_resource()};
    const auto value = bg::ScalarValue<>{source, allocator};

    CHECK(*bg::scalar_cast<bg::i64>(value) == source_value);
}

TEST_CASE("an erased scalar value can replace its value",
          "[value][scalar][erased][feature:scalar-api][scalar:generic]") {
    const auto first = bg::ScalarValue{std::string_view{"first"}};
    const auto second = bg::ScalarValue{42};
    auto value = bg::ScalarValue<>{first};

    value = second;

    const auto view = value.view();
    CHECK(*bg::scalar_cast<bg::i64>(view) == 42);
}

TEST_CASE("an erased scalar value can replace its value from a borrowed view",
          "[value][scalar][erased][feature:scalar-api][scalar:generic]") {
    const auto source_value = bg::i64{42};
    const auto source = bg::Scalar<bg::i64>::from(source_value);
    const auto initial = bg::ScalarValue{std::string_view{"first"}};
    auto value = bg::ScalarValue<>{initial};

    value = source;

    const auto view = value.view();
    CHECK(*bg::scalar_cast<bg::i64>(view) == source_value);
}

TEST_CASE("scalar handles compare by type and value", "[value][scalar][erased][feature:scalar-api]") {
    const auto left_value = bg::i64{42};
    const auto equal_value = bg::i64{42};
    const auto unequal_value = bg::i64{43};
    const auto double_value = 42.0;
    const auto left = bg::Scalar<bg::i64>::from(left_value);
    const auto equal = bg::Scalar<bg::i64>::from(equal_value);
    const auto different_value = bg::Scalar<bg::i64>::from(unequal_value);
    const auto different_type = bg::Scalar<double>::from(double_value);

    CHECK(left == equal);
    CHECK_FALSE(left == different_value);
    CHECK_FALSE(left == different_type);
    CHECK(left.hash() == equal.hash());
}

TEST_CASE("a constrained scalar variable exposes its required operations",
          "[value][scalar][erased][feature:scalar-api][scalar:generic]") {
    using KEY = bg::ScalarVariable<"KEY", bg::EqualityComparable, bg::Hashable>;

    const auto left_value = bg::i64{42};
    const auto equal_value = bg::i64{42};
    const auto different_value = bg::i64{43};
    const auto left = bg::Scalar<KEY>::from<bg::i64>(left_value);
    const auto equal = bg::Scalar<KEY>::from<bg::i64>(equal_value);
    const auto different = bg::Scalar<KEY>::from<bg::i64>(different_value);

    CHECK(left == equal);
    CHECK_FALSE(left == different);
    CHECK(left.hash() == equal.hash());
    CHECK(&left.type() == &bg::int64_type());
}

TEST_CASE("scalar variable equality rejects a different semantic type",
          "[value][scalar][erased][feature:scalar-api][scalar:generic]") {
    using VALUE = bg::ScalarVariable<"VALUE", bg::EqualityComparable>;

    const auto integer_value = bg::i64{42};
    const auto string_value = bg::ScalarValue<bg::string>{"42"};
    const auto integer = bg::Scalar<VALUE>::from<bg::i64>(integer_value);
    const auto string_view = string_value.view();
    const auto string = bg::Scalar<VALUE>::from(string_view);

    CHECK_FALSE(integer == string);
    CHECK_FALSE(string == integer);
}

TEST_CASE("checked scalar casts reject an incompatible value", "[value][scalar][erased][feature:scalar-api]") {
    const auto value = bg::i64{42};
    const auto erased = bg::Scalar<>::from<bg::i64>(value);

    CHECK(*bg::scalar_cast<bg::i64>(erased) == value);
    CHECK(*bg::unchecked_scalar_cast<bg::i64>(erased) == value);
    CHECK_THROWS_AS(bg::scalar_cast<double>(erased), std::bad_cast);
}
