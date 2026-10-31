#include <bg/scalar/cpp_runtime_type.hpp>
#include <bg/scalar/value.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <memory_resource>
#include <string_view>
#include <typeinfo>

using namespace bg;

TEST_CASE("an erased scalar value copies a borrowed value",
          "[value][scalar][erased][feature:scalar-api][scalar:generic]") {
    auto source = std::string{"value"};
    auto source_owner = ScalarValue<string>{source};
    const auto source_view = source_owner.view();
    const auto view = Scalar<SCALAR>::from(source_view);
    auto value = ScalarValue<>{view};

    source.assign("changed");

    CHECK(&value.type() == &string_type());
    CHECK(*scalar_cast<string>(value) == "value");

    const auto same_source = ScalarValue{std::string_view{"value"}};
    const auto same = ScalarValue<>{same_source};
    CHECK(value.equals(same));
    CHECK(value.hash() == same.hash());
}

TEST_CASE("an erased scalar value stores a pointer-sized borrowed value inline",
          "[value][scalar][erased][feature:scalar-api][scalar:generic][scalar:integer][storage:inline]") {
    const auto source_value = i64{42};
    const auto source = Scalar<i64>::from(source_value);
    auto allocator = bg::allocator{*std::pmr::null_memory_resource()};
    const auto value = ScalarValue<>{source, allocator};

    CHECK(*scalar_cast<i64>(value) == source_value);
}

TEST_CASE("an erased scalar value can replace its value",
          "[value][scalar][erased][feature:scalar-api][scalar:generic]") {
    const auto first = ScalarValue{std::string_view{"first"}};
    const auto second = ScalarValue{42};
    auto value = ScalarValue<>{first};

    value = second;

    const auto view = value.view();
    CHECK(*scalar_cast<i64>(view) == 42);
}

TEST_CASE("an erased scalar value can replace its value from a borrowed view",
          "[value][scalar][erased][feature:scalar-api][scalar:generic]") {
    const auto source_value = i64{42};
    const auto source = Scalar<i64>::from(source_value);
    const auto initial = ScalarValue{std::string_view{"first"}};
    auto value = ScalarValue<>{initial};

    value = source;

    const auto view = value.view();
    CHECK(*scalar_cast<i64>(view) == source_value);
}

TEST_CASE("scalar handles compare by type and value", "[value][scalar][erased][feature:scalar-api]") {
    const auto left_value = i64{42};
    const auto equal_value = i64{42};
    const auto unequal_value = i64{43};
    const auto double_value = 42.0;
    const auto left = Scalar<i64>::from(left_value);
    const auto equal = Scalar<i64>::from(equal_value);
    const auto different_value = Scalar<i64>::from(unequal_value);
    const auto different_type = Scalar<double>::from(double_value);

    CHECK(left == equal);
    CHECK_FALSE(left == different_value);
    CHECK_FALSE(left == different_type);
    CHECK(left.hash() == equal.hash());
}

TEST_CASE("a constrained scalar variable exposes its required operations",
          "[value][scalar][erased][feature:scalar-api][scalar:generic]") {
    using KEY = ScalarVariable<"KEY", EqualityComparable, Hashable>;

    const auto left_value = i64{42};
    const auto equal_value = i64{42};
    const auto different_value = i64{43};
    const auto left = Scalar<KEY>::from<i64>(left_value);
    const auto equal = Scalar<KEY>::from<i64>(equal_value);
    const auto different = Scalar<KEY>::from<i64>(different_value);

    CHECK(left == equal);
    CHECK_FALSE(left == different);
    CHECK(left.hash() == equal.hash());
    CHECK(&left.type() == &int64_type());
}

TEST_CASE("scalar variable equality rejects a different semantic type",
          "[value][scalar][erased][feature:scalar-api][scalar:generic]") {
    using VALUE = ScalarVariable<"VALUE", EqualityComparable>;

    const auto integer_value = i64{42};
    const auto string_value = ScalarValue<string>{"42"};
    const auto integer = Scalar<VALUE>::from<i64>(integer_value);
    const auto string_view = string_value.view();
    const auto string = Scalar<VALUE>::from(string_view);

    CHECK_FALSE(integer == string);
    CHECK_FALSE(string == integer);
}

TEST_CASE("checked scalar casts reject an incompatible value", "[value][scalar][erased][feature:scalar-api]") {
    const auto value = i64{42};
    const auto erased = Scalar<>::from<i64>(value);

    CHECK(*scalar_cast<i64>(erased) == value);
    CHECK(*unchecked_scalar_cast<i64>(erased) == value);
    CHECK_THROWS_AS(scalar_cast<double>(erased), std::bad_cast);
}
